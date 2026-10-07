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
 *	resolve		(worker 0) promise each value the candidates hold
 *			pending the identifier the serial run gives it, and
 *			write it over each marker, hashing the candidate
 *			again;
 *	find new tuples	a worker to a part, list its candidates by stripe;
 *			then a stripe of the canonical index to a part, mark
 *			each candidate of the stripe a first or a repeat of
 *			an earlier one, counting each task's firsts;
 *	count		(worker 0) each task's firsts give it the positions
 *			its new tuples take in its target's append, and each
 *			target its count per stripe; the tuple limit and the
 *			identifier space are asked of the exact total;
 *	stage		a task to a part: copy its firsts, with their hashes
 *			and their keys' hashes, into their positions; then an
 *			index of a target to a part, list the staged tuples
 *			by their stripe of the index;
 *	classify	a stripe of a secondary index to a part: find the key
 *			each staged tuple of the stripe joins, numbering the
 *			stripe's new keys;
 *	reserve		(worker 0) ask the memory limit of exactly what the
 *			serial insertions would hold, with the tables their
 *			growth replaces held beside it, and grow every array
 *			to it, preparing each table's growth without filling
 *			it; grow the value table and the pool for the new
 *			values;
 *	apply		(worker 0) append the new values and store the
 *			columns; then a stripe to a part, fill each grown
 *			table, place the canonical entries and link the
 *			chains.
 *
 * No step up to the application changes what a relation or the integer
 * table holds, and every step that can fail -- scratch the limit or the
 * allocator refuses, a limit the new tuples or integers would pass, a
 * growth refused -- is among them.  The scratch of a step is sized before
 * the step begins, from counts the steps before it found, and taken as one
 * block charged once, which the step's parts carve between them: no part
 * allocates, and no part can fail.  A failure declines the merge, and the
 * round runs again serially, where whatever refuses it reports itself; the
 * merge itself never reports and never fails part way.  Every part of a
 * step writes only what its stripe, its task or its worker owns, and the
 * pool's barrier between steps publishes what each step wrote to the
 * next.  Which parts a relation's append is listed, classified and applied
 * in is the relation store's to say, and the merge runs every part it
 * names.
 */

typedef struct relic_merge relic_merge_t;
typedef struct relic_merge_block relic_merge_block_t;
typedef struct relic_merge_firsts relic_merge_firsts_t;
typedef struct relic_merge_firsts_place relic_merge_firsts_place_t;
typedef struct relic_merge_keys_place relic_merge_keys_place_t;
typedef struct relic_merge_list relic_merge_list_t;
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
 * The firsts of one stripe met so far, as the stripe's part finds them:
 * `table` maps a candidate's screen hash to its position in `refs` plus
 * one, and `count` firsts are met.  The part makes it on its own stack
 * from the stripe's place in the step's block.
 */
struct relic_merge_firsts {
	relic_idtab_t		 table;
	relic_merge_ref_t	*refs;
	size_t			 count;
};

/*
 * Where one stripe's firsts lie in the block of the step that finds them:
 * room for all `room` candidates of the stripe, a table's slots at `slots`
 * and the references at `refs`, so that finding the firsts never
 * allocates.  The stripe's part makes its table there when it begins, so
 * that the parts clear their tables' memory in parallel.
 */
struct relic_merge_firsts_place {
	void			*slots;
	relic_merge_ref_t	*refs;
	size_t			 room;
};

/*
 * Where the keys new to one classify part's stripe lie in the block of the
 * step that classifies them: room for `room` keys, a table's slots at
 * `slots` and the first tuple of each key at `first`.  The part makes its
 * table there when it begins, as a stripe's firsts are made.
 */
struct relic_merge_keys_place {
	void		*slots;
	uint32_t	*first;
	size_t		 room;
};

/*
 * A worker's candidates listed by their stripe of the canonical index, as
 * it stands once the pending values are resolved: those of stripe s are
 * `list[start[s]]` up to `list[start[s + 1]]`, in increasing position
 * (relic_list_by_stripe()).
 */
struct relic_merge_list {
	uint32_t	*list;
	size_t		 start[RELIC_STRIPES + 1];
};

/*
 * A step's scratch: one allocation of `bytes` bytes at `memory`, charged
 * to the round, which the step's parts carve between them.  An empty block
 * holds nothing and is charged nothing.
 */
struct relic_merge_block {
	void	*memory;
	size_t	 bytes;
};

/*
 * A part of the work on one target's append, as the relation store numbers
 * the parts of a listing, a classification or an application.
 */
struct relic_merge_part {
	size_t	target;
	size_t	part;
};

/*
 * A merge under way.  `appends` holds each target's new tuples, and
 * `projected` the bytes each target will hold once it has taken them.
 * `pending` holds the round's `pending_count` pending values in task
 * order, the strings' bytes in `pending_bytes`, where each worker's lie
 * together, `pending_map` the identifier each is promised, and
 * `table_projected` the bytes the value table and the pool will hold once
 * they have taken the new ones; `pending_charged` is what the three were
 * charged, and `pending_reserved` says the value table and the pool hold
 * a reservation for them not yet appended.  `texts_bytes` is at least
 * what the serial run's text buffers would hold by the round's end, which
 * the admission counts.
 * `worker_lists` lists each worker's candidates by stripe, and
 * `firsts_places` says where each stripe's firsts lie, in `find_block`,
 * which holds both while the firsts are found.
 * `task_firsts[t * RELIC_STRIPES + s]` counts task t's firsts in stripe s,
 * and `task_starts[t]` is the position task t's first new tuple takes in
 * its target's append.  `listing_parts`, `classify_parts` and
 * `apply_parts` number the parts of every target's append.  `keys_places`
 * says where each classify part's new keys lie, in `classify_block`, which
 * holds them while the keys are classified.
 */
struct relic_merge {
	relic_t				*ctx;
	relic_pool_t			*pool;
	relic_round_t			*round;
	relic_append_t			*appends;
	size_t				*projected;
	relic_merge_list_t		*worker_lists;
	relic_merge_firsts_place_t	 firsts_places[RELIC_STRIPES];
	relic_merge_block_t		 find_block;
	size_t				*task_firsts;
	size_t				*task_starts;
	relic_merge_part_t		*listing_parts;
	size_t				 listing_part_count;
	relic_merge_part_t		*classify_parts;
	size_t				 classify_part_count;
	relic_merge_keys_place_t	*keys_places;
	relic_merge_block_t		 classify_block;
	relic_merge_part_t		*apply_parts;
	size_t				 apply_part_count;
	relic_pending_t			*pending;
	size_t				 pending_count;
	char				*pending_bytes;
	relic_pending_map_t		 pending_map;
	size_t				 pending_charged;
	size_t				 table_projected;
	size_t				 texts_bytes;
	bool				 pending_reserved;
};

static void	 apply_merge(relic_merge_t *merge);
static void	 apply_part(void *argument, size_t part, size_t worker_index);
static void	 block_close(relic_merge_t *merge,
		    relic_merge_block_t *block);
static int	 block_open(relic_merge_t *merge, relic_merge_block_t *block,
		    size_t (*layout)(relic_merge_t *, void *));
static size_t	 classify_layout(relic_merge_t *merge, void *memory);
static int	 classify_new_keys(relic_merge_t *merge);
static void	 classify_part(void *argument, size_t part,
		    size_t worker_index);
static int	 count_new_tuples(relic_merge_t *merge);
static const relic_candidate_t *entry_of(const relic_merge_t *merge,
		    relic_merge_ref_t ref);
static size_t	 find_layout(relic_merge_t *merge, void *memory);
static int	 find_new_tuples(relic_merge_t *merge);
static void	 find_new_in_stripe(void *argument, size_t stripe,
		    size_t worker_index);
static bool	 firsts_hold(const relic_merge_t *merge,
		    const relic_merge_firsts_t *firsts, relic_merge_ref_t ref);
static void	 firsts_add(const relic_merge_t *merge,
		    relic_merge_firsts_t *firsts, relic_merge_ref_t ref);
static void	 gather_pending(relic_merge_t *merge);
static bool	 holds_marker(const relic_dom_t *tuple, uint16_t arity);
static size_t	 list_search(const uint32_t *list, size_t first, size_t end,
		    size_t position);
static void	 list_staged_part(void *argument, size_t part,
		    size_t worker_index);
static void	 list_worker(void *argument, size_t worker,
		    size_t worker_index);
static int	 merge_begin(relic_merge_t *merge, relic_t *ctx,
		    relic_pool_t *pool, relic_round_t *round);
static void	 merge_end(relic_merge_t *merge);
static int	 number_parts(const relic_merge_t *merge,
		    size_t (*parts_of)(const relic_append_t *),
		    relic_merge_part_t **partsp, size_t *countp);
static size_t	 pending_columns(const relic_pool_t *pool);
static int	 pending_open(relic_merge_t *merge, size_t count,
		    size_t bytes);
static size_t	 pending_string_bytes(const relic_pool_t *pool);
static int	 prepare_merge(relic_merge_t *merge, relic_t *ctx,
		    relic_pool_t *pool, relic_round_t *round,
		    size_t texts_bytes);
static void	 rehash_worker(void *argument, size_t worker,
		    size_t worker_index);
static int	 reserve_room(relic_merge_t *merge);
static int	 resolve_pending(relic_merge_t *merge);
static int	 stage_new_tuples(relic_merge_t *merge);
static void	 stage_task(void *argument, size_t task, size_t worker_index);
static size_t	 stripe_candidates(const relic_pool_t *pool, size_t stripe);
static const relic_dom_t *tuple_of(const relic_merge_t *merge,
		    relic_merge_ref_t ref);

bool
relic_round_merge(relic_t *ctx, relic_pool_t *pool, relic_round_t *round,
    size_t texts_bytes)
{
	relic_merge_t merge;
	bool merged;

	merged = prepare_merge(&merge, ctx, pool, round, texts_bytes) == 0;
	if (merged)
		apply_merge(&merge);
	merge_end(&merge);

	return (merged);
}

/*
 * Ready a merge: a record per target, per task and per worker, sized by the
 * program and the pool rather than the data, and so not charged, each
 * append starting empty.  Returns 0, or -1 when they could not be
 * allocated or the round holds more candidates than RELIC_TUPLE_MAX, which
 * keeps every position the merge counts within the 32 bits a table holds
 * and is more than any relation can take.
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
	merge->worker_lists = calloc(pool->worker_count,
	    sizeof(*merge->worker_lists));
	if (merge->appends == NULL || merge->projected == NULL ||
	    merge->task_firsts == NULL || merge->task_starts == NULL ||
	    merge->worker_lists == NULL)
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
    relic_round_t *round, size_t texts_bytes)
{
	if (merge_begin(merge, ctx, pool, round) != 0)
		return (-1);
	merge->texts_bytes = texts_bytes;
	if (resolve_pending(merge) != 0)
		return (-1);
	if (find_new_tuples(merge) != 0)
		return (-1);
	if (count_new_tuples(merge) != 0)
		return (-1);
	if (stage_new_tuples(merge) != 0)
		return (-1);
	if (classify_new_keys(merge) != 0)
		return (-1);
	if (number_parts(merge, relic_relation_apply_parts, &merge->apply_parts,
	    &merge->apply_part_count) != 0)
		return (-1);

	return (reserve_room(merge));
}

/*
 * Promise every value the candidates hold pending the identifier it will
 * take, and write it over each marker, hashing each candidate that held one
 * again, a worker's candidates to a part.  The values are gathered in task
 * order, each task's candidates in the order its worker derived them and
 * each candidate's columns in order, which is the order the serial
 * evaluator meets them in, and the value table and the pool promise
 * identifiers in order of first occurrence: each value is promised the
 * identifier the serial run gives it.  Nothing is interned until the merge
 * can no longer be declined.  Returns 0, or -1 when the round holds more
 * pending columns than a marker can count or their arrays could not be had.
 */
static int
resolve_pending(relic_merge_t *merge)
{
	size_t count;

	count = pending_columns(merge->pool);
	if (count == 0)
		return (0);
	if (count > RELIC_DOM_INDEX_MASK ||
	    pending_open(merge, count, pending_string_bytes(merge->pool)) != 0)
		return (-1);

	gather_pending(merge);
	assert(merge->pending_count == count);
	merge->table_projected = relic_pending_measure(merge->ctx,
	    merge->pending, merge->pending_count, merge->pending_bytes,
	    &merge->pending_map);
	relic_pool_for(merge->pool, merge->pool->worker_count, rehash_worker,
	    merge);

	return (0);
}

/*
 * How many columns of every worker's candidates hold a pending marker.  A
 * worker that met no value the context lacked holds none.
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
		if (candidates->value_count == 0)
			continue;

		for (i = 0; i < candidates->word_count; i++) {
			if ((candidates->words[i] & RELIC_DOM_TYPE_MASK) == 0)
				count++;
		}
	}

	return (count);
}

/*
 * The bytes of every worker's pending strings, each distinct string of a
 * worker counted once.
 */
static size_t
pending_string_bytes(const relic_pool_t *pool)
{
	size_t bytes;
	size_t w;

	bytes = 0;
	for (w = 0; w < pool->worker_count; w++)
		bytes += pool->workers[w].candidates.value_byte_count;

	return (bytes);
}

/*
 * Make the arrays of `count` pending values, the buffer for `bytes` bytes of
 * their strings, and their map, charged to the round.  Returns 0, or -1
 * having charged nothing, when their bytes are more than a size can count
 * or the limit or the allocator refused them.
 */
static int
pending_open(relic_merge_t *merge, size_t count, size_t bytes)
{
	relic_account_t *account;
	size_t charge;

	account = &merge->ctx->account;
	charge = relic_saturating_sum(relic_saturating_product(count,
	    sizeof(*merge->pending)), relic_pending_map_bytes(count));
	charge = relic_saturating_sum(charge, bytes);
	if (charge == SIZE_MAX || !relic_account_charge_round(account, charge))
		return (-1);

	merge->pending = calloc(count, sizeof(*merge->pending));
	merge->pending_bytes = malloc(bytes > 0 ? bytes : 1);
	if (merge->pending == NULL || merge->pending_bytes == NULL ||
	    relic_pending_map_open(&merge->pending_map, count) != 0) {
		free(merge->pending);
		free(merge->pending_bytes);
		merge->pending = NULL;
		merge->pending_bytes = NULL;
		relic_account_release_round(account, charge);
		return (-1);
	}
	merge->pending_charged = charge;

	return (0);
}

/*
 * Gather the candidates' pending values into the round's, in task order,
 * rewriting each marker, which names a value among its worker's, to name it
 * among the round's.  Each worker's string bytes are copied once, together,
 * and a string among the round's names its bytes where its worker's landed.
 */
static void
gather_pending(relic_merge_t *merge)
{
	const relic_candidates_t *holder;
	const relic_round_task_t *task;
	relic_candidates_t *candidates;
	relic_candidate_t *candidate;
	size_t base[RELIC_THREADS_MAX];
	relic_pending_t *value;
	relic_dom_t *tuple;
	uint16_t arity;
	uint16_t column;
	size_t copied;
	size_t end;
	size_t t;
	size_t i;
	size_t w;

	assert(merge->pool->worker_count <= RELIC_THREADS_MAX);
	copied = 0;
	for (w = 0; w < merge->pool->worker_count; w++) {
		holder = &merge->pool->workers[w].candidates;
		base[w] = copied;
		if (holder->value_byte_count > 0)
			bcopy(holder->value_bytes, merge->pending_bytes + copied,
			    holder->value_byte_count);
		copied += holder->value_byte_count;
	}

	for (t = 0; t < merge->pool->task_count; t++) {
		task = &merge->pool->tasks[t];
		candidates = &merge->pool->workers[task->worker].candidates;
		if (candidates->value_count == 0)
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

				assert(tuple[column] <= candidates->value_count);
				value = &merge->pending[merge->pending_count];
				*value = candidates->values[tuple[column] - 1];
				if (value->kind == RELIC_PENDING_TEXT)
					value->u.text.offset += base[task->worker];
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
	if (candidates->value_count == 0)
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
 * index to a part, once every worker's candidates are listed by stripe, a
 * worker to a part.  The lists and the tables of firsts are the step's
 * block, released once the firsts are counted.  Returns 0, or -1 when the
 * block could not be had.
 */
static int
find_new_tuples(relic_merge_t *merge)
{
	if (block_open(merge, &merge->find_block, find_layout) != 0)
		return (-1);

	relic_pool_for(merge->pool, merge->pool->worker_count, list_worker,
	    merge);
	relic_pool_for(merge->pool, RELIC_STRIPES, find_new_in_stripe, merge);

	block_close(merge, &merge->find_block);

	return (0);
}

/*
 * Lay the block of the step that finds the firsts out from `memory`, or
 * only measure it when `memory` is NULL (relic_block_piece()), and return
 * its bytes, SIZE_MAX when they are more than a size counts: a list of
 * each worker's candidates, and for each stripe a table of firsts and
 * their references, with room for every candidate of the stripe.
 */
static size_t
find_layout(relic_merge_t *merge, void *memory)
{
	relic_merge_firsts_place_t *place;
	size_t bytes;
	size_t s;
	size_t w;
	char *cursor;

	cursor = memory;
	bytes = 0;

	for (w = 0; w < merge->pool->worker_count; w++)
		merge->worker_lists[w].list = relic_block_piece(&cursor, &bytes,
		    merge->pool->workers[w].candidates.count,
		    sizeof(*merge->worker_lists[w].list));

	for (s = 0; s < RELIC_STRIPES; s++) {
		place = &merge->firsts_places[s];
		place->room = stripe_candidates(merge->pool, s);
		place->slots = relic_block_piece(&cursor, &bytes,
		    relic_idtab_bytes_for(true, place->room), 1);
		place->refs = relic_block_piece(&cursor, &bytes, place->room,
		    sizeof(*place->refs));
	}

	return (bytes);
}

/*
 * List worker `worker`'s candidates by their stripe.  Only the worker's
 * own list is written.
 */
static void
list_worker(void *argument, size_t worker, size_t worker_index RELIC_UNUSED)
{
	const relic_candidates_t *candidates;
	relic_merge_list_t *worker_list;
	relic_merge_t *merge;
	size_t s;

	merge = argument;
	candidates = &merge->pool->workers[worker].candidates;
	worker_list = &merge->worker_lists[worker];
	relic_list_by_stripe(candidates->stripes, candidates->count,
	    worker_list->list, worker_list->start);

	for (s = 0; s < RELIC_STRIPES; s++)
		assert(worker_list->start[s + 1] - worker_list->start[s] ==
		    candidates->by_stripe[s]);
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
 * A task's candidates are a run of positions among its worker's, and each
 * worker's list of the stripe holds its positions in increasing order, so
 * the task's candidates of the stripe are the run of the list from the
 * first position at or past the task's first candidate up to the first
 * past its last: found by a search, they are the same run whatever order
 * the workers ran their tasks in, and the same candidates staging reads.
 *
 * The stripe's firsts, their table and their count, are the part's own,
 * made on its stack at the stripe's place.  Only the stripe's candidates
 * are touched, and only this part walks them.
 */
static void
find_new_in_stripe(void *argument, size_t stripe,
    size_t worker_index RELIC_UNUSED)
{
	const relic_merge_firsts_place_t *place;
	const relic_merge_list_t *worker_list;
	const relic_round_task_t *task;
	relic_candidates_t *candidates;
	relic_merge_firsts_t firsts;
	relic_merge_t *merge;
	relic_merge_ref_t ref;
	size_t task_firsts;
	size_t stripe_end;
	size_t task_end;
	size_t t;
	size_t i;

	merge = argument;
	place = &merge->firsts_places[stripe];
	relic_idtab_init_given(&firsts.table, true, place->room, place->slots);
	firsts.refs = place->refs;
	firsts.count = 0;

	for (t = 0; t < merge->pool->task_count; t++) {
		task = &merge->pool->tasks[t];
		worker_list = &merge->worker_lists[task->worker];
		candidates = &merge->pool->workers[task->worker].candidates;
		ref.worker = (uint32_t)task->worker;

		task_firsts = 0;
		stripe_end = worker_list->start[stripe + 1];
		task_end = task->first_candidate + task->candidate_count;
		i = list_search(worker_list->list, worker_list->start[stripe],
		    stripe_end, task->first_candidate);
		for (; i < stripe_end && worker_list->list[i] < task_end; i++) {
			ref.position = worker_list->list[i];
			if (firsts_hold(merge, &firsts, ref)) {
				candidates->entries[ref.position].repeat = true;
			} else {
				firsts_add(merge, &firsts, ref);
				task_firsts++;
			}
		}
		merge->task_firsts[t * RELIC_STRIPES + stripe] = task_firsts;
	}

	assert(firsts.count <= place->room);
}

/*
 * The first index in [first, end) of `list`, whose positions increase
 * there, that holds a position at or past `position`; `end` when none
 * does.
 */
static size_t
list_search(const uint32_t *list, size_t first, size_t end, size_t position)
{
	size_t middle;

	while (first < end) {
		middle = first + (end - first) / 2;
		if (list[middle] < position)
			first = middle + 1;
		else
			end = middle;
	}

	return (first);
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
 * task order, and its count of them, and each append its count, in all and
 * by stripe.  Returns 0,
 * or -1 when the new tuples would pass the tuple limit or a relation's
 * identifier space, which the serial run then reports at the tuple it
 * refuses.
 */
static int
count_new_tuples(relic_merge_t *merge)
{
	relic_round_task_t *task;
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

		task->inserted = 0;
		for (s = 0; s < RELIC_STRIPES; s++) {
			firsts = merge->task_firsts[t * RELIC_STRIPES + s];
			append->growth.by_stripe[s] += firsts;
			append->count += firsts;
			task->inserted += firsts;
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
 * Stage every new tuple in its target's append, a task to a part, and then
 * list each append's staged tuples by stripe, an index to a part.  Returns
 * 0, or -1 when an append's arrays or the list of parts could not be had.
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

	if (number_parts(merge, relic_relation_list_parts,
	    &merge->listing_parts, &merge->listing_part_count) != 0)
		return (-1);
	relic_pool_for(merge->pool, merge->listing_part_count, list_staged_part,
	    merge);

	return (0);
}

/*
 * List the staged tuples of one part of a target's append by stripe.
 */
static void
list_staged_part(void *argument, size_t part,
    size_t worker_index RELIC_UNUSED)
{
	const relic_merge_part_t *part_record;
	relic_merge_t *merge;

	merge = argument;
	part_record = &merge->listing_parts[part];
	relic_relation_list_append(&merge->appends[part_record->target],
	    part_record->part);
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

	assert(position == merge->task_starts[task] + record->inserted);
}

/*
 * Classify every key the new tuples bring, in every part the relation
 * store names for each target's append, each part with a table of the new
 * keys of its stripe, which lie in the step's block, released once every
 * key is classified.  Returns 0, or -1 when the list of parts, their
 * records or the block could not be had.
 */
static int
classify_new_keys(relic_merge_t *merge)
{
	if (number_parts(merge, relic_relation_classify_parts,
	    &merge->classify_parts, &merge->classify_part_count) != 0)
		return (-1);
	if (merge->classify_part_count == 0)
		return (0);

	merge->keys_places = calloc(merge->classify_part_count,
	    sizeof(*merge->keys_places));
	if (merge->keys_places == NULL)
		return (-1);
	if (block_open(merge, &merge->classify_block, classify_layout) != 0)
		return (-1);

	relic_pool_for(merge->pool, merge->classify_part_count, classify_part,
	    merge);

	block_close(merge, &merge->classify_block);

	return (0);
}

/*
 * Lay the block of the step that classifies the keys out from `memory`, or
 * only measure it when `memory` is NULL (relic_block_piece()), and return
 * its bytes, SIZE_MAX when they are more than a size counts: for each
 * classify part a table of new keys and the first tuple of each, with room
 * for every tuple the part reads, which no number of new keys exceeds.
 */
static size_t
classify_layout(relic_merge_t *merge, void *memory)
{
	const relic_merge_part_t *part_record;
	relic_merge_keys_place_t *place;
	size_t bytes;
	size_t p;
	char *cursor;

	cursor = memory;
	bytes = 0;

	for (p = 0; p < merge->classify_part_count; p++) {
		part_record = &merge->classify_parts[p];
		place = &merge->keys_places[p];
		place->room = relic_relation_classify_tuples(
		    &merge->appends[part_record->target], part_record->part);
		place->slots = relic_block_piece(&cursor, &bytes,
		    relic_idtab_bytes_for(true, place->room), 1);
		place->first = relic_block_piece(&cursor, &bytes, place->room,
		    sizeof(*place->first));
	}

	return (bytes);
}

/*
 * Number, in target order, every part `parts_of` gives each target's
 * append, into an array of records naming each part's target and its part
 * there.  Returns 0, or -1 when the array could not be allocated.
 */
static int
number_parts(const relic_merge_t *merge,
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
 * Classify the keys of one part of a target's append, with a table of new
 * keys made on the part's own stack at the part's place, which has room
 * for every tuple the part reads.
 */
static void
classify_part(void *argument, size_t part, size_t worker_index RELIC_UNUSED)
{
	const relic_merge_keys_place_t *place;
	const relic_merge_part_t *part_record;
	relic_new_keys_t new_keys;
	relic_merge_t *merge;

	merge = argument;
	part_record = &merge->classify_parts[part];
	place = &merge->keys_places[part];

	relic_idtab_init_given(&new_keys.table, true, place->room,
	    place->slots);
	new_keys.first = place->first;
	relic_relation_classify_append(
	    merge->round->targets[part_record->target],
	    &merge->appends[part_record->target], part_record->part, &new_keys);
}

/*
 * Ask the memory limit of what every target holds while it takes its new
 * tuples -- exactly what it would hold once it had taken them, and beside
 * that the tables its growth replaces until they are freed -- and of what
 * the value table and the pool hold once they have taken the new values,
 * with everything the round holds still charged, and make that room.  The
 * serial run would hold its text buffers too, at the capacity they reach
 * by the round's end, and the question counts them: the workers' own were
 * released before it, so that the admission is at least as strict as every
 * question the serial run puts in the round.
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
	growth = relic_saturating_sum(growth, merge->texts_bytes);
	if (relic_account_exceeds(&merge->ctx->account, table_now,
	    relic_saturating_sum(growth, table_then))) {
		merge->pool->limit_declines++;
		return (-1);
	}

	/*
	 * The value table and the pool are reserved first: they can also
	 * refuse for having no identifiers left, which the serial evaluator
	 * meets before it inserts the batch, and so before any relation has
	 * grown.
	 */
	if (merge->pending_count > 0) {
		if (relic_pending_reserve(merge->ctx, &merge->pending_map) != 0)
			return (-1);
		merge->pending_reserved = true;
	}
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
		merge->pending_reserved = false;
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
		if (merge->pool->tasks[t].inserted != 0)
			merge->round->items[merge->pool->tasks[t].item].gained =
			    true;
	}
}

/*
 * Apply one part of a target's append.
 */
static void
apply_part(void *argument, size_t part, size_t worker_index RELIC_UNUSED)
{
	const relic_merge_part_t *part_record;
	relic_merge_t *merge;

	merge = argument;
	part_record = &merge->apply_parts[part];
	relic_relation_apply_append(merge->round->targets[part_record->target],
	    &merge->appends[part_record->target], part_record->part);
}

/*
 * Release everything the merge holds, whether it applied or declined: the
 * appends, with their charge and whatever growth they prepared and did not
 * fill, the pending values, their bytes and their map, with their charge,
 * a step's block a failure left open, and the records.
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

	/*
	 * A reservation made for a merge that then declined is given back:
	 * the serial run makes its own, and the copies of the new strings
	 * must not be made twice.
	 */
	if (merge->pending_reserved)
		relic_pending_unreserve(merge->ctx, &merge->pending_map);

	if (merge->pending != NULL) {
		free(merge->pending);
		free(merge->pending_bytes);
		relic_pending_map_close(&merge->pending_map);
		relic_account_release_round(&merge->ctx->account,
		    merge->pending_charged);
	}

	block_close(merge, &merge->find_block);
	block_close(merge, &merge->classify_block);

	free(merge->appends);
	free(merge->projected);
	free(merge->worker_lists);
	free(merge->task_firsts);
	free(merge->task_starts);
	free(merge->listing_parts);
	free(merge->classify_parts);
	free(merge->keys_places);
	free(merge->apply_parts);
}

/*
 * Give a step's block the arrays `layout` lays out, charged to the round
 * before it is allocated.  `layout` sets every part's record each time it
 * runs: given no memory it measures, setting each part's room and leaving
 * its arrays NULL, and given the block it carves the arrays from it.  A
 * step that needs no bytes keeps the records its measuring set, each with
 * no room and no arrays, which a part with nothing to do reads as such,
 * and its block is neither allocated nor carved.  Returns 0, or -1 having
 * charged nothing and holding nothing, when the bytes are more than a size
 * counts or the limit or the allocator refused them.
 */
static int
block_open(relic_merge_t *merge, relic_merge_block_t *block,
    size_t (*layout)(relic_merge_t *, void *))
{
	relic_account_t *account;
	size_t carved;
	size_t bytes;

	assert(block->memory == NULL && block->bytes == 0);
	account = &merge->ctx->account;

	bytes = layout(merge, NULL);
	if (bytes == 0)
		return (0);
	if (bytes == SIZE_MAX || !relic_account_charge_round(account, bytes))
		return (-1);

	block->memory = malloc(bytes);
	if (block->memory == NULL) {
		relic_account_release_round(account, bytes);
		return (-1);
	}
	block->bytes = bytes;

	carved = layout(merge, block->memory);
	assert(carved == bytes);
	(void)carved;

	return (0);
}

/*
 * Release a step's block and its charge, leaving it empty.  The tables
 * carved from it hold nothing of their own, and are forgotten with it.
 */
static void
block_close(relic_merge_t *merge, relic_merge_block_t *block)
{
	free(block->memory);
	relic_account_release_round(&merge->ctx->account, block->bytes);
	block->memory = NULL;
	block->bytes = 0;
}
