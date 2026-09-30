/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <sys/mman.h>

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include "internal.h"

/*
 * Under AddressSanitizer an array's header, and a mapping's bytes past its
 * last item, are marked unaddressable, so the sanitizer reports a write just
 * outside the array as it would for a plain heap block.  Without it the
 * marking is nothing, as the sanitizer's own header defines it.
 */
#ifdef RELIC_ADDRESS_SANITIZER
#include <sanitizer/asan_interface.h>
#else
#define	ASAN_POISON_MEMORY_REGION(start, bytes)	((void)(start), (void)(bytes))
#define	ASAN_UNPOISON_MEMORY_REGION(start, bytes) ((void)(start), (void)(bytes))
#endif

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
 * The state the unkeyed fold of relic_hash_words_wide() starts from, in
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

/*
 * The record in front of the first item of every growable array, saying how
 * its storage was obtained: the length of its mapping, or zero for a heap
 * block.  relic_free_array() reads it to choose between munmap(2) and
 * free(3), so no caller has to remember which storage an array got.
 *
 * The record is the last thing before the first item, where a write just
 * before the array lands, and a length read from a damaged record would
 * unmap memory another array is using.  So the record carries a seal: the
 * complement of the record's own address folded with its length.  Zeros,
 * ordinary data, a pointer to the record itself and a record copied from
 * another array all fail it, and every read of a record checks it first.
 */
typedef struct relic_array_record relic_array_record_t;

struct relic_array_record {
	size_t		mapped_bytes;
	uintptr_t	seal;
};

/*
 * The bytes in front of the first item: the record, and before it whatever
 * padding leaves the items as aligned as malloc(3) would have left them.
 * The array's storage begins here, and it is what free(3), realloc(3) and
 * munmap(2) are given.
 */
#define	RELIC_ARRAY_HEADER_BYTES					\
	((sizeof(relic_array_record_t) + _Alignof(max_align_t) - 1) /	\
	    _Alignof(max_align_t) * _Alignof(max_align_t))

/*
 * The mapped length a record gives a heap block: none.
 */
#define	RELIC_ARRAY_HEAP_BLOCK	((size_t)0)

/*
 * A heap block's size is its header and fewer bytes of items than the
 * threshold, a sum that must not overflow whatever the threshold is set to.
 */
_Static_assert(RELIC_ARRAY_MAP_BYTES <= SIZE_MAX - RELIC_ARRAY_HEADER_BYTES,
    "a heap array's size must fit a size_t");

static void	*array_allocate(size_t bytes);
static void	*array_allocate_zeroed(size_t bytes);
static relic_array_record_t array_checked_record(const void *items);
static void	*array_install_record(void *storage, size_t mapped_bytes);
static bool	 array_is_large(size_t bytes);
static void	*array_map(size_t bytes);
static void	*array_resize(void *items, size_t kept_bytes,
		    size_t wanted_bytes);
static void	*array_storage(void *items);
static _Noreturn void record_corrupt(const relic_array_record_t *stored);
static uintptr_t record_seal(const relic_array_record_t *record,
		    size_t mapped_bytes);
static uint64_t	 finalize(uint64_t state);
static uint64_t	 hash_fold(uint64_t state, uint32_t unit);
static uint64_t	 hash_start(uint64_t seed);
static size_t	 idprobe_empty_slot(relic_idprobe_t *probe);
static size_t	 idtab_slot_bytes(const relic_idtab_t *table);
static uint8_t	 idtab_tag(uint32_t hash);
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
	size_t kept_bytes;
	size_t wanted_bytes;

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

	kept_bytes = *capacity * item_size;
	wanted_bytes = wanted_capacity * item_size;

	grown = array_resize(items, kept_bytes, wanted_bytes);
	if (grown == NULL)
		return (NULL);

	*capacity = wanted_capacity;

	return (grown);
}

void
relic_free_array(void *items)
{
	relic_array_record_t record;
	void *storage;

	if (items == NULL)
		return;

	record = array_checked_record(items);
	storage = array_storage(items);

	if (record.mapped_bytes == RELIC_ARRAY_HEAP_BLOCK) {
		free(storage);
		return;
	}

	/*
	 * The record is sealed, so the range is exactly the mapping mmap(2)
	 * returned.  munmap(2) can still refuse one for want of kernel
	 * memory, where the kernel had merged the mapping with a neighbour
	 * and must split the merged region to release it.  The pages then
	 * stay mapped and unused, which costs memory and nothing else.
	 */
	/*
	 * The pages are unmarked first: a sanitizer runtime that does not see
	 * munmap(2) would otherwise hold them unaddressable for whatever is
	 * mapped there next.
	 */
	ASAN_UNPOISON_MEMORY_REGION(storage, record.mapped_bytes);
	if (munmap(storage, record.mapped_bytes) != 0)
		assert(errno == ENOMEM);
}

bool
relic_array_mapped(const void *items)
{
	if (items == NULL)
		return (false);

	return (array_checked_record(items).mapped_bytes !=
	    RELIC_ARRAY_HEAP_BLOCK);
}

/*
 * The start of an array's storage, a header's length in front of its first
 * item.
 */
static void *
array_storage(void *items)
{
	return ((char *)items - RELIC_ARRAY_HEADER_BYTES);
}

/*
 * The record in front of the first item, once its seal has been checked.  A
 * record that fails its seal stops the process: nothing it says can be
 * trusted, and the storage it describes cannot be released without it.
 */
static relic_array_record_t
array_checked_record(const void *items)
{
	const relic_array_record_t *stored;
	relic_array_record_t record;

	stored = (const relic_array_record_t *)items - 1;

	ASAN_UNPOISON_MEMORY_REGION(stored, sizeof(*stored));
	record = *stored;
	ASAN_POISON_MEMORY_REGION(stored, sizeof(*stored));

	if (record.seal != record_seal(stored, record.mapped_bytes))
		record_corrupt(stored);

	return (record);
}

/*
 * Stop the process over a record that failed its seal.  Under
 * AddressSanitizer the sanitizer first says what the record's address is --
 * inside a block already freed, in a redzone, in memory it never handed out
 * -- with the stacks it kept for it, which is the diagnosis of an array
 * released twice or of a pointer that was never an array's.
 */
static _Noreturn void
record_corrupt(const relic_array_record_t *stored RELIC_UNUSED)
{
#ifdef RELIC_ADDRESS_SANITIZER
	__asan_describe_address((void *)(uintptr_t)stored);
#endif
	abort();
}

static uintptr_t
record_seal(const relic_array_record_t *record, size_t mapped_bytes)
{
	return (~((uintptr_t)record ^ (uintptr_t)mapped_bytes));
}

/*
 * Write the sealed record into new storage, and hide the whole header from
 * the sanitizer.  Returns the first item.
 */
static void *
array_install_record(void *storage, size_t mapped_bytes)
{
	relic_array_record_t *record;
	char *items;

	items = (char *)storage + RELIC_ARRAY_HEADER_BYTES;
	record = (relic_array_record_t *)(void *)items - 1;

	ASAN_UNPOISON_MEMORY_REGION(storage, RELIC_ARRAY_HEADER_BYTES);
	record->mapped_bytes = mapped_bytes;
	record->seal = record_seal(record, mapped_bytes);
	ASAN_POISON_MEMORY_REGION(storage, RELIC_ARRAY_HEADER_BYTES);

	return (items);
}

/*
 * Whether an array of this many bytes gets a mapping of its own.
 */
static bool
array_is_large(size_t bytes)
{
	return (bytes >= RELIC_ARRAY_MAP_BYTES);
}

/*
 * Storage for `bytes` bytes of items behind their header, the items'
 * contents unspecified.  A large array is mapped and a small one is a heap
 * block, whose size cannot overflow because it is below the threshold.
 * Returns the first item, or NULL when nothing could be allocated.
 */
static void *
array_allocate(size_t bytes)
{
	void *storage;

	if (array_is_large(bytes))
		return (array_map(bytes));

	storage = malloc(RELIC_ARRAY_HEADER_BYTES + bytes);
	if (storage == NULL)
		return (NULL);

	return (array_install_record(storage, RELIC_ARRAY_HEAP_BLOCK));
}

/*
 * Storage as array_allocate() gives it, with every item zero.  A mapping's
 * pages arrive zeroed, so only a heap block is cleared.
 */
static void *
array_allocate_zeroed(size_t bytes)
{
	void *storage;

	if (array_is_large(bytes))
		return (array_map(bytes));

	storage = calloc(1, RELIC_ARRAY_HEADER_BYTES + bytes);
	if (storage == NULL)
		return (NULL);

	return (array_install_record(storage, RELIC_ARRAY_HEAP_BLOCK));
}

/*
 * A mapping of its own for `bytes` bytes of items behind their header,
 * rounded up to whole pages, which arrive zeroed.  The bytes past the last
 * item are hidden from the sanitizer, as the redzone after a heap block is.
 * Returns the first item, or NULL when nothing could be mapped.
 */
static void *
array_map(size_t bytes)
{
	void *region;
	size_t page_bytes;
	size_t used_bytes;
	size_t mapped_bytes;
	int page_size;

	page_size = getpagesize();
	page_bytes = (size_t)page_size;

	if (bytes > SIZE_MAX - RELIC_ARRAY_HEADER_BYTES - page_bytes)
		return (NULL);
	used_bytes = RELIC_ARRAY_HEADER_BYTES + bytes;
	mapped_bytes = used_bytes + page_bytes - 1;
	mapped_bytes -= mapped_bytes % page_bytes;

	region = mmap(NULL, mapped_bytes, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | RELIC_MAP_ANON, -1, 0);
	if (region == MAP_FAILED)
		return (NULL);

	ASAN_POISON_MEMORY_REGION((char *)region + used_bytes,
	    mapped_bytes - used_bytes);

	return (array_install_record(region, mapped_bytes));
}

/*
 * Give an array room for `wanted_bytes` bytes, keeping its first
 * `kept_bytes`.
 *
 * A heap block that stays small is resized by realloc(3), which may extend
 * it where it lies.  Every other growth -- a heap block becoming large, or a
 * mapping growing -- takes new storage, copies the kept bytes into it and
 * releases the old, so the old generation leaves the resident set as soon as
 * the copy is made.  On failure the array is left exactly as it was.
 */
static void *
array_resize(void *items, size_t kept_bytes, size_t wanted_bytes)
{
	void *resized;
	void *moved;
	bool stays_in_heap;

	if (items == NULL)
		return (array_allocate(wanted_bytes));

	stays_in_heap = array_checked_record(items).mapped_bytes ==
	    RELIC_ARRAY_HEAP_BLOCK && !array_is_large(wanted_bytes);

	if (stays_in_heap) {
		resized = realloc(array_storage(items),
		    RELIC_ARRAY_HEADER_BYTES + wanted_bytes);
		if (resized == NULL)
			return (NULL);
		return (array_install_record(resized, RELIC_ARRAY_HEAP_BLOCK));
	}

	moved = array_allocate(wanted_bytes);
	if (moved == NULL)
		return (NULL);
	bcopy(items, moved, kept_bytes);
	relic_free_array(items);

	return (moved);
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
	relic_free_array(table->slots);
	relic_free_array(table->tags);
	relic_idtab_init(table, table->tagged);
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

/*
 * The tag of a hash: its top eight bits.  A table of at most 2^24 slots
 * chooses a slot from bits below them, so the tag and the slot are
 * independent there, and above that size they overlap and a tag passes over
 * fewer mismatches, which costs time and nothing else.
 */
static uint8_t
idtab_tag(uint32_t hash)
{
	return ((uint8_t)(hash >> 24));
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
	probe->tag = idtab_tag(hash);
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
	grown->slots = array_allocate_zeroed(capacity * sizeof(*grown->slots));
	if (grown->slots == NULL)
		return (-1);
	if (grown->tagged) {
		grown->tags = array_allocate_zeroed(capacity *
		    sizeof(*grown->tags));
		if (grown->tags == NULL) {
			relic_free_array(grown->slots);
			grown->slots = NULL;
			return (-1);
		}
	}
	grown->capacity = capacity;

	return (0);
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

	for (slot = 0; slot < table->capacity; slot++) {
		id = table->slots[slot];
		if (id == RELIC_ID_NONE)
			continue;

		relic_idtab_place(grown, rehash(id, ctx), id);
	}

	relic_free_array(table->slots);
	relic_free_array(table->tags);
	*table = *grown;
	relic_idtab_init(grown, table->tagged);
}

size_t
relic_idtab_bytes(const relic_idtab_t *table)
{
	return (table->capacity * idtab_slot_bytes(table));
}

size_t
relic_idtab_bytes_after_reserve(const relic_idtab_t *table, size_t added)
{
	size_t capacity;

	if (idtab_capacity_holds(table->capacity, table->count, added))
		return (relic_idtab_bytes(table));

	capacity = idtab_capacity_for(table, added);
	if (capacity == 0)
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
