/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
 * is staged and used by the set's table, the base store's lookup and the
 * append alike.
 *
 * Staging measures each fact as it stages it: it looks the tuple up in its
 * predicate's base store and marks it new or not.  Nothing can change a base
 * store while a load runs, so the mark holds until the commit.  A new tuple
 * is staged only if the load, with it, can still fit the limits: the base
 * stores' facts plus the new ones must fit the tuple limit, and a floor
 * under what the commit will hold must fit the memory limit.  So a load the
 * limits refuse is parsed no further than the fact that shows it, rather
 * than staging and interning the rest of its input first.
 *
 * The floor, `columns_floor`, is the bytes the columns of the loaded
 * predicates' base stores must hold once every new tuple is appended, each
 * store's count plus its new tuples at its arity, to which the value table
 * and the symbol pool are added.  Those two only grow while a load runs, and
 * the commit discards the derived relations before it reserves, so for any
 * growth the stores can hold the floor never exceeds what the commit's own
 * check measures: staging refuses no load the commit would admit.  A growth
 * past what a store can hold the reservation refuses in any case.  The
 * commit measures exactly, and can still refuse what the floor let
 * through.
 *
 * When the whole load has been read, the commit adds the new tuples in
 * three steps, ordered so that every step that can fail comes before the
 * first that changes anything a reader can see:
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
 * A refusal, by staging or by the reserve, names the first predicate, in
 * the order the load names them, whose store the facts read so far would
 * grow.  A refused reserve leaves every base store's contents as they were;
 * what was derived has been discarded, and the next query evaluates it
 * again.  A load of facts the stores already hold changes nothing at all,
 * and the relations stay current.
 *
 * Staging is the load's working storage rather than the program's, and is
 * charged to no limit: the limits bound it through the new tuples they
 * admit, and it is released when the load returns.  Charged, it would count
 * against the memory limit while the derived relations the commit is about
 * to discard still did.  Its identifier tables hash with the context's seed,
 * since the text is untrusted.
 */

struct relic_staged_set {
	relic_dom_t	*tuples;	/* packed at the arity */
	uint64_t	*hashes;	/* by tuple: as a relation hashes it */
	bool		*marks;		/* by tuple: new to the base store */
	relic_idtab_t	 index;		/* tuple position + 1 */
	const relic_relation_t *store;	/* base store as staging began */
	size_t		 count;
	size_t		 column_capacity;
	size_t		 hash_capacity;
	size_t		 mark_capacity;
	size_t		 new_count;
	uint64_t	 hash_seed;
	relic_pred_id_t	 pred;
	uint16_t	 arity;
};

static int	 admit_new_tuple(relic_staging_t *staging,
		    const relic_staged_set_t *set);
static int	 collect_growths(relic_staging_t *staging,
		    relic_base_growth_t **growthsp, size_t *countp);
static void	 commit_append(relic_staging_t *staging);
static relic_pred_id_t first_growing(const relic_staging_t *staging,
		    const relic_staged_set_t *set);
static bool	 held_by_store(const relic_staged_set_t *set,
		    const relic_dom_t *tuple, uint64_t hash);
static size_t	 row_bytes(const relic_staged_set_t *set);
static int	 set_add(relic_staging_t *staging, relic_staged_set_t *set,
		    const relic_dom_t *tuple);
static uint64_t	 set_hash(const relic_staged_set_t *set,
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
	staging->base_tuples = relic_base_total(ctx);
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
 * having reported why the fact could not be staged: a limit the load can no
 * longer fit, or memory.
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

	if (staging->new_tuples == 0)
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
 * Whether the base store of `set`'s predicate, which a predicate lacks
 * until it holds a fact, holds `tuple`, of wide hash `hash`, already.
 */
static bool
held_by_store(const relic_staged_set_t *set, const relic_dom_t *tuple,
    uint64_t hash)
{
	if (set->store == NULL)
		return (false);

	return (relic_relation_contains_hashed(set->store, tuple, hash));
}

/*
 * Check that one more new tuple of `set` leaves the load able to fit both
 * limits: the base stores' facts and the new ones within the tuple limit,
 * and the floor under what the commit will hold within the memory limit.
 * Returns 0, or -1 having reported the refusal.
 */
static int
admit_new_tuple(relic_staging_t *staging, const relic_staged_set_t *set)
{
	const relic_account_t *account;
	relic_t *ctx;
	size_t floor_bytes;
	size_t max_tuples;

	ctx = staging->ctx;
	account = &ctx->account;
	max_tuples = ctx->config.max_tuples;

	if (relic_limit_set(max_tuples) &&
	    (staging->base_tuples > max_tuples ||
	    staging->new_tuples >= max_tuples - staging->base_tuples))
		return (relic_bases_refuse(ctx, "tuples", max_tuples,
		    first_growing(staging, set)));

	floor_bytes = relic_saturating_sum(
	    relic_saturating_sum(account->value_bytes, account->pool_bytes),
	    relic_saturating_sum(staging->columns_floor, row_bytes(set)));
	if (relic_limit_set(account->limit) && floor_bytes > account->limit)
		return (relic_bases_refuse(ctx, "memory", account->limit,
		    first_growing(staging, set)));

	return (0);
}

/*
 * The predicate a refusal names: the first, in the order the load names
 * them, whose store the facts staged so far, with one more new tuple of
 * `set`, would grow.
 */
static relic_pred_id_t
first_growing(const relic_staging_t *staging, const relic_staged_set_t *set)
{
	size_t i;

	for (i = 0; i < staging->set_count; i++) {
		if (staging->sets[i].new_count > 0 || &staging->sets[i] == set)
			return (staging->sets[i].pred);
	}

	return (set->pred);
}

/* The bytes one tuple of `set`'s predicate takes in its store's columns. */
static size_t
row_bytes(const relic_staged_set_t *set)
{
	return ((size_t)set->arity * sizeof(relic_dom_t));
}

/*
 * The growth of each base store that gains a tuple, in the order the load
 * first named their predicates, into a new array in *growthsp: its new
 * tuples by the stripe of its canonical index each falls in.
 * Returns 0, or -1 when memory ran out, having reported nothing.
 */
static int
collect_growths(relic_staging_t *staging, relic_base_growth_t **growthsp,
    size_t *countp)
{
	const relic_staged_set_t *set;
	relic_base_growth_t *growths;
	relic_tuple_growth_t *added;
	size_t count;
	size_t i;
	size_t t;

	growths = reallocarray(NULL, staging->set_count, sizeof(*growths));
	if (growths == NULL)
		return (-1);

	count = 0;
	for (i = 0; i < staging->set_count; i++) {
		set = &staging->sets[i];
		if (set->new_count == 0)
			continue;

		growths[count].pred = set->pred;
		added = &growths[count].added;
		bzero(added, sizeof(*added));
		for (t = 0; t < set->count; t++) {
			if (set->marks[t])
				added->by_stripe[relic_hash_stripe(
				    set->hashes[t])]++;
		}
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
	set->store = relic_base_store(staging->ctx, pred);
	if (set->store != NULL)
		staging->columns_floor = relic_saturating_sum(
		    staging->columns_floor, relic_saturating_product(
		    set->store->count, row_bytes(set)));

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
 * Add a tuple to a set unless the set holds it, marked new or not to its
 * base store.  A new tuple is admitted by the limits first.  Room in the
 * table, the columns, the hashes and the marks is made before the tuple is
 * stored, so a failure leaves the set and the staging's counts as they
 * were.  Returns 0, or -1 having reported that the load no longer fits a
 * limit or that the set cannot grow: past the identifiers a set numbers its
 * tuples with, or out of memory.
 */
static int
set_add(relic_staging_t *staging, relic_staged_set_t *set,
    const relic_dom_t *tuple)
{
	char described[RELIC_PRED_TEXT_MAX];
	relic_idprobe_t probe;
	relic_dom_t *tuples;
	uint64_t *hashes;
	bool *marks;
	uint64_t hash;
	uint32_t id;
	uint16_t column;
	bool is_new;

	hash = set_hash(set, tuple);
	relic_idprobe_open(&probe, &set->index, relic_hash_low(hash));
	while (relic_idprobe_next(&probe, &id)) {
		if (set->arity == 0 || bcmp(set_tuple(set, id - 1), tuple,
		    set->arity * sizeof(*tuple)) == 0)
			return (0);
	}

	is_new = !held_by_store(set, tuple, hash);
	if (is_new && admit_new_tuple(staging, set) != 0)
		return (-1);

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
	set->marks[set->count] = is_new;
	set->count++;
	relic_idtab_place(&set->index, relic_hash_low(hash),
	    (uint32_t)set->count);

	if (is_new) {
		set->new_count++;
		staging->new_tuples++;
		staging->columns_floor = relic_saturating_sum(
		    staging->columns_floor, row_bytes(set));
	}

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
 * A tuple's wide hash as every relation of the context computes it, which is
 * what lets the commit hand it to the base store.
 */
static uint64_t
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

	return (relic_hash_low(set->hashes[id - 1]));
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
