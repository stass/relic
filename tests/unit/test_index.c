/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for secondary indexes and relation statistics.
 *
 * An index that lost a tuple would not fail: a lookup would miss the tuple
 * and the answer would be short, reported as success.  So every scenario
 * here compares what an index says against what a scan of the relation says,
 * the one being the other's specification: the tuples a walk of key k visits
 * are exactly the tuples whose key columns hold k, in identifier order.
 *
 * The same applies to the two ways an index comes to hold a tuple -- built
 * over tuples already stored, or extended as each is inserted -- and to the
 * one moment the two meet, an insertion while a lookup is walking, which is
 * what a lookup inside a fixed point does all the time.  A statistic is
 * compared with a count taken by hand, before and after the relation changes
 * under it.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_ACCOUNTING	"an index is charged exactly"
#define	SCENARIO_BUILD		"an index built late matches one kept up"
#define	SCENARIO_CHAINS		"a lookup visits a key's tuples in order"
#define	SCENARIO_DISCARD	"a discard drops every index"
#define	SCENARIO_DISTINCT	"a distinct count follows the tuples"
#define	SCENARIO_FRONTIER	"a delta lookup starts where its delta does"
#define	SCENARIO_LATE_KEY	"a key first held late is found by its values"
#define	SCENARIO_LIMIT		"the memory limit refuses a build whole"
#define	SCENARIO_LIMIT_SWEEP	"the memory limit holds as indexes grow"
#define	SCENARIO_MASK		"an index is keyed on columns the relation has"
#define	SCENARIO_WALK		"a lookup walking while its key grows"

/*
 * How many tuples the scenarios that sweep insert, and how many distinct
 * values each column draws from.  Enough tuples that the key table and both
 * arrays grow several times, which is where a rehash loses keys if the hash
 * it recomputes is not the hash it placed them under; few enough values that
 * every key has a long chain.
 */
#define	SWEEP_TUPLES	3000u
#define	SWEEP_VALUES	17u

/* The build scenario's memory allowance: room for the tuples, not the index. */
#define	LIMIT_TUPLES	100u

/*
 * The limit sweep inserts up to LIMIT_SWEEP_TUPLES tuples into a relation
 * with two indexes, under LIMIT_SWEEP_STEPS limits spread evenly up to what
 * the unlimited run occupies.
 */
#define	LIMIT_SWEEP_TUPLES	300u
#define	LIMIT_SWEEP_STEPS	200u

static relic_t	*context(const char *scenario, const relic_config_t *config);
static relic_relation_t *relation(relic_t *ctx, const char *scenario,
		    const char *name, uint16_t arity);
static relic_dom_t value(relic_t *ctx, unsigned int ordinal);
static bool	 fill(relic_t *ctx, const char *scenario, relic_relation_t *rel,
		    unsigned int first, unsigned int count);
static bool	 index_agrees(const relic_relation_t *rel, uint32_t position,
		    const char *scenario);
static bool	 keys_agree(uint16_t arity, uint32_t key_mask,
		    const relic_dom_t *a, const relic_dom_t *b);
static relic_tuple_id_t first_since_by_scan(const relic_relation_t *rel,
		    uint32_t key_mask, const relic_dom_t *probe,
		    relic_tuple_id_t since);
static bool	 delta_lookups_agree(relic_t *ctx, relic_relation_t *rel,
		    uint32_t position, relic_tuple_id_t since);
static bool	 frontiers_rest_at(const relic_relation_t *rel,
		    uint32_t position, relic_tuple_id_t since);
static size_t	 distinct_by_hand(const relic_relation_t *rel, uint16_t column);
static size_t	 storage_by_hand(const relic_t *ctx);
static size_t	 index_bytes_by_hand(const relic_index_t *index);
static void	 a_key_first_held_late_is_found_by_its_values(void);
static void	 a_lookup_visits_a_keys_tuples_in_order(void);
static void	 an_index_built_late_matches_one_kept_up(void);
static void	 a_lookup_walking_while_its_key_grows(void);
static void	 a_delta_lookup_starts_where_its_delta_does(void);
static void	 a_discard_drops_every_index(void);
static void	 an_index_is_charged_exactly(void);
static void	 the_memory_limit_refuses_a_build_whole(void);
static bool	 fill_under_limit(size_t max_memory, size_t *unlimited);
static void	 the_memory_limit_holds_as_indexes_grow(void);
static void	 an_index_is_keyed_on_columns_it_has(void);
static void	 a_distinct_count_follows_the_tuples(void);

static relic_t *
context(const char *scenario, const relic_config_t *config)
{
	relic_t *ctx;

	ctx = relic_create_config(config);
	if (ctx == NULL)
		check(false, scenario, "a context should be created");

	return (ctx);
}

/*
 * The derived relation of a predicate with this name and arity, or NULL
 * having complained: the relation evaluation writes and a discard empties.
 * The predicate is made derived as evaluation's are, by a rule deriving it
 * joining a rule table, here a scratch one.
 */
static relic_relation_t *
relation(relic_t *ctx, const char *scenario, const char *name, uint16_t arity)
{
	relic_location_t where;
	relic_relation_t *rel;
	relic_ruletab_t rules;
	relic_rule_t rule;
	relic_pred_id_t pred;
	uint32_t symbol;
	int appended;

	bzero(&where, sizeof(where));
	where.file = "test_index.c";
	where.line = 1;
	where.column = 1;

	if (relic_symbol_intern(ctx, name, strlen(name), &symbol) != 0 ||
	    relic_pred_intern(ctx, symbol, arity, &where, &pred) != 0) {
		checkf(false, scenario, "%s/%u should intern", name,
		    (unsigned int)arity);
		return (NULL);
	}

	bzero(&rules, sizeof(rules));
	bzero(&rule, sizeof(rule));
	rule.head = pred;
	rule.head_arity = arity;
	appended = relic_ruletab_append(ctx, &rules, &rule);
	relic_free_array(rules.entries);

	rel = appended == 0 ? relic_relation_written(ctx, pred) : NULL;
	if (rel == NULL)
		checkf(false, scenario, "%s/%u should have a relation", name,
		    (unsigned int)arity);

	return (rel);
}

/*
 * The integer value `ordinal`, interned.  Interning an integer allocates
 * nothing the scenarios need to release, and distinct ordinals give distinct
 * identifiers.
 */
static relic_dom_t
value(relic_t *ctx, unsigned int ordinal)
{
	relic_dom_t dom;

	if (relic_value_intern_integer(ctx, (int64_t)ordinal, &dom) != 0)
		return (RELIC_ID_NONE);

	return (dom);
}

/*
 * Insert the 3-ary tuples numbered first .. first + count - 1, where tuple i
 * is (i mod 17, i mod 5, i): distinct as whole tuples, with long runs of
 * repeated values in the first two columns.  Returns whether every insertion
 * succeeded and added a tuple.
 */
static bool
fill(relic_t *ctx, const char *scenario, relic_relation_t *rel,
    unsigned int first, unsigned int count)
{
	relic_dom_t tuple[3];
	unsigned int i;
	bool added;

	for (i = first; i < first + count; i++) {
		tuple[0] = value(ctx, i % SWEEP_VALUES);
		tuple[1] = value(ctx, i % 5);
		tuple[2] = value(ctx, i);

		if (relic_relation_insert(ctx, rel, tuple, &added) != 0 ||
		    !added) {
			checkf(false, scenario, "tuple %u should be added", i);
			return (false);
		}
	}

	return (true);
}

/*
 * Whether two tuples of one arity agree on every column of `key_mask`.
 */
static bool
keys_agree(uint16_t arity, uint32_t key_mask, const relic_dom_t *a,
    const relic_dom_t *b)
{
	uint16_t column;

	for (column = 0; column < arity; column++) {
		if ((key_mask & (UINT32_C(1) << column)) != 0 &&
		    a[column] != b[column])
			return (false);
	}

	return (true);
}

/*
 * The bytes an index's arrays occupy, added up from their capacities: the
 * key table's slots, each with its tag, the keys' entries and values, and
 * the chains.
 */
static size_t
index_bytes_by_hand(const relic_index_t *index)
{
	return (index->keys.capacity * (sizeof(uint32_t) + 1) +
	    index->entry_capacity * sizeof(relic_index_key_t) +
	    index->key_value_capacity * index->key_length *
	    sizeof(relic_dom_t) +
	    index->next_capacity * sizeof(relic_tuple_id_t));
}

/*
 * Whether the index at `position` agrees with a scan of the relation: for the
 * key of every tuple, walking the index from that key visits exactly the
 * tuples a scan finds holding it, in identifier order.
 */
static bool
index_agrees(const relic_relation_t *rel, uint32_t position,
    const char *scenario)
{
	const relic_index_t *index;
	const relic_dom_t *tuple;
	relic_tuple_id_t expected;
	relic_tuple_id_t walked;
	relic_tuple_id_t id;

	index = &rel->indexes[position];

	for (id = RELIC_TUPLE_FIRST; id <= rel->count; id++) {
		tuple = relic_relation_tuple(rel, id);

		walked = relic_index_first(index, rel, tuple);
		for (expected = RELIC_TUPLE_FIRST; expected <= rel->count;
		    expected++) {
			if (!keys_agree(rel->arity, index->key_mask,
			    relic_relation_tuple(rel, expected), tuple))
				continue;

			if (walked != expected) {
				checkf(false, scenario, "the walk from tuple %u "
				    "should reach %u, and reached %u",
				    (unsigned int)id, (unsigned int)expected,
				    (unsigned int)walked);
				return (false);
			}
			walked = relic_index_next(index, walked);
		}

		if (walked != RELIC_ID_NONE) {
			checkf(false, scenario, "the walk from tuple %u should "
			    "end, and went on to %u", (unsigned int)id,
			    (unsigned int)walked);
			return (false);
		}
	}

	return (true);
}

static size_t
distinct_by_hand(const relic_relation_t *rel, uint16_t column)
{
	relic_tuple_id_t earlier;
	relic_tuple_id_t id;
	relic_dom_t held;
	size_t distinct;
	bool seen;

	distinct = 0;
	for (id = RELIC_TUPLE_FIRST; id <= rel->count; id++) {
		held = relic_relation_tuple(rel, id)[column];

		seen = false;
		for (earlier = RELIC_TUPLE_FIRST; !seen && earlier < id;
		    earlier++)
			seen = relic_relation_tuple(rel, earlier)[column] == held;
		if (!seen)
			distinct++;
	}

	return (distinct);
}

/*
 * The storage total recomputed from every relation, which is what the
 * running total the limit reads must always equal.
 */
static size_t
storage_by_hand(const relic_t *ctx)
{
	size_t bytes;
	size_t position;

	bytes = 0;
	for (position = 0; position < ctx->store.derived.count; position++) {
		if (ctx->store.derived.entries[position] != NULL)
			bytes += relic_relation_bytes(
			    ctx->store.derived.entries[position]);
	}
	for (position = 0; position < ctx->store.bases.count; position++) {
		if (ctx->store.bases.entries[position] != NULL)
			bytes += relic_relation_bytes(
			    ctx->store.bases.entries[position]);
	}

	return (bytes);
}

/*
 * A lookup visits the tuples holding its key and no others, in identifier
 * order, under a key of one column and a key of two that are not adjacent;
 * a key no tuple holds has no tuples at all.
 */
static void
a_lookup_visits_a_keys_tuples_in_order(void)
{
	relic_relation_t *rel;
	relic_config_t config;
	relic_dom_t absent[3];
	uint32_t first_column;
	uint32_t outer_columns;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	ctx = context(SCENARIO_CHAINS, &config);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_CHAINS, "r", 3);
	if (rel == NULL || !fill(ctx, SCENARIO_CHAINS, rel, 0, SWEEP_TUPLES)) {
		relic_destroy(ctx);
		return;
	}

	if (relic_relation_index(ctx, rel, 0x1, &first_column) != 0 ||
	    relic_relation_index(ctx, rel, 0x5, &outer_columns) != 0) {
		check(false, SCENARIO_CHAINS, "both indexes should be built");
		relic_destroy(ctx);
		return;
	}

	check(index_agrees(rel, first_column, SCENARIO_CHAINS),
	    SCENARIO_CHAINS, "the index on column 0 agrees with a scan");
	check(index_agrees(rel, outer_columns, SCENARIO_CHAINS),
	    SCENARIO_CHAINS, "the index on columns 0 and 2 agrees with a scan");
	checkf(rel->indexes[first_column].key_count == SWEEP_VALUES,
	    SCENARIO_CHAINS, "column 0 should hold %u keys, not %zu",
	    SWEEP_VALUES, rel->indexes[first_column].key_count);

	absent[0] = value(ctx, SWEEP_VALUES + 1);
	absent[1] = value(ctx, 0);
	absent[2] = value(ctx, 0);
	check(relic_index_first(&rel->indexes[first_column], rel, absent) ==
	    RELIC_ID_NONE && relic_index_first(&rel->indexes[outer_columns],
	    rel, absent) == RELIC_ID_NONE, SCENARIO_CHAINS,
	    "a key no tuple holds has no tuples");

	relic_destroy(ctx);
}

/*
 * An index built over tuples already stored holds what one built first and
 * kept up by every insertion holds, chain for chain.  Asking again for an
 * index with the same key gives the same position and builds nothing.
 */
static void
an_index_built_late_matches_one_kept_up(void)
{
	relic_relation_t *early;
	relic_relation_t *late;
	relic_config_t config;
	uint32_t again;
	uint32_t early_position;
	uint32_t late_position;
	relic_t *ctx;
	uint32_t i;

	bzero(&config, sizeof(config));
	ctx = context(SCENARIO_BUILD, &config);
	if (ctx == NULL)
		return;

	early = relation(ctx, SCENARIO_BUILD, "early", 3);
	late = relation(ctx, SCENARIO_BUILD, "late", 3);
	if (early == NULL || late == NULL ||
	    relic_relation_index(ctx, early, 0x3, &early_position) != 0 ||
	    !fill(ctx, SCENARIO_BUILD, early, 0, SWEEP_TUPLES) ||
	    !fill(ctx, SCENARIO_BUILD, late, 0, SWEEP_TUPLES) ||
	    relic_relation_index(ctx, late, 0x3, &late_position) != 0) {
		check(false, SCENARIO_BUILD, "both relations are indexed");
		relic_destroy(ctx);
		return;
	}

	check(index_agrees(early, early_position, SCENARIO_BUILD) &&
	    index_agrees(late, late_position, SCENARIO_BUILD), SCENARIO_BUILD,
	    "both indexes agree with a scan");

	for (i = 0; i < SWEEP_TUPLES; i++) {
		if (early->indexes[early_position].next[i] !=
		    late->indexes[late_position].next[i]) {
			checkf(false, SCENARIO_BUILD,
			    "tuple %u should have the same successor in both",
			    (unsigned int)i + 1);
			break;
		}
	}

	check(relic_relation_index(ctx, late, 0x3, &again) == 0 &&
	    again == late_position && late->index_count == 1, SCENARIO_BUILD,
	    "asking again finds the index already built");

	relic_destroy(ctx);
}

/*
 * An index stores each key's values from the tuple that introduced it, and
 * compares a probe against them, so a key first held late in the relation
 * is found by its own values.  The first half of the tuples hold only the
 * first three keys of column 0 and the rest bring in LATE_KEYS in all, each
 * new key's first tuple past every older key's; the index on column 0, kept
 * up as they arrive, and the index on columns 0 and 2, built once they
 * have, both agree with a scan, and a key no tuple holds has no tuples.
 */
#define	LATE_KEYS	40u
#define	LATE_TUPLES	400u

static void
a_key_first_held_late_is_found_by_its_values(void)
{
	relic_relation_t *rel;
	relic_config_t config;
	relic_dom_t tuple[3];
	uint32_t kept_up;
	uint32_t built;
	unsigned int key;
	unsigned int i;
	relic_t *ctx;
	bool added;

	bzero(&config, sizeof(config));
	ctx = context(SCENARIO_LATE_KEY, &config);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_LATE_KEY, "r", 3);
	if (rel == NULL || relic_relation_index(ctx, rel, 0x1, &kept_up) != 0) {
		check(false, SCENARIO_LATE_KEY, "the relation is indexed");
		relic_destroy(ctx);
		return;
	}

	for (i = 0; i < LATE_TUPLES; i++) {
		key = i < LATE_TUPLES / 2 ? i % 3 : i % LATE_KEYS;
		tuple[0] = value(ctx, key);
		tuple[1] = value(ctx, i);
		tuple[2] = value(ctx, i % 2);
		if (relic_relation_insert(ctx, rel, tuple, &added) != 0 ||
		    !added) {
			checkf(false, SCENARIO_LATE_KEY,
			    "tuple %u should be added", i);
			relic_destroy(ctx);
			return;
		}
	}

	if (relic_relation_index(ctx, rel, 0x5, &built) != 0) {
		check(false, SCENARIO_LATE_KEY, "the second index is built");
		relic_destroy(ctx);
		return;
	}

	checkf(rel->indexes[kept_up].key_count == LATE_KEYS, SCENARIO_LATE_KEY,
	    "column 0 should hold %u keys, not %zu", LATE_KEYS,
	    rel->indexes[kept_up].key_count);
	check(index_agrees(rel, kept_up, SCENARIO_LATE_KEY), SCENARIO_LATE_KEY,
	    "the index kept up agrees with a scan");
	check(index_agrees(rel, built, SCENARIO_LATE_KEY), SCENARIO_LATE_KEY,
	    "the index built late agrees with a scan");

	tuple[0] = value(ctx, LATE_KEYS);
	tuple[1] = value(ctx, 0);
	tuple[2] = value(ctx, 0);
	check(relic_index_first(&rel->indexes[kept_up], rel, tuple) ==
	    RELIC_ID_NONE && relic_index_first(&rel->indexes[built], rel,
	    tuple) == RELIC_ID_NONE, SCENARIO_LATE_KEY,
	    "a key no tuple holds has no tuples");

	relic_destroy(ctx);
}

/*
 * A lookup inside a fixed point walks a chain while the rule below it inserts
 * into the same relation.  The walk takes the relation's end when it starts
 * and stops at the first tuple at or past it, so it visits exactly the
 * tuples that held its key when it started -- what a scan started at the
 * same moment would visit -- however many tuples with the same key are
 * inserted during it.  A walk started afterwards visits them all.
 */
static void
a_lookup_walking_while_its_key_grows(void)
{
	relic_relation_t *rel;
	relic_config_t config;
	relic_tuple_id_t end;
	relic_tuple_id_t id;
	relic_dom_t probe[3];
	relic_dom_t tuple[3];
	uint32_t position;
	size_t visited;
	size_t before;
	relic_t *ctx;
	bool added;

	bzero(&config, sizeof(config));
	ctx = context(SCENARIO_WALK, &config);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_WALK, "grow", 3);
	if (rel == NULL || !fill(ctx, SCENARIO_WALK, rel, 0, 200) ||
	    relic_relation_index(ctx, rel, 0x1, &position) != 0) {
		check(false, SCENARIO_WALK, "the relation is indexed");
		relic_destroy(ctx);
		return;
	}

	bzero(probe, sizeof(probe));
	probe[0] = value(ctx, 0);
	before = 0;
	for (id = RELIC_TUPLE_FIRST; id <= rel->count; id++) {
		if (relic_relation_tuple(rel, id)[0] == probe[0])
			before++;
	}

	/*
	 * Each step of the walk inserts a tuple with the key being walked,
	 * which lands at the end of the very chain the walk is on.
	 */
	end = relic_relation_end(rel);
	visited = 0;
	id = relic_index_first(&rel->indexes[position], rel, probe);
	while (id != RELIC_ID_NONE && id < end) {
		visited++;

		tuple[0] = probe[0];
		tuple[1] = value(ctx, 7);
		tuple[2] = value(ctx, 100000 + (unsigned int)visited);
		if (relic_relation_insert(ctx, rel, tuple, &added) != 0 ||
		    !added) {
			check(false, SCENARIO_WALK, "the walk's insertions land");
			break;
		}

		id = relic_index_next(&rel->indexes[position], id);
	}

	checkf(visited == before, SCENARIO_WALK,
	    "the walk should visit the %zu tuples there when it started, "
	    "and visited %zu", before, visited);
	check(index_agrees(rel, position, SCENARIO_WALK), SCENARIO_WALK,
	    "a later walk finds every tuple, the new ones included");

	relic_destroy(ctx);
}

/*
 * The first tuple at or past `since` that agrees with `probe` on the columns
 * of `key_mask`, found by scanning: what relic_index_first_since() is
 * specified to return.
 */
static relic_tuple_id_t
first_since_by_scan(const relic_relation_t *rel, uint32_t key_mask,
    const relic_dom_t *probe, relic_tuple_id_t since)
{
	relic_tuple_id_t id;

	for (id = since; id <= rel->count; id++) {
		if (keys_agree(rel->arity, key_mask,
		    relic_relation_tuple(rel, id), probe))
			return (id);
	}

	return (RELIC_ID_NONE);
}

/*
 * Whether a delta lookup from `since` finds what a scan finds for every
 * value the first column draws from, and for one it never holds.
 */
static bool
delta_lookups_agree(relic_t *ctx, relic_relation_t *rel, uint32_t position,
    relic_tuple_id_t since)
{
	relic_index_t *index;
	relic_tuple_id_t expected;
	relic_tuple_id_t found;
	relic_dom_t probe[3];
	unsigned int key;

	index = &rel->indexes[position];
	bzero(probe, sizeof(probe));

	for (key = 0; key <= SWEEP_VALUES; key++) {
		probe[0] = value(ctx, key == SWEEP_VALUES ? 999999u : key);
		expected = first_since_by_scan(rel, index->key_mask, probe,
		    since);
		found = relic_index_first_since(index, rel, probe, since);
		if (found != expected) {
			checkf(false, SCENARIO_FRONTIER, "key %u from %u "
			    "should start at %u, and started at %u", key,
			    (unsigned int)since, (unsigned int)expected,
			    (unsigned int)found);
			return (false);
		}
	}

	return (true);
}

/*
 * Whether every key's frontier rests where a lookup from `since` left it:
 * at the key's first tuple at or past `since`, or at its last tuple when all
 * of them are below.  A frontier left anywhere earlier would have the next
 * lookup walk again what this one walked.
 */
static bool
frontiers_rest_at(const relic_relation_t *rel, uint32_t position,
    relic_tuple_id_t since)
{
	const relic_index_t *index;
	const relic_index_key_t *entry;
	relic_tuple_id_t expected;
	size_t key;

	index = &rel->indexes[position];

	for (key = 0; key < index->keys.count; key++) {
		entry = &index->entries[key];
		expected = first_since_by_scan(rel, index->key_mask,
		    relic_relation_tuple(rel, entry->first), since);
		if (expected == RELIC_ID_NONE)
			expected = entry->last;

		if (entry->frontier != expected) {
			checkf(false, SCENARIO_FRONTIER, "a frontier should "
			    "rest at %u, and rests at %u",
			    (unsigned int)expected,
			    (unsigned int)entry->frontier);
			return (false);
		}
	}

	return (true);
}

/*
 * A lookup of a semi-naive delta finds a key's first tuple at or past the
 * delta's start, starting from where the key's previous delta lookup left
 * off rather than from the key's first tuple.
 *
 * The relation grows round by round, as a member of a fixed point does, and
 * each round asks for every key from the round's start: a key whose chain
 * lay wholly below the previous start and has grown since is found from
 * where the previous walk ran off its end, and one that has not grown is
 * not found.  Every key's frontier then rests where its lookup stopped,
 * which is what makes the positioning walk each chain once over all the
 * rounds.  Asking a second time from the same start, and asking from a start
 * below the largest yet asked for, which no evaluation does, both find what
 * a scan finds and leave the frontiers where they were.
 */
static void
a_delta_lookup_starts_where_its_delta_does(void)
{
	relic_relation_t *rel;
	relic_config_t config;
	relic_tuple_id_t since;
	uint32_t position;
	unsigned int round;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	ctx = context(SCENARIO_FRONTIER, &config);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_FRONTIER, "member", 3);
	if (rel == NULL || !fill(ctx, SCENARIO_FRONTIER, rel, 0, 100) ||
	    relic_relation_index(ctx, rel, 0x1, &position) != 0) {
		check(false, SCENARIO_FRONTIER, "the relation is indexed");
		relic_destroy(ctx);
		return;
	}

	/*
	 * Round r adds r * 5 tuples, so the early rounds' deltas leave most
	 * keys' chains wholly below their start, and the later ones reach
	 * every key.
	 */
	for (round = 0; round < 12; round++) {
		since = relic_relation_end(rel);
		if (!fill(ctx, SCENARIO_FRONTIER, rel, 100 + round * 1000,
		    round * 5) ||
		    !delta_lookups_agree(ctx, rel, position, since) ||
		    !frontiers_rest_at(rel, position, since) ||
		    !delta_lookups_agree(ctx, rel, position, since) ||
		    !delta_lookups_agree(ctx, rel, position,
		    RELIC_TUPLE_FIRST) ||
		    !delta_lookups_agree(ctx, rel, position, since / 2) ||
		    !frontiers_rest_at(rel, position, since))
			break;
	}

	checkf(round == 12, SCENARIO_FRONTIER, "every round's delta lookups "
	    "should agree with a scan, and round %u did not", round);
	check(index_agrees(rel, position, SCENARIO_FRONTIER), SCENARIO_FRONTIER,
	    "a walk from a key's first tuple still finds every tuple");

	relic_destroy(ctx);
}

/*
 * A discard drops every index with the tuples, so which indexes a relation
 * keeps up depends only on what was asked of it since, never on the plans
 * and queries of an earlier program.  Asked for again after a refill, an
 * index is built afresh and agrees with a scan.
 */
static void
a_discard_drops_every_index(void)
{
	relic_relation_t *rel;
	relic_config_t config;
	uint32_t position;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	ctx = context(SCENARIO_DISCARD, &config);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_DISCARD, "r", 3);
	if (rel == NULL || !fill(ctx, SCENARIO_DISCARD, rel, 0, 500) ||
	    relic_relation_index(ctx, rel, 0x6, &position) != 0 ||
	    relic_relation_index(ctx, rel, 0x1, &position) != 0) {
		check(false, SCENARIO_DISCARD, "the relation is indexed");
		relic_destroy(ctx);
		return;
	}

	relic_relation_discard_all(ctx);

	checkf(rel->index_count == 0, SCENARIO_DISCARD,
	    "a discard should leave no index, not %zu", rel->index_count);
	checkf(relic_relation_total_bytes(ctx) == 0, SCENARIO_DISCARD,
	    "a discard should leave no storage, not %zu bytes",
	    relic_relation_total_bytes(ctx));

	if (!fill(ctx, SCENARIO_DISCARD, rel, 1000, 300) ||
	    relic_relation_index(ctx, rel, 0x6, &position) != 0) {
		check(false, SCENARIO_DISCARD,
		    "the refilled relation is indexed");
		relic_destroy(ctx);
		return;
	}

	checkf(position == 0 && rel->index_count == 1, SCENARIO_DISCARD,
	    "the first index after a discard should be the only one, at "
	    "position 0, not %zu at position %u", rel->index_count,
	    (unsigned int)position);
	check(index_agrees(rel, position, SCENARIO_DISCARD), SCENARIO_DISCARD,
	    "the rebuilt index agrees with a scan");

	relic_destroy(ctx);
}

/*
 * The storage total counts every index's arrays exactly, whether they grew
 * while the index was built or as tuples were inserted afterwards, and a
 * relation's own count includes them.
 */
static void
an_index_is_charged_exactly(void)
{
	relic_relation_t *rel;
	relic_config_t config;
	uint32_t position;
	size_t unindexed;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	ctx = context(SCENARIO_ACCOUNTING, &config);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_ACCOUNTING, "r", 3);
	if (rel == NULL || !fill(ctx, SCENARIO_ACCOUNTING, rel, 0, 1000)) {
		relic_destroy(ctx);
		return;
	}
	unindexed = relic_relation_bytes(rel);

	if (relic_relation_index(ctx, rel, 0x1, &position) != 0) {
		check(false, SCENARIO_ACCOUNTING, "the relation is indexed");
		relic_destroy(ctx);
		return;
	}

	checkf(relic_relation_bytes(rel) == unindexed +
	    relic_index_bytes(&rel->indexes[position]) &&
	    relic_index_bytes(&rel->indexes[position]) > 0,
	    SCENARIO_ACCOUNTING, "the relation should count its index, "
	    "%zu bytes over %zu", relic_relation_bytes(rel), unindexed);
	checkf(relic_index_bytes(&rel->indexes[position]) ==
	    index_bytes_by_hand(&rel->indexes[position]) &&
	    rel->indexes[position].key_value_capacity >=
	    rel->indexes[position].key_count, SCENARIO_ACCOUNTING,
	    "the index should count its tags and its keys' values, %zu "
	    "bytes, not %zu", index_bytes_by_hand(&rel->indexes[position]),
	    relic_index_bytes(&rel->indexes[position]));
	checkf(relic_relation_total_bytes(ctx) == storage_by_hand(ctx),
	    SCENARIO_ACCOUNTING, "after the build the total should be %zu, "
	    "not %zu", storage_by_hand(ctx), relic_relation_total_bytes(ctx));

	if (fill(ctx, SCENARIO_ACCOUNTING, rel, 1000, SWEEP_TUPLES)) {
		checkf(relic_relation_total_bytes(ctx) == storage_by_hand(ctx),
		    SCENARIO_ACCOUNTING, "after insertion the total should "
		    "be %zu, not %zu", storage_by_hand(ctx),
		    relic_relation_total_bytes(ctx));
		checkf(relic_index_bytes(&rel->indexes[position]) ==
		    index_bytes_by_hand(&rel->indexes[position]),
		    SCENARIO_ACCOUNTING, "after insertion the index should "
		    "count %zu bytes, not %zu",
		    index_bytes_by_hand(&rel->indexes[position]),
		    relic_index_bytes(&rel->indexes[position]));
	}

	relic_destroy(ctx);
}

/*
 * A memory limit that admits a relation's tuples but not an index over them
 * refuses the build, names the relation being indexed, publishes no index,
 * and leaves the storage total exactly what the relation holds.  The
 * relation is still the set it was.
 */
static void
the_memory_limit_refuses_a_build_whole(void)
{
	relic_relation_t *rel;
	relic_config_t config;
	relic_dom_t tuple[3];
	const char *reported;
	char expected[96];
	uint32_t position;
	size_t before;
	relic_t *ctx;
	bool added;

	bzero(&config, sizeof(config));
	ctx = context(SCENARIO_LIMIT, &config);
	if (ctx == NULL)
		return;

	/*
	 * The allowance is measured rather than assumed: what the tuples
	 * alone occupy, plus a little, is less than the index needs.
	 */
	rel = relation(ctx, SCENARIO_LIMIT, "edge", 3);
	if (rel == NULL || !fill(ctx, SCENARIO_LIMIT, rel, 0, LIMIT_TUPLES)) {
		relic_destroy(ctx);
		return;
	}
	config.max_memory = relic_relation_total_bytes(ctx) + 64;
	relic_destroy(ctx);

	ctx = context(SCENARIO_LIMIT, &config);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_LIMIT, "edge", 3);
	if (rel == NULL || !fill(ctx, SCENARIO_LIMIT, rel, 0, LIMIT_TUPLES)) {
		relic_destroy(ctx);
		return;
	}
	before = relic_relation_total_bytes(ctx);

	check(relic_relation_index(ctx, rel, 0x1, &position) != 0,
	    SCENARIO_LIMIT, "the build is refused");

	snprintf(expected, sizeof(expected),
	    "limit exceeded: memory=%zu while indexing edge/3",
	    config.max_memory);
	reported = relic_error(ctx);
	checkf(reported != NULL && strcmp(reported, expected) == 0,
	    SCENARIO_LIMIT, "the diagnostic should read \"%s\", not \"%s\"",
	    expected, reported == NULL ? "(none)" : reported);

	check(rel->index_count == 0, SCENARIO_LIMIT,
	    "no index is published");
	checkf(relic_relation_total_bytes(ctx) == before &&
	    before == storage_by_hand(ctx), SCENARIO_LIMIT,
	    "the total should be back at %zu, and is %zu", before,
	    relic_relation_total_bytes(ctx));

	tuple[0] = value(ctx, 0);
	tuple[1] = value(ctx, 0);
	tuple[2] = value(ctx, 0);
	check(relic_relation_contains(rel, tuple) &&
	    relic_relation_insert(ctx, rel, tuple, &added) == 0 && !added,
	    SCENARIO_LIMIT, "the relation still holds its tuples as a set");

	relic_destroy(ctx);
}

/*
 * Insert tuples into a relation indexed on {0} and on {1, 2}, both indexes
 * built while it is empty so that they grow only with insertions, until
 * LIMIT_SWEEP_TUPLES are held or the memory limit refuses one.
 *
 * With no limit (`max_memory` 0), the storage held after each insertion is
 * recorded in `unlimited`, which has room for LIMIT_SWEEP_TUPLES.  Under a
 * limit, each insertion must be refused exactly when the unlimited run held
 * more than the limit after it, and must otherwise leave the same storage
 * held: the limit neither lets storage past it nor refuses a tuple that
 * fits.  After every insertion the storage total must also equal what the
 * arrays hold.  Returns whether all of that held throughout.
 */
static bool
fill_under_limit(size_t max_memory, size_t *unlimited)
{
	relic_relation_t *rel;
	relic_config_t config;
	relic_dom_t tuple[3];
	uint32_t position;
	unsigned int i;
	size_t held;
	relic_t *ctx;
	bool should_refuse;
	bool refused;
	bool added;
	bool within;

	bzero(&config, sizeof(config));
	config.max_memory = max_memory;
	ctx = context(SCENARIO_LIMIT_SWEEP, &config);
	if (ctx == NULL)
		return (false);

	rel = relation(ctx, SCENARIO_LIMIT_SWEEP, "r", 3);
	if (rel == NULL ||
	    relic_relation_index(ctx, rel, 0x1, &position) != 0 ||
	    relic_relation_index(ctx, rel, 0x6, &position) != 0) {
		check(false, SCENARIO_LIMIT_SWEEP,
		    "the empty relation is indexed");
		relic_destroy(ctx);
		return (false);
	}

	within = true;
	for (i = 0; within && i < LIMIT_SWEEP_TUPLES; i++) {
		tuple[0] = value(ctx, i % SWEEP_VALUES);
		tuple[1] = value(ctx, i % 5);
		tuple[2] = value(ctx, i);
		refused = relic_relation_insert(ctx, rel, tuple, &added) != 0;
		held = relic_relation_total_bytes(ctx);

		if (max_memory == 0) {
			unlimited[i] = held;
			within = !refused;
		} else {
			should_refuse = unlimited[i] > max_memory;
			within = refused == should_refuse &&
			    (refused || held == unlimited[i]);
		}
		within = within && held == storage_by_hand(ctx);

		if (!within)
			checkf(false, SCENARIO_LIMIT_SWEEP, "under a limit of "
			    "%zu, insertion %u was %s holding %zu bytes, with "
			    "%zu in the arrays", max_memory, i,
			    refused ? "refused" : "admitted", held,
			    storage_by_hand(ctx));
		if (refused)
			break;
	}

	relic_destroy(ctx);

	return (within);
}

/*
 * The memory limit counts exactly what an insertion will make the tuple
 * store and every index allocate, so under any limit an insertion is refused
 * exactly when it would take the storage past the limit, before anything
 * grows.
 */
static void
the_memory_limit_holds_as_indexes_grow(void)
{
	size_t unlimited[LIMIT_SWEEP_TUPLES];
	size_t step;

	if (!fill_under_limit(0, unlimited))
		return;

	for (step = 1; step <= LIMIT_SWEEP_STEPS; step++) {
		if (!fill_under_limit(unlimited[LIMIT_SWEEP_TUPLES - 1] * step /
		    LIMIT_SWEEP_STEPS, unlimited))
			return;
	}
}

/*
 * A key mask must name at least one column but not all of them, and only
 * columns the relation has.  Anything else is refused as the caller's
 * mistake it is.
 */
static void
an_index_is_keyed_on_columns_it_has(void)
{
	relic_relation_t *rel;
	relic_relation_t *wide;
	relic_config_t config;
	uint32_t position;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	ctx = context(SCENARIO_MASK, &config);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_MASK, "r", 3);
	wide = relation(ctx, SCENARIO_MASK, "wide", RELIC_MAX_ARITY);
	if (rel == NULL || wide == NULL) {
		relic_destroy(ctx);
		return;
	}

	check(relic_relation_index(ctx, rel, 0, &position) != 0,
	    SCENARIO_MASK, "an empty key is refused");
	check(relic_relation_index(ctx, rel, 0x8, &position) != 0,
	    SCENARIO_MASK, "a column past the arity is refused");
	check(relic_relation_index(ctx, rel, 0x7, &position) != 0,
	    SCENARIO_MASK, "the whole tuple is refused");
	check(relic_relation_index(ctx, wide, UINT32_MAX, &position) != 0,
	    SCENARIO_MASK, "the whole of the widest tuple is refused");
	check(rel->index_count == 0, SCENARIO_MASK,
	    "a refused key builds nothing");
	check(relic_relation_index(ctx, wide, UINT32_C(1) << 31,
	    &position) == 0, SCENARIO_MASK,
	    "the last column of the widest relation is a key");

	relic_destroy(ctx);
}

/*
 * A distinct count is right when it is first taken, stays right while the
 * relation does not change, and is right again after it grows.  A discard
 * followed by a refill to the same number of different tuples is the case a
 * cache keyed on the tuple count alone would get wrong, and must not.
 */
static void
a_distinct_count_follows_the_tuples(void)
{
	relic_relation_t *rel;
	relic_config_t config;
	relic_dom_t tuple[3];
	size_t distinct;
	relic_t *ctx;
	uint16_t column;
	unsigned int i;
	bool added;

	bzero(&config, sizeof(config));
	ctx = context(SCENARIO_DISTINCT, &config);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_DISTINCT, "r", 3);
	if (rel == NULL || !fill(ctx, SCENARIO_DISTINCT, rel, 0, 12)) {
		relic_destroy(ctx);
		return;
	}

	for (column = 0; column < 3; column++) {
		checkf(relic_relation_distinct(ctx, rel, column,
		    &distinct) == 0 && distinct == distinct_by_hand(rel,
		    column), SCENARIO_DISTINCT,
		    "column %u should hold %zu values, not %zu",
		    (unsigned int)column, distinct_by_hand(rel, column),
		    distinct);
	}

	if (fill(ctx, SCENARIO_DISTINCT, rel, 12, 30))
		checkf(relic_relation_distinct(ctx, rel, 0, &distinct) == 0 &&
		    distinct == SWEEP_VALUES, SCENARIO_DISTINCT,
		    "after growing, column 0 should hold %u values, not %zu",
		    SWEEP_VALUES, distinct);

	/*
	 * Refill to the same 42 tuples, every one holding the same value in
	 * column 0.
	 */
	relic_relation_discard_all(ctx);
	for (i = 0; i < 42; i++) {
		tuple[0] = value(ctx, 0);
		tuple[1] = value(ctx, 0);
		tuple[2] = value(ctx, i);
		if (relic_relation_insert(ctx, rel, tuple, &added) != 0) {
			check(false, SCENARIO_DISTINCT, "the refill lands");
			break;
		}
	}

	checkf(relic_relation_count(rel) == 42 &&
	    relic_relation_distinct(ctx, rel, 0, &distinct) == 0 &&
	    distinct == 1, SCENARIO_DISTINCT,
	    "after a refill to the same count, column 0 should hold one "
	    "value, not %zu", distinct);
	check(relic_relation_distinct(ctx, rel, 3, &distinct) != 0,
	    SCENARIO_DISTINCT, "a column past the arity is refused");

	relic_destroy(ctx);
}

int
main(void)
{
	a_lookup_visits_a_keys_tuples_in_order();
	an_index_built_late_matches_one_kept_up();
	a_lookup_walking_while_its_key_grows();
	a_key_first_held_late_is_found_by_its_values();
	a_delta_lookup_starts_where_its_delta_does();
	a_discard_drops_every_index();
	an_index_is_charged_exactly();
	the_memory_limit_refuses_a_build_whole();
	the_memory_limit_holds_as_indexes_grow();
	an_index_is_keyed_on_columns_it_has();
	a_distinct_count_follows_the_tuples();

	return (check_summary("test_index"));
}
