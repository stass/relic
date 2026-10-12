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
 * the load first names them, even when the first fact named is one the
 * store holds already, so that a refusal can name predicates in that order.
 * Each set holds a builder (relation.h) of the facts new to the base store,
 * in the order the load first states them, packed at the predicate's arity
 * and indexed as the store's own tuples are: the payload an empty store
 * adopts at the commit as it stands.  A fact the store or the builder holds
 * already is not staged again.  The hash every relation of the context gives
 * a tuple is computed once as the fact is staged, and serves the builder's
 * lookup, the store's and the builder's insertion alike; the builder keeps
 * no hashes, so its growth and an append to a populated store compute them
 * again from the columns.
 *
 * Nothing can change a base store while a load runs, so a fact found new
 * stays new until the commit.  A new tuple is staged only if the load, with
 * it, can still fit the limits: the base stores' facts plus the new ones
 * must fit the tuple limit, and a floor under what the commit will hold
 * must fit the memory limit.  So a load the limits refuse is parsed no
 * further than the fact that shows it, rather than staging and interning
 * the rest of its input first.
 *
 * The floor, `columns_floor`, is the bytes the columns of the loaded
 * predicates' base stores must hold once every new tuple is in them, each
 * store's count plus its new tuples at its arity, to which the value table
 * and the symbol pool are added.  Those two only grow while a load runs, and
 * the commit discards the derived relations before it reserves, so for any
 * growth the stores can hold the floor never exceeds what the commit's own
 * check measures: staging refuses no load the commit would admit.  A growth
 * past what a store can hold the reservation refuses in any case.  The
 * commit measures exactly, and can still refuse what the floor let
 * through.
 *
 * When the whole load has been read, the commit gives each base store its
 * new tuples in four steps, ordered so that every step that can fail comes
 * before the first that changes a base store's contents:
 *
 *	prepare: describe what each store gains, in the order the load first
 *	named the predicates: a store holding no fact takes its builder's
 *	payload whole, and one holding facts has the builder's tuples
 *	appended, their hashes computed here.  A failure here changes
 *	nothing.
 *
 *	discard: when any tuple is new the program is about to change, so what
 *	was derived from it is discarded.  That cannot fail, and it leaves the
 *	store holding the base stores alone, so the limits of the next step
 *	count the program's facts and nothing derived.
 *
 *	reserve: make every store ready for what it gains, both limits checked
 *	once over all of them before any store changes.
 *
 *	apply: give each store its tuples, in the order they were staged.
 *	Room is made, and every tuple is one its store lacks, so nothing looks
 *	anything up and nothing can fail.
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
 * admit, and what the commit does not adopt is released when the load
 * returns.  Charged, it would count against the memory limit while the
 * derived relations the commit is about to discard still did.  An adopted
 * payload is charged once, as the store's, by the commit that adopts it.
 * Builders hash with the context's seed, since the text is untrusted.
 */

struct relic_staged_set {
	relic_builder_t	 builder;	/* the facts new to the store */
	const relic_relation_t *store;	/* base store as staging began */
};

static int	 admit_new_tuple(relic_staging_t *staging,
		    const relic_staged_set_t *set);
static relic_pred_id_t first_growing(const relic_staging_t *staging,
		    const relic_staged_set_t *set);
static bool	 held_by_store(const relic_staged_set_t *set,
		    const relic_dom_t *tuple, uint64_t hash);
static int	 prepare_actions(relic_staging_t *staging,
		    relic_base_action_t **actionsp, size_t *countp);
static void	 release_actions(relic_base_action_t *actions, size_t count);
static size_t	 row_bytes(const relic_staged_set_t *set);
static int	 set_add(relic_staging_t *staging, relic_staged_set_t *set,
		    const relic_dom_t *tuple);
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
 * it failed to parse, was refused by the commit or committed.  A builder
 * whose payload a store adopted holds nothing by then.
 */
void
relic_staging_reset(relic_staging_t *staging)
{
	size_t i;

	for (i = 0; i < staging->set_count; i++)
		relic_builder_reset(&staging->sets[i].builder);

	free(staging->sets);
	relic_idtab_reset(&staging->by_pred);
	bzero(staging, sizeof(*staging));
}

/*
 * Stage the fact `pred(tuple...)`, `tuple` holding the predicate's arity of
 * interned values.  A fact already staged, or already held, is not staged
 * again.  Returns 0, or -1 having reported why the fact could not be
 * staged: a limit the load can no longer fit, or memory.
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
	relic_base_action_t *actions;
	size_t count;
	int status;

	if (staging->new_tuples == 0)
		return (0);

	if (prepare_actions(staging, &actions, &count) != 0) {
		release_actions(actions, count);
		relic_diag_no_memory(staging->ctx);
		return (-1);
	}

	relic_discard_derived(staging->ctx);
	status = relic_bases_reserve(staging->ctx, actions, count);
	if (status == 0)
		relic_bases_apply(staging->ctx, actions, count);

	release_actions(actions, count);

	return (status);
}

/*
 * The commit's manifest, into a new array in *actionsp: an action for each
 * set whose builder holds a tuple, in the order the load first named their
 * predicates.  Returns 0, or -1 when memory ran out, having reported
 * nothing.  Whatever this returns, the caller releases the *countp actions
 * with release_actions().
 */
static int
prepare_actions(relic_staging_t *staging, relic_base_action_t **actionsp,
    size_t *countp)
{
	relic_base_action_t *actions;
	relic_builder_t *builder;
	size_t count;
	size_t i;
	int status;

	*actionsp = NULL;
	*countp = 0;

	actions = reallocarray(NULL, staging->set_count, sizeof(*actions));
	if (actions == NULL)
		return (-1);

	count = 0;
	status = 0;
	for (i = 0; i < staging->set_count && status == 0; i++) {
		builder = &staging->sets[i].builder;
		if (relic_builder_count(builder) == 0)
			continue;

		status = relic_base_action_prepare(staging->ctx,
		    &actions[count], builder);
		count++;
	}

	*actionsp = actions;
	*countp = count;

	return (status);
}

static void
release_actions(relic_base_action_t *actions, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		relic_base_action_release(&actions[i]);
	free(actions);
}

/*
 * Whether the base store of `set`'s predicate, which a predicate lacks
 * until something asks for it, holds `tuple`, of wide hash `hash`, already.
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
		if (relic_builder_count(&staging->sets[i].builder) > 0 ||
		    &staging->sets[i] == set)
			return (staging->sets[i].builder.pred);
	}

	return (set->builder.pred);
}

/* The bytes one tuple of `set`'s predicate takes in its store's columns. */
static size_t
row_bytes(const relic_staged_set_t *set)
{
	return ((size_t)set->builder.arity * sizeof(relic_dom_t));
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
	    staging->sets[staging->last].builder.pred == pred) {
		*setp = &staging->sets[staging->last];
		return (0);
	}

	hash = relic_hash_word(staging->ctx->hash_seed, pred);
	relic_idprobe_open(&probe, &staging->by_pred, hash);
	while (relic_idprobe_next(&probe, &id)) {
		if (staging->sets[id - 1].builder.pred == pred) {
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
	relic_builder_init(&set->builder, staging->ctx, pred);
	set->store = relic_base_store(staging->ctx, pred);
	if (set->store != NULL)
		staging->columns_floor = relic_saturating_sum(
		    staging->columns_floor, relic_saturating_product(
		    relic_relation_count(set->store), row_bytes(set)));

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
	    staging->sets[id - 1].builder.pred));
}

/*
 * Stage a tuple of `set`'s predicate unless the builder or the base store
 * holds it already.  A new tuple is admitted by the limits first, and the
 * builder makes room before it stores the tuple, so a failure leaves the
 * set and the staging's counts as they were.  Returns 0, or -1 having
 * reported that the load no longer fits a limit or that the builder cannot
 * grow: past the identifiers a relation numbers its tuples with, or out of
 * memory.
 */
static int
set_add(relic_staging_t *staging, relic_staged_set_t *set,
    const relic_dom_t *tuple)
{
	uint64_t hash;

	hash = relic_tuple_hash(set->builder.hash_seed, tuple,
	    set->builder.arity);
	if (relic_builder_contains_hashed(&set->builder, tuple, hash) ||
	    held_by_store(set, tuple, hash))
		return (0);

	if (admit_new_tuple(staging, set) != 0)
		return (-1);

	if (relic_builder_add_hashed(staging->ctx, &set->builder, tuple,
	    hash) != 0)
		return (-1);

	staging->new_tuples++;
	staging->columns_floor = relic_saturating_sum(staging->columns_floor,
	    row_bytes(set));

	return (0);
}
