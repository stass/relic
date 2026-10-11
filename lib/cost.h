/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_COST_H
#define RELIC_COST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "exec.h"
#include "frozen.h"
#include "plan.h"

/*
 * The work estimator (lib/cost.c): how much work a delta variant's plan
 * would do in the round about to run, from the sizes of the views it reads
 * and the distinct keys of the indexes it looks up through.
 *
 * A round chooses between the two plans of a variant (plan.h) by this
 * estimate, before any of its items runs.  The estimate is integer
 * arithmetic over counts the store already keeps: it reads no tuple, runs
 * no expression, allocates nothing and moves no frontier, and so cannot
 * change what any plan derives, only which of two equivalent plans derives
 * it.  It reads the store as frozen.h presents it.
 */

typedef struct relic_cost_catalog relic_cost_catalog_t;
typedef struct relic_cost_statistic relic_cost_statistic_t;

/*
 * The distinct keys of one secondary index, `index_position` of
 * `relation`, as last counted.  `key_count` was summed from the index's
 * stripes when the relation held `tuple_count` tuples.  The store only
 * appends while a component is evaluated, so a relation holding as many
 * tuples holds the same ones and the count is still exact: the tuple count
 * is the entry's version, and no insertion has to mark it stale.  Both
 * start at zero, which is the truth for an empty relation, so an entry
 * needs no flag saying whether it has been counted.
 *
 * A relation holds no more keys than tuples, so `key_count` fits a tuple
 * identifier.
 */
struct relic_cost_statistic {
	const relic_relation_t	*relation;
	relic_tuple_id_t	 tuple_count;
	relic_tuple_id_t	 key_count;
	uint32_t		 index_position;
};

/*
 * The statistics a component's recipes read, `count` entries in the order
 * relic_cost_statistic_compare() gives, an entry for every index a recipe
 * step reads and each index once.  The estimator finds a step's entry by
 * searching for the index relic_cost_step_index() names, so an index no
 * entry holds is a broken catalog rather than a zero.  It belongs to the
 * component's plans, lives in the evaluation's arena and is read and
 * refreshed only by the thread that gathers each round, never by a worker.
 */
struct relic_cost_catalog {
	relic_cost_statistic_t	*entries;
	size_t			 count;
};

/*
 * qsort(3) order of catalog entries: by the predicate of the relation, by
 * index position, and then by relation, so that the entries of one index
 * are adjacent and every index has its own place.  The last key only
 * orders the two relations a predicate can have, its derived relation and
 * the base store a base view reads, and since nothing the catalog's order
 * decides is observable, it may compare their addresses.
 */
int	 relic_cost_statistic_compare(const void *left, const void *right);

/*
 * Whether a recipe step reads the distinct keys of an index, and if so
 * which one, as its operation names it: by relation and position.
 */
bool	 relic_cost_step_index(const relic_cost_step_t *step,
	    const relic_relation_t **relationp, uint32_t *positionp);

/*
 * The candidate of `family` the round about to run over the views `env`
 * resolves should run: its only one, or of two the delta-first plan unless
 * the input-first plan is estimated to do strictly less work.  Equal
 * estimates, two saturated ones among them, choose delta-first.
 */
size_t	 relic_cost_choose(const relic_plan_family_t *family,
	    const relic_exec_env_t *env, relic_cost_catalog_t *catalog);

/*
 * The estimated work of running the plan `recipe` describes once, over the
 * views `env` resolves now: one unit per probe, tuple visited, simple
 * operation and emission, saturating at UINT64_MAX.  The catalog entries
 * of the indexes the recipe's lookups name are refreshed when their
 * relation has grown.  It is a ranking of plans, not a prediction of time.
 */
uint64_t relic_cost_estimate(const relic_cost_recipe_t *recipe,
	    const relic_exec_env_t *env, relic_cost_catalog_t *catalog);

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
