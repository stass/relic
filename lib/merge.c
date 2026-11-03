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
 * Deferred publication has one boundary: prepare ordered appends, reserve
 * every public allocation, then apply without failure. Until application,
 * tuples, values and index entries remain private; reservation may retain
 * larger capacities even when the merge is declined.
 *
 * The general collector resolves pending values and finds each tuple's first
 * candidate in task order. The dense collector orders cell positions by their
 * earliest occurrence ranks. Both give every task its insertion count and its
 * starting position in its target's append, preserving serial tuple IDs.
 *
 * Staging fills those disjoint append ranges and lists them by index stripe.
 * Classification numbers new secondary keys in insertion order. Reservation
 * checks the exact serial storage projection, including replaced tables and
 * expression scratch, before growing relation, index and value arrays.
 * Application publishes values and columns, then fills canonical tables and
 * links secondary chains. The relation store defines the independent parts
 * of listing, classification and application; pool barriers separate them.
 *
 * A private failure or interrupt discards preparation and asks the caller to
 * retry the round serially, where the ordinary evaluator reports the outcome.
 * Interrupted staging is checked before listing can read its unfinished rows.
 * The final interrupt check precedes application, which cannot be rolled back.
 */

typedef struct relic_merge relic_merge_t;
typedef struct relic_merge_block relic_merge_block_t;
typedef struct relic_merge_compaction relic_merge_compaction_t;
typedef struct relic_merge_firsts relic_merge_firsts_t;
typedef struct relic_merge_firsts_place relic_merge_firsts_place_t;
typedef struct relic_merge_keys_place relic_merge_keys_place_t;
typedef struct relic_merge_list relic_merge_list_t;
typedef struct relic_merge_part relic_merge_part_t;
typedef struct relic_merge_ref relic_merge_ref_t;
typedef struct relic_merge_span relic_merge_span_t;
typedef struct relic_merge_resolution relic_merge_resolution_t;

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
 * One worker's part of a compaction.  `descriptor_of[m - 1]` is the
 * number, counting from one, of the compact descriptor that worker-local
 * marker m was given at its first occurrence, and zero while no candidate
 * column met so far holds m; there is an entry for each of the worker's
 * pending values.  `byte_base` is where the worker's pending strings'
 * bytes begin in the merge's copy of every worker's.
 */
struct relic_merge_span {
	uint32_t	*descriptor_of;
	size_t		 byte_base;
};

/*
 * The scratch resolve_pending() compacts the round's pending values in,
 * which it releases once every candidate is rewritten.
 *
 * A worker gives each distinct value it holds pending one local marker, so
 * one value may fill many candidate columns.  The compaction keeps one
 * descriptor per local marker some candidate holds, in the order the walk
 * of the candidates in task order first meets each, and that sequence is
 * what the value resolver measures.  `descriptors` has room for `room`,
 * every worker's pending values together, and holds `count`; equal values
 * of two workers are two descriptors, which the resolver promises one
 * identifier.  `string_bytes` is the bytes of every worker's pending
 * strings together, which the spans divide between the workers.  `spans`
 * has a record for each of the pool's workers.  The maps and the
 * descriptors lie in `block`, charged to the round.
 */
struct relic_merge_compaction {
	relic_merge_t		*merge;
	relic_pending_t		*descriptors;
	size_t			 room;
	size_t			 count;
	size_t			 string_bytes;
	relic_merge_block_t	 block;
	relic_merge_span_t	 spans[RELIC_THREADS_MAX];
};

/*
 * The round's pending values once they are resolved, which the merge
 * holds until it applies or declines.  It is opened only when some
 * candidate holds a pending value, once every such value has its compact
 * descriptor.  `map` holds the identifier promised each descriptor, and
 * `bytes` the copy of every worker's pending strings, which the map's new
 * strings point into until they are appended or their reservation is given
 * back.  `charged` is what the two were charged, `bytes` being NULL when
 * they are not held.  `projected` is the bytes the value table and the
 * pool will hold once they have taken the new values, and `reserved` says
 * they hold a reservation for them not yet appended.  The map's count is
 * zero unless some value was promised.
 */
struct relic_merge_resolution {
	relic_pending_map_t	 map;
	char			*bytes;
	size_t			 charged;
	size_t			 projected;
	bool			 reserved;
};

/*
 * A merge under way.  `appends` holds each target's new tuples, and
 * `projected` the bytes each target will hold once it has taken them.
 * `resolution` holds the round's resolved pending values.  `texts_bytes`
 * is at least what the serial run's text buffers would hold by the round's
 * end, which the admission counts.
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
	relic_dense_t			*dense;
	relic_t				*ctx;
	relic_pool_t			*pool;
	relic_round_t			*round;
	relic_destination_append_t	*appends;
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
	relic_merge_resolution_t	 resolution;
	size_t				 texts_bytes;
};

static void	 apply_merge(relic_merge_t *merge);
static void	 apply_part(void *argument, size_t part, size_t worker_index);
static void	 block_close(relic_merge_t *merge,
		    relic_merge_block_t *block);
static int	 block_open(relic_merge_t *merge, relic_merge_block_t *block,
		    size_t (*layout)(void *, void *), void *owner);
static size_t	 classify_layout(void *owner, void *memory);
static int	 classify_new_keys(relic_merge_t *merge);
static void	 classify_part(void *argument, size_t part,
		    size_t worker_index);
static int	 collect_descriptors(relic_merge_compaction_t *compaction);
static int	 compact_and_resolve(relic_merge_compaction_t *compaction);
static size_t	 compaction_layout(void *owner, void *memory);
static void	 copy_strings(const relic_merge_compaction_t *compaction);
static int	 count_new_tuples(relic_merge_t *merge);
static int	 describe_marker(relic_merge_compaction_t *compaction,
		    size_t worker, relic_dom_t marker);
static relic_candidate_t *entry_of(const relic_merge_t *merge,
		    relic_merge_ref_t ref);
static size_t	 find_layout(void *owner, void *memory);
static int	 find_new_tuples(relic_merge_t *merge);
static void	 find_new_in_stripe(void *argument, size_t stripe,
		    size_t worker_index);
static relic_candidate_t *firsts_find(const relic_merge_t *merge,
		    const relic_merge_firsts_t *firsts, relic_merge_ref_t ref);
static void	 firsts_add(const relic_merge_t *merge,
		    relic_merge_firsts_t *firsts, relic_merge_ref_t ref);
static size_t	 list_search(const uint32_t *list, size_t first, size_t end,
		    size_t position);
static void	 list_staged_part(void *argument, size_t part,
		    size_t worker_index);
static void	 list_worker(void *argument, size_t worker,
		    size_t worker_index);
static size_t	 measure_descriptors(
		    const relic_merge_compaction_t *compaction);
static int	 merge_begin(relic_merge_t *merge, relic_t *ctx,
		    relic_pool_t *pool, relic_round_t *round);
static void	 merge_end(relic_merge_t *merge);
static int	 number_parts(const relic_merge_t *merge,
		    size_t (*parts_of)(const relic_append_t *),
		    relic_merge_part_t **partsp, size_t *countp);
static size_t	 pending_string_bytes(const relic_pool_t *pool);
static size_t	 pending_value_count(const relic_pool_t *pool);
static int	 prepare_merge(relic_merge_t *merge, relic_t *ctx,
		    relic_pool_t *pool, relic_round_t *round,
		    size_t texts_bytes, relic_dense_t *dense);
static void	 prepare_spans(relic_merge_compaction_t *compaction);
static int	 reserve_room(relic_merge_t *merge);
static void	 resolution_close(relic_merge_t *merge);
static int	 resolution_open(relic_merge_t *merge, size_t room,
		    size_t string_bytes);
static bool	 resolution_promised(
		    const relic_merge_resolution_t *resolution);
static int	 resolve_pending(relic_merge_t *merge);
static bool	 rewrite_markers(const relic_merge_span_t *span,
		    const relic_dom_t *promised, relic_dom_t *tuple,
		    uint16_t arity);
static void	 rewrite_worker(void *argument, size_t worker,
		    size_t worker_index);
static int	 stage_new_tuples(relic_merge_t *merge);
static void	 stage_task(void *argument, size_t task, size_t worker_index);
static size_t	 stripe_candidates(const relic_pool_t *pool, size_t stripe);
static const relic_dom_t *tuple_of(const relic_merge_t *merge,
		    relic_merge_ref_t ref);

bool
relic_round_merge(relic_t *ctx, relic_pool_t *pool, relic_round_t *round,
    size_t texts_bytes, relic_dense_t *dense)
{
	relic_merge_t merge;
	bool merged;

	merged = prepare_merge(&merge, ctx, pool, round, texts_bytes, dense) == 0;
	if (relic_signal_flag_raised(&ctx->interrupted))
		merged = false;
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
 *
 * The steps walk candidates two ways: task by task, through each task's
 * run of its worker's candidates, and worker by worker, through all of a
 * worker's.  The two meet the same candidates only when the tasks' runs
 * cover every candidate the workers hold, which a worker's tasks do, its
 * runs following one another; a merge whose counts disagree is declined.
 */
static int
merge_begin(relic_merge_t *merge, relic_t *ctx, relic_pool_t *pool,
    relic_round_t *round)
{
	size_t candidates;
	size_t held;
	size_t i;

	bzero(merge, sizeof(*merge));
	merge->ctx = ctx;
	merge->pool = pool;
	merge->round = round;

	candidates = 0;
	for (i = 0; i < pool->task_count; i++)
		candidates += pool->tasks[i].candidate_count;
	held = 0;
	for (i = 0; i < pool->worker_count; i++)
		held += pool->workers[i].candidates.count;
	assert(candidates == held);
	if (candidates != held || candidates > RELIC_TUPLE_MAX)
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
    relic_round_t *round, size_t texts_bytes, relic_dense_t *dense)
{
	if (merge_begin(merge, ctx, pool, round) != 0)
		return (-1);
	merge->texts_bytes = texts_bytes;
	merge->dense = dense;
	if (dense != NULL) {
		if (relic_dense_order(dense) != 0)
			return (-1);
	} else {
		if (resolve_pending(merge) != 0)
			return (-1);
		if (find_new_tuples(merge) != 0)
			return (-1);
	}
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
 * Promise every value the candidates hold pending the identifier the serial
 * run gives it, and write that identifier over each column holding it.
 *
 * A worker holds each distinct value it met pending once, and its
 * candidates' columns hold local markers naming those values, so one value
 * can fill many columns.  The walk of the candidates in task order, each
 * task's candidates in the order its worker derived them and each
 * candidate's columns in order, meets the values in the order the serial
 * evaluator meets them.  It gives a local marker a compact descriptor the
 * first time it meets it and passes over it after that: every occurrence it
 * passes over follows an equal one it kept, so each value's first
 * occurrence among the descriptors is its first in the walk.  The value
 * resolver promises new values identifiers in order of first occurrence,
 * so from the descriptors it promises each value the identifier the serial
 * run gives it, having measured no more descriptors than the workers hold
 * pending values, however many columns hold them.  Equal values of two
 * workers are two descriptors, promised one identifier.  Then, a worker's
 * candidates to a part, each local marker is replaced by the identifier
 * promised its descriptor, the descriptor number itself never being
 * written, and each candidate that held a marker is hashed again.
 *
 * Nothing is interned until the merge can no longer be declined.  The
 * resolution, sized by the descriptors, is opened only once the walk has
 * made them.  The compaction's maps and descriptors are released once
 * every part has rewritten its candidates; the resolved values, whose new
 * strings point into the merge's copy of the workers' bytes, stay until
 * merge_end().  Returns 0, or -1 when the workers hold more pending values
 * than a marker can count or more string bytes than a size counts, the
 * scratch or the resolution could not be had, a marker names none of its
 * worker's values, or the new values' storage is more than a size counts.
 */
static int
resolve_pending(relic_merge_t *merge)
{
	relic_merge_compaction_t compaction;
	int status;

	bzero(&compaction, sizeof(compaction));
	compaction.merge = merge;
	compaction.room = pending_value_count(merge->pool);
	if (compaction.room == 0)
		return (0);
	compaction.string_bytes = pending_string_bytes(merge->pool);
	if (compaction.room > RELIC_DOM_INDEX_MASK ||
	    compaction.string_bytes == SIZE_MAX)
		return (-1);

	if (block_open(merge, &compaction.block, compaction_layout,
	    &compaction) != 0)
		return (-1);
	status = compact_and_resolve(&compaction);
	block_close(merge, &compaction.block);

	return (status);
}

/*
 * How many values every worker holds pending together, each distinct value
 * of a worker counted once, saturating at SIZE_MAX.
 */
static size_t
pending_value_count(const relic_pool_t *pool)
{
	size_t count;
	size_t w;

	count = 0;
	for (w = 0; w < pool->worker_count; w++)
		count = relic_saturating_sum(count,
		    pool->workers[w].candidates.value_count);

	return (count);
}

/*
 * The bytes of every worker's pending strings, each distinct string of a
 * worker counted once, saturating at SIZE_MAX.
 */
static size_t
pending_string_bytes(const relic_pool_t *pool)
{
	size_t bytes;
	size_t w;

	bytes = 0;
	for (w = 0; w < pool->worker_count; w++)
		bytes = relic_saturating_sum(bytes,
		    pool->workers[w].candidates.value_byte_count);

	return (bytes);
}

/*
 * Lay the compaction's block out from `memory`, or only measure it when
 * `memory` is NULL (relic_block_piece()), and return its bytes, SIZE_MAX
 * when they are more than a size counts: each worker's map, with an entry
 * for each of its pending values, and room for a descriptor for every
 * pending value.
 */
static size_t
compaction_layout(void *owner, void *memory)
{
	relic_merge_compaction_t *compaction;
	const relic_pool_t *pool;
	relic_merge_span_t *span;
	size_t bytes;
	size_t w;
	char *cursor;

	compaction = owner;
	pool = compaction->merge->pool;
	cursor = memory;
	bytes = 0;

	assert(pool->worker_count <= RELIC_THREADS_MAX);
	for (w = 0; w < pool->worker_count; w++) {
		span = &compaction->spans[w];
		span->descriptor_of = relic_block_piece(&cursor, &bytes,
		    pool->workers[w].candidates.value_count,
		    sizeof(*span->descriptor_of));
	}
	compaction->descriptors = relic_block_piece(&cursor, &bytes,
	    compaction->room, sizeof(*compaction->descriptors));

	return (bytes);
}

/*
 * Resolve the pending values into the merge's resolution, the compaction's
 * block open: describe every local marker the candidates hold; then, when
 * some candidate holds a pending value, open the resolution with room for
 * the descriptors, copy the workers' strings into it, measure the
 * descriptors, and rewrite every worker's candidates.  Values no candidate
 * holds are never described, and when no candidate holds one no resolution
 * is opened.  Returns 0, or -1 as
 * resolve_pending() does, the candidates untouched, leaving whatever
 * resolution it opened for merge_end() to release.
 */
static int
compact_and_resolve(relic_merge_compaction_t *compaction)
{
	relic_merge_resolution_t *resolution;
	relic_merge_t *merge;

	merge = compaction->merge;
	resolution = &merge->resolution;
	prepare_spans(compaction);
	if (collect_descriptors(compaction) != 0)
		return (-1);
	if (compaction->count == 0)
		return (0);

	if (resolution_open(merge, compaction->count,
	    compaction->string_bytes) != 0)
		return (-1);
	copy_strings(compaction);

	resolution->projected = measure_descriptors(compaction);
	if (resolution->projected == SIZE_MAX)
		return (-1);

	relic_pool_for(merge->pool, merge->pool->worker_count, rewrite_worker,
	    compaction);

	return (0);
}

/*
 * Ready every worker's span: clear its map, no marker having a descriptor
 * yet, and give it where its pending strings' bytes will begin in the
 * resolution's copy, the workers' end to end in worker order, so that
 * describing a string can name its bytes there before the copy is made.
 * The offsets stay within the compaction's `string_bytes`, their sum, which
 * resolve_pending() refused when it saturated, so no partial sum wraps.
 */
static void
prepare_spans(relic_merge_compaction_t *compaction)
{
	const relic_candidates_t *holder;
	relic_merge_span_t *span;
	relic_merge_t *merge;
	size_t base;
	size_t w;

	merge = compaction->merge;
	base = 0;
	for (w = 0; w < merge->pool->worker_count; w++) {
		holder = &merge->pool->workers[w].candidates;
		span = &compaction->spans[w];
		if (holder->value_count > 0)
			bzero(span->descriptor_of, holder->value_count *
			    sizeof(*span->descriptor_of));
		span->byte_base = base;
		base += holder->value_byte_count;
	}
	assert(base == compaction->string_bytes);
}

/*
 * Give every local marker the candidates hold a compact descriptor, at its
 * first occurrence in the walk of every task's candidates in task order,
 * each task's in the order its worker derived them and each candidate's
 * columns in order.  Only the maps and the descriptors are written: the
 * candidates keep their markers until every one has a descriptor and the
 * descriptors their promises.  Returns 0, or -1 when a marker names none
 * of its worker's values.
 */
static int
collect_descriptors(relic_merge_compaction_t *compaction)
{
	const relic_candidates_t *candidates;
	const relic_candidate_t *candidate;
	const relic_round_task_t *task;
	const relic_merge_t *merge;
	const relic_dom_t *tuple;
	uint16_t arity;
	uint16_t column;
	size_t end;
	size_t t;
	size_t i;

	merge = compaction->merge;
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
			    relic_destination_relation(merge->round->targets[candidate->target]));
			for (column = 0; column < arity; column++) {
				if ((tuple[column] & RELIC_DOM_TYPE_MASK) != 0)
					continue;
				if (describe_marker(compaction, task->worker,
				    tuple[column]) != 0)
					return (-1);
			}
		}
	}

	return (0);
}

/*
 * Give local marker `marker` of worker `worker` the next compact
 * descriptor, a copy of the value it names, unless an earlier occurrence
 * gave it one.  A string's descriptor names its bytes where the worker's
 * land in the merge's copy.  A map entry goes from zero to its
 * descriptor once, so the descriptors never outnumber the pending values,
 * the compaction's room.  Returns 0, or -1 having described nothing when
 * the marker names none of the worker's values or a string's bytes lie
 * outside the worker's, which a worker's pending values never do.
 */
static int
describe_marker(relic_merge_compaction_t *compaction, size_t worker,
    relic_dom_t marker)
{
	const relic_candidates_t *candidates;
	const relic_pending_t *value;
	relic_merge_span_t *span;
	relic_pending_t *descriptor;
	bool valid;

	candidates = &compaction->merge->pool->workers[worker].candidates;
	span = &compaction->spans[worker];
	valid = marker >= 1 && marker <= candidates->value_count;
	assert(valid);
	if (!valid)
		return (-1);
	if (span->descriptor_of[marker - 1] != 0)
		return (0);

	value = &candidates->values[marker - 1];
	valid = value->kind != RELIC_PENDING_TEXT ||
	    (value->u.text.offset <= candidates->value_byte_count &&
	    value->u.text.length <= candidates->value_byte_count -
	    value->u.text.offset);
	assert(valid);
	if (!valid)
		return (-1);

	assert(compaction->count < compaction->room);
	descriptor = &compaction->descriptors[compaction->count];
	*descriptor = *value;
	if (descriptor->kind == RELIC_PENDING_TEXT)
		descriptor->u.text.offset += span->byte_base;
	compaction->count++;
	span->descriptor_of[marker - 1] = (uint32_t)compaction->count;

	return (0);
}

/*
 * Give the merge's resolution a map with room for `room` descriptors and a
 * buffer for `string_bytes` bytes of pending strings, at least one so that
 * an empty string names a byte that exists, charged to the round before
 * either is allocated.  Returns 0, or -1 having charged and holding
 * nothing, when their bytes are more than a size counts or the limit or
 * the allocator refused them.
 */
static int
resolution_open(relic_merge_t *merge, size_t room, size_t string_bytes)
{
	relic_merge_resolution_t *resolution;
	relic_account_t *account;
	size_t buffer_bytes;
	size_t charge;

	resolution = &merge->resolution;
	account = &merge->ctx->account;
	assert(resolution->bytes == NULL);
	buffer_bytes = string_bytes > 0 ? string_bytes : 1;
	charge = relic_saturating_sum(relic_pending_map_bytes(room),
	    buffer_bytes);
	if (charge == SIZE_MAX || !relic_account_charge_round(account, charge))
		return (-1);

	resolution->bytes = malloc(buffer_bytes);
	if (resolution->bytes == NULL) {
		relic_account_release_round(account, charge);
		return (-1);
	}
	if (relic_pending_map_open(&resolution->map, room) != 0) {
		free(resolution->bytes);
		resolution->bytes = NULL;
		relic_account_release_round(account, charge);
		return (-1);
	}
	resolution->charged = charge;

	return (0);
}

/*
 * Measure the compaction's descriptors into the open resolution's map,
 * with a probe charged to the round only while it measures.  Returns what
 * relic_pending_measure() returns, or SIZE_MAX when the limit or the
 * allocator refused the probe.
 */
static size_t
measure_descriptors(const relic_merge_compaction_t *compaction)
{
	relic_pending_probe_t probe;
	relic_merge_resolution_t *resolution;
	relic_account_t *account;
	relic_merge_t *merge;
	size_t projected;
	size_t charge;

	merge = compaction->merge;
	resolution = &merge->resolution;
	account = &merge->ctx->account;
	charge = relic_pending_probe_bytes(compaction->count);
	if (charge == SIZE_MAX || !relic_account_charge_round(account, charge))
		return (SIZE_MAX);
	if (relic_pending_probe_open(&probe, compaction->count) != 0) {
		relic_account_release_round(account, charge);
		return (SIZE_MAX);
	}

	projected = relic_pending_measure(merge->ctx, compaction->descriptors,
	    compaction->count, resolution->bytes, &probe, &resolution->map);

	relic_pending_probe_close(&probe);
	relic_account_release_round(account, charge);

	return (projected);
}

/*
 * Copy every worker's pending strings' bytes, once, into the open
 * resolution's buffer, where the worker's span says they begin.
 */
static void
copy_strings(const relic_merge_compaction_t *compaction)
{
	const relic_candidates_t *holder;
	const relic_merge_t *merge;
	size_t w;

	merge = compaction->merge;
	for (w = 0; w < merge->pool->worker_count; w++) {
		holder = &merge->pool->workers[w].candidates;
		if (holder->value_byte_count > 0)
			bcopy(holder->value_bytes, merge->resolution.bytes +
			    compaction->spans[w].byte_base,
			    holder->value_byte_count);
	}
}

/*
 * Write the identifier promised over every local marker among worker
 * `worker`'s candidates, through the worker's map of descriptors, and hash
 * each candidate that held one again, moving it to its stripe.  The walk
 * that described the markers met every candidate, so every marker has a
 * descriptor.  Only the worker's own candidates and stripe counts are
 * written; the compaction and the promises are only read.
 */
static void
rewrite_worker(void *argument, size_t worker, size_t worker_index RELIC_UNUSED)
{
	const relic_merge_compaction_t *compaction;
	const relic_relation_t *relation;
	const relic_merge_t *merge;
	relic_candidates_t *candidates;
	relic_candidate_t *candidate;
	relic_dom_t *tuple;
	size_t stripe;
	size_t i;

	compaction = argument;
	merge = compaction->merge;
	candidates = &merge->pool->workers[worker].candidates;
	if (candidates->value_count == 0)
		return;

	for (i = 0; i < candidates->count; i++) {
		candidate = &candidates->entries[i];
		relation = relic_destination_relation(merge->round->targets[candidate->target]);
		tuple = &candidates->words[candidate->offset];
		if (!rewrite_markers(&compaction->spans[worker],
		    merge->resolution.map.ids, tuple,
		    relic_relation_arity(relation)))
			continue;

		candidate->hash = relic_relation_hash(relation, tuple);
		stripe = relic_hash_stripe(candidate->hash);
		candidates->by_stripe[candidates->stripes[i]]--;
		candidates->by_stripe[stripe]++;
		candidates->stripes[i] = (uint8_t)stripe;
	}
}

/*
 * Replace each local marker among `tuple`'s `arity` columns with the
 * identifier `promised` holds for the descriptor `span` maps the marker
 * to, and return whether any column held a marker.
 */
static bool
rewrite_markers(const relic_merge_span_t *span, const relic_dom_t *promised,
    relic_dom_t *tuple, uint16_t arity)
{
	uint32_t descriptor;
	uint16_t column;
	bool rewritten;

	rewritten = false;
	for (column = 0; column < arity; column++) {
		if ((tuple[column] & RELIC_DOM_TYPE_MASK) != 0)
			continue;

		descriptor = span->descriptor_of[tuple[column] - 1];
		assert(descriptor != 0);
		tuple[column] = promised[descriptor - 1];
		rewritten = true;
	}

	return (rewritten);
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
	if (block_open(merge, &merge->find_block, find_layout, merge) != 0)
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
find_layout(void *owner, void *memory)
{
	relic_merge_firsts_place_t *place;
	relic_merge_t *merge;
	size_t bytes;
	size_t s;
	size_t w;
	char *cursor;

	merge = owner;
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
	relic_candidate_t *first, *candidate;
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
			candidate = &candidates->entries[ref.position];
			first = firsts_find(merge, &firsts, ref);
			if (first != NULL) {
				first->contribution = relic_contribution_combine(
				    &merge->round->targets[candidate->target]->view,
				    first->contribution, candidate->contribution);
				candidate->contribution = 0;
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
static relic_candidate_t *
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
static relic_candidate_t *
firsts_find(const relic_merge_t *merge, const relic_merge_firsts_t *firsts,
    relic_merge_ref_t ref)
{
	const relic_candidate_t *candidate;
	relic_candidate_t *first;
	relic_idprobe_t probe;
	uint16_t arity;
	uint32_t met_plus_one;

	candidate = entry_of(merge, ref);
	arity = relic_relation_arity(relic_destination_relation(merge->round->targets[candidate->target]));

	relic_idprobe_open(&probe, &firsts->table,
	    relic_candidate_screen_hash(candidate->target, candidate->hash));
	while (relic_idprobe_next(&probe, &met_plus_one)) {
		first = entry_of(merge, firsts->refs[met_plus_one - 1]);
		if (first->target == candidate->target &&
		    relic_tuple_equal(tuple_of(merge,
		    firsts->refs[met_plus_one - 1]), tuple_of(merge, ref),
		    arity))
			return (first);
	}

	return (NULL);
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
		append = &merge->appends[target].relation;
		merge->task_starts[t] = append->count;
		if (merge->dense != NULL) {
			append->count += task->inserted;
			continue;
		}

		task->inserted = 0;
		for (s = 0; s < RELIC_STRIPES; s++) {
			firsts = merge->task_firsts[t * RELIC_STRIPES + s];
			append->growth.by_stripe[s] += firsts;
			append->count += firsts;
			task->inserted += firsts;
		}
	}

	if (merge->dense != NULL && merge->appends[0].relation.count != merge->dense->count)
		return (-1);
	total = 0;
	for (t = 0; t < merge->round->target_count; t++) {
		append = &merge->appends[t].relation;
		if (append->count > RELIC_TUPLE_MAX -
		    relic_relation_count(relic_destination_relation(merge->round->targets[t])))
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
 * list each append's staged tuples by stripe, an index to a part.  A dense
 * collector's positions, all copied into the append, are then released.
 * Returns 0, or -1 when an append's arrays or the list of parts could not
 * be had or the context was interrupted.
 */
static int
stage_new_tuples(relic_merge_t *merge)
{
	size_t t;
	size_t s;

	for (t = 0; t < merge->round->target_count; t++) {
		if (relic_destination_append_open(merge->ctx,
		    merge->round->targets[t], &merge->appends[t]) != 0)
			return (-1);
	}

	relic_pool_for(merge->pool, merge->pool->task_count, stage_task,
	    merge);
	if (relic_signal_flag_raised(&merge->ctx->interrupted))
		return (-1);

	if (number_parts(merge, relic_relation_list_parts,
	    &merge->listing_parts, &merge->listing_part_count) != 0)
		return (-1);
	relic_pool_for(merge->pool, merge->listing_part_count, list_staged_part,
	    merge);
	if (merge->dense != NULL) {
		for (s = 0; s < RELIC_STRIPES; s++)
			merge->appends[0].relation.growth.by_stripe[s] =
			    merge->appends[0].relation.stripe_start[s + 1] -
			    merge->appends[0].relation.stripe_start[s];
		relic_dense_release_order(merge->dense);
	}

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
	relic_relation_list_append(&merge->appends[part_record->target].relation,
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
	relic_dom_t tuple[2];
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
	relation = relic_destination_relation(merge->round->targets[target]);
	candidates = &merge->pool->workers[record->worker].candidates;

	position = merge->task_starts[task];
	if (merge->dense != NULL) {
		end = position + record->inserted;
		for (; position < end; position++) {
			if ((position & 4095u) == 0 &&
			    relic_signal_flag_raised(&merge->ctx->interrupted))
				return;
			relic_dense_tuple(merge->dense, position, tuple);
			relic_destination_stage(merge->round->targets[target], &merge->appends[target],
			    position, tuple, relic_relation_hash(relation, tuple), 1);
		}
		return;
	}
	end = record->first_candidate + record->candidate_count;
	for (i = record->first_candidate; i < end; i++) {
		candidate = &candidates->entries[i];
		if (candidate->contribution == 0)
			continue;

		relic_destination_stage(merge->round->targets[target], &merge->appends[target],
		    position, &candidates->words[candidate->offset],
		    candidate->hash, candidate->contribution);
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
	if (block_open(merge, &merge->classify_block, classify_layout,
	    merge) != 0)
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
classify_layout(void *owner, void *memory)
{
	const relic_merge_part_t *part_record;
	relic_merge_keys_place_t *place;
	relic_merge_t *merge;
	size_t bytes;
	size_t p;
	char *cursor;

	merge = owner;
	cursor = memory;
	bytes = 0;

	for (p = 0; p < merge->classify_part_count; p++) {
		part_record = &merge->classify_parts[p];
		place = &merge->keys_places[p];
		place->room = relic_relation_classify_tuples(
		    &merge->appends[part_record->target].relation, part_record->part);
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
		count += parts_of(&merge->appends[t].relation);
	*countp = 0;
	if (count == 0)
		return (0);

	parts = calloc(count, sizeof(*parts));
	if (parts == NULL)
		return (-1);
	*partsp = parts;

	for (t = 0; t < merge->round->target_count; t++) {
		target_parts = parts_of(&merge->appends[t].relation);
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
	    relic_destination_relation(merge->round->targets[part_record->target]),
	    &merge->appends[part_record->target].relation, part_record->part, &new_keys);
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
	table_then = resolution_promised(&merge->resolution) ?
	    merge->resolution.projected : table_now;

	growth = 0;
	for (t = 0; t < merge->round->target_count; t++) {
		relation = relic_destination_relation(merge->round->targets[t]);
		merge->projected[t] = relic_relation_bytes_after_append(
		    relation, &merge->appends[t].relation);
		during = relic_destination_bytes_during_append(merge->round->targets[t],
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
	if (resolution_promised(&merge->resolution)) {
		if (relic_pending_reserve(merge->ctx,
		    &merge->resolution.map) != 0)
			return (-1);
		merge->resolution.reserved = true;
	}
	for (t = 0; t < merge->round->target_count; t++) {
		if (relic_destination_reserve_append(merge->ctx,
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
 * Give the value table and the pool the new values, under the identifiers
 * the candidates already hold, and every target its new tuples: their
 * columns, and then, a stripe to a part, their canonical entries and their
 * links in every secondary index.
 * Nothing here can fail.  Every item one of whose tasks inserted a new
 * tuple has gained, and each new tuple counts as inserted.
 */
static void
apply_merge(relic_merge_t *merge)
{
	const relic_relation_t *relation;
	size_t t;

	if (resolution_promised(&merge->resolution)) {
		relic_pending_append(merge->ctx, &merge->resolution.map);
		merge->resolution.reserved = false;
		assert(relic_value_storage_bytes(merge->ctx) ==
		    merge->resolution.projected);
	}

	for (t = 0; t < merge->round->target_count; t++)
		relic_destination_take_append(merge->ctx,
		    merge->round->targets[t], &merge->appends[t]);

	relic_pool_for(merge->pool, merge->apply_part_count, apply_part,
	    merge);

	for (t = 0; t < merge->round->target_count; t++) {
		relation = relic_destination_relation(merge->round->targets[t]);
		relic_relation_finish_append(merge->ctx, relation,
		    &merge->appends[t].relation);
		assert(relic_relation_bytes(relation) == merge->projected[t]);
		merge->ctx->counters.tuples_inserted += merge->appends[t].relation.count;
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
	relic_relation_apply_append(merge->round->targets[part_record->target]->relation,
	    &merge->appends[part_record->target].relation, part_record->part);
}

/*
 * Release everything the merge holds, whether it applied or declined: the
 * appends, with their charge and whatever growth they prepared and did not
 * fill, the resolved values, a step's block a failure left open, and the
 * records.
 */
static void
merge_end(relic_merge_t *merge)
{
	size_t t;

	if (merge->appends != NULL) {
		for (t = 0; t < merge->round->target_count; t++)
			relic_destination_append_close(merge->ctx,
			    &merge->appends[t]);
	}

	resolution_close(merge);

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
 * Release the merge's resolution, its map and its bytes with their
 * charge, leaving it empty; a resolution never opened, or released already,
 * holds nothing.  A reservation made for a merge that then declined is
 * given back first, while the bytes its new strings point into exist: the
 * serial run makes its own, and the copies of the new strings must not be
 * made twice.
 */
static void
resolution_close(relic_merge_t *merge)
{
	relic_merge_resolution_t *resolution;

	resolution = &merge->resolution;
	if (resolution->reserved)
		relic_pending_unreserve(merge->ctx, &resolution->map);

	if (resolution->bytes != NULL) {
		relic_pending_map_close(&resolution->map);
		free(resolution->bytes);
		relic_account_release_round(&merge->ctx->account,
		    resolution->charged);
	}
	bzero(resolution, sizeof(*resolution));
}

/*
 * Whether the merge has promised pending values identifiers, which it
 * must reserve room for and append.
 */
static bool
resolution_promised(const relic_merge_resolution_t *resolution)
{
	return (resolution->map.count > 0);
}

/*
 * Give a step's block the arrays `layout` lays out for `owner`, the merge
 * or a step's own record, charged to the round before it is allocated.
 * `layout` sets every part's record each time it runs: given no memory it
 * measures, setting each part's room and leaving its arrays NULL, and given
 * the block it carves the arrays from it.  A step that needs no bytes keeps
 * the records its measuring set, each with no room and no arrays, which a
 * part with nothing to do reads as such, and its block is neither
 * allocated nor carved.  Returns 0, or -1 having charged nothing and
 * holding nothing, when the bytes are more than a size counts or the limit
 * or the allocator refused them.
 */
static int
block_open(relic_merge_t *merge, relic_merge_block_t *block,
    size_t (*layout)(void *, void *), void *owner)
{
	relic_account_t *account;
	size_t carved RELIC_UNUSED;
	size_t bytes;

	assert(block->memory == NULL && block->bytes == 0);
	account = &merge->ctx->account;

	bytes = layout(owner, NULL);
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

	carved = layout(owner, block->memory);
	assert(carved == bytes);

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
