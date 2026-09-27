/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * A load's facts, staged and then committed to the base stores all or
 * nothing.
 *
 * The facts a program states for a predicate are the tuples of its base
 * store, which nothing but this commit writes.  A load stages its facts as
 * it reads them: one staged set per predicate its facts name, in the order
 * the load first names them, each holding the load's distinct facts of that
 * predicate packed at its arity, an identifier table over them that makes
 * the set a set, and per tuple its hash and a mark.  The hash is the one
 * every relation of the context gives the tuple, computed once as the fact
 * is staged and used by the set's table, the measure and the append alike.
 * When the whole load has been read, the commit adds them in four steps,
 * ordered so that every step that can fail comes before the first that
 * changes anything a reader can see:
 *
 *	measure: look each staged tuple up in its predicate's base store and
 *	mark it new or not.  Nothing but the marks changes, and nothing can
 *	fail.
 *
 *	discard: when any tuple is new the program is about to change, so what
 *	was derived from it is discarded.  That cannot fail, and it leaves the
 *	store holding the base stores alone, so the limits of the next step
 *	count the program's facts and nothing derived.
 *
 *	reserve: make room in every store for its new tuples, both limits
 *	checked once over all of them before any store grows.
 *
 *	append: add each new tuple, in the order it was staged.  Room is made,
 *	and every tuple appended is one its store lacks, so no append looks
 *	anything up and none can fail.
 *
 * A refused reserve leaves every base store's contents as they were; what
 * was derived has been discarded, and the next query evaluates it again.
 * A load of facts the stores already hold changes nothing at all, and the
 * relations stay current.
 *
 * Staging is the load's working storage rather than the program's, and is
 * charged to no limit: it is proportional to the text being loaded, as the
 * text and the program arena are, and it is released when the load returns.
 * Charged, it would count against the memory limit while the derived
 * relations the commit is about to discard still did.  Its identifier
 * tables hash with the context's seed, since the text is untrusted.
 */

struct relic_staged_set {
	relic_dom_t	*tuples;	/* packed at the arity */
	uint32_t	*hashes;	/* by tuple: as a relation hashes it */
	bool		*marks;		/* by tuple: new to the base store */
	relic_idtab_t	 index;		/* tuple position + 1 */
	size_t		 count;
	size_t		 column_capacity;
	size_t		 hash_capacity;
	size_t		 mark_capacity;
	size_t		 new_count;
	uint64_t	 hash_seed;
	relic_pred_id_t	 pred;
	uint16_t	 arity;
};

static int	 collect_growths(relic_staging_t *staging,
		    relic_base_growth_t **growthsp, size_t *countp);
static void	 commit_append(relic_staging_t *staging);
static size_t	 commit_measure(relic_staging_t *staging);
static bool	 held_by_store(const relic_relation_t *store,
		    const relic_staged_set_t *set, size_t position);
static int	 set_add(relic_staging_t *staging, relic_staged_set_t *set,
		    const relic_dom_t *tuple);
static uint32_t	 set_hash(const relic_staged_set_t *set,
		    const relic_dom_t *tuple);
static uint32_t	 set_rehash(uint32_t id, void *ctx);
static void	 set_reset(relic_staged_set_t *set);
static const relic_dom_t *set_tuple(const relic_staged_set_t *set,
		    size_t position);
static int	 staging_set_of(relic_staging_t *staging, relic_pred_id_t pred,
		    relic_staged_set_t **setp);
static uint32_t	 staging_rehash(uint32_t id, void *ctx);

void
relic_staging_init(relic_staging_t *staging, relic_t *ctx)
{
	bzero(staging, sizeof(*staging));
	staging->ctx = ctx;
	relic_idtab_init(&staging->by_pred, false);
}

/*
 * Release everything staging holds.  This is a load's only exit, whether
 * it failed to parse, was refused by the commit or committed.
 */
void
relic_staging_reset(relic_staging_t *staging)
{
	size_t i;

	for (i = 0; i < staging->set_count; i++)
		set_reset(&staging->sets[i]);

	relic_free_array(staging->sets);
	relic_idtab_reset(&staging->by_pred);
	bzero(staging, sizeof(*staging));
}

/*
 * Stage the fact `pred(tuple...)`, `tuple` holding the predicate's arity of
 * interned values.  A fact already staged is staged once.  Returns 0, or -1
 * having reported why the fact could not be staged.
 */
int
relic_staging_add(relic_staging_t *staging, relic_pred_id_t pred,
    const relic_dom_t *tuple)
{
	relic_staged_set_t *set;

	if (staging_set_of(staging, pred, &set) != 0) {
		relic_diag_no_memory(staging->ctx);
		return (-1);
	}

	return (set_add(staging, set, tuple));
}

/*
 * Commit the staged facts to their base stores, all or nothing.  Returns 0,
 * or -1 having reported why the load is refused, with every base store's
 * contents as they were.
 */
int
relic_staging_commit(relic_staging_t *staging)
{
	relic_base_growth_t *growths;
	size_t growth_count;
	int status;

	if (commit_measure(staging) == 0)
		return (0);

	if (collect_growths(staging, &growths, &growth_count) != 0) {
		relic_diag_no_memory(staging->ctx);
		return (-1);
	}

	relic_discard_derived(staging->ctx);

	status = relic_bases_reserve(staging->ctx, growths, growth_count);
	if (status == 0)
		commit_append(staging);

	free(growths);

	return (status);
}

/*
 * Whether the base store `store`, which a predicate lacks until it holds a
 * fact, holds the `position`th tuple of `set` already.
 */
static bool
held_by_store(const relic_relation_t *store, const relic_staged_set_t *set,
    size_t position)
{
	if (store == NULL)
		return (false);

	return (relic_relation_contains_hashed(store, set_tuple(set, position),
	    set->hashes[position]));
}

/*
 * Mark every staged tuple new or not, against the base store of its
 * predicate, and count the new ones of each set.  A predicate with no base
 * store yet holds nothing, so all its tuples are new.  Every mark is
 * written here, and the append trusts them.  Returns how many tuples are
 * new in all.
 */
static size_t
commit_measure(relic_staging_t *staging)
{
	relic_staged_set_t *set;
	const relic_relation_t *store;
	size_t new_total;
	size_t i;
	size_t t;

	new_total = 0;

	for (i = 0; i < staging->set_count; i++) {
		set = &staging->sets[i];
		store = relic_base_store(staging->ctx, set->pred);
		set->new_count = 0;

		for (t = 0; t < set->count; t++) {
			set->marks[t] = !held_by_store(store, set, t);
			if (set->marks[t])
				set->new_count++;
		}

		new_total += set->new_count;
	}

	return (new_total);
}

/*
 * The growth of each base store that gains a tuple, in the order the load
 * first named their predicates, into a new array in *growthsp.  Returns 0,
 * or -1 when memory ran out, having reported nothing.
 */
static int
collect_growths(relic_staging_t *staging, relic_base_growth_t **growthsp,
    size_t *countp)
{
	relic_base_growth_t *growths;
	size_t count;
	size_t i;

	growths = reallocarray(NULL, staging->set_count, sizeof(*growths));
	if (growths == NULL)
		return (-1);

	count = 0;
	for (i = 0; i < staging->set_count; i++) {
		if (staging->sets[i].new_count == 0)
			continue;

		growths[count].pred = staging->sets[i].pred;
		growths[count].added = staging->sets[i].new_count;
		count++;
	}

	*growthsp = growths;
	*countp = count;

	return (0);
}

/*
 * Append every new tuple to its base store, set by set, each set's in the
 * order it was staged.
 */
static void
commit_append(relic_staging_t *staging)
{
	const relic_staged_set_t *set;
	size_t i;
	size_t t;

	for (i = 0; i < staging->set_count; i++) {
		set = &staging->sets[i];

		for (t = 0; t < set->count; t++) {
			if (set->marks[t])
				relic_base_append_hashed(staging->ctx,
				    set->pred, set_tuple(set, t),
				    set->hashes[t]);
		}
	}
}

/*
 * The staged set of `pred` in *setp, begun empty if this is the predicate's
 * first fact in the load.  A fact finds its set through the set the previous
 * fact used, which a file of one predicate's facts always does, and
 * otherwise through the table from predicate to set.  Returns 0, or -1 when
 * memory ran out, having reported nothing.
 */
static int
staging_set_of(relic_staging_t *staging, relic_pred_id_t pred,
    relic_staged_set_t **setp)
{
	relic_staged_set_t *sets;
	relic_staged_set_t *set;
	relic_idprobe_t probe;
	uint32_t hash;
	uint32_t id;

	if (staging->set_count > 0 &&
	    staging->sets[staging->last].pred == pred) {
		*setp = &staging->sets[staging->last];
		return (0);
	}

	hash = relic_hash_word(staging->ctx->hash_seed, pred);
	relic_idprobe_open(&probe, &staging->by_pred, hash);
	while (relic_idprobe_next(&probe, &id)) {
		if (staging->sets[id - 1].pred == pred) {
			staging->last = id - 1;
			*setp = &staging->sets[id - 1];
			return (0);
		}
	}

	if (relic_idtab_reserve(&staging->by_pred, 1, staging_rehash,
	    staging) != 0)
		return (-1);

	sets = relic_grow_array(staging->sets, &staging->set_capacity,
	    staging->set_count + 1, sizeof(*staging->sets));
	if (sets == NULL)
		return (-1);
	staging->sets = sets;

	set = &staging->sets[staging->set_count];
	bzero(set, sizeof(*set));
	relic_idtab_init(&set->index, true);
	set->hash_seed = staging->ctx->hash_seed;
	set->pred = pred;
	set->arity = relic_pred_of(staging->ctx, pred)->arity;

	staging->set_count++;
	relic_idtab_place(&staging->by_pred, hash,
	    (uint32_t)staging->set_count);
	staging->last = staging->set_count - 1;
	*setp = set;

	return (0);
}

/*
 * The hash of the predicate of the `id`th set, for the table from predicate
 * to set to use when it grows.
 */
static uint32_t
staging_rehash(uint32_t id, void *ctx)
{
	const relic_staging_t *staging;

	staging = ctx;

	return (relic_hash_word(staging->ctx->hash_seed,
	    staging->sets[id - 1].pred));
}

/*
 * Add a tuple to a set unless the set holds it.  Room in the table, the
 * columns, the hashes and the marks is made before the tuple is stored, so a
 * failure leaves the set as it was.  Returns 0, or -1 having reported that
 * the set cannot grow: past the identifiers a set numbers its tuples with,
 * or out of memory.
 */
static int
set_add(relic_staging_t *staging, relic_staged_set_t *set,
    const relic_dom_t *tuple)
{
	char described[RELIC_PRED_TEXT_MAX];
	relic_idprobe_t probe;
	relic_dom_t *tuples;
	uint32_t *hashes;
	bool *marks;
	uint32_t hash;
	uint32_t id;
	uint16_t column;

	hash = set_hash(set, tuple);
	relic_idprobe_open(&probe, &set->index, hash);
	while (relic_idprobe_next(&probe, &id)) {
		if (set->arity == 0 || bcmp(set_tuple(set, id - 1), tuple,
		    set->arity * sizeof(*tuple)) == 0)
			return (0);
	}

	if (set->count >= UINT32_MAX - 1) {
		relic_diagf(staging->ctx, RELIC_ERROR, NULL,
		    "too many facts of %s in one load",
		    relic_pred_describe(staging->ctx, set->pred, described,
		    sizeof(described)));
		return (-1);
	}

	if (relic_idtab_reserve(&set->index, 1, set_rehash, set) != 0) {
		relic_diag_no_memory(staging->ctx);
		return (-1);
	}

	hashes = relic_grow_array(set->hashes, &set->hash_capacity,
	    set->count + 1, sizeof(*set->hashes));
	if (hashes == NULL) {
		relic_diag_no_memory(staging->ctx);
		return (-1);
	}
	set->hashes = hashes;

	marks = relic_grow_array(set->marks, &set->mark_capacity,
	    set->count + 1, sizeof(*set->marks));
	if (marks == NULL) {
		relic_diag_no_memory(staging->ctx);
		return (-1);
	}
	set->marks = marks;

	if (set->arity > 0) {
		/*
		 * The column count is checked rather than formed and hoped
		 * for, though a set this large could not have been built.
		 */
		if (set->count + 1 > SIZE_MAX / set->arity) {
			relic_diag_no_memory(staging->ctx);
			return (-1);
		}
		tuples = relic_grow_array(set->tuples, &set->column_capacity,
		    (set->count + 1) * set->arity, sizeof(*set->tuples));
		if (tuples == NULL) {
			relic_diag_no_memory(staging->ctx);
			return (-1);
		}
		set->tuples = tuples;

		for (column = 0; column < set->arity; column++)
			set->tuples[set->count * set->arity + column] =
			    tuple[column];
	}

	set->hashes[set->count] = hash;
	set->marks[set->count] = false;
	set->count++;
	relic_idtab_place(&set->index, hash, (uint32_t)set->count);

	return (0);
}

/*
 * The staged tuple at `position`, or a pointer that is never read for a set
 * of arity zero.
 */
static const relic_dom_t *
set_tuple(const relic_staged_set_t *set, size_t position)
{
	static const relic_dom_t no_columns[1];

	if (set->arity == 0)
		return (no_columns);

	return (&set->tuples[position * set->arity]);
}

/*
 * A tuple's hash as every relation of the context computes it, which is what
 * lets the commit hand it to the base store.
 */
static uint32_t
set_hash(const relic_staged_set_t *set, const relic_dom_t *tuple)
{
	return (relic_tuple_hash(set->hash_seed, tuple, set->arity));
}

/*
 * The hash of the `id`th staged tuple of a set, for its table to use when
 * it grows: the one kept beside the tuple, so growing reads no columns.
 */
static uint32_t
set_rehash(uint32_t id, void *ctx)
{
	const relic_staged_set_t *set;

	set = ctx;

	return (set->hashes[id - 1]);
}

static void
set_reset(relic_staged_set_t *set)
{
	relic_free_array(set->tuples);
	relic_free_array(set->hashes);
	relic_free_array(set->marks);
	relic_idtab_reset(&set->index);
	bzero(set, sizeof(*set));
}
