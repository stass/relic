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
 * The odd constant the fold multiplies its state by after mixing in each
 * unit of a key, the first multiplier of SplitMix64's mixer.  Its bits are
 * spread across the word: a multiplier with few bits set, such as FNV's
 * 2^40 + 435, satisfies a short polynomial relation modulo 2^64, which lets
 * a key's later units be chosen to cancel a difference its earlier ones
 * made under a fixed share of seeds.
 */
#define	RELIC_HASH_MULTIPLIER	UINT64_C(0xbf58476d1ce4e5b9)

/*
 * The state the unkeyed fold of a wide hash starts from, in
 * place of a seed: the golden ratio's fraction, an arbitrary constant whose
 * bits are mixed, so that a key of no words still has a stripe.
 */
#define	RELIC_STRIPE_FOLD_START	UINT64_C(0x9e3779b97f4a7c15)

/*
 * The smallest table worth allocating, and the smallest array worth growing
 * to.  Both are first allocations rather than thresholds: a table doubles
 * from here and an array doubles from there.
 */
#define	RELIC_IDTAB_MIN_CAPACITY	16u
#define	RELIC_ARRAY_MIN_COUNT		8u

static uint64_t	 finalize(uint64_t state);
static uint64_t	 hash_fold(uint64_t state, uint32_t unit);
static uint64_t	 hash_start(uint64_t seed);
static size_t	 idprobe_empty_slot(relic_idprobe_t *probe);
static size_t	 idtab_slot_bytes(const relic_idtab_t *table);
static bool	 idtab_capacity_holds(size_t capacity, size_t count,
		    size_t added);
static size_t	 idtab_capacity_for(const relic_idtab_t *table,
		    size_t added);
static int	 idtab_allocate(const relic_idtab_t *table, size_t capacity,
		    relic_idtab_t *grown);

/*
 * Avalanche the folded state into a hash so that every bit of the state
 * affects every bit of the hash, the low bits in particular.
 *
 * This matters more here than it would in a chained table.  A slot is chosen
 * with hash & (capacity - 1), so only the low bits select it, and collisions
 * under linear probing form runs that lengthen each other.  The fold alone
 * mixes poorly into its low bits for short keys, and sequential integer keys
 * -- which a Datalog program full of small integers produces -- would
 * otherwise probe in near-perfect runs.  This is the 64-bit finalizer of
 * MurmurHash3.  A hash kept in an unstriped table is its low 32 bits; a
 * striped table also reads its top bits.
 */
static uint64_t
finalize(uint64_t state)
{
	state ^= state >> 33;
	state *= UINT64_C(0xff51afd7ed558ccd);
	state ^= state >> 33;
	state *= UINT64_C(0xc4ceb9fe1a85ec53);
	state ^= state >> 33;

	return (state);
}

/*
 * Every hash is a keyed multiplicative fold over a 64-bit state, followed by
 * finalize():
 *
 *	state = seed
 *	for each unit u of the key:  state = (state ^ u) * multiplier
 *	hash  = finalize(state)
 *
 * A unit is a byte of a byte string or a word of a word sequence, never more
 * than half as wide as the state, and that is what stops keys from being
 * crafted to collide under every seed.  Multiplying by an odd number carries
 * a difference in the state's top bit through unchanged, whatever the other
 * bits hold, and changes every other difference by carries that depend on
 * those bits, and so on the seed.  No unit reaches the top bit, so every
 * difference a unit makes has been changed in a way only the seed decides
 * before the next unit meets it, and no later unit can be chosen to cancel
 * it under every seed.  A state as wide as a word would give a word's top
 * bit that path: two keys whose words differ in bit 31 in pairs would collide
 * under every seed, and keys differing only in their words' top bits, as
 * domain identifiers of different types do, would keep their differences
 * there and share a handful of hashes between all of them.
 *
 * hash_start(), hash_fold() and finalize() are those three steps, and each
 * hash function is written in terms of them so that none can drift from the
 * others.
 *
 * hash_start() also asserts that the seed is not zero.  No context holds a
 * seed of zero, so a zero is an owner that was never given its context's
 * seed, and since every hash starts here the one assertion checks every
 * owner.
 */
static uint64_t
hash_start(uint64_t seed)
{
	assert(seed != 0);

	return (seed);
}

static uint64_t
hash_fold(uint64_t state, uint32_t unit)
{
	return ((state ^ unit) * RELIC_HASH_MULTIPLIER);
}

uint32_t
relic_hash_bytes(uint64_t seed, const void *bytes, size_t length)
{
	const unsigned char *data;
	uint64_t state;
	size_t i;

	data = bytes;
	state = hash_start(seed);

	for (i = 0; i < length; i++)
		state = hash_fold(state, data[i]);

	return ((uint32_t)finalize(state));
}

/*
 * Mixing each word as a value rather than as its four bytes is what makes the
 * result independent of byte order.
 */
uint32_t
relic_hash_words(uint64_t seed, const uint32_t *words, size_t count)
{
	uint64_t state;
	size_t i;

	state = hash_start(seed);

	for (i = 0; i < count; i++)
		state = hash_fold(state, words[i]);

	return ((uint32_t)finalize(state));
}

/*
 * Two folds of the same words in one pass: the keyed one relic_hash_words()
 * makes, and an unkeyed one from a fixed state, whose high half is the
 * stripe's part.  Neither multiplication waits for the other, so the second
 * costs little beside the first.  The unkeyed fold is not finalized: the
 * stripe is read from its top bits, and a product's top bits depend on
 * every bit of its operands, which is what makes multiplicative hashing
 * spread keys that differ in any bit.
 */
uint64_t
relic_hash_words_wide(uint64_t seed, const uint32_t *words, size_t count)
{
	uint64_t keyed;
	uint64_t unkeyed;
	size_t i;

	keyed = hash_start(seed);
	unkeyed = RELIC_STRIPE_FOLD_START;

	for (i = 0; i < count; i++) {
		keyed = hash_fold(keyed, words[i]);
		unkeyed = hash_fold(unkeyed, words[i]);
	}

	return ((unkeyed & ~(uint64_t)UINT32_MAX) |
	    (uint32_t)finalize(keyed));
}

uint64_t
relic_hash_bytes_wide(uint64_t seed, const void *bytes, size_t length)
{
	const unsigned char *data;
	uint64_t keyed;
	uint64_t unkeyed;
	size_t i;

	data = bytes;
	keyed = hash_start(seed);
	unkeyed = RELIC_STRIPE_FOLD_START;

	for (i = 0; i < length; i++) {
		keyed = hash_fold(keyed, data[i]);
		unkeyed = hash_fold(unkeyed, data[i]);
	}

	return ((unkeyed & ~(uint64_t)UINT32_MAX) |
	    (uint32_t)finalize(keyed));
}

uint32_t
relic_hash_word(uint64_t seed, uint32_t word)
{
	return (relic_hash_words(seed, &word, 1));
}

/*
 * An integer hashes as the sequence of its high and low halves.  The halves
 * are extracted by shifting rather than by reading the representation's
 * bytes, so the result does not depend on byte order.  Conversion to the
 * unsigned type is a defined wraparound, so a negative value mixes as well
 * as a positive one.
 */
uint32_t
relic_hash_int64(uint64_t seed, int64_t value)
{
	uint32_t halves[2];
	uint64_t bits;

	bits = (uint64_t)value;
	halves[0] = (uint32_t)(bits >> 32);
	halves[1] = (uint32_t)bits;

	return (relic_hash_words(seed, halves, 2));
}

void *
relic_block_piece(char **cursorp, size_t *bytesp, size_t count, size_t size)
{
	size_t bytes;
	void *piece;

	bytes = relic_saturating_product(count, size);
	if (bytes > SIZE_MAX - (sizeof(uint64_t) - 1))
		bytes = SIZE_MAX;
	else
		bytes = (bytes + sizeof(uint64_t) - 1) &
		    ~(sizeof(uint64_t) - 1);
	*bytesp = relic_saturating_sum(*bytesp, bytes);

	if (*cursorp == NULL)
		return (NULL);
	piece = *cursorp;
	*cursorp += bytes;

	return (piece);
}

/*
 * Two passes: one counts each stripe's positions and makes the counts
 * where each stripe begins, and one writes each position at its stripe's
 * next place, which, walking the positions in order, leaves every stripe's
 * in increasing order.
 */
void
relic_list_by_stripe(const uint8_t *stripes, size_t count, uint32_t *list,
    size_t *starts)
{
	size_t next[RELIC_STRIPES];
	size_t i;
	size_t s;

	assert(count <= UINT32_MAX);

	bzero(next, sizeof(next));
	for (i = 0; i < count; i++) {
		assert(stripes[i] < RELIC_STRIPES);
		next[stripes[i]]++;
	}

	starts[0] = 0;
	for (s = 0; s < RELIC_STRIPES; s++) {
		starts[s + 1] = starts[s] + next[s];
		next[s] = starts[s];
	}

	for (i = 0; i < count; i++) {
		list[next[stripes[i]]] = (uint32_t)i;
		next[stripes[i]]++;
	}
}

size_t
relic_grow_array_capacity(size_t capacity, size_t count)
{
	size_t wanted;

	if (relic_grow_array_holds(capacity, count))
		return (capacity);
	if (count == 0)
		count = 1;

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

size_t
relic_grow_capacity_doubling(size_t capacity, size_t count)
{
	if (count <= capacity)
		return (capacity);

	if (capacity < RELIC_ARRAY_MIN_COUNT)
		capacity = RELIC_ARRAY_MIN_COUNT;
	while (capacity < count) {
		if (capacity > SIZE_MAX / 2)
			return (0);
		capacity *= 2;
	}

	return (capacity);
}

size_t
relic_grow_array_capacity_stepwise(size_t capacity, size_t held,
    size_t requests, size_t per_request)
{
	size_t end;
	size_t passing;

	assert(held <= capacity);

	if (requests == 0)
		return (capacity);
	if (per_request != 0 && requests > (SIZE_MAX - held) / per_request)
		return (0);
	end = held + requests * per_request;

	/*
	 * Requests of no items ask for no items, which relic_grow_array()
	 * rounds up to one: only the first of them can grow the array.
	 */
	if (end == 0)
		return (relic_grow_array_capacity(capacity, 0));

	/*
	 * Each pass finds the first request whose items the capacity cannot
	 * hold and grows for that request.  The request lies within the
	 * requests, since the capacity is short of their end, and the growth
	 * leaves the capacity holding it, so what is held after it is again
	 * within the capacity.
	 */
	while (capacity < end) {
		passing = held +
		    ((capacity - held) / per_request + 1) * per_request;
		capacity = relic_grow_array_capacity(capacity, passing);
		if (capacity == 0)
			return (0);
		held = passing;
	}

	return (capacity);
}

/*
 * The capacity the steps reach is either the capacity the array has, when
 * no step passes it, or at least twice it and at least the smallest array
 * worth having, since the first step that passes doubles it at least and
 * raises it to that smallest array.  relic_grow_array() asked for that many
 * items therefore grows to exactly that many.
 */
void *
relic_grow_array_stepwise(void *items, size_t *capacity, size_t held,
    size_t requests, size_t per_request, size_t item_size)
{
	size_t wanted;
	void *grown;

	assert(requests > 0);

	wanted = relic_grow_array_capacity_stepwise(*capacity, held, requests,
	    per_request);
	if (wanted == 0)
		return (NULL);

	grown = relic_grow_array(items, capacity, wanted, item_size);
	assert(grown == NULL || *capacity == wanted);

	return (grown);
}

void *
relic_grow_array(void *items, size_t *capacity, size_t count, size_t item_size)
{
	void *grown;
	size_t wanted_capacity;

	assert(item_size > 0);

	wanted_capacity = relic_grow_array_capacity(*capacity, count);
	if (wanted_capacity == 0)
		return (NULL);

	if (wanted_capacity == *capacity)
		return (items);

	/*
	 * An item of no bytes is a caller's error, which the assertion
	 * reports; a release build refuses it rather than divide by it.
	 */
	if (item_size == 0 || wanted_capacity > SIZE_MAX / item_size)
		return (NULL);

	grown = realloc(items, wanted_capacity * item_size);
	if (grown == NULL)
		return (NULL);

	*capacity = wanted_capacity;

	return (grown);
}

void
relic_idtab_init(relic_idtab_t *table, bool tagged)
{
	bzero(table, sizeof(*table));
	table->tagged = tagged;
}

void
relic_idtab_reset(relic_idtab_t *table)
{
	if (!table->given) {
		free(table->slots);
		free(table->tags);
	}
	relic_idtab_init(table, table->tagged);
}

void
relic_idtab_clear(relic_idtab_t *table)
{
	if (table->slots == NULL)
		return;

	bzero(table->slots, table->capacity * sizeof(*table->slots));
	if (table->tags != NULL)
		bzero(table->tags, table->capacity);
	table->count = 0;
}

void
relic_idtab_drop_tags(relic_idtab_t *table)
{
	/* A given table's tags are part of its owner's block. */
	assert(!table->given);
	if (table->given)
		return;

	free(table->tags);
	table->tags = NULL;
	table->tagged = false;
}

/*
 * A table given its memory lays out its slots first and its tags after
 * them, so that the slots keep the alignment the memory has.
 */
size_t
relic_idtab_bytes_for(bool tagged, size_t count)
{
	relic_idtab_t empty;
	size_t capacity;

	if (count == 0)
		return (0);

	relic_idtab_init(&empty, tagged);
	capacity = idtab_capacity_for(&empty, count);
	if (capacity == 0)
		return (SIZE_MAX);

	return (capacity * idtab_slot_bytes(&empty));
}

void
relic_idtab_init_given(relic_idtab_t *table, bool tagged, size_t count,
    void *memory)
{
	relic_idtab_init(table, tagged);
	if (count == 0)
		return;

	table->capacity = idtab_capacity_for(table, count);
	assert(table->capacity != 0);
	bzero(memory, table->capacity * idtab_slot_bytes(table));

	table->slots = memory;
	if (tagged)
		table->tags = (uint8_t *)(table->slots + table->capacity);
	table->given = true;
}

/*
 * The bytes one slot costs: its identifier, and its tag when the table keeps
 * them.
 */
static size_t
idtab_slot_bytes(const relic_idtab_t *table)
{
	return (sizeof(*table->slots) + (table->tagged ? sizeof(uint8_t) : 0));
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
	probe->tag = relic_idtab_tag(hash);
}

bool
relic_idprobe_next(relic_idprobe_t *probe, uint32_t *idp)
{
	const relic_idtab_t *table;
	size_t slot;
	uint32_t id;

	table = probe->table;
	if (table == NULL)
		return (false);

	/*
	 * The slot advances past every occupied slot the walk reaches, a
	 * candidate or a slot whose tag rules it out, and never past an empty
	 * one, so a probe that has returned false is left standing on the
	 * empty slot where the key belongs.  relic_idtab_place() relies on
	 * that.
	 */
	do {
		slot = probe->slot;
		id = table->slots[slot];
		if (id == RELIC_ID_NONE)
			return (false);

		probe->slot = (slot + 1) & probe->mask;
	} while (table->tags != NULL && table->tags[slot] != probe->tag);

	*idp = id;

	return (true);
}

/*
 * Whether `capacity` slots hold `count` entries and `added` more without
 * passing three quarters full.
 *
 * The load factor is a performance choice, but the fact that some slot stays
 * empty is a correctness one: a full table would make relic_idprobe_next()
 * circle forever instead of reporting that the key is absent.  A total that
 * does not fit a size_t fits no table.
 */
static bool
idtab_capacity_holds(size_t capacity, size_t count, size_t added)
{
	if (added > SIZE_MAX - count)
		return (false);

	return (count + added <= capacity - capacity / 4);
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
 * preceding relic_idtab_reserve() guarantees.  relic_idtab_complete_growth()
 * also uses this, on a table it has just allocated.
 */
void
relic_idtab_place(relic_idtab_t *table, uint32_t hash, uint32_t id)
{
	relic_idprobe_t probe;
	size_t slot;

	relic_idprobe_open(&probe, table, hash);
	slot = idprobe_empty_slot(&probe);
	table->slots[slot] = id;
	if (table->tags != NULL)
		table->tags[slot] = probe.tag;
	table->count++;
}

/*
 * The capacity a growth for `added` more entries would produce: the current
 * size doubled, or the smallest table worth allocating when there is not one
 * yet, and doubled again until the entries fit.  Zero says no table whose
 * size can be expressed holds them.
 *
 * The capacity stays a power of two, which is what lets a probe mask a hash
 * into a slot.
 */
static size_t
idtab_capacity_for(const relic_idtab_t *table, size_t added)
{
	size_t capacity;

	capacity = table->capacity;
	do {
		if (capacity < RELIC_IDTAB_MIN_CAPACITY)
			capacity = RELIC_IDTAB_MIN_CAPACITY;
		else if (capacity > SIZE_MAX / 2 / idtab_slot_bytes(table))
			return (0);
		else
			capacity *= 2;
	} while (!idtab_capacity_holds(capacity, table->count, added));

	return (capacity);
}

/*
 * Make `grown` an empty table of the same kind as `table`, with arrays of
 * `capacity` slots.  Returns 0, or -1 with `grown` holding nothing.
 */
static int
idtab_allocate(const relic_idtab_t *table, size_t capacity,
    relic_idtab_t *grown)
{
	/*
	 * idtab_capacity_for() stops doubling before a capacity whose slots
	 * and tags together would not fit in a size_t.
	 */
	assert(capacity <= SIZE_MAX / idtab_slot_bytes(table));

	relic_idtab_init(grown, table->tagged);
	grown->slots = calloc(capacity, sizeof(*grown->slots));
	if (grown->slots == NULL)
		return (-1);
	if (grown->tagged) {
		grown->tags = calloc(capacity, sizeof(*grown->tags));
		if (grown->tags == NULL) {
			free(grown->slots);
			grown->slots = NULL;
			return (-1);
		}
	}
	grown->capacity = capacity;

	return (0);
}

bool
relic_idtab_has_room(const relic_idtab_t *table, size_t added)
{
	return (idtab_capacity_holds(table->capacity, table->count, added));
}

/*
 * A table grows by allocating its replacement, the half that can fail, and
 * then reinserting every entry and installing the replacement in one
 * assignment, which cannot.  A failed allocation therefore leaves the table
 * exactly as it was rather than half rehashed.
 */
int
relic_idtab_reserve(relic_idtab_t *table, size_t added,
    relic_rehash_fn rehash, void *ctx)
{
	relic_idtab_t grown;

	/* Most reservations find the room already there. */
	if (idtab_capacity_holds(table->capacity, table->count, added))
		return (0);

	if (relic_idtab_prepare_growth(table, added, &grown) != 0)
		return (-1);

	relic_idtab_complete_growth(table, &grown, rehash, ctx);

	return (0);
}

int
relic_idtab_prepare_growth(const relic_idtab_t *table, size_t added,
    relic_idtab_t *grown)
{
	size_t capacity;

	relic_idtab_init(grown, table->tagged);
	if (idtab_capacity_holds(table->capacity, table->count, added))
		return (0);

	/* A table given its memory was made with room for all it holds. */
	assert(!table->given);
	if (table->given)
		return (-1);

	capacity = idtab_capacity_for(table, added);
	if (capacity == 0)
		return (-1);

	return (idtab_allocate(table, capacity, grown));
}

void
relic_idtab_complete_growth(relic_idtab_t *table, relic_idtab_t *grown,
    relic_rehash_fn rehash, void *ctx)
{
	uint32_t id;
	size_t slot;

	if (grown->slots == NULL)
		return;

	assert(grown->count == 0 && grown->tagged == table->tagged);
	assert(!table->given);

	for (slot = 0; slot < table->capacity; slot++) {
		id = table->slots[slot];
		if (id == RELIC_ID_NONE)
			continue;

		relic_idtab_place(grown, rehash(id, ctx), id);
	}

	free(table->slots);
	free(table->tags);
	*table = *grown;
	relic_idtab_init(grown, table->tagged);
}

size_t
relic_idtab_bytes(const relic_idtab_t *table)
{
	return (table->capacity * idtab_slot_bytes(table));
}

size_t
relic_idtab_bytes_untagged(const relic_idtab_t *table)
{
	return (table->capacity * sizeof(*table->slots));
}

bool
relic_idtab_capacity_after_reserve(const relic_idtab_t *table, size_t added,
    size_t *capacityp)
{
	*capacityp = table->capacity;
	if (idtab_capacity_holds(table->capacity, table->count, added))
		return (true);
	if (table->given)
		return (false);

	*capacityp = idtab_capacity_for(table, added);

	return (*capacityp != 0);
}

int
relic_idtab_grow_to(relic_idtab_t *table, size_t capacity,
    relic_rehash_fn rehash, void *ctx)
{
	relic_idtab_t grown;

	if (capacity == table->capacity)
		return (0);

	assert(capacity > table->capacity && !table->given);
	assert((capacity & (capacity - 1)) == 0);
	if (idtab_allocate(table, capacity, &grown) != 0)
		return (-1);
	relic_idtab_complete_growth(table, &grown, rehash, ctx);

	return (0);
}

size_t
relic_idtab_bytes_after_reserve(const relic_idtab_t *table, size_t added)
{
	size_t capacity;

	if (!relic_idtab_capacity_after_reserve(table, added, &capacity))
		return (relic_idtab_bytes(table));

	return (capacity * idtab_slot_bytes(table));
}

size_t
relic_idtab_bytes_replaced_by_reserve(const relic_idtab_t *table,
    size_t added)
{
	if (idtab_capacity_holds(table->capacity, table->count, added))
		return (0);

	return (relic_idtab_bytes(table));
}
