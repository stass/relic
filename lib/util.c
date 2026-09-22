/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdlib.h>
#include <strings.h>

#include "compat.h"
#include "util.h"

/*
 * The FNV-1a parameters, used both as the byte hash's starting state and as
 * the constant that mixes an integer key before it is finalized.  They are
 * ordinary compile-time constants: relic never randomizes a hash seed.
 */
#define	RELIC_HASH_BASIS	2166136261u
#define	RELIC_HASH_PRIME	16777619u

/*
 * The smallest table worth allocating, and the smallest array worth growing
 * to.  Both are first allocations rather than thresholds: a table doubles
 * from here and an array doubles from there.
 */
#define	RELIC_IDTAB_MIN_CAPACITY	16u
#define	RELIC_ARRAY_MIN_COUNT		8u

static uint32_t	 finalize(uint32_t hash);
static size_t	 idprobe_empty_slot(relic_idprobe_t *probe);
static int	 idtab_grow(relic_idtab_t *table, relic_rehash_fn rehash,
		    void *ctx);
static bool	 idtab_would_overfill(const relic_idtab_t *table);

/*
 * Avalanche a hash so that every input bit affects the low bits.
 *
 * This matters more here than it would in a chained table.  A slot is chosen
 * with hash & (capacity - 1), so only the low bits select it, and collisions
 * under linear probing form runs that lengthen each other.  FNV-1a alone
 * mixes poorly into its low bits for short keys, and sequential integer keys
 * -- which a Datalog program full of small integers produces -- would
 * otherwise probe in near-perfect runs.
 */
static uint32_t
finalize(uint32_t hash)
{
	hash ^= hash >> 16;
	hash *= 0x85ebca6bu;
	hash ^= hash >> 13;
	hash *= 0xc2b2ae35u;
	hash ^= hash >> 16;

	return (hash);
}

uint32_t
relic_hash_bytes(const void *bytes, size_t length)
{
	const unsigned char *data;
	uint32_t hash;
	size_t i;

	data = bytes;
	hash = RELIC_HASH_BASIS;

	for (i = 0; i < length; i++) {
		hash ^= data[i];
		hash *= RELIC_HASH_PRIME;
	}

	return (finalize(hash));
}

uint32_t
relic_hash_word(uint32_t word)
{
	return (finalize(word ^ RELIC_HASH_BASIS));
}

uint32_t
relic_hash_int64(int64_t value)
{
	uint64_t bits;
	uint32_t low;
	uint32_t high;

	/*
	 * The halves are extracted by shifting rather than by reading the
	 * representation's bytes, so a table probes in the same order on a
	 * big-endian machine as on a little-endian one.  Conversion to the
	 * unsigned type is a defined wraparound, so a negative value mixes as
	 * well as a positive one.
	 */
	bits = (uint64_t)value;
	low = (uint32_t)bits;
	high = (uint32_t)(bits >> 32);

	return (relic_hash_word(low ^ relic_hash_word(high)));
}

void *
relic_grow_array(void *items, size_t *capacity, size_t count, size_t item_size)
{
	void *grown;
	size_t wanted;

	if (count == 0)
		count = 1;

	if (count <= *capacity)
		return (items);

	/*
	 * Doubling from here is not representable, and no allocator could
	 * serve an array this size in any case.
	 */
	if (*capacity > SIZE_MAX / 2)
		return (NULL);

	/*
	 * Double, then raise the result to the smallest array worth having
	 * and to what the caller asked for.  reallocarray() rejects a product
	 * that would overflow, so the multiplication by item_size needs no
	 * check here.
	 */
	wanted = *capacity * 2;
	if (wanted < RELIC_ARRAY_MIN_COUNT)
		wanted = RELIC_ARRAY_MIN_COUNT;
	if (wanted < count)
		wanted = count;

	grown = reallocarray(items, wanted, item_size);
	if (grown == NULL)
		return (NULL);

	*capacity = wanted;

	return (grown);
}

void
relic_idtab_init(relic_idtab_t *table)
{
	bzero(table, sizeof(*table));
}

void
relic_idtab_reset(relic_idtab_t *table)
{
	free(table->slots);
	bzero(table, sizeof(*table));
}

void
relic_idprobe_open(relic_idprobe_t *probe, const relic_idtab_t *table,
    uint32_t hash)
{
	if (table->capacity == 0) {
		bzero(probe, sizeof(*probe));
		return;
	}

	/*
	 * The table is held rather than its slot array, so that a cursor left
	 * open across a growth reads the current array instead of the freed
	 * one.  It would be looking in the wrong places -- the mask it
	 * captured is the old one -- but a table only ever grows, so the
	 * stale mask is smaller than the current capacity and every slot it
	 * selects is in bounds.  A logic error is recoverable; a read of
	 * freed memory is not.
	 */
	probe->table = table;
	probe->mask = table->capacity - 1;
	probe->slot = (size_t)hash & probe->mask;
}

bool
relic_idprobe_next(relic_idprobe_t *probe, uint32_t *idp)
{
	uint32_t id;

	if (probe->table == NULL)
		return (false);

	id = probe->table->slots[probe->slot];
	if (id == RELIC_ID_NONE)
		return (false);

	/*
	 * The slot advances only when a candidate is handed back, so a probe
	 * that has returned false is left standing on the empty slot where
	 * the key belongs.  relic_idtab_place() relies on that.
	 */
	probe->slot = (probe->slot + 1) & probe->mask;
	*idp = id;

	return (true);
}

/*
 * Whether one more entry would take the table past three quarters full.
 *
 * The load factor is a performance choice, but the fact that some slot stays
 * empty is a correctness one: a full table would make relic_idprobe_next()
 * circle forever instead of reporting that the key is absent.
 */
static bool
idtab_would_overfill(const relic_idtab_t *table)
{
	return (table->count + 1 > table->capacity - table->capacity / 4);
}

/*
 * Advance a probe past every candidate to the empty slot that ends its walk,
 * and return that slot.  A key known to be absent belongs there.
 *
 * The walk terminates because the table grows before it fills, so some slot
 * is always empty.
 */
static size_t
idprobe_empty_slot(relic_idprobe_t *probe)
{
	uint32_t skipped;

	while (relic_idprobe_next(probe, &skipped))
		;

	return (probe->slot);
}

/*
 * Store an identifier known to be absent in the first empty slot of its
 * probe sequence.  Walking with the same cursor a lookup uses is what keeps
 * the two from drifting apart: there is one probe sequence in the tree, not
 * one per operation.
 *
 * The table must already have slots and at least one of them empty, which a
 * preceding relic_idtab_reserve() guarantees.  idtab_grow() also uses this,
 * on a table it has just allocated.
 */
void
relic_idtab_place(relic_idtab_t *table, uint32_t hash, uint32_t id)
{
	relic_idprobe_t probe;

	relic_idprobe_open(&probe, table, hash);
	table->slots[idprobe_empty_slot(&probe)] = id;
	table->count++;
}

/*
 * Replace the slot array with a larger one and reinsert every entry: twice
 * the current size, or the smallest table worth allocating when there is not
 * one yet.
 *
 * The replacement is built complete and installed in one assignment, so a
 * failed allocation leaves the caller's table exactly as it was rather than
 * half rehashed.
 */
static int
idtab_grow(relic_idtab_t *table, relic_rehash_fn rehash, void *ctx)
{
	relic_idtab_t grown;
	size_t capacity;
	size_t slot;

	capacity = RELIC_IDTAB_MIN_CAPACITY;
	if (table->capacity >= capacity) {
		if (table->capacity > SIZE_MAX / 2 / sizeof(*table->slots))
			return (-1);
		capacity = table->capacity * 2;
	}

	relic_idtab_init(&grown);
	grown.slots = calloc(capacity, sizeof(*grown.slots));
	if (grown.slots == NULL)
		return (-1);
	grown.capacity = capacity;

	for (slot = 0; slot < table->capacity; slot++) {
		uint32_t id;

		id = table->slots[slot];
		if (id == RELIC_ID_NONE)
			continue;

		relic_idtab_place(&grown, rehash(id, ctx), id);
	}

	free(table->slots);
	*table = grown;

	return (0);
}

int
relic_idtab_reserve(relic_idtab_t *table, relic_rehash_fn rehash, void *ctx)
{
	if (!idtab_would_overfill(table))
		return (0);

	return (idtab_grow(table, rehash, ctx));
}
