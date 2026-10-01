/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
 * A value the context lacked when the round began -- an integer or a
 * string a column of the batch holds pending -- is kept among the worker's
 * pending values, a string with its bytes, and the column is rewritten to
 * name it there before the batch is screened.  Equal values then hold equal
 * columns, so a tuple holding one is screened by value as any other is, and
 * the merge gives every such value its identifier.
 *
 * The buffers are charged to the memory account as they grow.  A growth the
 * limit or the allocator refuses is not reported and is not a failure: the
 * task gives up, the job is abandoned, and the round runs again serially,
 * where whatever refuses it reports itself.
 */

typedef struct relic_candidates_growth relic_candidates_growth_t;

/*
 * What the candidate arrays are grown by: `tuples` more candidates of
 * `words` columns in all, and `values` more pending values holding
 * `value_bytes` bytes of strings between them.
 */
struct relic_candidates_growth {
	size_t	tuples;
	size_t	words;
	size_t	values;
	size_t	value_bytes;
};

static size_t	 array_bytes_after(size_t capacity, size_t needed,
		    bool grows, size_t size);
static uint32_t	 candidate_rehash(uint32_t position, void *ctx);
static int	 candidates_add(relic_candidates_t *candidates,
		    relic_account_t *account, size_t target,
		    const relic_dom_t *tuple, uint16_t arity, uint64_t hash);
static size_t	 candidates_bytes(const relic_candidates_t *candidates);
static size_t	 candidates_bytes_after(const relic_candidates_t *candidates,
		    const relic_candidates_growth_t *growth);
static int	 candidates_grow(relic_candidates_t *candidates,
		    const relic_candidates_growth_t *growth);
static bool	 candidates_hold(const relic_candidates_t *candidates,
		    size_t target, const relic_dom_t *tuple, uint16_t arity,
		    uint64_t hash);
static int	 candidates_mark_value(relic_candidates_t *candidates,
		    relic_account_t *account, const relic_pending_t *pending,
		    const char *pending_bytes, relic_dom_t *markerp);
static int	 candidates_reserve(relic_candidates_t *candidates,
		    relic_account_t *account,
		    const relic_candidates_growth_t *growth);
static bool	 pending_equal(const relic_candidates_t *candidates,
		    const relic_pending_t *held, const relic_pending_t *pending,
		    const char *pending_bytes);
static uint32_t	 pending_hash(uint64_t seed, const relic_pending_t *pending,
		    const char *bytes);
static const char *pending_text(const char *bytes,
		    const relic_pending_t *pending);
static uint32_t	 value_rehash(uint32_t position, void *ctx);
static int	 mark_pending(relic_candidates_t *candidates,
		    relic_account_t *account, relic_scratch_t *scratch,
		    uint16_t arity);
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
 * worker's candidates could not grow or have outgrown the tuple limit, or
 * the task met an evaluation error, which the serial run reports.
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
	scratch->note.where = NULL;
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
 * not cut, screening each batch as it fills.  A plan that met an evaluation
 * error has ended, and what its batch holds is not screened: the round is
 * abandoned, and its serial run meets the error, or one before it, and
 * reports it.
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
		if (scratch->error.failed)
			return (-1);
		if (screen_batch(job, worker, item->target) != 0)
			return (-1);
	} while (status == RELIC_EXEC_BATCH_FULL);

	return (0);
}

/*
 * Screen the scratch's batch of tuples for target `target` and empty it.
 * Each is a derivation.  One the frozen store holds is dropped, and so is
 * one among the worker's candidates; any other becomes a candidate.  A
 * tuple holding a pending value is never in the frozen store, whose
 * relations hold only values the context had assigned.  Returns 0, or -1
 * when the candidates could not grow or now hold more tuples than the
 * tuple limit could admit.
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

	if (mark_pending(&worker->candidates, job->account, scratch,
	    arity) != 0)
		return (-1);
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
 * Rewrite every pending marker in the scratch's batch of `arity`-column
 * tuples, which names a value among the batch's pending values, to the
 * marker naming it among the worker's, and empty the batch's pending
 * values and their bytes.  Returns 0, or -1 when the worker's values could
 * not grow.
 */
static int
mark_pending(relic_candidates_t *candidates, relic_account_t *account,
    relic_scratch_t *scratch, uint16_t arity)
{
	relic_dom_t *column;
	size_t columns;
	size_t i;

	columns = scratch->batch_count * arity;
	for (i = 0; i < columns && scratch->pending_count > 0; i++) {
		column = &scratch->batch[i];
		if ((*column & RELIC_DOM_TYPE_MASK) != 0)
			continue;

		assert(*column >= 1 && *column <= scratch->pending_count);
		if (candidates_mark_value(candidates, account,
		    &scratch->pending[*column - 1],
		    scratch->pending_bytes.bytes, column) != 0)
			return (-1);
	}
	scratch->pending_count = 0;
	scratch->pending_bytes.used = 0;

	return (0);
}

/*
 * Set *markerp to the marker of a batch's pending value, whose bytes, when
 * it is a string, lie in `pending_bytes`, among the worker's pending values:
 * its position there plus one, adding it, a string's bytes copied, when the
 * worker has not met it.  Returns 0, or -1 when the values could not grow,
 * or a marker for one more would need type bits.
 */
static int
candidates_mark_value(relic_candidates_t *candidates,
    relic_account_t *account, const relic_pending_t *pending,
    const char *pending_bytes, relic_dom_t *markerp)
{
	relic_candidates_growth_t growth;
	relic_pending_t *added;
	relic_idprobe_t probe;
	uint32_t position;
	uint32_t hash;

	hash = pending_hash(candidates->hash_seed, pending, pending_bytes);
	relic_idprobe_open(&probe, &candidates->value_screen, hash);
	while (relic_idprobe_next(&probe, &position)) {
		if (pending_equal(candidates, &candidates->values[position - 1],
		    pending, pending_bytes)) {
			*markerp = position;
			return (0);
		}
	}

	if (candidates->value_count >= RELIC_DOM_INDEX_MASK)
		return (-1);
	bzero(&growth, sizeof(growth));
	growth.values = 1;
	if (pending->kind == RELIC_PENDING_TEXT)
		growth.value_bytes = pending->u.text.length;
	if (candidates_reserve(candidates, account, &growth) != 0)
		return (-1);

	added = &candidates->values[candidates->value_count];
	*added = *pending;
	if (pending->kind == RELIC_PENDING_TEXT) {
		added->u.text.offset = candidates->value_byte_count;
		if (pending->u.text.length > 0)
			bcopy(pending_bytes + pending->u.text.offset,
			    candidates->value_bytes +
			    candidates->value_byte_count,
			    pending->u.text.length);
		candidates->value_byte_count += pending->u.text.length;
	}
	candidates->value_count++;
	relic_idtab_place(&candidates->value_screen, hash,
	    (uint32_t)candidates->value_count);
	*markerp = (relic_dom_t)candidates->value_count;

	return (0);
}

/*
 * The screen hash of a pending value: an integer's of its value, a
 * string's of its bytes, which lie in `bytes`.
 */
static uint32_t
pending_hash(uint64_t seed, const relic_pending_t *pending, const char *bytes)
{
	if (pending->kind == RELIC_PENDING_INTEGER)
		return (relic_hash_int64(seed, pending->u.integer));

	return (relic_hash_bytes(seed, pending_text(bytes, pending),
	    pending->u.text.length));
}

/*
 * The bytes of pending string `pending`, which lie in `bytes`.  An empty
 * string has none, and the buffer that would hold them may not exist yet,
 * so its bytes are a constant empty string rather than an offset into it.
 */
static const char *
pending_text(const char *bytes, const relic_pending_t *pending)
{
	if (pending->u.text.length == 0)
		return ("");

	return (bytes + pending->u.text.offset);
}

/*
 * Whether a value the worker holds is a batch's pending value, whose bytes,
 * when it is a string, lie in `pending_bytes`: of one kind, and the same
 * integer or the same bytes.
 */
static bool
pending_equal(const relic_candidates_t *candidates,
    const relic_pending_t *held, const relic_pending_t *pending,
    const char *pending_bytes)
{
	if (held->kind != pending->kind)
		return (false);
	if (held->kind == RELIC_PENDING_INTEGER)
		return (held->u.integer == pending->u.integer);

	return (held->u.text.length == pending->u.text.length &&
	    bcmp(pending_text(candidates->value_bytes, held),
	    pending_text(pending_bytes, pending), held->u.text.length) == 0);
}

/*
 * The screen hash of the pending value at position `position` minus one,
 * for the value screen to use when it grows.
 */
static uint32_t
value_rehash(uint32_t position, void *ctx)
{
	const relic_candidates_t *candidates;

	candidates = ctx;

	return (pending_hash(candidates->hash_seed,
	    &candidates->values[position - 1], candidates->value_bytes));
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
	relic_candidates_growth_t growth;
	relic_candidate_t *candidate;
	size_t stripe;
	uint16_t column;

	if (candidates->count >= RELIC_TUPLE_MAX)
		return (-1);
	bzero(&growth, sizeof(growth));
	growth.tuples = 1;
	growth.words = arity;
	if (candidates_reserve(candidates, account, &growth) != 0)
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
 * Make room for what `growth` adds, charging the growth to the account
 * before it is made and giving back whatever was charged and not grown.
 * Returns 0, or -1 when the limit refused the charge or the allocator the
 * growth.
 *
 * A worker holds no more candidates than a relation holds tuples, which
 * also keeps the screen's positions plus one inside its identifiers.
 */
static int
candidates_reserve(relic_candidates_t *candidates, relic_account_t *account,
    const relic_candidates_growth_t *growth)
{
	size_t projected;
	size_t grown;
	int status;

	projected = candidates_bytes_after(candidates, growth);
	if (projected == candidates->charged)
		return (0);
	if (projected == SIZE_MAX)
		return (-1);

	assert(projected > candidates->charged);
	if (!relic_account_charge_round(account,
	    projected - candidates->charged))
		return (-1);

	status = candidates_grow(candidates, growth);

	grown = candidates_bytes(candidates);
	assert(grown <= projected);
	relic_account_release_round(account, projected - grown);
	candidates->charged = grown;

	return (status);
}

/*
 * Grow the candidate arrays by `growth`: those of the candidates when it
 * adds a candidate, whatever its arity, those of the pending values when it
 * adds a value, and the values' bytes when it adds a string's.  Whatever a
 * failed growth managed is kept, and counted by the caller.
 */
static int
candidates_grow(relic_candidates_t *candidates,
    const relic_candidates_growth_t *growth)
{
	relic_candidate_t *entries;
	relic_pending_t *values;
	uint8_t *stripes;
	relic_dom_t *words;
	char *bytes;

	if (growth->tuples > 0) {
		entries = relic_grow_array(candidates->entries,
		    &candidates->capacity, candidates->count + growth->tuples,
		    sizeof(*candidates->entries));
		if (entries == NULL)
			return (-1);
		candidates->entries = entries;

		stripes = relic_grow_array(candidates->stripes,
		    &candidates->stripe_capacity,
		    candidates->count + growth->tuples,
		    sizeof(*candidates->stripes));
		if (stripes == NULL)
			return (-1);
		candidates->stripes = stripes;

		words = relic_grow_array(candidates->words,
		    &candidates->word_capacity,
		    candidates->word_count + growth->words,
		    sizeof(*candidates->words));
		if (words == NULL)
			return (-1);
		candidates->words = words;

		if (relic_idtab_reserve(&candidates->screen, growth->tuples,
		    candidate_rehash, candidates) != 0)
			return (-1);
	}

	if (growth->values > 0) {
		values = relic_grow_array(candidates->values,
		    &candidates->value_capacity,
		    candidates->value_count + growth->values,
		    sizeof(*candidates->values));
		if (values == NULL)
			return (-1);
		candidates->values = values;

		if (relic_idtab_reserve(&candidates->value_screen,
		    growth->values, value_rehash, candidates) != 0)
			return (-1);
	}

	if (growth->value_bytes > 0) {
		bytes = relic_grow_array(candidates->value_bytes,
		    &candidates->value_byte_capacity,
		    candidates->value_byte_count + growth->value_bytes, 1);
		if (bytes == NULL)
			return (-1);
		candidates->value_bytes = bytes;
	}

	return (0);
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
	    relic_idtab_bytes(&candidates->screen) +
	    candidates->value_capacity * sizeof(*candidates->values) +
	    relic_idtab_bytes(&candidates->value_screen) +
	    candidates->value_byte_capacity);
}

/*
 * The bytes the candidate arrays would hold once grown by `growth`, asked
 * of the growth policies they grow by, or SIZE_MAX when that growth could
 * not happen.  Which arrays grow is what candidates_grow() grows.
 */
static size_t
candidates_bytes_after(const relic_candidates_t *candidates,
    const relic_candidates_growth_t *growth)
{
	bool tuples;
	bool values;
	bool value_bytes;
	size_t bytes;

	tuples = growth->tuples > 0;
	values = growth->values > 0;
	value_bytes = growth->value_bytes > 0;

	bytes = array_bytes_after(candidates->capacity,
	    candidates->count + growth->tuples, tuples,
	    sizeof(*candidates->entries));
	bytes = relic_saturating_sum(bytes,
	    array_bytes_after(candidates->stripe_capacity,
	    candidates->count + growth->tuples, tuples,
	    sizeof(*candidates->stripes)));
	bytes = relic_saturating_sum(bytes,
	    array_bytes_after(candidates->word_capacity,
	    candidates->word_count + growth->words, tuples,
	    sizeof(*candidates->words)));
	bytes = relic_saturating_sum(bytes,
	    relic_idtab_bytes_after_reserve(&candidates->screen,
	    growth->tuples));
	bytes = relic_saturating_sum(bytes,
	    array_bytes_after(candidates->value_capacity,
	    candidates->value_count + growth->values, values,
	    sizeof(*candidates->values)));
	bytes = relic_saturating_sum(bytes,
	    relic_idtab_bytes_after_reserve(&candidates->value_screen,
	    growth->values));

	return (relic_saturating_sum(bytes,
	    array_bytes_after(candidates->value_byte_capacity,
	    candidates->value_byte_count + growth->value_bytes, value_bytes,
	    1)));
}

/*
 * The bytes a growable array of `capacity` items of `size` bytes holds
 * once grown to hold `needed` when it `grows`, or its present bytes when
 * it does not; SIZE_MAX when it could not grow.
 */
static size_t
array_bytes_after(size_t capacity, size_t needed, bool grows, size_t size)
{
	if (grows) {
		capacity = relic_grow_array_capacity(capacity, needed);
		if (capacity == 0)
			return (SIZE_MAX);
	}

	return (relic_saturating_product(capacity, size));
}

/*
 * The screen is tagged: a tuple new to the worker, which most are, is then
 * compared against no candidate whose slot it passes on the way to an empty
 * one, and those comparisons read columns far from the table.
 */
void
relic_candidates_init(relic_candidates_t *candidates, uint64_t hash_seed)
{
	bzero(candidates, sizeof(*candidates));
	relic_idtab_init(&candidates->screen, true);
	relic_idtab_init(&candidates->value_screen, true);
	candidates->hash_seed = hash_seed;
}

void
relic_candidates_release(relic_candidates_t *candidates,
    relic_account_t *account)
{
	relic_free_array(candidates->entries);
	relic_free_array(candidates->stripes);
	relic_free_array(candidates->words);
	relic_idtab_reset(&candidates->screen);
	relic_free_array(candidates->values);
	relic_free_array(candidates->value_bytes);
	relic_idtab_reset(&candidates->value_screen);
	relic_account_release_round(account, candidates->charged);

	candidates->entries = NULL;
	candidates->stripes = NULL;
	candidates->words = NULL;
	candidates->values = NULL;
	candidates->value_bytes = NULL;
	bzero(candidates->by_stripe, sizeof(candidates->by_stripe));
	candidates->count = 0;
	candidates->capacity = 0;
	candidates->stripe_capacity = 0;
	candidates->word_count = 0;
	candidates->word_capacity = 0;
	candidates->value_count = 0;
	candidates->value_capacity = 0;
	candidates->value_byte_count = 0;
	candidates->value_byte_capacity = 0;
	candidates->charged = 0;
}
