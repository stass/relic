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
 * suite is built twice, once over the compiler's overflow builtins and once
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

int
main(void)
{
	operations_at_the_extremes_are_exact_or_reported();
	quotients_truncate_and_remainders_follow_the_dividend();
	negation_overflows_only_at_the_minimum();

	return (check_summary("arith"));
}
