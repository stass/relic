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
 * The work estimator: a walk of a plan's operations in order, carrying B,
 * the bindings estimated to reach the operation, and W, the work
 * accumulated, from zero (plan.h gives each operation's rule).  The walk
 * stops once no binding reaches the next operation, since nothing after it
 * runs, or once W has saturated, since nothing after it can lower W.  A
 * candidate's estimate walks its recipe from one binding and adds to W the
 * work of building the indexes its draft names and its relations lack,
 * which preparing it would do before it ran.  A round's scheduler walks a
 * prepared plan's own operations, from one binding or from a driver's
 * measured visits, by the same rules and with nothing to build.
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

typedef struct relic_cost_walk relic_cost_walk_t;

/*
 * Where a walk is: B, the bindings reaching the next operation, and W, the
 * work so far.
 */
struct relic_cost_walk {
	uint64_t	bindings;
	uint64_t	work;
};

static uint64_t	 acquisition_work(const relic_plan_draft_t *draft);
static void	 count_op(relic_cost_walk_t *walk, const relic_op_t *op,
		    enum relic_cost_kind kind, const relic_exec_env_t *env);
static uint64_t	 execution_work(const relic_cost_recipe_t *recipe,
		    const relic_exec_env_t *env);
static enum relic_cost_kind fold_kind(const relic_op_aggregate_t *fold);
static relic_tuple_id_t index_keys(const relic_relation_t *relation,
		    uint32_t key_mask);
static uint64_t	 plan_walk(const relic_plan_t *plan, size_t from,
		    uint64_t bindings, const relic_exec_env_t *env);
static relic_tuple_id_t range_length(relic_tuple_range_t range);
static uint64_t	 saturating_add(uint64_t left, uint64_t right);
static uint64_t	 saturating_multiply(uint64_t left, uint64_t right);
static bool	 walk_is_over(const relic_cost_walk_t *walk);

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
	return (saturating_add(execution_work(&candidate->recipe, env),
	    acquisition_work(&candidate->draft)));
}

uint64_t
relic_cost_plan(const relic_plan_t *plan, const relic_exec_env_t *env)
{
	return (plan_walk(plan, 0, 1, env));
}

uint64_t
relic_cost_driven(const relic_plan_t *plan, size_t driver, uint64_t visits,
    const relic_exec_env_t *env)
{
	assert(driver < plan->op_count);

	return (saturating_add(visits, plan_walk(plan, driver + 1, visits,
	    env)));
}

/*
 * Each switch names every value of its enum, so that a kind added later
 * must be classified here rather than be counted as free.
 */
enum relic_cost_kind
relic_cost_kind_of(const relic_op_t *op)
{
	switch (op->kind) {
	case RELIC_OP_SCAN:
		return (RELIC_COST_SCAN);
	case RELIC_OP_LOOKUP:
		return (RELIC_COST_LOOKUP);
	case RELIC_OP_MEMBER:
	case RELIC_OP_EXISTS:
		return (RELIC_COST_PROBE);
	case RELIC_OP_ANTI_MEMBER:
		return (RELIC_COST_ANTI_PROBE);
	case RELIC_OP_FILTER:
	case RELIC_OP_ASSIGN:
		return (RELIC_COST_OPERATION);
	case RELIC_OP_EMIT:
		return (RELIC_COST_EMIT);
	case RELIC_OP_AGGREGATE:
		break;
	}

	return (fold_kind(&op->u.aggregate));
}

static enum relic_cost_kind
fold_kind(const relic_op_aggregate_t *fold)
{
	switch (fold->access) {
	case RELIC_FOLD_SCAN:
		return (RELIC_COST_FOLD_SCAN);
	case RELIC_FOLD_LOOKUP:
		return (RELIC_COST_FOLD_LOOKUP);
	case RELIC_FOLD_SUMMARY:
		return (RELIC_COST_FOLD_SUMMARY);
	case RELIC_FOLD_PROBE:
		break;
	}

	return (RELIC_COST_FOLD_PROBE);
}

/*
 * The work of running a recipe's plan once, by the walk above.
 */
static uint64_t
execution_work(const relic_cost_recipe_t *recipe,
    const relic_exec_env_t *env)
{
	relic_cost_walk_t walk;
	size_t i;

	walk.bindings = 1;
	walk.work = 0;
	for (i = 0; i < recipe->step_count && !walk_is_over(&walk); i++)
		count_op(&walk, recipe->steps[i].op, recipe->steps[i].kind,
		    env);

	return (walk.work);
}

/*
 * The work of a plan's operations from position `from` on, reached by
 * `bindings` bindings, by the walk above.
 */
static uint64_t
plan_walk(const relic_plan_t *plan, size_t from, uint64_t bindings,
    const relic_exec_env_t *env)
{
	relic_cost_walk_t walk;
	const relic_op_t *op;
	size_t i;

	walk.bindings = bindings;
	walk.work = 0;
	for (i = from; i < plan->op_count && !walk_is_over(&walk); i++) {
		op = &plan->ops[i];
		count_op(&walk, op, relic_cost_kind_of(op), env);
	}

	return (walk.work);
}

/*
 * Whether nothing after this point of a walk can change its work: no
 * binding reaches the next operation, or the work has saturated.
 */
static bool
walk_is_over(const relic_cost_walk_t *walk)
{
	return (walk->bindings == 0 || walk->work == UINT64_MAX);
}

/*
 * Count one operation, of class `kind`, into a walk: the work it does for
 * the bindings reaching it, and the bindings it passes on (plan.h).
 */
static void
count_op(relic_cost_walk_t *walk, const relic_op_t *op,
    enum relic_cost_kind kind, const relic_exec_env_t *env)
{
	uint64_t bindings;
	uint64_t visits;
	relic_tuple_id_t tuples;

	bindings = walk->bindings;
	switch (kind) {
	case RELIC_COST_SCAN:
		tuples = range_length(relic_exec_view(env, &op->u.atom));
		visits = saturating_multiply(bindings, tuples);
		walk->work = saturating_add(walk->work, visits);
		walk->bindings = visits;
		break;
	case RELIC_COST_LOOKUP:
		tuples = range_length(relic_exec_view(env, &op->u.atom));
		visits = relic_cost_lookup_visits(bindings, tuples,
		    index_keys(op->u.atom.relation, op->u.atom.key_mask));
		walk->work = saturating_add(walk->work,
		    saturating_add(bindings, visits));
		walk->bindings = visits;
		break;
	case RELIC_COST_PROBE:
		walk->work = saturating_add(walk->work, bindings);
		tuples = range_length(relic_exec_view(env, &op->u.atom));
		if (tuples == 0)
			walk->bindings = 0;
		break;
	case RELIC_COST_ANTI_PROBE:
	case RELIC_COST_OPERATION:
	case RELIC_COST_EMIT:
		walk->work = saturating_add(walk->work, bindings);
		break;
	case RELIC_COST_FOLD_SCAN:
		tuples = range_length(
		    relic_relation_all(op->u.aggregate.relation));
		walk->work = saturating_add(walk->work, saturating_add(
		    bindings, saturating_multiply(bindings, tuples)));
		break;
	case RELIC_COST_FOLD_LOOKUP:
		tuples = range_length(
		    relic_relation_all(op->u.aggregate.relation));
		visits = relic_cost_lookup_visits(bindings, tuples,
		    index_keys(op->u.aggregate.relation,
		    op->u.aggregate.key_mask));
		walk->work = saturating_add(walk->work,
		    saturating_add(bindings, visits));
		break;
	case RELIC_COST_FOLD_PROBE:
		tuples = range_length(
		    relic_relation_all(op->u.aggregate.relation));
		walk->work = saturating_add(walk->work, saturating_add(
		    bindings, tuples == 0 ? 0 : bindings));
		break;
	case RELIC_COST_FOLD_SUMMARY:
		walk->work = saturating_add(walk->work,
		    saturating_add(bindings, bindings));
		break;
	}
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
