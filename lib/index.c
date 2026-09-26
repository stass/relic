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
static uint32_t		 probe_key(const relic_index_t *index,
			    const relic_relation_t *relation,
			    const relic_dom_t *probe);
static relic_tuple_id_t	 walk_toward(const relic_index_t *index,
			    relic_tuple_id_t id, relic_tuple_id_t since);

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
	index->latest_since = RELIC_ID_NONE;
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
	size_t entries;
	size_t key_slots;
	size_t next;

	if (added_keys > SIZE_MAX - index->key_count)
		return (relic_index_bytes(index));

	key_slots = relic_idtab_bytes_after_reserve(&index->keys, added_keys);

	entries = relic_grow_array_capacity(index->entry_capacity,
	    index->key_count + added_keys);
	next = relic_grow_array_capacity(index->next_capacity, tuples);
	if (entries == 0 || next == 0 ||
	    entries > SIZE_MAX / sizeof(*index->entries) ||
	    next > SIZE_MAX / sizeof(*index->next))
		return (relic_index_bytes(index));

	entries *= sizeof(*index->entries);
	next *= sizeof(*index->next);
	if (entries > SIZE_MAX - next || entries + next > SIZE_MAX - key_slots)
		return (relic_index_bytes(index));

	return (key_slots + entries + next);
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
	entry->frontier = id;
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
	uint32_t key_id;
	uint16_t length;

	length = gather_key(index, probe, key);
	key_id = find_key(index, relation, key, length,
	    key_hash(relation, key, length));

	return (key_id);
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
