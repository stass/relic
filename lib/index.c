/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * Secondary indexes: the tuples of a relation grouped by the values of some of
 * their columns.
 *
 * An index answers "which tuples hold these values in the key columns?" by
 * grouping tuples into one chain per distinct key.  A key lives in the
 * stripe its hash chooses, and the stripe's open-addressing identifier table
 * maps the key's hash to its key identifier, the key's entry records the
 * first and last tuple of its chain, its values are kept beside the entries,
 * and the chain itself is threaded through one `next` identifier per tuple.
 * A lookup probes once for the key, comparing against stored values, and
 * then follows the chain.
 *
 * A tuple is added at the end of its key's chain as it is inserted, so every
 * chain lists its tuples in increasing identifier order, which is insertion
 * order.  That order is what makes a lookup visit exactly what a scan would
 * have matched, in the same order, and what lets a lookup that must not see
 * tuples inserted after it started simply stop at the first identifier past
 * the relation's end as it was then.
 *
 * A lookup of a semi-naive delta, the tuples of one key at or past some
 * identifier, starts from the key's frontier rather than from its first
 * tuple, and so does an existence check of a delta, which reads only the
 * first of those tuples.  The frontier is moved forward as far as each such
 * walk goes, and deltas only move forward, so over a whole fixed point the
 * positioning walks each chain at most once however many rounds, lookups
 * and checks there are.  The evaluator raises an index to a delta's start
 * before any lookup or check starts there, and neither moves that start:
 * each only walks and moves frontiers, toward the one start every walk of
 * the round shares, so the order the walks at one start are made in changes
 * nothing any of them finds.
 *
 * Adding a tuple is four operations, and only the third can fail.
 * relic_index_classify() hashes the tuple's key and probes the key's stripe
 * for it, keeping the hash, what the probe found and the identifier the
 * tuple will take as the index's pending insertion;
 * relic_index_pending_bytes_after() is what the memory limit is asked
 * with; relic_index_reserve_pending() makes room, for the tuple's link and,
 * only when its key is new, for the key; and relic_index_add_pending() links
 * the tuple in, probing nothing again.  The last three read everything they
 * need from the pending insertion, so none can be handed a tuple other than
 * the one classified.  The relation store classifies and
 * reserves in every index before it stores anything, so that no failure
 * leaves a tuple the relation holds missing from an index, which would be a
 * lookup silently missing an answer.
 *
 * Nothing here knows about limits or the context.  The relation store
 * measures an index before and after a reservation and does the accounting.
 */

typedef struct relic_index_extent relic_index_extent_t;
typedef struct relic_index_rehash relic_index_rehash_t;
typedef struct relic_key_ref relic_key_ref_t;

/*
 * The sizes a stripe's storage could have: its key table's bytes, and the
 * capacities of its arrays -- entries, frontiers and key values, each
 * counted in keys.  A capacity of zero is a growth that would be refused.
 */
struct relic_index_extent {
	size_t	table_bytes;
	size_t	entries;
	size_t	frontiers;
	size_t	key_values;
};

/*
 * What a stripe's key table needs in order to recompute the hash of a key it
 * holds: the index, for the key's length, the stripe, which holds the key's
 * values, and the relation, whose seed the hash is keyed by.
 */
struct relic_index_rehash {
	const relic_index_t		*index;
	const relic_index_stripe_t	*stripe;
	const relic_relation_t		*relation;
};

/*
 * Where a key belongs in an index: its wide hash, the stripe the hash
 * chooses, and its identifier there, RELIC_ID_NONE when the index holds no
 * such key.
 */
struct relic_key_ref {
	const relic_index_stripe_t	*stripe;
	uint64_t			 hash;
	uint32_t			 key_id;
};

static const relic_dom_t *appended_tuple(const relic_append_t *append,
			    const relic_relation_t *relation, size_t position);
static void		 chain_link(relic_index_t *index,
			    relic_index_stripe_t *stripe, uint32_t key_id,
			    relic_tuple_id_t id);
static void		 chain_start(relic_index_t *index,
			    relic_index_stripe_t *stripe,
			    const relic_relation_t *relation,
			    relic_tuple_id_t id, uint64_t hash);
static size_t		 extent_bytes(const relic_index_t *index,
			    const relic_index_extent_t *extent);
static uint32_t		 find_key(const relic_index_t *index,
			    const relic_index_stripe_t *stripe,
			    const relic_dom_t *key, uint64_t hash);
static void		 gather_key(const relic_index_t *index,
			    const relic_dom_t *tuple, relic_dom_t *key);
static uint64_t		 key_hash(const relic_index_t *index,
			    const relic_relation_t *relation,
			    const relic_dom_t *key);
static size_t		 key_offset(const relic_index_t *index,
			    uint32_t key_id);
static uint32_t		 key_rehash(uint32_t key_id, void *ctx);
static const relic_index_stripe_t *key_stripe(const relic_index_t *index,
			    uint64_t hash);
static const relic_dom_t *key_values_of(const relic_index_t *index,
			    const relic_index_stripe_t *stripe,
			    uint32_t key_id);
static size_t		 number_new_key(const relic_index_t *index,
			    const relic_relation_t *relation,
			    const relic_append_t *append,
			    relic_new_keys_t *new_keys, const relic_dom_t *key,
			    uint64_t hash, size_t position, size_t *addedp);
static relic_key_ref_t	 probe_key(const relic_index_t *index,
			    const relic_relation_t *relation,
			    const relic_dom_t *probe);
static size_t		 stripe_bytes(const relic_index_t *index,
			    const relic_index_stripe_t *stripe);
static size_t		 stripe_bytes_after_new_keys(
			    const relic_index_t *index,
			    const relic_index_stripe_t *stripe, size_t added);
static void		 stripe_free(relic_index_stripe_t *stripe);
static int		 stripe_reserve_key(const relic_index_t *index,
			    relic_index_stripe_t *stripe,
			    const relic_relation_t *relation);
static int		 stripe_reserve_keys(const relic_index_t *index,
			    relic_index_stripe_t *stripe, size_t added,
			    relic_idtab_t *grown);
static relic_tuple_id_t	 walk_toward(const relic_index_t *index,
			    relic_tuple_id_t id, relic_tuple_id_t since);
static relic_index_stripe_t *writable_key_stripe(relic_index_t *index,
			    uint64_t hash);

/*
 * An empty index keyed on the columns of `key_mask`, which lists them in
 * `key_column` once so that gathering a key visits its columns and no
 * others.  The relation store keys an index on at least one column, so a
 * key has values to store.  The key tables are tagged: a probe that misses
 * would otherwise compare against each key it passes.
 */
void
relic_index_init(relic_index_t *index, uint32_t key_mask)
{
	size_t s;
	uint16_t column;

	bzero(index, sizeof(*index));
	for (s = 0; s < RELIC_STRIPES; s++)
		relic_idtab_init(&index->stripes[s].keys, true);
	index->key_mask = key_mask;

	for (column = 0; column < RELIC_MAX_ARITY; column++) {
		if ((key_mask & (UINT32_C(1) << column)) != 0) {
			index->key_column[index->key_length] = column;
			index->key_length++;
		}
	}
	assert(index->key_length > 0);
}

/*
 * Release everything the index holds.  What is left is zeroed, not an index:
 * relic_index_init() must set it up again before it is used.
 */
void
relic_index_free(relic_index_t *index)
{
	size_t s;

	for (s = 0; s < RELIC_STRIPES; s++)
		stripe_free(&index->stripes[s]);
	free(index->next);
	bzero(index, sizeof(*index));
}

static void
stripe_free(relic_index_stripe_t *stripe)
{
	relic_idtab_reset(&stripe->keys);
	free(stripe->entries);
	free(stripe->frontiers);
	free(stripe->key_values);
}

/*
 * Copy a tuple's key columns, in ascending column order, into `key`, which
 * has room for `key_length` values.
 */
static void
gather_key(const relic_index_t *index, const relic_dom_t *tuple,
    relic_dom_t *key)
{
	uint16_t i;

	for (i = 0; i < index->key_length; i++)
		key[i] = tuple[index->key_column[i]];
}

/*
 * The wide hash of a key's values, keyed by the seed of the relation the
 * index belongs to.
 */
static uint64_t
key_hash(const relic_index_t *index, const relic_relation_t *relation,
    const relic_dom_t *key)
{
	return (relic_hash_words_wide(relation->hash_seed, key,
	    index->key_length));
}

/*
 * Where key `key_id`'s values begin in its stripe's `key_values`: keys are
 * numbered from one, and each holds `key_length` values.
 */
static size_t
key_offset(const relic_index_t *index, uint32_t key_id)
{
	return ((size_t)(key_id - 1) * index->key_length);
}

/*
 * The values of key `key_id` of a stripe, `key_length` of them.
 */
static const relic_dom_t *
key_values_of(const relic_index_t *index, const relic_index_stripe_t *stripe,
    uint32_t key_id)
{
	return (&stripe->key_values[key_offset(index, key_id)]);
}

/*
 * The hash of a key a stripe's table holds, for the table to use when it
 * grows.
 */
static uint32_t
key_rehash(uint32_t key_id, void *ctx)
{
	const relic_index_rehash_t *rehash;

	rehash = ctx;

	return (relic_hash_words(rehash->relation->hash_seed,
	    key_values_of(rehash->index, rehash->stripe, key_id),
	    rehash->index->key_length));
}

/*
 * The identifier within `stripe` of the key holding these values, whose wide
 * hash is `hash`, or RELIC_ID_NONE when no tuple of the relation holds them.
 */
static uint32_t
find_key(const relic_index_t *index, const relic_index_stripe_t *stripe,
    const relic_dom_t *key, uint64_t hash)
{
	relic_idprobe_t probe;
	size_t key_bytes;
	uint32_t key_id;

	key_bytes = (size_t)index->key_length * sizeof(*key);

	relic_idprobe_open(&probe, &stripe->keys, relic_hash_low(hash));
	while (relic_idprobe_next(&probe, &key_id)) {
		if (bcmp(key_values_of(index, stripe, key_id), key,
		    key_bytes) == 0)
			return (key_id);
	}

	return (RELIC_ID_NONE);
}

size_t
relic_index_bytes(const relic_index_t *index)
{
	size_t bytes;
	size_t s;

	bytes = index->next_capacity * sizeof(*index->next);
	for (s = 0; s < RELIC_STRIPES; s++)
		bytes += stripe_bytes(index, &index->stripes[s]);

	return (bytes);
}

/*
 * The bytes a stripe's key table and arrays hold.
 */
static size_t
stripe_bytes(const relic_index_t *index, const relic_index_stripe_t *stripe)
{
	return (relic_idtab_bytes(&stripe->keys) +
	    stripe->entry_capacity * sizeof(*stripe->entries) +
	    stripe->frontier_capacity * sizeof(*stripe->frontiers) +
	    stripe->key_value_capacity * index->key_length *
	    sizeof(*stripe->key_values));
}

/*
 * The bytes a stripe would hold at `extent`.  SIZE_MAX says that is more
 * than a size_t holds, or that a capacity is zero, which is a growth that
 * would be refused and which no size describes.
 */
static size_t
extent_bytes(const relic_index_t *index, const relic_index_extent_t *extent)
{
	size_t key_bytes;
	size_t total;

	if (extent->entries == 0 || extent->frontiers == 0 ||
	    extent->key_values == 0)
		return (SIZE_MAX);

	key_bytes = (size_t)index->key_length *
	    sizeof(*index->stripes[0].key_values);

	total = extent->table_bytes;
	total = relic_saturating_sum(total, relic_saturating_product(
	    extent->entries, sizeof(*index->stripes[0].entries)));
	total = relic_saturating_sum(total, relic_saturating_product(
	    extent->frontiers, sizeof(*index->stripes[0].frontiers)));
	total = relic_saturating_sum(total, relic_saturating_product(
	    extent->key_values, key_bytes));

	return (total);
}

/*
 * Make room in a stripe for one more key, growing its table at once.
 * Returns 0, or -1 when something could not grow; whatever was obtained is
 * kept, and the caller accounts for it by measuring.
 */
static int
stripe_reserve_key(const relic_index_t *index, relic_index_stripe_t *stripe,
    const relic_relation_t *relation)
{
	relic_index_rehash_t rehash;
	relic_idtab_t grown;
	int status;

	status = stripe_reserve_keys(index, stripe, 1, &grown);

	rehash.index = index;
	rehash.stripe = stripe;
	rehash.relation = relation;
	relic_idtab_complete_growth(&stripe->keys, &grown, key_rehash, &rehash);

	return (status);
}

/*
 * Classify the tuple the relation store is about to insert as tuple `id`:
 * hash its key and find the key in the stripe the hash chooses, keeping the
 * hash, the key's identifier and `id` as the index's pending insertion for
 * the three steps that follow.  Nothing changes but `pending`.
 */
void
relic_index_classify(relic_index_t *index, const relic_relation_t *relation,
    const relic_dom_t *tuple, relic_tuple_id_t id)
{
	relic_key_ref_t ref;

	ref = probe_key(index, relation, tuple);
	index->pending.hash = ref.hash;
	index->pending.key_id = ref.key_id;
	index->pending.tuple = id;
}

/*
 * The stripe a key of wide hash `hash` lives in, read-only and writable.
 */
static const relic_index_stripe_t *
key_stripe(const relic_index_t *index, uint64_t hash)
{
	return (&index->stripes[relic_hash_stripe(hash)]);
}

static relic_index_stripe_t *
writable_key_stripe(relic_index_t *index, uint64_t hash)
{
	return (&index->stripes[relic_hash_stripe(hash)]);
}

/*
 * The bytes of the parts of the index the pending insertion can grow: the
 * chain links, and the stripe its key lives in.  The relation store measures
 * these before and after it reserves, and asks the memory limit with them,
 * so that an insertion costs a look at the parts it touches rather than at
 * every stripe.
 */
size_t
relic_index_pending_bytes(const relic_index_t *index)
{
	return (index->next_capacity * sizeof(*index->next) +
	    stripe_bytes(index, key_stripe(index, index->pending.hash)));
}

/*
 * The same parts' bytes once relic_index_reserve_pending() had made room
 * for the pending tuple and, when it is new, its key.  A key the index
 * holds needs no room.
 *
 * A growth that could not happen at all is reported as the bytes the parts
 * already hold, so that the limit admits it and the reservation reports the
 * real obstacle, as the relation store does for its own arrays.
 */
size_t
relic_index_pending_bytes_after(const relic_index_t *index)
{
	const relic_index_stripe_t *stripe;
	size_t next_capacity;
	size_t stripe_after;
	size_t bytes;

	next_capacity = relic_grow_array_capacity(index->next_capacity,
	    index->pending.tuple);
	if (next_capacity == 0)
		return (relic_index_pending_bytes(index));

	stripe = key_stripe(index, index->pending.hash);
	if (index->pending.key_id != RELIC_ID_NONE)
		stripe_after = stripe_bytes(index, stripe);
	else
		stripe_after = stripe_bytes_after_new_keys(index, stripe, 1);
	if (stripe_after == SIZE_MAX)
		return (relic_index_pending_bytes(index));

	bytes = relic_saturating_sum(relic_saturating_product(next_capacity,
	    sizeof(*index->next)), stripe_after);
	if (bytes == SIZE_MAX)
		return (relic_index_pending_bytes(index));

	return (bytes);
}

/*
 * Make room for the pending tuple's link and, when its key is new, for the
 * key in its stripe.  Returns 0, or -1 when an array could not grow;
 * whatever the steps before it obtained is kept, and the caller accounts for
 * it by measuring.
 */
int
relic_index_reserve_pending(relic_index_t *index,
    const relic_relation_t *relation)
{
	relic_tuple_id_t *next;

	next = relic_grow_array(index->next, &index->next_capacity,
	    index->pending.tuple, sizeof(*index->next));
	if (next == NULL)
		return (-1);
	index->next = next;

	if (index->pending.key_id != RELIC_ID_NONE)
		return (0);

	return (stripe_reserve_key(index,
	    writable_key_stripe(index, index->pending.hash), relation));
}

/*
 * Link the pending tuple, which the relation now holds, at the end of its
 * key's chain, starting a chain when its key is new.  The index must have
 * room, which relic_index_reserve_pending() guarantees, and nothing here can
 * fail.
 *
 * The reservation between the classification and this may have moved the
 * stripe's arrays and rehashed its table.  The pending insertion survives
 * that because it records identifiers and a hash, not positions: the
 * stripe's arrays are found afresh here and a new key is placed by a fresh
 * probe.
 *
 * Tuples must be added in increasing identifier order, which both insertion
 * and building an index do; that is what keeps every chain in identifier
 * order.
 */
void
relic_index_add_pending(relic_index_t *index,
    const relic_relation_t *relation)
{
	relic_index_stripe_t *stripe;
	relic_tuple_id_t id;

	id = index->pending.tuple;
	assert(id != RELIC_ID_NONE && id <= relic_relation_count(relation));

	stripe = writable_key_stripe(index, index->pending.hash);
	if (index->pending.key_id != RELIC_ID_NONE)
		chain_link(index, stripe, index->pending.key_id, id);
	else
		chain_start(index, stripe, relation, id, index->pending.hash);
}

/*
 * Link tuple `id` at the end of the chain of key `key_id` of `stripe`,
 * which it holds.  The slot of the chain's last tuple in `next` belongs to
 * the chain, and the new tuple's slot to the new tuple, so linking touches
 * no word another key's chain uses.
 */
static void
chain_link(relic_index_t *index, relic_index_stripe_t *stripe,
    uint32_t key_id, relic_tuple_id_t id)
{
	relic_index_key_t *entry;

	entry = &stripe->entries[key_id - 1];
	index->next[id - 1] = RELIC_ID_NONE;
	index->next[entry->last - 1] = id;
	entry->last = id;
}

/*
 * Start the chain of a key new to `stripe`, whose wide hash is `hash`, with
 * tuple `id`, which the relation holds and which holds the key.  The stripe
 * must have room for one more key.
 *
 * The key's identifier is one past the stripe's last, so it can never be
 * RELIC_ID_NONE, and it cannot overflow: a stripe has no more keys than the
 * relation has tuples.  Its values are stored before it is placed, so that
 * every key the table holds has them.
 */
static void
chain_start(relic_index_t *index, relic_index_stripe_t *stripe,
    const relic_relation_t *relation, relic_tuple_id_t id, uint64_t hash)
{
	relic_index_key_t *entry;
	uint32_t key_id;

	stripe->key_count++;
	key_id = (uint32_t)stripe->key_count;
	entry = &stripe->entries[key_id - 1];
	entry->first = id;
	entry->last = id;
	relic_atomic_u32_store_relaxed(&stripe->frontiers[key_id - 1], id);
	index->next[id - 1] = RELIC_ID_NONE;
	gather_key(index, relic_relation_tuple(relation, id),
	    &stripe->key_values[key_offset(index, key_id)]);
	relic_idtab_place(&stripe->keys, relic_hash_low(hash), key_id);
}

/*
 * Record, for the tuple at `position` of an append, which is `tuple`, its
 * key's wide hash and the stripe that chooses.
 */
void
relic_index_stage(const relic_index_t *index,
    const relic_relation_t *relation, relic_append_keys_t *keys,
    size_t position, const relic_dom_t *tuple)
{
	relic_dom_t key[RELIC_MAX_ARITY];
	uint64_t hash;

	gather_key(index, tuple, key);
	hash = key_hash(index, relation, key);
	keys->hashes[position] = hash;
	keys->stripes[position] = (uint8_t)relic_hash_stripe(hash);
}

/*
 * Classify, in identifier order, the tuples of an append whose keys lie in
 * stripe `s`: each joins the key the stripe holds with its values, or, when
 * the stripe holds none, a key new to it, numbered on from the stripe's
 * last in the order the tuples first bring the new keys, as insertions one
 * at a time would number them.  Records each tuple's key and the stripe's
 * count of new keys in `keys`.
 *
 * The index is only read.  `new_keys` finds the new keys already met, and
 * has room for every tuple of the stripe.
 */
void
relic_index_classify_append(const relic_index_t *index,
    const relic_relation_t *relation, const relic_append_t *append,
    relic_append_keys_t *keys, size_t s, relic_new_keys_t *new_keys)
{
	const relic_index_stripe_t *stripe;
	relic_dom_t key[RELIC_MAX_ARITY];
	uint32_t key_id;
	size_t added;
	size_t i;

	stripe = &index->stripes[s];
	added = 0;

	for (i = 0; i < append->count; i++) {
		if (keys->stripes[i] != s)
			continue;

		gather_key(index, appended_tuple(append, relation, i), key);
		key_id = find_key(index, stripe, key, keys->hashes[i]);
		if (key_id == RELIC_ID_NONE)
			key_id = (uint32_t)(stripe->key_count + 1 +
			    number_new_key(index, relation, append, new_keys,
			    key, keys->hashes[i], i, &added));
		keys->key_ids[i] = key_id;
	}

	keys->new_keys[s] = added;
}

/*
 * The columns of the tuple at `position` of an append to `relation`.
 */
static const relic_dom_t *
appended_tuple(const relic_append_t *append, const relic_relation_t *relation,
    size_t position)
{
	return (&append->columns[position * relic_relation_arity(relation)]);
}

/*
 * The position among a stripe's new keys of the key `key`, of wide hash
 * `hash`, which the stripe does not hold: the position of the new key
 * already met with its values, or the next, *addedp, for the tuple at
 * `position` of the append, which is then the key's first.
 */
static size_t
number_new_key(const relic_index_t *index, const relic_relation_t *relation,
    const relic_append_t *append, relic_new_keys_t *new_keys,
    const relic_dom_t *key, uint64_t hash, size_t position, size_t *addedp)
{
	relic_dom_t met[RELIC_MAX_ARITY];
	relic_idprobe_t probe;
	size_t key_bytes;
	size_t ordinal;
	uint32_t met_plus_one;

	key_bytes = (size_t)index->key_length * sizeof(*key);

	relic_idprobe_open(&probe, &new_keys->table, relic_hash_low(hash));
	while (relic_idprobe_next(&probe, &met_plus_one)) {
		gather_key(index, appended_tuple(append, relation,
		    new_keys->first[met_plus_one - 1]), met);
		if (bcmp(met, key, key_bytes) == 0)
			return (met_plus_one - 1);
	}

	ordinal = *addedp;
	(*addedp)++;
	new_keys->first[ordinal] = (uint32_t)position;
	relic_idtab_place(&new_keys->table, relic_hash_low(hash),
	    (uint32_t)(ordinal + 1));

	return (ordinal);
}

/*
 * The bytes the index would hold once an append to its relation, which
 * holds `tuples`, had been linked in, with `keys` classified: exactly what
 * linking the append's tuples one insertion at a time would reach, since
 * each array grows a step at a time to a capacity that depends only on what
 * it comes to hold, and a table doubles.  SIZE_MAX when some growth could
 * not happen.
 */
size_t
relic_index_bytes_after_append(const relic_index_t *index, size_t tuples,
    const relic_append_t *append, const relic_append_keys_t *keys)
{
	size_t next_capacity;
	size_t total;
	size_t s;

	next_capacity = relic_grow_array_capacity_stepwise(index->next_capacity,
	    tuples, append->count, 1);
	if (next_capacity == 0)
		return (SIZE_MAX);

	total = relic_saturating_product(next_capacity, sizeof(*index->next));
	for (s = 0; s < RELIC_STRIPES; s++)
		total = relic_saturating_sum(total, stripe_bytes_after_new_keys(
		    index, &index->stripes[s], keys->new_keys[s]));

	return (total);
}

/*
 * The bytes of the key tables relic_index_reserve_append() replaces for an
 * append with `keys` classified: the table of each stripe that grows, which
 * stays allocated beside the table grown for it until the stripe is linked.
 */
size_t
relic_index_bytes_replaced_by_append(const relic_index_t *index,
    const relic_append_keys_t *keys)
{
	size_t replaced;
	size_t total;
	size_t s;

	total = 0;
	for (s = 0; s < RELIC_STRIPES; s++) {
		replaced = relic_idtab_bytes_replaced_by_reserve(
		    &index->stripes[s].keys, keys->new_keys[s]);
		total = relic_saturating_sum(total, replaced);
	}

	return (total);
}

/*
 * The bytes a stripe would hold once grown a key at a time for `added`
 * new keys, or SIZE_MAX when that growth could not happen.
 */
static size_t
stripe_bytes_after_new_keys(const relic_index_t *index,
    const relic_index_stripe_t *stripe, size_t added)
{
	relic_index_extent_t extent;

	if (added == 0)
		return (stripe_bytes(index, stripe));

	extent.table_bytes = relic_idtab_bytes_after_reserve(&stripe->keys,
	    added);
	extent.entries = relic_grow_array_capacity_stepwise(
	    stripe->entry_capacity, stripe->key_count, added, 1);
	extent.frontiers = relic_grow_array_capacity_stepwise(
	    stripe->frontier_capacity, stripe->key_count, added, 1);
	extent.key_values = relic_grow_array_capacity_stepwise(
	    stripe->key_value_capacity, stripe->key_count, added, 1);

	return (extent_bytes(index, &extent));
}

/*
 * Make room for an append to the index's relation, which holds `tuples`,
 * with `keys` classified: the chain links of its tuples, and in each stripe
 * its new keys, growing every array to exactly the capacity one insertion
 * at a time would reach and preparing each stripe's key table in
 * `keys->grown`, allocated and not yet filled.  Returns 0, or -1 when
 * something could not grow; whatever was obtained is kept, the arrays in
 * the index and the prepared tables in `keys`, for the caller to account
 * and release.
 */
int
relic_index_reserve_append(relic_index_t *index, size_t tuples,
    const relic_append_t *append, relic_append_keys_t *keys)
{
	relic_tuple_id_t *next;
	size_t s;

	if (append->count != 0) {
		next = relic_grow_array_stepwise(index->next,
		    &index->next_capacity, tuples, append->count, 1,
		    sizeof(*index->next));
		if (next == NULL)
			return (-1);
		index->next = next;
	}

	for (s = 0; s < RELIC_STRIPES; s++) {
		if (keys->new_keys[s] == 0)
			continue;
		if (stripe_reserve_keys(index, &index->stripes[s],
		    keys->new_keys[s], &keys->grown[s]) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Make room in a stripe for `added` new keys, at least one: its arrays
 * grown as a key at a time would grow them, and its table's growth
 * prepared in `grown`.
 */
static int
stripe_reserve_keys(const relic_index_t *index, relic_index_stripe_t *stripe,
    size_t added, relic_idtab_t *grown)
{
	relic_index_key_t *entries;
	relic_atomic_u32_t *frontiers;
	relic_dom_t *values;

	if (relic_idtab_prepare_growth(&stripe->keys, added, grown) != 0)
		return (-1);

	entries = relic_grow_array_stepwise(stripe->entries,
	    &stripe->entry_capacity, stripe->key_count, added, 1,
	    sizeof(*stripe->entries));
	if (entries == NULL)
		return (-1);
	stripe->entries = entries;

	frontiers = relic_grow_array_stepwise(stripe->frontiers,
	    &stripe->frontier_capacity, stripe->key_count, added, 1,
	    sizeof(*stripe->frontiers));
	if (frontiers == NULL)
		return (-1);
	stripe->frontiers = frontiers;

	values = relic_grow_array_stepwise(stripe->key_values,
	    &stripe->key_value_capacity, stripe->key_count, added, 1,
	    (size_t)index->key_length * sizeof(*stripe->key_values));
	if (values == NULL)
		return (-1);
	stripe->key_values = values;

	return (0);
}

/*
 * Link the tuples of an append whose keys lie in stripe `s` into the index,
 * in identifier order, as relic_index_classify_append() classified them:
 * the stripe's key table is first grown into the table the reservation
 * prepared, and then each tuple joins its key's chain or starts the chain
 * of a new key.  The relation holds the append's tuples, and the index has
 * room for them, which relic_index_reserve_append() made.  Nothing here can
 * fail, and nothing here touches another stripe or a `next` slot another
 * stripe's tuples own.
 */
void
relic_index_link_append(relic_index_t *index,
    const relic_relation_t *relation, const relic_append_t *append,
    relic_append_keys_t *keys, size_t s)
{
	relic_index_rehash_t rehash;
	relic_index_stripe_t *stripe;
	relic_tuple_id_t id;
	uint32_t key_id;
	size_t i;

	stripe = &index->stripes[s];
	rehash.index = index;
	rehash.stripe = stripe;
	rehash.relation = relation;
	relic_idtab_complete_growth(&stripe->keys, &keys->grown[s], key_rehash,
	    &rehash);

	for (i = 0; i < append->count; i++) {
		if (keys->stripes[i] != s)
			continue;

		id = (relic_tuple_id_t)(append->first + i);
		key_id = keys->key_ids[i];
		if (key_id <= stripe->key_count) {
			chain_link(index, stripe, key_id, id);
		} else {
			assert(key_id == stripe->key_count + 1);
			chain_start(index, stripe, relation, id,
			    keys->hashes[i]);
		}
	}
}

/*
 * The key `probe` holds in the key columns: its hash, its stripe, and its
 * identifier there, RELIC_ID_NONE when no tuple of the relation holds it.
 * `probe` is laid out as a tuple of the relation, and only its key columns
 * are read, so a caller never needs to know how the index packs a key.
 */
static relic_key_ref_t
probe_key(const relic_index_t *index, const relic_relation_t *relation,
    const relic_dom_t *probe)
{
	relic_dom_t key[RELIC_MAX_ARITY];
	relic_key_ref_t ref;

	gather_key(index, probe, key);
	ref.hash = key_hash(index, relation, key);
	ref.stripe = key_stripe(index, ref.hash);
	ref.key_id = find_key(index, ref.stripe, key, ref.hash);

	return (ref);
}

uint32_t
relic_index_key_mask(const relic_index_t *index)
{
	return (index->key_mask);
}

/*
 * The first tuple, in identifier order, that agrees with `probe` on the key
 * columns, or RELIC_ID_NONE when none does.
 */
relic_tuple_id_t
relic_index_first(const relic_index_t *index, const relic_relation_t *relation,
    const relic_dom_t *probe)
{
	relic_key_ref_t ref;

	ref = probe_key(index, relation, probe);
	if (ref.key_id == RELIC_ID_NONE)
		return (RELIC_ID_NONE);

	return (ref.stripe->entries[ref.key_id - 1].first);
}

/*
 * Walk a chain forward from its tuple `id` toward `since`, and return where
 * the walk stops: the first chain tuple at or past `since`, or the chain's
 * last tuple when every tuple from `id` on is below `since`.
 */
static relic_tuple_id_t
walk_toward(const relic_index_t *index, relic_tuple_id_t id,
    relic_tuple_id_t since)
{
	while (id < since && index->next[id - 1] != RELIC_ID_NONE)
		id = index->next[id - 1];

	return (id);
}

/*
 * Raise the index to a delta start `since`, from which the lookups about to
 * be made begin, if it is past every start the index was raised to before.
 *
 * Every chain tuple before a key's frontier is below `latest_since`, and
 * raising `latest_since` only loosens that bound, so the frontiers stay
 * valid without being touched.  A start below `latest_since` changes
 * nothing.
 */
void
relic_index_raise_since(relic_index_t *index, relic_tuple_id_t since)
{
	if (since > index->latest_since)
		index->latest_since = since;
}

/*
 * The first tuple, in identifier order, that agrees with `probe` on the key
 * columns and whose identifier is at least `since`, or RELIC_ID_NONE when
 * none does: where a lookup of a delta beginning at `since` starts, and the
 * tuple an existence check of that delta reads.
 *
 * Every chain tuple before a key's frontier is below `latest_since`, so a
 * start at or past `latest_since` can be walked to from the frontier.  When
 * the start is `latest_since` itself the walk leaves the frontier where it
 * stops, at the tuple found or at the chain's last tuple, and every chain
 * tuple before either is below the start, which keeps the invariant.  A
 * start below `latest_since` is walked to from the chain's head and moves
 * nothing; the evaluator never asks for one, since a relation's deltas
 * only advance while it is a member, it is a member of one component of
 * one evaluation, and its indexes are dropped when it is discarded or
 * cleared.
 *
 * A start past `latest_since` is a caller that did not raise the index
 * first.  It would be answered correctly and could move nothing, since the
 * tuples its walk passes need not be below `latest_since`, so every such
 * lookup would walk from the same stale frontier and a fixed point would
 * pay for each chain once per lookup rather than once.  Nothing wrong would
 * show but the time, so a debug build stops there instead.
 *
 * Every lookup or check of one key at one start walks toward that start
 * through this function, so each stops at the same tuple and leaves the
 * same frontier, whichever frontier it began from: one that began from an
 * older frontier than another left only walks further to reach the same
 * stop.  Lookups and checks at one start may therefore be made in any
 * order, on any number of threads at once, which is why a frontier is
 * loaded and stored as a relaxed atomic word.
 *
 * A step of the walk reads the `next` array and no tuple.
 */
relic_tuple_id_t
relic_index_first_since(const relic_index_t *index,
    const relic_relation_t *relation, const relic_dom_t *probe,
    relic_tuple_id_t since)
{
	relic_atomic_u32_t *frontier;
	relic_key_ref_t ref;
	relic_tuple_id_t start;
	relic_tuple_id_t stop;

	assert(since <= index->latest_since);

	ref = probe_key(index, relation, probe);
	if (ref.key_id == RELIC_ID_NONE)
		return (RELIC_ID_NONE);
	frontier = &ref.stripe->frontiers[ref.key_id - 1];

	if (since < index->latest_since) {
		stop = walk_toward(index,
		    ref.stripe->entries[ref.key_id - 1].first, since);
	} else {
		start = relic_atomic_u32_load_relaxed(frontier);
		stop = walk_toward(index, start, since);
		if (since == index->latest_since && stop != start)
			relic_atomic_u32_store_relaxed(frontier, stop);
	}

	return (stop >= since ? stop : RELIC_ID_NONE);
}

/*
 * The tuple after `id` on its key's chain, or RELIC_ID_NONE when `id` is the
 * last.  `id` must be one the index holds.
 */
relic_tuple_id_t
relic_index_next(const relic_index_t *index, relic_tuple_id_t id)
{
	return (index->next[id - 1]);
}
