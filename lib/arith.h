/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_ARITH_H
#define RELIC_ARITH_H

#include <stdbool.h>
#include <stdint.h>

#include "ast.h"

/*
 * Integer arithmetic over int64_t, every operation checked before its result
 * exists (lib/arith.c).
 *
 * An operation's result is defined when it fits int64_t and, for division
 * and remainder, the divisor is not zero.  Division truncates toward zero and
 * the remainder takes the dividend's sign, a - (a / b) * b, so the remainder
 * is defined exactly when the quotient is: INT64_MIN / -1 and INT64_MIN mod -1
 * both overflow.  No undefined operation is ever executed -- no signed
 * overflow and no division the processor would trap on -- so a result that is
 * not defined is an answer the caller reports, and nothing a sanitizer could
 * find.
 */
enum relic_arith_status {
	RELIC_ARITH_DEFINED,
	RELIC_ARITH_OVERFLOW,
	RELIC_ARITH_ZERO_DIVISOR
};

enum relic_arith_status relic_arith_binary(enum relic_arith_op op,
	    int64_t left, int64_t right, int64_t *resultp);
enum relic_arith_status relic_arith_negate(int64_t operand, int64_t *resultp);

/*
 * An exact sum of int64_t values: a 128-bit two's complement integer held
 * in two 64-bit words, `hi` the high word.
 *
 * A sum of fewer than 2^32 addends, each within 2^63 in magnitude, is
 * within 2^95, so the accumulator never wraps and its total is exact
 * whatever order the addends come in.  Whether the total fits int64_t is
 * asked once, at the end, so a sum overflows exactly when the total over
 * the integers does not fit.  Every operation is on unsigned words, whose
 * wrapping is defined, so no signed overflow happens anywhere.
 *
 * A zeroed sum is zero.  relic_sum_add() adds one value, and
 * relic_sum_combine() adds the total of another sum, so that sums kept
 * over disjoint parts of the addends combine to the sum of them all; the
 * bound above is on the addends of all the parts together, so a combined
 * total is exact under it as an added one is.  relic_sum_fits() says
 * whether the total fits int64_t, which is when the high word is the sign
 * extension of the low one, and relic_sum_value() gives it when it does.
 */
typedef struct relic_sum relic_sum_t;

struct relic_sum {
	uint64_t	lo;
	uint64_t	hi;
};

void	 relic_sum_add(relic_sum_t *sum, int64_t addend);
void	 relic_sum_combine(relic_sum_t *sum, const relic_sum_t *more);
bool	 relic_sum_fits(const relic_sum_t *sum);
int64_t	 relic_sum_value(const relic_sum_t *sum);

#endif /* RELIC_ARITH_H */
