/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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

#define	SCENARIO_ACCOUNT	"the account follows a relation as it grows"
#define	SCENARIO_ARITY_MAX	"a relation of the greatest arity is exact"
#define	SCENARIO_BATCH		"a batch inserts what single insertions would"
#define	SCENARIO_BATCH_LIMIT	"a limit refuses a batch where it would one"
#define	SCENARIO_CONSISTENT	"a refused insertion leaves the relation whole"
#define	SCENARIO_EMPTY_TUPLE	"a relation of arity zero holds one tuple"
#define	SCENARIO_ENVELOPE	"a million tuples fit in the expected memory"
#define	SCENARIO_GROWTH		"the store stays a set as it grows"
#define	SCENARIO_MEMORY_LIMIT	"the memory limit refuses a tuple and says so"
#define	SCENARIO_OWNERSHIP	"a relation belongs to one predicate"
#define	SCENARIO_APPENDED	"an append holds what insertions would"
#define	SCENARIO_RANGES		"a range names the tuples added since a mark"
#define	SCENARIO_RESERVED	"a reservation for many holds them all"
#define	SCENARIO_SCREEN		"a screen finds what an insertion would"
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
 * The tuples both relations of the append scenario hold before any append,
 * so that the first batches bring keys the relations hold as well as new
 * ones.
 */
#define	APPEND_SEEDS	5u

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
static void	 the_account_follows_a_relation_as_it_grows(void);
static void	 a_relation_stays_where_it_was_put(void);
static void	 a_batch_inserts_what_single_insertions_would(void);
static bool	 insert_in_batches(relic_t *ctx, relic_relation_t *batched,
		    const relic_dom_t *tuples, bool *added);
static void	 compare_with_single(relic_t *ctx,
		    const relic_relation_t *batched, relic_relation_t *single,
		    const relic_dom_t *tuples, const bool *added);
static void	 a_limit_refuses_a_batch_where_it_would_one(void);
static void	 a_reservation_for_many_holds_them_all(void);
static void	 fill_tuple(relic_t *ctx, relic_dom_t *columns,
		    unsigned int ordinal);
static bool	 reserve_then_fill(relic_t *ctx, relic_relation_t *rel,
		    size_t added, unsigned int *ordinalp);
static void	 refused_as_too_large(relic_t *ctx, relic_relation_t *rel,
		    size_t added, const char *what);
static void	 an_append_holds_what_insertions_would(void);
static bool	 seed_twins(relic_t *ctx, relic_relation_t *one,
		    relic_relation_t *many, unsigned int seeds);
static unsigned int appended_key(unsigned int batch, unsigned int ordinal,
		    size_t i);
static bool	 append_tuples(relic_t *ctx, relic_relation_t *rel,
		    const relic_dom_t *tuples, size_t count);
static bool	 append_steps(relic_t *ctx, relic_relation_t *rel,
		    relic_append_t *append);
static bool	 classify_part(relic_relation_t *rel, relic_append_t *append,
		    size_t part);
static bool	 layouts_agree(const relic_relation_t *one,
		    const relic_relation_t *many);
static bool	 tables_agree(const relic_idtab_t *one,
		    const relic_idtab_t *many);
static bool	 key_stripes_agree(const relic_index_stripe_t *one,
		    const relic_index_stripe_t *many, uint16_t key_length);
static bool	 chains_agree(const relic_relation_t *one,
		    const relic_relation_t *many);
static void	 a_screen_finds_what_an_insertion_would(void);

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

	bzero(&where, sizeof(where));
	where.file = "test_relation.c";
	where.line = 1;
	where.column = 1;

	if (relic_pred_intern(ctx, name, strlen(name), arity, &where,
	    &pred) != 0) {
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

	rel = relic_relation_read(ctx, pred);
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

	binary = relic_relation_read(ctx, binary_pred);
	unary = relic_relation_read(ctx, unary_pred);
	if (binary == NULL || unary == NULL) {
		check(false, SCENARIO_OWNERSHIP,
		    "each predicate should have a relation");
		relic_destroy(ctx);
		return;
	}

	check(binary != unary, SCENARIO_OWNERSHIP,
	    "edge/2 and edge/1 should have relations of their own");
	check(relic_relation_read(ctx, binary_pred) == binary,
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

	check(relic_relation_read(ctx, RELIC_ID_NONE) == NULL,
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

	first = relic_relation_read(ctx, first_pred);
	second = relic_relation_read(ctx, second_pred);
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
 * slots hold beside what the values hold.
 *
 * Every value the scenario inserts is interned first, and the limit is set
 * to MEMORY_LIMIT bytes past what the values then hold, so that it is spent
 * on the relation alone.  Where the refusal lands depends on how the two
 * arrays happen to have doubled, so the scenario asserts what the limit
 * promises rather than a tuple count: no insertion is accepted once the
 * storage would pass the allowance, the storage never does pass it, and the
 * relation survives the refusal.
 */
static void
the_memory_limit_refuses_a_tuple_and_says_so(void)
{
	relic_t *ctx;
	relic_relation_t *rel;
	relic_dom_t columns[2];
	relic_dom_t left;
	char expected[128];
	const char *reported;
	unsigned int ordinal;
	size_t accepted;
	bool added;

	ctx = context(SCENARIO_MEMORY_LIMIT, NULL);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_MEMORY_LIMIT, "edge", 2);
	if (rel == NULL) {
		relic_destroy(ctx);
		return;
	}

	for (ordinal = 0; ordinal <= LIMIT_ATTEMPTS; ordinal++)
		(void)value(ctx, SCENARIO_MEMORY_LIMIT, ordinal);
	ctx->account.limit = relic_value_storage_bytes(ctx) +
	    ctx->account.predtab_bytes + MEMORY_LIMIT;
	snprintf(expected, sizeof(expected),
	    "limit exceeded: memory=%zu while deriving edge/2",
	    ctx->account.limit);

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
 * The account keeps step with a relation as its columns and the stripes of
 * its canonical index double many times over.  A grown relation is charged
 * what relic_relation_bytes() reports for it; cleared, it gives every byte
 * back; grown again from nothing, it is charged afresh.  Both sides derive
 * the bytes from capacities, so this pins the account's updates through
 * growth and release, not the byte formula itself.  The tuples are triples,
 * for values enough to be distinct.
 */
static void
the_account_follows_a_relation_as_it_grows(void)
{
	enum { TUPLE_COUNT = 100000 };

	relic_t *ctx;
	relic_relation_t *rel;
	relic_dom_t values[ENVELOPE_VALUES];
	relic_dom_t columns[3];
	unsigned int ordinal;
	bool added;

	ctx = context(SCENARIO_ACCOUNT, NULL);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_ACCOUNT, "triple", 3);
	if (rel == NULL) {
		relic_destroy(ctx);
		return;
	}

	for (ordinal = 0; ordinal < ENVELOPE_VALUES; ordinal++)
		values[ordinal] = value(ctx, SCENARIO_ACCOUNT, ordinal);

	for (ordinal = 0; ordinal < TUPLE_COUNT; ordinal++) {
		columns[0] = values[ordinal / ENVELOPE_VALUES /
		    ENVELOPE_VALUES];
		columns[1] = values[ordinal / ENVELOPE_VALUES %
		    ENVELOPE_VALUES];
		columns[2] = values[ordinal % ENVELOPE_VALUES];

		if (relic_relation_insert(ctx, rel, columns, &added) != 0 ||
		    !added) {
			checkf(false, SCENARIO_ACCOUNT,
			    "tuple %u is distinct and should be added",
			    ordinal);
			relic_destroy(ctx);
			return;
		}
	}

	checkf(relic_relation_total_bytes(ctx) == relic_relation_bytes(rel),
	    SCENARIO_ACCOUNT, "the account should hold the relation's %zu "
	    "bytes, not %zu", relic_relation_bytes(rel),
	    relic_relation_total_bytes(ctx));

	relic_relation_clear(ctx, rel);
	checkf(relic_relation_total_bytes(ctx) == 0, SCENARIO_ACCOUNT,
	    "a cleared relation should give back every byte, not keep %zu",
	    relic_relation_total_bytes(ctx));

	if (relic_relation_insert(ctx, rel, columns, &added) != 0 || !added) {
		check(false, SCENARIO_ACCOUNT,
		    "a cleared relation should take a tuple again");
		relic_destroy(ctx);
		return;
	}

	checkf(relic_relation_total_bytes(ctx) == relic_relation_bytes(rel),
	    SCENARIO_ACCOUNT, "the account should hold the regrown "
	    "relation's %zu bytes, not %zu", relic_relation_bytes(rel),
	    relic_relation_total_bytes(ctx));

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

	first = relic_relation_read(ctx, first_pred);
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

		if (relic_relation_read(ctx, other) == NULL) {
			checkf(false, SCENARIO_STABLE,
			    "relation %u should be created", ordinal);
			break;
		}

		again = relic_relation_read(ctx, first_pred);

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

/*
 * The tuple reserve_then_fill() inserts `ordinal`th: the first test value
 * paired with the one of that ordinal.
 */
static void
fill_tuple(relic_t *ctx, relic_dom_t *columns, unsigned int ordinal)
{
	pair(columns, value(ctx, SCENARIO_RESERVED, 0),
	    value(ctx, SCENARIO_RESERVED, ordinal));
}

/*
 * Reserve room in `rel` for `added` more tuples, counted by the stripe of
 * the canonical index each falls in, then insert that many new ones, each
 * pairing the first test value with the next of *ordinalp.  Returns whether
 * the reservation reached the size projected for it, kept the storage total
 * in step, and left the insertions nothing to grow.
 */
static bool
reserve_then_fill(relic_t *ctx, relic_relation_t *rel, size_t added,
    unsigned int *ordinalp)
{
	relic_tuple_growth_t growth;
	relic_dom_t columns[2];
	const relic_dom_t *storage;
	size_t bytes_before;
	size_t projected;
	size_t store_bytes_before;
	size_t i;
	bool added_one;

	bzero(&growth, sizeof(growth));
	for (i = 0; i < added; i++) {
		fill_tuple(ctx, columns, *ordinalp + 1 + (unsigned int)i);
		growth.by_stripe[relic_hash_stripe(relic_tuple_hash(
		    rel->hash_seed, columns, 2))]++;
	}

	bytes_before = relic_relation_bytes(rel);
	projected = relic_relation_bytes_after_growth(rel, &growth);
	store_bytes_before = ctx->account.store_bytes;

	if (relic_relation_reserve(ctx, rel, &growth, NULL) != 0 ||
	    relic_relation_bytes(rel) != projected ||
	    ctx->account.store_bytes - store_bytes_before !=
	    projected - bytes_before)
		return (false);

	storage = relic_relation_tuple(rel, RELIC_TUPLE_FIRST);
	for (i = 0; i < added; i++) {
		fill_tuple(ctx, columns, ++*ordinalp);
		if (!insert(ctx, SCENARIO_RESERVED, *ordinalp, rel, columns,
		    &added_one) || !added_one)
			return (false);
	}

	return (relic_relation_bytes(rel) == projected &&
	    relic_relation_tuple(rel, RELIC_TUPLE_FIRST) == storage);
}

/*
 * A reservation for many tuples grows the columns and the canonical index
 * once, to exactly the bytes projected for it, and charges them to the
 * storage total; the tuples it reserved for then go in without growing
 * anything.  A reservation that would take a relation past the identifier
 * space, or whose size cannot be expressed, is projected as no growth, and
 * refused as a relation too large to grow having changed nothing.  A
 * relation with a secondary index is refused as an internal error, since
 * the tuples appended into the room would be missing from the index.
 */
static void
a_reservation_for_many_holds_them_all(void)
{
	static const size_t addeds[] = { 0, 1, 2, 100, 3000 };
	static const char indexed[] = "internal error: r/2 was reserved for "
	    "many tuples while it has secondary indexes";

	relic_tuple_growth_t growth;
	relic_t *ctx;
	relic_relation_t *rel;
	relic_dom_t columns[2];
	const char *reported;
	unsigned int ordinal;
	uint32_t position;
	size_t bytes;
	size_t store_bytes;
	size_t a;
	bool added;

	ctx = context(SCENARIO_RESERVED, NULL);
	if (ctx == NULL)
		return;

	rel = relation(ctx, SCENARIO_RESERVED, "r", 2);
	if (rel == NULL) {
		relic_destroy(ctx);
		return;
	}

	for (ordinal = 1; ordinal <= 10; ordinal++) {
		pair(columns, value(ctx, SCENARIO_RESERVED, ordinal),
		    value(ctx, SCENARIO_RESERVED, ordinal));
		if (!insert(ctx, SCENARIO_RESERVED, ordinal, rel, columns,
		    &added)) {
			relic_destroy(ctx);
			return;
		}
	}

	for (a = 0; a < sizeof(addeds) / sizeof(addeds[0]); a++)
		checkf(reserve_then_fill(ctx, rel, addeds[a], &ordinal),
		    SCENARIO_RESERVED,
		    "reserving for %zu tuples should reach the projection, "
		    "charge it, and hold them without growing", addeds[a]);

	bytes = relic_relation_bytes(rel);
	store_bytes = ctx->account.store_bytes;
	refused_as_too_large(ctx, rel, SIZE_MAX, "an inexpressible count");
	refused_as_too_large(ctx, rel,
	    RELIC_TUPLE_MAX - relic_relation_count(rel) + 1,
	    "past the identifier space");
	check(relic_relation_bytes(rel) == bytes &&
	    ctx->account.store_bytes == store_bytes, SCENARIO_RESERVED,
	    "a refused reservation should grow and charge nothing");

	if (relic_relation_index(ctx, rel, 0x1, &position) != 0) {
		check(false, SCENARIO_RESERVED, "indexing r/2 should succeed");
		relic_destroy(ctx);
		return;
	}
	bytes = relic_relation_bytes(rel);
	store_bytes = ctx->account.store_bytes;
	bzero(&growth, sizeof(growth));
	growth.by_stripe[0] = 1;

	check(relic_relation_reserve(ctx, rel, &growth, NULL) != 0,
	    SCENARIO_RESERVED,
	    "reserving in a relation with a secondary index should fail");
	reported = relic_error(ctx);
	checkf(reported != NULL && strcmp(reported, indexed) == 0,
	    SCENARIO_RESERVED, "reserving in an indexed relation should "
	    "report \"%s\", not \"%s\"", indexed,
	    reported == NULL ? "(none)" : reported);
	check(relic_relation_bytes(rel) == bytes &&
	    ctx->account.store_bytes == store_bytes, SCENARIO_RESERVED,
	    "a refused reservation of an indexed relation should grow and "
	    "charge nothing");

	relic_destroy(ctx);
}

/*
 * Check that reserving `added` more tuples, all of them in the first stripe,
 * is projected as no growth and refused as a relation too large to grow.
 * `what` says how many that is, for the message.
 */
static void
refused_as_too_large(relic_t *ctx, relic_relation_t *rel, size_t added,
    const char *what)
{
	static const char expected[] =
	    "the relation of r/2 is too large to grow";

	relic_tuple_growth_t growth;
	const char *reported;

	bzero(&growth, sizeof(growth));
	growth.by_stripe[0] = added;

	checkf(relic_relation_bytes_after_growth(rel, &growth) ==
	    relic_relation_bytes(rel), SCENARIO_RESERVED,
	    "reserving %s should project no growth", what);
	checkf(relic_relation_reserve(ctx, rel, &growth, NULL) != 0,
	    SCENARIO_RESERVED, "reserving %s should fail", what);

	reported = relic_error(ctx);
	checkf(reported != NULL && strcmp(reported, expected) == 0,
	    SCENARIO_RESERVED,
	    "reserving %s should report \"%s\", not \"%s\"", what,
	    expected, reported == NULL ? "(none)" : reported);
}

/*
 * A round's merge gives a relation its new tuples as one append, a stripe
 * to a thread, and the relation must come out exactly as inserting them one
 * at a time would leave it: the same identifiers, the same chains in every
 * secondary index, and the same bytes, which the append's projection must
 * predict and the storage total must be charged; from the reservation until
 * the append is finished, the total counts the tables the reservation
 * replaces beside their replacements, as the projection of the bytes held
 * during the append predicts, and the append gives back what it charged the
 * round.  Two relations with the same indexes and the same tuples take the
 * same batches, one by insertion and one by append, from none to past
 * several doublings, with the keys appended_key() chooses.  A relation of
 * no columns takes its one tuple by append too.
 */
static void
an_append_holds_what_insertions_would(void)
{
	static const size_t counts[] = { 0, 1, 2, 7, 64, 300, 1000 };
	static const relic_dom_t nothing[1];
	relic_relation_t *one;
	relic_relation_t *many;
	relic_relation_t *unit;
	relic_dom_t *tuples;
	relic_t *ctx;
	unsigned int ordinal;
	unsigned int batch;
	size_t c;
	size_t i;
	bool added;
	bool built;

	ctx = context(SCENARIO_APPENDED, NULL);
	if (ctx == NULL)
		return;

	one = relation(ctx, SCENARIO_APPENDED, "one", 2);
	many = relation(ctx, SCENARIO_APPENDED, "many", 2);
	unit = relation(ctx, SCENARIO_APPENDED, "unit", 0);
	tuples = calloc(2 * 1000, sizeof(*tuples));
	built = one != NULL && many != NULL && unit != NULL && tuples != NULL;
	if (built)
		built = seed_twins(ctx, one, many, APPEND_SEEDS);
	if (!built) {
		check(false, SCENARIO_APPENDED, "the fixture is built");
		free(tuples);
		relic_destroy(ctx);
		return;
	}

	ordinal = APPEND_SEEDS;
	for (c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
		batch = ordinal;
		for (i = 0; i < counts[c]; i++, ordinal++)
			pair(&tuples[2 * i], value(ctx, SCENARIO_APPENDED,
			    appended_key(batch, ordinal, i)),
			    value(ctx, SCENARIO_APPENDED, 1000000u + ordinal));

		for (i = 0; i < counts[c]; i++)
			if (!insert(ctx, SCENARIO_APPENDED, (unsigned int)i,
			    one, &tuples[2 * i], &added))
				break;

		checkf(append_tuples(ctx, many, tuples, counts[c]),
		    SCENARIO_APPENDED, "an append of %zu tuples should reach "
		    "exactly the bytes projected, charged, and give back "
		    "its round's charge", counts[c]);
		checkf(relic_relation_bytes(many) == relic_relation_bytes(one),
		    SCENARIO_APPENDED, "after %zu more tuples the append "
		    "should hold %zu bytes as the insertions do, not %zu",
		    counts[c], relic_relation_bytes(one),
		    relic_relation_bytes(many));
		checkf(layouts_agree(one, many), SCENARIO_APPENDED,
		    "after %zu more tuples the append should grow every part "
		    "and number every key as the insertions do", counts[c]);
		checkf(chains_agree(one, many), SCENARIO_APPENDED,
		    "after %zu more tuples the append should hold the "
		    "insertions' tuples and chains", counts[c]);
	}

	check(append_tuples(ctx, unit, nothing, 1) &&
	    relic_relation_count(unit) == 1 &&
	    relic_relation_contains(unit, nothing), SCENARIO_APPENDED,
	    "a relation of no columns should take its tuple by append");
	check(relic_relation_total_bytes(ctx) ==
	    relic_relation_bytes(one) + relic_relation_bytes(many) +
	    relic_relation_bytes(unit), SCENARIO_APPENDED,
	    "the storage total should hold what the relations do");

	free(tuples);
	relic_destroy(ctx);
}

/*
 * Give two relations the same `seeds` tuples and the same two secondary
 * indexes, on the first column and on the second.  Returns whether both
 * took them.
 */
static bool
seed_twins(relic_t *ctx, relic_relation_t *one, relic_relation_t *many,
    unsigned int seeds)
{
	relic_relation_t *twins[2];
	relic_dom_t tuple[2];
	unsigned int ordinal;
	uint32_t position;
	size_t t;
	bool added;

	twins[0] = one;
	twins[1] = many;
	for (t = 0; t < 2; t++) {
		for (ordinal = 0; ordinal < seeds; ordinal++) {
			pair(tuple, value(ctx, SCENARIO_APPENDED, ordinal),
			    value(ctx, SCENARIO_APPENDED, 1000000u + ordinal));
			if (!insert(ctx, SCENARIO_APPENDED, ordinal, twins[t],
			    tuple, &added))
				return (false);
		}
		if (relic_relation_index(ctx, twins[t], 0x1, &position) != 0)
			return (false);
		if (relic_relation_index(ctx, twins[t], 0x2, &position) != 0)
			return (false);
	}

	return (true);
}

/*
 * The first column of tuple `i` of a batch, the `ordinal`th tuple made,
 * whose first tuple is the `batch`th: in turn one of seven keys every batch
 * shares, which after the first batches the relation holds; a key new to
 * the relation and to the batch; and one of eight keys new to the relation
 * that the batch repeats, whose second tuple joins the chain its first
 * starts.
 */
static unsigned int
appended_key(unsigned int batch, unsigned int ordinal, size_t i)
{
	switch (i % 3) {
	case 0:
		return (ordinal % 7);
	case 1:
		return (ordinal);
	default:
		return (2000000u + batch * 8u + (unsigned int)(i % 8));
	}
}

/*
 * Give a relation `count` tuples it does not hold, packed at its arity, as
 * one append, through every step a round's merge takes, here on one
 * thread.  Returns whether every step succeeded and the relation then holds
 * exactly the bytes projected, with the storage total charged for them,
 * and whether the append gave back its charge to the round.
 */
static bool
append_tuples(relic_t *ctx, relic_relation_t *rel, const relic_dom_t *tuples,
    size_t count)
{
	relic_append_t append;
	uint16_t arity;
	uint64_t hash;
	size_t i;
	bool done;

	arity = relic_relation_arity(rel);
	bzero(&append, sizeof(append));
	append.count = count;
	done = relic_relation_append_open(ctx, rel, &append) == 0;

	for (i = 0; done && i < count; i++) {
		hash = relic_tuple_hash(rel->hash_seed, &tuples[i * arity],
		    arity);
		append.growth.by_stripe[relic_hash_stripe(hash)]++;
		relic_relation_stage(rel, &append, i, &tuples[i * arity],
		    hash);
	}

	if (done)
		done = append_steps(ctx, rel, &append);
	relic_relation_append_close(ctx, &append);

	return (done && ctx->account.round_bytes == 0);
}

/*
 * The steps of an append once its tuples are staged: list every part,
 * classify every part, project, reserve, take, apply every part and
 * finish.  The parts are run last to first, since a merge's threads may
 * run them in any order.
 */
static bool
append_steps(relic_t *ctx, relic_relation_t *rel, relic_append_t *append)
{
	size_t store_before;
	size_t bytes_before;
	size_t projected;
	size_t during;
	size_t part;

	for (part = relic_relation_list_parts(append); part-- > 0;)
		relic_relation_list_append(append, part);
	for (part = relic_relation_classify_parts(append); part-- > 0;) {
		if (!classify_part(rel, append, part))
			return (false);
	}

	projected = relic_relation_bytes_after_append(rel, append);
	during = relic_relation_bytes_during_append(rel, append);
	bytes_before = relic_relation_bytes(rel);
	store_before = ctx->account.store_bytes;
	if (relic_relation_reserve_append(ctx, rel, append) != 0)
		return (false);
	if (ctx->account.store_bytes - store_before != during - bytes_before)
		return (false);

	relic_relation_take_append(ctx, rel, append);
	for (part = relic_relation_apply_parts(append); part-- > 0;)
		relic_relation_apply_append(rel, append, part);
	relic_relation_finish_append(ctx, rel, append);

	return (relic_relation_bytes(rel) == projected &&
	    ctx->account.store_bytes - store_before ==
	    projected - bytes_before);
}

/*
 * Classify one part of an append with a table of new keys that has room for
 * every tuple the part reads, as a merge makes one.  Returns whether the
 * table could be made.
 */
static bool
classify_part(relic_relation_t *rel, relic_append_t *append, size_t part)
{
	relic_new_keys_t new_keys;
	relic_idtab_t empty;
	size_t room;
	bool made;

	room = relic_relation_classify_tuples(append, part);
	relic_idtab_init(&empty, true);
	relic_idtab_init(&new_keys.table, true);
	new_keys.first = calloc(room + 1, sizeof(*new_keys.first));
	made = new_keys.first != NULL;
	if (made)
		made = relic_idtab_prepare_growth(&empty, room,
		    &new_keys.table) == 0;
	if (made)
		relic_relation_classify_append(rel, append, part, &new_keys);

	relic_idtab_reset(&new_keys.table);
	free(new_keys.first);

	return (made);
}

/*
 * Whether two relations with the same indexes are built alike, part for
 * part: every array and table at the same capacity, every table holding as
 * many entries, and every stripe of every secondary index numbering the
 * same keys, with the same values, first and last tuples and frontiers.
 * Equal totals of bytes could hide two parts grown differently, and equal
 * chains a stripe whose keys are numbered in another order.
 */
static bool
layouts_agree(const relic_relation_t *one, const relic_relation_t *many)
{
	size_t x;
	size_t s;

	if (one->payload.column_capacity != many->payload.column_capacity)
		return (false);

	for (s = 0; s < RELIC_STRIPES; s++) {
		if (!tables_agree(&one->payload.index[s],
		    &many->payload.index[s]))
			return (false);
	}

	for (x = 0; x < one->index_count; x++) {
		if (one->indexes[x].next_capacity !=
		    many->indexes[x].next_capacity)
			return (false);
		for (s = 0; s < RELIC_STRIPES; s++) {
			if (!key_stripes_agree(&one->indexes[x].stripes[s],
			    &many->indexes[x].stripes[s],
			    one->indexes[x].key_length))
				return (false);
		}
	}

	return (true);
}

static bool
tables_agree(const relic_idtab_t *one, const relic_idtab_t *many)
{
	return (one->capacity == many->capacity && one->count == many->count);
}

/*
 * Whether two stripes of secondary indexes keyed on `key_length` columns
 * are built alike (layouts_agree()).
 */
static bool
key_stripes_agree(const relic_index_stripe_t *one,
    const relic_index_stripe_t *many, uint16_t key_length)
{
	size_t k;

	if (!tables_agree(&one->keys, &many->keys) ||
	    one->key_count != many->key_count ||
	    one->entry_capacity != many->entry_capacity ||
	    one->frontier_capacity != many->frontier_capacity ||
	    one->key_value_capacity != many->key_value_capacity)
		return (false);

	for (k = 0; k < one->key_count; k++) {
		if (one->entries[k].first != many->entries[k].first ||
		    one->entries[k].last != many->entries[k].last ||
		    relic_atomic_u32_load_relaxed(&one->frontiers[k]) !=
		    relic_atomic_u32_load_relaxed(&many->frontiers[k]))
			return (false);
	}

	return (one->key_count == 0 || bcmp(one->key_values, many->key_values,
	    one->key_count * key_length * sizeof(*one->key_values)) == 0);
}

/*
 * Whether two relations with the same indexes hold the same tuples under
 * the same identifiers, find each by its columns, and chain each tuple's key
 * through the same tuples in every index.
 */
static bool
chains_agree(const relic_relation_t *one, const relic_relation_t *many)
{
	const relic_index_t *index_one;
	const relic_index_t *index_many;
	const relic_dom_t *tuple;
	relic_tuple_id_t walk_one;
	relic_tuple_id_t walk_many;
	relic_tuple_id_t id;
	uint32_t x;

	if (relic_relation_count(one) != relic_relation_count(many))
		return (false);

	for (id = RELIC_TUPLE_FIRST; id <= relic_relation_count(one); id++) {
		tuple = relic_relation_tuple(one, id);
		if (!relic_tuple_equal(tuple, relic_relation_tuple(many, id),
		    relic_relation_arity(one)) ||
		    relic_relation_find(many, tuple) != id)
			return (false);

		for (x = 0; x < one->index_count; x++) {
			index_one = relic_relation_index_at(one, x);
			index_many = relic_relation_index_at(many, x);
			walk_one = relic_index_first(index_one, one, tuple);
			walk_many = relic_index_first(index_many, many, tuple);
			while (walk_one == walk_many &&
			    walk_one != RELIC_ID_NONE) {
				walk_one = relic_index_next(index_one,
				    walk_one);
				walk_many = relic_index_next(index_many,
				    walk_many);
			}
			if (walk_one != walk_many)
				return (false);
		}
	}

	return (true);
}

/*
 * relic_relation_screen() is the read-only half of a batch insertion: over a
 * batch of which some tuples are held and some are not, it finds the held
 * ones and gives every tuple the hash the relation keys it by.  A batch
 * inserted with those hashes then adds exactly the tuples the screen did
 * not find, under the identifiers a batch insertion hashing for itself
 * gives them.
 */
static void
a_screen_finds_what_an_insertion_would(void)
{
	enum { HELD = 20, BATCH = 40 };
	uint64_t hashes[BATCH];
	bool added[BATCH];
	bool held[BATCH];
	relic_dom_t tuples[BATCH * 2];
	relic_dom_t columns[2];
	relic_relation_t *screened;
	relic_relation_t *plain;
	relic_config_t config;
	uint32_t position;
	size_t completed;
	relic_t *ctx;
	bool agree;
	bool one;
	size_t i;

	bzero(&config, sizeof(config));
	ctx = context(SCENARIO_SCREEN, &config);
	if (ctx == NULL)
		return;

	screened = relation(ctx, SCENARIO_SCREEN, "screened", 2);
	plain = relation(ctx, SCENARIO_SCREEN, "plain", 2);
	if (screened == NULL || plain == NULL ||
	    relic_relation_index(ctx, screened, 0x2, &position) != 0 ||
	    relic_relation_index(ctx, plain, 0x2, &position) != 0) {
		check(false, SCENARIO_SCREEN, "the fixture is built");
		relic_destroy(ctx);
		return;
	}

	for (i = 0; i < HELD; i++) {
		pair(columns, value(ctx, SCENARIO_SCREEN, (unsigned int)i),
		    value(ctx, SCENARIO_SCREEN, (unsigned int)(i % 3)));
		if (!insert(ctx, SCENARIO_SCREEN, (unsigned int)i, screened,
		    columns, &one) ||
		    !insert(ctx, SCENARIO_SCREEN, (unsigned int)i, plain,
		    columns, &one))
			break;
	}

	for (i = 0; i < BATCH; i++)
		pair(&tuples[i * 2], value(ctx, SCENARIO_SCREEN,
		    (unsigned int)(i + HELD / 2)), value(ctx, SCENARIO_SCREEN,
		    (unsigned int)((i + HELD / 2) % 3)));

	relic_relation_screen(screened, tuples, BATCH, hashes, held);
	agree = true;
	for (i = 0; i < BATCH; i++) {
		if (held[i] != (i + HELD / 2 < HELD) ||
		    hashes[i] != relic_tuple_hash(screened->hash_seed,
		    &tuples[i * 2], 2))
			agree = false;
	}
	check(agree, SCENARIO_SCREEN, "the screen finds the held tuples and "
	    "hashes each as the relation does");

	check(relic_relation_insert_batch_hashed(ctx, screened, tuples,
	    hashes, BATCH, added, &completed) == 0 && completed == BATCH,
	    SCENARIO_SCREEN, "the screened batch is inserted");
	agree = true;
	for (i = 0; i < BATCH; i++) {
		if (added[i] == held[i])
			agree = false;
	}
	check(agree, SCENARIO_SCREEN,
	    "the batch adds exactly what the screen did not find");

	check(relic_relation_insert_batch(ctx, plain, tuples, BATCH, added,
	    &completed) == 0 && relic_relation_count(plain) ==
	    relic_relation_count(screened) &&
	    bcmp(plain->payload.columns, screened->payload.columns,
	    relic_relation_count(plain) * 2 *
	    sizeof(*plain->payload.columns)) == 0,
	    SCENARIO_SCREEN, "the hashed batch numbers the tuples as the "
	    "plain one does");

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
	the_account_follows_a_relation_as_it_grows();
	a_relation_stays_where_it_was_put();
	a_batch_inserts_what_single_insertions_would();
	a_limit_refuses_a_batch_where_it_would_one();
	a_reservation_for_many_holds_them_all();
	an_append_holds_what_insertions_would();
	a_screen_finds_what_an_insertion_would();

	return (check_summary("relation"));
}
