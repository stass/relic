/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * The estimator reads the store only as frozen.h presents it, as the
 * executor does, so a round's choice of plan cannot write what the plans
 * then read.
 */
#define	RELIC_FROZEN_ONLY

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include "cost.h"

/*
 * The work estimator: a walk of a recipe's steps in plan order, carrying B,
 * the bindings estimated to reach the step, from one, and W, the work
 * accumulated, from zero (plan.h gives each step's rule).  The walk stops
 * once no binding reaches the next step, since nothing after it runs, or
 * once W has saturated, since nothing after it can lower W.
 *
 * Every quantity is an unsigned 64-bit count and every sum and product
 * saturates at UINT64_MAX rather than wrapping, so a plan whose work does
 * not fit is estimated as the most work there is and is never chosen over
 * one whose work does.  A view's size and an index's keys are tuple counts
 * and so fit 32 bits, which is what keeps a lookup's division exact without
 * a wider type (relic_cost_lookup_visits()).
 */

_Static_assert(sizeof(relic_tuple_id_t) <= sizeof(uint32_t),
    "a lookup multiplies a remainder below 2^32 by a count below 2^32");
_Static_assert((relic_tuple_id_t)-1 > 0,
    "tuple counts are unsigned");

static relic_tuple_id_t	 index_keys(relic_cost_catalog_t *catalog,
			    const relic_cost_step_t *step);
static relic_tuple_id_t	 range_length(relic_tuple_range_t range);
static uint64_t		 saturating_add(uint64_t left, uint64_t right);
static uint64_t		 saturating_multiply(uint64_t left, uint64_t right);

int
relic_cost_statistic_compare(const void *left, const void *right)
{
	const relic_cost_statistic_t *left_entry;
	const relic_cost_statistic_t *right_entry;
	relic_pred_id_t left_pred;
	relic_pred_id_t right_pred;

	left_entry = left;
	right_entry = right;
	left_pred = relic_relation_pred(left_entry->relation);
	right_pred = relic_relation_pred(right_entry->relation);

	if (left_pred != right_pred)
		return (left_pred < right_pred ? -1 : 1);
	if (left_entry->index_position != right_entry->index_position)
		return (left_entry->index_position <
		    right_entry->index_position ? -1 : 1);
	if (left_entry->relation != right_entry->relation)
		return ((uintptr_t)left_entry->relation <
		    (uintptr_t)right_entry->relation ? -1 : 1);

	return (0);
}

bool
relic_cost_step_index(const relic_cost_step_t *step,
    const relic_relation_t **relationp, uint32_t *positionp)
{
	const relic_op_t *op;

	op = step->op;
	if (step->kind == RELIC_COST_LOOKUP) {
		*relationp = op->u.atom.relation;
		*positionp = op->u.atom.index_position;
		return (true);
	}
	if (step->kind == RELIC_COST_FOLD_LOOKUP) {
		*relationp = op->u.aggregate.relation;
		*positionp = op->u.aggregate.index_position;
		return (true);
	}

	return (false);
}

size_t
relic_cost_choose(const relic_plan_family_t *family,
    const relic_exec_env_t *env, relic_cost_catalog_t *catalog)
{
	const relic_plan_candidate_t *candidates;
	uint64_t delta_first;
	uint64_t input_first;

	assert(family->count >= 1 && family->count <= RELIC_FAMILY_MAX);
	if (family->count == 1)
		return (RELIC_FAMILY_DELTA_FIRST);

	candidates = family->candidates;
	delta_first = relic_cost_estimate(
	    &candidates[RELIC_FAMILY_DELTA_FIRST].recipe, env, catalog);
	input_first = relic_cost_estimate(
	    &candidates[RELIC_FAMILY_INPUT_FIRST].recipe, env, catalog);

	return (input_first < delta_first ? RELIC_FAMILY_INPUT_FIRST :
	    RELIC_FAMILY_DELTA_FIRST);
}

uint64_t
relic_cost_estimate(const relic_cost_recipe_t *recipe,
    const relic_exec_env_t *env, relic_cost_catalog_t *catalog)
{
	const relic_cost_step_t *step;
	const relic_op_t *op;
	uint64_t bindings;
	uint64_t visits;
	uint64_t work;
	relic_tuple_id_t tuples;
	size_t i;

	bindings = 1;
	work = 0;
	for (i = 0; i < recipe->step_count; i++) {
		if (bindings == 0 || work == UINT64_MAX)
			break;
		step = &recipe->steps[i];
		op = step->op;

		switch (step->kind) {
		case RELIC_COST_SCAN:
			tuples = range_length(relic_exec_view(env,
			    &op->u.atom));
			visits = saturating_multiply(bindings, tuples);
			work = saturating_add(work, visits);
			bindings = visits;
			break;
		case RELIC_COST_LOOKUP:
			tuples = range_length(relic_exec_view(env,
			    &op->u.atom));
			visits = relic_cost_lookup_visits(bindings, tuples,
			    index_keys(catalog, step));
			work = saturating_add(work,
			    saturating_add(bindings, visits));
			bindings = visits;
			break;
		case RELIC_COST_PROBE:
			work = saturating_add(work, bindings);
			tuples = range_length(relic_exec_view(env,
			    &op->u.atom));
			if (tuples == 0)
				bindings = 0;
			break;
		case RELIC_COST_ANTI_PROBE:
		case RELIC_COST_OPERATION:
		case RELIC_COST_EMIT:
			work = saturating_add(work, bindings);
			break;
		case RELIC_COST_FOLD_SCAN:
			tuples = range_length(
			    relic_relation_all(op->u.aggregate.relation));
			work = saturating_add(work, saturating_add(bindings,
			    saturating_multiply(bindings, tuples)));
			break;
		case RELIC_COST_FOLD_LOOKUP:
			tuples = range_length(
			    relic_relation_all(op->u.aggregate.relation));
			visits = relic_cost_lookup_visits(bindings, tuples,
			    index_keys(catalog, step));
			work = saturating_add(work,
			    saturating_add(bindings, visits));
			break;
		case RELIC_COST_FOLD_PROBE:
			tuples = range_length(
			    relic_relation_all(op->u.aggregate.relation));
			work = saturating_add(work, saturating_add(bindings,
			    tuples == 0 ? 0 : bindings));
			break;
		}
	}

	return (work);
}

/*
 * B * N / k rounded up, with k = max(1, K), computed as q * N plus
 * ceil(r * N / k) for B = q * k + r.  The remainder r is below k, and k and
 * N are below 2^32, so r * N fits 64 bits and only q * N and the sum can
 * overflow, and those saturate.  The split is exact: B * N / k is q * N
 * plus r * N / k, and q * N is a whole number, so rounding the second part
 * up rounds the whole up.
 */
uint64_t
relic_cost_lookup_visits(uint64_t bindings, relic_tuple_id_t tuples,
    relic_tuple_id_t keys)
{
	uint64_t divisor;
	uint64_t quotient;
	uint64_t remainder_share;

	divisor = keys > 0 ? keys : 1;
	quotient = bindings / divisor;
	remainder_share = (bindings % divisor) * tuples;

	return (saturating_add(saturating_multiply(quotient, tuples),
	    remainder_share / divisor + (remainder_share % divisor != 0)));
}

/*
 * The distinct keys of the index `step` looks up through, from its catalog
 * entry, counted afresh when its relation holds a different number of
 * tuples than when the entry was last counted (cost.h).  The entry is the
 * first not ordered before the index, by binary search; counting reads the
 * index's stripe headers alone.  The catalog holds every index its recipes
 * read (cost.h); were one missing, the lookup would be estimated as
 * visiting every tuple of its view, which costs a worse choice and never a
 * wrong answer.
 */
static relic_tuple_id_t
index_keys(relic_cost_catalog_t *catalog, const relic_cost_step_t *step)
{
	relic_cost_statistic_t wanted;
	relic_cost_statistic_t *statistic;
	const relic_index_t *index;
	size_t high;
	size_t keys;
	size_t low;
	size_t middle;
	relic_tuple_id_t tuples;
	bool found;

	bzero(&wanted, sizeof(wanted));
	found = relic_cost_step_index(step, &wanted.relation,
	    &wanted.index_position);
	assert(found);
	if (!found)
		return (0);

	low = 0;
	high = catalog->count;
	while (low < high) {
		middle = low + (high - low) / 2;
		if (relic_cost_statistic_compare(&catalog->entries[middle],
		    &wanted) < 0)
			low = middle + 1;
		else
			high = middle;
	}
	found = low < catalog->count &&
	    relic_cost_statistic_compare(&catalog->entries[low], &wanted) == 0;
	assert(found);
	if (!found)
		return (0);
	statistic = &catalog->entries[low];

	tuples = range_length(relic_relation_all(statistic->relation));
	if (tuples != statistic->tuple_count) {
		index = relic_relation_index_at(statistic->relation,
		    statistic->index_position);
		keys = relic_index_key_count(index);
		assert(keys <= tuples);
		statistic->key_count = (relic_tuple_id_t)keys;
		statistic->tuple_count = tuples;
	}

	return (statistic->key_count);
}

/*
 * The tuples a range covers, none when it is empty.
 */
static relic_tuple_id_t
range_length(relic_tuple_range_t range)
{
	return (range.end > range.first ? range.end - range.first : 0);
}

static uint64_t
saturating_add(uint64_t left, uint64_t right)
{
	return (left > UINT64_MAX - right ? UINT64_MAX : left + right);
}

static uint64_t
saturating_multiply(uint64_t left, uint64_t right)
{
	if (left != 0 && right > UINT64_MAX / left)
		return (UINT64_MAX);

	return (left * right);
}
