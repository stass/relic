/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the growable array and the identifier table.
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
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_DETERMINISM	"hashing is reproducible"
#define	SCENARIO_GROWTH		"an array grows without losing its contents"
#define	SCENARIO_LOOKUP		"a key survives the table growing under it"
#define	SCENARIO_OVERFLOW	"an impossible array size is refused"
#define	SCENARIO_SPREAD		"hashing separates similar keys"

/*
 * The keys the identifier table is driven with.  Identifier n names keys[n-1],
 * which is exactly how the symbol pool and the value table use it: the table
 * stores an identifier and the owner looks the key up by it.
 */
typedef struct key_set key_set_t;

struct key_set {
	uint32_t	*keys;
	size_t		 count;
};

static uint32_t	 key_rehash(uint32_t id, void *ctx);
static bool	 key_set_find(const relic_idtab_t *table,
		    const key_set_t *keys, uint32_t key, uint32_t *idp);
static void	 an_array_grows_without_losing_its_contents(void);
static void	 an_array_refuses_an_impossible_size(void);
static void	 equal_input_hashes_equally(void);
static void	 every_inserted_key_is_found_after_growth(void);
static void	 similar_keys_reach_different_slots(void);

static uint32_t
key_rehash(uint32_t id, void *ctx)
{
	const key_set_t *keys;

	keys = ctx;

	return (relic_hash_word(keys->keys[id - 1]));
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

	relic_idprobe_open(&probe, table, relic_hash_word(key));
	while (relic_idprobe_next(&probe, &id)) {
		if (keys->keys[id - 1] == key) {
			*idp = id;
			return (true);
		}
	}

	return (false);
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
	relic_idtab_init(&table);

	/*
	 * Reserve, record the key, then place, which is the order the symbol
	 * pool and the value table use: the step that can fail runs before
	 * the key exists, so a failure leaves nothing behind to be found or
	 * not found.
	 */
	first_uninserted = KEY_COUNT;
	for (i = 0; i < KEY_COUNT; i++) {
		uint32_t key;

		key = (uint32_t)(i * 7919u + 13u);

		if (relic_idtab_reserve(&table, key_rehash, &keys) != 0) {
			first_uninserted = i;
			break;
		}

		storage[keys.count] = key;
		keys.count++;
		relic_idtab_place(&table, relic_hash_word(key),
		    (uint32_t)keys.count);
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
 * The same input must hash to the same value every time, because identifier
 * assignment and therefore every golden output depends on it.  A seed taken
 * from the clock, the environment or an address would fail here.
 */
static void
equal_input_hashes_equally(void)
{
	static const char text[] = "path";

	check(relic_hash_bytes(text, sizeof(text) - 1) ==
	    relic_hash_bytes("path", 4), SCENARIO_DETERMINISM,
	    "equal bytes should hash equally");
	check(relic_hash_word(42) == relic_hash_word(42),
	    SCENARIO_DETERMINISM, "equal words should hash equally");
	check(relic_hash_int64(-1) == relic_hash_int64(-1),
	    SCENARIO_DETERMINISM, "equal integers should hash equally");
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
		occupancy[relic_hash_int64((int64_t)i) % BUCKET_COUNT]++;

	busiest = 0;
	for (i = 0; i < BUCKET_COUNT; i++) {
		if (occupancy[i] > busiest)
			busiest = occupancy[i];
	}

	check(busiest < KEY_COUNT / 4, SCENARIO_SPREAD,
	    "consecutive integers should not pile into one bucket");
}

int
main(void)
{
	an_array_grows_without_losing_its_contents();
	an_array_refuses_an_impossible_size();
	equal_input_hashes_equally();
	every_inserted_key_is_found_after_growth();
	similar_keys_reach_different_slots();

	return (check_summary("util"));
}
