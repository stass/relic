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
#include <stdlib.h>
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
 * finds them in task order. A selected dense round instead lowers shared
 * occurrence ranks, keeping no worker tuple copies (lib/dense.c).
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

typedef struct relic_candidates_array relic_candidates_array_t;
typedef struct relic_candidates_growth relic_candidates_growth_t;
typedef struct relic_candidates_storage relic_candidates_storage_t;
typedef struct relic_candidates_table relic_candidates_table_t;

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

/*
 * One growable array of the candidates as a growth asks of it: its
 * `capacity` in items of `size` bytes, the items it must hold once the
 * growth is made, and whether the growth grows it at all.
 */
struct relic_candidates_array {
	size_t	capacity;
	size_t	needed;
	size_t	size;
	bool	grows;
};

/*
 * One identifier table of the candidates as a growth asks of it: the
 * table, and how many entries the growth adds to it.
 */
struct relic_candidates_table {
	const relic_idtab_t	*table;
	size_t			 added;
};

/*
 * The growable arrays and the identifier tables of a worker's candidates,
 * each named, so that a list of them is indexed by name and holds no more
 * of them than there are.
 */
enum relic_candidate_array {
	RELIC_CANDIDATE_ENTRIES,
	RELIC_CANDIDATE_STRIPES,
	RELIC_CANDIDATE_WORDS,
	RELIC_CANDIDATE_VALUES,
	RELIC_CANDIDATE_VALUE_BYTES,
	RELIC_CANDIDATE_ARRAYS
};

enum relic_candidate_table {
	RELIC_CANDIDATE_SCREEN,
	RELIC_CANDIDATE_VALUE_SCREEN,
	RELIC_CANDIDATE_TABLES
};

/*
 * Every array and table of a worker's candidates, as a growth asks of each.
 */
struct relic_candidates_storage {
	relic_candidates_array_t	arrays[RELIC_CANDIDATE_ARRAYS];
	relic_candidates_table_t	tables[RELIC_CANDIDATE_TABLES];
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
static bool	 candidates_have_room(const relic_candidates_t *candidates,
		    const relic_candidates_growth_t *growth);
static relic_candidate_t *candidates_find(relic_candidates_t *candidates,
		    size_t target, const relic_dom_t *tuple, uint16_t arity,
		    uint64_t hash);
static int	 candidates_mark_value(relic_candidates_t *candidates,
		    relic_account_t *account, const relic_pending_t *pending,
		    const char *pending_bytes, relic_dom_t *markerp);
static void	 candidates_prefetch(const relic_candidates_t *candidates,
		    size_t target, const uint64_t *hashes, const bool *held,
		    size_t count);
static int	 candidates_reserve(relic_candidates_t *candidates,
		    relic_account_t *account,
		    const relic_candidates_growth_t *growth);
static relic_candidates_storage_t candidates_storage(
		    const relic_candidates_t *candidates,
		    const relic_candidates_growth_t *growth);
static int	 mark_pending(relic_candidates_t *candidates,
		    relic_account_t *account, relic_scratch_t *scratch,
		    uint16_t arity);
static bool	 pending_equal(const relic_candidates_t *candidates,
		    const relic_pending_t *held, const relic_pending_t *pending,
		    const char *pending_bytes);
static uint32_t	 pending_hash(uint64_t seed, const relic_pending_t *pending,
		    const char *bytes);
static const char *pending_text(const char *bytes,
		    const relic_pending_t *pending);
static void	 place_seed(relic_scratch_t *scratch, const relic_rule_t *rule);
static int	 run_plan_piece(const relic_round_job_t *job,
		    relic_worker_t *worker, const relic_round_item_t *item,
		    const relic_round_task_t *task);
static int	 run_task(const relic_round_job_t *job, relic_worker_t *worker,
		    size_t worker_index, relic_round_task_t *task);
static int	 screen_batch(const relic_round_job_t *job,
		    relic_worker_t *worker, const relic_round_task_t *task);
static uint32_t	 value_rehash(uint32_t position, void *ctx);
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
		status = screen_batch(job, worker, task);
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
 * not cut, screening each batch as the executor pauses.  A plan that met an
 * evaluation error has ended, and what its batch holds is not screened: the
 * round is abandoned, and its serial run meets the error, or one before it,
 * and reports it.  An interrupt is looked for at every return of the
 * executor and abandons the round alike, to be reported by its serial run.
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
		if (screen_batch(job, worker, task) != 0)
			return (-1);
		if (relic_signal_flag_raised(job->interrupted))
			return (-1);
	} while (status == RELIC_EXEC_PAUSED);

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
    const relic_round_task_t *task)
{
	uint64_t hashes[RELIC_INSERT_BATCH];
	bool held[RELIC_INSERT_BATCH];
	const relic_relation_t *relation;
	const relic_destination_view_t *destination;
	relic_candidate_t *first;
	const relic_dom_t *tuple;
	relic_scratch_t *scratch;
	uint16_t arity;
	size_t target;
	size_t i;

	scratch = &worker->scratch;
	if (job->dense != NULL) {
		for (i = 0; i < scratch->batch_count; i++) {
			worker->derivations++;
			tuple = &scratch->batch[RELIC_PAIR_GRID_ARITY * i];
			if (!relic_dense_take(job->dense,
			    (size_t)(task - job->tasks),
			    &worker->dense_sequence, tuple))
				return (-1);
		}
		scratch->batch_count = 0;
		return (0);
	}
	target = job->round->items[task->item].target;
	destination = relic_destination_view(job->round->targets[target]);
	relation = destination->relation;
	relic_destination_project(destination, scratch->batch,
	    scratch->batch_count);
	arity = relic_relation_arity(relation);

	if (mark_pending(&worker->candidates, job->account, scratch,
	    arity) != 0)
		return (-1);
	relic_relation_screen(relation, scratch->batch, scratch->batch_count,
	    hashes, held);
	candidates_prefetch(&worker->candidates, target, hashes, held,
	    scratch->batch_count);

	for (i = 0; i < scratch->batch_count; i++) {
		tuple = &scratch->batch[i * arity];
		worker->derivations++;

		if (held[i])
			continue;
		first = candidates_find(&worker->candidates, target, tuple,
		    arity, hashes[i]);
		if (first != NULL) {
			first->contribution = relic_contribution_combine(
			    destination, first->contribution, 1);
			continue;
		}
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
	assert(candidates->value_count < candidates->value_capacity &&
	    candidates->value_byte_count + growth.value_bytes <=
	    candidates->value_byte_capacity);

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
 * Ask for the memory the screen's probes of a batch will read, for those of
 * its `count` tuples for target `target` that the frozen store lacks,
 * before any of them is probed, so that their cache misses overlap rather
 * than wait on one another as the probes, one after another, would.
 *
 * A probe that finds a candidate in the slot its hash selects reads three
 * lines, each named by the one before it: the slot, with its tag beside
 * it in the tags' own array; the entry of the candidate the slot holds;
 * and that candidate's columns, which the comparison reads.  So the tuples
 * are passed over three times: every tuple's slot and tag; then the entry
 * of each slot holding a candidate of the tuple's tag; then that entry's
 * columns.  A tuple
 * whose slot is empty, or holds a candidate of another tag, which the probe
 * passes over unread, takes the first pass only, as a tuple new to the
 * worker almost always does.
 *
 * The passes change nothing.  A prefetch is a hint that cannot fault, and
 * a line asked for in vain -- a candidate of the tuple's tag holding other
 * columns, a slot a growth made by the batch's own additions has moved,
 * or, for a candidate of no columns, the place just past the last
 * candidate's columns -- costs only that line.
 */
static void
candidates_prefetch(const relic_candidates_t *candidates, size_t target,
    const uint64_t *hashes, const bool *held, size_t count)
{
	uint32_t screen_hashes[RELIC_INSERT_BATCH];
	uint32_t positions[RELIC_INSERT_BATCH];
	const relic_candidate_t *candidate;
	size_t i;

	assert(count <= RELIC_INSERT_BATCH);

	for (i = 0; i < count; i++) {
		screen_hashes[i] = relic_candidate_screen_hash(target,
		    hashes[i]);
		if (!held[i])
			relic_idtab_prefetch(&candidates->screen,
			    screen_hashes[i]);
	}

	for (i = 0; i < count; i++) {
		positions[i] = RELIC_ID_NONE;
		if (!held[i])
			positions[i] = relic_idtab_peek(&candidates->screen,
			    screen_hashes[i]);
		if (positions[i] != RELIC_ID_NONE)
			RELIC_PREFETCH(&candidates->entries[positions[i] - 1]);
	}

	for (i = 0; i < count; i++) {
		if (positions[i] != RELIC_ID_NONE) {
			candidate = &candidates->entries[positions[i] - 1];
			RELIC_PREFETCH(&candidates->words[candidate->offset]);
		}
	}
}

/*
 * Whether the worker has gathered this tuple for this target already.
 */
static relic_candidate_t *
candidates_find(relic_candidates_t *candidates, size_t target,
    const relic_dom_t *tuple, uint16_t arity, uint64_t hash)
{
	relic_candidate_t *candidate;
	relic_idprobe_t probe;
	uint32_t position;

	relic_idprobe_open(&probe, &candidates->screen,
	    relic_candidate_screen_hash(target, hash));
	while (relic_idprobe_next(&probe, &position)) {
		candidate = &candidates->entries[position - 1];
		if (candidate->target == target &&
		    relic_tuple_equal(&candidates->words[candidate->offset],
		    tuple, arity))
			return (candidate);
	}

	return (NULL);
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
	assert(candidates->count < candidates->capacity &&
	    candidates->count < candidates->stripe_capacity &&
	    candidates->word_count + arity <= candidates->word_capacity);

	candidate = &candidates->entries[candidates->count];
	candidate->offset = candidates->word_count;
	candidate->target = (uint32_t)target;
	candidate->hash = hash;
	candidate->contribution = 1;
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

	if (candidates_have_room(candidates, growth)) {
		assert(candidates_bytes_after(candidates, growth) ==
		    candidates->charged);
		return (0);
	}

	projected = candidates_bytes_after(candidates, growth);
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
 * Every array and table of the candidates, as `growth` asks of each.  This
 * is the one list of them: what they hold, what they would hold once
 * grown and whether any must grow are each computed from it, and what
 * candidates_grow() grows is checked against it.
 */
static relic_candidates_storage_t
candidates_storage(const relic_candidates_t *candidates,
    const relic_candidates_growth_t *growth)
{
	relic_candidates_storage_t storage = {
		.arrays = {
			[RELIC_CANDIDATE_ENTRIES] = {
				.capacity = candidates->capacity,
				.needed = candidates->count + growth->tuples,
				.size = sizeof(*candidates->entries),
				.grows = growth->tuples > 0,
			},
			[RELIC_CANDIDATE_STRIPES] = {
				.capacity = candidates->stripe_capacity,
				.needed = candidates->count + growth->tuples,
				.size = sizeof(*candidates->stripes),
				.grows = growth->tuples > 0,
			},
			[RELIC_CANDIDATE_WORDS] = {
				.capacity = candidates->word_capacity,
				.needed = candidates->word_count +
				    growth->words,
				.size = sizeof(*candidates->words),
				.grows = growth->tuples > 0,
			},
			[RELIC_CANDIDATE_VALUES] = {
				.capacity = candidates->value_capacity,
				.needed = candidates->value_count +
				    growth->values,
				.size = sizeof(*candidates->values),
				.grows = growth->values > 0,
			},
			[RELIC_CANDIDATE_VALUE_BYTES] = {
				.capacity = candidates->value_byte_capacity,
				.needed = candidates->value_byte_count +
				    growth->value_bytes,
				.size = 1,
				.grows = growth->value_bytes > 0,
			},
		},
		.tables = {
			[RELIC_CANDIDATE_SCREEN] = {
				.table = &candidates->screen,
				.added = growth->tuples,
			},
			[RELIC_CANDIDATE_VALUE_SCREEN] = {
				.table = &candidates->value_screen,
				.added = growth->values,
			},
		},
	};

	return (storage);
}

/*
 * Whether the candidate arrays already hold what `growth` adds, so that
 * candidates_grow() would grow none of them and nothing is charged: each
 * array that grows holds the items it needs, and each table has room for
 * its entries.  It compares counts with capacities, which is cheaper than
 * projecting bytes through each array's growth policy; most candidates are
 * added to arrays that already hold them.
 */
static bool
candidates_have_room(const relic_candidates_t *candidates,
    const relic_candidates_growth_t *growth)
{
	const relic_candidates_array_t *array;
	const relic_candidates_table_t *table;
	relic_candidates_storage_t storage;
	size_t i;

	storage = candidates_storage(candidates, growth);
	for (i = 0; i < RELIC_CANDIDATE_ARRAYS; i++) {
		array = &storage.arrays[i];
		if (array->grows &&
		    !relic_grow_array_holds(array->capacity, array->needed))
			return (false);
	}
	for (i = 0; i < RELIC_CANDIDATE_TABLES; i++) {
		table = &storage.tables[i];
		if (!relic_idtab_has_room(table->table, table->added))
			return (false);
	}

	return (true);
}

/*
 * Grow the candidate arrays by `growth`: those of the candidates when it
 * adds a candidate, whatever its arity, those of the pending values when it
 * adds a value, and the values' bytes when it adds a string's, after which
 * every array and table candidates_storage() names has room for it.
 * Whatever a failed growth managed is kept, and counted by the caller.
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

	assert(candidates_have_room(candidates, growth));

	return (0);
}

/*
 * The bytes the candidate arrays and tables hold: what they would hold
 * once grown by nothing.
 */
static size_t
candidates_bytes(const relic_candidates_t *candidates)
{
	const relic_candidates_growth_t none = { 0 };
	relic_candidates_storage_t storage;
	size_t bytes;
	size_t i;

	storage = candidates_storage(candidates, &none);

	bytes = 0;
	for (i = 0; i < RELIC_CANDIDATE_ARRAYS; i++)
		bytes += storage.arrays[i].capacity * storage.arrays[i].size;
	for (i = 0; i < RELIC_CANDIDATE_TABLES; i++)
		bytes += relic_idtab_bytes(storage.tables[i].table);

	return (bytes);
}

/*
 * The bytes the candidate arrays and tables would hold once grown by
 * `growth`, asked of the growth policies they grow by: SIZE_MAX when an
 * array could not grow, and for a table that could not, its present bytes
 * (relic_idtab_bytes_after_reserve()), which a table bounded by
 * RELIC_TUPLE_MAX entries never meets.
 */
static size_t
candidates_bytes_after(const relic_candidates_t *candidates,
    const relic_candidates_growth_t *growth)
{
	const relic_candidates_array_t *array;
	const relic_candidates_table_t *table;
	relic_candidates_storage_t storage;
	size_t bytes;
	size_t i;

	storage = candidates_storage(candidates, growth);

	bytes = 0;
	for (i = 0; i < RELIC_CANDIDATE_ARRAYS; i++) {
		array = &storage.arrays[i];
		bytes = relic_saturating_sum(bytes,
		    array_bytes_after(array->capacity, array->needed,
		    array->grows, array->size));
	}
	for (i = 0; i < RELIC_CANDIDATE_TABLES; i++) {
		table = &storage.tables[i];
		bytes = relic_saturating_sum(bytes,
		    relic_idtab_bytes_after_reserve(table->table,
		    table->added));
	}

	return (bytes);
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

/*
 * The room is made as a growth by that many candidates of that many
 * columns would make it, and charged likewise, so that the candidates
 * gathered into it grow nothing until they pass it.
 */
int
relic_candidates_reserve(relic_candidates_t *candidates,
    relic_account_t *account, size_t tuples, size_t words)
{
	relic_candidates_growth_t growth;

	assert(candidates->count == 0 && tuples <= RELIC_TUPLE_MAX);

	bzero(&growth, sizeof(growth));
	growth.tuples = tuples;
	growth.words = words;

	return (candidates_reserve(candidates, account, &growth));
}

void
relic_candidates_release(relic_candidates_t *candidates,
    relic_account_t *account)
{
	free(candidates->entries);
	free(candidates->stripes);
	free(candidates->words);
	relic_idtab_reset(&candidates->screen);
	free(candidates->values);
	free(candidates->value_bytes);
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
