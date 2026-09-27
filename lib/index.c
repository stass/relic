/*
 * Copyright (c) 2026 Stanislav Sedov
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
 * grouping tuples into one chain per distinct key.  The open-addressing
 * identifier table maps a key's hash to its key identifier, the key's entry
 * records the first and last tuple of its chain, its values are kept beside
 * the entries, and the chain itself is threaded through one `next`
 * identifier per tuple.  A lookup probes once for the key, comparing
 * against stored values, and then follows the chain.
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
 * tuple.  The frontier is moved forward as far as each such lookup walks,
 * and deltas only move forward, so over a whole fixed point the positioning
 * walks each chain at most once however many rounds and lookups there are.
 *
 * Adding a tuple is two operations, as installing an identifier in the table
 * is.  relic_index_reserve() makes room for the tuples and keys about to be
 * added and is the only part that can fail; relic_index_add() then links a
 * tuple in and cannot.  The relation store reserves in every index before it
 * stores anything, so that no failure leaves a tuple the relation holds
 * missing from an index, which would be a lookup silently missing an answer.
 *
 * Nothing here knows about limits or the context.  The relation store
 * measures an index before and after a reservation and does the accounting.
 */

typedef struct relic_index_rehash relic_index_rehash_t;

/*
 * What the key table needs in order to recompute the hash of a key it holds:
 * the index, which holds the key's values, and the relation, whose seed the
 * hash is keyed by.
 */
struct relic_index_rehash {
	const relic_index_t	*index;
	const relic_relation_t	*relation;
};

static uint32_t		 find_key(const relic_index_t *index,
			    const relic_dom_t *key, uint32_t hash);
static void		 gather_key(const relic_index_t *index,
			    const relic_dom_t *tuple, relic_dom_t *key);
static uint32_t		 key_hash(const relic_index_t *index,
			    const relic_relation_t *relation,
			    const relic_dom_t *key);
static size_t		 key_offset(const relic_index_t *index,
			    uint32_t key_id);
static uint32_t		 key_rehash(uint32_t key_id, void *ctx);
static const relic_dom_t *key_values_of(const relic_index_t *index,
			    uint32_t key_id);
static uint32_t		 probe_key(const relic_index_t *index,
			    const relic_relation_t *relation,
			    const relic_dom_t *probe);
static size_t		 saturating_product(size_t count, size_t size);
static size_t		 saturating_sum(size_t a, size_t b);
static relic_tuple_id_t	 walk_toward(const relic_index_t *index,
			    relic_tuple_id_t id, relic_tuple_id_t since);

/*
 * An empty index keyed on the columns of `key_mask`, which lists them in
 * `key_column` once so that gathering a key visits its columns and no
 * others.  The relation store keys an index on at least one column, so a
 * key has values to store.  The key table is tagged: a probe that misses
 * would otherwise compare against each key it passes.
 */
void
relic_index_init(relic_index_t *index, uint32_t key_mask)
{
	uint16_t column;

	bzero(index, sizeof(*index));
	relic_idtab_init(&index->keys, true);
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
	relic_idtab_reset(&index->keys);
	relic_free_array(index->entries);
	relic_free_array(index->key_values);
	relic_free_array(index->next);
	bzero(index, sizeof(*index));
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
 * The hash of a key's values, keyed by the seed of the relation the index
 * belongs to.
 */
static uint32_t
key_hash(const relic_index_t *index, const relic_relation_t *relation,
    const relic_dom_t *key)
{
	return (relic_hash_words(relation->hash_seed, key, index->key_length));
}

/*
 * Where key `key_id`'s values begin in `key_values`: keys are numbered from
 * one, and each holds `key_length` values.
 */
static size_t
key_offset(const relic_index_t *index, uint32_t key_id)
{
	return ((size_t)(key_id - 1) * index->key_length);
}

/*
 * The values of key `key_id`, `key_length` of them.
 */
static const relic_dom_t *
key_values_of(const relic_index_t *index, uint32_t key_id)
{
	return (&index->key_values[key_offset(index, key_id)]);
}

/*
 * `count` items of `size` bytes, or SIZE_MAX when that is more than a size_t
 * holds.
 */
static size_t
saturating_product(size_t count, size_t size)
{
	if (size != 0 && count > SIZE_MAX / size)
		return (SIZE_MAX);

	return (count * size);
}

/*
 * The sum of two sizes, or SIZE_MAX when it is more than a size_t holds.
 */
static size_t
saturating_sum(size_t a, size_t b)
{
	if (a > SIZE_MAX - b)
		return (SIZE_MAX);

	return (a + b);
}

/*
 * The hash of a key the table holds, for the table to use when it grows.
 */
static uint32_t
key_rehash(uint32_t key_id, void *ctx)
{
	const relic_index_rehash_t *rehash;

	rehash = ctx;

	return (key_hash(rehash->index, rehash->relation,
	    key_values_of(rehash->index, key_id)));
}

/*
 * The identifier of the key holding these values, or RELIC_ID_NONE when no
 * tuple of the relation holds them.
 */
static uint32_t
find_key(const relic_index_t *index, const relic_dom_t *key, uint32_t hash)
{
	relic_idprobe_t probe;
	size_t key_bytes;
	uint32_t key_id;

	key_bytes = (size_t)index->key_length * sizeof(*key);

	relic_idprobe_open(&probe, &index->keys, hash);
	while (relic_idprobe_next(&probe, &key_id)) {
		if (bcmp(key_values_of(index, key_id), key, key_bytes) == 0)
			return (key_id);
	}

	return (RELIC_ID_NONE);
}

size_t
relic_index_bytes(const relic_index_t *index)
{
	return (relic_idtab_bytes(&index->keys) +
	    index->entry_capacity * sizeof(*index->entries) +
	    index->key_value_capacity * index->key_length *
	    sizeof(*index->key_values) +
	    index->next_capacity * sizeof(*index->next));
}

/*
 * The bytes the index would hold once relic_index_reserve() had made room for
 * `tuples` tuples in all and `added_keys` more keys.
 *
 * Each growth policy is asked what it would do rather than measured
 * afterwards, which is what lets the memory limit refuse an allocation
 * instead of reporting one.  A growth that could not happen at all is
 * reported as the size the index already has, so that the limit admits it
 * and the reservation reports the real obstacle, as the relation store does
 * for its own arrays.
 */
size_t
relic_index_bytes_after_reserve(const relic_index_t *index, size_t tuples,
    size_t added_keys)
{
	size_t entry_count;
	size_t key_bytes;
	size_t next_count;
	size_t total;
	size_t value_count;
	size_t keys;

	keys = saturating_sum(index->key_count, added_keys);
	entry_count = relic_grow_array_capacity(index->entry_capacity, keys);
	value_count = relic_grow_array_capacity(index->key_value_capacity,
	    keys);
	next_count = relic_grow_array_capacity(index->next_capacity, tuples);
	if (keys == SIZE_MAX || entry_count == 0 || value_count == 0 ||
	    next_count == 0)
		return (relic_index_bytes(index));

	key_bytes = (size_t)index->key_length * sizeof(*index->key_values);

	total = relic_idtab_bytes_after_reserve(&index->keys, added_keys);
	total = saturating_sum(total, saturating_product(entry_count,
	    sizeof(*index->entries)));
	total = saturating_sum(total, saturating_product(value_count,
	    key_bytes));
	total = saturating_sum(total, saturating_product(next_count,
	    sizeof(*index->next)));
	if (total == SIZE_MAX)
		return (relic_index_bytes(index));

	return (total);
}

/*
 * Make room for `tuples` tuples in all and `added_keys` more keys.  Returns
 * 0, or -1 when an array could not grow; whatever the steps before it
 * obtained is kept, and the caller accounts for it by measuring.
 *
 * The relation store asks for a key per tuple about to be added, whether or
 * not it brings a new one.  Finding out would take a probe that
 * relic_index_add() then makes again, and for the one tuple an insertion
 * adds, reserving regardless costs at most a growth a little sooner than
 * necessary.  What matters is that relic_index_bytes_after_reserve() assumes
 * the same, so the limit is charged for exactly what is allocated.
 */
int
relic_index_reserve(relic_index_t *index, const relic_relation_t *relation,
    size_t tuples, size_t added_keys)
{
	relic_index_rehash_t rehash;
	relic_index_key_t *entries;
	relic_tuple_id_t *next;
	relic_dom_t *values;

	if (added_keys > SIZE_MAX - index->key_count)
		return (-1);

	rehash.index = index;
	rehash.relation = relation;
	if (relic_idtab_reserve(&index->keys, added_keys,
	    key_rehash, &rehash) != 0)
		return (-1);

	entries = relic_grow_array(index->entries, &index->entry_capacity,
	    index->key_count + added_keys, sizeof(*index->entries));
	if (entries == NULL)
		return (-1);
	index->entries = entries;

	values = relic_grow_array(index->key_values,
	    &index->key_value_capacity, index->key_count + added_keys,
	    (size_t)index->key_length * sizeof(*index->key_values));
	if (values == NULL)
		return (-1);
	index->key_values = values;

	next = relic_grow_array(index->next, &index->next_capacity, tuples,
	    sizeof(*index->next));
	if (next == NULL)
		return (-1);
	index->next = next;

	return (0);
}

/*
 * Link tuple `id`, which the relation holds, at the end of its key's chain,
 * starting a chain when its key is new.  The index must have room, which a
 * preceding relic_index_reserve() for at least `id` tuples guarantees, and
 * nothing here can fail.
 *
 * Tuples must be added in increasing identifier order, which both insertion
 * and building an index do; that is what keeps every chain in identifier
 * order.
 */
void
relic_index_add(relic_index_t *index, const relic_relation_t *relation,
    relic_tuple_id_t id)
{
	relic_dom_t key[RELIC_MAX_ARITY];
	relic_index_key_t *entry;
	uint32_t key_id;
	uint32_t hash;

	gather_key(index, relic_relation_tuple(relation, id), key);
	hash = key_hash(index, relation, key);
	index->next[id - 1] = RELIC_ID_NONE;

	key_id = find_key(index, key, hash);
	if (key_id != RELIC_ID_NONE) {
		entry = &index->entries[key_id - 1];
		index->next[entry->last - 1] = id;
		entry->last = id;
		return;
	}

	/*
	 * A new key.  Its identifier is one past the last, so it can never be
	 * RELIC_ID_NONE, and it cannot overflow: an index has no more keys
	 * than its relation has tuples.  Its values are stored before it is
	 * placed, so that every key the table holds has them.
	 */
	index->key_count++;
	key_id = (uint32_t)index->key_count;
	entry = &index->entries[key_id - 1];
	entry->first = id;
	entry->last = id;
	entry->frontier = id;
	bcopy(key, &index->key_values[key_offset(index, key_id)],
	    (size_t)index->key_length * sizeof(*key));
	relic_idtab_place(&index->keys, hash, key_id);
}

/*
 * The identifier of the key `probe` holds in the key columns, or
 * RELIC_ID_NONE when no tuple of the relation holds it.  `probe` is laid out
 * as a tuple of the relation, and only its key columns are read, so a caller
 * never needs to know how the index packs a key.
 */
static uint32_t
probe_key(const relic_index_t *index, const relic_relation_t *relation,
    const relic_dom_t *probe)
{
	relic_dom_t key[RELIC_MAX_ARITY];

	gather_key(index, probe, key);

	return (find_key(index, key, key_hash(index, relation, key)));
}

/*
 * The first tuple, in identifier order, that agrees with `probe` on the key
 * columns, or RELIC_ID_NONE when none does.
 */
relic_tuple_id_t
relic_index_first(const relic_index_t *index, const relic_relation_t *relation,
    const relic_dom_t *probe)
{
	uint32_t key_id;

	key_id = probe_key(index, relation, probe);
	if (key_id == RELIC_ID_NONE)
		return (RELIC_ID_NONE);

	return (index->entries[key_id - 1].first);
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
 * The first tuple, in identifier order, that agrees with `probe` on the key
 * columns and whose identifier is at least `since`, or RELIC_ID_NONE when
 * none does: where a lookup of a delta beginning at `since` starts.
 *
 * Every chain tuple before a key's frontier is below `latest_since`, so a
 * start at or past `latest_since` can be walked to from the frontier.  Such
 * a walk raises `latest_since` to its start and leaves the frontier where
 * it stops, at the tuple found or at the chain's last tuple, both of which
 * keep the invariant.  A start below `latest_since` is walked to from the
 * chain's head and moves nothing; the evaluator never asks for one, since a
 * relation's deltas advance and its indexes are dropped with the discard
 * that precedes every evaluation, so that walk costs the chain's length on
 * no path it has.
 *
 * A step of the walk reads the `next` array and no tuple.
 */
relic_tuple_id_t
relic_index_first_since(relic_index_t *index, const relic_relation_t *relation,
    const relic_dom_t *probe, relic_tuple_id_t since)
{
	relic_index_key_t *entry;
	relic_tuple_id_t stop;
	uint32_t key_id;

	key_id = probe_key(index, relation, probe);
	if (key_id == RELIC_ID_NONE)
		return (RELIC_ID_NONE);
	entry = &index->entries[key_id - 1];

	if (since < index->latest_since) {
		stop = walk_toward(index, entry->first, since);
	} else {
		index->latest_since = since;
		stop = walk_toward(index, entry->frontier, since);
		entry->frontier = stop;
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
