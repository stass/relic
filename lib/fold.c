/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdint.h>
#include <strings.h>

#include "fold.h"

/*
 * The aggregate functions' accumulators, as fold.h describes them.
 *
 * Every operation reads value views and writes only its accumulator.
 * Stored-row folds share the same arithmetic and type-outcome rules.
 */

static enum relic_fold_step choose_extreme(enum relic_aggregate_fn fn,
		    const relic_value_view_t *extreme,
		    const relic_value_view_t *candidate);

void
relic_fold_init(relic_fold_state_t *state)
{
	bzero(state, sizeof(*state));
}

/*
 * Whether `candidate`, of the extreme's type, takes the extreme's place:
 * below it for a min, above it for a max, or equal to it.
 */
static enum relic_fold_step
choose_extreme(enum relic_aggregate_fn fn, const relic_value_view_t *extreme,
    const relic_value_view_t *candidate)
{
	switch (relic_value_view_order(candidate, extreme)) {
	case RELIC_ORDER_EQUAL:
		return (RELIC_FOLD_KEPT);
	case RELIC_ORDER_LESS:
		return (fn == RELIC_AGG_MIN ? RELIC_FOLD_REPLACED :
		    RELIC_FOLD_KEPT);
	case RELIC_ORDER_GREATER:
		return (fn == RELIC_AGG_MAX ? RELIC_FOLD_REPLACED :
		    RELIC_FOLD_KEPT);
	case RELIC_ORDER_UNORDERED:
		break;
	}

	return (RELIC_FOLD_KEPT);
}

/*
 * A min's or a max's first row fixes the type every later row must have,
 * and is the extreme until a better one comes.
 */
enum relic_fold_step
relic_fold_add(enum relic_aggregate_fn fn, relic_fold_state_t *state,
    uint64_t rows, const relic_value_view_t *extreme,
    const relic_value_view_t *value)
{
	switch (fn) {
	case RELIC_AGG_COUNT:
		return (RELIC_FOLD_KEPT);
	case RELIC_AGG_SUM:
		if (state->invalid)
			return (RELIC_FOLD_KEPT);
		if (value->type != RELIC_INTEGER) {
			state->invalid = true;
			return (RELIC_FOLD_INVALIDATED);
		}
		relic_sum_add(&state->sum, value->integer);
		return (RELIC_FOLD_KEPT);
	case RELIC_AGG_MIN:
	case RELIC_AGG_MAX:
		break;
	}

	if (rows == 0) {
		state->type = value->type;
		return (RELIC_FOLD_REPLACED);
	}

	if (state->invalid)
		return (RELIC_FOLD_KEPT);

	if (value->type != state->type) {
		state->invalid = true;
		return (RELIC_FOLD_INVALIDATED);
	}

	return (choose_extreme(fn, extreme, value));
}

/*
 * A group has fewer than 2^32 rows, a relation's identifier bound, so a
 * count fits int64_t.
 */
enum relic_fold_outcome
relic_fold_outcome(enum relic_aggregate_fn fn, const relic_fold_state_t *state,
    uint64_t rows, int64_t *integerp)
{
	switch (fn) {
	case RELIC_AGG_COUNT:
		*integerp = (int64_t)rows;
		return (RELIC_FOLD_VALUE);
	case RELIC_AGG_SUM:
		if (state->invalid)
			return (RELIC_FOLD_MIXED);
		if (!relic_sum_fits(&state->sum))
			return (RELIC_FOLD_OVERFLOW);
		*integerp = relic_sum_value(&state->sum);
		return (RELIC_FOLD_VALUE);
	case RELIC_AGG_MIN:
	case RELIC_AGG_MAX:
		break;
	}

	if (rows == 0)
		return (RELIC_FOLD_EMPTY);

	return (state->invalid ? RELIC_FOLD_MIXED : RELIC_FOLD_VALUE);
}
