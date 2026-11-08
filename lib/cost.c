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

#include "cost.h"

/*
 * The work estimator: a walk of a recipe's steps in plan order, carrying B,
 * the bindings estimated to reach the step, from one, and W, the work
 * accumulated, from zero (plan.h gives each step's rule).  The walk stops
 * once no binding reaches the next step, since nothing after it runs; a
 * saturated W stays saturated, but the bindings still reach EMIT, which
 * predicts the plan's derivations.  A candidate's
 * estimate adds to W the work of building the indexes its draft names and
 * its relations lack, which preparing it would do before it ran.
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

static uint64_t	 acquisition_work(const relic_plan_draft_t *draft);
static uint64_t	 execution_work(const relic_cost_recipe_t *recipe,
		    const relic_exec_env_t *env, uint64_t *emitted);
static relic_tuple_id_t index_keys(const relic_relation_t *relation,
		    uint32_t key_mask);
static relic_tuple_id_t range_length(relic_tuple_range_t range);
static uint64_t	 saturating_add(uint64_t left, uint64_t right);
static uint64_t	 saturating_multiply(uint64_t left, uint64_t right);

size_t
relic_cost_choose(const relic_plan_family_t *family,
    const relic_exec_env_t *env)
{
	const relic_plan_candidate_t *candidates;
	uint64_t delta_first;
	uint64_t input_first;

	assert(family->count >= 1 && family->count <= RELIC_FAMILY_MAX);
	if (family->count == 1)
		return (RELIC_FAMILY_DELTA_FIRST);

	candidates = family->candidates;
	delta_first = relic_cost_estimate(
	    &candidates[RELIC_FAMILY_DELTA_FIRST], env);
	input_first = relic_cost_estimate(
	    &candidates[RELIC_FAMILY_INPUT_FIRST], env);

	return (input_first < delta_first ? RELIC_FAMILY_INPUT_FIRST :
	    RELIC_FAMILY_DELTA_FIRST);
}

uint64_t
relic_cost_estimate(const relic_plan_candidate_t *candidate,
    const relic_exec_env_t *env)
{
	uint64_t emitted;

	return (saturating_add(execution_work(&candidate->recipe, env,
	    &emitted), acquisition_work(&candidate->draft)));
}

uint64_t
relic_cost_derivations(const relic_plan_candidate_t *candidate,
    const relic_exec_env_t *env)
{
	uint64_t emitted;

	(void)execution_work(&candidate->recipe, env, &emitted);

	return (emitted);
}

/*
 * The work of running a recipe's plan once, by the walk above, and in
 * *emitted the bindings estimated to reach its EMIT, each a tuple derived.
 * The walk goes on after its work saturates, since later steps may still
 * lower the bindings that reach EMIT.
 */
static uint64_t
execution_work(const relic_cost_recipe_t *recipe,
    const relic_exec_env_t *env, uint64_t *emitted)
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
	*emitted = 0;
	for (i = 0; i < recipe->step_count; i++) {
		if (bindings == 0)
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
			    index_keys(op->u.atom.relation,
			    op->u.atom.key_mask));
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
			work = saturating_add(work, bindings);
			break;
		case RELIC_COST_EMIT:
			work = saturating_add(work, bindings);
			*emitted = saturating_add(*emitted, bindings);
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
			    index_keys(op->u.aggregate.relation,
			    op->u.aggregate.key_mask));
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
 * The work of building the indexes a draft names that their relations
 * lack: two units per tuple of the relation for each, a visit of the tuple
 * and a probe of the new index for its key.  The draft names each index
 * once, so each is charged once however many operations read it, and
 * whether or not the walk above reaches them: preparing the plan builds
 * them all.
 */
static uint64_t
acquisition_work(const relic_plan_draft_t *draft)
{
	const relic_plan_access_t *access;
	uint64_t work;
	uint32_t position;
	size_t i;

	work = 0;
	for (i = 0; i < draft->access_count; i++) {
		access = &draft->accesses[i];
		if (relic_relation_index_find(access->relation,
		    access->key_mask, &position))
			continue;
		work = saturating_add(work, saturating_multiply(2,
		    range_length(relic_relation_all(access->relation))));
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
 * The distinct keys over the whole relation of its index keyed on
 * `key_mask`, as the index's stripe headers count them.  An index the
 * relation has not built is taken to hold a key per tuple, the most it
 * could: the estimate of its lookups is the least it could be, so the
 * plan that would build it is not ruled out before it is tried, and
 * acquisition_work() charges the build.  Nothing here builds an index to
 * learn its count.
 */
static relic_tuple_id_t
index_keys(const relic_relation_t *relation, uint32_t key_mask)
{
	relic_tuple_id_t tuples;
	uint32_t position;
	size_t keys;

	tuples = range_length(relic_relation_all(relation));
	if (!relic_relation_index_find(relation, key_mask, &position))
		return (tuples);

	keys = relic_index_key_count(relic_relation_index_at(relation,
	    position));
	assert(keys <= tuples);

	return ((relic_tuple_id_t)keys);
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
