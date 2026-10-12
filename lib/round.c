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
 * and a plan with a driver (plan.h) slice by slice of the driver's visits,
 * in their order, its prefix having been run once when the item was
 * prepared (lib/slice.c).  The pool's workers claim the tasks
 * (lib/pool.c), each worker its own in increasing order.  A task derives
 * exactly what the serial evaluator derives while the driver's cursor is
 * inside its slice, in the same order, and screens what it derives
 * (lib/task.c): a worker's candidates are the tuples it derived that
 * neither the frozen store nor its own earlier candidates hold.
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
 * get, an evaluation error a prefix or a task met, or an interrupt
 * preparation or a task found -- is declined or abandoned: its candidates
 * are released, its counts and notes forgotten, and the caller runs the
 * round serially, where whatever refuses it refuses it as it always
 * would.  So a limit or an error is reported only in a round run serially,
 * at the tuple the serial evaluator meets it, and a merge never fails.
 *
 * Whether a round is shared is decided twice.  Before anything is
 * prepared, a round whose items could not do enough work even were each
 * driver to visit its whole view is declined at the cost of arithmetic.
 * Once prepared, with each driver's visits counted, the round is shared
 * only when its expected work repays what preparing and sharing it costs
 * (round_repays()).
 */

/*
 * How many pieces a plan's driver is cut into for each worker, at most.
 * Enough that a worker which draws a heavy piece is balanced by others
 * drawing several light ones, and few enough that claiming and bookkeeping
 * stay a small part of a piece's work.  A constant of performance alone:
 * what a round derives does not depend on how its drivers are cut, only on
 * the pieces being contiguous and in order.
 */
#define	RELIC_CHUNKS_PER_WORKER	8u

/*
 * The work a round must be expected to do, in tuples read and derived, to
 * be shared among the pool's threads, and the work each piece of a cut
 * plan is given.  A round below the first runs serially, since publishing
 * it and waiting for every thread costs more than it would gain; a round
 * of the fixed point's last few, which shrink toward nothing, is such a
 * round.  A piece is a quarter of the threshold, so that a round just past
 * it is cut into four pieces, and its tasks share out among several
 * workers rather than run on one; the two move together.  Constants of
 * performance alone, measured at four and sixteen threads on tc-cyclic,
 * cspa, dist/3 and chains of six thousand edges.
 */
#define	RELIC_PARALLEL_MIN_WORK	8000u
#define	RELIC_CHUNK_WORK	(RELIC_PARALLEL_MIN_WORK / 4)

/*
 * What sharing a round costs beside its work, in the same units: waking
 * the pool and waiting for its slowest worker, taken as one piece's work;
 * and how many times the links preparing its drivers followed the round's
 * expected work must exceed, since a link costs about what a visit does
 * and the round would not follow them serially.  Constants of performance
 * alone, set by that reasoning and not tuned.
 */
#define	RELIC_PARALLEL_STARTUP_WORK	RELIC_CHUNK_WORK
#define	RELIC_PREPARATION_SHARE		4u

typedef struct relic_round_expectation relic_round_expectation_t;

/*
 * What a round's workers are expected to gather in all: `candidates`, and
 * `words`, their columns.
 */
struct relic_round_expectation {
	uint64_t	candidates;
	uint64_t	words;
};

static void	 abandon_round(relic_t *ctx, relic_pool_t *pool);
static void	 decline_round(relic_t *ctx, relic_pool_t *pool);
static int	 begin_worker_texts(relic_t *ctx, relic_pool_t *pool);
static uint64_t	 ceiling_divide(uint64_t dividend, uint64_t divisor);
static size_t	 end_worker_texts(relic_pool_t *pool);
static int	 cut_tasks(relic_t *ctx, relic_pool_t *pool,
		    const relic_round_t *round);
static uint64_t	 driven_estimate(const relic_round_t *round,
		    const relic_round_item_t *item, size_t driver,
		    uint64_t visits);
static void	 forget_worker_counters(relic_pool_t *pool);
static bool	 is_lookup_driven(const relic_round_item_t *item);
static size_t	 item_pieces(const relic_pool_t *pool,
		    const relic_prepared_item_t *prepared, bool finest);
static uint64_t	 item_setup(const relic_round_item_t *item,
		    const relic_prepared_item_t *prepared);
static void	 job_begin(relic_round_job_t *job, relic_t *ctx,
		    relic_pool_t *pool, const relic_round_t *round);
static uint64_t	 optimistic_estimate(const relic_round_t *round,
		    const relic_round_item_t *item);
static int	 pool_prepare(relic_pool_t *pool, const relic_round_t *round);
static int	 prepare_items(relic_t *ctx, relic_pool_t *pool,
		    const relic_round_t *round, bool finest);
static void	 record_task_work(const relic_pool_t *pool,
		    relic_round_t *round);
static const relic_relation_t *relation_read_as_it_runs(
		    const relic_op_t *op);
static void	 release_candidates(relic_t *ctx, relic_pool_t *pool);
static int	 report_notes(relic_t *ctx, relic_pool_t *pool,
		    const relic_round_t *round);
static int	 reserve_candidates(relic_t *ctx, relic_pool_t *pool,
		    const relic_round_t *round);
static relic_round_expectation_t round_expectation(
		    const relic_round_t *round);
static bool	 round_may_repay(const relic_t *ctx,
		    const relic_round_t *round);
static void	 round_needs(const relic_round_t *round, size_t *op_roomp,
		    uint16_t *arity_roomp);
static bool	 round_reads_no_target(const relic_round_t *round);
static bool	 round_repays(const relic_pool_t *pool,
		    const relic_round_t *round);
static uint64_t	 saturating_add(uint64_t left, uint64_t right);
static uint64_t	 scaled_work(uint64_t work, uint64_t now, uint64_t then);
static void	 take_counters(relic_pool_t *pool, const relic_round_t *round,
		    relic_counters_t *counters);
static uint64_t	 tasks_run_by_threads(const relic_pool_t *pool);
static uint64_t	 whole_estimate(const relic_round_t *round,
		    const relic_round_item_t *item);
static int	 worker_fit(relic_worker_t *worker, size_t op_room,
		    uint16_t arity_room);
static size_t	 worker_share(uint64_t expected, size_t workers,
		    size_t admissible);

/*
 * Under RELIC_F_PARALLEL_ALWAYS every round that may wait is run on the
 * pool, however little it holds and however few workers the pool has, and
 * cut as finely as the pool allows, so that small programs exercise what
 * large ones do.  Otherwise a round is shared only when the context has
 * threads to share it with and it is expected to repay them.
 *
 * The workers' buffers for strings are begun before any item is prepared,
 * since a prefix can compute one, and worker 0's scratch runs the
 * prefixes, so what their strings grew it to counts toward what the
 * serial run would hold, as a task's would.
 *
 * A round the caller interrupted before its tasks all ended is abandoned,
 * for its serial run to report the interrupt, whatever its tasks saw: a
 * task that runs no plan, or one that ends between looks, never sees it.
 * An interrupt made while the merged round reports its notes fails it
 * instead, as it would fail the serial run reporting the same notes.
 */
enum relic_round_outcome
relic_round_run(relic_t *ctx, relic_round_t *round)
{
	relic_round_job_t job;
	relic_pool_t *pool;
	size_t texts_bytes;
	size_t workers;
	bool always;
	int status;

	always = (ctx->config.flags & RELIC_F_PARALLEL_ALWAYS) != 0;
	if (!always && !round_may_repay(ctx, round))
		return (RELIC_ROUND_DECLINED);
	if (!round_reads_no_target(round))
		return (RELIC_ROUND_DECLINED);

	if (ctx->pool == NULL) {
		if (relic_pool_create(&ctx->pool, ctx->config.threads,
		    ctx->hash_seed) != 0)
			return (RELIC_ROUND_DECLINED);
	}
	pool = ctx->pool;
	workers = relic_pool_workers(pool);
	if (!always && workers < 2)
		return (RELIC_ROUND_DECLINED);
	if (pool_prepare(pool, round) != 0)
		return (RELIC_ROUND_DECLINED);
	if (relic_round_first_call(round) != NULL &&
	    begin_worker_texts(ctx, pool) != 0)
		return (RELIC_ROUND_DECLINED);
	if (prepare_items(ctx, pool, round, always) != 0) {
		decline_round(ctx, pool);
		return (RELIC_ROUND_DECLINED);
	}
	if (!always && !round_repays(pool, round)) {
		pool->preparation_declines++;
		decline_round(ctx, pool);
		return (RELIC_ROUND_DECLINED);
	}
	if (cut_tasks(ctx, pool, round) != 0) {
		decline_round(ctx, pool);
		return (RELIC_ROUND_DECLINED);
	}
	if (reserve_candidates(ctx, pool, round) != 0) {
		decline_round(ctx, pool);
		return (RELIC_ROUND_DECLINED);
	}

	job_begin(&job, ctx, pool, round);
	relic_pool_for(pool, pool->task_count, relic_round_run_task, &job);
	texts_bytes = end_worker_texts(pool);

	if (relic_atomic_bool_load_relaxed(&job.abandoned) ||
	    relic_signal_flag_raised(&ctx->interrupted) ||
	    !relic_round_merge(ctx, pool, round, texts_bytes)) {
		abandon_round(ctx, pool);
		pool->abandoned_rounds++;
		return (RELIC_ROUND_DECLINED);
	}

	status = report_notes(ctx, pool, round);
	take_counters(pool, round, &ctx->counters);
	release_candidates(ctx, pool);

	record_task_work(pool, round);
	pool->merged_rounds++;
	pool->thread_tasks += tasks_run_by_threads(pool);
	if (pool->worker_count > 1)
		ctx->counters.parallel_rounds++;

	return (status == 0 ? RELIC_ROUND_MERGED : RELIC_ROUND_FAILED);
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
relic_round_record(relic_round_item_t *item, uint64_t work,
    const relic_driver_reads_t *reads, uint64_t inserted)
{
	if (item->history != NULL) {
		item->history->work = work;
		item->history->reads = *reads;
		item->history->ran = true;
	}
	if (item->insertions != NULL) {
		item->insertions->inserted = inserted;
		item->insertions->ran = true;
	}
}

/*
 * Whether a round could repay sharing it, asked before anything is
 * prepared: the context has threads to share it with, and the round could
 * do enough work were each driver to visit every tuple of its view.  It
 * declines only rounds too small to share however their drivers turn out,
 * and its view of a LOOKUP driver is that optimistic so that a selective
 * key does not decline a round its fan-out would repay.  The work summed
 * so far stays below the threshold, so comparing each estimate with what
 * remains of it cannot overflow.
 */
static bool
round_may_repay(const relic_t *ctx, const relic_round_t *round)
{
	uint64_t estimate;
	uint64_t work;
	size_t i;

	if (ctx->config.threads < 2)
		return (false);

	work = 0;
	for (i = 0; i < round->item_count; i++) {
		estimate = optimistic_estimate(round, &round->items[i]);
		if (estimate >= RELIC_PARALLEL_MIN_WORK - work)
			return (true);
		work += estimate;
	}

	return (false);
}

/*
 * The most work an item is expected to do before its driver is prepared:
 * as driven_estimate() expects of its driver visiting its whole view, or
 * the whole plan's for an item with no driver.
 */
static uint64_t
optimistic_estimate(const relic_round_t *round,
    const relic_round_item_t *item)
{
	relic_tuple_range_t view;
	size_t driver;

	if (item->plan == NULL)
		return (whole_estimate(round, item));
	driver = relic_plan_driver(item->plan);
	if (driver == RELIC_PLAN_NO_DRIVER)
		return (whole_estimate(round, item));

	view = relic_exec_view(round->env, &item->plan->ops[driver].u.atom);

	return (driven_estimate(round, item, driver,
	    view.end > view.first ? view.end - view.first : 0));
}

/*
 * The work an item whose driver is at `driver` is expected to do from its
 * driver on when the driver visits `visits` tuples: what the driver and
 * the operations after it did the last time the plan ran, scaled by how
 * many tuples the driver visited then and visits now, or, for a plan that
 * has not run, keeps no history or visited nothing, what the estimator
 * walks from the visits (relic_cost_driven()).  A recursive component's
 * rounds change size gradually, so the round before predicts the next well
 * enough for a decision whose only cost is a missed chance or a barrier
 * paid for nothing.  The prefix is left out: it runs once, and has run
 * when the estimate decides anything but the first check.
 */
static uint64_t
driven_estimate(const relic_round_t *round, const relic_round_item_t *item,
    size_t driver, uint64_t visits)
{
	const relic_plan_history_t *history;

	history = item->history;
	if (history == NULL || !history->ran || history->reads.visits == 0)
		return (relic_cost_driven(item->plan, driver, visits,
		    round->env));

	assert(history->work >= history->reads.prefix);

	return (scaled_work(history->work - history->reads.prefix, visits,
	    history->reads.visits));
}

/*
 * The work an item with no driver is expected to do, run whole: one
 * derivation for a seed, what the plan did the last time it ran, or what
 * the estimator walks for one that has not (relic_cost_plan()).  Such a
 * plan starts with an assignment or a fold, whose work does not follow a
 * view the round can measure.
 */
static uint64_t
whole_estimate(const relic_round_t *round, const relic_round_item_t *item)
{
	if (item->plan == NULL)
		return (1);
	if (item->history != NULL && item->history->ran)
		return (item->history->work);

	return (relic_cost_plan(item->plan, round->env));
}

/*
 * `work` times `now` over `then`, rounded up, or UINT64_MAX when that is
 * more than a uint64_t holds.  Both counts are a driver's visits of one
 * view, at most a relation's tuples, below 2^32, so the remainder's
 * product fits.
 */
static uint64_t
scaled_work(uint64_t work, uint64_t now, uint64_t then)
{
	uint64_t whole;
	uint64_t part;

	assert(now <= RELIC_TUPLE_MAX && then > 0 &&
	    then <= RELIC_TUPLE_MAX);

	whole = work / then;
	part = ceiling_divide(work % then * now, then);
	if (whole != 0 && now > (UINT64_MAX - part) / whole)
		return (UINT64_MAX);

	return (whole * now + part);
}

/*
 * `dividend` over `divisor`, which is not zero, rounded up.
 */
static uint64_t
ceiling_divide(uint64_t dividend, uint64_t divisor)
{
	return (dividend / divisor + (dividend % divisor != 0));
}

static uint64_t
saturating_add(uint64_t left, uint64_t right)
{
	return (left > UINT64_MAX - right ? UINT64_MAX : left + right);
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
 * Record in each item's history the work its prefix and its tasks did and
 * the tuples they inserted, which the merge has made the item's turn, as
 * the item's serial turn would have recorded them.
 */
static void
record_task_work(const relic_pool_t *pool, relic_round_t *round)
{
	const relic_prepared_item_t *prepared;
	const relic_round_task_t *task;
	relic_driver_reads_t reads;
	uint64_t inserted;
	uint64_t work;
	size_t piece;
	size_t i;

	for (i = 0; i < round->item_count; i++) {
		prepared = &pool->prepared[i];
		reads = prepared->reads;
		work = relic_exec_work(0, &prepared->prefix_reads);
		inserted = 0;
		for (piece = 0; piece < prepared->pieces; piece++) {
			task = &pool->tasks[prepared->first_task + piece];
			work += task->work;
			reads.prefix += task->reads.prefix;
			reads.visits += task->reads.visits;
			inserted += task->inserted;
		}
		relic_round_record(&round->items[i], work, &reads, inserted);
	}
}

/*
 * Give every worker's candidates, before the round's tasks run, room for
 * the candidates it is expected to gather, so that a round predicted well
 * grows none of its buffers, and one that exceeds its prediction grows
 * from the prediction rather than from nothing.  With one worker that is
 * every candidate the round is expected to gather; with several, twice an
 * even share of them, since a worker gathers what the tasks it happens to
 * claim derive.  Each worker's columns are reserved at the expected
 * candidates' width, rounded up.  The room is charged as the buffers'
 * growth is, and released with them at the round's end.  Returns 0, or -1
 * when the limit or the allocator refused it, which declines the round
 * before anything runs; what was obtained is the candidates' to release.
 */
static int
reserve_candidates(relic_t *ctx, relic_pool_t *pool,
    const relic_round_t *round)
{
	relic_round_expectation_t expected;
	uint64_t width;
	size_t share;
	size_t i;

	expected = round_expectation(round);
	share = worker_share(expected.candidates, pool->worker_count,
	    relic_tuples_admissible(ctx));
	if (share == 0)
		return (0);

	width = (expected.words + expected.candidates - 1) /
	    expected.candidates;
	for (i = 0; i < pool->worker_count; i++) {
		if (relic_candidates_reserve(&pool->workers[i].candidates,
		    &ctx->account, share, share * (size_t)width) != 0) {
			pool->reservation_declines++;
			return (-1);
		}
	}
	pool->expected_candidates += expected.candidates;

	return (0);
}

/*
 * What a round's items are expected to gather: the tuples each inserted
 * the last time it ran, and their columns, each at its target's arity.  The
 * new tuples of a recursive component's rounds do not follow the size of
 * its delta -- a round may read many times its predecessor's delta and
 * insert a tenth as much -- but a round's insertions predict the next
 * round's within a small factor, so they are taken unscaled.  An item that
 * has not run, or keeps no record, is expected to gather nothing: its
 * first view bounds neither what it derives nor what of that is new, and
 * room reserved for a large view that yields little would be charged for
 * nothing.  Each item's count is below 2^32 and an arity below 2^16, so
 * neither sum over a round's items can overflow.
 */
static relic_round_expectation_t
round_expectation(const relic_round_t *round)
{
	const relic_insert_history_t *insertions;
	relic_round_expectation_t expected;
	uint16_t arity;
	size_t i;

	bzero(&expected, sizeof(expected));
	for (i = 0; i < round->item_count; i++) {
		insertions = round->items[i].insertions;
		if (insertions == NULL || !insertions->ran)
			continue;

		arity = relic_relation_arity(
		    round->targets[round->items[i].target]);
		expected.candidates += insertions->inserted;
		expected.words += insertions->inserted * arity;
	}

	return (expected);
}

/*
 * The candidates each of `workers` workers is given room for when a round
 * expects `expected`: all of them for one worker, twice an even share of
 * them for several, and never more than all of them, nor more than the
 * `admissible` tuples the tuple limit admits, past which a worker abandons
 * its round anyway.
 */
static size_t
worker_share(uint64_t expected, size_t workers, size_t admissible)
{
	uint64_t share;

	assert(workers > 0);

	if (expected > RELIC_TUPLE_MAX)
		expected = RELIC_TUPLE_MAX;
	if (expected > admissible)
		expected = admissible;

	share = expected;
	if (workers > 1)
		share = 2 * ((expected + workers - 1) / workers);
	if (share > expected)
		share = expected;

	return ((size_t)share);
}

/*
 * Leave a round declined before it was published as though it had never
 * begun: the workers' buffers for strings and their candidates released,
 * with their charge, and whatever preparing its items counted, noted or
 * met forgotten, with the prepared records, which the next round's
 * preparation writes afresh.  Any may hold nothing, and releasing nothing
 * is harmless, so every way a round is declined before publication ends
 * here.
 */
static void
decline_round(relic_t *ctx, relic_pool_t *pool)
{
	(void)end_worker_texts(pool);
	release_candidates(ctx, pool);
	forget_worker_counters(pool);
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
 * longest plan and widest target, and a prepared record for each item.
 * Returns 0, or -1, unreported, when something could not be allocated.
 */
static int
pool_prepare(relic_pool_t *pool, const relic_round_t *round)
{
	relic_prepared_item_t *prepared;
	size_t op_room;
	size_t i;
	uint16_t arity_room;

	round_needs(round, &op_room, &arity_room);
	for (i = 0; i < pool->worker_count; i++) {
		if (worker_fit(&pool->workers[i], op_room, arity_room) != 0)
			return (-1);
	}

	prepared = relic_grow_array(pool->prepared, &pool->prepared_capacity,
	    round->item_count, sizeof(*pool->prepared));
	if (prepared == NULL)
		return (-1);
	pool->prepared = prepared;

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
 * Prepare every item of the round on worker 0, in order: a seed and a plan
 * with no driver are run whole, and are expected to do what
 * whole_estimate() says; a plan with a driver has its prefix run and its
 * driver's visits counted (relic_slice_prepare()), and is expected to do
 * what driven_estimate() says of those visits.  Each item is then given
 * its pieces, `finest` asking for as many as the pool allows.  The links
 * preparation followed are counted whatever becomes of the round.  Returns
 * 0, or -1 when a prefix met a failure or the evaluation was interrupted,
 * for the round to be declined.
 */
static int
prepare_items(relic_t *ctx, relic_pool_t *pool, const relic_round_t *round,
    bool finest)
{
	const relic_round_item_t *item;
	relic_prepared_item_t *prepared;
	size_t driver;
	size_t i;
	int status;

	for (i = 0; i < round->item_count; i++) {
		item = &round->items[i];
		prepared = &pool->prepared[i];

		driver = item->plan == NULL ? RELIC_PLAN_NO_DRIVER :
		    relic_plan_driver(item->plan);
		if (driver == RELIC_PLAN_NO_DRIVER) {
			bzero(prepared, sizeof(*prepared));
			prepared->form = RELIC_TASK_WHOLE;
			prepared->pieces = 1;
			prepared->estimate = whole_estimate(round, item);
			continue;
		}

		status = relic_slice_prepare(prepared,
		    &pool->workers[0].scratch, round->env, item->plan,
		    &ctx->interrupted);
		pool->preparation_links += prepared->links;
		if (status != 0)
			return (-1);

		if (prepared->form == RELIC_TASK_SLICE) {
			prepared->estimate = driven_estimate(round, item,
			    driver, prepared->visits);
			prepared->pieces = item_pieces(pool, prepared, finest);
		}
	}

	return (0);
}

/*
 * How many pieces a SLICE item is cut into: its expected work in pieces of
 * RELIC_CHUNK_WORK, at least one; or, when `finest`, as many as there may
 * be.  There may be no more than the pool's workers use to balance their
 * shares, and no more than the driver's visits, so that each piece holds
 * one.  A plan over a small delta with a large fan-out below it is then
 * cut finely, where cutting by its visits alone would give it one piece,
 * and a single visit is never divided, however much work is below it.
 */
static size_t
item_pieces(const relic_pool_t *pool, const relic_prepared_item_t *prepared,
    bool finest)
{
	uint64_t wanted;
	uint64_t most;

	assert(prepared->form == RELIC_TASK_SLICE && prepared->visits > 0);

	most = (uint64_t)RELIC_CHUNKS_PER_WORKER * pool->worker_count;
	if (most > prepared->visits)
		most = prepared->visits;
	if (finest)
		return ((size_t)most);

	wanted = prepared->estimate / RELIC_CHUNK_WORK;
	if (wanted < 1)
		return (1);
	if (wanted > most)
		return ((size_t)most);

	return ((size_t)wanted);
}

/*
 * Whether a prepared round repays sharing it, by its items' expected work
 * E, the sum of their estimates, against what preparing and sharing it
 * costs.  It does when E is at least RELIC_PARALLEL_MIN_WORK; when E is
 * more than RELIC_PREPARATION_SHARE times the links preparing its drivers
 * follows, counting the pass that will find the starts of the pieces of a
 * lookup cut into several (relic_slice_cut_links()); and when the round
 * would end sooner shared than serially.  Shared, it takes at least an
 * even share of E for each busy worker, one that has a task, and at least
 * its largest piece, a piece's work being taken as an even share of its
 * item's: a round of one large item that is not cut gains nothing.  To
 * that is added what worker 0 does before the tasks run -- the links and
 * each item's setup (item_setup()) -- and RELIC_PARALLEL_STARTUP_WORK, and
 * the sum must be below E.  An estimate that saturated cannot be
 * compared, and declines.
 */
static bool
round_repays(const relic_pool_t *pool, const relic_round_t *round)
{
	const relic_prepared_item_t *prepared;
	uint64_t largest;
	uint64_t links;
	uint64_t piece;
	uint64_t setup;
	uint64_t tasks;
	uint64_t total;
	uint64_t busy;
	uint64_t span;
	size_t i;

	total = 0;
	links = 0;
	setup = 0;
	tasks = 0;
	largest = 0;
	for (i = 0; i < round->item_count; i++) {
		prepared = &pool->prepared[i];
		total = saturating_add(total, prepared->estimate);
		links = saturating_add(links, saturating_add(prepared->links,
		    relic_slice_cut_links(prepared, round->items[i].plan)));
		setup = saturating_add(setup, item_setup(&round->items[i],
		    prepared));
		if (prepared->form != RELIC_TASK_EMPTY)
			tasks += prepared->pieces;

		piece = ceiling_divide(prepared->estimate, prepared->pieces);
		if (piece > largest)
			largest = piece;
	}

	if (total == UINT64_MAX || total < RELIC_PARALLEL_MIN_WORK)
		return (false);
	/* E > RELIC_PREPARATION_SHARE * links, without the product. */
	if (links >= ceiling_divide(total, RELIC_PREPARATION_SHARE))
		return (false);

	busy = tasks < pool->worker_count ? tasks : pool->worker_count;
	span = ceiling_divide(total, busy > 0 ? busy : 1);
	if (largest > span)
		span = largest;

	return (saturating_add(saturating_add(span, links), saturating_add(
	    setup, RELIC_PARALLEL_STARTUP_WORK)) < total);
}

/*
 * What worker 0 does for a prepared item before its tasks run, besides
 * following links: a unit for each operation of its prefix, as the cost
 * model counts a test (cost.h), whether or not the prefix ran to its end,
 * the tuples its prefix read, and a unit for each task it is cut into.
 */
static uint64_t
item_setup(const relic_round_item_t *item,
    const relic_prepared_item_t *prepared)
{
	size_t driver;
	size_t prefix;

	prefix = 0;
	if (item->plan != NULL) {
		driver = relic_plan_driver(item->plan);
		if (driver != RELIC_PLAN_NO_DRIVER)
			prefix = driver;
	}

	return (saturating_add(saturating_add(prefix, relic_exec_work(0,
	    &prepared->prefix_reads)), prepared->pieces));
}

/*
 * Whether an item's plan has a driver and it is a LOOKUP.
 */
static bool
is_lookup_driven(const relic_round_item_t *item)
{
	size_t driver;

	if (item->plan == NULL)
		return (false);
	driver = relic_plan_driver(item->plan);

	return (driver != RELIC_PLAN_NO_DRIVER &&
	    item->plan->ops[driver].kind == RELIC_OP_LOOKUP);
}

/*
 * Enumerate the round's tasks in serial order: item by item, and the
 * slices of a SLICE item in the order of its driver's visits.  The task
 * array is grown once, to the round's pieces, and nothing is published
 * until every task is written.  Returns 0, or -1, unreported, when the
 * array could not grow, or the evaluation was interrupted or a lookup's
 * chain ended short while its pieces were found, for the round to be
 * declined.
 */
static int
cut_tasks(relic_t *ctx, relic_pool_t *pool, const relic_round_t *round)
{
	relic_prepared_item_t *prepared;
	relic_round_task_t *tasks;
	relic_round_task_t *task;
	uint64_t links;
	size_t count;
	size_t piece;
	size_t first;
	size_t i;
	int status;

	count = 0;
	for (i = 0; i < round->item_count; i++) {
		if (pool->prepared[i].pieces > SIZE_MAX - count)
			return (-1);
		count += pool->prepared[i].pieces;
	}
	tasks = relic_grow_array(pool->tasks, &pool->task_capacity, count,
	    sizeof(*pool->tasks));
	if (tasks == NULL)
		return (-1);
	pool->tasks = tasks;

	pool->task_count = 0;
	for (i = 0; i < round->item_count; i++) {
		prepared = &pool->prepared[i];
		assert(prepared->pieces >= 1);
		first = pool->task_count;
		prepared->first_task = first;
		for (piece = 0; piece < prepared->pieces; piece++) {
			task = &pool->tasks[pool->task_count];
			pool->task_count++;

			bzero(task, sizeof(*task));
			task->item = i;
			task->form = prepared->form;
		}

		if (prepared->form != RELIC_TASK_SLICE)
			continue;
		links = 0;
		status = relic_slice_cut(prepared, round->items[i].plan,
		    &pool->tasks[first], &ctx->interrupted, &links);
		pool->preparation_links += links;
		if (status != 0)
			return (-1);
	}

	return (0);
}

/*
 * Report each item's first mixed-type comparisons in item order, its
 * prefix's before its tasks' and its tasks' in task order, which is the
 * order the serial evaluator meets them in, once the round has merged.
 * The serial evaluator looks for an interrupt as it goes on from each
 * note, so an interrupt the diagnostic callback makes is found after the
 * item that noted it, and named for that item's target.  Returns 0, or -1
 * when it was found and reported, the later items' notes unreported.
 */
static int
report_notes(relic_t *ctx, relic_pool_t *pool, const relic_round_t *round)
{
	relic_prepared_item_t *prepared;
	const relic_round_item_t *item;
	size_t task;
	size_t i;

	for (i = 0; i < round->item_count; i++) {
		prepared = &pool->prepared[i];
		item = &round->items[i];
		relic_report_mixed_note(ctx, item->rule, &prepared->note);
		for (task = prepared->first_task; task < prepared->first_task +
		    prepared->pieces; task++)
			relic_report_mixed_note(ctx, item->rule,
			    &pool->tasks[task].note);
		if (relic_refuse_interrupted(ctx, "deriving",
		    round->targets[item->target]) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Add what the round counted to the context's counters -- each item's
 * prefix once, and what every worker's tasks counted -- and start counting
 * afresh.  Count the slices of indexed drivers the round ran.
 */
static void
take_counters(relic_pool_t *pool, const relic_round_t *round,
    relic_counters_t *counters)
{
	const relic_prepared_item_t *prepared;
	relic_worker_t *worker;
	size_t i;

	for (i = 0; i < round->item_count; i++) {
		prepared = &pool->prepared[i];
		relic_exec_counters_add(&counters->read,
		    &prepared->prefix_reads);
		if (prepared->form == RELIC_TASK_SLICE &&
		    is_lookup_driven(&round->items[i]))
			pool->lookup_slices += prepared->pieces;
	}

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
