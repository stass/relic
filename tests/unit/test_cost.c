/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the work estimator (lib/cost.c).
 *
 * A wrong estimate does not fail: it chooses the slower of two plans that
 * derive the same tuples, and every answer is still right.  So the
 * estimator is held here to the exact numbers its rules give, over
 * relations built to the sizes and key counts the rules are written in
 * terms of, rather than to whichever plan happens to win.  The recipes are
 * assembled by hand from operations that name those relations, so each
 * rule is exercised on its own and the worked example of the design is
 * checked to the unit.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_ARITHMETIC	"a lookup's visits are exact or saturate"
#define	SCENARIO_EXAMPLE	"the worked example costs what it says"
#define	SCENARIO_DIRECTIONS	"a small delta and a large one choose apart"
#define	SCENARIO_EMPTY		"an empty view ends the bindings, not the work"
#define	SCENARIO_FOLDS		"a fold counts its group's rows by its access"
#define	SCENARIO_SATURATED	"saturated work ties, and ties go delta-first"
#define	SCENARIO_STATISTICS	"key counts follow the relation's growth"
#define	SCENARIO_STEP_INDEX	"a step names the index its operation reads"

/*
 * The worked example's sizes: guard tuples, edges, distinct edge sources
 * and destinations, and keys of the recursive relation's first column.
 */
#define	GUARDS		2000u
#define	EDGES		100000u
#define	SOURCES		2000u
#define	DESTINATIONS	10000u
#define	R_KEYS		10000u

/* The large delta of the worked example. */
#define	LARGE_DELTA	1000000u

typedef struct example example_t;

/*
 * The relations of the worked example, the indexes its lookups read, and
 * the deltas a round would resolve its views from:
 *
 *	r(X, Z) :- guard(X), edge(X, Y), r(Y, Z).
 *
 * `deltas` is indexed by predicate identifier minus one, as an
 * evaluation's are, and `env` resolves views through it.
 */
struct example {
	relic_t			*ctx;
	relic_relation_t	*guard;
	relic_relation_t	*edge;
	relic_relation_t	*r;
	relic_relation_t	*empty;
	relic_tuple_range_t	*deltas;
	relic_exec_env_t	 env;
	uint32_t		 edge_by_source;
	uint32_t		 edge_by_destination;
	uint32_t		 r_by_first;
	uint32_t		 empty_by_first;
};

static bool	 example_build(example_t *example);
static void	 example_release(example_t *example);
static void	 set_r_delta(example_t *example, relic_tuple_id_t first,
		    relic_tuple_id_t end);
static relic_relation_t *relation(relic_t *ctx, const char *name,
		    uint16_t arity);
static bool	 insert_pair(relic_t *ctx, relic_relation_t *rel,
		    unsigned int left, unsigned int right);
static relic_dom_t value(relic_t *ctx, unsigned int ordinal);
static void	 atom_op(relic_op_t *op, enum relic_op_kind kind,
		    const relic_relation_t *rel, enum relic_view view,
		    uint32_t index_position);
static void	 fold_op(relic_op_t *op, const relic_relation_t *rel,
		    enum relic_fold_access access, uint32_t index_position);
static void	 step(relic_cost_step_t *steps, size_t i, const relic_op_t *op,
		    enum relic_cost_kind kind);
static void	 catalog_entry(relic_cost_statistic_t *entries, size_t i,
		    const relic_relation_t *rel, uint32_t index_position);
static void	 catalog_sort(relic_cost_catalog_t *catalog,
		    relic_cost_statistic_t *entries, size_t count);
static const relic_cost_statistic_t *catalog_find(
		    const relic_cost_catalog_t *catalog,
		    const relic_relation_t *rel, uint32_t index_position);
static void	 a_step_names_the_index_its_operation_reads(void);
static void	 a_lookups_visits_are_exact_or_saturate(void);
static void	 the_worked_example_costs_what_it_says(void);
static void	 an_empty_view_ends_the_bindings_not_the_work(void);
static void	 a_fold_counts_its_groups_rows_by_its_access(void);
static void	 saturated_work_ties_and_ties_go_delta_first(void);
static void	 key_counts_follow_the_relations_growth(void);

/*
 * The relation of a new predicate with this name and arity, made a derived
 * relation, as an evaluation's are, by appending a rule that derives it to
 * a scratch rule table.  NULL having complained.
 */
static relic_relation_t *
relation(relic_t *ctx, const char *name, uint16_t arity)
{
	relic_location_t where;
	relic_ruletab_t rules;
	relic_rule_t rule;
	relic_pred_id_t pred;
	int appended;

	bzero(&where, sizeof(where));
	where.file = "test_cost.c";
	where.line = 1;
	where.column = 1;

	if (relic_pred_intern(ctx, name, strlen(name), arity, &where,
	    &pred) != 0)
		return (NULL);

	bzero(&rules, sizeof(rules));
	bzero(&rule, sizeof(rule));
	rule.head = pred;
	rule.head_arity = arity;
	appended = relic_ruletab_append(ctx, &rules, &rule);
	free(rules.entries);

	return (appended == 0 ? relic_relation_written(ctx, pred) : NULL);
}

static relic_dom_t
value(relic_t *ctx, unsigned int ordinal)
{
	relic_dom_t dom;

	if (relic_value_intern_integer(ctx, (int64_t)ordinal, &dom) != 0)
		return (RELIC_ID_NONE);

	return (dom);
}

/*
 * Insert (left, right) into a binary relation, or (left) into a unary one.
 * Returns whether the tuple was added.
 */
static bool
insert_pair(relic_t *ctx, relic_relation_t *rel, unsigned int left,
    unsigned int right)
{
	relic_dom_t tuple[2];
	bool added;

	tuple[0] = value(ctx, left);
	tuple[1] = value(ctx, right);

	return (relic_relation_insert(ctx, rel, tuple, &added) == 0 && added);
}

/*
 * Build the worked example's store: guard(0 .. 1999); 100000 edges from
 * 2000 sources to 10000 destinations, edge i running from i mod 2000 to
 * (i / 2000) * 200 + i mod 200, which names each pair once and reaches
 * every destination; r with 10000 tuples, one per first-column key; and an
 * empty relation.  Indexes are built on edge's two columns and r's first.
 */
static bool
example_build(example_t *example)
{
	relic_config_t config;
	unsigned int i;

	bzero(example, sizeof(*example));
	bzero(&config, sizeof(config));
	example->ctx = relic_create_config(&config);
	if (example->ctx == NULL)
		return (false);

	example->guard = relation(example->ctx, "guard", 1);
	example->edge = relation(example->ctx, "edge", 2);
	example->r = relation(example->ctx, "r", 2);
	example->empty = relation(example->ctx, "empty", 2);
	if (example->guard == NULL || example->edge == NULL ||
	    example->r == NULL || example->empty == NULL)
		return (false);

	for (i = 0; i < GUARDS; i++) {
		if (!insert_pair(example->ctx, example->guard, i, 0))
			return (false);
	}
	for (i = 0; i < EDGES; i++) {
		if (!insert_pair(example->ctx, example->edge, i % SOURCES,
		    (i / SOURCES) * 200 + i % 200))
			return (false);
	}
	for (i = 0; i < R_KEYS; i++) {
		if (!insert_pair(example->ctx, example->r, i, 0))
			return (false);
	}

	if (relic_relation_index(example->ctx, example->edge, 1u,
	    &example->edge_by_source) != 0 ||
	    relic_relation_index(example->ctx, example->edge, 2u,
	    &example->edge_by_destination) != 0 ||
	    relic_relation_index(example->ctx, example->r, 1u,
	    &example->r_by_first) != 0 ||
	    relic_relation_index(example->ctx, example->empty, 1u,
	    &example->empty_by_first) != 0)
		return (false);

	example->deltas = calloc(relic_pred_count(example->ctx),
	    sizeof(*example->deltas));
	if (example->deltas == NULL)
		return (false);

	example->env.ctx = example->ctx;
	example->env.deltas = example->deltas;
	set_r_delta(example, R_KEYS, R_KEYS + 1);

	return (true);
}

static void
example_release(example_t *example)
{
	free(example->deltas);
	relic_destroy(example->ctx);
	bzero(example, sizeof(*example));
}

/*
 * Make r's delta the range [first, end).  The estimator reads a view only
 * as the range it resolves to, so a range past r's tuples stands for a
 * delta that large without the store having to hold it.
 */
static void
set_r_delta(example_t *example, relic_tuple_id_t first, relic_tuple_id_t end)
{
	relic_tuple_range_t *delta;

	delta = &example->deltas[relic_relation_pred(example->r) - 1];
	delta->first = first;
	delta->end = end;
}

static void
atom_op(relic_op_t *op, enum relic_op_kind kind, const relic_relation_t *rel,
    enum relic_view view, uint32_t index_position)
{
	bzero(op, sizeof(*op));
	op->kind = kind;
	op->u.atom.relation = rel;
	op->u.atom.view = view;
	op->u.atom.index_position = index_position;
	op->u.atom.arity = rel == NULL ? 0 : relic_relation_arity(rel);
}

static void
fold_op(relic_op_t *op, const relic_relation_t *rel,
    enum relic_fold_access access, uint32_t index_position)
{
	bzero(op, sizeof(*op));
	op->kind = RELIC_OP_AGGREGATE;
	op->u.aggregate.relation = rel;
	op->u.aggregate.access = access;
	op->u.aggregate.index_position = index_position;
}

static void
step(relic_cost_step_t *steps, size_t i, const relic_op_t *op,
    enum relic_cost_kind kind)
{
	bzero(&steps[i], sizeof(steps[i]));
	steps[i].op = op;
	steps[i].kind = kind;
}

static void
catalog_entry(relic_cost_statistic_t *entries, size_t i,
    const relic_relation_t *rel, uint32_t index_position)
{
	bzero(&entries[i], sizeof(entries[i]));
	entries[i].relation = rel;
	entries[i].index_position = index_position;
}

/*
 * Make `catalog` the `count` entries of `entries`, in the order the
 * estimator searches them by.
 */
static void
catalog_sort(relic_cost_catalog_t *catalog, relic_cost_statistic_t *entries,
    size_t count)
{
	qsort(entries, count, sizeof(*entries), relic_cost_statistic_compare);
	catalog->entries = entries;
	catalog->count = count;
}

/*
 * The entry of `catalog` for an index, wherever the sort put it.
 */
static const relic_cost_statistic_t *
catalog_find(const relic_cost_catalog_t *catalog, const relic_relation_t *rel,
    uint32_t index_position)
{
	size_t i;

	for (i = 0; i < catalog->count; i++) {
		if (catalog->entries[i].relation == rel &&
		    catalog->entries[i].index_position == index_position)
			return (&catalog->entries[i]);
	}

	return (NULL);
}

/*
 * A lookup and a looked-up fold name the index their operation reads, the
 * catalog's key; every other step names none.
 */
static void
a_step_names_the_index_its_operation_reads(void)
{
	const relic_relation_t *named;
	relic_relation_t *rel;
	relic_cost_step_t steps[3];
	relic_op_t ops[3];
	relic_t *ctx;
	uint32_t position;

	ctx = relic_create();
	rel = ctx == NULL ? NULL : relation(ctx, "p", 2);
	if (rel == NULL) {
		check(false, SCENARIO_STEP_INDEX, "a relation is made");
		relic_destroy(ctx);
		return;
	}

	atom_op(&ops[0], RELIC_OP_LOOKUP, rel, RELIC_VIEW_ALL, 3);
	fold_op(&ops[1], rel, RELIC_FOLD_LOOKUP, 5);
	atom_op(&ops[2], RELIC_OP_SCAN, rel, RELIC_VIEW_ALL, 7);
	step(steps, 0, &ops[0], RELIC_COST_LOOKUP);
	step(steps, 1, &ops[1], RELIC_COST_FOLD_LOOKUP);
	step(steps, 2, &ops[2], RELIC_COST_SCAN);

	named = NULL;
	position = 0;
	check(relic_cost_step_index(&steps[0], &named, &position) &&
	    named == rel && position == 3, SCENARIO_STEP_INDEX,
	    "a lookup names its operation's index");
	named = NULL;
	check(relic_cost_step_index(&steps[1], &named, &position) &&
	    named == rel && position == 5, SCENARIO_STEP_INDEX,
	    "a looked-up fold names its operation's index");
	check(!relic_cost_step_index(&steps[2], &named, &position),
	    SCENARIO_STEP_INDEX, "a scan names no index");

	relic_destroy(ctx);
}

/*
 * ceil(B * N / max(1, K)): zero for no bindings or an empty view, a whole
 * view per binding for an index of no keys, rounded up only after the
 * multiplication, exact when B * N overflows but the quotient does not,
 * and saturated when the quotient does not fit.
 */
static void
a_lookups_visits_are_exact_or_saturate(void)
{
	check(relic_cost_lookup_visits(0, 10, 4) == 0 &&
	    relic_cost_lookup_visits(5, 0, 4) == 0 &&
	    relic_cost_lookup_visits(0, 0, 0) == 0, SCENARIO_ARITHMETIC,
	    "no bindings or an empty view visit nothing");
	check(relic_cost_lookup_visits(3, 10, 0) == 30,
	    SCENARIO_ARITHMETIC, "an index of no keys divides by one");
	check(relic_cost_lookup_visits(4, 10, 4) == 10 &&
	    relic_cost_lookup_visits(3, 10, 4) == 8 &&
	    relic_cost_lookup_visits(1, 1, 10000) == 1 &&
	    relic_cost_lookup_visits(10, 1, 100) == 1,
	    SCENARIO_ARITHMETIC, "exact quotients stay exact and the rest "
	    "round up after multiplying");
	check(relic_cost_lookup_visits(UINT64_C(1) << 62, 4, 8) ==
	    UINT64_C(1) << 61, SCENARIO_ARITHMETIC,
	    "a product past 64 bits keeps its representable quotient");
	check(relic_cost_lookup_visits(UINT64_MAX, RELIC_TUPLE_MAX,
	    RELIC_TUPLE_MAX) == UINT64_MAX &&
	    relic_cost_lookup_visits(RELIC_TUPLE_MAX - 1, RELIC_TUPLE_MAX,
	    RELIC_TUPLE_MAX) == RELIC_TUPLE_MAX - 1 &&
	    relic_cost_lookup_visits(UINT64_MAX, 1, 1) == UINT64_MAX,
	    SCENARIO_ARITHMETIC, "the largest counts give their exact "
	    "quotient");
	check(relic_cost_lookup_visits(UINT64_C(1) << 62, 8, 1) ==
	    UINT64_MAX &&
	    relic_cost_lookup_visits(UINT64_MAX, RELIC_TUPLE_MAX, 1) ==
	    UINT64_MAX, SCENARIO_ARITHMETIC,
	    "a quotient past 64 bits saturates");
}

/*
 * The design's worked example, r(X, Z) :- guard(X), edge(X, Y), r(Y, Z),
 * with the guard measured at 2000, 100000 edges over 2000 sources and
 * 10000 destinations, and r's first-column index at 10000 keys:
 *
 *	delta-first	D + (D + 10D) + 10D + 10D	= 32D
 *	input-first	2000 + (2000 + 100000) + (100000 + 10D) + 10D
 *			= 204000 + 20D
 *
 * A one-tuple delta selects delta-first, 32 against 204020; a million-tuple
 * delta selects input-first, 20204000 against 32000000.  Both plans are
 * estimated through one catalog, as a component's are, and a family of the
 * two chooses as their estimates say.  The catalog is filled out of order
 * and sorted, as an evaluation's is.
 */
static void
the_worked_example_costs_what_it_says(void)
{
	relic_op_t delta_first_ops[4];
	relic_op_t input_first_ops[4];
	relic_cost_step_t delta_first_steps[4];
	relic_cost_step_t input_first_steps[4];
	relic_cost_statistic_t entries[3];
	relic_cost_recipe_t delta_first;
	relic_cost_recipe_t input_first;
	const relic_cost_statistic_t *by_destination;
	const relic_cost_statistic_t *by_first;
	const relic_cost_statistic_t *by_source;
	relic_plan_family_t family;
	relic_cost_catalog_t catalog;
	example_t example;
	uint64_t first_cost;
	uint64_t input_cost;

	if (!example_build(&example)) {
		check(false, SCENARIO_EXAMPLE, "the example is built");
		example_release(&example);
		return;
	}

	catalog_entry(entries, 0, example.r, example.r_by_first);
	catalog_entry(entries, 1, example.edge, example.edge_by_destination);
	catalog_entry(entries, 2, example.edge, example.edge_by_source);
	catalog_sort(&catalog, entries, 3);

	atom_op(&delta_first_ops[0], RELIC_OP_SCAN, example.r,
	    RELIC_VIEW_DELTA, 0);
	atom_op(&delta_first_ops[1], RELIC_OP_LOOKUP, example.edge,
	    RELIC_VIEW_ALL, example.edge_by_destination);
	atom_op(&delta_first_ops[2], RELIC_OP_MEMBER, example.guard,
	    RELIC_VIEW_ALL, 0);
	atom_op(&delta_first_ops[3], RELIC_OP_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	step(delta_first_steps, 0, &delta_first_ops[0], RELIC_COST_SCAN);
	step(delta_first_steps, 1, &delta_first_ops[1], RELIC_COST_LOOKUP);
	step(delta_first_steps, 2, &delta_first_ops[2], RELIC_COST_PROBE);
	step(delta_first_steps, 3, &delta_first_ops[3], RELIC_COST_EMIT);
	delta_first.steps = delta_first_steps;
	delta_first.step_count = 4;

	atom_op(&input_first_ops[0], RELIC_OP_SCAN, example.guard,
	    RELIC_VIEW_ALL, 0);
	atom_op(&input_first_ops[1], RELIC_OP_LOOKUP, example.edge,
	    RELIC_VIEW_ALL, example.edge_by_source);
	atom_op(&input_first_ops[2], RELIC_OP_LOOKUP, example.r,
	    RELIC_VIEW_DELTA, example.r_by_first);
	atom_op(&input_first_ops[3], RELIC_OP_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	step(input_first_steps, 0, &input_first_ops[0], RELIC_COST_SCAN);
	step(input_first_steps, 1, &input_first_ops[1], RELIC_COST_LOOKUP);
	step(input_first_steps, 2, &input_first_ops[2], RELIC_COST_LOOKUP);
	step(input_first_steps, 3, &input_first_ops[3], RELIC_COST_EMIT);
	input_first.steps = input_first_steps;
	input_first.step_count = 4;

	set_r_delta(&example, R_KEYS, R_KEYS + 1);
	first_cost = relic_cost_estimate(&delta_first, &example.env, &catalog);
	input_cost = relic_cost_estimate(&input_first, &example.env, &catalog);
	checkf(first_cost == 32 && input_cost == 204020, SCENARIO_EXAMPLE,
	    "a one-tuple delta costs 32 delta-first and 204020 input-first, "
	    "not %llu and %llu", (unsigned long long)first_cost,
	    (unsigned long long)input_cost);
	check(first_cost < input_cost, SCENARIO_DIRECTIONS,
	    "a one-tuple delta chooses delta-first");

	set_r_delta(&example, R_KEYS, R_KEYS + LARGE_DELTA);
	first_cost = relic_cost_estimate(&delta_first, &example.env, &catalog);
	input_cost = relic_cost_estimate(&input_first, &example.env, &catalog);
	checkf(first_cost == 32 * (uint64_t)LARGE_DELTA &&
	    input_cost == 204000 + 20 * (uint64_t)LARGE_DELTA,
	    SCENARIO_EXAMPLE, "a million-tuple delta costs 32000000 "
	    "delta-first and 20204000 input-first, not %llu and %llu",
	    (unsigned long long)first_cost, (unsigned long long)input_cost);
	check(input_cost < first_cost, SCENARIO_DIRECTIONS,
	    "a million-tuple delta chooses input-first");

	bzero(&family, sizeof(family));
	family.candidates[RELIC_FAMILY_DELTA_FIRST].recipe = delta_first;
	family.candidates[RELIC_FAMILY_INPUT_FIRST].recipe = input_first;
	family.count = 2;
	check(relic_cost_choose(&family, &example.env, &catalog) ==
	    RELIC_FAMILY_INPUT_FIRST, SCENARIO_DIRECTIONS,
	    "a family chooses input-first for a million-tuple delta");
	set_r_delta(&example, R_KEYS, R_KEYS + 1);
	check(relic_cost_choose(&family, &example.env, &catalog) ==
	    RELIC_FAMILY_DELTA_FIRST, SCENARIO_DIRECTIONS,
	    "a family chooses delta-first for a one-tuple delta");

	by_source = catalog_find(&catalog, example.edge,
	    example.edge_by_source);
	by_destination = catalog_find(&catalog, example.edge,
	    example.edge_by_destination);
	by_first = catalog_find(&catalog, example.r, example.r_by_first);
	if (by_source == NULL || by_destination == NULL || by_first == NULL) {
		check(false, SCENARIO_STATISTICS, "the catalog keeps every "
		    "index");
		example_release(&example);
		return;
	}
	checkf(by_source->key_count == SOURCES &&
	    by_destination->key_count == DESTINATIONS &&
	    by_first->key_count == R_KEYS && by_first->tuple_count == R_KEYS,
	    SCENARIO_STATISTICS,
	    "the catalog counted %u, %u and %u keys, not %u, %u and %u",
	    (unsigned int)by_source->key_count,
	    (unsigned int)by_destination->key_count,
	    (unsigned int)by_first->key_count, SOURCES, DESTINATIONS,
	    R_KEYS);

	example_release(&example);
}

/*
 * The flow rules around empty inputs.  A scan of an empty view costs
 * nothing and passes nothing on, so a plan that starts with one costs
 * zero.  Work done before a late empty relation is kept: scanning 2000
 * guards and probing an empty relation for each costs 4000, and the EMIT
 * after it adds nothing.  A negated probe of the same empty relation lets
 * every binding through, so the EMIT after it adds 2000.  A lookup into an
 * empty view probes once per binding and visits nothing.
 */
static void
an_empty_view_ends_the_bindings_not_the_work(void)
{
	relic_op_t ops[3];
	relic_cost_step_t steps[3];
	relic_cost_statistic_t entries[1];
	relic_cost_recipe_t recipe;
	relic_cost_catalog_t catalog;
	example_t example;
	uint64_t cost;

	if (!example_build(&example)) {
		check(false, SCENARIO_EMPTY, "the example is built");
		example_release(&example);
		return;
	}
	catalog_entry(entries, 0, example.empty, example.empty_by_first);
	catalog_sort(&catalog, entries, 1);
	recipe.steps = steps;
	recipe.step_count = 3;

	atom_op(&ops[0], RELIC_OP_SCAN, example.empty, RELIC_VIEW_ALL, 0);
	atom_op(&ops[1], RELIC_OP_SCAN, example.guard, RELIC_VIEW_ALL, 0);
	atom_op(&ops[2], RELIC_OP_EMIT, example.r, RELIC_VIEW_ALL, 0);
	step(steps, 0, &ops[0], RELIC_COST_SCAN);
	step(steps, 1, &ops[1], RELIC_COST_SCAN);
	step(steps, 2, &ops[2], RELIC_COST_EMIT);
	cost = relic_cost_estimate(&recipe, &example.env, &catalog);
	checkf(cost == 0, SCENARIO_EMPTY, "a first empty scan costs zero, "
	    "not %llu", (unsigned long long)cost);

	atom_op(&ops[0], RELIC_OP_SCAN, example.guard, RELIC_VIEW_ALL, 0);
	atom_op(&ops[1], RELIC_OP_MEMBER, example.empty, RELIC_VIEW_ALL, 0);
	step(steps, 0, &ops[0], RELIC_COST_SCAN);
	step(steps, 1, &ops[1], RELIC_COST_PROBE);
	cost = relic_cost_estimate(&recipe, &example.env, &catalog);
	checkf(cost == 2 * GUARDS, SCENARIO_EMPTY, "probing an empty "
	    "relation keeps the work before it, %u, not %llu", 2 * GUARDS,
	    (unsigned long long)cost);

	atom_op(&ops[1], RELIC_OP_ANTI_MEMBER, example.empty, RELIC_VIEW_ALL,
	    0);
	step(steps, 1, &ops[1], RELIC_COST_ANTI_PROBE);
	cost = relic_cost_estimate(&recipe, &example.env, &catalog);
	checkf(cost == 3 * GUARDS, SCENARIO_EMPTY, "a negated probe of an "
	    "empty relation passes every binding on, %u, not %llu",
	    3 * GUARDS, (unsigned long long)cost);

	atom_op(&ops[1], RELIC_OP_LOOKUP, example.empty, RELIC_VIEW_ALL,
	    example.empty_by_first);
	step(steps, 1, &ops[1], RELIC_COST_LOOKUP);
	cost = relic_cost_estimate(&recipe, &example.env, &catalog);
	checkf(cost == 2 * GUARDS && entries[0].key_count == 0,
	    SCENARIO_EMPTY, "a lookup into an empty view probes and visits "
	    "nothing, %u, not %llu", 2 * GUARDS, (unsigned long long)cost);

	example_release(&example);
}

/*
 * A fold passes its bindings on whatever it reads, and visits its group's
 * rows as its access says: scanning the whole relation per binding,
 * looking the group up, or probing for one row, none when the relation is
 * empty.  After scanning 2000 guards, a fold over edge costs 2000 folds
 * plus 2000 * 100000 rows scanned, or 2000 * 100000 / 2000 looked up on
 * the sources, or 2000 probes; over the empty relation a probe visits
 * nothing.  The EMIT adds 2000 each time.
 */
static void
a_fold_counts_its_groups_rows_by_its_access(void)
{
	relic_op_t ops[3];
	relic_cost_step_t steps[3];
	relic_cost_statistic_t entries[1];
	relic_cost_recipe_t recipe;
	relic_cost_catalog_t catalog;
	example_t example;
	uint64_t cost;
	uint64_t expected;

	if (!example_build(&example)) {
		check(false, SCENARIO_FOLDS, "the example is built");
		example_release(&example);
		return;
	}
	catalog_entry(entries, 0, example.edge, example.edge_by_source);
	catalog_sort(&catalog, entries, 1);
	recipe.steps = steps;
	recipe.step_count = 3;

	atom_op(&ops[0], RELIC_OP_SCAN, example.guard, RELIC_VIEW_ALL, 0);
	atom_op(&ops[2], RELIC_OP_EMIT, example.r, RELIC_VIEW_ALL, 0);
	step(steps, 0, &ops[0], RELIC_COST_SCAN);
	step(steps, 2, &ops[2], RELIC_COST_EMIT);

	fold_op(&ops[1], example.edge, RELIC_FOLD_SCAN, 0);
	step(steps, 1, &ops[1], RELIC_COST_FOLD_SCAN);
	cost = relic_cost_estimate(&recipe, &example.env, &catalog);
	expected = 3 * (uint64_t)GUARDS + (uint64_t)GUARDS * EDGES;
	checkf(cost == expected, SCENARIO_FOLDS, "a scanning fold costs "
	    "%llu, not %llu", (unsigned long long)expected,
	    (unsigned long long)cost);

	fold_op(&ops[1], example.edge, RELIC_FOLD_LOOKUP,
	    example.edge_by_source);
	step(steps, 1, &ops[1], RELIC_COST_FOLD_LOOKUP);
	cost = relic_cost_estimate(&recipe, &example.env, &catalog);
	expected = 3 * (uint64_t)GUARDS + EDGES;
	checkf(cost == expected, SCENARIO_FOLDS, "a looked-up fold costs "
	    "%llu, not %llu", (unsigned long long)expected,
	    (unsigned long long)cost);

	fold_op(&ops[1], example.edge, RELIC_FOLD_PROBE, 0);
	step(steps, 1, &ops[1], RELIC_COST_FOLD_PROBE);
	cost = relic_cost_estimate(&recipe, &example.env, &catalog);
	checkf(cost == 4 * GUARDS, SCENARIO_FOLDS, "a probing fold costs "
	    "%u, not %llu", 4 * GUARDS, (unsigned long long)cost);

	fold_op(&ops[1], example.empty, RELIC_FOLD_PROBE, 0);
	cost = relic_cost_estimate(&recipe, &example.env, &catalog);
	checkf(cost == 3 * GUARDS, SCENARIO_FOLDS, "a probing fold of an "
	    "empty relation visits nothing, %u, not %llu", 3 * GUARDS,
	    (unsigned long long)cost);

	example_release(&example);
}

/*
 * Work too large to count saturates at UINT64_MAX rather than wrapping
 * round to something small, so two plans that both saturate cost the same
 * and the tie goes as every tie does, to delta-first.  Three scans of a
 * delta of RELIC_TUPLE_MAX - R_KEYS tuples, nested, visit more than 2^64
 * tuples.  A family of one plan chooses it without estimating.
 */
static void
saturated_work_ties_and_ties_go_delta_first(void)
{
	relic_op_t ops[4];
	relic_cost_step_t steps[4];
	relic_cost_recipe_t recipe;
	relic_plan_family_t family;
	relic_cost_catalog_t catalog;
	example_t example;
	uint64_t cost;
	size_t i;

	if (!example_build(&example)) {
		check(false, SCENARIO_SATURATED, "the example is built");
		example_release(&example);
		return;
	}
	bzero(&catalog, sizeof(catalog));
	set_r_delta(&example, R_KEYS, RELIC_TUPLE_MAX);

	for (i = 0; i < 3; i++) {
		atom_op(&ops[i], RELIC_OP_SCAN, example.r, RELIC_VIEW_DELTA,
		    0);
		step(steps, i, &ops[i], RELIC_COST_SCAN);
	}
	atom_op(&ops[3], RELIC_OP_EMIT, example.r, RELIC_VIEW_ALL, 0);
	step(steps, 3, &ops[3], RELIC_COST_EMIT);
	recipe.steps = steps;
	recipe.step_count = 4;

	cost = relic_cost_estimate(&recipe, &example.env, &catalog);
	check(cost == UINT64_MAX, SCENARIO_SATURATED,
	    "three nested scans of four billion tuples saturate");
	bzero(&family, sizeof(family));
	family.candidates[RELIC_FAMILY_DELTA_FIRST].recipe = recipe;
	family.candidates[RELIC_FAMILY_INPUT_FIRST].recipe = recipe;
	family.candidates[RELIC_FAMILY_INPUT_FIRST].recipe.step_count = 3;
	check(relic_cost_estimate(
	    &family.candidates[RELIC_FAMILY_INPUT_FIRST].recipe, &example.env,
	    &catalog) == cost, SCENARIO_SATURATED,
	    "two saturated plans cost the same");
	family.count = 2;
	check(relic_cost_choose(&family, &example.env, &catalog) ==
	    RELIC_FAMILY_DELTA_FIRST, SCENARIO_SATURATED,
	    "a saturated tie chooses delta-first");
	family.count = 1;
	check(relic_cost_choose(&family, &example.env, &catalog) ==
	    RELIC_FAMILY_DELTA_FIRST, SCENARIO_SATURATED,
	    "a family of one plan chooses it");

	example_release(&example);
}

/*
 * A catalog entry starts at zero tuples and zero keys and is counted the
 * first time a recipe reads it.  It stays as counted while its relation
 * holds as many tuples, and is counted again once the relation grows.  The
 * key count is the whole index's whatever view the lookup reads, while the
 * view's own size is what the lookup divides: the old, delta and current
 * views of one relation give three costs over one count.  A later index of
 * the same relation, which may move the array of its indexes, leaves the
 * entry naming its own index by position.
 */
static void
key_counts_follow_the_relations_growth(void)
{
	static const enum relic_view views[] = {
		RELIC_VIEW_OLD, RELIC_VIEW_DELTA, RELIC_VIEW_CURRENT
	};
	static const uint64_t visits[] = { 2, 1, 2 };
	relic_op_t ops[3];
	relic_cost_step_t steps[3];
	relic_cost_statistic_t entries[1];
	relic_cost_recipe_t recipe;
	relic_cost_catalog_t catalog;
	example_t example;
	uint32_t second_column;
	uint64_t cost;
	unsigned int i;

	if (!example_build(&example)) {
		check(false, SCENARIO_STATISTICS, "the example is built");
		example_release(&example);
		return;
	}
	catalog_entry(entries, 0, example.r, example.r_by_first);
	catalog_sort(&catalog, entries, 1);
	recipe.steps = steps;
	recipe.step_count = 3;

	/*
	 * Two bindings from a two-tuple guard view are looked up in r and
	 * emitted: 2 visits, 2 probes and the lookup's Q visits, then Q
	 * emissions.  With r's last tuple as its delta and 10000 keys,
	 * Q = ceil(2 * N / 10000) is 2 for the 9999-tuple old view, 1 for the
	 * one-tuple delta and 2 for the 10000-tuple current view.
	 */
	example.deltas[relic_relation_pred(example.guard) - 1].first = 1;
	example.deltas[relic_relation_pred(example.guard) - 1].end = 3;
	atom_op(&ops[0], RELIC_OP_SCAN, example.guard, RELIC_VIEW_DELTA, 0);
	atom_op(&ops[2], RELIC_OP_EMIT, example.r, RELIC_VIEW_ALL, 0);
	step(steps, 0, &ops[0], RELIC_COST_SCAN);
	step(steps, 2, &ops[2], RELIC_COST_EMIT);
	set_r_delta(&example, R_KEYS, R_KEYS + 1);

	check(entries[0].tuple_count == 0 && entries[0].key_count == 0,
	    SCENARIO_STATISTICS, "an entry starts uncounted at zero");
	for (i = 0; i < 3; i++) {
		atom_op(&ops[1], RELIC_OP_LOOKUP, example.r, views[i],
		    example.r_by_first);
		step(steps, 1, &ops[1], RELIC_COST_LOOKUP);
		cost = relic_cost_estimate(&recipe, &example.env, &catalog);
		checkf(cost == 4 + 2 * visits[i], SCENARIO_STATISTICS,
		    "view %u costs %llu, not %llu", i,
		    (unsigned long long)(4 + 2 * visits[i]),
		    (unsigned long long)cost);
	}
	check(entries[0].tuple_count == R_KEYS &&
	    entries[0].key_count == R_KEYS, SCENARIO_STATISTICS,
	    "the entry is counted once at 10000 tuples and keys");

	entries[0].key_count = 1;
	(void)relic_cost_estimate(&recipe, &example.env, &catalog);
	check(entries[0].key_count == 1, SCENARIO_STATISTICS,
	    "a relation of as many tuples is not counted again");
	entries[0].key_count = R_KEYS;

	if (relic_relation_index(example.ctx, example.r, 2u,
	    &second_column) != 0) {
		check(false, SCENARIO_STATISTICS, "a second index is built");
		example_release(&example);
		return;
	}
	for (i = R_KEYS; i < 2 * R_KEYS; i++) {
		if (!insert_pair(example.ctx, example.r, i, 1)) {
			check(false, SCENARIO_STATISTICS, "r grows");
			example_release(&example);
			return;
		}
	}
	(void)relic_cost_estimate(&recipe, &example.env, &catalog);
	checkf(entries[0].tuple_count == 2 * R_KEYS &&
	    entries[0].key_count == 2 * R_KEYS, SCENARIO_STATISTICS,
	    "after r grows its first-column index is counted again at "
	    "%u keys, not %u", 2 * R_KEYS,
	    (unsigned int)entries[0].key_count);
	check(entries[0].index_position == example.r_by_first,
	    SCENARIO_STATISTICS, "the entry still names its index by "
	    "position");

	example_release(&example);
}

int
main(void)
{
	a_lookups_visits_are_exact_or_saturate();
	a_step_names_the_index_its_operation_reads();
	the_worked_example_costs_what_it_says();
	an_empty_view_ends_the_bindings_not_the_work();
	a_fold_counts_its_groups_rows_by_its_access();
	saturated_work_ties_and_ties_go_delta_first();
	key_counts_follow_the_relations_growth();

	return (check_summary("test_cost"));
}
