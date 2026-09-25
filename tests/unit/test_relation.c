/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the relation store.
 *
 * Three properties carry everything built on top of it and none of them is
 * visible in a return value that a careless implementation would get wrong
 * loudly.
 *
 * A relation is a set.  Inserting a tuple it already holds must change
 * nothing and must say so, because that single bit is how the evaluator
 * learns it has reached a fixed point.  A store that reported a duplicate as
 * new would not fail; it would make every recursive program run forever.
 *
 * Tuple identifiers are insertion order and never move.  Semi-naive
 * evaluation names the tuples derived in one iteration by a range of them, so
 * a store that reordered or reused an identifier would hand the next
 * iteration a delta describing different tuples than the ones it derived.
 *
 * A refused insertion leaves the relation untouched.  A limit that fired
 * halfway through a growth would leave a relation whose index and columns
 * disagree, and the wrong answers that followed would be reported as success.
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

#define	SCENARIO_ARITY_MAX	"a relation of the greatest arity is exact"
#define	SCENARIO_BATCH		"a batch inserts what single insertions would"
#define	SCENARIO_BATCH_LIMIT	"a limit refuses a batch where it would one"
#define	SCENARIO_CONSISTENT	"a refused insertion leaves the relation whole"
#define	SCENARIO_EMPTY_TUPLE	"a relation of arity zero holds one tuple"
#define	SCENARIO_ENVELOPE	"a million tuples fit in the expected memory"
#define	SCENARIO_GROWTH		"the store stays a set as it grows"
#define	SCENARIO_MEMORY_LIMIT	"the memory limit refuses a tuple and says so"
#define	SCENARIO_OWNERSHIP	"a relation belongs to one predicate"
#define	SCENARIO_RANGES		"a range names the tuples added since a mark"
#define	SCENARIO_STABLE		"a relation stays where it was put"
#define	SCENARIO_SET		"inserting a tuple twice adds it once"
#define	SCENARIO_TUPLE_LIMIT	"the tuple limit refuses a tuple and says so"

/*
 * How many tuples the growth scenario inserts.  Enough that both the column
 * array and the index double several times, since a rehash is where an
 * open-addressing table loses entries if its probe sequence is wrong.
 */
#define	GROWTH_COUNT	5000

/*
 * The envelope scenario's size, and the distinct values it draws columns
 * from.  A thousand values in two columns make a million distinct pairs, so
 * the relation is large while the value table stays small: what is being
 * measured is the cost of storing tuples, not the cost of interning values.
 */
#define	ENVELOPE_TUPLES	1000000u
#define	ENVELOPE_VALUES	1000u

/*
 * What a tuple of two columns may cost, in bytes of relation and index
 * storage.
 *
 * Its contents are eight bytes of columns and a four-byte index slot, so
 * twelve bytes is the floor that no arrangement of these two arrays can go
 * under.  Both are sized by doubling and the index keeps a quarter of its
 * slots empty, so each may hold a little under twice what its contents need,
 * which puts the ceiling near twenty-five.  Twenty-eight leaves room for that
 * without leaving room for a second representation of a tuple: an index
 * holding whole tuples beside their identifiers, or a header on each tuple,
 * would take a two-column relation past thirty at once.
 */
#define	ENVELOPE_MIN_BYTES_PER_TUPLE	12
#define	ENVELOPE_MAX_BYTES_PER_TUPLE	28

/*
 * How many further predicates the stability scenario creates.  The relation
 * table starts at eight entries and doubles, so this takes it through several
 * growths with the first relation held across each one.
 */
#define	STABLE_PREDICATES	40

/* The limit scenarios' allowances, both small enough to reach quickly. */
#define	TUPLE_LIMIT		10u
#define	MEMORY_LIMIT		2048u

/*
 * How many insertions a limit scenario will attempt before concluding that
 * the limit is not being enforced at all.  Far more than either allowance
 * can absorb.
 */
#define	LIMIT_ATTEMPTS		10000u

/*
 * The batch scenario derives BATCH_TUPLES pairs from BATCH_VALUES values in
 * each column, so that about a third of them repeat an earlier pair, and
 * hands them over in batches whose sizes cycle through BATCH_SIZES, one of
 * them larger than RELIC_INSERT_BATCH.
 */
#define	BATCH_TUPLES		6000u
#define	BATCH_VALUES		67u
#define	BATCH_SIZES		{ 1, 7, 64, 65, 200, 3 }

/*
 * The fixture helpers below all take the scenario they serve, and the ones a
 * scenario calls in a loop take the case as well.
 *
 * Nine scenarios share them and one sweeps five thousand insertions, so a
 * failure reported as "inserting a tuple" and nothing more would name neither
 * what was being tested nor which tuple failed, which is the situation the
 * harness exists to avoid.
 */
static relic_t	*context(const char *scenario, const relic_config_t *config);
static relic_pred_id_t predicate(relic_t *ctx, const char *scenario,
		    const char *name, uint16_t arity);
static relic_relation_t *relation(relic_t *ctx, const char *scenario,
		    const char *name, uint16_t arity);
static bool	 insert(relic_t *ctx, const char *scenario,
		    unsigned int ordinal, relic_relation_t *rel,
		    const relic_dom_t *columns, bool *addedp);
static relic_dom_t value(relic_t *ctx, const char *scenario,
		    unsigned int ordinal);
static void	 pair(relic_dom_t *columns, relic_dom_t left,
		    relic_dom_t right);
static size_t	 range_length(relic_tuple_range_t range);

static void	 inserting_a_tuple_twice_adds_it_once(void);
static void	 the_store_stays_a_set_as_it_grows(void);
static void	 a_range_names_the_tuples_added_since_a_mark(void);
static void	 a_relation_of_arity_zero_holds_one_tuple(void);
static void	 a_relation_of_the_greatest_arity_is_exact(void);
static void	 a_relation_belongs_to_one_predicate(void);
static void	 the_tuple_limit_refuses_a_tuple_and_says_so(void);
static void	 the_memory_limit_refuses_a_tuple_and_says_so(void);
static void	 a_refused_insertion_leaves_the_relation_whole(relic_t *ctx,
		    relic_relation_t *rel, const char *scenario);
static void	 a_million_tuples_fit_in_the_expected_memory(void);
static void	 a_relation_stays_where_it_was_put(void);
static void	 a_batch_inserts_what_single_insertions_would(void);
static bool	 insert_in_batches(relic_t *ctx, relic_relation_t *batched,
		    const relic_dom_t *tuples, bool *added);
static void	 compare_with_single(relic_t *ctx,
		    const relic_relation_t *batched, relic_relation_t *single,
		    const relic_dom_t *tuples, const bool *added);
static void	 a_limit_refuses_a_batch_where_it_would_one(void);

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
 * The identifier of the predicate with this name and arity, interning the
 * name and then the predicate.  RELIC_ID_NONE reports that the fixture could
 * not be built.
 */
static relic_pred_id_t
predicate(relic_t *ctx, const char *scenario, const char *name, uint16_t arity)
{
	relic_location_t where;
	relic_pred_id_t pred;
	uint32_t symbol;

	if (relic_symbol_intern(ctx, name, strlen(name), &symbol) != 0) {
		checkf(false, scenario, "the name of %s/%u should intern",
		    name, (unsigned int)arity);
		return (RELIC_ID_NONE);
	}

	bzero(&where, sizeof(where));
	where.file = "test_relation.c";
	where.line = 1;
	where.column = 1;

	if (relic_pred_intern(ctx, symbol, arity, &where, &pred) != 0) {
		checkf(false, scenario, "the predicate %s/%u should intern",
		    name, (unsigned int)arity);
		return (RELIC_ID_NONE);
	}

	return (pred);
}

/*
 * The relation of the predicate with this name and arity, for a scenario that
 * wants one relation and has no use for the predicate identifier.  A scenario
 * that wants both takes the two steps itself.
 */
static relic_relation_t *
relation(relic_t *ctx, const char *scenario, const char *name, uint16_t arity)
{
	relic_relation_t *rel;
	relic_pred_id_t pred;

	pred = predicate(ctx, scenario, name, arity);
	if (pred == RELIC_ID_NONE)
		return (NULL);

	rel = relic_relation_for(ctx, pred);
	if (rel == NULL)
		checkf(false, scenario, "%s/%u should have a relation", name,
		    (unsigned int)arity);

	return (rel);
}

/*
 * Insert a tuple that is expected to be accepted, reporting newness through
 * *addedp.  Returns false having complained when the insertion failed, so a
 * scenario that does not test failure need not repeat the message.
 *
 * `ordinal` says which tuple of the scenario this was, so a sweep of
 * thousands still names the one that failed.
 */
static bool
insert(relic_t *ctx, const char *scenario, unsigned int ordinal,
    relic_relation_t *rel, const relic_dom_t *columns, bool *addedp)
{
	if (relic_relation_insert(ctx, rel, columns, addedp) == 0)
		return (true);

	checkf(false, scenario, "inserting tuple %u should succeed", ordinal);

	return (false);
}

/*
 * The domain identifier of the nth test value, interned as the symbol "vn".
 * Distinct ordinals give distinct values, which is all any scenario here
 * needs of them.
 */
static relic_dom_t
value(relic_t *ctx, const char *scenario, unsigned int ordinal)
{
	char spelling[32];
	relic_dom_t dom;

	snprintf(spelling, sizeof(spelling), "v%u", ordinal);

	if (relic_value_intern_bytes(ctx, RELIC_SYMBOL, spelling,
	    strlen(spelling), &dom) != 0) {
		checkf(false, scenario, "the value %s should intern",
		    spelling);
		return (RELIC_ID_NONE);
	}

	return (dom);
}

static void
pair(relic_dom_t *columns, relic_dom_t left, relic_dom_t right)
{
	columns[0] = left;
	columns[1] = right;
}

static size_t
range_length(relic_tuple_range_t range)
{
	return ((size_t)(range.end - range.first));
}

/*
 * The property the whole store exists to have: a relation is a set.
 *
 * The second insertion of a tuple must report that nothing was added and must
 * leave the count where it was, and a tuple that was never inserted must not
 * be found.  Membership is asked both ways round because a store that
 * answered yes to everything would pass a test that only ever asked about
 * tuples it had inserted.
 */
static void
inserting_a_tuple_twice_adds_it_once(void)
{
	relic_t *ctx;
	relic_relation_t *rel;
	relic_dom_t stored[2];
	relic_dom_t absent[2];
	bool added;

	ctx = context(SCENARIO_SET, NULL);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_SET, "edge", 2);
	if (rel == NULL) {
		relic_destroy(ctx);
		return;
	}

	pair(stored, value(ctx, SCENARIO_SET, 1), value(ctx, SCENARIO_SET, 2));
	pair(absent, value(ctx, SCENARIO_SET, 2), value(ctx, SCENARIO_SET, 1));

	check(relic_relation_count(rel) == 0, SCENARIO_SET,
	    "a new relation should be empty");
	check(!relic_relation_contains(rel, stored), SCENARIO_SET,
	    "a new relation should contain nothing");

	if (!insert(ctx, SCENARIO_SET, 0, rel, stored, &added)) {
		relic_destroy(ctx);
		return;
	}
	check(added, SCENARIO_SET, "the first insertion should add the tuple");

	if (!insert(ctx, SCENARIO_SET, 0, rel, stored, &added)) {
		relic_destroy(ctx);
		return;
	}
	check(!added, SCENARIO_SET,
	    "the second insertion should add nothing");

	checkf(relic_relation_count(rel) == 1, SCENARIO_SET,
	    "the relation should hold one tuple, not %zu",
	    relic_relation_count(rel));
	check(relic_relation_contains(rel, stored), SCENARIO_SET,
	    "the tuple inserted should be found");
	check(!relic_relation_contains(rel, absent), SCENARIO_SET,
	    "a tuple with the columns reversed is a different tuple");

	relic_destroy(ctx);
}

/*
 * The same property across enough insertions to double both arrays several
 * times, with every tuple reinserted afterwards.
 *
 * A growth that dropped an entry from the index would not lose the tuple --
 * the columns are still there -- so the store would keep answering, and the
 * reinsertion pass is what exposes it: a tuple the index has forgotten is
 * reported as new and appended a second time, and the relation stops being a
 * set without anything failing.
 */
static void
the_store_stays_a_set_as_it_grows(void)
{
	relic_t *ctx;
	relic_relation_t *rel;
	relic_dom_t columns[2];
	relic_dom_t left;
	unsigned int ordinal;
	size_t reinserted;
	bool added;

	ctx = context(SCENARIO_GROWTH, NULL);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_GROWTH, "edge", 2);
	if (rel == NULL) {
		relic_destroy(ctx);
		return;
	}

	left = value(ctx, SCENARIO_GROWTH, 0);

	for (ordinal = 0; ordinal < GROWTH_COUNT; ordinal++) {
		pair(columns, left, value(ctx, SCENARIO_GROWTH, ordinal + 1));

		if (!insert(ctx, SCENARIO_GROWTH, ordinal, rel, columns,
		    &added)) {
			relic_destroy(ctx);
			return;
		}

		checkf(added, SCENARIO_GROWTH,
		    "tuple %u is distinct and should be added", ordinal);
	}

	checkf(relic_relation_count(rel) == GROWTH_COUNT, SCENARIO_GROWTH,
	    "the relation should hold %d tuples, not %zu", GROWTH_COUNT,
	    relic_relation_count(rel));

	reinserted = 0;

	for (ordinal = 0; ordinal < GROWTH_COUNT; ordinal++) {
		pair(columns, left, value(ctx, SCENARIO_GROWTH, ordinal + 1));

		if (!relic_relation_contains(rel, columns)) {
			checkf(false, SCENARIO_GROWTH,
			    "tuple %u should still be found after the growth",
			    ordinal);
			break;
		}

		if (!insert(ctx, SCENARIO_GROWTH, ordinal, rel, columns,
		    &added)) {
			relic_destroy(ctx);
			return;
		}

		if (added) {
			checkf(false, SCENARIO_GROWTH,
			    "tuple %u should not be added a second time",
			    ordinal);
			break;
		}

		reinserted++;
	}

	checkf(reinserted == GROWTH_COUNT, SCENARIO_GROWTH,
	    "every one of %d tuples should survive the growth, not %zu",
	    GROWTH_COUNT, reinserted);
	checkf(relic_relation_count(rel) == GROWTH_COUNT, SCENARIO_GROWTH,
	    "reinserting everything should add nothing, leaving %d tuples, "
	    "not %zu", GROWTH_COUNT, relic_relation_count(rel));

	/*
	 * The running total the memory limit is enforced against must agree
	 * with what the relation actually holds.  A total that drifted would
	 * make the limit fire early or never.
	 */
	checkf(relic_relation_total_bytes(ctx) == relic_relation_bytes(rel),
	    SCENARIO_GROWTH,
	    "the storage total (%zu) should be the one relation's size (%zu)",
	    relic_relation_total_bytes(ctx), relic_relation_bytes(rel));
	checkf(relic_relation_total_tuples(ctx) == GROWTH_COUNT,
	    SCENARIO_GROWTH,
	    "the tuple total should be %d, not %zu", GROWTH_COUNT,
	    relic_relation_total_tuples(ctx));

	relic_destroy(ctx);
}

/*
 * Identifier ranges, which is how a scan and a semi-naive delta read a
 * relation.
 *
 * A mark is taken part way through, and the range measured from it must name
 * exactly the tuples inserted afterwards -- no earlier one, and every later
 * one, in the order they were inserted.  A store that renumbered on a growth
 * would answer this wrongly while every membership test still passed.
 */
static void
a_range_names_the_tuples_added_since_a_mark(void)
{
	static const unsigned int before = 3;
	static const unsigned int after = 4;

	relic_t *ctx;
	relic_relation_t *rel;
	const relic_dom_t *stored;
	relic_tuple_range_t all;
	relic_tuple_range_t delta;
	relic_tuple_range_t outside;
	relic_tuple_id_t mark;
	relic_tuple_id_t id;
	relic_dom_t columns[2];
	relic_dom_t left;
	unsigned int ordinal;
	bool added;

	ctx = context(SCENARIO_RANGES, NULL);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_RANGES, "edge", 2);
	if (rel == NULL) {
		relic_destroy(ctx);
		return;
	}

	check(range_length(relic_relation_all(rel)) == 0, SCENARIO_RANGES,
	    "an empty relation should offer an empty range");

	left = value(ctx, SCENARIO_RANGES, 0);

	for (ordinal = 0; ordinal < before; ordinal++) {
		pair(columns, left, value(ctx, SCENARIO_RANGES, ordinal + 1));
		if (!insert(ctx, SCENARIO_RANGES, ordinal, rel, columns,
		    &added)) {
			relic_destroy(ctx);
			return;
		}
	}

	mark = relic_relation_end(rel);

	check(range_length(relic_relation_since(rel, mark)) == 0,
	    SCENARIO_RANGES,
	    "a mark taken before anything is added should name no tuples");

	for (ordinal = before; ordinal < before + after; ordinal++) {
		pair(columns, left, value(ctx, SCENARIO_RANGES, ordinal + 1));
		if (!insert(ctx, SCENARIO_RANGES, ordinal, rel, columns,
		    &added)) {
			relic_destroy(ctx);
			return;
		}
	}

	all = relic_relation_all(rel);
	delta = relic_relation_since(rel, mark);

	checkf(range_length(all) == before + after, SCENARIO_RANGES,
	    "the whole relation should be %u tuples, not %zu", before + after,
	    range_length(all));
	checkf(range_length(delta) == after, SCENARIO_RANGES,
	    "the delta should be the %u tuples added since the mark, not %zu",
	    after, range_length(delta));
	checkf(delta.end == all.end, SCENARIO_RANGES,
	    "the delta should end where the relation does (%u against %u)",
	    delta.end, all.end);

	/*
	 * Reading the range back must give the tuples in the order they were
	 * inserted, which is what makes a result reproducible.
	 */
	for (id = all.first; id < all.end; id++) {
		ordinal = (unsigned int)(id - all.first);

		pair(columns, left, value(ctx, SCENARIO_RANGES, ordinal + 1));
		stored = relic_relation_tuple(rel, id);

		if (stored == NULL) {
			checkf(false, SCENARIO_RANGES,
			    "identifier %u should name a tuple", id);
			continue;
		}

		checkf(stored[0] == columns[0] && stored[1] == columns[1],
		    SCENARIO_RANGES,
		    "identifier %u should name the %uth tuple inserted", id,
		    ordinal);
	}

	check(relic_relation_tuple(rel, RELIC_ID_NONE) == NULL,
	    SCENARIO_RANGES, "identifier zero should name no tuple");
	check(relic_relation_tuple(rel, all.end) == NULL, SCENARIO_RANGES,
	    "an identifier past the end should name no tuple");

	/*
	 * A mark outside the identifiers the relation has assigned is brought
	 * back inside them at both ends, so every identifier a range names is
	 * one that names a tuple.  A range starting below the first
	 * identifier would put identifier zero on the first step of an
	 * ordinary walk, where a consumer reading it gets NULL.
	 */
	outside = relic_relation_since(rel, RELIC_ID_NONE);

	checkf(outside.first == RELIC_TUPLE_FIRST, SCENARIO_RANGES,
	    "a mark below the first identifier should give a range starting "
	    "at %u, not %u", RELIC_TUPLE_FIRST, outside.first);
	checkf(range_length(outside) == before + after, SCENARIO_RANGES,
	    "and naming all %u tuples, not %zu", before + after,
	    range_length(outside));
	check(relic_relation_tuple(rel, outside.first) != NULL,
	    SCENARIO_RANGES,
	    "every identifier a range names should name a tuple");

	outside = relic_relation_since(rel, (relic_tuple_id_t)(all.end + 1));

	check(range_length(outside) == 0, SCENARIO_RANGES,
	    "a mark past the end should give an empty range");

	relic_destroy(ctx);
}

/*
 * A relation of arity zero, which the language reaches through a fact with no
 * arguments and which the store reaches through no case of its own.
 *
 * Its one tuple has no columns, so both insertions present the same empty
 * key: the first adds it and the second must find it.  A hash or a comparison
 * that was undefined for an empty key would make the second insertion add a
 * second copy, and the relation would answer "true" twice over.
 */
static void
a_relation_of_arity_zero_holds_one_tuple(void)
{
	relic_t *ctx;
	relic_relation_t *rel;
	bool added;

	ctx = context(SCENARIO_EMPTY_TUPLE, NULL);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_EMPTY_TUPLE, "ready", 0);
	if (rel == NULL) {
		relic_destroy(ctx);
		return;
	}

	check(!relic_relation_contains(rel, NULL), SCENARIO_EMPTY_TUPLE,
	    "an empty relation of arity zero holds no tuple");

	if (!insert(ctx, SCENARIO_EMPTY_TUPLE, 0, rel, NULL, &added)) {
		relic_destroy(ctx);
		return;
	}
	check(added, SCENARIO_EMPTY_TUPLE,
	    "the first insertion should add the empty tuple");

	if (!insert(ctx, SCENARIO_EMPTY_TUPLE, 0, rel, NULL, &added)) {
		relic_destroy(ctx);
		return;
	}
	check(!added, SCENARIO_EMPTY_TUPLE,
	    "the second insertion should find the first");

	checkf(relic_relation_count(rel) == 1, SCENARIO_EMPTY_TUPLE,
	    "the relation should hold one tuple, not %zu",
	    relic_relation_count(rel));
	check(relic_relation_contains(rel, NULL), SCENARIO_EMPTY_TUPLE,
	    "the empty tuple should be found once it is there");
	check(range_length(relic_relation_all(rel)) == 1,
	    SCENARIO_EMPTY_TUPLE, "its range should name one tuple");

	relic_destroy(ctx);
}

/*
 * A relation at the structural arity limit, where a tuple is as wide as the
 * language allows.
 *
 * Two tuples differing only in their last column are the case that matters: a
 * hash or a comparison that stopped short of the final column would treat
 * them as one, and the store would silently drop half of a relation.
 */
static void
a_relation_of_the_greatest_arity_is_exact(void)
{
	relic_t *ctx;
	relic_relation_t *rel;
	relic_dom_t wide[RELIC_MAX_ARITY];
	relic_dom_t altered[RELIC_MAX_ARITY];
	unsigned int column;
	bool added;

	ctx = context(SCENARIO_ARITY_MAX, NULL);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_ARITY_MAX, "wide", RELIC_MAX_ARITY);
	if (rel == NULL) {
		relic_destroy(ctx);
		return;
	}

	for (column = 0; column < RELIC_MAX_ARITY; column++)
		wide[column] = value(ctx, SCENARIO_ARITY_MAX, column);

	bcopy(wide, altered, sizeof(wide));
	altered[RELIC_MAX_ARITY - 1] = value(ctx, SCENARIO_ARITY_MAX,
	    RELIC_MAX_ARITY);

	if (!insert(ctx, SCENARIO_ARITY_MAX, 0, rel, wide, &added)) {
		relic_destroy(ctx);
		return;
	}
	check(added, SCENARIO_ARITY_MAX, "the wide tuple should be added");

	if (!insert(ctx, SCENARIO_ARITY_MAX, 0, rel, wide, &added)) {
		relic_destroy(ctx);
		return;
	}
	check(!added, SCENARIO_ARITY_MAX,
	    "the same wide tuple should not be added twice");

	if (!insert(ctx, SCENARIO_ARITY_MAX, 1, rel, altered, &added)) {
		relic_destroy(ctx);
		return;
	}
	check(added, SCENARIO_ARITY_MAX,
	    "a tuple differing only in its last column is a different tuple");

	checkf(relic_relation_count(rel) == 2, SCENARIO_ARITY_MAX,
	    "the relation should hold two tuples, not %zu",
	    relic_relation_count(rel));

	relic_destroy(ctx);
}

/*
 * Relations belong to predicates, and a predicate's identity is its name and
 * its arity.
 *
 * `edge/2` and `edge/1` are unrelated predicates, so they must hold unrelated
 * tuples; a store keyed on the name alone would answer queries about one from
 * the tuples of the other.  Both relations are asked for again after the
 * second is created, because creating it may move the table.
 */
static void
a_relation_belongs_to_one_predicate(void)
{
	relic_t *ctx;
	relic_relation_t *binary;
	relic_relation_t *unary;
	relic_pred_id_t binary_pred;
	relic_pred_id_t unary_pred;
	relic_dom_t columns[2];
	bool added;

	ctx = context(SCENARIO_OWNERSHIP, NULL);
	if (ctx == NULL)
		return;

	binary_pred = predicate(ctx, SCENARIO_OWNERSHIP, "edge", 2);
	unary_pred = predicate(ctx, SCENARIO_OWNERSHIP, "edge", 1);
	if (binary_pred == RELIC_ID_NONE || unary_pred == RELIC_ID_NONE) {
		relic_destroy(ctx);
		return;
	}

	binary = relic_relation_for(ctx, binary_pred);
	unary = relic_relation_for(ctx, unary_pred);
	if (binary == NULL || unary == NULL) {
		check(false, SCENARIO_OWNERSHIP,
		    "each predicate should have a relation");
		relic_destroy(ctx);
		return;
	}

	check(binary != unary, SCENARIO_OWNERSHIP,
	    "edge/2 and edge/1 should have relations of their own");
	check(relic_relation_for(ctx, binary_pred) == binary,
	    SCENARIO_OWNERSHIP,
	    "asking for a predicate's relation twice should give one relation");
	checkf(binary->arity == 2 && unary->arity == 1, SCENARIO_OWNERSHIP,
	    "each relation should take its arity from its predicate, not %u "
	    "and %u", binary->arity, unary->arity);

	pair(columns, value(ctx, SCENARIO_OWNERSHIP, 1),
	    value(ctx, SCENARIO_OWNERSHIP, 2));

	if (!insert(ctx, SCENARIO_OWNERSHIP, 0, binary, columns, &added)) {
		relic_destroy(ctx);
		return;
	}

	checkf(relic_relation_count(unary) == 0, SCENARIO_OWNERSHIP,
	    "a tuple in edge/2 should leave edge/1 empty, not holding %zu",
	    relic_relation_count(unary));
	check(!relic_relation_contains(unary, columns), SCENARIO_OWNERSHIP,
	    "edge/1 should not find a tuple inserted into edge/2");

	check(relic_relation_for(ctx, RELIC_ID_NONE) == NULL,
	    SCENARIO_OWNERSHIP,
	    "no relation belongs to an identifier naming no predicate");

	relic_destroy(ctx);
}

/*
 * Check that a relation which has just refused a tuple is exactly what it was
 * before the refusal.
 *
 * The count must stand, every tuple must still be found, and reinserting one
 * of them must still report that nothing was added.  The last is the sharp
 * one: it proves the index and the columns still agree, which a failure part
 * way through a growth is what would break.
 */
static void
a_refused_insertion_leaves_the_relation_whole(relic_t *ctx,
    relic_relation_t *rel, const char *scenario)
{
	relic_tuple_range_t all;
	const relic_dom_t *stored;
	relic_dom_t columns[2];
	size_t held;
	bool added;

	held = relic_relation_count(rel);
	all = relic_relation_all(rel);

	checkf(range_length(all) == held, scenario,
	    "the range should still name every one of %zu tuples, not %zu",
	    held, range_length(all));

	stored = relic_relation_tuple(rel, all.first);
	if (stored == NULL) {
		check(false, scenario,
		    "the first tuple should still be readable");
		return;
	}

	pair(columns, stored[0], stored[1]);

	check(relic_relation_contains(rel, columns), scenario,
	    "a tuple stored before the refusal should still be found");

	if (relic_relation_insert(ctx, rel, columns, &added) != 0) {
		check(false, scenario,
		    "reinserting a tuple already held should not need room");
		return;
	}

	check(!added, scenario,
	    "reinserting a tuple already held should add nothing");
	checkf(relic_relation_count(rel) == held, scenario,
	    "the relation should still hold %zu tuples, not %zu", held,
	    relic_relation_count(rel));
}

/*
 * The tuple limit, which counts across every relation.
 *
 * Tuples are inserted into two relations until one is refused.  The refusal
 * must arrive exactly when the allowance is used up, must name the limit, its
 * configured value and the predicate being derived into, and must leave the
 * relation usable.
 */
static void
the_tuple_limit_refuses_a_tuple_and_says_so(void)
{
	static const char expected[] =
	    "limit exceeded: tuples=10 while deriving edge/2";

	relic_t *ctx;
	relic_relation_t *first;
	relic_relation_t *second;
	relic_pred_id_t first_pred;
	relic_pred_id_t second_pred;
	relic_config_t config;
	relic_dom_t columns[2];
	relic_dom_t left;
	const char *reported;
	unsigned int ordinal;
	size_t accepted;
	bool added;

	bzero(&config, sizeof(config));
	config.max_tuples = TUPLE_LIMIT;

	ctx = context(SCENARIO_TUPLE_LIMIT, &config);
	if (ctx == NULL)
		return;

	first_pred = predicate(ctx, SCENARIO_TUPLE_LIMIT, "edge", 2);
	second_pred = predicate(ctx, SCENARIO_TUPLE_LIMIT, "seen", 2);
	if (first_pred == RELIC_ID_NONE || second_pred == RELIC_ID_NONE) {
		relic_destroy(ctx);
		return;
	}

	first = relic_relation_for(ctx, first_pred);
	second = relic_relation_for(ctx, second_pred);
	if (first == NULL || second == NULL) {
		check(false, SCENARIO_TUPLE_LIMIT,
		    "each predicate should have a relation");
		relic_destroy(ctx);
		return;
	}

	left = value(ctx, SCENARIO_TUPLE_LIMIT, 0);
	accepted = 0;

	/*
	 * Half the allowance goes into the second relation, so that the
	 * refusal can only be produced by a total kept across relations
	 * rather than by a count kept in the one being inserted into.
	 */
	for (ordinal = 0; ordinal < TUPLE_LIMIT / 2; ordinal++) {
		pair(columns, left,
		    value(ctx, SCENARIO_TUPLE_LIMIT, ordinal + 1));

		if (!insert(ctx, SCENARIO_TUPLE_LIMIT, ordinal, second,
		    columns, &added)) {
			relic_destroy(ctx);
			return;
		}
		accepted++;
	}

	for (ordinal = 0; ordinal < LIMIT_ATTEMPTS; ordinal++) {
		pair(columns, left,
		    value(ctx, SCENARIO_TUPLE_LIMIT, ordinal + 1));

		if (relic_relation_insert(ctx, first, columns, &added) != 0)
			break;

		accepted++;
	}

	checkf(accepted == TUPLE_LIMIT, SCENARIO_TUPLE_LIMIT,
	    "the limit should admit exactly %u tuples across all relations, "
	    "not %zu", TUPLE_LIMIT, accepted);

	reported = relic_error(ctx);
	checkf(reported != NULL && strcmp(reported, expected) == 0,
	    SCENARIO_TUPLE_LIMIT,
	    "the diagnostic should read \"%s\", not \"%s\"", expected,
	    reported == NULL ? "(none)" : reported);

	a_refused_insertion_leaves_the_relation_whole(ctx, first,
	    SCENARIO_TUPLE_LIMIT);

	relic_destroy(ctx);
}

/*
 * The memory limit, which counts the bytes every relation's columns and index
 * slots hold.
 *
 * Where the refusal lands depends on how the two arrays happen to have
 * doubled, so the scenario asserts what the limit promises rather than a
 * tuple count: no insertion is accepted once the storage would pass the
 * allowance, the storage never does pass it, and the relation survives the
 * refusal.
 */
static void
the_memory_limit_refuses_a_tuple_and_says_so(void)
{
	static const char expected[] =
	    "limit exceeded: memory=2048 while deriving edge/2";

	relic_t *ctx;
	relic_relation_t *rel;
	relic_config_t config;
	relic_dom_t columns[2];
	relic_dom_t left;
	const char *reported;
	unsigned int ordinal;
	size_t accepted;
	bool added;

	bzero(&config, sizeof(config));
	config.max_memory = MEMORY_LIMIT;

	ctx = context(SCENARIO_MEMORY_LIMIT, &config);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_MEMORY_LIMIT, "edge", 2);
	if (rel == NULL) {
		relic_destroy(ctx);
		return;
	}

	left = value(ctx, SCENARIO_MEMORY_LIMIT, 0);
	accepted = 0;

	for (ordinal = 0; ordinal < LIMIT_ATTEMPTS; ordinal++) {
		pair(columns, left,
		    value(ctx, SCENARIO_MEMORY_LIMIT, ordinal + 1));

		if (relic_relation_insert(ctx, rel, columns, &added) != 0)
			break;

		accepted++;
	}

	checkf(accepted > 0 && accepted < LIMIT_ATTEMPTS,
	    SCENARIO_MEMORY_LIMIT,
	    "the limit should admit some tuples and then refuse one, not %zu "
	    "of %u attempts", accepted, LIMIT_ATTEMPTS);
	checkf(relic_relation_total_bytes(ctx) <= MEMORY_LIMIT,
	    SCENARIO_MEMORY_LIMIT,
	    "storage should never pass the %u bytes allowed, and holds %zu",
	    MEMORY_LIMIT, relic_relation_total_bytes(ctx));

	reported = relic_error(ctx);
	checkf(reported != NULL && strcmp(reported, expected) == 0,
	    SCENARIO_MEMORY_LIMIT,
	    "the diagnostic should read \"%s\", not \"%s\"", expected,
	    reported == NULL ? "(none)" : reported);

	a_refused_insertion_leaves_the_relation_whole(ctx, rel,
	    SCENARIO_MEMORY_LIMIT);

	relic_destroy(ctx);
}

/*
 * A million tuples, and what they cost.
 *
 * The count is the point of the exercise -- an insertion path that was
 * accidentally quadratic would not finish -- and the memory envelope is what
 * makes the run an assertion rather than a stopwatch.  A store that grew a
 * header per tuple, or an index that kept whole tuples beside their
 * identifiers, would still pass every correctness scenario above and would
 * leave this range immediately.
 */
static void
a_million_tuples_fit_in_the_expected_memory(void)
{
	relic_t *ctx;
	relic_relation_t *rel;
	relic_dom_t values[ENVELOPE_VALUES];
	relic_dom_t columns[2];
	size_t bytes;
	size_t per_tuple;
	unsigned int ordinal;
	bool added;

	ctx = context(SCENARIO_ENVELOPE, NULL);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_ENVELOPE, "pair", 2);
	if (rel == NULL) {
		relic_destroy(ctx);
		return;
	}

	for (ordinal = 0; ordinal < ENVELOPE_VALUES; ordinal++)
		values[ordinal] = value(ctx, SCENARIO_ENVELOPE, ordinal);

	for (ordinal = 0; ordinal < ENVELOPE_TUPLES; ordinal++) {
		pair(columns, values[ordinal / ENVELOPE_VALUES],
		    values[ordinal % ENVELOPE_VALUES]);

		if (relic_relation_insert(ctx, rel, columns, &added) != 0) {
			checkf(false, SCENARIO_ENVELOPE,
			    "insertion %u of %u should succeed", ordinal,
			    ENVELOPE_TUPLES);
			relic_destroy(ctx);
			return;
		}

		if (!added) {
			checkf(false, SCENARIO_ENVELOPE,
			    "tuple %u is distinct and should be added",
			    ordinal);
			relic_destroy(ctx);
			return;
		}
	}

	checkf(relic_relation_count(rel) == ENVELOPE_TUPLES,
	    SCENARIO_ENVELOPE, "the relation should hold %u tuples, not %zu",
	    ENVELOPE_TUPLES, relic_relation_count(rel));

	bytes = relic_relation_total_bytes(ctx);
	per_tuple = bytes / ENVELOPE_TUPLES;

	checkf(per_tuple >= ENVELOPE_MIN_BYTES_PER_TUPLE &&
	    per_tuple <= ENVELOPE_MAX_BYTES_PER_TUPLE, SCENARIO_ENVELOPE,
	    "a tuple of two columns should cost between %d and %d bytes of "
	    "storage, not %zu (%zu bytes in all)",
	    ENVELOPE_MIN_BYTES_PER_TUPLE, ENVELOPE_MAX_BYTES_PER_TUPLE,
	    per_tuple, bytes);

	relic_destroy(ctx);
}

/*
 * A relation, once created, stays at one address for the life of the
 * context, however many other relations are created afterwards.
 *
 * Everything above this layer holds relations: a plan resolves the relation
 * of each body literal once and then scans it, and an evaluation holds the
 * head's relation across the whole of a rule.  If creating a relation could
 * move the ones already created, every one of those would be a dangling
 * pointer, and the rule that avoided it would have to be obeyed at each of
 * them -- a list that grows with every caller and is only ever as good as its
 * last entry.
 *
 * Enough predicates are interned to take the table through several growths,
 * and the first relation is used after each one.  A table of relations rather
 * than of pointers to them would move the first relation at the ninth
 * predicate and the insertion below would write into freed memory.
 */
static void
a_relation_stays_where_it_was_put(void)
{
	relic_t *ctx;
	relic_relation_t *first;
	relic_relation_t *again;
	relic_pred_id_t first_pred;
	relic_dom_t columns[2];
	char spelling[32];
	unsigned int ordinal;
	bool added;

	ctx = context(SCENARIO_STABLE, NULL);
	if (ctx == NULL)
		return;

	first_pred = predicate(ctx, SCENARIO_STABLE, "edge", 2);
	if (first_pred == RELIC_ID_NONE) {
		relic_destroy(ctx);
		return;
	}

	first = relic_relation_for(ctx, first_pred);
	if (first == NULL) {
		check(false, SCENARIO_STABLE, "the first relation is created");
		relic_destroy(ctx);
		return;
	}

	pair(columns, value(ctx, SCENARIO_STABLE, 1),
	    value(ctx, SCENARIO_STABLE, 2));

	if (!insert(ctx, SCENARIO_STABLE, 0, first, columns, &added)) {
		relic_destroy(ctx);
		return;
	}

	for (ordinal = 0; ordinal < STABLE_PREDICATES; ordinal++) {
		relic_pred_id_t other;

		snprintf(spelling, sizeof(spelling), "p%u", ordinal);

		other = predicate(ctx, SCENARIO_STABLE, spelling, 2);
		if (other == RELIC_ID_NONE)
			break;

		if (relic_relation_for(ctx, other) == NULL) {
			checkf(false, SCENARIO_STABLE,
			    "relation %u should be created", ordinal);
			break;
		}

		again = relic_relation_for(ctx, first_pred);

		checkf(again == first, SCENARIO_STABLE,
		    "the first relation should still be where it was after "
		    "%u more relations were created", ordinal + 1);

		/*
		 * Using the pointer taken before the growth is the part that
		 * matters; comparing addresses alone would not fault on a
		 * store that had freed it.
		 */
		checkf(relic_relation_count(first) == 1, SCENARIO_STABLE,
		    "the first relation should still hold its one tuple, not "
		    "%zu, after %u more relations were created",
		    relic_relation_count(first), ordinal + 1);
		checkf(relic_relation_contains(first, columns),
		    SCENARIO_STABLE,
		    "the first relation should still find its tuple after %u "
		    "more relations were created", ordinal + 1);
	}

	relic_destroy(ctx);
}

/*
 * A batch is inserted as its tuples would be one at a time, in order: each
 * is reported added exactly when a single insertion would have added it,
 * and takes the identifier it would have taken.
 *
 * The same sequence of pairs goes into one relation in batches and into
 * another one tuple at a time.  A pair repeats an earlier one about a third
 * of the time, often within the same batch, so a batch must find a
 * duplicate of a tuple it inserted itself; the canonical index grows many
 * times partway through a batch; and one batch size is larger than
 * RELIC_INSERT_BATCH, so it is inserted in chunks.  A relation of arity
 * zero takes a batch of two empty tuples, adding the first.
 */
static void
a_batch_inserts_what_single_insertions_would(void)
{
	relic_relation_t *batched;
	relic_relation_t *single;
	relic_relation_t *empty;
	relic_config_t config;
	relic_dom_t *tuples;
	bool *added;
	size_t completed;
	size_t i;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	ctx = context(SCENARIO_BATCH, &config);
	if (ctx == NULL)
		return;

	tuples = calloc(BATCH_TUPLES * 2, sizeof(*tuples));
	added = calloc(BATCH_TUPLES, sizeof(*added));
	batched = relation(ctx, SCENARIO_BATCH, "batched", 2);
	single = relation(ctx, SCENARIO_BATCH, "single", 2);
	empty = relation(ctx, SCENARIO_BATCH, "empty", 0);

	if (tuples != NULL && added != NULL && batched != NULL &&
	    single != NULL && empty != NULL) {
		for (i = 0; i < BATCH_TUPLES; i++)
			pair(&tuples[i * 2], value(ctx, SCENARIO_BATCH,
			    (unsigned int)(i % BATCH_VALUES)),
			    value(ctx, SCENARIO_BATCH,
			    (unsigned int)(i * 7 % (BATCH_VALUES + 2))));

		if (insert_in_batches(ctx, batched, tuples, added))
			compare_with_single(ctx, batched, single, tuples,
			    added);

		check(relic_relation_insert_batch(ctx, empty, tuples, 2,
		    added, &completed) == 0 && completed == 2 && added[0] &&
		    !added[1] && relic_relation_count(empty) == 1,
		    SCENARIO_BATCH, "a batch of two empty tuples adds the "
		    "first");
	} else {
		check(false, SCENARIO_BATCH, "the fixture is built");
	}

	free(tuples);
	free(added);
	relic_destroy(ctx);
}

/*
 * Insert the batch scenario's tuples into `batched` in batches whose sizes
 * cycle through BATCH_SIZES, recording in `added` which were added.
 * Returns whether every batch was inserted.
 */
static bool
insert_in_batches(relic_t *ctx, relic_relation_t *batched,
    const relic_dom_t *tuples, bool *added)
{
	static const size_t sizes[] = BATCH_SIZES;
	size_t completed;
	size_t size;
	size_t done;
	size_t i;

	for (done = 0, i = 0; done < BATCH_TUPLES; done += size, i++) {
		size = sizes[i % (sizeof(sizes) / sizeof(sizes[0]))];
		if (size > BATCH_TUPLES - done)
			size = BATCH_TUPLES - done;

		if (relic_relation_insert_batch(ctx, batched,
		    &tuples[done * 2], size, &added[done], &completed) != 0 ||
		    completed != size) {
			checkf(false, SCENARIO_BATCH, "the batch at tuple %zu "
			    "should be inserted whole", done);
			return (false);
		}
	}

	return (true);
}

/*
 * Insert the same tuples into `single` one at a time, and check that each
 * was added exactly when the batch added it and that both relations hold
 * the same tuples under the same identifiers.
 */
static void
compare_with_single(relic_t *ctx, const relic_relation_t *batched,
    relic_relation_t *single, const relic_dom_t *tuples, const bool *added)
{
	relic_tuple_id_t id;
	size_t first_wrong;
	size_t i;
	bool single_added;

	first_wrong = BATCH_TUPLES;
	for (i = 0; i < BATCH_TUPLES && first_wrong == BATCH_TUPLES; i++) {
		if (!insert(ctx, SCENARIO_BATCH, (unsigned int)i, single,
		    &tuples[i * 2], &single_added))
			return;
		if (single_added != added[i])
			first_wrong = i;
	}
	checkf(first_wrong == BATCH_TUPLES, SCENARIO_BATCH, "tuple %zu should "
	    "be added in the batch exactly when it is added alone",
	    first_wrong);

	checkf(relic_relation_count(batched) == relic_relation_count(single) &&
	    relic_relation_count(batched) < BATCH_TUPLES, SCENARIO_BATCH,
	    "both relations should hold the same %zu tuples, fewer than "
	    "offered, not %zu", relic_relation_count(single),
	    relic_relation_count(batched));

	first_wrong = 0;
	for (id = RELIC_TUPLE_FIRST; id <= relic_relation_count(single) &&
	    first_wrong == 0; id++) {
		if (bcmp(relic_relation_tuple(batched, id),
		    relic_relation_tuple(single, id),
		    2 * sizeof(relic_dom_t)) != 0)
			first_wrong = id;
	}
	checkf(first_wrong == 0, SCENARIO_BATCH, "tuple %zu should have the "
	    "identifier it has when inserted alone", first_wrong);
}

/*
 * A limit refuses the tuple of a batch a single insertion would have been
 * refused on, with the same diagnostic: the tuples before it are inserted
 * and reported added, and it and the rest are reported not added.  The
 * relation is left whole.
 */
static void
a_limit_refuses_a_batch_where_it_would_one(void)
{
	static const char expected[] =
	    "limit exceeded: tuples=10 while deriving edge/2";

	relic_relation_t *rel;
	relic_config_t config;
	relic_dom_t tuples[2 * 2 * TUPLE_LIMIT];
	bool added[2 * TUPLE_LIMIT];
	const char *reported;
	size_t first_wrong;
	size_t completed;
	size_t i;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	config.max_tuples = TUPLE_LIMIT;
	ctx = context(SCENARIO_BATCH_LIMIT, &config);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_BATCH_LIMIT, "edge", 2);
	if (rel == NULL) {
		relic_destroy(ctx);
		return;
	}

	for (i = 0; i < 2 * TUPLE_LIMIT; i++) {
		pair(&tuples[i * 2], value(ctx, SCENARIO_BATCH_LIMIT, 0),
		    value(ctx, SCENARIO_BATCH_LIMIT, (unsigned int)i + 1));
		added[i] = true;
	}

	check(relic_relation_insert_batch(ctx, rel, tuples, 2 * TUPLE_LIMIT,
	    added, &completed) != 0 && completed == TUPLE_LIMIT,
	    SCENARIO_BATCH_LIMIT, "the batch is refused after the insertions "
	    "the limit admits");

	first_wrong = 2 * TUPLE_LIMIT;
	for (i = 0; i < 2 * TUPLE_LIMIT && first_wrong == 2 * TUPLE_LIMIT;
	    i++) {
		if (added[i] != (i < TUPLE_LIMIT))
			first_wrong = i;
	}
	checkf(first_wrong == 2 * TUPLE_LIMIT &&
	    relic_relation_count(rel) == TUPLE_LIMIT, SCENARIO_BATCH_LIMIT,
	    "exactly the first %u tuples should be added, and tuple %zu's "
	    "report is wrong", TUPLE_LIMIT, first_wrong);

	reported = relic_error(ctx);
	checkf(reported != NULL && strcmp(reported, expected) == 0,
	    SCENARIO_BATCH_LIMIT, "the diagnostic should read \"%s\", not "
	    "\"%s\"", expected, reported == NULL ? "(none)" : reported);

	a_refused_insertion_leaves_the_relation_whole(ctx, rel,
	    SCENARIO_BATCH_LIMIT);

	relic_destroy(ctx);
}

int
main(void)
{
	inserting_a_tuple_twice_adds_it_once();
	the_store_stays_a_set_as_it_grows();
	a_range_names_the_tuples_added_since_a_mark();
	a_relation_of_arity_zero_holds_one_tuple();
	a_relation_of_the_greatest_arity_is_exact();
	a_relation_belongs_to_one_predicate();
	the_tuple_limit_refuses_a_tuple_and_says_so();
	the_memory_limit_refuses_a_tuple_and_says_so();
	a_million_tuples_fit_in_the_expected_memory();
	a_relation_stays_where_it_was_put();
	a_batch_inserts_what_single_insertions_would();
	a_limit_refuses_a_batch_where_it_would_one();

	return (check_summary("relation"));
}
