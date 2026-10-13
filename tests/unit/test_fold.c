/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the aggregate functions' accumulators (lib/fold.c).
 *
 * A group's rows are folded one at a time into one accumulator, or split
 * into parts folded apart and then combined, as a parallel reduction
 * splits them.  Every split and every order of combination must give the
 * outcome an exact model of the group gives: a count is its rows, a sum is
 * its exact total or an overflow, an extreme is the least or the greatest
 * of one type, and a group holding a row outside the function's domain
 * has no value however far its sum lies outside int64_t.  The step a call
 * reports is checked too, since a reduction acts on it: a group is
 * reported invalidated exactly once, and an extreme is replaced exactly
 * when the model's extreme moves.
 */

#include "config.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "fold.h"
#include "harness.h"

#define	SCENARIO_SUM		"a sum's outcome is the exact model's"
#define	SCENARIO_EXTREME	"an extreme's outcome is the exact model's"
#define	SCENARIO_EMPTY		"an empty group has its function's outcome"
#define	SCENARIO_STEP		"a step reports what it did to the group"

/* Rows in one random group, parts it is split into, groups tried. */
#define	MAX_ROWS		12
#define	MAX_PARTS		4
#define	TRIALS			20000

typedef struct part part_t;

/*
 * One accumulator and what its caller keeps beside it: the rows folded,
 * the extreme's view, and how many steps reported the group invalidated.
 */
struct part {
	relic_fold_state_t	state;
	relic_value_view_t	extreme;
	uint64_t		rows;
	unsigned int		invalidations;
};

/*
 * How the parts of a split group are combined: each into one accumulator
 * in turn, ((A + B) + C), or each with the combination of those after it,
 * (A + (B + C)).  Either way every combination is made into an accumulator
 * with no rows of its own, so the invalidations it counts are the ones its
 * combinations reported.
 */
enum nesting {
	NEST_LEFT,
	NEST_RIGHT
};

static void	 part_init(part_t *part);
static void	 part_add(enum relic_aggregate_fn fn, part_t *part,
		    const relic_value_view_t *value);
static void	 part_combine(enum relic_aggregate_fn fn, part_t *part,
		    const part_t *more);
static void	 fold_split(enum relic_aggregate_fn fn,
		    const relic_value_view_t *rows, size_t count,
		    const size_t *split, size_t parts, const size_t *order,
		    enum nesting nesting, part_t *whole);
static uint64_t	 next_random(uint64_t *state);
static void	 shuffle(uint64_t *seed, size_t *order, size_t count);
static relic_value_view_t integer_view(int64_t integer);
static relic_value_view_t text_view(enum relic_type type, const char *bytes);
static int	 model_order(const relic_value_view_t *left,
		    const relic_value_view_t *right);
static void	 counts_are_their_rows(void);
static void	 empty_groups_have_their_outcome(void);
static void	 invalidation_is_reported_once(void);
static void	 maximum_plus_one_minus_one_fits_however_split(void);
static void	 invalid_rows_come_before_overflow(void);
#ifdef __SIZEOF_INT128__
static void	 random_sums_agree_with_the_model(void);
#endif
static void	 random_extremes_agree_with_the_model(void);
static void	 mixed_types_leave_no_extreme(void);
static void	 equal_extremes_tie(void);

static void
part_init(part_t *part)
{
	bzero(part, sizeof(*part));
	relic_fold_init(&part->state);
}

static void
part_add(enum relic_aggregate_fn fn, part_t *part,
    const relic_value_view_t *value)
{
	switch (relic_fold_add(fn, &part->state, part->rows, &part->extreme,
	    value)) {
	case RELIC_FOLD_REPLACED:
		part->extreme = *value;
		break;
	case RELIC_FOLD_INVALIDATED:
		part->invalidations++;
		break;
	case RELIC_FOLD_KEPT:
	case RELIC_FOLD_TIED:
		break;
	}
	part->rows++;
}

/*
 * A part's invalidation was reported when it was folded; combining it
 * reports the whole's, which is counted afresh.
 */
static void
part_combine(enum relic_aggregate_fn fn, part_t *part, const part_t *more)
{
	switch (relic_fold_combine(fn, &part->state, part->rows,
	    &part->extreme, &more->state, more->rows, &more->extreme)) {
	case RELIC_FOLD_REPLACED:
		part->extreme = more->extreme;
		break;
	case RELIC_FOLD_INVALIDATED:
		part->invalidations++;
		break;
	case RELIC_FOLD_KEPT:
	case RELIC_FOLD_TIED:
		break;
	}
	part->rows += more->rows;
}

/*
 * Fold `count` rows split into `parts` parts, row i joining part split[i],
 * and combine the parts into `whole` in the order `order` gives, nested as
 * `nesting` says.  A row whose split[i] is `parts` joins no part: it is
 * added to the whole once the parts are combined, as a reduction goes on
 * adding rows to a group after merging a partial one into it.  Each part
 * must have reported its own invalidation at most once.
 */
static void
fold_split(enum relic_aggregate_fn fn, const relic_value_view_t *rows,
    size_t count, const size_t *split, size_t parts, const size_t *order,
    enum nesting nesting, part_t *whole)
{
	part_t part[MAX_PARTS];
	part_t outer;
	size_t i;

	for (i = 0; i < parts; i++)
		part_init(&part[i]);
	for (i = 0; i < count; i++) {
		if (split[i] < parts)
			part_add(fn, &part[split[i]], &rows[i]);
	}
	for (i = 0; i < parts; i++)
		checkf(part[i].invalidations <= 1, SCENARIO_STEP,
		    "part %zu should report its invalidation at most once, "
		    "not %u times", i, part[i].invalidations);

	part_init(whole);
	if (nesting == NEST_LEFT) {
		for (i = 0; i < parts; i++)
			part_combine(fn, whole, &part[order[i]]);
	} else {
		for (i = parts; i > 0; i--) {
			part_init(&outer);
			part_combine(fn, &outer, &part[order[i - 1]]);
			part_combine(fn, &outer, whole);
			*whole = outer;
		}
	}

	for (i = 0; i < count; i++) {
		if (split[i] == parts)
			part_add(fn, whole, &rows[i]);
	}
}

/*
 * xorshift64*, seeded per test so a failure names a reproducible trial.
 */
static uint64_t
next_random(uint64_t *state)
{
	*state ^= *state >> 12;
	*state ^= *state << 25;
	*state ^= *state >> 27;

	return (*state * UINT64_C(2685821657736338717));
}

/* Put 0 .. count-1 into `order` in a random order. */
static void
shuffle(uint64_t *seed, size_t *order, size_t count)
{
	size_t swap;
	size_t i;
	size_t j;

	for (i = 0; i < count; i++)
		order[i] = i;
	for (i = count; i > 1; i--) {
		j = next_random(seed) % i;
		swap = order[i - 1];
		order[i - 1] = order[j];
		order[j] = swap;
	}
}

static relic_value_view_t
integer_view(int64_t integer)
{
	relic_value_view_t view;

	bzero(&view, sizeof(view));
	view.type = RELIC_INTEGER;
	view.integer = integer;
	view.bytes = "";

	return (view);
}

static relic_value_view_t
text_view(enum relic_type type, const char *bytes)
{
	relic_value_view_t view;

	bzero(&view, sizeof(view));
	view.type = type;
	view.bytes = bytes;
	view.length = strlen(bytes);

	return (view);
}

/*
 * The order the extreme tests expect, worked out without the value layer:
 * integers numerically, and the NUL-terminated spellings those tests use
 * by strcmp(), which compares bytes as unsigned char as the language's
 * order does.  Negative, zero or positive as `left` is below, equal to or
 * above `right`; both are of one type.
 */
static int
model_order(const relic_value_view_t *left, const relic_value_view_t *right)
{
	if (left->type != RELIC_INTEGER)
		return (strcmp(left->bytes, right->bytes));

	if (left->integer < right->integer)
		return (-1);

	return (left->integer > right->integer ? 1 : 0);
}

/*
 * A count reads no value and is its rows however they were split.
 */
static void
counts_are_their_rows(void)
{
	static const size_t split[5] = { 0, 1, 1, 2, 0 };
	static const size_t order[3] = { 2, 0, 1 };
	relic_value_view_t rows[5];
	part_t whole;
	int64_t integer;
	size_t i;

	for (i = 0; i < 5; i++)
		rows[i] = i == 3 ? text_view(RELIC_SYMBOL, "a") :
		    integer_view((int64_t)i);

	fold_split(RELIC_AGG_COUNT, rows, 5, split, 3, order, NEST_LEFT,
	    &whole);
	integer = -1;
	check(relic_fold_outcome(RELIC_AGG_COUNT, &whole.state, whole.rows,
	    &integer) == RELIC_FOLD_VALUE && integer == 5 &&
	    whole.invalidations == 0, SCENARIO_SUM,
	    "five rows of mixed types split three ways should count five");
}

/*
 * A group with no rows: a count or a sum is zero, a min or a max has no
 * value and no note, and empty parts combined stay so.  An empty part
 * combined into a part with rows changes nothing.
 */
static void
empty_groups_have_their_outcome(void)
{
	static const enum relic_aggregate_fn fns[] = {
		RELIC_AGG_COUNT, RELIC_AGG_SUM, RELIC_AGG_MIN, RELIC_AGG_MAX
	};
	static const size_t order[2] = { 0, 1 };
	relic_value_view_t row;
	part_t whole;
	part_t empty;
	int64_t integer;
	size_t f;
	bool expected;

	for (f = 0; f < sizeof(fns) / sizeof(fns[0]); f++) {
		fold_split(fns[f], NULL, 0, NULL, 2, order, NEST_LEFT,
		    &whole);
		integer = -1;
		if (fns[f] == RELIC_AGG_COUNT || fns[f] == RELIC_AGG_SUM)
			expected = relic_fold_outcome(fns[f], &whole.state, 0,
			    &integer) == RELIC_FOLD_VALUE && integer == 0;
		else
			expected = relic_fold_outcome(fns[f], &whole.state, 0,
			    &integer) == RELIC_FOLD_EMPTY;
		checkf(expected && whole.rows == 0, SCENARIO_EMPTY,
		    "function %zu over two empty parts should give its empty "
		    "outcome", f);

		part_init(&whole);
		row = integer_view(7);
		part_add(fns[f], &whole, &row);
		part_init(&empty);
		part_combine(fns[f], &whole, &empty);
		integer = -1;
		if (fns[f] == RELIC_AGG_COUNT || fns[f] == RELIC_AGG_SUM)
			expected = relic_fold_outcome(fns[f], &whole.state,
			    whole.rows, &integer) == RELIC_FOLD_VALUE &&
			    integer == (fns[f] == RELIC_AGG_COUNT ? 1 : 7);
		else
			expected = relic_fold_outcome(fns[f], &whole.state,
			    whole.rows, &integer) == RELIC_FOLD_VALUE &&
			    whole.extreme.integer == 7;
		checkf(expected && whole.rows == 1, SCENARIO_EMPTY,
		    "function %zu: an empty part should not change a group "
		    "of one row", f);
	}
}

/*
 * The steps of one accumulator, call by call: the row or the combination
 * that first leaves a sum or an extreme with no value reports it, and
 * nothing after it does -- another invalid row, a valid one, or another
 * invalid part combined in.  A count is given no value at all, which it
 * must not read.
 */
static void
invalidation_is_reported_once(void)
{
	static const enum relic_aggregate_fn fns[3] = {
		RELIC_AGG_SUM, RELIC_AGG_MIN, RELIC_AGG_MAX
	};
	relic_value_view_t symbol;
	relic_value_view_t string;
	relic_value_view_t one;
	part_t invalid;
	part_t part;
	size_t f;

	symbol = text_view(RELIC_SYMBOL, "x");
	string = text_view(RELIC_STRING, "x");
	one = integer_view(1);

	for (f = 0; f < 3; f++) {
		part_init(&part);
		part_add(fns[f], &part, &symbol);
		checkf(part.invalidations == (fns[f] == RELIC_AGG_SUM ?
		    1u : 0u), SCENARIO_STEP, "function %zu: a first symbol "
		    "should invalidate a sum only", f);
		part_add(fns[f], &part, &string);
		part_add(fns[f], &part, &one);
		part_add(fns[f], &part, &symbol);
		checkf(part.invalidations == 1, SCENARIO_STEP,
		    "function %zu: later rows should report no second "
		    "invalidation, not %u in all", f, part.invalidations);

		part_init(&invalid);
		part_add(fns[f], &invalid, &one);
		part_add(fns[f], &invalid, &symbol);
		check(relic_fold_combine(fns[f], &part.state, part.rows,
		    &part.extreme, &invalid.state, invalid.rows,
		    &invalid.extreme) == RELIC_FOLD_KEPT, SCENARIO_STEP,
		    "an invalid part combined into an invalid group should "
		    "report nothing");

		part_init(&part);
		part_add(fns[f], &part, &one);
		check(relic_fold_combine(fns[f], &part.state, part.rows,
		    &part.extreme, &invalid.state, invalid.rows,
		    &invalid.extreme) == RELIC_FOLD_INVALIDATED, SCENARIO_STEP,
		    "an invalid part combined into a valid group should "
		    "invalidate it");
	}

	part_init(&part);
	check(relic_fold_add(RELIC_AGG_COUNT, &part.state, 0, &part.extreme,
	    NULL) == RELIC_FOLD_KEPT, SCENARIO_STEP,
	    "a count should take a row without its value");
}

/*
 * INT64_MAX, 1 and -1 total INT64_MAX, though a running total may leave
 * int64_t on the way: every order of the three, split every way into
 * parts, and the parts combined in every order, must give it.
 */
static void
maximum_plus_one_minus_one_fits_however_split(void)
{
	static const int64_t addends[3] = { INT64_MAX, 1, -1 };
	static const size_t permutations[6][3] = {
		{ 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 },
		{ 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 }
	};
	static const size_t splits[5][3] = {
		{ 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0, 0 },
		{ 0, 1, 2 }
	};
	relic_value_view_t rows[3];
	part_t whole;
	int64_t integer;
	size_t p;
	size_t s;
	size_t o;
	size_t i;

	for (p = 0; p < 6; p++) {
		for (i = 0; i < 3; i++)
			rows[i] = integer_view(addends[permutations[p][i]]);
		for (s = 0; s < 5; s++) {
			for (o = 0; o < 6; o++) {
				fold_split(RELIC_AGG_SUM, rows, 3, splits[s],
				    3, permutations[o], NEST_LEFT, &whole);
				integer = 0;
				checkf(relic_fold_outcome(RELIC_AGG_SUM,
				    &whole.state, whole.rows, &integer) ==
				    RELIC_FOLD_VALUE && integer == INT64_MAX,
				    SCENARIO_SUM,
				    "order %zu split %zu combined in order %zu "
				    "should total INT64_MAX", p, s, o);
			}
		}
	}
}

/*
 * A row that is not an integer leaves a sum with no value, and that comes
 * before the range: INT64_MAX, INT64_MAX and a symbol give the note, not
 * the overflow, wherever the symbol falls and however the rows are split.
 * Without the symbol the total overflows.
 */
static void
invalid_rows_come_before_overflow(void)
{
	static const size_t splits[5][3] = {
		{ 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0, 0 },
		{ 0, 1, 2 }
	};
	static const size_t orders[3][3] = {
		{ 0, 1, 2 }, { 1, 2, 0 }, { 2, 0, 1 }
	};
	relic_value_view_t rows[3];
	part_t whole;
	int64_t integer;
	size_t at;
	size_t s;
	size_t o;
	size_t i;

	for (at = 0; at < 3; at++) {
		for (i = 0; i < 3; i++)
			rows[i] = i == at ? text_view(RELIC_SYMBOL, "x") :
			    integer_view(INT64_MAX);
		for (s = 0; s < 5; s++) {
			for (o = 0; o < 3; o++) {
				fold_split(RELIC_AGG_SUM, rows, 3, splits[s],
				    3, orders[o], NEST_LEFT, &whole);
				checkf(relic_fold_outcome(RELIC_AGG_SUM,
				    &whole.state, whole.rows, &integer) ==
				    RELIC_FOLD_MIXED && whole.invalidations ==
				    1, SCENARIO_SUM,
				    "symbol at %zu, split %zu, order %zu should "
				    "leave the sum invalid, reported once", at,
				    s, o);
			}
		}
	}

	rows[2] = integer_view(INT64_MAX);
	fold_split(RELIC_AGG_SUM, rows, 2, splits[4], 2, orders[0], NEST_LEFT,
	    &whole);
	check(relic_fold_outcome(RELIC_AGG_SUM, &whole.state, whole.rows,
	    &integer) == RELIC_FOLD_OVERFLOW, SCENARIO_SUM,
	    "INT64_MAX twice should overflow");
}

#ifdef __SIZEOF_INT128__
/*
 * Random groups of integers drawn toward the extremes, now and then with
 * a symbol among them, split into random parts combined in a random
 * order, nested both ways, with some rows added after the combination:
 * the outcome must be the model's -- invalid when a row is not an
 * integer, else the exact total or an overflow -- and an invalid group is
 * reported invalidated exactly once after its parts.
 */
static void
random_sums_agree_with_the_model(void)
{
	static const int64_t edges[] = {
		INT64_MAX, INT64_MIN, INT64_MAX - 1, INT64_MIN + 1, 1, -1, 0
	};
	static const enum nesting nestings[2] = { NEST_LEFT, NEST_RIGHT };
	relic_value_view_t rows[MAX_ROWS];
	size_t split[MAX_ROWS];
	size_t order[MAX_PARTS];
	part_t whole;
	__int128 total;
	uint64_t seed;
	uint64_t draw;
	int64_t integer;
	size_t trial;
	size_t count;
	size_t parts;
	size_t n;
	size_t i;
	enum relic_fold_outcome expected;
	enum relic_fold_outcome outcome;
	bool invalid;

	seed = UINT64_C(0x9e3779b97f4a7c15);
	for (trial = 0; trial < TRIALS; trial++) {
		count = next_random(&seed) % (MAX_ROWS + 1);
		parts = 1 + next_random(&seed) % MAX_PARTS;
		total = 0;
		invalid = false;
		for (i = 0; i < count; i++) {
			draw = next_random(&seed);
			if (draw % 41 == 0) {
				rows[i] = text_view(RELIC_STRING, "s");
				invalid = true;
			} else if (draw % 3 == 0) {
				rows[i] = integer_view((int64_t)
				    next_random(&seed));
			} else {
				rows[i] = integer_view(edges[(draw >> 8) %
				    (sizeof(edges) / sizeof(edges[0]))]);
			}
			if (rows[i].type == RELIC_INTEGER)
				total += rows[i].integer;
			split[i] = next_random(&seed) % (parts + 1);
		}
		shuffle(&seed, order, parts);

		if (invalid)
			expected = RELIC_FOLD_MIXED;
		else if (total < INT64_MIN || total > INT64_MAX)
			expected = RELIC_FOLD_OVERFLOW;
		else
			expected = RELIC_FOLD_VALUE;

		for (n = 0; n < 2; n++) {
			fold_split(RELIC_AGG_SUM, rows, count, split, parts,
			    order, nestings[n], &whole);

			integer = 0;
			outcome = relic_fold_outcome(RELIC_AGG_SUM,
			    &whole.state, whole.rows, &integer);
			checkf(outcome == expected &&
			    (expected != RELIC_FOLD_VALUE ||
			    integer == (int64_t)total) &&
			    whole.rows == count &&
			    whole.invalidations == (invalid ? 1u : 0u),
			    SCENARIO_SUM,
			    "trial %zu, nesting %zu: %zu rows in %zu parts "
			    "should give outcome %d, gave %d, invalidated %u "
			    "times", trial, n, count, parts, (int)expected,
			    (int)outcome, whole.invalidations);
		}
	}
}
#endif

/*
 * Random groups of one type -- integers, or symbols from a few spellings
 * so that equal values recur -- split into random parts combined in a
 * random order, nested both ways, with some rows added after the
 * combination: a min and a max must be the model's least and greatest,
 * and a group of no rows has neither.  The model orders values itself
 * rather than through the value layer the accumulators use.
 */
static void
random_extremes_agree_with_the_model(void)
{
	static const char *const spellings[] = {
		"", "a", "ab", "b", "ba", "z", "\x80", "\xff"
	};
	static const enum relic_aggregate_fn fns[2] = {
		RELIC_AGG_MIN, RELIC_AGG_MAX
	};
	static const enum nesting nestings[2] = { NEST_LEFT, NEST_RIGHT };
	relic_value_view_t rows[MAX_ROWS];
	relic_value_view_t best;
	size_t split[MAX_ROWS];
	size_t order[MAX_PARTS];
	part_t whole;
	uint64_t seed;
	int64_t integer;
	size_t trial;
	size_t count;
	size_t parts;
	size_t f;
	size_t n;
	size_t i;
	int order_to_best;
	bool text;
	bool agree;

	seed = UINT64_C(0x2545f4914f6cdd1d);
	for (trial = 0; trial < TRIALS; trial++) {
		count = next_random(&seed) % (MAX_ROWS + 1);
		parts = 1 + next_random(&seed) % MAX_PARTS;
		text = next_random(&seed) % 2 == 0;
		for (i = 0; i < count; i++) {
			if (text)
				rows[i] = text_view(RELIC_SYMBOL,
				    spellings[next_random(&seed) %
				    (sizeof(spellings) /
				    sizeof(spellings[0]))]);
			else
				rows[i] = integer_view((int64_t)
				    (next_random(&seed) % 9) - 4);
			split[i] = next_random(&seed) % (parts + 1);
		}
		shuffle(&seed, order, parts);

		for (f = 0; f < 2; f++) {
			bzero(&best, sizeof(best));
			for (i = 0; i < count; i++) {
				if (i == 0) {
					best = rows[i];
					continue;
				}
				order_to_best = model_order(&rows[i], &best);
				if ((fns[f] == RELIC_AGG_MIN &&
				    order_to_best < 0) ||
				    (fns[f] == RELIC_AGG_MAX &&
				    order_to_best > 0))
					best = rows[i];
			}

			for (n = 0; n < 2; n++) {
				fold_split(fns[f], rows, count, split, parts,
				    order, nestings[n], &whole);

				if (count == 0) {
					checkf(relic_fold_outcome(fns[f],
					    &whole.state, whole.rows,
					    &integer) == RELIC_FOLD_EMPTY,
					    SCENARIO_EXTREME, "trial %zu: no "
					    "rows should have no extreme",
					    trial);
					continue;
				}

				agree = relic_fold_outcome(fns[f],
				    &whole.state, whole.rows, &integer) ==
				    RELIC_FOLD_VALUE &&
				    whole.extreme.type == best.type &&
				    model_order(&whole.extreme, &best) == 0 &&
				    whole.invalidations == 0;
				checkf(agree, SCENARIO_EXTREME,
				    "trial %zu, nesting %zu: the %s of %zu %s "
				    "rows in %zu parts should be the model's",
				    trial, n,
				    fns[f] == RELIC_AGG_MIN ? "min" : "max",
				    count, text ? "symbol" : "integer", parts);
			}
		}
	}
}

/*
 * Rows of two types leave a min and a max with no value, whichever type
 * comes first and however the rows are split, and the group is reported
 * invalidated once.  A symbol and a string of the same spelling are two
 * types.  The first order's parts are combined left-nested and the
 * reversed order's right-nested.
 */
static void
mixed_types_leave_no_extreme(void)
{
	static const enum relic_aggregate_fn fns[2] = {
		RELIC_AGG_MIN, RELIC_AGG_MAX
	};
	static const size_t splits[4][3] = {
		{ 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 2 }
	};
	static const size_t orders[2][3] = {
		{ 0, 1, 2 }, { 2, 1, 0 }
	};
	relic_value_view_t rows[3];
	part_t whole;
	int64_t integer;
	size_t f;
	size_t at;
	size_t s;
	size_t o;
	size_t i;

	for (f = 0; f < 2; f++) {
		for (at = 0; at < 3; at++) {
			for (i = 0; i < 3; i++)
				rows[i] = i == at ?
				    text_view(RELIC_STRING, "a") :
				    text_view(RELIC_SYMBOL, "a");
			for (s = 0; s < 4; s++) {
				for (o = 0; o < 2; o++) {
					fold_split(fns[f], rows, 3, splits[s],
					    3, orders[o], o == 0 ? NEST_LEFT :
					    NEST_RIGHT, &whole);
					checkf(relic_fold_outcome(fns[f],
					    &whole.state, whole.rows,
					    &integer) == RELIC_FOLD_MIXED &&
					    whole.invalidations == 1,
					    SCENARIO_EXTREME,
					    "function %zu, string at %zu, "
					    "split %zu, order %zu should have "
					    "no extreme, reported once", f, at,
					    s, o);
				}
			}
		}
	}
}

/*
 * A value equal to the extreme ties rather than replacing it, whether it
 * is added or combined, so a caller can keep the earlier occurrence; a
 * better value replaces it.
 */
static void
equal_extremes_tie(void)
{
	relic_value_view_t three;
	relic_value_view_t two;
	part_t part;
	part_t more;

	three = integer_view(3);
	two = integer_view(2);

	part_init(&part);
	check(relic_fold_add(RELIC_AGG_MIN, &part.state, 0, &part.extreme,
	    &three) == RELIC_FOLD_REPLACED, SCENARIO_STEP,
	    "a min's first row should become its extreme");
	part.extreme = three;
	part.rows = 1;
	check(relic_fold_add(RELIC_AGG_MIN, &part.state, 1, &part.extreme,
	    &three) == RELIC_FOLD_TIED, SCENARIO_STEP,
	    "an equal row should tie with the extreme");
	check(relic_fold_add(RELIC_AGG_MAX, &part.state, 1, &part.extreme,
	    &two) == RELIC_FOLD_KEPT, SCENARIO_STEP,
	    "a lesser row should not replace a max");
	check(relic_fold_add(RELIC_AGG_MIN, &part.state, 1, &part.extreme,
	    &two) == RELIC_FOLD_REPLACED, SCENARIO_STEP,
	    "a lesser row should replace a min");

	part_init(&part);
	part_add(RELIC_AGG_MAX, &part, &three);
	part_init(&more);
	part_add(RELIC_AGG_MAX, &more, &three);
	check(relic_fold_combine(RELIC_AGG_MAX, &part.state, part.rows,
	    &part.extreme, &more.state, more.rows, &more.extreme) ==
	    RELIC_FOLD_TIED, SCENARIO_STEP,
	    "an equal extreme combined should tie");
}

int
main(void)
{
	counts_are_their_rows();
	empty_groups_have_their_outcome();
	invalidation_is_reported_once();
	maximum_plus_one_minus_one_fits_however_split();
	invalid_rows_come_before_overflow();
#ifdef __SIZEOF_INT128__
	random_sums_agree_with_the_model();
#endif
	random_extremes_agree_with_the_model();
	mixed_types_leave_no_extreme();
	equal_extremes_tie();

	return (check_summary("fold"));
}
