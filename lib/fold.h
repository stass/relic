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
 * A fold over a stored group walks the group's rows into one accumulator.
 * A reduction keeps an accumulator per group and adds each row as it
 * arrives, and can combine two accumulators kept over disjoint parts of
 * one group's rows.  Both go through these operations, so a group means
 * one thing however its rows were reached, and the outcome -- a value, no
 * value, a note about the rows' types, an overflow -- is decided in one
 * place.
 *
 * An accumulator is one function's state over a group, apart from two
 * things the caller keeps.  The group's row count is one, shared by every
 * function folded over the group, so each call is given `rows`, the rows
 * the accumulator has seen before it.  A min's or a max's extreme value is
 * held by the caller, as an identifier when a fold walks stored rows or as
 * an owned copy when a reduction retains one, so each call is given a view
 * of it (frozen.h), read only when the accumulator has seen a row and is
 * still valid; a call that makes another value the extreme says so, and
 * the caller replaces what it holds.  Nothing here allocates, reports or
 * writes the context.
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
 * may be NULL for it.  relic_fold_combine() folds the state `more` kept
 * over `more_rows` other rows, with its extreme `more_extreme`, into
 * `state`.  Both say what the step did: RELIC_FOLD_INVALIDATED once, at
 * the row or the combination that first left the group with no value;
 * RELIC_FOLD_REPLACED when the extreme is now the value added or the other
 * extreme; RELIC_FOLD_TIED when that value equals the extreme, for a
 * caller that keeps the earlier of two equal extremes' occurrences; and
 * RELIC_FOLD_KEPT otherwise.  The caller counts the rows itself.
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
	RELIC_FOLD_TIED,
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
enum relic_fold_step relic_fold_combine(enum relic_aggregate_fn fn,
	    relic_fold_state_t *state, uint64_t rows,
	    const relic_value_view_t *extreme, const relic_fold_state_t *more,
	    uint64_t more_rows, const relic_value_view_t *more_extreme);
enum relic_fold_outcome relic_fold_outcome(enum relic_aggregate_fn fn,
	    const relic_fold_state_t *state, uint64_t rows, int64_t *integerp);

#endif /* RELIC_FOLD_H */
