/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * A task sees the store only as frozen.h presents it, as the executor does,
 * which is what keeps it from writing what every other task of its round
 * reads.  The headers that define a context, a relation and an index refuse
 * to be included where this is defined.
 */
#define	RELIC_FROZEN_ONLY

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <strings.h>

#include "round.h"

/*
 * The tasks of a deferred round, as a worker runs them.
 *
 * A task is an item of the round run over the whole of its first
 * operation's view, or over one contiguous piece of it.  It runs the
 * executor exactly as the serial evaluator does, up to the EMIT, and what
 * differs is where a full batch goes: rather than inserted, it is screened.
 * A tuple the frozen store holds is one the serial insertion would have
 * found present, and a tuple the worker has gathered before in this round
 * is one the serial evaluator would have inserted earlier -- the worker
 * claims its tasks in serial order -- and found present now; each is
 * counted as a derivation and dropped.  Any other becomes a candidate,
 * appended to the worker's buffer with the hash the screen computed.
 *
 * So a worker's candidates are its distinct new tuples, in the order it
 * derived them, and never its derivations: a rule deriving the same few
 * tuples from millions of bindings holds a few candidates.  A task's
 * candidates lie together in its worker's buffer, which is how the merge
 * finds them in task order.
 *
 * The buffers are charged to the memory account as they grow.  A growth the
 * limit or the allocator refuses is not reported and is not a failure: the
 * task gives up, the job is abandoned, and the round runs again serially,
 * where whatever refuses it reports itself.
 */

static uint32_t	 candidate_rehash(uint32_t position, void *ctx);
static int	 candidates_add(relic_candidates_t *candidates,
		    relic_account_t *account, size_t target,
		    const relic_dom_t *tuple, uint16_t arity, uint64_t hash);
static size_t	 candidates_bytes(const relic_candidates_t *candidates);
static size_t	 candidates_bytes_after_add(
		    const relic_candidates_t *candidates, uint16_t arity);
static int	 candidates_grow(relic_candidates_t *candidates,
		    uint16_t arity);
static bool	 candidates_hold(const relic_candidates_t *candidates,
		    size_t target, const relic_dom_t *tuple, uint16_t arity,
		    uint64_t hash);
static int	 candidates_reserve(relic_candidates_t *candidates,
		    relic_account_t *account, uint16_t arity);
static void	 place_seed(relic_scratch_t *scratch, const relic_rule_t *rule);
static int	 run_plan_piece(const relic_round_job_t *job,
		    relic_worker_t *worker, const relic_round_item_t *item,
		    const relic_round_task_t *task);
static int	 run_task(const relic_round_job_t *job, relic_worker_t *worker,
		    size_t worker_index, relic_round_task_t *task);
static int	 screen_batch(const relic_round_job_t *job,
		    relic_worker_t *worker, size_t target);
static uint64_t	 worker_work(const relic_worker_t *worker);

/*
 * Every task is run once, by the worker that claimed it, and a worker's
 * claims come in increasing order (relic_pool_for()): its candidates are
 * in serial order, which is what lets its screen drop a repeat as a tuple
 * the serial evaluator would have inserted earlier.
 */
void
relic_round_run_task(void *argument, size_t task, size_t worker_index)
{
	relic_round_job_t *job;

	job = argument;
	if (relic_atomic_bool_load_relaxed(&job->abandoned))
		return;

	if (run_task(job, &job->workers[worker_index], worker_index,
	    &job->tasks[task]) != 0)
		relic_atomic_bool_store_relaxed(&job->abandoned, true);
}

/*
 * Run one task on a worker, gathering its candidates after those of the
 * worker's earlier tasks, and record where they lie, the first mixed-type
 * comparison it met and the work it did.  Returns 0, or -1 when the
 * worker's candidates could not grow or have outgrown the tuple limit.
 */
static int
run_task(const relic_round_job_t *job, relic_worker_t *worker,
    size_t worker_index, relic_round_task_t *task)
{
	const relic_round_item_t *item;
	relic_scratch_t *scratch;
	uint64_t work_before;
	int status;

	item = &job->round->items[task->item];
	scratch = &worker->scratch;

	task->worker = worker_index;
	task->first_candidate = worker->candidates.count;
	work_before = worker_work(worker);

	if (item->plan == NULL) {
		place_seed(scratch, item->rule);
		status = screen_batch(job, worker, item->target);
	} else {
		status = run_plan_piece(job, worker, item, task);
	}

	task->candidate_count = worker->candidates.count -
	    task->first_candidate;
	task->note = scratch->note;
	scratch->note.filter = NULL;
	task->work = worker_work(worker) - work_before;

	return (status);
}

/*
 * The work a worker has counted since its counts were last taken: the
 * tuples it read and the tuples it derived.
 */
static uint64_t
worker_work(const relic_worker_t *worker)
{
	return (relic_exec_work(worker->derivations,
	    &worker->scratch.counters));
}

/*
 * Put a seed's one tuple, its head of constants, in the scratch's batch, as
 * the EMIT of a plan puts the tuples it derives.
 */
static void
place_seed(relic_scratch_t *scratch, const relic_rule_t *rule)
{
	uint16_t column;

	assert(scratch->batch_count == 0);
	assert(rule->head_arity <= scratch->arity_room);

	for (column = 0; column < rule->head_arity; column++)
		scratch->batch[column] = rule->head_args[column].u.constant;
	scratch->batch_count = 1;
}

/*
 * Run a task's plan over its piece, or over its whole first view when it is
 * not cut, screening each batch as it fills.
 */
static int
run_plan_piece(const relic_round_job_t *job, relic_worker_t *worker,
    const relic_round_item_t *item, const relic_round_task_t *task)
{
	enum relic_exec_status status;
	relic_scratch_t *scratch;

	scratch = &worker->scratch;
	if (task->cut)
		relic_exec_start_piece(scratch, job->round->env, item->plan,
		    task->piece);
	else
		relic_exec_start(scratch, job->round->env, item->plan);

	do {
		status = relic_exec_run(scratch);
		if (screen_batch(job, worker, item->target) != 0)
			return (-1);
	} while (status == RELIC_EXEC_BATCH_FULL);

	return (0);
}

/*
 * Screen the scratch's batch of tuples for target `target` and empty it.
 * Each is a derivation.  One the frozen store holds is dropped, and so is
 * one among the worker's candidates; any other becomes a candidate.
 * Returns 0, or -1 when the candidates could not grow or now hold more
 * tuples than the tuple limit could admit.
 */
static int
screen_batch(const relic_round_job_t *job, relic_worker_t *worker,
    size_t target)
{
	uint64_t hashes[RELIC_INSERT_BATCH];
	bool held[RELIC_INSERT_BATCH];
	const relic_relation_t *relation;
	const relic_dom_t *tuple;
	relic_scratch_t *scratch;
	uint16_t arity;
	size_t i;

	scratch = &worker->scratch;
	relation = job->round->targets[target];
	arity = relic_relation_arity(relation);

	relic_relation_screen(relation, scratch->batch, scratch->batch_count,
	    hashes, held);

	for (i = 0; i < scratch->batch_count; i++) {
		tuple = &scratch->batch[i * arity];
		worker->derivations++;

		if (held[i] || candidates_hold(&worker->candidates, target,
		    tuple, arity, hashes[i]))
			continue;
		if (candidates_add(&worker->candidates, job->account, target,
		    tuple, arity, hashes[i]) != 0)
			return (-1);
	}
	scratch->batch_count = 0;

	if (worker->candidates.count > job->admissible)
		return (-1);

	return (0);
}

/*
 * The screen hash of the candidate at position `position` minus one, for
 * the screen to use when it grows.
 */
static uint32_t
candidate_rehash(uint32_t position, void *ctx)
{
	const relic_candidates_t *candidates;
	const relic_candidate_t *candidate;

	candidates = ctx;
	candidate = &candidates->entries[position - 1];

	return (relic_candidate_screen_hash(candidate->target,
	    candidate->hash));
}

/*
 * Whether the worker has gathered this tuple for this target already.
 */
static bool
candidates_hold(const relic_candidates_t *candidates, size_t target,
    const relic_dom_t *tuple, uint16_t arity, uint64_t hash)
{
	const relic_candidate_t *candidate;
	relic_idprobe_t probe;
	uint32_t position;

	relic_idprobe_open(&probe, &candidates->screen,
	    relic_candidate_screen_hash(target, hash));
	while (relic_idprobe_next(&probe, &position)) {
		candidate = &candidates->entries[position - 1];
		if (candidate->target == target &&
		    relic_tuple_equal(&candidates->words[candidate->offset],
		    tuple, arity))
			return (true);
	}

	return (false);
}

/*
 * Add a tuple the worker has not gathered for this target.  Returns 0, or
 * -1 when the candidates could not grow.
 */
static int
candidates_add(relic_candidates_t *candidates, relic_account_t *account,
    size_t target, const relic_dom_t *tuple, uint16_t arity, uint64_t hash)
{
	relic_candidate_t *candidate;
	size_t stripe;
	uint16_t column;

	if (candidates_reserve(candidates, account, arity) != 0)
		return (-1);

	candidate = &candidates->entries[candidates->count];
	candidate->offset = candidates->word_count;
	candidate->target = (uint32_t)target;
	candidate->hash = hash;
	candidate->repeat = false;
	stripe = relic_hash_stripe(hash);
	candidates->stripes[candidates->count] = (uint8_t)stripe;
	candidates->by_stripe[stripe]++;

	for (column = 0; column < arity; column++)
		candidates->words[candidates->word_count + column] =
		    tuple[column];
	candidates->word_count += arity;
	candidates->count++;

	relic_idtab_place(&candidates->screen,
	    relic_candidate_screen_hash(target, hash),
	    (uint32_t)candidates->count);

	return (0);
}

/*
 * Make room for one more candidate of `arity` columns, charging the growth
 * to the account before it is made and giving back whatever was charged and
 * not grown.  Returns 0, or -1 when the limit refused the charge or the
 * allocator the growth.
 *
 * A worker holds no more candidates than a relation holds tuples, which
 * also keeps the screen's positions plus one inside its identifiers.
 */
static int
candidates_reserve(relic_candidates_t *candidates, relic_account_t *account,
    uint16_t arity)
{
	size_t projected;
	size_t grown;
	int status;

	if (candidates->count >= RELIC_TUPLE_MAX)
		return (-1);

	projected = candidates_bytes_after_add(candidates, arity);
	if (projected == candidates->charged)
		return (0);
	if (projected == SIZE_MAX)
		return (-1);

	assert(projected > candidates->charged);
	if (!relic_account_charge_round(account,
	    projected - candidates->charged))
		return (-1);

	status = candidates_grow(candidates, arity);

	grown = candidates_bytes(candidates);
	assert(grown <= projected);
	relic_account_release_round(account, projected - grown);
	candidates->charged = grown;

	return (status);
}

/*
 * Grow the candidate arrays to hold one more candidate of `arity` columns.
 * Whatever a failed growth managed is kept, and counted by the caller.
 */
static int
candidates_grow(relic_candidates_t *candidates, uint16_t arity)
{
	relic_candidate_t *entries;
	uint8_t *stripes;
	relic_dom_t *words;

	entries = relic_grow_array(candidates->entries, &candidates->capacity,
	    candidates->count + 1, sizeof(*candidates->entries));
	if (entries == NULL)
		return (-1);
	candidates->entries = entries;

	stripes = relic_grow_array(candidates->stripes,
	    &candidates->stripe_capacity, candidates->count + 1,
	    sizeof(*candidates->stripes));
	if (stripes == NULL)
		return (-1);
	candidates->stripes = stripes;

	words = relic_grow_array(candidates->words, &candidates->word_capacity,
	    candidates->word_count + arity, sizeof(*candidates->words));
	if (words == NULL)
		return (-1);
	candidates->words = words;

	return (relic_idtab_reserve(&candidates->screen, 1, candidate_rehash,
	    candidates));
}

/*
 * The bytes the candidate arrays hold.
 */
static size_t
candidates_bytes(const relic_candidates_t *candidates)
{
	return (candidates->capacity * sizeof(*candidates->entries) +
	    candidates->stripe_capacity * sizeof(*candidates->stripes) +
	    candidates->word_capacity * sizeof(*candidates->words) +
	    relic_idtab_bytes(&candidates->screen));
}

/*
 * The bytes the candidate arrays would hold once grown for one more
 * candidate of `arity` columns, asked of the growth policies they grow by,
 * or SIZE_MAX when that growth could not happen.
 */
static size_t
candidates_bytes_after_add(const relic_candidates_t *candidates,
    uint16_t arity)
{
	size_t entry_capacity;
	size_t stripe_capacity;
	size_t word_capacity;
	size_t bytes;

	entry_capacity = relic_grow_array_capacity(candidates->capacity,
	    candidates->count + 1);
	stripe_capacity = relic_grow_array_capacity(
	    candidates->stripe_capacity, candidates->count + 1);
	word_capacity = relic_grow_array_capacity(candidates->word_capacity,
	    candidates->word_count + arity);
	if (entry_capacity == 0 || stripe_capacity == 0 || word_capacity == 0)
		return (SIZE_MAX);

	bytes = relic_saturating_sum(
	    relic_saturating_product(entry_capacity,
	    sizeof(*candidates->entries)),
	    relic_saturating_product(stripe_capacity,
	    sizeof(*candidates->stripes)));
	bytes = relic_saturating_sum(bytes,
	    relic_saturating_product(word_capacity,
	    sizeof(*candidates->words)));

	return (relic_saturating_sum(bytes,
	    relic_idtab_bytes_after_reserve(&candidates->screen, 1)));
}

/*
 * The screen is tagged: a tuple new to the worker, which most are, is then
 * compared against no candidate whose slot it passes on the way to an empty
 * one, and those comparisons read columns far from the table.
 */
void
relic_candidates_init(relic_candidates_t *candidates)
{
	bzero(candidates, sizeof(*candidates));
	relic_idtab_init(&candidates->screen, true);
}

void
relic_candidates_release(relic_candidates_t *candidates,
    relic_account_t *account)
{
	relic_free_array(candidates->entries);
	relic_free_array(candidates->stripes);
	relic_free_array(candidates->words);
	relic_idtab_reset(&candidates->screen);
	relic_account_release_round(account, candidates->charged);

	candidates->entries = NULL;
	candidates->stripes = NULL;
	candidates->words = NULL;
	bzero(candidates->by_stripe, sizeof(candidates->by_stripe));
	candidates->count = 0;
	candidates->capacity = 0;
	candidates->stripe_capacity = 0;
	candidates->word_count = 0;
	candidates->word_capacity = 0;
	candidates->charged = 0;
}
