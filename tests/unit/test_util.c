/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the growable array, the identifier table and the keyed
 * hashes the table is used with.
 *
 * The identifier table is used by the symbol pool, the value table and every
 * relation index, so a probe-sequence defect here would appear as several
 * unrelated bugs.  The tests therefore drive it as its callers do,
 * through a key that lives outside the table, and check the properties those
 * callers depend on: a key that was inserted is found, a key that was not is
 * reported absent, and both survive the table doubling underneath them.
 * Each of those runs on a tagged table and an untagged one, and a tagged
 * probe is shown to pass over the identifiers placed under other tags.
 */

#include "config.h"

#include <sys/mman.h>
#include <sys/wait.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "harness.h"
#include "internal.h"

#ifdef RELIC_ADDRESS_SANITIZER
#include <sanitizer/asan_interface.h>
#else
#define	ASAN_POISON_MEMORY_REGION(start, bytes)	((void)(start), (void)(bytes))
#define	ASAN_UNPOISON_MEMORY_REGION(start, bytes) ((void)(start), (void)(bytes))
#endif

#define	SCENARIO_CONTEXTS	"each context draws a seed of its own"
#define	SCENARIO_DAMAGED	"a damaged array header stops the process"
#define	SCENARIO_DETERMINISM	"hashing is reproducible within a seed"
#define	SCENARIO_FENCED		"the sanitizer sees past an array's ends"
#define	SCENARIO_FLOODING	"keys crafted to collide need the seed"
#define	SCENARIO_GROWTH		"an array grows without losing its contents"
#define	SCENARIO_KEYED		"a hash depends on its seed"
#define	SCENARIO_LOOKUP		"a key survives the table growing under it"
#define	SCENARIO_MAPPED		"a large array gets pages of its own"
#define	SCENARIO_OVERFLOW	"an impossible array size is refused"
#define	SCENARIO_PREDICTED	"a growth is exactly what was predicted"
#define	SCENARIO_RELEASED	"an outgrown mapping is released whole"
#define	SCENARIO_RESERVED	"a reservation for many holds them all"
#define	SCENARIO_SPREAD		"hashing separates similar keys"
#define	SCENARIO_TAGS		"a tagged probe passes over other tags"

/*
 * Two seeds chosen by the tests.  A context draws its seed at random, and a
 * test that hashes directly states the seed it hashes with, so that what it
 * checks does not depend on the draw.
 */
#define	SEED		UINT64_C(0x0123456789abcdef)
#define	OTHER_SEED	UINT64_C(0xfedcba9876543210)

/*
 * The columns of the key whose type variants are hashed, and the number of
 * variants, three types to a column.
 */
#define	VARIANT_COLUMNS	8
#define	VARIANT_COUNT	6561
_Static_assert(VARIANT_COLUMNS == 8 &&
    VARIANT_COUNT == 3 * 3 * 3 * 3 * 3 * 3 * 3 * 3,
    "VARIANT_COUNT is three to the power VARIANT_COLUMNS");

/*
 * The seeds a crafted pair of keys is tried under, and the bit a domain
 * identifier's type reaches highest.
 */
#define	CHANNEL_SEEDS	16384
#define	TOP_BIT		UINT32_C(0x80000000)

/*
 * The record in front of a growable array's first item: the two words just
 * before it, the array's mapped length and its seal.
 */
#define	RECORD_BYTES	(sizeof(size_t) + sizeof(uintptr_t))

/*
 * The status a child freeing an array exits with from its SIGABRT handler,
 * which says the library stopped it with abort(3).
 */
#define	ABORTED_STATUS	86

/*
 * The keys the identifier table is driven with.  Identifier n names keys[n-1],
 * which is exactly how the symbol pool and the value table use it: the table
 * stores an identifier and the owner looks the key up by it.  Like every
 * owner, the set holds the seed its keys are hashed with.
 */
typedef struct key_set key_set_t;

struct key_set {
	uint32_t	*keys;
	size_t		 count;
	uint64_t	 seed;
};

static uint32_t	 key_rehash(uint32_t id, void *ctx);
static uint32_t	 counting_rehash(uint32_t id, void *ctx);
static size_t	 capacity_reserved(size_t held, size_t added);
static bool	 key_set_find(const relic_idtab_t *table,
		    const key_set_t *keys, uint32_t key, uint32_t *idp);
static bool	 key_set_insert(relic_idtab_t *table, key_set_t *keys,
		    uint32_t key);
static size_t	 longest_walk(const relic_idtab_t *table,
		    const key_set_t *keys);
static bool	 reserve_then_fill(size_t held, size_t added, bool tagged);
static size_t	 walk_to(const relic_idtab_t *table, const key_set_t *keys,
		    uint32_t key);
static void	 an_array_grows_without_losing_its_contents(void);
static void	 an_array_is_fenced_for_the_sanitizer(void);
static void	 an_outgrown_mapping_is_released(void);
static void	 a_damaged_header_stops_the_process(void);
static void	 exit_aborted(int signal_number);
static bool	 freeing_aborts_with(uint32_t *items,
		    const unsigned char *record);
static void	 inverted_record(uint32_t *items, size_t offset,
		    unsigned char *record);
static unsigned char *record_of(uint32_t *items);
static bool	 page_is_mapped(uintptr_t address);
static void	 an_array_refuses_an_impossible_size(void);
static void	 a_large_array_moves_to_a_mapping(void);
static void	 a_large_table_starts_mapped_and_empty(void);
static void	 equal_input_hashes_equally(void);
static void	 every_inserted_key_is_found_after_growth(void);
static void	 found_after_growth(bool tagged);
static void	 a_growth_is_exactly_what_was_predicted(void);
static void	 a_hash_depends_on_its_seed(void);
static void	 a_reservation_for_many_holds_them_all(void);
static void	 a_reservation_is_exactly_what_was_predicted(void);
static void	 reserved_as_predicted(bool tagged);
static void	 a_tagged_probe_passes_over_other_tags(void);
static uint32_t	 stored_hash(uint32_t id, void *ctx);
static size_t	 candidates_for(const relic_idtab_t *table, uint32_t hash);
static void	 a_word_sequence_hashes_to_a_fixed_value(void);
static void	 colliding_keys_collide_only_under_their_seed(void);
static void	 each_context_draws_a_seed_of_its_own(void);
static void	 integers_differing_in_bit_31_of_each_half_differ(void);
static int	 order_hashes(const void *left, const void *right);
static void	 similar_keys_reach_different_slots(void);
static void	 three_word_differences_are_not_cancelled(void);
static void	 top_bit_differences_are_not_cancelled(void);
static void	 type_variants_of_a_key_spread(void);

static uint32_t
key_rehash(uint32_t id, void *ctx)
{
	const key_set_t *keys;

	keys = ctx;

	return (relic_hash_word(keys->seed, keys->keys[id - 1]));
}

/*
 * A key set's rehash that also counts its calls, which is how many entries a
 * growth moved.
 */
typedef struct counted_keys counted_keys_t;

struct counted_keys {
	key_set_t	*keys;
	size_t		 rehashes;
};

static uint32_t
counting_rehash(uint32_t id, void *ctx)
{
	counted_keys_t *counted;

	counted = ctx;
	counted->rehashes++;

	return (key_rehash(id, counted->keys));
}

/*
 * Look a key up the way a real owner does: walk the candidates for its hash
 * and compare the key each identifier names.
 */
static bool
key_set_find(const relic_idtab_t *table, const key_set_t *keys, uint32_t key,
    uint32_t *idp)
{
	relic_idprobe_t probe;
	uint32_t id;

	relic_idprobe_open(&probe, table, relic_hash_word(keys->seed, key));
	while (relic_idprobe_next(&probe, &id)) {
		if (keys->keys[id - 1] == key) {
			*idp = id;
			return (true);
		}
	}

	return (false);
}

/*
 * Add a key to the set and the table the way an owner interns one: reserve,
 * record the key, then place.  The step that can fail runs before the key
 * exists, so a failure leaves nothing behind to be found or not found.
 * keys->keys must have room for one more.
 */
static bool
key_set_insert(relic_idtab_t *table, key_set_t *keys, uint32_t key)
{
	if (relic_idtab_reserve(table, 1, key_rehash, keys) != 0)
		return (false);

	keys->keys[keys->count] = key;
	keys->count++;
	relic_idtab_place(table, relic_hash_word(keys->seed, key),
	    (uint32_t)keys->count);

	return (true);
}

/*
 * How many identifiers a lookup of an inserted key compares before it finds
 * the key: one when the key sits in the slot its hash selects, and one more
 * for every entry of the cluster ahead of it.
 */
static size_t
walk_to(const relic_idtab_t *table, const key_set_t *keys, uint32_t key)
{
	relic_idprobe_t probe;
	uint32_t id;
	size_t walked;

	walked = 0;
	relic_idprobe_open(&probe, table, relic_hash_word(keys->seed, key));
	while (relic_idprobe_next(&probe, &id)) {
		walked++;
		if (keys->keys[id - 1] == key)
			break;
	}

	return (walked);
}

/* The longest walk_to() over every key of the set. */
static size_t
longest_walk(const relic_idtab_t *table, const key_set_t *keys)
{
	size_t longest;
	size_t walked;
	size_t i;

	longest = 0;
	for (i = 0; i < keys->count; i++) {
		walked = walk_to(table, keys, keys->keys[i]);
		if (walked > longest)
			longest = walked;
	}

	return (longest);
}

/*
 * Insert more keys than any one slot array can hold, so the table doubles
 * several times, then look every key up again.  A probe sequence that
 * disagreed with the one insertion used, or a rehash that dropped an entry,
 * would lose keys here rather than in a subsystem far from the cause.
 *
 * The keys are spaced apart rather than consecutive so that a table which
 * happened to work only for dense keys does not pass.
 */
static void
every_inserted_key_is_found_after_growth(void)
{
	found_after_growth(false);
	found_after_growth(true);
}

/*
 * The growth scenario on a table created tagged or not.
 */
static void
found_after_growth(bool tagged)
{
	enum { KEY_COUNT = 5000 };

	relic_idtab_t table;
	key_set_t keys;
	uint32_t storage[KEY_COUNT];
	size_t first_uninserted;
	size_t first_lost;
	size_t first_spurious;
	size_t i;

	keys.keys = storage;
	keys.count = 0;
	keys.seed = SEED;
	relic_idtab_init(&table, tagged);

	first_uninserted = KEY_COUNT;
	for (i = 0; i < KEY_COUNT; i++) {
		uint32_t key;

		key = (uint32_t)(i * 7919u + 13u);
		if (!key_set_insert(&table, &keys, key)) {
			first_uninserted = i;
			break;
		}
	}

	checkf(first_uninserted == KEY_COUNT, SCENARIO_LOOKUP,
	    "insertion %zu of %d should have succeeded, %s", first_uninserted,
	    KEY_COUNT, tagged ? "tagged" : "untagged");
	check(table.count == KEY_COUNT, SCENARIO_LOOKUP,
	    "the table should hold every identifier inserted");
	check(table.count < table.capacity, SCENARIO_LOOKUP,
	    "the table should keep an empty slot so a probe terminates");

	if (first_uninserted != KEY_COUNT) {
		relic_idtab_reset(&table);
		return;
	}

	first_lost = KEY_COUNT;
	for (i = 0; i < KEY_COUNT && first_lost == KEY_COUNT; i++) {
		uint32_t found;

		if (!key_set_find(&table, &keys, storage[i], &found) ||
		    found != (uint32_t)(i + 1))
			first_lost = i;
	}

	checkf(first_lost == KEY_COUNT, SCENARIO_LOOKUP,
	    "key %zu should still name the identifier it was inserted with, "
	    "%s", first_lost, tagged ? "tagged" : "untagged");

	/*
	 * A table that reported a hit for a key it never held would make
	 * interning return a wrong identifier rather than assign a new one,
	 * which is the more damaging direction of the same defect.
	 */
	first_spurious = KEY_COUNT;
	for (i = 0; i < KEY_COUNT && first_spurious == KEY_COUNT; i++) {
		uint32_t found;

		if (key_set_find(&table, &keys, (uint32_t)(i * 7919u + 14u),
		    &found))
			first_spurious = i;
	}

	checkf(first_spurious == KEY_COUNT, SCENARIO_LOOKUP,
	    "key %zu was never inserted and should not be found, %s",
	    first_spurious, tagged ? "tagged" : "untagged");

	relic_idtab_reset(&table);
	check(table.slots == NULL && table.count == 0, SCENARIO_LOOKUP,
	    "a reset table should hold nothing");
}

static void
an_array_grows_without_losing_its_contents(void)
{
	enum { ITEM_COUNT = 1000 };

	uint32_t *items;
	size_t capacity;
	size_t count;
	size_t first_ungrown;
	size_t first_corrupted;
	size_t i;

	items = NULL;
	capacity = 0;
	count = 0;

	first_ungrown = ITEM_COUNT;
	for (i = 0; i < ITEM_COUNT; i++) {
		uint32_t *grown;

		grown = relic_grow_array(items, &capacity, count + 1,
		    sizeof(*items));
		if (grown == NULL) {
			first_ungrown = i;
			break;
		}
		items = grown;
		items[count] = (uint32_t)i;
		count++;
	}

	checkf(first_ungrown == ITEM_COUNT, SCENARIO_GROWTH,
	    "growth %zu of %d should have succeeded", first_ungrown,
	    ITEM_COUNT);
	check(capacity >= count, SCENARIO_GROWTH,
	    "capacity should cover the items stored");

	if (first_ungrown != ITEM_COUNT) {
		relic_free_array(items);
		return;
	}

	first_corrupted = ITEM_COUNT;
	for (i = 0; i < ITEM_COUNT && first_corrupted == ITEM_COUNT; i++) {
		if (items[i] != (uint32_t)i)
			first_corrupted = i;
	}

	checkf(first_corrupted == ITEM_COUNT, SCENARIO_GROWTH,
	    "item %zu should have survived the reallocations",
	    first_corrupted);

	relic_free_array(items);
}

/*
 * A count that cannot be allocated leaves the caller's array and capacity
 * exactly as they were, so a failed growth loses nothing already stored.
 */
static void
an_array_refuses_an_impossible_size(void)
{
	uint32_t *items;
	uint32_t *grown;
	size_t capacity;

	items = NULL;
	capacity = 0;

	grown = relic_grow_array(items, &capacity, SIZE_MAX, sizeof(*items));

	check(grown == NULL, SCENARIO_OVERFLOW,
	    "an unrepresentable array should not be allocated");
	check(capacity == 0, SCENARIO_OVERFLOW,
	    "a refused growth should leave the capacity unchanged");
}

/*
 * An array appended to one item at a time stays in the heap while it is
 * small, moves to a mapping of its own at the growth that takes its bytes to
 * RELIC_ARRAY_MAP_BYTES, and moves from mapping to mapping after that, with
 * every item surviving each move.  The array is grown to twice the threshold
 * so that both kinds of move happen.  A growth that chose its storage by
 * anything but the bytes it now holds -- the bytes it held before, or the
 * item count -- is seen as the first growth whose storage is wrong.
 */
static void
a_large_array_moves_to_a_mapping(void)
{
	uint32_t *items;
	size_t capacity;
	size_t count;
	size_t item_count;
	size_t wrong_storage_at;
	size_t first_corrupted;
	size_t i;
	bool grown_all;

	items = NULL;
	capacity = 0;
	count = 0;
	item_count = 2 * RELIC_ARRAY_MAP_BYTES / sizeof(*items) + 1;
	wrong_storage_at = 0;
	grown_all = true;

	for (i = 0; i < item_count; i++) {
		uint32_t *grown;
		bool large;

		grown = relic_grow_array(items, &capacity, count + 1,
		    sizeof(*items));
		if (grown == NULL) {
			grown_all = false;
			break;
		}
		items = grown;
		items[count] = (uint32_t)i;
		count++;

		large = capacity * sizeof(*items) >= RELIC_ARRAY_MAP_BYTES;
		if (wrong_storage_at == 0 && relic_array_mapped(items) != large)
			wrong_storage_at = capacity;
	}

	check(grown_all, SCENARIO_MAPPED,
	    "an array of twice the threshold should grow");
	checkf(wrong_storage_at == 0, SCENARIO_MAPPED,
	    "an array of %zu items should be mapped exactly when its bytes "
	    "reach the threshold", wrong_storage_at);
	check(relic_array_mapped(items), SCENARIO_MAPPED,
	    "an array of twice the threshold should be mapped");

	first_corrupted = count;
	for (i = 0; i < count && first_corrupted == count; i++) {
		if (items[i] != (uint32_t)i)
			first_corrupted = i;
	}
	checkf(first_corrupted == count, SCENARIO_MAPPED,
	    "item %zu should have survived the moves", first_corrupted);

	relic_free_array(items);
	check(!relic_array_mapped(NULL), SCENARIO_MAPPED,
	    "a null array should not count as mapped");
}

/*
 * A mapping an array has outgrown is released once its items are copied
 * out, and all of it: that release is what keeps a growth's old generation
 * out of the resident set.  So is the last mapping when the array is freed.
 * Both ends of each released range are probed, so releasing half a mapping
 * fails as surely as releasing none, and the probe is first shown to see
 * the mapping while it is live.
 */
static void
an_outgrown_mapping_is_released(void)
{
	uint32_t *items;
	uint32_t *grown;
	size_t capacity;
	size_t mapped_count;
	uintptr_t first;
	uintptr_t last;

	capacity = 0;
	mapped_count = RELIC_ARRAY_MAP_BYTES / sizeof(*items);

	items = relic_grow_array(NULL, &capacity, mapped_count, sizeof(*items));
	if (items == NULL) {
		check(false, SCENARIO_RELEASED,
		    "an array of the threshold's bytes should be allocated");
		return;
	}

	check(capacity == mapped_count && relic_array_mapped(items),
	    SCENARIO_RELEASED,
	    "an array of the threshold's bytes should be mapped");

	first = (uintptr_t)items;
	last = (uintptr_t)(items + capacity) - 1;
	check(page_is_mapped(first) && page_is_mapped(last), SCENARIO_RELEASED,
	    "both ends of a live mapping should be seen as mapped");

	grown = relic_grow_array(items, &capacity, capacity + 1,
	    sizeof(*items));
	if (grown == NULL) {
		check(false, SCENARIO_RELEASED, "a mapped array should grow");
		relic_free_array(items);
		return;
	}
	items = grown;

	check(capacity == 2 * mapped_count, SCENARIO_RELEASED,
	    "a mapped array should double");
	check(!page_is_mapped(first) && !page_is_mapped(last),
	    SCENARIO_RELEASED, "the mapping an array outgrew should be "
	    "released whole");

	first = (uintptr_t)items;
	last = (uintptr_t)(items + capacity) - 1;
	relic_free_array(items);

	check(!page_is_mapped(first) && !page_is_mapped(last),
	    SCENARIO_RELEASED, "a freed mapping should be released whole");
}

/*
 * Whether the page holding `address` is mapped.  msync(2) refuses a range
 * holding a page that is not with ENOMEM, and does nothing else to an
 * anonymous mapping that it is asked to schedule.
 */
static bool
page_is_mapped(uintptr_t address)
{
	uintptr_t page;
	size_t page_bytes;
	int page_size;

	page_size = getpagesize();
	page_bytes = (size_t)page_size;
	page = address - address % page_bytes;

	return (msync((void *)page, page_bytes, MS_ASYNC) == 0 ||
	    errno != ENOMEM);
}

/*
 * The record in front of a growable array's first item says what to
 * release, so a record damaged by a stray write must stop the process
 * rather than free or unmap by what it says -- a mapped record claiming a
 * longer mapping would unmap the neighbour after it.  Each byte of the
 * record is inverted in turn, in a heap array and in a mapped one, and the
 * whole record is replaced by another array's.  Freeing the array with each
 * damaged record must end in abort(3).
 */
static void
a_damaged_header_stops_the_process(void)
{
	unsigned char damaged[RECORD_BYTES];
	uint32_t *small;
	uint32_t *large;
	uint32_t *donor;
	size_t small_capacity;
	size_t large_capacity;
	size_t donor_capacity;
	size_t offset;
	size_t survived_small;
	size_t survived_large;

	small_capacity = 0;
	large_capacity = 0;
	donor_capacity = 0;
	small = relic_grow_array(NULL, &small_capacity, 1, sizeof(*small));
	large = relic_grow_array(NULL, &large_capacity,
	    RELIC_ARRAY_MAP_BYTES / sizeof(*large), sizeof(*large));
	donor = relic_grow_array(NULL, &donor_capacity, 1, sizeof(*donor));

	if (small == NULL || large == NULL || donor == NULL) {
		check(false, SCENARIO_DAMAGED,
		    "the arrays to damage should be allocated");
		relic_free_array(small);
		relic_free_array(large);
		relic_free_array(donor);
		return;
	}

	survived_small = 0;
	survived_large = 0;
	for (offset = 0; offset < RECORD_BYTES; offset++) {
		inverted_record(small, offset, damaged);
		if (!freeing_aborts_with(small, damaged))
			survived_small++;

		inverted_record(large, offset, damaged);
		if (!freeing_aborts_with(large, damaged))
			survived_large++;
	}

	checkf(survived_small == 0 && survived_large == 0, SCENARIO_DAMAGED,
	    "freeing with any byte of the record inverted should abort, but "
	    "%zu of a heap array's bytes and %zu of a mapped array's did not",
	    survived_small, survived_large);

	bcopy(record_of(donor), damaged, RECORD_BYTES);
	check(freeing_aborts_with(small, damaged), SCENARIO_DAMAGED,
	    "freeing with another array's record should abort");

	relic_free_array(small);
	relic_free_array(large);
	relic_free_array(donor);
}

/*
 * The record in front of `items`, made addressable to the sanitizer so that
 * the test can read and write it.
 */
static unsigned char *
record_of(uint32_t *items)
{
	unsigned char *record;

	record = (unsigned char *)items - RECORD_BYTES;
	ASAN_UNPOISON_MEMORY_REGION(record, RECORD_BYTES);

	return (record);
}

/*
 * The record of `items` with the byte at `offset` inverted, into `record`.
 */
static void
inverted_record(uint32_t *items, size_t offset, unsigned char *record)
{
	bcopy(record_of(items), record, RECORD_BYTES);
	record[offset] ^= 0xff;
}

/*
 * Whether freeing `items` with `record` in place of its own ends in abort(3).
 * The record is installed, a child process frees the array, and the array's
 * own record is put back once the child has ended, so the caller's array is
 * whole again.  The child's SIGABRT handler reports the abort as an exit
 * status of its own, so that the abort leaves no crash report behind.
 */
static bool
freeing_aborts_with(uint32_t *items, const unsigned char *record)
{
	unsigned char kept[RECORD_BYTES];
	unsigned char *installed;
	pid_t child;
	pid_t waited;
	int status;

	status = 0;
	installed = record_of(items);
	bcopy(installed, kept, RECORD_BYTES);
	bcopy(record, installed, RECORD_BYTES);

	child = fork();
	if (child == 0) {
		signal(SIGABRT, exit_aborted);
		relic_free_array(items);
		_exit(0);
	}

	waited = child == -1 ? -1 : waitpid(child, &status, 0);

	bcopy(kept, installed, RECORD_BYTES);
	ASAN_POISON_MEMORY_REGION(installed, RECORD_BYTES);

	if (waited != child || child == -1) {
		check(false, SCENARIO_DAMAGED,
		    "a child process should be forked and waited for");
		return (false);
	}

	return (WIFEXITED(status) && WEXITSTATUS(status) == ABORTED_STATUS);
}

static void
exit_aborted(int signal_number RELIC_UNUSED)
{
	_exit(ABORTED_STATUS);
}

/*
 * Under AddressSanitizer the header in front of an array's first item is
 * unaddressable, as a heap block's left redzone would be, and so are a
 * mapping's bytes past its last item, so a write just outside either end
 * is reported.  The items themselves stay addressable.  Without the
 * sanitizer there is nothing to observe.
 */
static void
an_array_is_fenced_for_the_sanitizer(void)
{
#ifdef RELIC_ADDRESS_SANITIZER
	uint32_t *small;
	uint32_t *large;
	size_t small_capacity;
	size_t large_capacity;

	small_capacity = 0;
	large_capacity = 0;
	small = relic_grow_array(NULL, &small_capacity, 1, sizeof(*small));
	large = relic_grow_array(NULL, &large_capacity,
	    RELIC_ARRAY_MAP_BYTES / sizeof(*large), sizeof(*large));

	if (small == NULL || large == NULL) {
		check(false, SCENARIO_FENCED,
		    "the arrays to fence should be allocated");
		relic_free_array(small);
		relic_free_array(large);
		return;
	}

	check(__asan_address_is_poisoned((char *)small - 1) &&
	    __asan_address_is_poisoned((char *)large - 1), SCENARIO_FENCED,
	    "the byte before an array's first item should be unaddressable");
	check(!__asan_address_is_poisoned(small) &&
	    !__asan_address_is_poisoned(&small[small_capacity - 1]) &&
	    !__asan_address_is_poisoned(large) &&
	    !__asan_address_is_poisoned(&large[large_capacity - 1]),
	    SCENARIO_FENCED, "an array's items should be addressable");
	check(__asan_address_is_poisoned(&large[large_capacity]),
	    SCENARIO_FENCED,
	    "the byte after a mapped array's last item should be "
	    "unaddressable");

	relic_free_array(small);
	relic_free_array(large);
#endif
}

/*
 * An identifier table reserved for enough entries that its slots and its
 * tags each reach the threshold gets both as mappings, and they arrive as
 * an empty table: a slot is empty only while it holds RELIC_ID_NONE, so a
 * large table allocated without zeroing would hold phantom entries.  Keys
 * inserted into it are then found where they were placed.
 */
static void
a_large_table_starts_mapped_and_empty(void)
{
	enum { KEY_COUNT = 1000 };

	relic_idtab_t table;
	key_set_t keys;
	uint32_t storage[KEY_COUNT];
	size_t reserved;
	size_t occupied;
	size_t lost;
	size_t i;

	keys.keys = storage;
	keys.count = 0;
	keys.seed = SEED;
	relic_idtab_init(&table, true);

	/*
	 * A table keeps a quarter of its slots free, so one of half the
	 * threshold's slots holds three quarters of them, and one entry more
	 * needs the next doubling: a slot, and a one-byte tag, for every
	 * byte of the threshold.
	 */
	reserved = RELIC_ARRAY_MAP_BYTES / 2 / 4 * 3 + 1;
	if (relic_idtab_reserve(&table, reserved, key_rehash, &keys) != 0) {
		check(false, SCENARIO_MAPPED,
		    "a large reservation should succeed");
		return;
	}

	check(table.capacity >= RELIC_ARRAY_MAP_BYTES, SCENARIO_MAPPED,
	    "the reservation should reach a tag per byte of the threshold");
	check(relic_array_mapped(table.slots) && relic_array_mapped(table.tags),
	    SCENARIO_MAPPED, "a large table's slots and tags should be mapped");

	occupied = 0;
	for (i = 0; i < table.capacity; i++) {
		if (table.slots[i] != RELIC_ID_NONE || table.tags[i] != 0)
			occupied++;
	}
	checkf(occupied == 0, SCENARIO_MAPPED,
	    "a new table should be empty, not hold %zu entries", occupied);

	for (i = 0; i < KEY_COUNT; i++) {
		if (!key_set_insert(&table, &keys, (uint32_t)(i * 7919u)))
			break;
	}

	lost = 0;
	for (i = 0; i < keys.count; i++) {
		uint32_t id;

		if (!key_set_find(&table, &keys, storage[i], &id) ||
		    id != i + 1)
			lost++;
	}
	checkf(keys.count == KEY_COUNT && lost == 0, SCENARIO_MAPPED,
	    "%zu of %d keys should be found in a large table",
	    keys.count - lost, KEY_COUNT);

	relic_idtab_reset(&table);
}

/*
 * Equal input under an equal seed must hash to the same value every time,
 * because a key is found only by recomputing the hash it was placed with.  A
 * hash that mixed in anything besides the seed and the key -- the clock, an
 * address, state left by an earlier call -- would fail here.
 */
static void
equal_input_hashes_equally(void)
{
	static const char text[] = "path";

	check(relic_hash_bytes(SEED, text, sizeof(text) - 1) ==
	    relic_hash_bytes(SEED, "path", 4), SCENARIO_DETERMINISM,
	    "equal bytes should hash equally");
	check(relic_hash_word(SEED, 42) == relic_hash_word(SEED, 42),
	    SCENARIO_DETERMINISM, "equal words should hash equally");
	check(relic_hash_int64(SEED, -1) == relic_hash_int64(SEED, -1),
	    SCENARIO_DETERMINISM, "equal integers should hash equally");
}

/*
 * A hash that silently ignored its seed would pass every other test here,
 * and leave every table open to keys crafted against the constants.  Each
 * hash function is therefore asked for a few keys under two seeds, and must
 * give a different answer for at least one of them.  One key would do for a
 * sound hash; a few keep a coincidence from failing the test.
 */
static void
a_hash_depends_on_its_seed(void)
{
	static const char *const texts[] = { "", "a", "path" };
	static const uint32_t sequence[] = { 0u, 1u, 0xffffffffu };
	static const int64_t integers[] = { 0, -1, INT64_MAX };
	size_t length;
	size_t i;
	bool bytes_moved;
	bool words_moved;
	bool word_moved;
	bool integer_moved;

	bytes_moved = false;
	for (i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
		length = strlen(texts[i]);
		bytes_moved |= relic_hash_bytes(SEED, texts[i], length) !=
		    relic_hash_bytes(OTHER_SEED, texts[i], length);
	}

	/* Every prefix of the sequence, the empty one included. */
	words_moved = false;
	for (i = 0; i <= sizeof(sequence) / sizeof(sequence[0]); i++) {
		words_moved |= relic_hash_words(SEED, sequence, i) !=
		    relic_hash_words(OTHER_SEED, sequence, i);
	}

	word_moved = false;
	for (i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
		word_moved |= relic_hash_word(SEED, sequence[i]) !=
		    relic_hash_word(OTHER_SEED, sequence[i]);
	}

	integer_moved = false;
	for (i = 0; i < sizeof(integers) / sizeof(integers[0]); i++) {
		integer_moved |= relic_hash_int64(SEED, integers[i]) !=
		    relic_hash_int64(OTHER_SEED, integers[i]);
	}

	check(bytes_moved, SCENARIO_KEYED,
	    "the byte hash should depend on the seed");
	check(words_moved, SCENARIO_KEYED,
	    "the word sequence hash should depend on the seed");
	check(word_moved, SCENARIO_KEYED,
	    "the word hash should depend on the seed");
	check(integer_moved, SCENARIO_KEYED,
	    "the integer hash should depend on the seed");

	/*
	 * The halves of the seed key different steps, the fold and the
	 * finalizer, so a change to either half alone must move the hash too.
	 */
	check(relic_hash_word(SEED, 1u) !=
	    relic_hash_word(SEED ^ UINT64_C(0xffffffff), 1u), SCENARIO_KEYED,
	    "the low half of the seed should key the hash");
	check(relic_hash_word(SEED, 1u) !=
	    relic_hash_word(SEED ^ UINT64_C(0xffffffff00000000), 1u),
	    SCENARIO_KEYED, "the high half of the seed should key the hash");
}

/*
 * Flooding is a property of the seed.
 *
 * An author who knows the seed can choose keys that all select one slot,
 * and then every insertion walks the whole cluster the earlier ones built,
 * which makes a load quadratic.  The keys below are chosen exactly that way
 * against SEED: each hashes, under SEED, to a value whose low bits are zero,
 * so each selects slot zero at every capacity the table passes through.
 * Inserted under SEED they form one cluster, and finding the last of them
 * walks all of them.
 *
 * Inserted under another seed the same keys spread like any others.  That
 * is the whole defence, and this is the test of it: were the hash keyed by
 * constants rather than by the seed, the keys would collide under every seed
 * and the second table would be as slow as the first.
 */
static void
colliding_keys_collide_only_under_their_seed(void)
{
	/*
	 * A table grows before it passes three quarters full, so KEY_COUNT
	 * keys end in a table of SLOT_COUNT slots, and a hash whose low bits
	 * select slot zero there selects it at every smaller capacity too.
	 */
	enum { KEY_COUNT = 256, SLOT_COUNT = 512 };

	uint32_t crafted[KEY_COUNT];
	uint32_t storage[KEY_COUNT];
	relic_idtab_t table;
	key_set_t keys;
	uint32_t candidate;
	uint32_t slot;
	size_t found;
	size_t flooded;
	size_t spread;
	size_t i;

	found = 0;
	for (candidate = 1; found < KEY_COUNT && candidate != 0; candidate++) {
		slot = relic_hash_word(SEED, candidate) & (SLOT_COUNT - 1);
		if (slot == 0) {
			crafted[found] = candidate;
			found++;
		}
	}

	if (found != KEY_COUNT) {
		check(false, SCENARIO_FLOODING,
		    "the search should find enough colliding keys");
		return;
	}

	keys.keys = storage;
	keys.count = 0;
	keys.seed = SEED;
	relic_idtab_init(&table, false);
	for (i = 0; i < KEY_COUNT; i++)
		(void)key_set_insert(&table, &keys, crafted[i]);
	check(keys.count == KEY_COUNT, SCENARIO_FLOODING,
	    "every crafted key should be inserted under its seed");
	check(table.capacity == SLOT_COUNT, SCENARIO_FLOODING,
	    "the table should have grown to the capacity the keys target");
	flooded = longest_walk(&table, &keys);
	relic_idtab_reset(&table);

	keys.count = 0;
	keys.seed = OTHER_SEED;
	relic_idtab_init(&table, false);
	for (i = 0; i < KEY_COUNT; i++)
		(void)key_set_insert(&table, &keys, crafted[i]);
	check(keys.count == KEY_COUNT, SCENARIO_FLOODING,
	    "every crafted key should be inserted under another seed");
	spread = longest_walk(&table, &keys);
	relic_idtab_reset(&table);

	checkf(flooded == KEY_COUNT, SCENARIO_FLOODING,
	    "under the seed they were crafted for, the keys should form one "
	    "cluster of %d, not %zu", KEY_COUNT, flooded);
	checkf(spread < KEY_COUNT / 16, SCENARIO_FLOODING,
	    "under another seed, no key should be %zu entries deep in a "
	    "cluster", spread);
}

/*
 * An integer hashes as the sequence of its halves, so two integers whose
 * halves both differ in bit 31 are the pair a fold over a state as wide as a
 * word would collide under every seed: the first flip passes the multiply
 * unchanged and the second cancels it.  This checks that they differ under
 * a few seeds.
 */
static void
integers_differing_in_bit_31_of_each_half_differ(void)
{
	static const uint64_t seeds[] = { SEED, OTHER_SEED, 1u };
	static const int64_t values[] = { 0, 5, -1, INT64_MAX };
	const uint64_t top_bits = UINT64_C(0x8000000080000000);
	int64_t partner;
	size_t collisions;
	size_t i;
	size_t j;

	collisions = 0;
	for (i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
		for (j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
			partner = (int64_t)((uint64_t)values[j] ^ top_bits);
			if (relic_hash_int64(seeds[i], values[j]) ==
			    relic_hash_int64(seeds[i], partner))
				collisions++;
		}
	}

	checkf(collisions == 0, SCENARIO_KEYED,
	    "integers differing in bit 31 of each half should not collide, "
	    "but %zu pairs did", collisions);
}

/*
 * A domain identifier carries its type in its top two bits, so the keys an
 * input can produce include every way of giving a key's columns other types
 * over the same positions.  A fold whose differences only ever move upward
 * would keep such keys' differences in the top two bits of its state, and
 * sort all 3^n of them into at most four hashes whatever the seed: a flood
 * built without knowing it.  Hashed properly they are as distinct as any
 * other keys, so no hash of these 6561 should be shared by more than two.
 */
static void
type_variants_of_a_key_spread(void)
{
	static const uint64_t seeds[] = { SEED, OTHER_SEED, 1u };
	static const uint32_t types[] = {
		RELIC_DOM_SYMBOL, RELIC_DOM_STRING, RELIC_DOM_INTEGER
	};
	static uint32_t hashes[VARIANT_COUNT];
	uint32_t key[VARIANT_COLUMNS];
	size_t largest;
	size_t run;
	size_t variant;
	size_t choice;
	size_t column;
	size_t i;

	largest = 0;
	for (i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
		for (variant = 0; variant < VARIANT_COUNT; variant++) {
			choice = variant;
			for (column = 0; column < VARIANT_COLUMNS; column++) {
				key[column] = types[choice % 3] |
				    (uint32_t)(column * 1000 + 7);
				choice /= 3;
			}
			hashes[variant] = relic_hash_words(seeds[i], key,
			    VARIANT_COLUMNS);
		}

		qsort(hashes, VARIANT_COUNT, sizeof(hashes[0]), order_hashes);
		run = 1;
		for (variant = 1; variant < VARIANT_COUNT; variant++) {
			run = hashes[variant] == hashes[variant - 1] ?
			    run + 1 : 1;
			if (run > largest)
				largest = run;
		}
	}

	checkf(largest <= 2, SCENARIO_FLOODING,
	    "keys differing only in their columns' types should spread, but "
	    "%zu of them shared one hash", largest);
}

static int
order_hashes(const void *left, const void *right)
{
	uint32_t a;
	uint32_t b;

	a = *(const uint32_t *)left;
	b = *(const uint32_t *)right;

	return ((a > b) - (a < b));
}

/*
 * A difference in a word's top bit passes a multiplication by an odd number
 * unchanged, whatever the other bits, so a fold that did nothing else would
 * hand it to the next word unchanged, and a mixing step that only shifted
 * and xored would hand it on as a pattern just as fixed.  Either way some
 * later word differing by that pattern would cancel it under every seed.
 * This pairs a key with one whose first word differs in bit 31 and whose
 * second differs in bit 31 alone or in bit 31 and one other bit, and expects
 * no pair to collide under any of sixteen seeds.
 */
static void
top_bit_differences_are_not_cancelled(void)
{
	static const uint32_t first[] = { 5u, 0x12345u };
	uint32_t second_differences[32];
	uint32_t mixed[2];
	uint32_t key[2];
	size_t collisions;
	uint64_t seed;
	unsigned int bit;
	size_t d;
	size_t i;

	second_differences[0] = TOP_BIT;
	for (bit = 0; bit < 31; bit++)
		second_differences[bit + 1] = TOP_BIT | UINT32_C(1) << bit;

	collisions = 0;
	for (seed = 1; seed <= 16; seed++) {
		for (d = 0; d < 32; d++) {
			for (i = 0; i < sizeof(first) / sizeof(first[0]); i++) {
				key[0] = first[i];
				key[1] = 0x777u;
				mixed[0] = key[0] ^ TOP_BIT;
				mixed[1] = key[1] ^ second_differences[d];

				if (relic_hash_words(seed * SEED, key, 2) ==
				    relic_hash_words(seed * SEED, mixed, 2))
					collisions++;
			}
		}
	}

	checkf(collisions == 0, SCENARIO_FLOODING,
	    "a difference in the top bit of one word should not be cancelled "
	    "by a later word, but %zu pairs collided", collisions);
}

/*
 * A multiplier with few bits set satisfies a short polynomial relation
 * modulo 2^64 -- FNV's prime, 2^40 + 435, less 435 and squared is a multiple
 * of 2^64 -- and such a relation lets a difference in one word be partly
 * offset by the next and cancelled by the third under a fixed share of
 * seeds.  The pair below is one: through FNV's prime it collided under about
 * one seed in a thousand.  A multiplier without such a relation leaves the
 * pair colliding only as often as any two keys do, which no seed here
 * should.
 */
static void
three_word_differences_are_not_cancelled(void)
{
	static const uint32_t key[] = { 0u, 0u, 0u };
	static const uint32_t crafted[] = {
		UINT32_C(0x000a0000), UINT32_C(0x11040000), UINT32_C(0x03660000)
	};
	size_t collisions;
	uint64_t seed;

	collisions = 0;
	for (seed = 1; seed <= CHANNEL_SEEDS; seed++) {
		if (relic_hash_words(seed * SEED, key, 3) ==
		    relic_hash_words(seed * SEED, crafted, 3))
			collisions++;
	}

	checkf(collisions == 0, SCENARIO_FLOODING,
	    "a crafted three-word difference should not be cancelled, but it "
	    "was under %zu of %d seeds", collisions, CHANNEL_SEEDS);
}

/*
 * A context draws its own seed.
 *
 * Two contexts holding one seed would mean the seed was read once and
 * cached, was never read at all, or came from something that is not random;
 * two independent draws of 64 bits are equal with probability 2^-64.  The
 * three tables a context creates with itself must hold its seed.  Every
 * other owner is checked wherever it hashes, by the hash functions'
 * assertion that no seed is zero.
 */
static void
each_context_draws_a_seed_of_its_own(void)
{
	relic_t *first;
	relic_t *second;

	first = relic_create();
	second = relic_create();

	if (first == NULL || second == NULL) {
		check(false, SCENARIO_CONTEXTS,
		    "two contexts should be created");
		relic_destroy(first);
		relic_destroy(second);
		return;
	}

	check(first->hash_seed != second->hash_seed, SCENARIO_CONTEXTS,
	    "two contexts should draw two seeds");
	check(first->symbols.hash_seed == first->hash_seed &&
	    first->values.hash_seed == first->hash_seed &&
	    first->program.predicates.hash_seed == first->hash_seed,
	    SCENARIO_CONTEXTS,
	    "the context's tables should hash with the context's seed");

	relic_destroy(first);
	relic_destroy(second);
}

/*
 * Only the low bits of a hash select a slot, so keys that differ slightly
 * must still spread across the table.  Consecutive integers are the case that
 * matters: a Datalog program full of small integers would otherwise probe in
 * long runs.
 *
 * The check is deliberately loose.  It is not measuring hash quality, it is
 * catching the mistake of selecting a slot from unmixed bits, which would put
 * far more than a handful of these keys in one bucket of sixteen.
 */
static void
similar_keys_reach_different_slots(void)
{
	enum { KEY_COUNT = 256, BUCKET_COUNT = 16 };

	unsigned int occupancy[BUCKET_COUNT];
	unsigned int busiest;
	size_t i;

	bzero(occupancy, sizeof(occupancy));

	for (i = 0; i < KEY_COUNT; i++)
		occupancy[relic_hash_int64(SEED, (int64_t)i) % BUCKET_COUNT]++;

	busiest = 0;
	for (i = 0; i < BUCKET_COUNT; i++) {
		if (occupancy[i] > busiest)
			busiest = occupancy[i];
	}

	check(busiest < KEY_COUNT / 4, SCENARIO_SPREAD,
	    "consecutive integers should not pile into one bucket");
}

/*
 * The array growth predictor agrees with the growth.
 *
 * The relation store's memory limit is enforced against what an array *would*
 * hold, computed before the array is asked to hold it, so that a limit
 * refuses an allocation instead of reporting one that has already happened.
 * That is sound only while the prediction and the policy are the same thing.
 * They are written as one function calling the other, and this is what says
 * so: were a later edit to give relic_grow_array() a growth factor of its
 * own, every relation test would still pass and the memory limit would
 * quietly become an approximation.
 */
static void
a_growth_is_exactly_what_was_predicted(void)
{
	enum { STEP_COUNT = 40 };

	uint32_t *items;
	size_t capacity;
	size_t i;

	items = NULL;
	capacity = 0;

	for (i = 0; i < STEP_COUNT; i++) {
		size_t predicted;
		size_t wanted;
		void *grown;

		wanted = i * i + 1;
		predicted = relic_grow_array_capacity(capacity, wanted);

		grown = relic_grow_array(items, &capacity, wanted,
		    sizeof(*items));
		if (grown == NULL) {
			checkf(false, SCENARIO_PREDICTED,
			    "an array of %zu items should grow", wanted);
			break;
		}
		items = grown;

		checkf(predicted == capacity, SCENARIO_PREDICTED,
		    "growing to hold %zu items should reach the predicted "
		    "capacity %zu, not %zu", wanted, predicted, capacity);
	}

	relic_free_array(items);

	/* A request that needs no growth is predicted as no growth. */
	capacity = 64;
	check(relic_grow_array_capacity(capacity, 1) == capacity,
	    SCENARIO_PREDICTED,
	    "an array with room to spare should be predicted unchanged");

	/*
	 * A capacity that cannot be doubled is predicted as a refusal, which
	 * is the one answer the caller must tell apart from a size.  It takes
	 * both halves: an array past half the address space, asked for more
	 * than it already has.
	 */
	check(relic_grow_array_capacity(SIZE_MAX / 2 + 1, SIZE_MAX) == 0,
	    SCENARIO_PREDICTED,
	    "an array that cannot be doubled should be predicted to refuse");
}

/*
 * Reserve room for `added` keys in a table already holding `held`, and
 * check the reservation reached the size predicted for it and that the keys
 * then fit without the table growing again.  Returns whether every check
 * held, so the caller can name the pair that failed.
 */
static bool
reserve_then_fill(size_t held, size_t added, bool tagged)
{
	enum { KEY_MAX = 4096 };

	relic_idtab_t table;
	key_set_t keys;
	counted_keys_t counted;
	uint32_t storage[KEY_MAX];
	uint32_t *slots;
	uint32_t *slots_before;
	size_t predicted;
	size_t i;
	bool held_up;

	keys.keys = storage;
	keys.count = 0;
	keys.seed = SEED;
	relic_idtab_init(&table, tagged);
	held_up = true;

	for (i = 0; i < held && held_up; i++)
		held_up = key_set_insert(&table, &keys, (uint32_t)(i * 7919u));
	slots_before = table.slots;

	/*
	 * A growth moves every entry once, so a reservation that grew the
	 * table rehashed exactly the entries it held, and one that did not
	 * rehashed none.
	 */
	predicted = relic_idtab_bytes_after_reserve(&table, added);
	counted.keys = &keys;
	counted.rehashes = 0;
	if (held_up)
		held_up = relic_idtab_reserve(&table, added, counting_rehash,
		    &counted) == 0 && relic_idtab_bytes(&table) == predicted &&
		    counted.rehashes ==
		    (slots_before == table.slots ? 0 : held);

	slots = table.slots;
	for (i = 0; i < added && held_up; i++)
		held_up = key_set_insert(&table, &keys,
		    (uint32_t)((held + i) * 7919u));

	held_up = held_up && table.slots == slots &&
	    relic_idtab_bytes(&table) == predicted;

	relic_idtab_reset(&table);

	return (held_up);
}

/*
 * The slots a table holding `held` keys has once it has reserved room for
 * `added` more, or zero when the fixture could not be built.
 */
static size_t
capacity_reserved(size_t held, size_t added)
{
	relic_idtab_t table;
	key_set_t keys;
	uint32_t storage[64];
	size_t capacity;
	size_t i;

	keys.keys = storage;
	keys.count = 0;
	keys.seed = SEED;
	relic_idtab_init(&table, false);
	capacity = 0;

	for (i = 0; i < held; i++) {
		if (!key_set_insert(&table, &keys, (uint32_t)i))
			break;
	}

	if (i == held &&
	    relic_idtab_reserve(&table, added, key_rehash, &keys) == 0)
		capacity = table.capacity;

	relic_idtab_reset(&table);

	return (capacity);
}

/*
 * A reservation for many entries grows the table once, to the size its
 * prediction names, and every entry reserved for then fits without another
 * growth.  The size is the smallest power of two, at least double the
 * table's, whose three quarters hold them: twelve entries fit sixteen slots
 * and thirteen need thirty-two.  A reservation of none changes nothing, and
 * one whose size could not be expressed is refused and leaves the table as
 * it was.
 */
static void
a_reservation_for_many_holds_them_all(void)
{
	static const size_t helds[] = { 0, 1, 7, 12, 13, 100 };
	static const size_t addeds[] = { 0, 1, 2, 5, 11, 12, 13, 100, 3000 };
	static const struct {
		size_t	held;
		size_t	added;
		size_t	capacity;
	} sized[] = {
		{ 0, 12, 16 }, { 0, 13, 32 }, { 12, 12, 32 }, { 12, 13, 64 },
		{ 1, 0, 16 }, { 12, 0, 16 },
	};

	relic_idtab_t table;
	key_set_t keys;
	uint32_t storage[1];
	uint32_t *slots;
	size_t h;
	size_t a;

	for (h = 0; h < sizeof(helds) / sizeof(helds[0]); h++) {
		for (a = 0; a < sizeof(addeds) / sizeof(addeds[0]); a++) {
			checkf(reserve_then_fill(helds[h], addeds[a], false),
			    SCENARIO_RESERVED,
			    "a table of %zu reserving for %zu more should "
			    "reach its prediction and then hold them",
			    helds[h], addeds[a]);
			checkf(reserve_then_fill(helds[h], addeds[a], true),
			    SCENARIO_RESERVED,
			    "a tagged table of %zu reserving for %zu more "
			    "should reach its prediction and then hold them",
			    helds[h], addeds[a]);
		}
	}

	for (h = 0; h < sizeof(sized) / sizeof(sized[0]); h++)
		checkf(capacity_reserved(sized[h].held, sized[h].added) ==
		    sized[h].capacity, SCENARIO_RESERVED,
		    "a table of %zu reserving for %zu more should have %zu "
		    "slots, not %zu", sized[h].held, sized[h].added,
		    sized[h].capacity,
		    capacity_reserved(sized[h].held, sized[h].added));

	keys.keys = storage;
	keys.count = 0;
	keys.seed = SEED;
	relic_idtab_init(&table, false);
	check(key_set_insert(&table, &keys, 1u), SCENARIO_RESERVED,
	    "inserting one key should succeed");
	slots = table.slots;

	check(relic_idtab_bytes_after_reserve(&table, SIZE_MAX) ==
	    relic_idtab_bytes(&table), SCENARIO_RESERVED,
	    "an inexpressible reservation should predict no growth");
	check(relic_idtab_reserve(&table, SIZE_MAX, key_rehash, &keys) != 0,
	    SCENARIO_RESERVED, "an inexpressible reservation should fail");
	check(table.slots == slots && table.count == 1, SCENARIO_RESERVED,
	    "a failed reservation should leave the table as it was");

	relic_idtab_reset(&table);
}

/*
 * The identifier table's byte predictor agrees with the table, for the same
 * reason and with the same consequence if it stops.
 */
static void
a_reservation_is_exactly_what_was_predicted(void)
{
	reserved_as_predicted(false);
	reserved_as_predicted(true);
}

/*
 * Reserve room for one key at a time, in a table created tagged or not,
 * and check that each reservation reaches the bytes predicted for it: four
 * per slot, and one more for a tag.
 */
static void
reserved_as_predicted(bool tagged)
{
	enum { KEY_COUNT = 3000 };

	relic_idtab_t table;
	key_set_t keys;
	uint32_t storage[KEY_COUNT];
	size_t slot_bytes;
	size_t i;

	keys.keys = storage;
	keys.count = 0;
	keys.seed = SEED;
	relic_idtab_init(&table, tagged);
	slot_bytes = sizeof(uint32_t) + (tagged ? 1 : 0);

	check(relic_idtab_bytes(&table) == 0, SCENARIO_PREDICTED,
	    "an empty table should hold no bytes");

	for (i = 0; i < KEY_COUNT; i++) {
		size_t predicted;

		predicted = relic_idtab_bytes_after_reserve(&table, 1);

		if (relic_idtab_reserve(&table, 1, key_rehash, &keys) != 0) {
			checkf(false, SCENARIO_PREDICTED,
			    "reserving room for key %zu should succeed", i);
			break;
		}

		checkf(predicted == relic_idtab_bytes(&table) &&
		    predicted == table.capacity * slot_bytes,
		    SCENARIO_PREDICTED,
		    "reserving room for key %zu should reach the predicted "
		    "%zu bytes, %zu to a slot, not %zu", i, predicted,
		    slot_bytes, relic_idtab_bytes(&table));

		storage[keys.count] = (uint32_t)(i * 7919u + 13u);
		keys.count++;
		relic_idtab_place(&table, relic_hash_word(SEED, storage[i]),
		    (uint32_t)keys.count);
	}

	relic_idtab_reset(&table);
	check(table.tagged == tagged, SCENARIO_PREDICTED,
	    "a reset table should keep its kind");
}

/*
 * The hash each identifier was placed under, kept by the owner as every
 * owner keeps its keys: identifier n's is hashes[n - 1].
 */
static uint32_t
stored_hash(uint32_t id, void *ctx)
{
	const uint32_t *hashes;

	hashes = ctx;

	return (hashes[id - 1]);
}

/*
 * How many candidates a probe for `hash` is handed.
 */
static size_t
candidates_for(const relic_idtab_t *table, uint32_t hash)
{
	relic_idprobe_t probe;
	size_t candidates;
	uint32_t id;

	candidates = 0;
	relic_idprobe_open(&probe, table, hash);
	while (relic_idprobe_next(&probe, &id))
		candidates++;

	return (candidates);
}

/*
 * A tagged table hands a probe only the identifiers whose tag, the top eight
 * bits of the hash they were placed under, is the probe's.  Every hash here
 * has the same low bits, so every identifier is in one cluster starting at
 * the slot they select, and each has a tag of its own.  An untagged probe is
 * handed the whole cluster, whichever hash it is for; a tagged one is handed
 * the one identifier placed under its hash, and none for a hash whose tag no
 * identifier carries.
 */
static void
a_tagged_probe_passes_over_other_tags(void)
{
	enum { CLUSTER = 200, LOW_BITS = 5, CHOSEN = 100 };

	static const bool kinds[] = { false, true };
	uint32_t hashes[CLUSTER];
	relic_idtab_t table;
	relic_idprobe_t probe;
	const char *kind;
	uint32_t absent;
	uint32_t found;
	size_t expected;
	size_t k;
	size_t i;
	bool tagged;
	bool handed;

	for (i = 0; i < CLUSTER; i++)
		hashes[i] = ((uint32_t)i << 24) | LOW_BITS;
	absent = (UINT32_C(0xff) << 24) | LOW_BITS;

	for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
		tagged = kinds[k];
		kind = tagged ? "a tagged" : "an untagged";

		relic_idtab_init(&table, tagged);
		if (relic_idtab_reserve(&table, CLUSTER, stored_hash,
		    hashes) != 0) {
			check(false, SCENARIO_TAGS, "the table is reserved");
			return;
		}
		for (i = 0; i < CLUSTER; i++)
			relic_idtab_place(&table, hashes[i], (uint32_t)(i + 1));

		expected = tagged ? 1 : CLUSTER;
		checkf(candidates_for(&table, hashes[CHOSEN]) == expected,
		    SCENARIO_TAGS, "%s probe should be handed %zu candidates, "
		    "not %zu", kind, expected,
		    candidates_for(&table, hashes[CHOSEN]));

		relic_idprobe_open(&probe, &table, hashes[CHOSEN]);
		handed = relic_idprobe_next(&probe, &found);
		checkf(handed && (!tagged || found == CHOSEN + 1),
		    SCENARIO_TAGS, "%s probe should be handed %s", kind,
		    tagged ? "the identifier placed under its hash first" :
		    "a candidate");

		expected = tagged ? 0 : CLUSTER;
		checkf(candidates_for(&table, absent) == expected,
		    SCENARIO_TAGS, "%s probe for an absent tag should be "
		    "handed %zu candidates, not %zu", kind, expected,
		    candidates_for(&table, absent));

		relic_idtab_reset(&table);
	}
}

/*
 * Under a stated seed, the word hash is pinned to fixed values.
 *
 * Every other property of a hash is statistical, and a statistical test
 * passes just as well on a platform that folds words by their bytes and so
 * probes in a different order on a big-endian machine.  Only a stated value
 * makes such a platform fail here rather than somewhere far away.  The
 * values are those of the keyed fold the hash is defined as, computed
 * independently of this implementation.
 */
static void
a_word_sequence_hashes_to_a_fixed_value(void)
{
	static const uint32_t words[] = { 1u, 2u, 3u };

	check(relic_hash_words(SEED, NULL, 0) == 0x89022ceau,
	    SCENARIO_DETERMINISM,
	    "the empty sequence should hash to 0x89022cea");
	check(relic_hash_words(SEED, words, 1) == 0x0542fc3au,
	    SCENARIO_DETERMINISM,
	    "the sequence 1 should hash to 0x0542fc3a");
	check(relic_hash_words(SEED, words, 3) == 0x8f6a8af5u,
	    SCENARIO_DETERMINISM,
	    "the sequence 1, 2, 3 should hash to 0x8f6a8af5");

	/*
	 * Order matters: a tuple is a sequence, and a hash that folded its
	 * columns commutatively would put (a, b) and (b, a) in one slot.
	 */
	check(relic_hash_words(SEED, words, 2) !=
	    relic_hash_words(SEED, words + 1, 2),
	    SCENARIO_DETERMINISM,
	    "reordering a sequence should change its hash");
}

int
main(void)
{
	an_array_grows_without_losing_its_contents();
	an_array_refuses_an_impossible_size();
	a_large_array_moves_to_a_mapping();
	a_large_table_starts_mapped_and_empty();
	an_outgrown_mapping_is_released();
	a_damaged_header_stops_the_process();
	an_array_is_fenced_for_the_sanitizer();
	equal_input_hashes_equally();
	every_inserted_key_is_found_after_growth();
	a_growth_is_exactly_what_was_predicted();
	a_hash_depends_on_its_seed();
	a_reservation_for_many_holds_them_all();
	a_reservation_is_exactly_what_was_predicted();
	a_tagged_probe_passes_over_other_tags();
	a_word_sequence_hashes_to_a_fixed_value();
	colliding_keys_collide_only_under_their_seed();
	each_context_draws_a_seed_of_its_own();
	integers_differing_in_bit_31_of_each_half_differ();
	similar_keys_reach_different_slots();
	three_word_differences_are_not_cancelled();
	top_bit_differences_are_not_cancelled();
	type_variants_of_a_key_spread();

	return (check_summary("util"));
}
