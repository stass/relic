/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for checked integer arithmetic (lib/arith.c).
 *
 * Every operator is tried at and around both extremes of int64_t, where a
 * result either is the exact one or is reported as overflowing, and division
 * and remainder at every combination of signs and at a zero divisor.  The
 * exact sum is driven with each extreme in each order and read back, and
 * checked near the bound its accumulator is sized for.  The suite is built
 * twice, once over the compiler's overflow builtins and once
 * over the portable comparisons, and must pass both; under
 * UndefinedBehaviorSanitizer neither may execute a signed overflow.
 */

#include "config.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arith.h"
#include "harness.h"

#define	SCENARIO_EXTREMES	"an operation at an extreme is exact or reported"
#define	SCENARIO_SIGNS		"a quotient truncates, a remainder takes its sign"
#define	SCENARIO_SUM		"a sum is exact in any order of its addends"

/*
 * Addends driven through a sum in each order.  A drive of 2^20 addends of
 * each extreme reaches totals past 2^83 in magnitude; the states near the
 * accumulator's 2^95 bound are built directly and checked one addition at a
 * time, since 2^32 additions per order would take the suite minutes under a
 * sanitizer.
 */
#define	DRIVE_ADDENDS		(UINT64_C(1) << 20)
#define	BOUND_ADDENDS		(UINT64_C(1) << 32)

#ifdef __SIZEOF_INT128__
typedef __int128 wide_t;
#endif

typedef struct arith_case arith_case_t;

/*
 * One operation and its expected outcome: the status, and the result when
 * the status is RELIC_ARITH_DEFINED.
 */
struct arith_case {
	enum relic_arith_op		op;
	int64_t				left;
	int64_t				right;
	enum relic_arith_status		status;
	int64_t				result;
};

static const char *op_spelling(enum relic_arith_op op);
static void	 run_cases(const arith_case_t *cases, size_t count,
		    const char *scenario);
static void	 negation_overflows_only_at_the_minimum(void);
static void	 operations_at_the_extremes_are_exact_or_reported(void);
static void	 quotients_truncate_and_remainders_follow_the_dividend(void);
static int64_t	 sum_of(const int64_t *addends, size_t count, bool *fitsp);
static void	 sums_are_exact_in_every_order(void);
static void	 sums_driven_by_the_extremes_come_back(void);
#ifdef __SIZEOF_INT128__
static relic_sum_t sum_holding(wide_t total);
static wide_t	 sum_total(const relic_sum_t *sum);
static void	 sums_near_the_bound_are_exact(void);
#endif

static const char *
op_spelling(enum relic_arith_op op)
{
	switch (op) {
	case RELIC_ARITH_ADD:
		return ("+");
	case RELIC_ARITH_SUBTRACT:
		return ("-");
	case RELIC_ARITH_MULTIPLY:
		return ("*");
	case RELIC_ARITH_DIVIDE:
		return ("/");
	case RELIC_ARITH_REMAINDER:
		break;
	}

	return ("mod");
}

/*
 * Run each case, checking its status, and its result when it has one.
 */
static void
run_cases(const arith_case_t *cases, size_t count, const char *scenario)
{
	enum relic_arith_status status;
	int64_t result;
	size_t i;

	for (i = 0; i < count; i++) {
		result = 0;
		status = relic_arith_binary(cases[i].op, cases[i].left,
		    cases[i].right, &result);

		checkf(status == cases[i].status &&
		    (status != RELIC_ARITH_DEFINED ||
		    result == cases[i].result), scenario,
		    "%" PRId64 " %s %" PRId64 " should give status %d and %"
		    PRId64 ", not status %d and %" PRId64, cases[i].left,
		    op_spelling(cases[i].op), cases[i].right,
		    (int)cases[i].status, cases[i].result, (int)status, result);
	}
}

/*
 * Each operator at both ends of the range: the nearest operations that fit
 * give their exact results, and the first that does not is reported, never
 * wrapped.
 */
static void
operations_at_the_extremes_are_exact_or_reported(void)
{
	static const arith_case_t cases[] = {
		{ RELIC_ARITH_ADD, INT64_MAX, 1, RELIC_ARITH_OVERFLOW, 0 },
		{ RELIC_ARITH_ADD, INT64_MAX, 0, RELIC_ARITH_DEFINED,
		    INT64_MAX },
		{ RELIC_ARITH_ADD, INT64_MAX - 1, 1, RELIC_ARITH_DEFINED,
		    INT64_MAX },
		{ RELIC_ARITH_ADD, INT64_MIN, -1, RELIC_ARITH_OVERFLOW, 0 },
		{ RELIC_ARITH_ADD, INT64_MIN + 1, -1, RELIC_ARITH_DEFINED,
		    INT64_MIN },
		{ RELIC_ARITH_ADD, INT64_MAX, INT64_MIN, RELIC_ARITH_DEFINED,
		    -1 },
		{ RELIC_ARITH_ADD, INT64_MAX, INT64_MAX, RELIC_ARITH_OVERFLOW,
		    0 },
		{ RELIC_ARITH_ADD, INT64_MIN, INT64_MIN, RELIC_ARITH_OVERFLOW,
		    0 },
		{ RELIC_ARITH_SUBTRACT, INT64_MIN, 1, RELIC_ARITH_OVERFLOW, 0 },
		{ RELIC_ARITH_SUBTRACT, INT64_MIN + 1, 1, RELIC_ARITH_DEFINED,
		    INT64_MIN },
		{ RELIC_ARITH_SUBTRACT, INT64_MAX, -1, RELIC_ARITH_OVERFLOW,
		    0 },
		{ RELIC_ARITH_SUBTRACT, INT64_MAX - 1, -1, RELIC_ARITH_DEFINED,
		    INT64_MAX },
		{ RELIC_ARITH_SUBTRACT, 0, INT64_MIN, RELIC_ARITH_OVERFLOW, 0 },
		{ RELIC_ARITH_SUBTRACT, -1, INT64_MIN, RELIC_ARITH_DEFINED,
		    INT64_MAX },
		{ RELIC_ARITH_SUBTRACT, INT64_MIN, INT64_MIN,
		    RELIC_ARITH_DEFINED, 0 },
		{ RELIC_ARITH_MULTIPLY, INT64_MAX, 2, RELIC_ARITH_OVERFLOW, 0 },
		{ RELIC_ARITH_MULTIPLY, INT64_MAX / 2, 2, RELIC_ARITH_DEFINED,
		    INT64_MAX - 1 },
		{ RELIC_ARITH_MULTIPLY, INT64_MIN / 2, 2, RELIC_ARITH_DEFINED,
		    INT64_MIN },
		{ RELIC_ARITH_MULTIPLY, INT64_MIN / 2 - 1, 2,
		    RELIC_ARITH_OVERFLOW, 0 },
		{ RELIC_ARITH_MULTIPLY, 2, INT64_MIN / 2, RELIC_ARITH_DEFINED,
		    INT64_MIN },
		{ RELIC_ARITH_MULTIPLY, -2, INT64_MIN / 2, RELIC_ARITH_OVERFLOW,
		    0 },
		{ RELIC_ARITH_MULTIPLY, INT64_MIN, -1, RELIC_ARITH_OVERFLOW,
		    0 },
		{ RELIC_ARITH_MULTIPLY, -1, INT64_MIN, RELIC_ARITH_OVERFLOW,
		    0 },
		{ RELIC_ARITH_MULTIPLY, INT64_MAX, -1, RELIC_ARITH_DEFINED,
		    -INT64_MAX },
		{ RELIC_ARITH_MULTIPLY, INT64_MIN, 1, RELIC_ARITH_DEFINED,
		    INT64_MIN },
		{ RELIC_ARITH_MULTIPLY, INT64_MIN, 0, RELIC_ARITH_DEFINED, 0 },
		{ RELIC_ARITH_MULTIPLY, -3037000500, -3037000500,
		    RELIC_ARITH_OVERFLOW, 0 },
		{ RELIC_ARITH_MULTIPLY, -3037000499, -3037000499,
		    RELIC_ARITH_DEFINED, INT64_C(9223372030926249001) },
		{ RELIC_ARITH_MULTIPLY, 3037000500, -3037000500,
		    RELIC_ARITH_OVERFLOW, 0 },
		{ RELIC_ARITH_MULTIPLY, 3037000499, -3037000499,
		    RELIC_ARITH_DEFINED, INT64_C(-9223372030926249001) },
		{ RELIC_ARITH_DIVIDE, INT64_MIN, -1, RELIC_ARITH_OVERFLOW, 0 },
		{ RELIC_ARITH_DIVIDE, INT64_MIN, 1, RELIC_ARITH_DEFINED,
		    INT64_MIN },
		{ RELIC_ARITH_DIVIDE, INT64_MIN + 1, -1, RELIC_ARITH_DEFINED,
		    INT64_MAX },
		{ RELIC_ARITH_DIVIDE, INT64_MAX, -1, RELIC_ARITH_DEFINED,
		    -INT64_MAX },
		{ RELIC_ARITH_DIVIDE, 1, 0, RELIC_ARITH_ZERO_DIVISOR, 0 },
		{ RELIC_ARITH_DIVIDE, 0, 0, RELIC_ARITH_ZERO_DIVISOR, 0 },
		{ RELIC_ARITH_DIVIDE, INT64_MIN, 0, RELIC_ARITH_ZERO_DIVISOR,
		    0 },
		{ RELIC_ARITH_REMAINDER, INT64_MIN, -1, RELIC_ARITH_OVERFLOW,
		    0 },
		{ RELIC_ARITH_REMAINDER, INT64_MIN + 1, -1,
		    RELIC_ARITH_DEFINED, 0 },
		{ RELIC_ARITH_REMAINDER, INT64_MIN, INT64_MAX,
		    RELIC_ARITH_DEFINED, -1 },
		{ RELIC_ARITH_REMAINDER, INT64_MAX, INT64_MIN,
		    RELIC_ARITH_DEFINED, INT64_MAX },
		{ RELIC_ARITH_REMAINDER, 5, 0, RELIC_ARITH_ZERO_DIVISOR, 0 }
	};

	run_cases(cases, sizeof(cases) / sizeof(cases[0]), SCENARIO_EXTREMES);
}

/*
 * The quotient truncates toward zero and the remainder carries the
 * dividend's sign, so that a = (a / b) * b + a mod b, for every combination
 * of signs.
 */
static void
quotients_truncate_and_remainders_follow_the_dividend(void)
{
	static const arith_case_t cases[] = {
		{ RELIC_ARITH_DIVIDE, 7, 2, RELIC_ARITH_DEFINED, 3 },
		{ RELIC_ARITH_DIVIDE, -7, 2, RELIC_ARITH_DEFINED, -3 },
		{ RELIC_ARITH_DIVIDE, 7, -2, RELIC_ARITH_DEFINED, -3 },
		{ RELIC_ARITH_DIVIDE, -7, -2, RELIC_ARITH_DEFINED, 3 },
		{ RELIC_ARITH_REMAINDER, 7, 2, RELIC_ARITH_DEFINED, 1 },
		{ RELIC_ARITH_REMAINDER, -7, 2, RELIC_ARITH_DEFINED, -1 },
		{ RELIC_ARITH_REMAINDER, 7, -2, RELIC_ARITH_DEFINED, 1 },
		{ RELIC_ARITH_REMAINDER, -7, -2, RELIC_ARITH_DEFINED, -1 },
		{ RELIC_ARITH_DIVIDE, 0, -5, RELIC_ARITH_DEFINED, 0 },
		{ RELIC_ARITH_REMAINDER, 0, -5, RELIC_ARITH_DEFINED, 0 },
		{ RELIC_ARITH_REMAINDER, 6, 3, RELIC_ARITH_DEFINED, 0 },
		{ RELIC_ARITH_REMAINDER, -6, 3, RELIC_ARITH_DEFINED, 0 }
	};

	run_cases(cases, sizeof(cases) / sizeof(cases[0]), SCENARIO_SIGNS);
}

static void
negation_overflows_only_at_the_minimum(void)
{
	int64_t result;

	check(relic_arith_negate(INT64_MIN, &result) == RELIC_ARITH_OVERFLOW,
	    SCENARIO_EXTREMES, "-INT64_MIN should overflow");
	check(relic_arith_negate(INT64_MIN + 1, &result) ==
	    RELIC_ARITH_DEFINED && result == INT64_MAX, SCENARIO_EXTREMES,
	    "-(INT64_MIN + 1) should be INT64_MAX");
	check(relic_arith_negate(INT64_MAX, &result) == RELIC_ARITH_DEFINED &&
	    result == -INT64_MAX, SCENARIO_EXTREMES,
	    "-INT64_MAX should be exact");
	check(relic_arith_negate(0, &result) == RELIC_ARITH_DEFINED &&
	    result == 0, SCENARIO_EXTREMES, "-0 should be 0");
}

/*
 * The total of a list of addends, added in the order given, and whether it
 * fits int64_t.
 */
static int64_t
sum_of(const int64_t *addends, size_t count, bool *fitsp)
{
	relic_sum_t sum;
	size_t i;

	sum.lo = 0;
	sum.hi = 0;
	for (i = 0; i < count; i++)
		relic_sum_add(&sum, addends[i]);

	*fitsp = relic_sum_fits(&sum);

	return (*fitsp ? relic_sum_value(&sum) : 0);
}

/*
 * Each case is three addends, padded with zeros, and its expected verdict;
 * every one of their six orders must give that verdict and, when the total
 * fits, that total, including the orders whose running total leaves
 * int64_t on the way.
 */
static void
sums_are_exact_in_every_order(void)
{
	static const struct {
		int64_t	addends[3];
		bool	fits;
		int64_t	total;
	} cases[] = {
		{ { 0, 0, 0 }, true, 0 },
		{ { INT64_MAX, 0, 0 }, true, INT64_MAX },
		{ { INT64_MIN, 0, 0 }, true, INT64_MIN },
		{ { INT64_MAX, 1, -1 }, true, INT64_MAX },
		{ { INT64_MIN, -1, 1 }, true, INT64_MIN },
		{ { INT64_MAX, INT64_MIN, -1 }, true, -2 },
		{ { INT64_MAX, INT64_MAX, INT64_MIN }, true, INT64_MAX - 1 },
		{ { INT64_MIN, INT64_MIN, INT64_MAX }, false, 0 },
		{ { INT64_MIN + 1, INT64_MIN, INT64_MAX }, true, INT64_MIN },
		{ { INT64_MAX, 1, 0 }, false, 0 },
		{ { INT64_MIN, -1, 0 }, false, 0 },
		{ { INT64_MAX, INT64_MAX, 2 }, false, 0 },
		{ { INT64_MIN, INT64_MIN, 0 }, false, 0 }
	};
	static const size_t orders[6][3] = {
		{ 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 },
		{ 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 }
	};
	int64_t permuted[3];
	int64_t total;
	size_t c;
	size_t o;
	size_t i;
	bool fits;

	for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		for (o = 0; o < 6; o++) {
			for (i = 0; i < 3; i++)
				permuted[i] = cases[c].addends[orders[o][i]];

			total = sum_of(permuted, 3, &fits);
			checkf(fits == cases[c].fits && total ==
			    cases[c].total, SCENARIO_SUM,
			    "case %zu in order %zu should give %s %" PRId64
			    ", gave %s %" PRId64, c, o,
			    cases[c].fits ? "the total" : "no total",
			    cases[c].total, fits ? "the total" : "no total",
			    total);
		}
	}
}

/*
 * 2^20 addends of each extreme, one extreme's run before the other's in
 * both orders and the two interleaved, total -2^20: the running total
 * passes 2^82 in magnitude in the first two, and must come back exactly.
 * A run of one extreme alone never fits.
 */
static void
sums_driven_by_the_extremes_come_back(void)
{
	static const char *const orders[] = {
		"maxima first", "minima first", "interleaved"
	};
	relic_sum_t sum;
	uint64_t i;
	size_t o;

	for (o = 0; o < sizeof(orders) / sizeof(orders[0]); o++) {
		sum.lo = 0;
		sum.hi = 0;

		for (i = 0; i < 2 * DRIVE_ADDENDS; i++) {
			if (o == 0)
				relic_sum_add(&sum, i < DRIVE_ADDENDS ?
				    INT64_MAX : INT64_MIN);
			else if (o == 1)
				relic_sum_add(&sum, i < DRIVE_ADDENDS ?
				    INT64_MIN : INT64_MAX);
			else
				relic_sum_add(&sum, i % 2 == 0 ?
				    INT64_MAX : INT64_MIN);

			if (i == DRIVE_ADDENDS - 1 && o < 2)
				checkf(!relic_sum_fits(&sum), SCENARIO_SUM,
				    "%s: a run of one extreme should not fit",
				    orders[o]);
		}

		checkf(relic_sum_fits(&sum) && relic_sum_value(&sum) ==
		    -(int64_t)DRIVE_ADDENDS, SCENARIO_SUM,
		    "%s: the total should be -2^20", orders[o]);
	}
}

#ifdef __SIZEOF_INT128__
/*
 * A sum whose accumulator holds `total`, built from its two's complement
 * words, and the total an accumulator holds.  Both are the oracle's side of
 * the comparison, computed in the compiler's 128-bit integers rather than
 * through the accumulator under test.
 */
static relic_sum_t
sum_holding(wide_t total)
{
	relic_sum_t sum;

	sum.lo = (uint64_t)total;
	sum.hi = (uint64_t)((unsigned __int128)total >> 64);

	return (sum);
}

static wide_t
sum_total(const relic_sum_t *sum)
{
	return ((wide_t)(((unsigned __int128)sum->hi << 64) | sum->lo));
}

/*
 * The largest totals fewer than 2^32 addends can reach: 2^32 - 1 maxima,
 * 2^32 - 1 minima, and states between, each given one more addend of
 * either extreme and of either sign near zero.  Each addition must agree
 * with the oracle's, and whether the result fits must be whether the
 * oracle's total lies within int64_t.
 */
static void
sums_near_the_bound_are_exact(void)
{
	static const int64_t addends[] = {
		INT64_MAX, INT64_MIN, 1, -1, 0
	};
	wide_t states[8];
	wide_t expected;
	relic_sum_t sum;
	size_t s;
	size_t a;
	bool fits;

	states[0] = (wide_t)(BOUND_ADDENDS - 1) * INT64_MAX;
	states[1] = (wide_t)(BOUND_ADDENDS - 1) * INT64_MIN;
	states[2] = (wide_t)(BOUND_ADDENDS / 2) * INT64_MAX;
	states[3] = (wide_t)(BOUND_ADDENDS / 2) * INT64_MIN;
	states[4] = (wide_t)INT64_MAX;
	states[5] = (wide_t)INT64_MIN;
	states[6] = (wide_t)INT64_MAX + 1;
	states[7] = (wide_t)INT64_MIN - 1;

	for (s = 0; s < sizeof(states) / sizeof(states[0]); s++) {
		for (a = 0; a < sizeof(addends) / sizeof(addends[0]); a++) {
			sum = sum_holding(states[s]);
			relic_sum_add(&sum, addends[a]);
			expected = states[s] + addends[a];
			fits = expected >= INT64_MIN && expected <= INT64_MAX;

			checkf(sum_total(&sum) == expected, SCENARIO_SUM,
			    "state %zu plus addend %zu should be exact", s, a);
			checkf(relic_sum_fits(&sum) == fits, SCENARIO_SUM,
			    "state %zu plus addend %zu should %s", s, a,
			    fits ? "fit" : "not fit");
			if (fits)
				checkf(relic_sum_value(&sum) ==
				    (int64_t)expected, SCENARIO_SUM,
				    "state %zu plus addend %zu should read "
				    "back", s, a);
		}
	}
}
#endif

int
main(void)
{
	operations_at_the_extremes_are_exact_or_reported();
	quotients_truncate_and_remainders_follow_the_dividend();
	negation_overflows_only_at_the_minimum();
	sums_are_exact_in_every_order();
	sums_driven_by_the_extremes_come_back();
#ifdef __SIZEOF_INT128__
	sums_near_the_bound_are_exact();
#endif

	return (check_summary("arith"));
}
