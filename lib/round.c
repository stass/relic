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
 * view.  The pool's workers claim the tasks (lib/pool.c), each worker its
 * own in increasing order.  A task derives exactly what the serial
 * evaluator derives while its first cursor is inside its piece, in the
 * same order, and screens what it derives (lib/task.c): a worker's
 * candidates are the tuples it derived that neither the frozen store nor
 * its own earlier candidates hold.
 *
 * The merge then inserts the candidates task by task, in task order, with
 * the insertion the serial evaluator uses.  The candidates in that order are
 * the serial derivation order with some duplicates removed -- tuples the
 * store held, and repeats of the same worker's earlier candidates -- and a
 * tuple several workers gathered is found present by the merge after its
 * first, as serially it would have been.  So every tuple takes the
 * identifier it takes serially, every later derivation of it is the
 * duplicate it is serially, and the relations and every counter come out as
 * the serial evaluator leaves them, whatever the number of workers.
 *
 * Nothing is modified until the merge begins.  A round that cannot go on
 * before then -- a candidate buffer that could not grow, for memory or for
 * the memory limit, more candidates than the tuple limit could still admit,
 * or storage the pool itself could not get -- is abandoned: its candidates
 * are released, its counts forgotten, and the caller runs the round
 * serially, where whatever refuses it refuses it as it always would.  The
 * merge is asked first whether either limit could refuse any of it, and a
 * round that might be refused is abandoned too.  So a limit refuses only in
 * a round run serially, on the tuple the serial evaluator is refused, and a
 * merge fails only when the allocator refuses it.
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

/*
 * The work a round must be expected to do, in tuples read and derived, to
 * be shared among the pool's threads, and the work each piece of a cut
 * plan is given.  A round below the first runs serially, since publishing
 * it and waiting for every thread costs more than it would gain; a round
 * of the fixed point's last few, which shrink toward nothing, is such a
 * round.  Constants of performance alone, measured on tc-cyclic and cspa.
 */
#define	RELIC_PARALLEL_MIN_WORK	65536u
#define	RELIC_CHUNK_WORK	4096u

static void	 abandon_round(relic_t *ctx, relic_pool_t *pool);
static void	 count_target_candidates(relic_pool_t *pool,
		    const relic_round_t *round);
static int	 cut_tasks(relic_pool_t *pool, const relic_round_t *round,
		    bool finest);
static void	 forget_worker_counters(relic_pool_t *pool);
static uint64_t	 item_estimate(const relic_round_t *round,
		    const relic_round_item_t *item);
static bool	 item_is_cut(const relic_round_item_t *item);
static size_t	 item_pieces(const relic_pool_t *pool,
		    const relic_round_t *round, const relic_round_item_t *item,
		    bool finest);
static uint64_t	 item_range(const relic_round_t *round,
		    const relic_round_item_t *item);
static relic_tuple_range_t item_view(const relic_round_t *round,
		    const relic_round_item_t *item);
static void	 job_begin(relic_round_job_t *job, relic_t *ctx,
		    const relic_pool_t *pool, const relic_round_t *round);
static bool	 merge_fits(const relic_t *ctx, const relic_pool_t *pool,
		    const relic_round_t *round);
static int	 merge_round(relic_t *ctx, relic_pool_t *pool,
		    relic_round_t *round);
static int	 merge_task(relic_t *ctx, const relic_candidates_t *candidates,
		    const relic_round_task_t *task, relic_relation_t *target,
		    bool *gainedp);
static bool	 op_reads_as_it_runs(const relic_op_t *op);
static int	 pool_prepare(relic_pool_t *pool, const relic_round_t *round);
static void	 record_task_work(const relic_pool_t *pool,
		    relic_round_t *round);
static void	 release_candidates(relic_t *ctx, relic_pool_t *pool);
static bool	 round_is_worth_sharing(const relic_t *ctx,
		    const relic_round_t *round);
static void	 round_needs(const relic_round_t *round, size_t *op_roomp,
		    uint16_t *arity_roomp);
static bool	 round_reads_no_target(const relic_round_t *round);
static uint64_t	 scaled_work(uint64_t work, uint64_t range_now,
		    uint64_t range_then);
static void	 take_worker_counters(relic_pool_t *pool,
		    relic_counters_t *counters);
static uint64_t	 tasks_run_by_threads(const relic_pool_t *pool);
static size_t	 tuples_admissible(const relic_t *ctx);
static size_t	 view_pieces(const relic_pool_t *pool,
		    relic_tuple_range_t view);
static relic_tuple_range_t view_piece(relic_tuple_range_t view, size_t piece,
		    size_t pieces);
static int	 worker_fit(relic_worker_t *worker, size_t op_room,
		    uint16_t arity_room);

/*
 * Under RELIC_F_PARALLEL_ALWAYS every round that may wait is run on the
 * pool, however little it holds and however few workers the pool has, and
 * cut as finely as the pool allows, so that small programs exercise what
 * large ones do.  Otherwise a round is shared only when the context has
 * threads to share it with and it is expected to repay them.
 */
enum relic_round_outcome
relic_round_run(relic_t *ctx, relic_round_t *round)
{
	relic_round_job_t job;
	relic_pool_t *pool;
	bool abandoned;
	bool always;
	int status;

	always = (ctx->config.flags & RELIC_F_PARALLEL_ALWAYS) != 0;
	if (!always && !round_is_worth_sharing(ctx, round))
		return (RELIC_ROUND_DECLINED);
	if (!round_reads_no_target(round))
		return (RELIC_ROUND_DECLINED);

	if (ctx->pool == NULL) {
		if (relic_pool_create(&ctx->pool, ctx->config.threads) != 0)
			return (RELIC_ROUND_DECLINED);
	}
	pool = ctx->pool;
	relic_pool_adopt(pool);
	if (!always && pool->worker_count < 2)
		return (RELIC_ROUND_DECLINED);
	if (pool_prepare(pool, round) != 0 ||
	    cut_tasks(pool, round, always) != 0)
		return (RELIC_ROUND_DECLINED);

	job_begin(&job, ctx, pool, round);
	relic_pool_run(pool, &job);

	abandoned = relic_atomic_bool_load_relaxed(&job.abandoned);
	if (!abandoned) {
		count_target_candidates(pool, round);
		abandoned = !merge_fits(ctx, pool, round);
	}
	if (abandoned) {
		abandon_round(ctx, pool);
		pool->abandoned_rounds++;
		return (RELIC_ROUND_DECLINED);
	}

	take_worker_counters(pool, &ctx->counters);
	status = merge_round(ctx, pool, round);
	release_candidates(ctx, pool);
	if (status != 0)
		return (RELIC_ROUND_FAILED);

	record_task_work(pool, round);
	pool->merged_rounds++;
	pool->thread_tasks += tasks_run_by_threads(pool);
	if (pool->worker_count > 1)
		ctx->counters.parallel_rounds++;

	return (RELIC_ROUND_MERGED);
}

void
relic_round_record(const relic_round_t *round, relic_round_item_t *item,
    uint64_t work)
{
	if (item->history == NULL)
		return;

	item->history->work = work;
	item->history->range = item_range(round, item);
	item->history->ran = true;
}

/*
 * Whether a round is worth sharing: the context has threads to share it
 * with, and the round is expected to do enough work to repay them.  The
 * work summed so far stays below the threshold, so comparing each estimate
 * with what remains of it cannot overflow.
 */
static bool
round_is_worth_sharing(const relic_t *ctx, const relic_round_t *round)
{
	uint64_t estimate;
	uint64_t work;
	size_t i;

	if (ctx->config.threads < 2)
		return (false);

	work = 0;
	for (i = 0; i < round->item_count; i++) {
		estimate = item_estimate(round, &round->items[i]);
		if (estimate >= RELIC_PARALLEL_MIN_WORK - work)
			return (true);
		work += estimate;
	}

	return (false);
}

/*
 * The work an item is expected to do: what it did the last time it ran,
 * scaled by how its first scan's view has grown or shrunk since, or the
 * view itself for an item that has not run or keeps no history.  A
 * recursive component's rounds change size gradually, so the round before
 * predicts the next well enough for a threshold whose only cost is a
 * missed chance or a barrier paid for nothing.  An item whose view was
 * empty last time did work that no tuple of it accounts for -- the rest of
 * its plan found nothing to read -- so there is nothing to scale, and it
 * is expected to do that much again and to read its view now.
 */
static uint64_t
item_estimate(const relic_round_t *round, const relic_round_item_t *item)
{
	const relic_plan_history_t *history;
	uint64_t range;

	range = item_range(round, item);
	history = item->history;
	if (history == NULL || !history->ran)
		return (range);
	if (history->range == 0)
		return (history->work + range);

	return (scaled_work(history->work, range, history->range));
}

/*
 * `work` times `range_now` over `range_then`, which is not zero, or
 * UINT64_MAX when that is more than a uint64_t holds.  A range is at most
 * a relation's tuples, below 2^32, so the remainder's product fits.
 */
static uint64_t
scaled_work(uint64_t work, uint64_t range_now, uint64_t range_then)
{
	uint64_t whole;
	uint64_t part;

	whole = work / range_then;
	part = work % range_then * range_now / range_then;
	if (whole != 0 && range_now > (UINT64_MAX - part) / whole)
		return (UINT64_MAX);

	return (whole * range_now + part);
}

/*
 * The tuples an item's first scan reads, as the round resolves its view,
 * or one for an item that is not cut.
 */
static uint64_t
item_range(const relic_round_t *round, const relic_round_item_t *item)
{
	relic_tuple_range_t view;

	if (!item_is_cut(item))
		return (1);

	view = item_view(round, item);

	return (view.end - view.first);
}

/*
 * Ready a job for the round's tasks: none claimed, none finished, none
 * abandoned.
 */
static void
job_begin(relic_round_job_t *job, relic_t *ctx, const relic_pool_t *pool,
    const relic_round_t *round)
{
	bzero(job, sizeof(*job));
	job->round = round;
	job->tasks = pool->tasks;
	job->account = &ctx->account;
	job->task_count = pool->task_count;
	job->admissible = tuples_admissible(ctx);
	relic_atomic_size_init(&job->next_task, 0);
	relic_atomic_size_init(&job->finished, 0);
	relic_atomic_bool_init(&job->abandoned, false);
}

/*
 * Record in each item's history the work its tasks did, which the merge
 * has made the item's turn.  An item's tasks are consecutive.
 */
static void
record_task_work(const relic_pool_t *pool, relic_round_t *round)
{
	const relic_round_task_t *task;
	uint64_t work;
	size_t i;

	work = 0;
	for (i = 0; i < pool->task_count; i++) {
		task = &pool->tasks[i];
		work += task->work;

		if (i + 1 == pool->task_count ||
		    pool->tasks[i + 1].item != task->item) {
			relic_round_record(round, &round->items[task->item],
			    work);
			work = 0;
		}
	}
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
 * Ready the pool for a round: each worker's scratch holding the round's
 * longest plan and widest target, and room for a count per target.
 * Returns 0, or -1, unreported, when something could not be allocated.
 */
static int
pool_prepare(relic_pool_t *pool, const relic_round_t *round)
{
	size_t *target_candidates;
	size_t op_room;
	size_t i;
	uint16_t arity_room;

	round_needs(round, &op_room, &arity_room);
	for (i = 0; i < pool->worker_count; i++) {
		if (worker_fit(&pool->workers[i], op_room, arity_room) != 0)
			return (-1);
	}

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
 * How many pieces an item is cut into: one for an item that is not cut,
 * and otherwise its expected work in pieces of RELIC_CHUNK_WORK, at least
 * one and at most what view_pieces() allows, or `finest`, that most.  A
 * plan over a small delta with a large fan-out below it is then cut
 * finely, where cutting by its range alone would give it one piece.
 */
static size_t
item_pieces(const relic_pool_t *pool, const relic_round_t *round,
    const relic_round_item_t *item, bool finest)
{
	uint64_t wanted;
	size_t most;

	if (!item_is_cut(item))
		return (1);

	most = view_pieces(pool, item_view(round, item));
	if (finest)
		return (most);

	wanted = item_estimate(round, item) / RELIC_CHUNK_WORK;
	if (wanted < 1)
		return (1);
	if (wanted > most)
		return (most);

	return ((size_t)wanted);
}

/*
 * The most pieces a view is cut into: as many as the pool's workers use to
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
 * of an item that is cut in the order of its view, growing the pool's task
 * array as each item needs.  Returns 0, or -1, unreported, when the array
 * could not grow.
 */
static int
cut_tasks(relic_pool_t *pool, const relic_round_t *round, bool finest)
{
	const relic_round_item_t *item;
	relic_round_task_t *tasks;
	relic_round_task_t *task;
	relic_tuple_range_t view;
	size_t pieces;
	size_t piece;
	size_t i;

	pool->task_count = 0;

	for (i = 0; i < round->item_count; i++) {
		item = &round->items[i];
		bzero(&view, sizeof(view));
		if (item_is_cut(item))
			view = item_view(round, item);
		pieces = item_pieces(pool, round, item, finest);

		tasks = relic_grow_array(pool->tasks, &pool->task_capacity,
		    pool->task_count + pieces, sizeof(*pool->tasks));
		if (tasks == NULL)
			return (-1);
		pool->tasks = tasks;

		for (piece = 0; piece < pieces; piece++) {
			task = &pool->tasks[pool->task_count];
			pool->task_count++;

			bzero(task, sizeof(*task));
			task->item = i;
			task->cut = item_is_cut(item);
			if (task->cut)
				task->piece = view_piece(view, piece, pieces);
		}
	}

	return (0);
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
 * Whether the merge can be made without either limit refusing it, given
 * the candidates each target was offered, as count_target_candidates()
 * counts them.
 *
 * The tuple limit is asked of all the candidates: each worker checked its
 * own against what the limit admits, and several can together hold more.
 * The candidates of different workers may repeat one another, so the merge
 * may insert fewer than their count, and never more; a round whose count
 * does not fit is run serially, which inserts each new tuple once and is
 * refused where the serial evaluator is.
 *
 * The memory limit is asked with the candidates still held, since they
 * count against it for as long as they exist, and the merge's insertions
 * ask it for room as the serial evaluator's do.  An insertion the limit
 * refused only because the candidates held the room would be a refusal the
 * serial evaluator never meets.  So before anything is modified, the merge
 * is asked for its worst case: every relation the round writes grown by
 * every candidate offered it, one insertion at a time, as though each were
 * new.  No insertion of the merge asks for more; if that fits beside the
 * candidates, the limit refuses nothing in the merge, and if it does not,
 * the round is abandoned and run serially, which admits or refuses each
 * tuple as it always does.
 */
static bool
merge_fits(const relic_t *ctx, const relic_pool_t *pool,
    const relic_round_t *round)
{
	const relic_relation_t *target;
	size_t offered;
	size_t growth;
	size_t total;
	size_t i;

	total = 0;
	for (i = 0; i < round->target_count; i++)
		total = relic_saturating_sum(total,
		    pool->target_candidates[i]);
	if (total > tuples_admissible(ctx))
		return (false);

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
	uint64_t hashes[RELIC_INSERT_BATCH];
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
		relic_exec_counters_add(&counters->read,
		    &worker->scratch.counters);
	}

	forget_worker_counters(pool);
}

/*
 * How many of the round's tasks a thread of the pool ran rather than the
 * calling thread, worker 0.
 */
static uint64_t
tasks_run_by_threads(const relic_pool_t *pool)
{
	uint64_t count;
	size_t i;

	count = 0;
	for (i = 0; i < pool->task_count; i++) {
		if (pool->tasks[i].worker != 0)
			count++;
	}

	return (count);
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
