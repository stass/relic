/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <strings.h>

#include "compat.h"
#include "util.h"

/*
 * The FNV-1a prime, which the fold multiplies the state by after mixing in
 * each unit of a key.  The state it starts from is not the FNV basis but half
 * of the caller's seed.
 */
#define	RELIC_HASH_PRIME	16777619u

/*
 * The smallest table worth allocating, and the smallest array worth growing
 * to.  Both are first allocations rather than thresholds: a table doubles
 * from here and an array doubles from there.
 */
#define	RELIC_IDTAB_MIN_CAPACITY	16u
#define	RELIC_ARRAY_MIN_COUNT		8u

static uint32_t	 finalize(uint32_t hash);
static uint32_t	 hash_finish(uint64_t seed, uint32_t state);
static uint32_t	 hash_fold(uint32_t state, uint32_t unit);
static uint32_t	 hash_start(uint64_t seed);
static size_t	 idprobe_empty_slot(relic_idprobe_t *probe);
static int	 idtab_grow(relic_idtab_t *table, relic_rehash_fn rehash,
		    void *ctx);
static size_t	 idtab_next_capacity(const relic_idtab_t *table);
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

/*
 * Every hash is a keyed FNV-1a fold followed by finalize():
 *
 *	state = low 32 bits of seed
 *	for each unit u of the key:  state = (state ^ u) * FNV prime
 *	hash  = finalize(state ^ high 32 bits of seed)
 *
 * The two halves of the seed enter at different points so that both the fold
 * and the finalizer are keyed.  A unit is a byte of a byte string and a word
 * of a word sequence.  The three functions below are those three steps, and
 * each hash function is written in terms of them so that none can drift from
 * the others.
 *
 * hash_start() also asserts that the seed is not zero.  No context holds a
 * seed of zero, so a zero is an owner that was never given its context's
 * seed, and since every hash starts here the one assertion checks every
 * owner.
 */
static uint32_t
hash_start(uint64_t seed)
{
	assert(seed != 0);

	return ((uint32_t)seed);
}

static uint32_t
hash_fold(uint32_t state, uint32_t unit)
{
	return ((state ^ unit) * RELIC_HASH_PRIME);
}

static uint32_t
hash_finish(uint64_t seed, uint32_t state)
{
	return (finalize(state ^ (uint32_t)(seed >> 32)));
}

uint32_t
relic_hash_bytes(uint64_t seed, const void *bytes, size_t length)
{
	const unsigned char *data;
	uint32_t state;
	size_t i;

	data = bytes;
	state = hash_start(seed);

	for (i = 0; i < length; i++)
		state = hash_fold(state, data[i]);

	return (hash_finish(seed, state));
}

/*
 * Mixing each word as a value rather than as its four bytes is what makes the
 * result independent of byte order.
 */
uint32_t
relic_hash_words(uint64_t seed, const uint32_t *words, size_t count)
{
	uint32_t state;
	size_t i;

	state = hash_start(seed);

	for (i = 0; i < count; i++)
		state = hash_fold(state, words[i]);

	return (hash_finish(seed, state));
}

uint32_t
relic_hash_word(uint64_t seed, uint32_t word)
{
	return (relic_hash_words(seed, &word, 1));
}

/*
 * An integer is hashed in two steps: its high half alone, then its low half
 * mixed with that hash.
 *
 * Folding the two halves as one word sequence would admit a collision that
 * holds under every seed.  Flipping bit 31 of a word adds 2^31 to the state,
 * multiplying by an odd prime keeps that difference in bit 31 alone, and the
 * next word can flip it back, so v and v ^ 0x8000000080000000 would hash
 * alike whatever the seed.  Finalizing the high half first spreads a flip of
 * any of its bits over the whole word, by an amount that depends on the
 * seed, before the low half can meet it.  No other key has two words that
 * can both reach bit 31: tuple and index keys are domain identifiers, which
 * reach it only once two billion values have been interned, and the one
 * other word that can, an adornment's column mask, is the last of its key.
 *
 * The halves are extracted by shifting rather than by reading the
 * representation's bytes, so the result does not depend on byte order.
 * Conversion to the unsigned type is a defined wraparound, so a negative
 * value mixes as well as a positive one.
 */
uint32_t
relic_hash_int64(uint64_t seed, int64_t value)
{
	uint64_t bits;
	uint32_t low;
	uint32_t high;

	bits = (uint64_t)value;
	low = (uint32_t)bits;
	high = (uint32_t)(bits >> 32);

	return (relic_hash_word(seed, low ^ relic_hash_word(seed, high)));
}

size_t
relic_grow_array_capacity(size_t capacity, size_t count)
{
	size_t wanted;

	if (count == 0)
		count = 1;

	if (count <= capacity)
		return (capacity);

	/*
	 * Doubling from here is not representable, and no allocator could
	 * serve an array this size in any case.
	 */
	if (capacity > SIZE_MAX / 2)
		return (0);

	/*
	 * Double, then raise the result to the smallest array worth having
	 * and to what the caller asked for.
	 */
	wanted = capacity * 2;
	if (wanted < RELIC_ARRAY_MIN_COUNT)
		wanted = RELIC_ARRAY_MIN_COUNT;
	if (wanted < count)
		wanted = count;

	return (wanted);
}

void *
relic_grow_array(void *items, size_t *capacity, size_t count, size_t item_size)
{
	void *grown;
	size_t wanted;

	wanted = relic_grow_array_capacity(*capacity, count);
	if (wanted == 0)
		return (NULL);

	if (wanted == *capacity)
		return (items);

	/*
	 * reallocarray() rejects a product that would overflow, so the
	 * multiplication by item_size needs no check here.
	 */
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
 * The capacity a growth would produce: twice the current size, or the
 * smallest table worth allocating when there is not one yet.  Zero says the
 * table has reached a size that cannot be doubled.
 */
static size_t
idtab_next_capacity(const relic_idtab_t *table)
{
	if (table->capacity < RELIC_IDTAB_MIN_CAPACITY)
		return (RELIC_IDTAB_MIN_CAPACITY);

	if (table->capacity > SIZE_MAX / 2 / sizeof(*table->slots))
		return (0);

	return (table->capacity * 2);
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

	capacity = idtab_next_capacity(table);
	if (capacity == 0)
		return (-1);

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

size_t
relic_idtab_bytes(const relic_idtab_t *table)
{
	return (table->capacity * sizeof(*table->slots));
}

size_t
relic_idtab_bytes_after_reserve(const relic_idtab_t *table)
{
	size_t capacity;

	if (!idtab_would_overfill(table))
		return (relic_idtab_bytes(table));

	capacity = idtab_next_capacity(table);
	if (capacity == 0)
		return (relic_idtab_bytes(table));

	return (capacity * sizeof(*table->slots));
}
