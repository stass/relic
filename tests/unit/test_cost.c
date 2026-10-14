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
 * terms of, rather than to whichever plan happens to win.  The candidates
 * are assembled by hand from operations that name those relations and the
 * indexes they read, so each rule is exercised on its own and the worked
 * examples of the design are checked to the unit.
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

#define	SCENARIO_ACQUISITION	"a missing index is charged for building it"
#define	SCENARIO_ARITHMETIC	"a lookup's visits are exact or saturate"
#define	SCENARIO_CHAIN		"chain distances keep delta-first"
#define	SCENARIO_DIRECTIONS	"a small delta and a large one choose apart"
#define	SCENARIO_EMPTY		"an empty view ends the bindings, not the work"
#define	SCENARIO_EXAMPLE	"the worked example costs what it says"
#define	SCENARIO_FOLDS		"a fold counts its group's rows by its access"
#define	SCENARIO_SATURATED	"saturated work ties, and ties go delta-first"
#define	SCENARIO_STATISTICS	"key counts are read from the index as it is"

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

/* The size of the chain distances are measured over. */
#define	CHAIN		4000u

/* Key masks: an index on the first column, and one on the second. */
#define	BY_FIRST	1u
#define	BY_SECOND	2u

/* The most operations and indexes a candidate built here holds. */
#define	CANDIDATE_OPS		6
#define	CANDIDATE_ACCESSES	2

typedef struct example example_t;
typedef struct candidate candidate_t;

/*
 * The relations of the worked example, with indexes on edge's two columns
 * and the first of r and of empty, and the deltas a round would resolve
 * its views from:
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
};

/*
 * A candidate assembled by hand, with room for its operations, the steps
 * of its recipe and the indexes its draft names: `candidate` points into
 * the rest, which candidate_add() and candidate_access() fill in order.
 */
struct candidate {
	relic_plan_candidate_t	candidate;
	relic_op_t		ops[CANDIDATE_OPS];
	relic_cost_step_t	steps[CANDIDATE_OPS];
	relic_plan_access_t	accesses[CANDIDATE_ACCESSES];
};

static bool	 example_build(example_t *example);
static void	 example_release(example_t *example);
static void	 set_r_delta(example_t *example, relic_tuple_id_t first,
		    relic_tuple_id_t end);
static void	 set_delta(example_t *example, const relic_relation_t *rel,
		    relic_tuple_id_t first, relic_tuple_id_t end);
static relic_relation_t *relation(relic_t *ctx, const char *name,
		    uint16_t arity);
static bool	 build_index(relic_t *ctx, relic_relation_t *rel,
		    uint32_t key_mask);
static bool	 insert_pair(relic_t *ctx, relic_relation_t *rel,
		    unsigned int left, unsigned int right);
static relic_dom_t value(relic_t *ctx, unsigned int ordinal);
static void	 candidate_init(candidate_t *built);
static relic_op_t *candidate_add(candidate_t *built,
		    enum relic_op_kind kind, enum relic_cost_kind cost);
static void	 candidate_atom(candidate_t *built, enum relic_op_kind kind,
		    enum relic_cost_kind cost, const relic_relation_t *rel,
		    enum relic_view view, uint32_t key_mask);
static void	 candidate_fold(candidate_t *built,
		    const relic_relation_t *rel, enum relic_fold_access access,
		    enum relic_cost_kind cost, uint32_t key_mask);
static void	 candidate_access(candidate_t *built, relic_relation_t *rel,
		    uint32_t key_mask);
static uint64_t	 estimate(const candidate_t *built,
		    const example_t *example);
static void	 a_lookups_visits_are_exact_or_saturate(void);
static void	 the_worked_example_costs_what_it_says(void);
static void	 an_empty_view_ends_the_bindings_not_the_work(void);
static void	 a_fold_counts_its_groups_rows_by_its_access(void);
static void	 saturated_work_ties_and_ties_go_delta_first(void);
static void	 key_counts_are_read_from_the_index_as_it_is(void);
static void	 a_derived_relation_and_its_base_store_count_apart(void);
static void	 a_missing_index_is_charged_for_building_it(void);
static void	 chain_distances_keep_delta_first(void);

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
 * Have a relation build its index keyed on `key_mask`.  Returns whether it
 * has one.
 */
static bool
build_index(relic_t *ctx, relic_relation_t *rel, uint32_t key_mask)
{
	uint32_t position;

	return (relic_relation_index(ctx, rel, key_mask, &position) == 0);
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

	if (!build_index(example->ctx, example->edge, BY_FIRST) ||
	    !build_index(example->ctx, example->edge, BY_SECOND) ||
	    !build_index(example->ctx, example->r, BY_FIRST) ||
	    !build_index(example->ctx, example->empty, BY_FIRST))
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

/*
 * Make a relation's delta the range [first, end).
 */
static void
set_delta(example_t *example, const relic_relation_t *rel,
    relic_tuple_id_t first, relic_tuple_id_t end)
{
	relic_tuple_range_t *delta;

	delta = &example->deltas[relic_relation_pred(rel) - 1];
	delta->first = first;
	delta->end = end;
}

static void
candidate_init(candidate_t *built)
{
	bzero(built, sizeof(*built));
	built->candidate.draft.ops = built->ops;
	built->candidate.draft.accesses = built->accesses;
	built->candidate.recipe.steps = built->steps;
}

/*
 * Append an operation of `kind` to a candidate, and the recipe step that
 * counts it as `cost`.
 */
static relic_op_t *
candidate_add(candidate_t *built, enum relic_op_kind kind,
    enum relic_cost_kind cost)
{
	relic_plan_draft_t *draft;
	relic_cost_recipe_t *recipe;
	relic_op_t *op;

	draft = &built->candidate.draft;
	recipe = &built->candidate.recipe;

	op = &built->ops[draft->op_count];
	bzero(op, sizeof(*op));
	op->kind = kind;
	bzero(&built->steps[recipe->step_count], sizeof(*built->steps));
	built->steps[recipe->step_count].op = op;
	built->steps[recipe->step_count].kind = cost;
	draft->op_count++;
	recipe->step_count++;

	return (op);
}

static void
candidate_atom(candidate_t *built, enum relic_op_kind kind,
    enum relic_cost_kind cost, const relic_relation_t *rel,
    enum relic_view view, uint32_t key_mask)
{
	relic_op_t *op;

	op = candidate_add(built, kind, cost);
	if (kind == RELIC_OP_EMIT) {
		op->u.emit.arity = rel == NULL ? 0 : relic_relation_arity(rel);
		return;
	}
	op->u.atom.relation = rel;
	op->u.atom.view = view;
	op->u.atom.key_mask = key_mask;
	op->u.atom.arity = rel == NULL ? 0 : relic_relation_arity(rel);
}

static void
candidate_fold(candidate_t *built, const relic_relation_t *rel,
    enum relic_fold_access access, enum relic_cost_kind cost,
    uint32_t key_mask)
{
	relic_op_t *op;

	op = candidate_add(built, RELIC_OP_AGGREGATE, cost);
	op->u.aggregate.relation = rel;
	op->u.aggregate.access = access;
	op->u.aggregate.key_mask = key_mask;
}

/*
 * Name an index in a candidate's draft, as the planner names each index
 * its operations read, once.
 */
static void
candidate_access(candidate_t *built, relic_relation_t *rel,
    uint32_t key_mask)
{
	relic_plan_draft_t *draft;

	draft = &built->candidate.draft;
	built->accesses[draft->access_count].relation = rel;
	built->accesses[draft->access_count].key_mask = key_mask;
	draft->access_count++;
}

static uint64_t
estimate(const candidate_t *built, const example_t *example)
{
	return (relic_cost_estimate(&built->candidate, &example->env));
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
 * delta selects input-first, 20204000 against 32000000.  Every index either
 * reads is built, so neither is charged for building one, and a family of
 * the two chooses as their estimates say.
 */
static void
the_worked_example_costs_what_it_says(void)
{
	relic_plan_family_t family;
	candidate_t delta_first;
	candidate_t input_first;
	example_t example;
	uint64_t first_cost;
	uint64_t input_cost;

	if (!example_build(&example)) {
		check(false, SCENARIO_EXAMPLE, "the example is built");
		example_release(&example);
		return;
	}

	candidate_init(&delta_first);
	candidate_atom(&delta_first, RELIC_OP_SCAN, RELIC_COST_SCAN, example.r,
	    RELIC_VIEW_DELTA, 0);
	candidate_atom(&delta_first, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP,
	    example.edge, RELIC_VIEW_ALL, BY_SECOND);
	candidate_atom(&delta_first, RELIC_OP_MEMBER, RELIC_COST_PROBE,
	    example.guard, RELIC_VIEW_ALL, 0);
	candidate_atom(&delta_first, RELIC_OP_EMIT, RELIC_COST_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	candidate_access(&delta_first, example.edge, BY_SECOND);

	candidate_init(&input_first);
	candidate_atom(&input_first, RELIC_OP_SCAN, RELIC_COST_SCAN,
	    example.guard, RELIC_VIEW_ALL, 0);
	candidate_atom(&input_first, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP,
	    example.edge, RELIC_VIEW_ALL, BY_FIRST);
	candidate_atom(&input_first, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP,
	    example.r, RELIC_VIEW_DELTA, BY_FIRST);
	candidate_atom(&input_first, RELIC_OP_EMIT, RELIC_COST_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	candidate_access(&input_first, example.edge, BY_FIRST);
	candidate_access(&input_first, example.r, BY_FIRST);

	set_r_delta(&example, R_KEYS, R_KEYS + 1);
	first_cost = estimate(&delta_first, &example);
	input_cost = estimate(&input_first, &example);
	checkf(first_cost == 32 && input_cost == 204020, SCENARIO_EXAMPLE,
	    "a one-tuple delta costs 32 delta-first and 204020 input-first, "
	    "not %llu and %llu", (unsigned long long)first_cost,
	    (unsigned long long)input_cost);
	check(first_cost < input_cost, SCENARIO_DIRECTIONS,
	    "a one-tuple delta chooses delta-first");

	set_r_delta(&example, R_KEYS, R_KEYS + LARGE_DELTA);
	first_cost = estimate(&delta_first, &example);
	input_cost = estimate(&input_first, &example);
	checkf(first_cost == 32 * (uint64_t)LARGE_DELTA &&
	    input_cost == 204000 + 20 * (uint64_t)LARGE_DELTA,
	    SCENARIO_EXAMPLE, "a million-tuple delta costs 32000000 "
	    "delta-first and 20204000 input-first, not %llu and %llu",
	    (unsigned long long)first_cost, (unsigned long long)input_cost);
	check(input_cost < first_cost, SCENARIO_DIRECTIONS,
	    "a million-tuple delta chooses input-first");

	bzero(&family, sizeof(family));
	family.candidates[RELIC_FAMILY_DELTA_FIRST] = delta_first.candidate;
	family.candidates[RELIC_FAMILY_INPUT_FIRST] = input_first.candidate;
	family.count = 2;
	check(relic_cost_choose(&family, &example.env) ==
	    RELIC_FAMILY_INPUT_FIRST, SCENARIO_DIRECTIONS,
	    "a family chooses input-first for a million-tuple delta");
	set_r_delta(&example, R_KEYS, R_KEYS + 1);
	check(relic_cost_choose(&family, &example.env) ==
	    RELIC_FAMILY_DELTA_FIRST, SCENARIO_DIRECTIONS,
	    "a family chooses delta-first for a one-tuple delta");

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
	candidate_t built;
	example_t example;
	uint64_t cost;

	if (!example_build(&example)) {
		check(false, SCENARIO_EMPTY, "the example is built");
		example_release(&example);
		return;
	}

	candidate_init(&built);
	candidate_atom(&built, RELIC_OP_SCAN, RELIC_COST_SCAN, example.empty,
	    RELIC_VIEW_ALL, 0);
	candidate_atom(&built, RELIC_OP_SCAN, RELIC_COST_SCAN, example.guard,
	    RELIC_VIEW_ALL, 0);
	candidate_atom(&built, RELIC_OP_EMIT, RELIC_COST_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	cost = estimate(&built, &example);
	checkf(cost == 0, SCENARIO_EMPTY, "a first empty scan costs zero, "
	    "not %llu", (unsigned long long)cost);

	candidate_init(&built);
	candidate_atom(&built, RELIC_OP_SCAN, RELIC_COST_SCAN, example.guard,
	    RELIC_VIEW_ALL, 0);
	candidate_atom(&built, RELIC_OP_MEMBER, RELIC_COST_PROBE, example.empty,
	    RELIC_VIEW_ALL, 0);
	candidate_atom(&built, RELIC_OP_EMIT, RELIC_COST_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	cost = estimate(&built, &example);
	checkf(cost == 2 * GUARDS, SCENARIO_EMPTY, "probing an empty "
	    "relation keeps the work before it, %u, not %llu", 2 * GUARDS,
	    (unsigned long long)cost);

	built.ops[1].kind = RELIC_OP_ANTI_MEMBER;
	built.steps[1].kind = RELIC_COST_ANTI_PROBE;
	cost = estimate(&built, &example);
	checkf(cost == 3 * GUARDS, SCENARIO_EMPTY, "a negated probe of an "
	    "empty relation passes every binding on, %u, not %llu",
	    3 * GUARDS, (unsigned long long)cost);

	built.ops[1].kind = RELIC_OP_LOOKUP;
	built.ops[1].u.atom.key_mask = BY_FIRST;
	built.steps[1].kind = RELIC_COST_LOOKUP;
	candidate_access(&built, example.empty, BY_FIRST);
	cost = estimate(&built, &example);
	checkf(cost == 2 * GUARDS, SCENARIO_EMPTY, "a lookup into an empty "
	    "view probes and visits nothing, %u, not %llu", 2 * GUARDS,
	    (unsigned long long)cost);

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
	candidate_t built;
	example_t example;
	relic_op_t *fold;
	uint64_t cost;
	uint64_t expected;

	if (!example_build(&example)) {
		check(false, SCENARIO_FOLDS, "the example is built");
		example_release(&example);
		return;
	}

	candidate_init(&built);
	candidate_atom(&built, RELIC_OP_SCAN, RELIC_COST_SCAN, example.guard,
	    RELIC_VIEW_ALL, 0);
	candidate_fold(&built, example.edge, RELIC_FOLD_SCAN,
	    RELIC_COST_FOLD_SCAN, 0);
	candidate_atom(&built, RELIC_OP_EMIT, RELIC_COST_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	fold = &built.ops[1];
	cost = estimate(&built, &example);
	expected = 3 * (uint64_t)GUARDS + (uint64_t)GUARDS * EDGES;
	checkf(cost == expected, SCENARIO_FOLDS, "a scanning fold costs "
	    "%llu, not %llu", (unsigned long long)expected,
	    (unsigned long long)cost);

	fold->u.aggregate.access = RELIC_FOLD_LOOKUP;
	fold->u.aggregate.key_mask = BY_FIRST;
	built.steps[1].kind = RELIC_COST_FOLD_LOOKUP;
	candidate_access(&built, example.edge, BY_FIRST);
	cost = estimate(&built, &example);
	expected = 3 * (uint64_t)GUARDS + EDGES;
	checkf(cost == expected, SCENARIO_FOLDS, "a looked-up fold costs "
	    "%llu, not %llu", (unsigned long long)expected,
	    (unsigned long long)cost);

	built.candidate.draft.access_count = 0;
	fold->u.aggregate.access = RELIC_FOLD_PROBE;
	built.steps[1].kind = RELIC_COST_FOLD_PROBE;
	cost = estimate(&built, &example);
	checkf(cost == 4 * GUARDS, SCENARIO_FOLDS, "a probing fold costs "
	    "%u, not %llu", 4 * GUARDS, (unsigned long long)cost);

	fold->u.aggregate.relation = example.empty;
	cost = estimate(&built, &example);
	checkf(cost == 3 * GUARDS, SCENARIO_FOLDS, "a probing fold of an "
	    "empty relation visits nothing, %u, not %llu", 3 * GUARDS,
	    (unsigned long long)cost);

	fold->u.aggregate.relation = example.edge;
	fold->u.aggregate.access = RELIC_FOLD_CARDINALITY;
	fold->u.aggregate.key_mask = 0;
	built.steps[1].kind = RELIC_COST_OPERATION;
	cost = estimate(&built, &example);
	checkf(cost == 3 * GUARDS,
	    SCENARIO_FOLDS, "a cardinality fold costs one operation per "
	    "binding and visits no row, %u, not %llu", 3 * GUARDS,
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
	relic_plan_family_t family;
	candidate_t built;
	example_t example;
	uint64_t cost;
	size_t i;

	if (!example_build(&example)) {
		check(false, SCENARIO_SATURATED, "the example is built");
		example_release(&example);
		return;
	}
	set_r_delta(&example, R_KEYS, RELIC_TUPLE_MAX);

	candidate_init(&built);
	for (i = 0; i < 3; i++)
		candidate_atom(&built, RELIC_OP_SCAN, RELIC_COST_SCAN,
		    example.r, RELIC_VIEW_DELTA, 0);
	candidate_atom(&built, RELIC_OP_EMIT, RELIC_COST_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);

	cost = estimate(&built, &example);
	check(cost == UINT64_MAX, SCENARIO_SATURATED,
	    "three nested scans of four billion tuples saturate");
	bzero(&family, sizeof(family));
	family.candidates[RELIC_FAMILY_DELTA_FIRST] = built.candidate;
	family.candidates[RELIC_FAMILY_INPUT_FIRST] = built.candidate;
	family.candidates[RELIC_FAMILY_INPUT_FIRST].recipe.step_count = 3;
	check(relic_cost_estimate(
	    &family.candidates[RELIC_FAMILY_INPUT_FIRST], &example.env) ==
	    cost, SCENARIO_SATURATED, "two saturated plans cost the same");
	family.count = 2;
	check(relic_cost_choose(&family, &example.env) ==
	    RELIC_FAMILY_DELTA_FIRST, SCENARIO_SATURATED,
	    "a saturated tie chooses delta-first");
	family.count = 1;
	check(relic_cost_choose(&family, &example.env) ==
	    RELIC_FAMILY_DELTA_FIRST, SCENARIO_SATURATED,
	    "a family of one plan chooses it");

	example_release(&example);
}

/*
 * A lookup's keys are the whole index's, read from the index each time it
 * is estimated, whatever view the lookup reads, while the view's own size
 * is what the lookup divides: the old, delta and current views of one
 * relation give three costs over one count.  Estimating twice gives the
 * same answer, and the next estimate after the relation grows reads the
 * keys it grew to, with nothing to refresh; a later index of the relation
 * changes neither.
 */
static void
key_counts_are_read_from_the_index_as_it_is(void)
{
	static const enum relic_view views[] = {
		RELIC_VIEW_OLD, RELIC_VIEW_DELTA, RELIC_VIEW_CURRENT
	};
	static const uint64_t visits[] = { 2, 1, 2 };
	candidate_t built;
	example_t example;
	uint64_t cost;
	unsigned int i;

	if (!example_build(&example)) {
		check(false, SCENARIO_STATISTICS, "the example is built");
		example_release(&example);
		return;
	}

	/*
	 * Two bindings from a two-tuple guard view are looked up in r and
	 * emitted: 2 visits, 2 probes and the lookup's Q visits, then Q
	 * emissions.  With r's last tuple as its delta and 10000 keys,
	 * Q = ceil(2 * N / 10000) is 2 for the 9999-tuple old view, 1 for the
	 * one-tuple delta and 2 for the 10000-tuple current view.
	 */
	set_delta(&example, example.guard, 1, 3);
	set_r_delta(&example, R_KEYS, R_KEYS + 1);
	candidate_init(&built);
	candidate_atom(&built, RELIC_OP_SCAN, RELIC_COST_SCAN, example.guard,
	    RELIC_VIEW_DELTA, 0);
	candidate_atom(&built, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP, example.r,
	    RELIC_VIEW_OLD, BY_FIRST);
	candidate_atom(&built, RELIC_OP_EMIT, RELIC_COST_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	candidate_access(&built, example.r, BY_FIRST);

	for (i = 0; i < 3; i++) {
		built.ops[1].u.atom.view = views[i];
		cost = estimate(&built, &example);
		checkf(cost == 4 + 2 * visits[i], SCENARIO_STATISTICS,
		    "view %u costs %llu, not %llu", i,
		    (unsigned long long)(4 + 2 * visits[i]),
		    (unsigned long long)cost);
		check(estimate(&built, &example) == cost, SCENARIO_STATISTICS,
		    "estimating again gives the same cost");
	}

	if (!build_index(example.ctx, example.r, BY_SECOND)) {
		check(false, SCENARIO_STATISTICS, "a second index is built");
		example_release(&example);
		return;
	}
	checkf(estimate(&built, &example) == cost, SCENARIO_STATISTICS,
	    "a second index of r leaves the current view's cost at %llu",
	    (unsigned long long)cost);
	for (i = R_KEYS; i < 2 * R_KEYS; i++) {
		if (!insert_pair(example.ctx, example.r, i, 1)) {
			check(false, SCENARIO_STATISTICS, "r grows");
			example_release(&example);
			return;
		}
	}

	/*
	 * r now holds 20000 tuples over 20000 first-column keys, and its
	 * current view is all of them: Q = ceil(2 * 20000 / 20000) = 2.  Had
	 * the count stayed at 10000, the view would have cost four visits.
	 */
	built.ops[1].u.atom.view = RELIC_VIEW_CURRENT;
	set_r_delta(&example, 2 * R_KEYS, 2 * R_KEYS + 1);
	cost = estimate(&built, &example);
	checkf(cost == 4 + 2 * 2, SCENARIO_STATISTICS, "after r grows to "
	    "20000 keys its current view costs %u, not %llu", 4 + 2 * 2,
	    (unsigned long long)cost);

	example_release(&example);
}

/*
 * A predicate can have two relations, its derived relation and the base
 * store a base view of it reads, and an operation reads one of them.  Each
 * is counted from its own indexes: the base store's ten tuples have ten
 * first-column keys and its index is built, the derived relation's ten
 * share one key and its index is not, so a lookup of one binding visits
 * one tuple of the first and, through the key per tuple a missing index is
 * taken to hold, one of the second, which is charged twenty for its build.
 * Once built, the derived relation's index counts its one key, and the
 * lookup visits all ten.
 */
static void
a_derived_relation_and_its_base_store_count_apart(void)
{
	relic_location_t where;
	relic_relation_t *base;
	relic_relation_t *derived;
	candidate_t built;
	example_t example;
	relic_pred_id_t pred;
	uint64_t base_cost;
	uint64_t derived_cost;
	unsigned int i;

	if (!example_build(&example)) {
		check(false, SCENARIO_STATISTICS, "the example is built");
		example_release(&example);
		return;
	}

	bzero(&where, sizeof(where));
	where.file = "test_cost.c";
	where.line = 1;
	where.column = 1;
	base = NULL;
	if (relic_pred_intern(example.ctx, "p", 1, 2, &where, &pred) == 0)
		base = relic_relation_read(example.ctx, pred);
	derived = base == NULL ? NULL : relation(example.ctx, "p", 2);
	if (base == NULL || derived == NULL || base == derived) {
		check(false, SCENARIO_STATISTICS, "a predicate has a base "
		    "store and a derived relation");
		example_release(&example);
		return;
	}
	for (i = 0; i < 10; i++) {
		if (!insert_pair(example.ctx, base, i, 0) ||
		    !insert_pair(example.ctx, derived, 0, i)) {
			check(false, SCENARIO_STATISTICS, "p's relations fill");
			example_release(&example);
			return;
		}
	}
	if (!build_index(example.ctx, base, BY_FIRST)) {
		check(false, SCENARIO_STATISTICS, "the base store is indexed");
		example_release(&example);
		return;
	}

	/* One binding, a lookup and an emission. */
	set_delta(&example, example.guard, 1, 2);
	candidate_init(&built);
	candidate_atom(&built, RELIC_OP_SCAN, RELIC_COST_SCAN, example.guard,
	    RELIC_VIEW_DELTA, 0);
	candidate_atom(&built, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP, base,
	    RELIC_VIEW_ALL, BY_FIRST);
	candidate_atom(&built, RELIC_OP_EMIT, RELIC_COST_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	candidate_access(&built, base, BY_FIRST);
	base_cost = estimate(&built, &example);

	built.ops[1].u.atom.relation = derived;
	built.accesses[0].relation = derived;
	derived_cost = estimate(&built, &example);
	checkf(base_cost == 4 && derived_cost == 4 + 2 * 10,
	    SCENARIO_STATISTICS, "the base store's lookup costs 4 and the "
	    "derived relation's 24, not %llu and %llu",
	    (unsigned long long)base_cost, (unsigned long long)derived_cost);

	if (!build_index(example.ctx, derived, BY_FIRST)) {
		check(false, SCENARIO_STATISTICS, "the derived relation is "
		    "indexed");
		example_release(&example);
		return;
	}
	derived_cost = estimate(&built, &example);
	checkf(derived_cost == 1 + (1 + 10) + 10, SCENARIO_STATISTICS,
	    "the derived relation's built index has one key, so its lookup "
	    "costs 22, not %llu", (unsigned long long)derived_cost);

	example_release(&example);
}

/*
 * An index the draft names and its relation lacks is charged two units per
 * tuple of the whole relation, once however many operations read it and
 * whether or not the walk reaches them, and its lookups are estimated as
 * though every tuple had a key of its own.  r has 10000 tuples, all with 0
 * in the second column, and no index on it: two bindings looked up on it
 * through its 10000-tuple current view visit ceil(2 * 10000 / 10000) = 2
 * tuples, so the plan costs 2 + (2 + 2) + 2 = 8 and 20000 for the build.
 * A second lookup through the same index adds its own work and no second
 * build.  Behind an empty scan nothing runs, and the build is still
 * charged.  Over the empty relation a build costs nothing.  Two missing
 * indexes are each charged: a further lookup of the two bindings in a
 * five-tuple relation, taken to have five keys, adds 2 + 2 and a build of
 * 10.  Once the index
 * is built it is charged nothing and counts its one key: the lookup then
 * visits every tuple, 2 + (2 + 20000) + 20000.
 */
static void
a_missing_index_is_charged_for_building_it(void)
{
	relic_relation_t *small;
	candidate_t built;
	example_t example;
	uint64_t cost;
	unsigned int i;

	if (!example_build(&example)) {
		check(false, SCENARIO_ACQUISITION, "the example is built");
		example_release(&example);
		return;
	}

	set_delta(&example, example.guard, 1, 3);
	candidate_init(&built);
	candidate_atom(&built, RELIC_OP_SCAN, RELIC_COST_SCAN, example.guard,
	    RELIC_VIEW_DELTA, 0);
	candidate_atom(&built, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP, example.r,
	    RELIC_VIEW_CURRENT, BY_SECOND);
	candidate_atom(&built, RELIC_OP_EMIT, RELIC_COST_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	candidate_access(&built, example.r, BY_SECOND);
	cost = estimate(&built, &example);
	checkf(cost == 8 + 2 * (uint64_t)R_KEYS, SCENARIO_ACQUISITION,
	    "a lookup through a missing index costs %llu, not %llu",
	    (unsigned long long)(8 + 2 * (uint64_t)R_KEYS),
	    (unsigned long long)cost);

	candidate_init(&built);
	candidate_atom(&built, RELIC_OP_SCAN, RELIC_COST_SCAN, example.guard,
	    RELIC_VIEW_DELTA, 0);
	candidate_atom(&built, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP, example.r,
	    RELIC_VIEW_CURRENT, BY_SECOND);
	candidate_atom(&built, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP, example.r,
	    RELIC_VIEW_CURRENT, BY_SECOND);
	candidate_atom(&built, RELIC_OP_EMIT, RELIC_COST_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	candidate_access(&built, example.r, BY_SECOND);
	cost = estimate(&built, &example);
	checkf(cost == 12 + 2 * (uint64_t)R_KEYS, SCENARIO_ACQUISITION,
	    "two lookups through one missing index cost %llu, not %llu",
	    (unsigned long long)(12 + 2 * (uint64_t)R_KEYS),
	    (unsigned long long)cost);

	built.ops[0].u.atom.relation = example.empty;
	built.ops[0].u.atom.view = RELIC_VIEW_ALL;
	cost = estimate(&built, &example);
	checkf(cost == 2 * (uint64_t)R_KEYS, SCENARIO_ACQUISITION,
	    "a build behind an empty scan is charged, %llu, not %llu",
	    (unsigned long long)(2 * (uint64_t)R_KEYS),
	    (unsigned long long)cost);

	built.accesses[0].relation = example.empty;
	built.ops[1].u.atom.relation = example.empty;
	built.ops[2].u.atom.relation = example.empty;
	cost = estimate(&built, &example);
	checkf(cost == 0, SCENARIO_ACQUISITION, "building an index of the "
	    "empty relation costs nothing, not %llu",
	    (unsigned long long)cost);

	small = relation(example.ctx, "small", 2);
	for (i = 0; small != NULL && i < 5; i++) {
		if (!insert_pair(example.ctx, small, i, 0))
			small = NULL;
	}
	if (small == NULL) {
		check(false, SCENARIO_ACQUISITION, "a second relation fills");
		example_release(&example);
		return;
	}
	candidate_init(&built);
	candidate_atom(&built, RELIC_OP_SCAN, RELIC_COST_SCAN, example.guard,
	    RELIC_VIEW_DELTA, 0);
	candidate_atom(&built, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP, example.r,
	    RELIC_VIEW_CURRENT, BY_SECOND);
	candidate_atom(&built, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP, small,
	    RELIC_VIEW_ALL, BY_FIRST);
	candidate_atom(&built, RELIC_OP_EMIT, RELIC_COST_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	candidate_access(&built, example.r, BY_SECOND);
	candidate_access(&built, small, BY_FIRST);
	cost = estimate(&built, &example);
	checkf(cost == 12 + 2 * (uint64_t)R_KEYS + 2 * 5,
	    SCENARIO_ACQUISITION, "two missing indexes are each charged, "
	    "%llu, not %llu",
	    (unsigned long long)(12 + 2 * (uint64_t)R_KEYS + 2 * 5),
	    (unsigned long long)cost);

	candidate_init(&built);
	candidate_atom(&built, RELIC_OP_SCAN, RELIC_COST_SCAN, example.guard,
	    RELIC_VIEW_DELTA, 0);
	candidate_atom(&built, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP, example.r,
	    RELIC_VIEW_CURRENT, BY_SECOND);
	candidate_atom(&built, RELIC_OP_EMIT, RELIC_COST_EMIT, example.r,
	    RELIC_VIEW_ALL, 0);
	candidate_access(&built, example.r, BY_SECOND);
	if (!build_index(example.ctx, example.r, BY_SECOND)) {
		check(false, SCENARIO_ACQUISITION, "the index is built");
		example_release(&example);
		return;
	}
	cost = estimate(&built, &example);
	checkf(cost == 2 + (2 + 2 * (uint64_t)R_KEYS) + 2 * (uint64_t)R_KEYS,
	    SCENARIO_ACQUISITION, "a built index costs its lookups from its "
	    "one key, %llu, not %llu",
	    (unsigned long long)(2 + (2 + 2 * (uint64_t)R_KEYS) +
	    2 * (uint64_t)R_KEYS), (unsigned long long)cost);

	example_release(&example);
}

/*
 * The design's chain distances, the variant of
 *
 *	dist(X, Z, E) :- dist(X, Y, D), edge(Y, Z), D < 1000000, E = D + 1.
 *
 * reading dist's delta, over a chain of E edges, one per source, a dist of
 * T tuples and a delta of D of them, with no index of dist on its second
 * column.  Delta-first scans the delta, filters, assigns, looks up edge on
 * its source and emits, 6D; input-first scans edge, looks the delta up on
 * Y, filters, assigns and emits, 2E + 4 * ceil(E * D / T), and builds the
 * index, 2T.  At E = D = T = 4000 that is 24000 against 32000, and the
 * family keeps delta-first, leaving the index unbuilt; a smaller delta
 * widens the gap.
 */
static void
chain_distances_keep_delta_first(void)
{
	relic_plan_family_t family;
	relic_relation_t *dist;
	relic_relation_t *edge;
	relic_tuple_range_t *deltas;
	relic_tuple_range_t all;
	relic_exec_env_t env;
	candidate_t delta_first;
	candidate_t input_first;
	relic_dom_t tuple[3];
	relic_t *ctx;
	uint64_t first_cost;
	uint64_t input_cost;
	unsigned int i;
	bool added;
	bool built;

	ctx = relic_create();
	edge = ctx == NULL ? NULL : relation(ctx, "edge", 2);
	dist = edge == NULL ? NULL : relation(ctx, "dist", 3);
	deltas = dist == NULL ? NULL :
	    calloc(relic_pred_count(ctx), sizeof(*deltas));
	built = deltas != NULL;
	for (i = 0; built && i < CHAIN; i++) {
		tuple[0] = value(ctx, i);
		tuple[1] = value(ctx, i + 1);
		tuple[2] = value(ctx, 1);
		built = insert_pair(ctx, edge, i, i + 1);
		if (built)
			built = relic_relation_insert(ctx, dist, tuple,
			    &added) == 0 && added;
	}
	if (!built || !build_index(ctx, edge, BY_FIRST)) {
		check(false, SCENARIO_CHAIN, "the chain is built");
		free(deltas);
		relic_destroy(ctx);
		return;
	}
	bzero(&env, sizeof(env));
	env.ctx = ctx;
	env.deltas = deltas;
	all = relic_relation_all(dist);
	deltas[relic_relation_pred(dist) - 1] = all;

	candidate_init(&delta_first);
	candidate_atom(&delta_first, RELIC_OP_SCAN, RELIC_COST_SCAN, dist,
	    RELIC_VIEW_DELTA, 0);
	candidate_add(&delta_first, RELIC_OP_FILTER, RELIC_COST_OPERATION);
	candidate_add(&delta_first, RELIC_OP_ASSIGN, RELIC_COST_OPERATION);
	candidate_atom(&delta_first, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP, edge,
	    RELIC_VIEW_ALL, BY_FIRST);
	candidate_atom(&delta_first, RELIC_OP_EMIT, RELIC_COST_EMIT, dist,
	    RELIC_VIEW_ALL, 0);
	candidate_access(&delta_first, edge, BY_FIRST);

	candidate_init(&input_first);
	candidate_atom(&input_first, RELIC_OP_SCAN, RELIC_COST_SCAN, edge,
	    RELIC_VIEW_ALL, 0);
	candidate_atom(&input_first, RELIC_OP_LOOKUP, RELIC_COST_LOOKUP, dist,
	    RELIC_VIEW_DELTA, BY_SECOND);
	candidate_add(&input_first, RELIC_OP_FILTER, RELIC_COST_OPERATION);
	candidate_add(&input_first, RELIC_OP_ASSIGN, RELIC_COST_OPERATION);
	candidate_atom(&input_first, RELIC_OP_EMIT, RELIC_COST_EMIT, dist,
	    RELIC_VIEW_ALL, 0);
	candidate_access(&input_first, dist, BY_SECOND);

	bzero(&family, sizeof(family));
	family.candidates[RELIC_FAMILY_DELTA_FIRST] = delta_first.candidate;
	family.candidates[RELIC_FAMILY_INPUT_FIRST] = input_first.candidate;
	family.count = 2;

	first_cost = relic_cost_estimate(&delta_first.candidate, &env);
	input_cost = relic_cost_estimate(&input_first.candidate, &env);
	checkf(first_cost == 6 * (uint64_t)CHAIN &&
	    input_cost == 8 * (uint64_t)CHAIN, SCENARIO_CHAIN,
	    "the whole chain as its delta costs 24000 delta-first and 32000 "
	    "input-first, not %llu and %llu", (unsigned long long)first_cost,
	    (unsigned long long)input_cost);
	check(relic_cost_choose(&family, &env) == RELIC_FAMILY_DELTA_FIRST,
	    SCENARIO_CHAIN, "the whole chain as its delta chooses "
	    "delta-first");

	deltas[relic_relation_pred(dist) - 1].first = all.end - 1;
	first_cost = relic_cost_estimate(&delta_first.candidate, &env);
	input_cost = relic_cost_estimate(&input_first.candidate, &env);
	checkf(first_cost == 6 && input_cost == 4 * (uint64_t)CHAIN + 4,
	    SCENARIO_CHAIN, "a one-tuple delta costs 6 delta-first and "
	    "16004 input-first, not %llu and %llu",
	    (unsigned long long)first_cost, (unsigned long long)input_cost);
	check(relic_cost_choose(&family, &env) == RELIC_FAMILY_DELTA_FIRST,
	    SCENARIO_CHAIN, "a one-tuple delta chooses delta-first");

	free(deltas);
	relic_destroy(ctx);
}

int
main(void)
{
	a_lookups_visits_are_exact_or_saturate();
	the_worked_example_costs_what_it_says();
	an_empty_view_ends_the_bindings_not_the_work();
	a_fold_counts_its_groups_rows_by_its_access();
	saturated_work_ties_and_ties_go_delta_first();
	key_counts_are_read_from_the_index_as_it_is();
	a_derived_relation_and_its_base_store_count_apart();
	a_missing_index_is_charged_for_building_it();
	chain_distances_keep_delta_first();

	return (check_summary("test_cost"));
}
