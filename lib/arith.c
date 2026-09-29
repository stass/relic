/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdint.h>

#include "arith.h"

/*
 * Checked int64_t arithmetic, as arith.h describes.
 *
 * Sums, differences and products use the compiler's checked builtins where
 * configure found them, which compile to the processor's overflow flag, and
 * otherwise compare the operands against the bounds before computing, which
 * gives the same answers.  A test build may define RELIC_ARITH_PORTABLE to
 * compile the comparisons where the builtins exist, so that both forms run
 * under the same tests.  Division and remainder need no builtin: their only
 * undefined cases are a zero divisor and INT64_MIN over -1, each tested for
 * before dividing.
 */

#if defined(HAVE_BUILTIN_OVERFLOW) && !defined(RELIC_ARITH_PORTABLE)
#define	RELIC_ARITH_BUILTINS	1
#endif

static enum relic_arith_status add(int64_t left, int64_t right,
		    int64_t *resultp);
static enum relic_arith_status divide(int64_t left, int64_t right,
		    int64_t *resultp);
static enum relic_arith_status multiply(int64_t left, int64_t right,
		    int64_t *resultp);
static enum relic_arith_status remainder_of(int64_t left, int64_t right,
		    int64_t *resultp);
static enum relic_arith_status subtract(int64_t left, int64_t right,
		    int64_t *resultp);

static enum relic_arith_status
add(int64_t left, int64_t right, int64_t *resultp)
{
#ifdef RELIC_ARITH_BUILTINS
	if (__builtin_add_overflow(left, right, resultp))
		return (RELIC_ARITH_OVERFLOW);
#else
	if ((right > 0 && left > INT64_MAX - right) ||
	    (right < 0 && left < INT64_MIN - right))
		return (RELIC_ARITH_OVERFLOW);
	*resultp = left + right;
#endif

	return (RELIC_ARITH_DEFINED);
}

static enum relic_arith_status
subtract(int64_t left, int64_t right, int64_t *resultp)
{
#ifdef RELIC_ARITH_BUILTINS
	if (__builtin_sub_overflow(left, right, resultp))
		return (RELIC_ARITH_OVERFLOW);
#else
	if ((right < 0 && left > INT64_MAX + right) ||
	    (right > 0 && left < INT64_MIN + right))
		return (RELIC_ARITH_OVERFLOW);
	*resultp = left - right;
#endif

	return (RELIC_ARITH_DEFINED);
}

/*
 * Without the builtin, a product is checked by dividing the bound it could
 * pass by one factor.  Division truncates toward zero, which rounds the
 * quotient of a bound toward the other factor's permitted side, so the
 * comparison is exact: for positive factors a * b > MAX exactly when
 * a > MAX / b, a product of mixed signs falls below MIN exactly when the
 * negative factor is below MIN over the positive one, and two negative
 * factors pass MAX exactly when a < MAX / b.  The signs are tested first so
 * that every bound is divided by a factor that keeps the division defined:
 * none of them is INT64_MIN / -1.
 */
static enum relic_arith_status
multiply(int64_t left, int64_t right, int64_t *resultp)
{
#ifdef RELIC_ARITH_BUILTINS
	if (__builtin_mul_overflow(left, right, resultp))
		return (RELIC_ARITH_OVERFLOW);
#else
	if (left > 0) {
		if (right > 0) {
			if (left > INT64_MAX / right)
				return (RELIC_ARITH_OVERFLOW);
		} else if (right < INT64_MIN / left)
			return (RELIC_ARITH_OVERFLOW);
	} else if (left < 0) {
		if (right > 0) {
			if (left < INT64_MIN / right)
				return (RELIC_ARITH_OVERFLOW);
		} else if (right < 0 && left < INT64_MAX / right)
			return (RELIC_ARITH_OVERFLOW);
	}
	*resultp = left * right;
#endif

	return (RELIC_ARITH_DEFINED);
}

static enum relic_arith_status
divide(int64_t left, int64_t right, int64_t *resultp)
{
	if (right == 0)
		return (RELIC_ARITH_ZERO_DIVISOR);
	if (left == INT64_MIN && right == -1)
		return (RELIC_ARITH_OVERFLOW);

	*resultp = left / right;

	return (RELIC_ARITH_DEFINED);
}

static enum relic_arith_status
remainder_of(int64_t left, int64_t right, int64_t *resultp)
{
	if (right == 0)
		return (RELIC_ARITH_ZERO_DIVISOR);
	if (left == INT64_MIN && right == -1)
		return (RELIC_ARITH_OVERFLOW);

	*resultp = left % right;

	return (RELIC_ARITH_DEFINED);
}

enum relic_arith_status
relic_arith_binary(enum relic_arith_op op, int64_t left, int64_t right,
    int64_t *resultp)
{
	switch (op) {
	case RELIC_ARITH_ADD:
		return (add(left, right, resultp));
	case RELIC_ARITH_SUBTRACT:
		return (subtract(left, right, resultp));
	case RELIC_ARITH_MULTIPLY:
		return (multiply(left, right, resultp));
	case RELIC_ARITH_DIVIDE:
		return (divide(left, right, resultp));
	case RELIC_ARITH_REMAINDER:
		return (remainder_of(left, right, resultp));
	}

	return (RELIC_ARITH_OVERFLOW);
}

enum relic_arith_status
relic_arith_negate(int64_t operand, int64_t *resultp)
{
	if (operand == INT64_MIN)
		return (RELIC_ARITH_OVERFLOW);

	*resultp = -operand;

	return (RELIC_ARITH_DEFINED);
}
