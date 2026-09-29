/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_ARITH_H
#define RELIC_ARITH_H

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

#endif /* RELIC_ARITH_H */
