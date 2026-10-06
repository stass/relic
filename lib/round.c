/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
 * The merge then inserts the candidates as the serial evaluator would have
 * inserted them, on the pool's workers (lib/merge.c).  The candidates in
 * task order are the serial derivation order with some duplicates removed
 * -- tuples the store held, and repeats of the same worker's earlier
 * candidates -- and a tuple several workers gathered is new only at its
 * first, as serially it would have been.  So every tuple takes the
 * identifier it takes serially, every later derivation of it is the
 * duplicate it is serially, and the relations and every counter come out as
 * the serial evaluator leaves them, whatever the number of workers.
 *
 * Nothing is modified until the merge has made room for everything it
 * inserts, the integers the round computed among it.  A round that cannot
 * go on before then -- a candidate buffer or a scratch table that could not
 * grow, for memory or for the memory limit, more candidates than the tuple
 * limit could still admit, new tuples or integers a limit would refuse,
 * room the allocator would not give, storage the pool itself could not
 * get, an evaluation error a task met, or an interrupt a task found -- is
 * abandoned: its candidates
 * are released, its counts forgotten, and the caller runs the round
 * serially, where whatever refuses it refuses it as it always would.  So a
 * limit or an error is reported only in a round run serially, at the tuple
 * the serial evaluator meets it, and a merge never fails.
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
static int	 begin_worker_texts(relic_t *ctx, relic_pool_t *pool);
static size_t	 end_worker_texts(relic_pool_t *pool);
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
		    relic_pool_t *pool, const relic_round_t *round);
static const relic_relation_t *relation_read_as_it_runs(
		    const relic_op_t *op);
static int	 pool_prepare(relic_pool_t *pool, const relic_round_t *round);
static void	 record_task_work(const relic_pool_t *pool,
		    relic_round_t *round);
static void	 release_candidates(relic_t *ctx, relic_pool_t *pool);
static void	 report_task_notes(relic_t *ctx, relic_pool_t *pool,
		    const relic_round_t *round);
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
bool
relic_round_run(relic_t *ctx, relic_round_t *round)
{
	relic_round_job_t job;
	relic_pool_t *pool;
	size_t texts_bytes;
	bool always;

	always = (ctx->config.flags & RELIC_F_PARALLEL_ALWAYS) != 0;
	if (!always && !round_is_worth_sharing(ctx, round))
		return (false);
	if (!round_reads_no_target(round))
		return (false);

	if (ctx->pool == NULL) {
		if (relic_pool_create(&ctx->pool, ctx->config.threads,
		    ctx->hash_seed) != 0)
			return (false);
	}
	pool = ctx->pool;
	relic_pool_adopt(pool);
	if (!always && pool->worker_count < 2)
		return (false);
	if (pool_prepare(pool, round) != 0 ||
	    cut_tasks(pool, round, always) != 0)
		return (false);
	if (relic_round_first_call(round) != NULL &&
	    begin_worker_texts(ctx, pool) != 0)
		return (false);

	job_begin(&job, ctx, pool, round);
	relic_pool_for(pool, pool->task_count, relic_round_run_task, &job);
	texts_bytes = end_worker_texts(pool);

	if (relic_atomic_bool_load_relaxed(&job.abandoned) ||
	    !relic_round_merge(ctx, pool, round, texts_bytes)) {
		abandon_round(ctx, pool);
		pool->abandoned_rounds++;
		return (false);
	}

	report_task_notes(ctx, pool, round);
	take_worker_counters(pool, &ctx->counters);
	release_candidates(ctx, pool);

	record_task_work(pool, round);
	pool->merged_rounds++;
	pool->thread_tasks += tasks_run_by_threads(pool);
	if (pool->worker_count > 1)
		ctx->counters.parallel_rounds++;

	return (true);
}

/*
 * A round whose plans call a function needs the buffers its strings are
 * computed in, on the serial evaluator's scratch or on every worker's.
 */
const relic_round_item_t *
relic_round_first_call(const relic_round_t *round)
{
	const relic_round_item_t *item;
	size_t i;

	for (i = 0; i < round->item_count; i++) {
		item = &round->items[i];
		if (item->plan != NULL && relic_plan_calls(item->plan))
			return (item);
	}

	return (NULL);
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
 * Ready a job for the round's tasks, none abandoned.
 */
static void
job_begin(relic_round_job_t *job, relic_t *ctx, relic_pool_t *pool,
    const relic_round_t *round)
{
	bzero(job, sizeof(*job));
	job->round = round;
	job->tasks = pool->tasks;
	job->workers = pool->workers;
	job->account = &ctx->account;
	job->admissible = relic_tuples_admissible(ctx);
	job->interrupted = &ctx->interrupted;
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
	const relic_relation_t *relation;
	const relic_plan_t *plan;
	size_t i;
	size_t k;
	size_t t;

	for (i = 0; i < round->item_count; i++) {
		plan = round->items[i].plan;
		if (plan == NULL)
			continue;

		for (k = 0; k < plan->op_count; k++) {
			relation = relation_read_as_it_runs(&plan->ops[k]);
			if (relation == NULL)
				continue;
			for (t = 0; t < round->target_count; t++) {
				if (relation == round->targets[t])
					return (false);
			}
		}
	}

	return (true);
}

/*
 * The relation an operation reads as it stands when the plan starts, which
 * is past whatever the items before it in a round inserted, rather than
 * through a member's view, which the deltas fixed before the round began;
 * NULL for an operation that reads none that way.  A fold reads its group
 * relation whole.  A filter, an assignment and the EMIT read no relation.
 * Every kind is named, so a new one does not compile until it is placed.
 */
static const relic_relation_t *
relation_read_as_it_runs(const relic_op_t *op)
{
	switch (op->kind) {
	case RELIC_OP_SCAN:
	case RELIC_OP_LOOKUP:
	case RELIC_OP_MEMBER:
	case RELIC_OP_EXISTS:
	case RELIC_OP_ANTI_MEMBER:
		if (op->u.atom.view != RELIC_VIEW_ALL)
			return (NULL);
		return (op->u.atom.relation);
	case RELIC_OP_AGGREGATE:
		return (op->u.aggregate.relation);
	case RELIC_OP_FILTER:
	case RELIC_OP_ASSIGN:
	case RELIC_OP_EMIT:
		break;
	}

	return (NULL);
}

/*
 * Ready the pool for a round: each worker's scratch holding the round's
 * longest plan and widest target.  Returns 0, or -1, unreported, when
 * something could not be allocated.
 */
static int
pool_prepare(relic_pool_t *pool, const relic_round_t *round)
{
	size_t op_room;
	size_t i;
	uint16_t arity_room;

	round_needs(round, &op_room, &arity_room);
	for (i = 0; i < pool->worker_count; i++) {
		if (worker_fit(&pool->workers[i], op_room, arity_room) != 0)
			return (-1);
	}

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
 * batch of `arity_room` columns, and for the batch's pending integers,
 * growing its storage when a round needs more than every round before it
 * did.  The scratch starts afresh, which forgets nothing: the counts, the
 * note and the error of the round before were taken or forgotten when it
 * ended.
 */
static int
worker_fit(relic_worker_t *worker, size_t op_room, uint16_t arity_room)
{
	relic_op_state_t *states;
	relic_dom_t *batch;
	relic_pending_t *pending;

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

	pending = relic_grow_array(worker->pending, &worker->pending_capacity,
	    (size_t)RELIC_INSERT_BATCH * arity_room, sizeof(*worker->pending));
	if (pending == NULL)
		return (-1);
	worker->pending = pending;

	relic_scratch_init(&worker->scratch, worker->states, op_room,
	    worker->batch, worker->pending, arity_room);

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
 * Report each task's first mixed-type comparison in task order, which is
 * the order the serial evaluator meets them in, once the round has merged.
 */
static void
report_task_notes(relic_t *ctx, relic_pool_t *pool,
    const relic_round_t *round)
{
	relic_round_task_t *task;
	size_t i;

	for (i = 0; i < pool->task_count; i++) {
		task = &pool->tasks[i];
		relic_report_mixed_note(ctx, round->items[task->item].rule,
		    &task->note);
	}
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
 * Forget what every worker counted, noted and met.
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
		worker->scratch.note.where = NULL;
		bzero(&worker->scratch.error, sizeof(worker->scratch.error));
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
 * Give every worker's scratch a round's buffers for the strings its tasks
 * compute, charged as the serial scratch's are.  Returns 0, or -1 having
 * given none, when the limit or the allocator refused one: the round is
 * then declined, and its serial run meets whatever refused it.
 */
static int
begin_worker_texts(relic_t *ctx, relic_pool_t *pool)
{
	enum relic_exec_failure failure;
	size_t i;

	for (i = 0; i < pool->worker_count; i++) {
		if (relic_scratch_begin_texts(&pool->workers[i].scratch,
		    &ctx->account, &failure) != 0) {
			(void)end_worker_texts(pool);
			return (-1);
		}
	}

	return (0);
}

/*
 * Release every worker's buffers for strings, with their charge, and give
 * at least what the serial scratch's would hold by the end of the round:
 * for each buffer, the largest capacity any worker's reached.  A worker
 * holding none adds nothing, and a round that computes no string gives
 * zero.
 */
static size_t
end_worker_texts(relic_pool_t *pool)
{
	relic_text_capacity_t peaks;
	relic_scratch_t *scratch;
	size_t i;

	bzero(&peaks, sizeof(peaks));
	for (i = 0; i < pool->worker_count; i++) {
		scratch = &pool->workers[i].scratch;
		if (scratch->text_stack.bytes == NULL)
			continue;
		relic_text_capacity_raise(&peaks, scratch);
		relic_scratch_end_texts(scratch);
	}

	return (relic_text_capacity_bytes(&peaks));
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
