/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

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
 * records the first and last tuple of its chain, and the chain itself is
 * threaded through one `next` identifier per tuple.  A lookup probes once for
 * the key and then follows the chain.
 *
 * A tuple is added at the end of its key's chain as it is inserted, so every
 * chain lists its tuples in increasing identifier order, which is insertion
 * order.  That order is what makes a lookup visit exactly what a scan would
 * have matched, in the same order, and what lets a lookup that must not see
 * tuples inserted after it started simply stop at the first identifier past
 * the relation's end as it was then.
 *
 * Adding a tuple is two operations, as installing an identifier in the table
 * is.  relic_index_reserve() makes room for one more tuple and one more key
 * and is the only part that can fail; relic_index_add() then links the tuple
 * in and cannot.  The relation store reserves in every index before it stores
 * anything, so that no failure leaves a tuple the relation holds missing from
 * an index, which would be a lookup silently missing an answer.
 *
 * Nothing here knows about limits or the context.  The relation store
 * measures an index before and after a reservation and does the accounting.
 */

typedef struct relic_index_rehash relic_index_rehash_t;

/*
 * What the key table needs in order to recompute the hash of a key it holds:
 * the key's first tuple lives in the relation, and the entry saying which
 * tuple that is lives in the index.
 */
struct relic_index_rehash {
	const relic_index_t	*index;
	const relic_relation_t	*relation;
};

static uint32_t		 find_key(const relic_index_t *index,
			    const relic_relation_t *relation,
			    const relic_dom_t *key, uint16_t length,
			    uint32_t hash);
static uint16_t		 gather_key(const relic_index_t *index,
			    const relic_dom_t *tuple, relic_dom_t *key);
static uint32_t		 key_hash(const relic_relation_t *relation,
			    const relic_dom_t *key, uint16_t length);
static bool		 key_matches(const relic_index_t *index,
			    const relic_relation_t *relation, uint32_t key_id,
			    const relic_dom_t *key, uint16_t length);
static uint32_t		 key_rehash(uint32_t key_id, void *ctx);

void
relic_index_init(relic_index_t *index, uint32_t key_mask)
{
	bzero(index, sizeof(*index));
	relic_idtab_init(&index->keys);
	index->key_mask = key_mask;
}

/*
 * Release everything the index holds, keeping its key mask, so that it is
 * the empty index over the same columns.
 */
void
relic_index_empty(relic_index_t *index)
{
	relic_idtab_reset(&index->keys);
	free(index->entries);
	free(index->next);
	index->entries = NULL;
	index->next = NULL;
	index->key_count = 0;
	index->entry_capacity = 0;
	index->next_capacity = 0;
}

/*
 * Copy a tuple's key columns, in ascending column order, into `key`, which
 * has room for the largest arity there is.  Returns how many were copied:
 * one per column in the mask.
 */
static uint16_t
gather_key(const relic_index_t *index, const relic_dom_t *tuple,
    relic_dom_t *key)
{
	uint16_t column;
	uint16_t length;

	length = 0;
	for (column = 0; column < RELIC_MAX_ARITY; column++) {
		if ((index->key_mask & (UINT32_C(1) << column)) == 0)
			continue;

		key[length] = tuple[column];
		length++;
	}

	return (length);
}

/*
 * The hash of a key's values, keyed by the seed of the relation the index
 * belongs to.
 */
static uint32_t
key_hash(const relic_relation_t *relation, const relic_dom_t *key,
    uint16_t length)
{
	return (relic_hash_words(relation->hash_seed, key, length));
}

/*
 * Whether key identifier `key_id` stands for these key values.  The key's
 * values are read from its first tuple, which is where the only copy of them
 * is.
 */
static bool
key_matches(const relic_index_t *index, const relic_relation_t *relation,
    uint32_t key_id, const relic_dom_t *key, uint16_t length)
{
	relic_dom_t held[RELIC_MAX_ARITY];
	const relic_dom_t *tuple;
	uint16_t i;

	tuple = relic_relation_tuple(relation,
	    index->entries[key_id - 1].first);
	gather_key(index, tuple, held);

	for (i = 0; i < length; i++) {
		if (held[i] != key[i])
			return (false);
	}

	return (true);
}

/*
 * The hash of a key the table holds, for the table to use when it grows.
 */
static uint32_t
key_rehash(uint32_t key_id, void *ctx)
{
	const relic_index_rehash_t *rehash;
	relic_dom_t key[RELIC_MAX_ARITY];
	const relic_dom_t *tuple;
	uint16_t length;

	rehash = ctx;
	tuple = relic_relation_tuple(rehash->relation,
	    rehash->index->entries[key_id - 1].first);
	length = gather_key(rehash->index, tuple, key);

	return (key_hash(rehash->relation, key, length));
}

/*
 * The identifier of the key holding these values, or RELIC_ID_NONE when no
 * tuple of the relation holds them.
 */
static uint32_t
find_key(const relic_index_t *index, const relic_relation_t *relation,
    const relic_dom_t *key, uint16_t length, uint32_t hash)
{
	relic_idprobe_t probe;
	uint32_t key_id;

	relic_idprobe_open(&probe, &index->keys, hash);
	while (relic_idprobe_next(&probe, &key_id)) {
		if (key_matches(index, relation, key_id, key, length))
			return (key_id);
	}

	return (RELIC_ID_NONE);
}

size_t
relic_index_bytes(const relic_index_t *index)
{
	return (relic_idtab_bytes(&index->keys) +
	    index->entry_capacity * sizeof(*index->entries) +
	    index->next_capacity * sizeof(*index->next));
}

/*
 * The bytes the index would hold once relic_index_reserve() had made room for
 * `tuples` tuples and one more key.
 *
 * Each growth policy is asked what it would do rather than measured
 * afterwards, which is what lets the memory limit refuse an allocation
 * instead of reporting one.  A growth that could not happen at all is
 * reported as the size the index already has, so that the limit admits it
 * and the reservation reports the real obstacle, as the relation store does
 * for its own arrays.
 */
size_t
relic_index_bytes_after_reserve(const relic_index_t *index, size_t tuples)
{
	size_t entries;
	size_t keys;
	size_t next;

	keys = relic_idtab_bytes_after_reserve(&index->keys);

	entries = relic_grow_array_capacity(index->entry_capacity,
	    index->key_count + 1);
	next = relic_grow_array_capacity(index->next_capacity, tuples);
	if (entries == 0 || next == 0 ||
	    entries > SIZE_MAX / sizeof(*index->entries) ||
	    next > SIZE_MAX / sizeof(*index->next))
		return (relic_index_bytes(index));

	entries *= sizeof(*index->entries);
	next *= sizeof(*index->next);
	if (entries > SIZE_MAX - next || entries + next > SIZE_MAX - keys)
		return (relic_index_bytes(index));

	return (keys + entries + next);
}

/*
 * Make room for `tuples` tuples and one more key.  Returns 0, or -1 when an
 * array could not grow; whatever the steps before it obtained is kept, and
 * the caller accounts for it by measuring.
 *
 * Room for a key is made whether or not the tuple about to be added brings a
 * new one.  Finding out would take a probe that relic_index_add() then makes
 * again, and reserving regardless costs at most one growth a little sooner
 * than necessary.  What matters is that relic_index_bytes_after_reserve()
 * assumes the same, so the limit is charged for exactly what is allocated.
 */
int
relic_index_reserve(relic_index_t *index, const relic_relation_t *relation,
    size_t tuples)
{
	relic_index_rehash_t rehash;
	relic_index_key_t *entries;
	relic_tuple_id_t *next;

	rehash.index = index;
	rehash.relation = relation;
	if (relic_idtab_reserve(&index->keys, key_rehash, &rehash) != 0)
		return (-1);

	entries = relic_grow_array(index->entries, &index->entry_capacity,
	    index->key_count + 1, sizeof(*index->entries));
	if (entries == NULL)
		return (-1);
	index->entries = entries;

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
	uint16_t length;

	length = gather_key(index, relic_relation_tuple(relation, id), key);
	hash = key_hash(relation, key, length);
	index->next[id - 1] = RELIC_ID_NONE;

	key_id = find_key(index, relation, key, length, hash);
	if (key_id != RELIC_ID_NONE) {
		entry = &index->entries[key_id - 1];
		index->next[entry->last - 1] = id;
		entry->last = id;
		return;
	}

	/*
	 * A new key.  Its identifier is one past the last, so it can never be
	 * RELIC_ID_NONE, and it cannot overflow: an index has no more keys
	 * than its relation has tuples.
	 */
	index->key_count++;
	key_id = (uint32_t)index->key_count;
	entry = &index->entries[key_id - 1];
	entry->first = id;
	entry->last = id;
	relic_idtab_place(&index->keys, hash, key_id);
}

/*
 * The first tuple, in identifier order, that agrees with `probe` on the key
 * columns, or RELIC_ID_NONE when none does.  `probe` is laid out as a tuple
 * of the relation, and only its key columns are read, so a caller never
 * needs to know how the index packs a key.
 */
relic_tuple_id_t
relic_index_first(const relic_index_t *index, const relic_relation_t *relation,
    const relic_dom_t *probe)
{
	relic_dom_t key[RELIC_MAX_ARITY];
	uint32_t key_id;
	uint16_t length;

	length = gather_key(index, probe, key);
	key_id = find_key(index, relation, key, length,
	    key_hash(relation, key, length));
	if (key_id == RELIC_ID_NONE)
		return (RELIC_ID_NONE);

	return (index->entries[key_id - 1].first);
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
