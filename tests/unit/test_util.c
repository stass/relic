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
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_CONTEXTS	"each context draws a seed of its own"
#define	SCENARIO_DETERMINISM	"hashing is reproducible within a seed"
#define	SCENARIO_FLOODING	"keys crafted to collide need the seed"
#define	SCENARIO_GROWTH		"an array grows without losing its contents"
#define	SCENARIO_KEYED		"a hash depends on its seed"
#define	SCENARIO_LOOKUP		"a key survives the table growing under it"
#define	SCENARIO_OVERFLOW	"an impossible array size is refused"
#define	SCENARIO_PREDICTED	"a growth is exactly what was predicted"
#define	SCENARIO_SPREAD		"hashing separates similar keys"

/*
 * Two seeds chosen by the tests.  A context draws its seed at random, and a
 * test that hashes directly states the seed it hashes with, so that what it
 * checks does not depend on the draw.
 */
#define	SEED		UINT64_C(0x0123456789abcdef)
#define	OTHER_SEED	UINT64_C(0xfedcba9876543210)

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
static bool	 key_set_find(const relic_idtab_t *table,
		    const key_set_t *keys, uint32_t key, uint32_t *idp);
static bool	 key_set_insert(relic_idtab_t *table, key_set_t *keys,
		    uint32_t key);
static size_t	 longest_walk(const relic_idtab_t *table,
		    const key_set_t *keys);
static size_t	 walk_to(const relic_idtab_t *table, const key_set_t *keys,
		    uint32_t key);
static void	 an_array_grows_without_losing_its_contents(void);
static void	 an_array_refuses_an_impossible_size(void);
static void	 equal_input_hashes_equally(void);
static void	 every_inserted_key_is_found_after_growth(void);
static void	 a_growth_is_exactly_what_was_predicted(void);
static void	 a_hash_depends_on_its_seed(void);
static void	 a_reservation_is_exactly_what_was_predicted(void);
static void	 a_word_sequence_hashes_to_a_fixed_value(void);
static void	 colliding_keys_collide_only_under_their_seed(void);
static void	 each_context_draws_a_seed_of_its_own(void);
static void	 integers_differing_in_bit_31_of_each_half_differ(void);
static void	 similar_keys_reach_different_slots(void);

static uint32_t
key_rehash(uint32_t id, void *ctx)
{
	const key_set_t *keys;

	keys = ctx;

	return (relic_hash_word(keys->seed, keys->keys[id - 1]));
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
	if (relic_idtab_reserve(table, key_rehash, keys) != 0)
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
	relic_idtab_init(&table);

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
	    "insertion %zu of %d should have succeeded", first_uninserted,
	    KEY_COUNT);
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
	    "key %zu should still name the identifier it was inserted with",
	    first_lost);

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
	    "key %zu was never inserted and should not be found",
	    first_spurious);

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
		free(items);
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

	free(items);
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
	relic_idtab_init(&table);
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
	relic_idtab_init(&table);
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
 * Flipping bit 31 of a word is the same as adding 2^31, and multiplying by
 * an odd number keeps that difference in bit 31 alone, so a fold that
 * multiplies after each word carries a flipped top bit to the next word
 * whatever the seed, where a second flip cancels it.  Two integers whose
 * halves both differ in bit 31 would then collide under every seed, which
 * is a collision crafted without the seed.  The integer hash finalizes its
 * high half before the low half joins it, which turns the difference into
 * one that depends on the seed; this checks that it does, under a few seeds.
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

	free(items);

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
 * The identifier table's byte predictor agrees with the table, for the same
 * reason and with the same consequence if it stops.
 */
static void
a_reservation_is_exactly_what_was_predicted(void)
{
	enum { KEY_COUNT = 3000 };

	relic_idtab_t table;
	key_set_t keys;
	uint32_t storage[KEY_COUNT];
	size_t i;

	keys.keys = storage;
	keys.count = 0;
	keys.seed = SEED;
	relic_idtab_init(&table);

	check(relic_idtab_bytes(&table) == 0, SCENARIO_PREDICTED,
	    "an empty table should hold no bytes");

	for (i = 0; i < KEY_COUNT; i++) {
		size_t predicted;

		predicted = relic_idtab_bytes_after_reserve(&table);

		if (relic_idtab_reserve(&table, key_rehash, &keys) != 0) {
			checkf(false, SCENARIO_PREDICTED,
			    "reserving room for key %zu should succeed", i);
			break;
		}

		checkf(predicted == relic_idtab_bytes(&table),
		    SCENARIO_PREDICTED,
		    "reserving room for key %zu should reach the predicted "
		    "%zu bytes, not %zu", i, predicted,
		    relic_idtab_bytes(&table));

		storage[keys.count] = (uint32_t)(i * 7919u + 13u);
		keys.count++;
		relic_idtab_place(&table, relic_hash_word(SEED, storage[i]),
		    (uint32_t)keys.count);
	}

	relic_idtab_reset(&table);
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

	check(relic_hash_words(SEED, NULL, 0) == 0x1d6ef8aeu,
	    SCENARIO_DETERMINISM,
	    "the empty sequence should hash to 0x1d6ef8ae");
	check(relic_hash_words(SEED, words, 1) == 0x702f1c2bu,
	    SCENARIO_DETERMINISM,
	    "the sequence 1 should hash to 0x702f1c2b");
	check(relic_hash_words(SEED, words, 3) == 0x58faea37u,
	    SCENARIO_DETERMINISM,
	    "the sequence 1, 2, 3 should hash to 0x58faea37");

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
	equal_input_hashes_equally();
	every_inserted_key_is_found_after_growth();
	a_growth_is_exactly_what_was_predicted();
	a_hash_depends_on_its_seed();
	a_reservation_is_exactly_what_was_predicted();
	a_word_sequence_hashes_to_a_fixed_value();
	colliding_keys_collide_only_under_their_seed();
	each_context_draws_a_seed_of_its_own();
	integers_differing_in_bit_31_of_each_half_differ();
	similar_keys_reach_different_slots();

	return (check_summary("util"));
}
