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
 * The merge of a deferred round: inserting what its tasks gathered, on the
 * pool's workers, as the serial evaluator would have inserted it.
 *
 * The serial evaluator inserts the tuples a round derives one at a time,
 * and what it leaves is fixed by one list: the tuples new to their
 * relation, in the order they were first derived.  Each takes the next
 * identifier of its relation, the next slot of its canonical stripe, the
 * next key of its secondary index's stripe where its key is new, and the
 * end of its key's chain; every array grows a step at a time to a capacity
 * set by what it comes to hold.  The candidates, read task by task in task
 * order, are that derivation order with some repeats already removed, so
 * the list is the candidates that no earlier candidate repeats -- the
 * firsts -- in that order.  The merge finds the firsts, gives each its
 * identifier, and then makes every part of every relation what the serial
 * insertions would have made it, each part at once.
 *
 *	resolve		(worker 0) promise each integer the candidates hold
 *			pending the identifier the serial run gives it, and
 *			write it over each marker, hashing the candidate
 *			again;
 *	find new tuples	a stripe of the canonical index to a part: mark each
 *			candidate of the stripe a first or a repeat of an
 *			earlier one, counting each task's firsts;
 *	count		(worker 0) each task's firsts give it the positions
 *			its new tuples take in its target's append, and each
 *			target its count per stripe; the tuple limit and the
 *			identifier space are asked of the exact total;
 *	stage		a task to a part: copy its firsts, with their hashes
 *			and their keys' hashes, into their positions;
 *	classify	a stripe of a secondary index to a part: find the key
 *			each staged tuple of the stripe joins, numbering the
 *			stripe's new keys;
 *	reserve		(worker 0) ask the memory limit of exactly what the
 *			serial insertions would hold, with the tables their
 *			growth replaces held beside it, and grow every array
 *			to it, preparing each table's growth without filling
 *			it; grow the integer table for the new integers;
 *	apply		(worker 0) append the new integers and store the
 *			columns; then a stripe to a part, fill each grown
 *			table, place the canonical entries and link the
 *			chains.
 *
 * No step up to the application changes what a relation or the integer
 * table holds, and every step that can fail -- a scratch table the limit or
 * the allocator refuses, a limit the new tuples or integers would pass, a
 * growth refused -- is among them.  A failure declines the merge, and the
 * round runs again serially, where whatever refuses it reports itself; the
 * merge itself never reports and never fails part way.  Every part of a
 * step writes only what its stripe, its task or its worker owns, and the
 * pool's barrier between steps publishes what each step wrote to the
 * next.  Which parts a relation's append is
 * classified and applied in is the relation store's to say, and the merge
 * runs every part it names.
 */

typedef struct relic_merge relic_merge_t;
typedef struct relic_merge_firsts relic_merge_firsts_t;
typedef struct relic_merge_part relic_merge_part_t;
typedef struct relic_merge_ref relic_merge_ref_t;

/*
 * A candidate, named by the worker that gathered it and its position among
 * that worker's candidates, which a worker keeps below RELIC_TUPLE_MAX.
 */
struct relic_merge_ref {
	uint32_t	worker;
	uint32_t	position;
};

/*
 * The firsts of one stripe met so far: `table` maps a candidate's screen
 * hash to its position in `refs` plus one.  It is made with room for every
 * candidate of the stripe, so finding the firsts never allocates.
 */
struct relic_merge_firsts {
	relic_idtab_t		 table;
	relic_merge_ref_t	*refs;
	size_t			 count;
};

/*
 * A part of the work on one target's append, as the relation store numbers
 * the parts of a classification or of an application.
 */
struct relic_merge_part {
	size_t	target;
	size_t	part;
};

/*
 * A merge under way.  `appends` holds each target's new tuples, and
 * `projected` the bytes each target will hold once it has taken them.
 * `pending` holds the round's `pending_count` pending integers in task
 * order, `pending_map` the identifier each is promised, and
 * `table_projected` the bytes the value table and the pool will hold once
 * they have taken the new ones; `pending_charged` is what the two arrays
 * were charged.
 * `task_firsts[t * RELIC_STRIPES + s]` counts task t's firsts in stripe s,
 * and `task_starts[t]` is the position task t's first new tuple takes in
 * its target's append.  `classify_parts` and `apply_parts` list the parts
 * of every target's append.  `refused` is set by a part that could
 * not have the memory it needed, and declines the merge.
 */
struct relic_merge {
	relic_t			*ctx;
	relic_pool_t		*pool;
	relic_round_t		*round;
	relic_append_t		*appends;
	size_t			*projected;
	size_t			*task_firsts;
	size_t			*task_starts;
	relic_merge_part_t	*classify_parts;
	size_t			 classify_part_count;
	relic_merge_part_t	*apply_parts;
	size_t			 apply_part_count;
	relic_pending_t		*pending;
	size_t			 pending_count;
	relic_pending_map_t	 pending_map;
	size_t			 pending_charged;
	size_t			 table_projected;
	relic_atomic_bool_t	 refused;
};

static void	 apply_merge(relic_merge_t *merge);
static void	 apply_part(void *argument, size_t part, size_t worker_index);
static void	*charged_array(relic_account_t *account, size_t count,
		    size_t size, size_t *chargedp);
static int	 classify_new_keys(relic_merge_t *merge);
static void	 classify_part(void *argument, size_t part,
		    size_t worker_index);
static int	 count_new_tuples(relic_merge_t *merge);
static const relic_candidate_t *entry_of(const relic_merge_t *merge,
		    relic_merge_ref_t ref);
static int	 find_new_tuples(relic_merge_t *merge);
static void	 gather_pending(relic_merge_t *merge);
static void	 find_new_in_stripe(void *argument, size_t stripe,
		    size_t worker_index);
static bool	 firsts_hold(const relic_merge_t *merge,
		    const relic_merge_firsts_t *firsts, relic_merge_ref_t ref);
static void	 firsts_add(const relic_merge_t *merge,
		    relic_merge_firsts_t *firsts, relic_merge_ref_t ref);
static void	 firsts_close(relic_account_t *account,
		    relic_merge_firsts_t *firsts, size_t charged);
static int	 firsts_open(relic_account_t *account,
		    relic_merge_firsts_t *firsts, size_t count,
		    size_t *chargedp);
static int	 list_parts(const relic_merge_t *merge,
		    size_t (*parts_of)(const relic_append_t *),
		    relic_merge_part_t **partsp, size_t *countp);
static int	 merge_begin(relic_merge_t *merge, relic_t *ctx,
		    relic_pool_t *pool, relic_round_t *round);
static bool	 holds_marker(const relic_dom_t *tuple, uint16_t arity);
static void	 merge_end(relic_merge_t *merge);
static void	 new_keys_close(relic_account_t *account,
		    relic_new_keys_t *new_keys, size_t charged);
static int	 new_keys_open(relic_account_t *account,
		    relic_new_keys_t *new_keys, size_t count, size_t *chargedp);
static size_t	 pending_columns(const relic_pool_t *pool);
static int	 pending_open(relic_merge_t *merge, size_t count);
static int	 prepare_merge(relic_merge_t *merge, relic_t *ctx,
		    relic_pool_t *pool, relic_round_t *round);
static void	 rehash_worker(void *argument, size_t worker,
		    size_t worker_index);
static int	 reserve_room(relic_merge_t *merge);
static int	 resolve_integers(relic_merge_t *merge);
static int	 scratch_table_open(relic_account_t *account,
		    relic_idtab_t *table, size_t count, size_t *chargedp);
static int	 stage_new_tuples(relic_merge_t *merge);
static void	 stage_task(void *argument, size_t task, size_t worker_index);
static size_t	 stripe_candidates(const relic_pool_t *pool, size_t stripe);
static size_t	 task_first_count(const relic_merge_t *merge, size_t task);
static const relic_dom_t *tuple_of(const relic_merge_t *merge,
		    relic_merge_ref_t ref);

bool
relic_round_merge(relic_t *ctx, relic_pool_t *pool, relic_round_t *round)
{
	relic_merge_t merge;
	bool merged;

	merged = prepare_merge(&merge, ctx, pool, round) == 0;
	if (merged)
		apply_merge(&merge);
	merge_end(&merge);

	return (merged);
}

/*
 * Ready a merge: a record per target and per task, sized by the program and
 * the pool rather than the data, and so not charged, each append starting
 * empty.  Returns 0, or -1 when they could not be allocated or the round
 * holds more candidates than RELIC_TUPLE_MAX, which keeps every position
 * the merge counts within the 32 bits a table holds and is more than any
 * relation can take.
 */
static int
merge_begin(relic_merge_t *merge, relic_t *ctx, relic_pool_t *pool,
    relic_round_t *round)
{
	size_t candidates;
	size_t i;

	bzero(merge, sizeof(*merge));
	merge->ctx = ctx;
	merge->pool = pool;
	merge->round = round;
	relic_atomic_bool_init(&merge->refused, false);

	candidates = 0;
	for (i = 0; i < pool->task_count; i++)
		candidates += pool->tasks[i].candidate_count;
	if (candidates > RELIC_TUPLE_MAX)
		return (-1);

	merge->appends = calloc(round->target_count, sizeof(*merge->appends));
	merge->projected = calloc(round->target_count,
	    sizeof(*merge->projected));
	merge->task_firsts = calloc(pool->task_count,
	    RELIC_STRIPES * sizeof(*merge->task_firsts));
	merge->task_starts = calloc(pool->task_count,
	    sizeof(*merge->task_starts));
	if (merge->appends == NULL || merge->projected == NULL ||
	    merge->task_firsts == NULL || merge->task_starts == NULL)
		return (-1);

	return (0);
}

/*
 * Every step of the merge that can fail, in order.  None of them changes
 * what a relation holds.  Returns 0, or -1 at the first that fails;
 * merge_end() releases what the merge holds either way.
 */
static int
prepare_merge(relic_merge_t *merge, relic_t *ctx, relic_pool_t *pool,
    relic_round_t *round)
{
	if (merge_begin(merge, ctx, pool, round) != 0)
		return (-1);
	if (resolve_integers(merge) != 0)
		return (-1);
	if (find_new_tuples(merge) != 0)
		return (-1);
	if (count_new_tuples(merge) != 0)
		return (-1);
	if (stage_new_tuples(merge) != 0)
		return (-1);
	if (classify_new_keys(merge) != 0)
		return (-1);
	if (list_parts(merge, relic_relation_apply_parts, &merge->apply_parts,
	    &merge->apply_part_count) != 0)
		return (-1);

	return (reserve_room(merge));
}

/*
 * Promise every integer the candidates hold pending the identifier it will
 * take, and write it over each marker, hashing each candidate that held one
 * again, a worker's candidates to a part.  The integers are gathered in
 * task order, each task's candidates in the order its worker derived them
 * and each candidate's columns in order, which is the order the serial
 * evaluator meets them in, and the value table promises identifiers in
 * order of first occurrence: each integer is promised the identifier the
 * serial run gives it.  Nothing is interned until the merge can no longer
 * be declined.  Returns 0, or -1 when the round holds more pending columns
 * than a marker can count or their arrays could not be had.
 */
static int
resolve_integers(relic_merge_t *merge)
{
	size_t count;

	count = pending_columns(merge->pool);
	if (count == 0)
		return (0);
	if (count > RELIC_DOM_INDEX_MASK || pending_open(merge, count) != 0)
		return (-1);

	gather_pending(merge);
	assert(merge->pending_count == count);
	merge->table_projected = relic_pending_measure(merge->ctx,
	    merge->pending, merge->pending_count, NULL, &merge->pending_map);
	relic_pool_for(merge->pool, merge->pool->worker_count, rehash_worker,
	    merge);

	return (0);
}

/*
 * How many columns of every worker's candidates hold a pending marker.  A
 * worker that met no integer the table lacked holds none.
 */
static size_t
pending_columns(const relic_pool_t *pool)
{
	const relic_candidates_t *candidates;
	size_t count;
	size_t i;
	size_t w;

	count = 0;
	for (w = 0; w < pool->worker_count; w++) {
		candidates = &pool->workers[w].candidates;
		if (candidates->integer_count == 0)
			continue;

		for (i = 0; i < candidates->word_count; i++) {
			if ((candidates->words[i] & RELIC_DOM_TYPE_MASK) == 0)
				count++;
		}
	}

	return (count);
}

/*
 * Make the arrays of `count` pending integers and their map, charged to the
 * round.  Returns 0, or -1 having charged nothing, when their bytes are more
 * than a size can count or the limit or the allocator refused them.
 */
static int
pending_open(relic_merge_t *merge, size_t count)
{
	relic_account_t *account;
	size_t bytes;

	account = &merge->ctx->account;
	bytes = relic_saturating_sum(relic_saturating_product(count,
	    sizeof(*merge->pending)), relic_pending_map_bytes(count));
	if (bytes == SIZE_MAX || !relic_account_charge_round(account, bytes))
		return (-1);

	merge->pending = calloc(count, sizeof(*merge->pending));
	if (merge->pending == NULL ||
	    relic_pending_map_open(&merge->pending_map, count) != 0) {
		free(merge->pending);
		merge->pending = NULL;
		relic_account_release_round(account, bytes);
		return (-1);
	}
	merge->pending_charged = bytes;

	return (0);
}

/*
 * Gather the candidates' pending integers into the round's, in task order,
 * rewriting each marker, which names an integer among its worker's, to
 * name it among the round's.
 */
static void
gather_pending(relic_merge_t *merge)
{
	const relic_round_task_t *task;
	relic_candidates_t *candidates;
	relic_candidate_t *candidate;
	relic_dom_t *tuple;
	uint16_t arity;
	uint16_t column;
	size_t end;
	size_t t;
	size_t i;

	for (t = 0; t < merge->pool->task_count; t++) {
		task = &merge->pool->tasks[t];
		candidates = &merge->pool->workers[task->worker].candidates;
		if (candidates->integer_count == 0)
			continue;

		end = task->first_candidate + task->candidate_count;
		for (i = task->first_candidate; i < end; i++) {
			candidate = &candidates->entries[i];
			tuple = &candidates->words[candidate->offset];
			arity = relic_relation_arity(
			    merge->round->targets[candidate->target]);
			for (column = 0; column < arity; column++) {
				if ((tuple[column] & RELIC_DOM_TYPE_MASK) != 0)
					continue;

				assert(tuple[column] <=
				    candidates->integer_count);
				merge->pending[merge->pending_count].kind =
				    RELIC_PENDING_INTEGER;
				merge->pending[merge->pending_count].u.integer =
				    candidates->integers[tuple[column] - 1];
				merge->pending_count++;
				tuple[column] =
				    (relic_dom_t)merge->pending_count;
			}
		}
	}
}

/*
 * Write the identifier promised over every marker among worker `worker`'s
 * candidates, and hash each candidate that held one again, moving it to
 * its stripe.  Only the worker's own candidates are touched.
 */
static void
rehash_worker(void *argument, size_t worker, size_t worker_index RELIC_UNUSED)
{
	const relic_relation_t *relation;
	relic_candidates_t *candidates;
	relic_candidate_t *candidate;
	relic_merge_t *merge;
	relic_dom_t *tuple;
	uint16_t arity;
	size_t stripe;
	size_t i;

	merge = argument;
	candidates = &merge->pool->workers[worker].candidates;
	if (candidates->integer_count == 0)
		return;

	for (i = 0; i < candidates->count; i++) {
		candidate = &candidates->entries[i];
		relation = merge->round->targets[candidate->target];
		arity = relic_relation_arity(relation);
		tuple = &candidates->words[candidate->offset];
		if (!holds_marker(tuple, arity))
			continue;

		relic_pending_rewrite(&merge->pending_map, tuple, 1, arity);
		candidate->hash = relic_relation_hash(relation, tuple);

		stripe = relic_hash_stripe(candidate->hash);
		candidates->by_stripe[candidates->stripes[i]]--;
		candidates->by_stripe[stripe]++;
		candidates->stripes[i] = (uint8_t)stripe;
	}
}

/*
 * Whether a tuple holds a pending marker in some column.
 */
static bool
holds_marker(const relic_dom_t *tuple, uint16_t arity)
{
	uint16_t column;

	for (column = 0; column < arity; column++) {
		if ((tuple[column] & RELIC_DOM_TYPE_MASK) == 0)
			return (true);
	}

	return (false);
}

/*
 * Mark every candidate a first or a repeat, a stripe of the canonical
 * index to a part.  Returns 0, or -1 when a stripe's table of firsts could
 * not be had.
 */
static int
find_new_tuples(relic_merge_t *merge)
{
	relic_pool_for(merge->pool, RELIC_STRIPES, find_new_in_stripe, merge);

	return (relic_atomic_bool_load_relaxed(&merge->refused) ? -1 : 0);
}

/*
 * Walk the candidates of stripe `stripe` in task order, each task's in the
 * order its worker derived them, and mark each one the same tuple for the
 * same target as an earlier one a repeat, and every other a first.  A
 * candidate of one worker is never a repeat of that worker's own earlier
 * one, which its screen dropped; a repeat is a tuple another worker
 * gathered in an earlier task, which the serial evaluator would have
 * inserted there and found present here.
 *
 * Only the stripe's candidates are touched, and only this part walks them.
 */
static void
find_new_in_stripe(void *argument, size_t stripe,
    size_t worker_index RELIC_UNUSED)
{
	relic_merge_firsts_t firsts;
	const relic_round_task_t *task;
	relic_candidates_t *candidates;
	relic_merge_t *merge;
	relic_merge_ref_t ref;
	size_t task_firsts;
	size_t charged;
	size_t end;
	size_t t;
	size_t i;

	merge = argument;
	if (firsts_open(&merge->ctx->account, &firsts,
	    stripe_candidates(merge->pool, stripe), &charged) != 0) {
		relic_atomic_bool_store_relaxed(&merge->refused, true);
		return;
	}

	for (t = 0; t < merge->pool->task_count; t++) {
		task = &merge->pool->tasks[t];
		candidates = &merge->pool->workers[task->worker].candidates;
		ref.worker = (uint32_t)task->worker;

		task_firsts = 0;
		end = task->first_candidate + task->candidate_count;
		for (i = task->first_candidate; i < end; i++) {
			if (candidates->stripes[i] != stripe)
				continue;

			ref.position = (uint32_t)i;
			if (firsts_hold(merge, &firsts, ref)) {
				candidates->entries[i].repeat = true;
			} else {
				firsts_add(merge, &firsts, ref);
				task_firsts++;
			}
		}
		merge->task_firsts[t * RELIC_STRIPES + stripe] = task_firsts;
	}

	firsts_close(&merge->ctx->account, &firsts, charged);
}

/*
 * How many candidates the round's workers gathered in stripe `stripe`.
 */
static size_t
stripe_candidates(const relic_pool_t *pool, size_t stripe)
{
	size_t count;
	size_t i;

	count = 0;
	for (i = 0; i < pool->worker_count; i++)
		count += pool->workers[i].candidates.by_stripe[stripe];

	return (count);
}

/*
 * The candidate `ref` names, and its tuple.
 */
static const relic_candidate_t *
entry_of(const relic_merge_t *merge, relic_merge_ref_t ref)
{
	return (&merge->pool->workers[ref.worker].candidates.entries[
	    ref.position]);
}

static const relic_dom_t *
tuple_of(const relic_merge_t *merge, relic_merge_ref_t ref)
{
	const relic_candidates_t *candidates;

	candidates = &merge->pool->workers[ref.worker].candidates;

	return (&candidates->words[candidates->entries[ref.position].offset]);
}

/*
 * Whether a first met already is the same tuple for the same target as the
 * candidate `ref` names.
 */
static bool
firsts_hold(const relic_merge_t *merge, const relic_merge_firsts_t *firsts,
    relic_merge_ref_t ref)
{
	const relic_candidate_t *candidate;
	const relic_candidate_t *first;
	relic_idprobe_t probe;
	uint16_t arity;
	uint32_t met_plus_one;

	candidate = entry_of(merge, ref);
	arity = relic_relation_arity(merge->round->targets[candidate->target]);

	relic_idprobe_open(&probe, &firsts->table,
	    relic_candidate_screen_hash(candidate->target, candidate->hash));
	while (relic_idprobe_next(&probe, &met_plus_one)) {
		first = entry_of(merge, firsts->refs[met_plus_one - 1]);
		if (first->target == candidate->target &&
		    relic_tuple_equal(tuple_of(merge,
		    firsts->refs[met_plus_one - 1]), tuple_of(merge, ref),
		    arity))
			return (true);
	}

	return (false);
}

/*
 * Add the candidate `ref` names to the firsts met.  The table has room for
 * every candidate of its stripe.
 */
static void
firsts_add(const relic_merge_t *merge, relic_merge_firsts_t *firsts,
    relic_merge_ref_t ref)
{
	const relic_candidate_t *candidate;

	candidate = entry_of(merge, ref);
	firsts->refs[firsts->count] = ref;
	firsts->count++;
	relic_idtab_place(&firsts->table,
	    relic_candidate_screen_hash(candidate->target, candidate->hash),
	    (uint32_t)firsts->count);
}

/*
 * Give each task the positions its firsts take in its target's append, in
 * task order, and each append its count, in all and by stripe.  Returns 0,
 * or -1 when the new tuples would pass the tuple limit or a relation's
 * identifier space, which the serial run then reports at the tuple it
 * refuses.
 */
static int
count_new_tuples(relic_merge_t *merge)
{
	const relic_round_task_t *task;
	relic_append_t *append;
	size_t firsts;
	size_t target;
	size_t total;
	size_t t;
	size_t s;

	for (t = 0; t < merge->pool->task_count; t++) {
		task = &merge->pool->tasks[t];
		target = merge->round->items[task->item].target;
		append = &merge->appends[target];
		merge->task_starts[t] = append->count;

		for (s = 0; s < RELIC_STRIPES; s++) {
			firsts = merge->task_firsts[t * RELIC_STRIPES + s];
			append->growth.by_stripe[s] += firsts;
			append->count += firsts;
		}
	}

	total = 0;
	for (t = 0; t < merge->round->target_count; t++) {
		append = &merge->appends[t];
		if (append->count > RELIC_TUPLE_MAX -
		    relic_relation_count(merge->round->targets[t]))
			return (-1);
		total += append->count;
	}

	if (total > relic_tuples_admissible(merge->ctx)) {
		merge->pool->limit_declines++;
		return (-1);
	}

	return (0);
}

/*
 * Stage every new tuple in its target's append, a task to a part.  Returns
 * 0, or -1 when an append's arrays could not be had.
 */
static int
stage_new_tuples(relic_merge_t *merge)
{
	size_t t;

	for (t = 0; t < merge->round->target_count; t++) {
		if (relic_relation_append_open(merge->ctx,
		    merge->round->targets[t], &merge->appends[t]) != 0)
			return (-1);
	}

	relic_pool_for(merge->pool, merge->pool->task_count, stage_task,
	    merge);

	return (0);
}

/*
 * Stage task `task`'s firsts, in the order its worker derived them, at the
 * positions count_new_tuples() gave it in its target's append.  No other
 * task's firsts take those positions.
 */
static void
stage_task(void *argument, size_t task, size_t worker_index RELIC_UNUSED)
{
	const relic_round_task_t *record;
	const relic_candidates_t *candidates;
	const relic_candidate_t *candidate;
	const relic_relation_t *relation;
	relic_merge_t *merge;
	size_t target;
	size_t position;
	size_t end;
	size_t i;

	merge = argument;
	record = &merge->pool->tasks[task];
	target = merge->round->items[record->item].target;
	relation = merge->round->targets[target];
	candidates = &merge->pool->workers[record->worker].candidates;

	position = merge->task_starts[task];
	end = record->first_candidate + record->candidate_count;
	for (i = record->first_candidate; i < end; i++) {
		candidate = &candidates->entries[i];
		if (candidate->repeat)
			continue;

		relic_relation_stage(relation, &merge->appends[target],
		    position, &candidates->words[candidate->offset],
		    candidate->hash);
		position++;
	}
}

/*
 * Classify every key the new tuples bring, in every part the relation
 * store names for each target's append.  Returns 0, or -1 when the
 * list of parts or a part's table of new keys could not be had.
 */
static int
classify_new_keys(relic_merge_t *merge)
{
	if (list_parts(merge, relic_relation_classify_parts,
	    &merge->classify_parts, &merge->classify_part_count) != 0)
		return (-1);

	relic_pool_for(merge->pool, merge->classify_part_count, classify_part,
	    merge);

	return (relic_atomic_bool_load_relaxed(&merge->refused) ? -1 : 0);
}

/*
 * List, in target order, every part `parts_of` gives each target's append.
 * Returns 0, or -1 when the list could not be allocated.
 */
static int
list_parts(const relic_merge_t *merge,
    size_t (*parts_of)(const relic_append_t *), relic_merge_part_t **partsp,
    size_t *countp)
{
	relic_merge_part_t *parts;
	size_t target_parts;
	size_t count;
	size_t t;
	size_t p;

	count = 0;
	for (t = 0; t < merge->round->target_count; t++)
		count += parts_of(&merge->appends[t]);
	*countp = 0;
	if (count == 0)
		return (0);

	parts = calloc(count, sizeof(*parts));
	if (parts == NULL)
		return (-1);
	*partsp = parts;

	for (t = 0; t < merge->round->target_count; t++) {
		target_parts = parts_of(&merge->appends[t]);
		for (p = 0; p < target_parts; p++) {
			parts[*countp].target = t;
			parts[*countp].part = p;
			(*countp)++;
		}
	}

	return (0);
}

/*
 * Classify the keys of one part of a target's append, with a table of the
 * part's new keys that has room for every tuple the part reads.
 */
static void
classify_part(void *argument, size_t part, size_t worker_index RELIC_UNUSED)
{
	const relic_merge_part_t *listed;
	relic_new_keys_t new_keys;
	relic_append_t *append;
	relic_merge_t *merge;
	size_t charged;

	merge = argument;
	listed = &merge->classify_parts[part];
	append = &merge->appends[listed->target];

	if (new_keys_open(&merge->ctx->account, &new_keys,
	    relic_relation_classify_tuples(append, listed->part),
	    &charged) != 0) {
		relic_atomic_bool_store_relaxed(&merge->refused, true);
		return;
	}

	relic_relation_classify_append(merge->round->targets[listed->target],
	    append, listed->part, &new_keys);

	new_keys_close(&merge->ctx->account, &new_keys, charged);
}

/*
 * Ask the memory limit of what every target holds while it takes its new
 * tuples -- exactly what it would hold once it had taken them, and beside
 * that the tables its growth replaces until they are freed -- and of what
 * the value table and the pool hold once they have taken the new values,
 * with everything the round holds still charged, and make that room.
 * Returns 0, or -1 when the limit would refuse, a growth could not happen,
 * or the allocator refused one.  What a refused reservation obtained stays
 * counted, the prepared tables until merge_end() releases them.
 */
static int
reserve_room(relic_merge_t *merge)
{
	const relic_relation_t *relation;
	size_t table_now;
	size_t table_then;
	size_t during;
	size_t growth;
	size_t t;

	table_now = relic_value_storage_bytes(merge->ctx);
	table_then = merge->pending_count > 0 ? merge->table_projected :
	    table_now;

	growth = 0;
	for (t = 0; t < merge->round->target_count; t++) {
		relation = merge->round->targets[t];
		merge->projected[t] = relic_relation_bytes_after_append(
		    relation, &merge->appends[t]);
		during = relic_relation_bytes_during_append(relation,
		    &merge->appends[t]);
		if (merge->projected[t] == SIZE_MAX || during == SIZE_MAX)
			return (-1);
		growth = relic_saturating_sum(growth,
		    during - relic_relation_bytes(relation));
	}
	if (relic_account_exceeds(&merge->ctx->account, table_now,
	    relic_saturating_sum(growth, table_then))) {
		merge->pool->limit_declines++;
		return (-1);
	}

	/*
	 * The integer table is reserved first: it can also refuse for having
	 * no identifiers left, which the serial evaluator meets before it
	 * inserts the batch, and so before any relation has grown.
	 */
	if (merge->pending_count > 0 &&
	    relic_pending_reserve(merge->ctx, &merge->pending_map) != 0)
		return (-1);
	for (t = 0; t < merge->round->target_count; t++) {
		if (relic_relation_reserve_append(merge->ctx,
		    merge->round->targets[t], &merge->appends[t]) != 0)
			return (-1);
	}

	/*
	 * The storage total now counts the tables the growth replaces beside
	 * their replacements, as the admission did, so it stays within the
	 * limit until the application frees them.
	 */
	assert(!relic_account_exceeds(&merge->ctx->account, 0, 0));

	return (0);
}

/*
 * Give the integer table the new integers, under the identifiers the
 * candidates already hold, and every target its new tuples: their columns,
 * and then, a stripe to a part, their canonical entries and their links in
 * every secondary index.
 * Nothing here can fail.  Every item one of whose tasks inserted a new
 * tuple has gained, and each new tuple counts as inserted.
 */
static void
apply_merge(relic_merge_t *merge)
{
	relic_relation_t *relation;
	size_t t;

	if (merge->pending_count > 0) {
		relic_pending_append(merge->ctx, &merge->pending_map);
		assert(relic_value_storage_bytes(merge->ctx) ==
		    merge->table_projected);
	}

	for (t = 0; t < merge->round->target_count; t++)
		relic_relation_take_append(merge->ctx,
		    merge->round->targets[t], &merge->appends[t]);

	relic_pool_for(merge->pool, merge->apply_part_count, apply_part,
	    merge);

	for (t = 0; t < merge->round->target_count; t++) {
		relation = merge->round->targets[t];
		relic_relation_finish_append(merge->ctx, relation,
		    &merge->appends[t]);
		assert(relic_relation_bytes(relation) == merge->projected[t]);
		merge->ctx->counters.tuples_inserted += merge->appends[t].count;
	}

	for (t = 0; t < merge->pool->task_count; t++) {
		if (task_first_count(merge, t) != 0)
			merge->round->items[merge->pool->tasks[t].item].gained =
			    true;
	}
}

/*
 * How many new tuples task `task` found.
 */
static size_t
task_first_count(const relic_merge_t *merge, size_t task)
{
	size_t count;
	size_t s;

	count = 0;
	for (s = 0; s < RELIC_STRIPES; s++)
		count += merge->task_firsts[task * RELIC_STRIPES + s];

	return (count);
}

/*
 * Apply one part of a target's append.
 */
static void
apply_part(void *argument, size_t part, size_t worker_index RELIC_UNUSED)
{
	const relic_merge_part_t *listed;
	relic_merge_t *merge;

	merge = argument;
	listed = &merge->apply_parts[part];
	relic_relation_apply_append(merge->round->targets[listed->target],
	    &merge->appends[listed->target], listed->part);
}

/*
 * Release everything the merge holds, whether it applied or declined: the
 * appends, with their charge and whatever growth they prepared and did not
 * fill, the pending integers and their map, with their charge, and the
 * records.
 */
static void
merge_end(relic_merge_t *merge)
{
	size_t t;

	if (merge->appends != NULL) {
		for (t = 0; t < merge->round->target_count; t++)
			relic_relation_append_close(merge->ctx,
			    &merge->appends[t]);
	}

	if (merge->pending != NULL) {
		free(merge->pending);
		relic_pending_map_close(&merge->pending_map);
		relic_account_release_round(&merge->ctx->account,
		    merge->pending_charged);
	}

	free(merge->appends);
	free(merge->projected);
	free(merge->task_firsts);
	free(merge->task_starts);
	free(merge->classify_parts);
	free(merge->apply_parts);
}

/*
 * An array of `count` items of `size` bytes, charged to the round before it
 * is allocated, the charge added to *chargedp; NULL, having charged
 * nothing, when the limit or the allocator refused it.
 */
static void *
charged_array(relic_account_t *account, size_t count, size_t size,
    size_t *chargedp)
{
	size_t capacity;
	size_t bytes;
	void *items;

	capacity = relic_grow_array_capacity(0, count);
	if (capacity == 0 || capacity > SIZE_MAX / size)
		return (NULL);
	bytes = capacity * size;
	if (!relic_account_charge_round(account, bytes))
		return (NULL);

	capacity = 0;
	items = relic_grow_array(NULL, &capacity, count, size);
	if (items == NULL) {
		relic_account_release_round(account, bytes);
		return (NULL);
	}
	*chargedp += bytes;

	return (items);
}

/*
 * Make an empty tagged table with room for `count` entries, charged to the
 * round, the charge added to *chargedp.  Returns 0, or -1 having charged
 * nothing.  Nothing is placed in it but what it has room for, so it never
 * grows and needs no rehash.
 */
static int
scratch_table_open(relic_account_t *account, relic_idtab_t *table,
    size_t count, size_t *chargedp)
{
	relic_idtab_t empty;
	size_t bytes;

	relic_idtab_init(&empty, true);
	bytes = relic_idtab_bytes_after_reserve(&empty, count);
	if (!relic_account_charge_round(account, bytes))
		return (-1);
	if (relic_idtab_prepare_growth(&empty, count, table) != 0) {
		relic_account_release_round(account, bytes);
		return (-1);
	}
	*chargedp += bytes;

	return (0);
}

/*
 * Make an empty table of firsts with room for `count` of them, charged to
 * the round, the charge in *chargedp.  Returns 0, or -1 having charged
 * nothing.
 */
static int
firsts_open(relic_account_t *account, relic_merge_firsts_t *firsts,
    size_t count, size_t *chargedp)
{
	bzero(firsts, sizeof(*firsts));
	relic_idtab_init(&firsts->table, true);
	*chargedp = 0;
	if (count == 0)
		return (0);

	if (scratch_table_open(account, &firsts->table, count, chargedp) != 0)
		return (-1);
	firsts->refs = charged_array(account, count, sizeof(*firsts->refs),
	    chargedp);
	if (firsts->refs == NULL) {
		firsts_close(account, firsts, *chargedp);
		return (-1);
	}

	return (0);
}

static void
firsts_close(relic_account_t *account, relic_merge_firsts_t *firsts,
    size_t charged)
{
	relic_idtab_reset(&firsts->table);
	relic_free_array(firsts->refs);
	relic_account_release_round(account, charged);
}

/*
 * The same for a part's new keys, with room for `count` of them.
 */
static int
new_keys_open(relic_account_t *account, relic_new_keys_t *new_keys,
    size_t count, size_t *chargedp)
{
	bzero(new_keys, sizeof(*new_keys));
	relic_idtab_init(&new_keys->table, true);
	*chargedp = 0;
	if (count == 0)
		return (0);

	if (scratch_table_open(account, &new_keys->table, count,
	    chargedp) != 0)
		return (-1);
	new_keys->first = charged_array(account, count,
	    sizeof(*new_keys->first), chargedp);
	if (new_keys->first == NULL) {
		new_keys_close(account, new_keys, *chargedp);
		return (-1);
	}

	return (0);
}

static void
new_keys_close(relic_account_t *account, relic_new_keys_t *new_keys,
    size_t charged)
{
	relic_idtab_reset(&new_keys->table);
	relic_free_array(new_keys->first);
	relic_account_release_round(account, charged);
}
