/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_COST_H
#define RELIC_COST_H

#include <stddef.h>
#include <stdint.h>

#include "exec.h"
#include "frozen.h"
#include "plan.h"

/*
 * The work estimator (lib/cost.c): how much work a delta variant's plan
 * would do in the round about to run, from the sizes of the views it reads
 * and the distinct keys of the indexes it looks up through, and what
 * building the indexes it lacks would cost; and, by the same rules, how
 * much a round's prepared plans are expected to do, which decides whether
 * the round is shared and how finely it is cut (lib/round.c).
 *
 * A round chooses between the two plans of a variant (plan.h) by this
 * estimate, before any of its items runs.  The estimate is integer
 * arithmetic over counts the store already keeps: it reads no tuple, runs
 * no expression, builds no index, allocates nothing and moves no frontier,
 * and so cannot change what any plan derives, only which of two equivalent
 * plans derives it.  It reads the store as frozen.h presents it.
 */

/*
 * The candidate of `family` the round about to run over the views `env`
 * resolves should run: its only one, or of two the delta-first plan unless
 * the input-first plan is estimated to do strictly less work.  Equal
 * estimates, two saturated ones among them, choose delta-first.
 */
size_t	 relic_cost_choose(const relic_plan_family_t *family,
	    const relic_exec_env_t *env);

/*
 * The estimated work of a candidate in the round about to run over the
 * views `env` resolves: what running its plan once would do, one unit per
 * probe, tuple visited, simple operation and emission, plus what building
 * the indexes its draft names and its relations lack would, two units per
 * tuple of the relation for each.  A lookup through an index not yet built
 * is estimated as though every tuple of its relation had a key of its own.
 * Saturates at UINT64_MAX.  It is a ranking of plans, not a prediction of
 * time, and reads nothing but its arguments and the store they reach.
 */
uint64_t relic_cost_estimate(const relic_plan_candidate_t *candidate,
	    const relic_exec_env_t *env);

/*
 * What a round's scheduler expects a prepared plan to do in the round
 * about to run, walked over the plan's own operations by the rules a
 * candidate's recipe is (plan.h), with no index to build: the indexes a
 * prepared plan reads exist.
 *
 * relic_cost_plan() is the work of running the plan once from its first
 * operation.  relic_cost_driven() is the work of its driver at `driver`
 * (relic_plan_driver()) and everything after it, when the driver visits
 * `visits` tuples: one unit for each, and the operations after it walked
 * from `visits` bindings, as though every tuple visited matched, since how
 * many match is not known before they are read.  The prefix before the
 * driver is not counted.  Both saturate at UINT64_MAX.
 */
uint64_t relic_cost_plan(const relic_plan_t *plan,
	    const relic_exec_env_t *env);
uint64_t relic_cost_driven(const relic_plan_t *plan, size_t driver,
	    uint64_t visits, const relic_exec_env_t *env);

/*
 * The class the estimator counts an operation as (plan.h): by its kind and,
 * for an AGGREGATE, by how it reaches its group's rows.  A recipe's steps
 * are classified by it, so a recipe and a prepared plan are counted alike.
 */
enum relic_cost_kind relic_cost_kind_of(const relic_op_t *op);

/*
 * The tuples a lookup is estimated to visit for `bindings` probes into a
 * view of `tuples` tuples whose index holds `keys` distinct keys over its
 * whole relation: ceil(bindings * tuples / max(1, keys)), saturating at
 * UINT64_MAX however large the product, and exact whenever the quotient is
 * representable.
 */
uint64_t relic_cost_lookup_visits(uint64_t bindings, relic_tuple_id_t tuples,
	    relic_tuple_id_t keys);

#endif /* RELIC_COST_H */
