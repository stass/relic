/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * Deferred rounds, as worker 0 runs them.
 *
 * A round of the evaluator -- a semi-naive round, the base pass of a
 * component, the single pass of one that is not recursive, a query that
 * gathers its answer -- runs items none of which reads a relation another
 * writes except through a view fixed before the round began.  So nothing a
 * round inserts is read by the round, every insertion can wait for its end,
 * and while its plans run the store is frozen.  A round that reads what it
 * writes -- a pass of the naive fixed point, whose rules read the members as
 * the rules before them left them -- is declined and runs serially; that is
 * decided here, from the round's plans, rather than trusted to the caller.
 *
 * The round is cut into tasks, enumerated in serial order: item by item,
 * and a plan whose first operation is a SCAN piece by piece of that scan's
 * view.  A task derives exactly what the serial evaluator derives while its
 * first cursor is inside its piece, in the same order, and screens what it
 * derives (lib/task.c): a worker's candidates are the tuples it derived
 * that neither the frozen store nor its own earlier candidates hold.
 *
 * The merge then inserts the candidates task by task, in task order, with
 * the insertion the serial evaluator uses.  The candidates in that order are
 * the serial derivation order with some duplicates removed -- tuples the
 * store held, and repeats of earlier candidates -- so every tuple takes the
 * identifier it takes serially, every later derivation of it is the
 * duplicate it is serially, and the relations and every counter come out as
 * the serial evaluator leaves them.
 *
 * Nothing is modified until the merge begins.  A round that cannot go on
 * before then -- a candidate buffer that could not grow, for memory or for
 * the memory limit, more candidates than the tuple limit could still admit,
 * or storage the pool itself could not get -- is abandoned: its candidates
 * are released, its counts forgotten, and the caller runs the round
 * serially, where whatever refuses it refuses it as it always would.  The
 * merge is asked first whether it can fit beside the candidates, so the
 * candidates' charge never makes the memory limit refuse what the serial
 * evaluator would have admitted; a deferred round therefore fails only as
 * the serial evaluator fails, in its merge, on the tuple the serial
 * evaluator is refused.
 */

/*
 * How many pieces a plan's first scan is cut into for each worker.  Enough
 * that a worker which draws a heavy piece is balanced by others drawing
 * several light ones, and few enough that claiming and bookkeeping stay a
 * small part of a piece's work.  A constant of performance alone: what a
 * round derives does not depend on how its scans are cut, only on the
 * pieces being contiguous and in order.
 */
#define	RELIC_CHUNKS_PER_WORKER	8u

static void	 abandon_round(relic_t *ctx, relic_pool_t *pool);
static void	 count_target_candidates(relic_pool_t *pool,
		    const relic_round_t *round);
static void	 cut_tasks(relic_pool_t *pool, const relic_round_t *round);
static void	 forget_worker_counters(relic_pool_t *pool);
static bool	 item_is_cut(const relic_round_item_t *item);
static relic_tuple_range_t item_view(const relic_round_t *round,
		    const relic_round_item_t *item);
static bool	 merge_fits(const relic_t *ctx, const relic_pool_t *pool,
		    const relic_round_t *round);
static int	 merge_round(relic_t *ctx, relic_pool_t *pool,
		    relic_round_t *round);
static int	 merge_task(relic_t *ctx, const relic_candidates_t *candidates,
		    const relic_round_task_t *task, relic_relation_t *target,
		    bool *gainedp);
static bool	 op_reads_as_it_runs(const relic_op_t *op);
static int	 pool_create(relic_t *ctx);
static int	 pool_prepare(relic_pool_t *pool, const relic_round_t *round);
static void	 release_candidates(relic_t *ctx, relic_pool_t *pool);
static void	 round_needs(const relic_round_t *round, size_t *op_roomp,
		    uint16_t *arity_roomp);
static bool	 round_reads_no_target(const relic_round_t *round);
static void	 take_worker_counters(relic_pool_t *pool,
		    relic_counters_t *counters);
static size_t	 tuples_admissible(const relic_t *ctx);
static size_t	 view_pieces(const relic_pool_t *pool,
		    relic_tuple_range_t view);
static relic_tuple_range_t view_piece(relic_tuple_range_t view, size_t piece,
		    size_t pieces);
static int	 worker_fit(relic_worker_t *worker, size_t op_room,
		    uint16_t arity_room);

enum relic_round_outcome
relic_round_run(relic_t *ctx, relic_round_t *round)
{
	relic_round_job_t job;
	relic_pool_t *pool;
	int status;

	if (!round_reads_no_target(round))
		return (RELIC_ROUND_DECLINED);

	if (ctx->pool == NULL) {
		if (pool_create(ctx) != 0)
			return (RELIC_ROUND_DECLINED);
	}
	pool = ctx->pool;
	if (pool_prepare(pool, round) != 0)
		return (RELIC_ROUND_DECLINED);
	cut_tasks(pool, round);

	bzero(&job, sizeof(job));
	job.round = round;
	job.tasks = pool->tasks;
	job.account = &ctx->account;
	job.task_count = pool->task_count;
	job.admissible = tuples_admissible(ctx);
	relic_round_work(&job, &pool->workers[0], 0);

	if (!job.abandoned) {
		count_target_candidates(pool, round);
		if (!merge_fits(ctx, pool, round))
			job.abandoned = true;
	}
	if (job.abandoned) {
		abandon_round(ctx, pool);
		pool->abandoned_rounds++;
		return (RELIC_ROUND_DECLINED);
	}

	take_worker_counters(pool, &ctx->counters);
	status = merge_round(ctx, pool, round);
	release_candidates(ctx, pool);
	if (status != 0)
		return (RELIC_ROUND_FAILED);

	pool->merged_rounds++;

	return (RELIC_ROUND_MERGED);
}

/*
 * A round releases its candidates before it returns, so only the storage
 * kept from round to round remains.
 */
void
relic_pool_free(relic_pool_t *pool)
{
	relic_worker_t *worker;
	size_t i;

	if (pool == NULL)
		return;

	for (i = 0; i < pool->worker_count; i++) {
		worker = &pool->workers[i];
		assert(worker->candidates.count == 0);
		relic_free_array(worker->states);
		relic_free_array(worker->batch);
		relic_idtab_reset(&worker->candidates.screen);
	}

	free(pool->workers);
	relic_free_array(pool->tasks);
	relic_free_array(pool->target_candidates);
	free(pool);
}

/*
 * Whether no item of the round reads a relation the round writes, except
 * through a view fixed before the round began, which is what lets every
 * insertion wait for the round's end.  A round that broke it would not
 * fail but derive less, so every round is asked rather than trusted to be
 * one that may wait.  The question costs the round's operations times its
 * targets, which is nothing beside the round it decides.
 */
static bool
round_reads_no_target(const relic_round_t *round)
{
	const relic_plan_t *plan;
	const relic_op_t *op;
	size_t i;
	size_t k;
	size_t t;

	for (i = 0; i < round->item_count; i++) {
		plan = round->items[i].plan;
		if (plan == NULL)
			continue;

		for (k = 0; k < plan->op_count; k++) {
			op = &plan->ops[k];
			if (!op_reads_as_it_runs(op))
				continue;
			for (t = 0; t < round->target_count; t++) {
				if (op->u.atom.relation == round->targets[t])
					return (false);
			}
		}
	}

	return (true);
}

/*
 * Whether an operation reads its relation as it stands when the plan
 * starts, which is past whatever the items before it in a round inserted,
 * rather than through a member's view, which the deltas fixed before the
 * round began.  A filter and the EMIT read no relation.  Every kind is
 * named, so a new one does not compile until it is placed.
 */
static bool
op_reads_as_it_runs(const relic_op_t *op)
{
	switch (op->kind) {
	case RELIC_OP_SCAN:
	case RELIC_OP_LOOKUP:
	case RELIC_OP_MEMBER:
	case RELIC_OP_ANTI_MEMBER:
		return (op->u.atom.view == RELIC_VIEW_ALL);
	case RELIC_OP_FILTER:
	case RELIC_OP_EMIT:
		return (false);
	}

	return (false);
}

/*
 * Give the context its pool: one worker, the calling thread.  Returns 0, or
 * -1 when it could not be allocated, which is not reported: the pool is
 * sized by the program rather than the data, and a round that cannot have
 * one runs serially instead.
 */
static int
pool_create(relic_t *ctx)
{
	relic_pool_t *pool;

	pool = calloc(1, sizeof(*pool));
	if (pool == NULL)
		return (-1);

	pool->workers = calloc(1, sizeof(*pool->workers));
	if (pool->workers == NULL) {
		free(pool);
		return (-1);
	}
	pool->worker_count = 1;
	relic_candidates_init(&pool->workers[0].candidates);

	ctx->pool = pool;

	return (0);
}

/*
 * Ready the pool for a round: each worker's scratch holding the round's
 * longest plan and widest target, and room for the round's tasks and for a
 * count per target.  Returns 0, or -1, unreported, when something could not
 * be allocated.
 */
static int
pool_prepare(relic_pool_t *pool, const relic_round_t *round)
{
	relic_round_task_t *tasks;
	size_t *target_candidates;
	size_t task_count;
	size_t op_room;
	size_t i;
	uint16_t arity_room;

	round_needs(round, &op_room, &arity_room);
	for (i = 0; i < pool->worker_count; i++) {
		if (worker_fit(&pool->workers[i], op_room, arity_room) != 0)
			return (-1);
	}

	task_count = 0;
	for (i = 0; i < round->item_count; i++) {
		if (item_is_cut(&round->items[i]))
			task_count += view_pieces(pool,
			    item_view(round, &round->items[i]));
		else
			task_count++;
	}

	tasks = relic_grow_array(pool->tasks, &pool->task_capacity, task_count,
	    sizeof(*pool->tasks));
	if (tasks == NULL)
		return (-1);
	pool->tasks = tasks;

	target_candidates = relic_grow_array(pool->target_candidates,
	    &pool->target_capacity, round->target_count,
	    sizeof(*pool->target_candidates));
	if (target_candidates == NULL)
		return (-1);
	pool->target_candidates = target_candidates;

	return (0);
}

/*
 * The most operations of any plan of the round, and the most columns of any
 * relation it writes, each at least one, so that a scratch holding them
 * holds every item of the round.  A seed's head has its target's columns.
 */
static void
round_needs(const relic_round_t *round, size_t *op_roomp,
    uint16_t *arity_roomp)
{
	const relic_plan_t *plan;
	uint16_t arity;
	size_t i;

	*op_roomp = 1;
	for (i = 0; i < round->item_count; i++) {
		plan = round->items[i].plan;
		if (plan != NULL && plan->op_count > *op_roomp)
			*op_roomp = plan->op_count;
	}

	*arity_roomp = 1;
	for (i = 0; i < round->target_count; i++) {
		arity = relic_relation_arity(round->targets[i]);
		if (arity > *arity_roomp)
			*arity_roomp = arity;
	}
}

/*
 * Give a worker's scratch room for plans of `op_room` operations and a
 * batch of `arity_room` columns, growing its storage when a round needs more
 * than every round before it did.  The scratch starts afresh, which forgets
 * nothing: the counts and the note of the round before were taken or
 * forgotten when it ended.
 */
static int
worker_fit(relic_worker_t *worker, size_t op_room, uint16_t arity_room)
{
	relic_op_state_t *states;
	relic_dom_t *batch;

	states = relic_grow_array(worker->states, &worker->state_capacity,
	    op_room, sizeof(*worker->states));
	if (states == NULL)
		return (-1);
	worker->states = states;

	batch = relic_grow_array(worker->batch, &worker->batch_capacity,
	    (size_t)RELIC_INSERT_BATCH * arity_room, sizeof(*worker->batch));
	if (batch == NULL)
		return (-1);
	worker->batch = batch;

	relic_scratch_init(&worker->scratch, worker->states, op_room,
	    worker->batch, arity_room);

	return (0);
}

/*
 * Whether an item is cut into pieces of its first operation's view: a plan
 * whose first operation is a SCAN.  A seed derives one tuple, and a plan
 * that starts elsewhere -- a LOOKUP whose key is all constants, a test of
 * constants -- has no range to cut without walking it, and such a plan is
 * rare and short.
 */
static bool
item_is_cut(const relic_round_item_t *item)
{
	return (item->plan != NULL &&
	    item->plan->ops[0].kind == RELIC_OP_SCAN);
}

/*
 * The view an item that is cut reads with its first scan, as the round
 * resolves it.
 */
static relic_tuple_range_t
item_view(const relic_round_t *round, const relic_round_item_t *item)
{
	assert(item_is_cut(item));

	return (relic_exec_view(round->env, &item->plan->ops[0].u.atom));
}

/*
 * How many pieces a view is cut into: as many as the pool's workers use to
 * balance their shares, and never more than the view holds tuples, so that
 * each piece holds one.  An empty view is one empty piece.
 */
static size_t
view_pieces(const relic_pool_t *pool, relic_tuple_range_t view)
{
	size_t pieces;
	size_t tuples;

	tuples = view.end - view.first;

	pieces = RELIC_CHUNKS_PER_WORKER * pool->worker_count;
	if (pieces > tuples)
		pieces = tuples;

	return (pieces > 0 ? pieces : 1);
}

/*
 * Enumerate the round's tasks in serial order: item by item, and the pieces
 * of an item that is cut in the order of its view.  pool_prepare() made
 * room for exactly these, counting them the same way.
 */
static void
cut_tasks(relic_pool_t *pool, const relic_round_t *round)
{
	const relic_round_item_t *item;
	relic_round_task_t *task;
	relic_tuple_range_t view;
	size_t pieces;
	size_t piece;
	size_t i;

	pool->task_count = 0;

	for (i = 0; i < round->item_count; i++) {
		item = &round->items[i];
		bzero(&view, sizeof(view));
		pieces = 1;
		if (item_is_cut(item)) {
			view = item_view(round, item);
			pieces = view_pieces(pool, view);
		}

		for (piece = 0; piece < pieces; piece++) {
			assert(pool->task_count < pool->task_capacity);
			task = &pool->tasks[pool->task_count];
			pool->task_count++;

			bzero(task, sizeof(*task));
			task->item = i;
			task->cut = item_is_cut(item);
			if (task->cut)
				task->piece = view_piece(view, piece, pieces);
		}
	}
}

/*
 * Piece `piece` of a view cut into `pieces`: the pieces are contiguous and
 * in order, together cover the view exactly, and differ in size by at most
 * one tuple.  A view holds fewer than 2^32 tuples and a round far fewer than
 * 2^32 pieces, so the products fit in 64 bits.
 */
static relic_tuple_range_t
view_piece(relic_tuple_range_t view, size_t piece, size_t pieces)
{
	relic_tuple_range_t range;
	uint64_t tuples;

	tuples = view.end - view.first;
	range.first = view.first +
	    (relic_tuple_id_t)(tuples * piece / pieces);
	range.end = view.first +
	    (relic_tuple_id_t)(tuples * (piece + 1) / pieces);

	return (range);
}

/*
 * How many more tuples the tuple limit admits, or SIZE_MAX when none is
 * set.
 */
static size_t
tuples_admissible(const relic_t *ctx)
{
	size_t limit;

	limit = ctx->config.max_tuples;
	if (!relic_limit_set(limit))
		return (SIZE_MAX);
	if (ctx->store.tuple_count >= limit)
		return (0);

	return (limit - ctx->store.tuple_count);
}

/*
 * Whether the merge can be made with the candidates still held, given the
 * candidates each target was offered, as count_target_candidates() counts
 * them.
 *
 * The candidates count against the memory limit for as long as they exist,
 * and the merge's insertions ask the limit for room as the serial
 * evaluator's do.  An insertion the limit refused only because the
 * candidates held the room would be a refusal the serial evaluator never
 * meets.  So before anything is modified, the merge is asked for its worst
 * case: every relation the round writes grown by every candidate offered
 * it, one insertion at a time, as though each were new.  No insertion of
 * the merge asks for more; if that fits beside the candidates, the limit
 * refuses nothing in the merge, and if it does not, the round is abandoned
 * and run serially, which admits or refuses each tuple as it always does.
 */
static bool
merge_fits(const relic_t *ctx, const relic_pool_t *pool,
    const relic_round_t *round)
{
	const relic_relation_t *target;
	size_t offered;
	size_t growth;
	size_t i;

	if (!relic_limit_set(ctx->account.limit))
		return (true);

	growth = 0;
	for (i = 0; i < round->target_count; i++) {
		offered = pool->target_candidates[i];
		if (offered == 0)
			continue;

		target = round->targets[i];
		growth = relic_saturating_sum(growth,
		    relic_relation_bytes_after_inserts(target, offered) -
		    relic_relation_bytes(target));
	}

	return (!relic_account_exceeds(&ctx->account, 0, growth));
}

/*
 * Count, for each target of the round, the candidates its tasks gathered.
 */
static void
count_target_candidates(relic_pool_t *pool, const relic_round_t *round)
{
	const relic_round_task_t *task;
	size_t i;

	bzero(pool->target_candidates,
	    round->target_count * sizeof(*pool->target_candidates));

	for (i = 0; i < pool->task_count; i++) {
		task = &pool->tasks[i];
		pool->target_candidates[round->items[task->item].target] +=
		    task->candidate_count;
	}
}

/*
 * Insert the round's candidates in task order, which is serial order, each
 * task's into its item's target, reporting each task's note before its
 * tuples as the serial evaluator reports a batch's note before inserting
 * it.  Returns 0, or -1 at the first insertion refused, which the relation
 * store has reported; no later task's note is reported, as the serial
 * evaluator would never have reached it.
 */
static int
merge_round(relic_t *ctx, relic_pool_t *pool, relic_round_t *round)
{
	relic_round_task_t *task;
	relic_round_item_t *item;
	size_t i;

	for (i = 0; i < pool->task_count; i++) {
		task = &pool->tasks[i];
		item = &round->items[task->item];

		relic_report_mixed_note(ctx, item->rule, &task->note);

		if (merge_task(ctx, &pool->workers[task->worker].candidates,
		    task, round->targets[item->target], &item->gained) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Insert one task's candidates into its target, in the order it derived
 * them, a batch at a time as the serial evaluator inserts a plan's batches,
 * counting each that was new and marking its item as having gained.  The
 * hashes were computed when the candidates were screened.
 *
 * A task's candidates were appended by one worker while it ran the task
 * alone, all for one relation, so their columns lie end to end at the
 * relation's arity.
 */
static int
merge_task(relic_t *ctx, const relic_candidates_t *candidates,
    const relic_round_task_t *task, relic_relation_t *target,
    bool *gainedp)
{
	uint32_t hashes[RELIC_INSERT_BATCH];
	bool added[RELIC_INSERT_BATCH];
	const relic_candidate_t *first;
	size_t completed;
	size_t count;
	size_t done;
	size_t i;
	int status;

	for (done = 0; done < task->candidate_count; done += count) {
		count = task->candidate_count - done < RELIC_INSERT_BATCH ?
		    task->candidate_count - done : RELIC_INSERT_BATCH;
		first = &candidates->entries[task->first_candidate + done];

		for (i = 0; i < count; i++) {
			assert(first[i].offset ==
			    first->offset + i * target->arity);
			hashes[i] = first[i].hash;
		}

		status = relic_relation_insert_batch_hashed(ctx, target,
		    &candidates->words[first->offset], hashes, count, added,
		    &completed);
		for (i = 0; i < completed; i++) {
			if (added[i]) {
				ctx->counters.tuples_inserted++;
				*gainedp = true;
			}
		}
		if (status != 0)
			return (-1);
	}

	return (0);
}

/*
 * Add what every worker counted to the context's counters, and start
 * counting afresh.
 */
static void
take_worker_counters(relic_pool_t *pool, relic_counters_t *counters)
{
	relic_worker_t *worker;
	size_t i;

	for (i = 0; i < pool->worker_count; i++) {
		worker = &pool->workers[i];
		counters->derivations += worker->derivations;
		counters->tuples_scanned +=
		    worker->scratch.counters.tuples_scanned;
		counters->tuples_looked_up +=
		    worker->scratch.counters.tuples_looked_up;
	}

	forget_worker_counters(pool);
}

/*
 * Forget what every worker counted and noted.
 */
static void
forget_worker_counters(relic_pool_t *pool)
{
	relic_worker_t *worker;
	size_t i;

	for (i = 0; i < pool->worker_count; i++) {
		worker = &pool->workers[i];
		worker->derivations = 0;
		bzero(&worker->scratch.counters,
		    sizeof(worker->scratch.counters));
		worker->scratch.note.filter = NULL;
	}
}

/*
 * Release every worker's candidates, memory and charge alike.
 */
static void
release_candidates(relic_t *ctx, relic_pool_t *pool)
{
	size_t i;

	for (i = 0; i < pool->worker_count; i++)
		relic_candidates_release(&pool->workers[i].candidates,
		    &ctx->account);
}

/*
 * Leave a round as though it had never run: its candidates released and
 * its counts and notes forgotten.
 */
static void
abandon_round(relic_t *ctx, relic_pool_t *pool)
{
	release_candidates(ctx, pool);
	forget_worker_counters(pool);
}
