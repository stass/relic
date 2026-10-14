/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_FOLD_H
#define RELIC_FOLD_H

#include <stdbool.h>
#include <stdint.h>

#include <relic.h>

#include "arith.h"
#include "ast.h"
#include "frozen.h"

/*
 * What the four aggregate functions make of a group's rows, as operations
 * on an accumulator (lib/fold.c).
 *
 * A fold walks a stored group's rows into an accumulator. Its outcome --
 * a value, no value, a mixed-type note or an overflow -- is decided here.
 * The caller keeps the row count and the identifier of a min's or max's
 * extreme. It supplies the count before each row and a borrowed view of
 * that extreme. A step that replaces the extreme says so; the caller
 * replaces its identifier. Nothing here allocates or writes the context.
 *
 * The state follows the function:
 *
 *	count	nothing: the row count is the value
 *	sum	the exact total of the rows (arith.h), and whether a row was
 *		not an integer, which leaves the sum with no value
 *	min,max	the type of the first row, and whether a row was of another
 *		type, which leaves the extreme with no value
 *
 * Once a row has left a group with no value, more rows do not change
 * that, and an invalid group is still a group: its row count, and every
 * other function over it, are unaffected.
 *
 * relic_fold_add() folds one row's value, which a count does not read and
 * may be NULL for it. It reports RELIC_FOLD_INVALIDATED once, on the first
 * row outside the function's domain, RELIC_FOLD_REPLACED when the row
 * becomes the extreme, and RELIC_FOLD_KEPT otherwise. The caller counts
 * the rows itself.
 *
 * relic_fold_outcome() is what the group gives once every row is folded:
 *
 *	RELIC_FOLD_VALUE	a count or sum's integer, in *integerp, or the
 *				extreme the caller holds for a min or a max
 *	RELIC_FOLD_EMPTY	a min or a max over no row, which has no value
 *				and draws no note; a count or a sum over none
 *				is zero
 *	RELIC_FOLD_MIXED	no value, because of the rows' types, which the
 *				aggregate's note describes
 *	RELIC_FOLD_OVERFLOW	a sum whose exact total does not fit int64_t
 *
 * A sum's invalidity comes before its range: a group holding a row that is
 * not an integer has no sum, however far its integers' total lies outside
 * int64_t.  The outcome is asked only when a consumer reads the group, so
 * a group nothing reads never overflows.
 */
typedef struct relic_fold_state relic_fold_state_t;

struct relic_fold_state {
	relic_sum_t	 sum;		/* RELIC_AGG_SUM */
	enum relic_type	 type;		/* MIN, MAX: every row's, after one */
	bool		 invalid;
};

enum relic_fold_step {
	RELIC_FOLD_KEPT,
	RELIC_FOLD_REPLACED,
	RELIC_FOLD_INVALIDATED
};

enum relic_fold_outcome {
	RELIC_FOLD_VALUE,
	RELIC_FOLD_EMPTY,
	RELIC_FOLD_MIXED,
	RELIC_FOLD_OVERFLOW
};

void	 relic_fold_init(relic_fold_state_t *state);
enum relic_fold_step relic_fold_add(enum relic_aggregate_fn fn,
	    relic_fold_state_t *state, uint64_t rows,
	    const relic_value_view_t *extreme, const relic_value_view_t *value);
enum relic_fold_outcome relic_fold_outcome(enum relic_aggregate_fn fn,
	    const relic_fold_state_t *state, uint64_t rows, int64_t *integerp);

#endif /* RELIC_FOLD_H */
