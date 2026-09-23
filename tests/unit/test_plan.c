/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the planner.
 *
 * A plan is invisible from the command line: the golden tests see only the
 * answers it produces, and a plan that scanned where it could have tested
 * membership, or placed a comparison late, would produce the same answers
 * more slowly.  This suite examines the plans themselves -- which operation
 * each literal became, what each column does, and where each test went --
 * because those are the decisions every later planner inherits.
 *
 * The first scenarios examine the placement rules every planner shares --
 * how a column is decided, where a test goes, what a delta variant starts
 * with -- under the textual reference planner of -X no-index and -X
 * no-reorder, so that the order they check is the order the rule is written
 * in.  The later ones examine what the default planner chooses: the access
 * path of each binding pattern, and the order its estimates give.
 */

#include "config.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_ACCESS		"each binding pattern gets its access path"
#define	SCENARIO_COMPARISONS	"a comparison goes where its operands are bound"
#define	SCENARIO_DELTA		"a delta variant scans its delta literal first"
#define	SCENARIO_DELTA_REFUSED	"a delta literal must be a positive literal"
#define	SCENARIO_EMIT_TARGET	"the EMIT writes the relation it is given"
#define	SCENARIO_ESTIMATE	"the smaller estimate goes first"
#define	SCENARIO_FACT		"a fact is a lone EMIT of constants"
#define	SCENARIO_GREEDY		"bound arguments go first, then small relations"
#define	SCENARIO_INCOMPLETE	"a relation being derived is assumed, not measured"
#define	SCENARIO_MEMBER		"a fully bound literal is a membership test"
#define	SCENARIO_NEGATION	"a negated literal is an ANTI_MEMBER, placed early"
#define	SCENARIO_RANKING	"passes choose the step-by-step greedy order"
#define	SCENARIO_REFERENCES	"the reference switches change only their choice"
#define	SCENARIO_TERMS		"a scan's columns bind, match or ignore"

/* The switches that make the default planner the textual one. */
#define	TEXTUAL_FLAGS		(RELIC_F_NO_INDEX | RELIC_F_NO_REORDER)

/* How many tuples half/2 holds in the divided-assumption scenario. */
#define	HALF_TUPLES		500

/*
 * The shape of the programs the ranking scenario generates: how many, the
 * relations their bodies read, the variables a rule may use, the most
 * tuples a relation holds, the values a column is drawn from, and the most
 * positive literals and tests a body holds.
 */
#define	RANKED_PROGRAMS		60
#define	RANKED_RULES		4
#define	RANKED_RELATIONS	5
#define	RANKED_VARIABLES	8
#define	RANKED_MAX_TUPLES	24
#define	RANKED_MAX_VALUES	8
#define	RANKED_MAX_POSITIVES	24
#define	RANKED_MAX_TESTS	6
#define	RANKED_TEXT_MAX		16384

typedef struct fixture fixture_t;
typedef struct text text_t;

/*
 * A context holding one program, and the plan of one of its rules.  The plan
 * lives in `arena`, which the fixture owns so that a test can inspect the
 * plan after planning and release both together.
 */
struct fixture {
	relic_t		*ctx;
	relic_arena_t	 arena;
	relic_plan_t	 plan;
	size_t		 errors;
};

/*
 * Program text under construction, in a fixed buffer.  `overflowed` records
 * that something did not fit, which fails the scenario rather than testing a
 * truncated program.
 */
struct text {
	char	buffer[RANKED_TEXT_MAX];
	size_t	length;
	bool	overflowed;
};

static void	 count_error(void *callback_ctx, enum relic_diag_level level,
		    const relic_location_t *where, const char *message);
static bool	 fixture_load(fixture_t *fixture, const char *program,
		    unsigned int flags, bool evaluate);
static void	 fixture_release(fixture_t *fixture);
static bool	 fixture_plan(fixture_t *fixture, const char *program,
		    size_t rule_index);
static bool	 fixture_plan_rule(fixture_t *fixture, size_t rule_index,
		    size_t delta_literal, const bool *incomplete);
static bool	 fixture_plan_variant(fixture_t *fixture, const char *program,
		    size_t rule_index, size_t delta_literal);
static bool	 kinds_are(const relic_plan_t *plan,
		    const enum relic_op_kind *expected, size_t count);
static bool	 reads_in_order(const fixture_t *fixture,
		    const char *const *expected, size_t count);
static uint16_t	 slot_named(const fixture_t *fixture, size_t rule_index,
		    const char *name);
static relic_dom_t symbol_value(relic_t *ctx, const char *name);
static uint32_t	 next_random(uint32_t *state, uint32_t bound);
static void	 text_append(text_t *text, const char *format, ...)
		    RELIC_PRINTFLIKE(2, 3);
static void	 generate_facts(text_t *text, uint32_t *state);
static void	 generate_rule(text_t *text, uint32_t *state, size_t number);
static void	 generate_positive(text_t *text, uint32_t *state,
		    uint32_t *variables);
static void	 generate_test(text_t *text, uint32_t *state,
		    uint32_t variables);
static void	 generate_variable(text_t *text, uint32_t *state,
		    uint32_t variables);
static uint16_t	 relation_arity(size_t relation);
static bool	 reference_order(relic_t *ctx, const relic_rule_t *rule,
		    uint64_t bound, const bool *incomplete, size_t *order);
static bool	 reference_score(relic_t *ctx, const relic_atom_t *atom,
		    uint64_t bound, const bool *incomplete,
		    uint16_t *bound_argsp, size_t *estimatep);
static bool	 reference_better(uint16_t bound_args, size_t estimate,
		    uint16_t best_bound_args, size_t best_estimate);
static size_t	 reference_place_tests(relic_t *ctx, const relic_rule_t *rule,
		    uint64_t bound, bool *placed, size_t *order, size_t count);
static bool	 reference_test_ready(relic_t *ctx,
		    const relic_literal_t *literal, uint64_t bound);
static uint64_t	 reference_slots(const relic_arg_t *args, uint16_t count);
static void	 check_ranked_program(uint32_t *state, unsigned int flags);
static void	 a_comparison_goes_where_its_operands_are_bound(void);
static void	 a_delta_literal_must_be_positive(void);
static void	 a_delta_variant_scans_its_delta_first(void);
static void	 the_emit_writes_the_relation_it_is_given(void);
static void	 a_fact_is_a_lone_emit_of_constants(void);
static void	 a_fully_bound_literal_is_a_membership_test(void);
static void	 a_negated_literal_is_placed_once_bound(void);
static void	 a_relation_being_derived_is_assumed(void);
static void	 an_assumed_relation_divides_per_bound_column(void);
static void	 a_scans_columns_bind_match_or_ignore(void);
static void	 a_bound_argument_outranks_a_smaller_estimate(void);
static void	 bound_arguments_then_small_relations_go_first(void);
static void	 each_binding_pattern_gets_its_access_path(void);
static void	 the_references_change_only_their_choice(void);
static void	 the_ranking_is_the_step_by_step_order(void);
static void	 the_smaller_estimate_goes_first(void);

static void
count_error(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_UNUSED,
    const char *message RELIC_UNUSED)
{
	fixture_t *fixture;

	fixture = callback_ctx;
	if (level == RELIC_ERROR)
		fixture->errors++;
}

/*
 * Create a context with `flags`, load a program into it and check it, and
 * when `evaluate` is set materialize it, so that its relations hold what
 * the program derives and the planner has something to measure.  Returns
 * whether all of that succeeded.  The fixture is released either way.
 */
static bool
fixture_load(fixture_t *fixture, const char *program, unsigned int flags,
    bool evaluate)
{
	relic_config_t config;

	bzero(fixture, sizeof(*fixture));
	relic_arena_init(&fixture->arena);

	bzero(&config, sizeof(config));
	config.flags = flags;

	fixture->ctx = relic_create_config(&config);
	if (fixture->ctx == NULL)
		return (false);
	relic_diag_set(fixture->ctx, count_error, fixture);

	if (relic_load_string(fixture->ctx, "test.dl", program) != 0 ||
	    relic_check(fixture->ctx) != 0)
		return (false);

	return (!evaluate || relic_materialize(fixture->ctx) == 0);
}

/*
 * Plan the rule at `rule_index` in load order, or its delta variant for the
 * body literal at `delta_literal`, into the relation of its head, with the
 * relations `incomplete` marks taken to be still derived.  Returns whether
 * the plan was built; a test that expects a refusal reads the error count.
 */
static bool
fixture_plan_rule(fixture_t *fixture, size_t rule_index,
    size_t delta_literal, const bool *incomplete)
{
	const relic_rule_t *rule;
	relic_relation_t *target;

	if (rule_index >= fixture->ctx->program.rules.count)
		return (false);

	rule = &fixture->ctx->program.rules.entries[rule_index];
	target = relic_relation_for(fixture->ctx, rule->head);
	if (target == NULL)
		return (false);

	return (relic_plan_rule(fixture->ctx, &fixture->arena, rule, target,
	    delta_literal, incomplete, &fixture->plan) == 0);
}

/*
 * Load a program without evaluating it and plan one of its rules, or one
 * delta variant of it, with the textual planner.
 */
static bool
fixture_plan_variant(fixture_t *fixture, const char *program,
    size_t rule_index, size_t delta_literal)
{
	return (fixture_load(fixture, program, TEXTUAL_FLAGS, false) &&
	    fixture_plan_rule(fixture, rule_index, delta_literal, NULL));
}

static bool
fixture_plan(fixture_t *fixture, const char *program, size_t rule_index)
{
	return (fixture_plan_variant(fixture, program, rule_index,
	    RELIC_PLAN_NO_DELTA));
}

static void
fixture_release(fixture_t *fixture)
{
	relic_arena_reset(&fixture->arena);
	relic_destroy(fixture->ctx);
}

/*
 * Whether a plan's operations are exactly these kinds, in this order.
 */
static bool
kinds_are(const relic_plan_t *plan, const enum relic_op_kind *expected,
    size_t count)
{
	size_t i;

	if (plan->op_count != count)
		return (false);

	for (i = 0; i < count; i++) {
		if (plan->ops[i].kind != expected[i])
			return (false);
	}

	return (true);
}

/*
 * Whether the plan's operations over relations read these predicates, by
 * name, in this order, ignoring the filters between them.  The EMIT counts,
 * so the last name is the head's.
 */
static bool
reads_in_order(const fixture_t *fixture, const char *const *expected,
    size_t count)
{
	const relic_op_t *op;
	const char *name;
	size_t matched;
	size_t i;

	matched = 0;
	for (i = 0; i < fixture->plan.op_count; i++) {
		op = &fixture->plan.ops[i];
		if (op->kind == RELIC_OP_FILTER)
			continue;

		name = relic_symbol_bytes(fixture->ctx, relic_pred_of(
		    fixture->ctx, op->u.atom.relation->pred)->name, NULL);
		if (matched == count || strcmp(name, expected[matched]) != 0)
			return (false);
		matched++;
	}

	return (matched == count);
}

/*
 * The slot a named variable of a rule was numbered into, or UINT16_MAX when
 * the rule has no such variable.
 */
static uint16_t
slot_named(const fixture_t *fixture, size_t rule_index, const char *name)
{
	const relic_rule_t *rule;
	const char *spelling;
	uint16_t slot;

	rule = &fixture->ctx->program.rules.entries[rule_index];

	for (slot = 0; slot < rule->variable_count; slot++) {
		spelling = relic_symbol_bytes(fixture->ctx,
		    rule->variables[slot].name, NULL);
		if (spelling != NULL && strcmp(spelling, name) == 0)
			return (slot);
	}

	return (UINT16_MAX);
}

static relic_dom_t
symbol_value(relic_t *ctx, const char *name)
{
	relic_dom_t dom;

	if (relic_value_intern_bytes(ctx, RELIC_SYMBOL, name, strlen(name),
	    &dom) != 0)
		return (RELIC_ID_NONE);

	return (dom);
}

/*
 * A scan's columns are decided one by one, left to right, against the slots
 * bound so far.  In e(X, a, _, X, Y) the first X binds, the constant is
 * matched, `_` is ignored, the second X is matched against the value the
 * first just bound, and Y binds.  The EMIT that follows reads both slots.
 */
static void
a_scans_columns_bind_match_or_ignore(void)
{
	static const enum relic_op_kind kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_EMIT
	};
	fixture_t fixture;
	const relic_plan_term_t *terms;
	uint16_t x;
	uint16_t y;

	if (!fixture_plan(&fixture, "e(a, a, a, a, a).\n"
	    "p(X, Y) :- e(X, a, _, X, Y).\n", 1)) {
		check(false, SCENARIO_TERMS, "the rule is planned");
		fixture_release(&fixture);
		return;
	}

	check(kinds_are(&fixture.plan, kinds, 2), SCENARIO_TERMS,
	    "the plan is a SCAN and an EMIT");

	x = slot_named(&fixture, 1, "X");
	y = slot_named(&fixture, 1, "Y");
	terms = fixture.plan.ops[0].u.atom.terms;

	check(terms[0].action == RELIC_PLAN_BIND && terms[0].u.slot == x,
	    SCENARIO_TERMS, "the first X binds its slot");
	check(terms[1].action == RELIC_PLAN_CONSTANT &&
	    terms[1].u.constant == symbol_value(fixture.ctx, "a"),
	    SCENARIO_TERMS, "the constant is matched");
	check(terms[2].action == RELIC_PLAN_ANY, SCENARIO_TERMS,
	    "the wildcard matches anything");
	check(terms[3].action == RELIC_PLAN_BOUND && terms[3].u.slot == x,
	    SCENARIO_TERMS, "the second X is matched against the first");
	check(terms[4].action == RELIC_PLAN_BIND && terms[4].u.slot == y,
	    SCENARIO_TERMS, "Y binds its slot");

	terms = fixture.plan.ops[1].u.atom.terms;
	check(terms[0].action == RELIC_PLAN_BOUND && terms[0].u.slot == x &&
	    terms[1].action == RELIC_PLAN_BOUND && terms[1].u.slot == y,
	    SCENARIO_TERMS, "the EMIT reads X and Y");

	fixture_release(&fixture);
}

/*
 * A literal whose every argument is a constant or a bound slot asks whether a
 * tuple is present, which the full-tuple index answers without a scan.  An
 * atom of arity zero is always fully bound.  A literal with a wildcard is
 * never a membership test, since only a scan can supply "any value".
 */
static void
a_fully_bound_literal_is_a_membership_test(void)
{
	static const enum relic_op_kind bound_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_MEMBER, RELIC_OP_MEMBER,
		RELIC_OP_MEMBER, RELIC_OP_SCAN, RELIC_OP_EMIT
	};
	fixture_t fixture;

	if (!fixture_plan(&fixture, "e(a, b).\nf(b, a).\ng(a).\nready.\n"
	    "p(X) :- e(X, Y), f(Y, X), g(a), ready, f(Y, _).\n", 4)) {
		check(false, SCENARIO_MEMBER, "the rule is planned");
		fixture_release(&fixture);
		return;
	}

	check(kinds_are(&fixture.plan, bound_kinds, 6), SCENARIO_MEMBER,
	    "bound literals are tests and the wildcard literal a scan");

	fixture_release(&fixture);
}

/*
 * A comparison binds nothing, so it goes at the earliest point both of its
 * operands are bound, and comparisons that become placeable at one point
 * keep their written order.  One over constants alone is placed before any
 * literal; one written before the literal binding its operand follows that
 * literal; one whose operands are bound by different literals follows the
 * later of them.
 */
static void
a_comparison_goes_where_its_operands_are_bound(void)
{
	static const enum relic_op_kind kinds[] = {
		RELIC_OP_FILTER, RELIC_OP_SCAN, RELIC_OP_FILTER,
		RELIC_OP_FILTER, RELIC_OP_SCAN, RELIC_OP_FILTER, RELIC_OP_EMIT
	};
	static const enum relic_compare_op filters[] = {
		RELIC_CMP_LT, RELIC_CMP_GE, RELIC_CMP_NE, RELIC_CMP_LE
	};
	fixture_t fixture;
	size_t filter;
	size_t i;

	if (!fixture_plan(&fixture, "e(a, 1).\nf(1).\n"
	    "p(X) :- N >= 0, X <= M, 1 < 2, e(X, N), X != b, f(M).\n", 2)) {
		check(false, SCENARIO_COMPARISONS, "the rule is planned");
		fixture_release(&fixture);
		return;
	}

	check(kinds_are(&fixture.plan, kinds, 7), SCENARIO_COMPARISONS,
	    "filters sit between the scans that bind their operands");

	filter = 0;
	for (i = 0; i < fixture.plan.op_count && filter < 4; i++) {
		if (fixture.plan.ops[i].kind != RELIC_OP_FILTER)
			continue;

		checkf(fixture.plan.ops[i].u.filter.compare == filters[filter],
		    SCENARIO_COMPARISONS,
		    "filter %zu should be <, >=, != and <= in that order",
		    filter);
		filter++;
	}

	fixture_release(&fixture);
}

/*
 * A delta variant places its delta literal first, as a SCAN of the delta,
 * and plans the rest as usual against what that scan binds.
 *
 * In p(X, Z) :- p(X, Y), p(Y, Z) with the second literal as the delta, the
 * delta scan binds Y and Z, and the first literal becomes a scan of the whole
 * relation that binds X and matches Y.  In r(X) :- e(X, Y), e(a, b), X != Y
 * with the constant literal as the delta, that literal is a delta SCAN even
 * though every argument is a constant, because a membership test would ask
 * the whole relation; the comparison follows the scan that binds its
 * operands, as it would in any plan.
 */
static void
a_delta_variant_scans_its_delta_first(void)
{
	static const enum relic_op_kind recursive_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_SCAN, RELIC_OP_EMIT
	};
	static const enum relic_op_kind constant_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_SCAN, RELIC_OP_FILTER, RELIC_OP_EMIT
	};
	fixture_t fixture;
	const relic_op_atom_t *delta;
	const relic_op_atom_t *full;
	uint16_t x;
	uint16_t y;
	uint16_t z;

	if (fixture_plan_variant(&fixture, "e(a, b).\n"
	    "p(X, Y) :- e(X, Y).\n"
	    "p(X, Z) :- p(X, Y), p(Y, Z).\n", 2, 1)) {
		check(kinds_are(&fixture.plan, recursive_kinds, 3),
		    SCENARIO_DELTA, "the variant is two SCANs and an EMIT");

		x = slot_named(&fixture, 2, "X");
		y = slot_named(&fixture, 2, "Y");
		z = slot_named(&fixture, 2, "Z");
		delta = &fixture.plan.ops[0].u.atom;
		full = &fixture.plan.ops[1].u.atom;

		check(delta->delta && !full->delta, SCENARIO_DELTA,
		    "only the first scan reads the delta");
		check(delta->terms[0].action == RELIC_PLAN_BIND &&
		    delta->terms[0].u.slot == y &&
		    delta->terms[1].action == RELIC_PLAN_BIND &&
		    delta->terms[1].u.slot == z, SCENARIO_DELTA,
		    "the delta literal p(Y, Z) binds Y and Z");
		check(full->terms[0].action == RELIC_PLAN_BIND &&
		    full->terms[0].u.slot == x &&
		    full->terms[1].action == RELIC_PLAN_BOUND &&
		    full->terms[1].u.slot == y, SCENARIO_DELTA,
		    "the other literal binds X and matches Y");
	} else {
		check(false, SCENARIO_DELTA, "the recursive variant is planned");
	}
	fixture_release(&fixture);

	if (fixture_plan_variant(&fixture, "e(a, b).\n"
	    "r(X) :- e(X, Y), e(a, b), X != Y.\n", 1, 1)) {
		check(kinds_are(&fixture.plan, constant_kinds, 4),
		    SCENARIO_DELTA, "the constant literal is scanned first");
		check(fixture.plan.ops[0].u.atom.delta &&
		    fixture.plan.ops[0].u.atom.terms[0].action ==
		    RELIC_PLAN_CONSTANT, SCENARIO_DELTA,
		    "the constant literal scans the delta, matching constants");
	} else {
		check(false, SCENARIO_DELTA, "the constant variant is planned");
	}
	fixture_release(&fixture);

	check(fixture_plan(&fixture, "e(a, b).\n"
	    "r(X) :- e(X, Y), e(a, b), X != Y.\n", 1) &&
	    fixture.plan.ops[2].kind == RELIC_OP_MEMBER &&
	    !fixture.plan.ops[0].u.atom.delta, SCENARIO_DELTA,
	    "without a delta the constant literal is a membership test");
	fixture_release(&fixture);
}

/*
 * A delta literal that is not a positive literal of the rule -- past the end
 * of the body, a comparison, or a negated literal -- is refused with one
 * error rather than read as an atom it is not.
 */
static void
a_delta_literal_must_be_positive(void)
{
	static const char *const program = "e(a, b).\n"
	    "r(X) :- e(X, Y), X != Y.\n";
	fixture_t fixture;

	check(!fixture_plan_variant(&fixture, program, 1, 2),
	    SCENARIO_DELTA_REFUSED, "a literal past the body is refused");
	checkf(fixture.errors == 1, SCENARIO_DELTA_REFUSED,
	    "one error should be reported, %zu were", fixture.errors);
	fixture_release(&fixture);

	check(!fixture_plan_variant(&fixture, program, 1, 1),
	    SCENARIO_DELTA_REFUSED, "a comparison is refused");
	checkf(fixture.errors == 1, SCENARIO_DELTA_REFUSED,
	    "one error should be reported, %zu were", fixture.errors);
	fixture_release(&fixture);

	check(!fixture_plan_variant(&fixture, "e(a, b).\nf(b).\n"
	    "r(X) :- e(X, Y), not f(Y).\n", 2, 1), SCENARIO_DELTA_REFUSED,
	    "a negated literal is refused");
	checkf(fixture.errors == 1, SCENARIO_DELTA_REFUSED,
	    "one error should be reported, %zu were", fixture.errors);
	fixture_release(&fixture);
}

/*
 * The EMIT writes whatever relation the caller names, which is how a query's
 * rule derives into the query's own answer, and the relation must have a
 * column for each head argument: a rule planned into a relation of another
 * arity is an internal error rather than a plan that reads past its head or
 * leaves a column unwritten.
 */
static void
the_emit_writes_the_relation_it_is_given(void)
{
	fixture_t fixture;
	relic_relation_t *answer;
	relic_relation_t *wide;
	const relic_rule_t *rule;
	const relic_op_t *emit;

	if (!fixture_load(&fixture, "e(a, b).\nr(X) :- e(X, _).\n",
	    TEXTUAL_FLAGS, false)) {
		check(false, SCENARIO_EMIT_TARGET, "the program loads");
		fixture_release(&fixture);
		return;
	}
	rule = &fixture.ctx->program.rules.entries[1];

	answer = relic_relation_create(fixture.ctx, RELIC_ID_NONE, 7, 1);
	wide = relic_relation_create(fixture.ctx, RELIC_ID_NONE, 8, 2);
	if (answer == NULL || wide == NULL) {
		check(false, SCENARIO_EMIT_TARGET, "the relations are created");
		relic_relation_free(answer);
		relic_relation_free(wide);
		fixture_release(&fixture);
		return;
	}

	check(relic_plan_rule(fixture.ctx, &fixture.arena, rule, answer,
	    RELIC_PLAN_NO_DELTA, NULL, &fixture.plan) == 0,
	    SCENARIO_EMIT_TARGET, "a relation of the head's arity is accepted");
	emit = &fixture.plan.ops[fixture.plan.op_count - 1];
	check(emit->kind == RELIC_OP_EMIT && emit->u.atom.relation == answer,
	    SCENARIO_EMIT_TARGET, "the EMIT writes the relation given");

	check(relic_plan_rule(fixture.ctx, &fixture.arena, rule, wide,
	    RELIC_PLAN_NO_DELTA, NULL, &fixture.plan) != 0,
	    SCENARIO_EMIT_TARGET, "a relation of another arity is refused");
	checkf(fixture.errors == 1, SCENARIO_EMIT_TARGET,
	    "one error should be reported, %zu were", fixture.errors);
	check(strstr(relic_error(fixture.ctx), "relation $q8/2") != NULL,
	    SCENARIO_EMIT_TARGET, "the error names the relation");

	relic_relation_free(answer);
	relic_relation_free(wide);
	fixture_release(&fixture);
}

/*
 * A fact's plan inserts its tuple and does nothing else, which is how the
 * evaluator loads facts without a loading step of its own.
 */
static void
a_fact_is_a_lone_emit_of_constants(void)
{
	static const enum relic_op_kind kinds[] = { RELIC_OP_EMIT };
	fixture_t fixture;
	const relic_plan_term_t *terms;

	if (!fixture_plan(&fixture, "e(a, 7).\n", 0)) {
		check(false, SCENARIO_FACT, "the fact is planned");
		fixture_release(&fixture);
		return;
	}

	check(kinds_are(&fixture.plan, kinds, 1), SCENARIO_FACT,
	    "the plan is one EMIT");

	terms = fixture.plan.ops[0].u.atom.terms;
	check(terms[0].action == RELIC_PLAN_CONSTANT &&
	    terms[1].action == RELIC_PLAN_CONSTANT, SCENARIO_FACT,
	    "both columns are constants");

	fixture_release(&fixture);
}

/*
 * A negated literal is a test like a comparison: it binds nothing, so it is
 * placed at the earliest point every argument is bound, and tests that
 * become placeable at one point keep their written order.
 *
 *	p(X) :- not done(a), not f(Y), e(X, Y), X != Y, not g(X), not frozen.
 *
 * `not done(a)` and `not frozen` read no variable, so they come before any
 * scan, in written order.  `not f(Y)` is written before the literal binding
 * Y and so follows it, ahead of the comparison and `not g(X)`, which were
 * written after it.  Every column of an ANTI_MEMBER is read, never bound.
 */
static void
a_negated_literal_is_placed_once_bound(void)
{
	static const enum relic_op_kind kinds[] = {
		RELIC_OP_ANTI_MEMBER, RELIC_OP_ANTI_MEMBER, RELIC_OP_SCAN,
		RELIC_OP_ANTI_MEMBER, RELIC_OP_FILTER, RELIC_OP_ANTI_MEMBER,
		RELIC_OP_EMIT
	};
	fixture_t fixture;
	const relic_op_atom_t *done;
	const relic_op_atom_t *frozen;
	const relic_op_atom_t *f;
	const relic_op_atom_t *g;

	if (!fixture_plan(&fixture, "e(a, b).\nf(b).\ng(a).\ndone(z).\n"
	    "frozen.\n"
	    "p(X) :- not done(a), not f(Y), e(X, Y), X != Y, not g(X), "
	    "not frozen.\n", 5)) {
		check(false, SCENARIO_NEGATION, "the rule is planned");
		fixture_release(&fixture);
		return;
	}

	/*
	 * The operands below are read by position, so a plan of other kinds
	 * would have them read the wrong arm of an operation.
	 */
	if (!kinds_are(&fixture.plan, kinds, 7)) {
		check(false, SCENARIO_NEGATION, "each negated literal follows "
		    "the scan that binds its variables");
		fixture_release(&fixture);
		return;
	}

	done = &fixture.plan.ops[0].u.atom;
	frozen = &fixture.plan.ops[1].u.atom;
	f = &fixture.plan.ops[3].u.atom;
	g = &fixture.plan.ops[5].u.atom;

	check(done->relation->arity == 1 &&
	    done->terms[0].action == RELIC_PLAN_CONSTANT &&
	    done->terms[0].u.constant == symbol_value(fixture.ctx, "a"),
	    SCENARIO_NEGATION, "not done(a) matches its constant");
	check(frozen->relation->arity == 0 && frozen->terms == NULL,
	    SCENARIO_NEGATION, "not frozen reads no column");
	check(f->terms[0].action == RELIC_PLAN_BOUND &&
	    f->terms[0].u.slot == slot_named(&fixture, 5, "Y"),
	    SCENARIO_NEGATION, "not f(Y) reads the Y the scan bound");
	check(g->terms[0].action == RELIC_PLAN_BOUND &&
	    g->terms[0].u.slot == slot_named(&fixture, 5, "X"),
	    SCENARIO_NEGATION, "not g(X) reads the X the scan bound");
	check(!done->delta && !f->delta, SCENARIO_NEGATION,
	    "no negated literal reads a delta");

	fixture_release(&fixture);
}

/*
 * Every binding pattern of a 3-ary literal, bound by constants: none bound is
 * a SCAN, all bound a MEMBER, and each of the six in between a LOOKUP keyed
 * on exactly its bound columns, through an index the relation built with
 * that key.  Pattern `mask` binds column c when bit c is set.
 *
 * The same holds for columns bound by an earlier literal rather than by a
 * constant: in r(Y) :- s(X), t(X, Y, _), s is placed first -- both are empty
 * and s is written first -- and t is then looked up by its first column.
 */
static void
each_binding_pattern_gets_its_access_path(void)
{
	static const char *const variables[] = { "A", "B", "C" };
	const relic_op_atom_t *atom;
	fixture_t fixture;
	char rule[128];
	char args[3][2];
	char head[16];
	uint32_t mask;
	size_t length;
	uint16_t column;

	for (mask = 0; mask < 8; mask++) {
		length = 0;
		head[0] = '\0';
		for (column = 0; column < 3; column++) {
			if ((mask & (UINT32_C(1) << column)) != 0) {
				strlcpy(args[column], "a", sizeof(args[column]));
				continue;
			}

			strlcpy(args[column], variables[column],
			    sizeof(args[column]));
			strlcat(head, length == 0 ? "(" : ", ", sizeof(head));
			strlcat(head, variables[column], sizeof(head));
			length++;
		}
		if (length > 0)
			strlcat(head, ")", sizeof(head));

		snprintf(rule, sizeof(rule), "t(a, a, a).\np%u%s :- t(%s, %s, "
		    "%s).\n", (unsigned int)mask, head, args[0], args[1],
		    args[2]);

		if (!fixture_load(&fixture, rule, 0, false) ||
		    !fixture_plan_rule(&fixture, 1, RELIC_PLAN_NO_DELTA, NULL) ||
		    fixture.plan.op_count != 2) {
			checkf(false, SCENARIO_ACCESS,
			    "pattern %u should be planned", (unsigned int)mask);
			fixture_release(&fixture);
			continue;
		}

		atom = &fixture.plan.ops[0].u.atom;
		if (mask == 0) {
			check(fixture.plan.ops[0].kind == RELIC_OP_SCAN,
			    SCENARIO_ACCESS, "nothing bound is a SCAN");
		} else if (mask == 7) {
			check(fixture.plan.ops[0].kind == RELIC_OP_MEMBER,
			    SCENARIO_ACCESS, "everything bound is a MEMBER");
		} else {
			checkf(fixture.plan.ops[0].kind == RELIC_OP_LOOKUP &&
			    atom->key_mask == mask &&
			    atom->index_position <
			    atom->relation->index_count &&
			    atom->relation->indexes[
			    atom->index_position].key_mask == mask,
			    SCENARIO_ACCESS,
			    "pattern %u should be a LOOKUP on its own key",
			    (unsigned int)mask);
		}
		checkf(atom->relation->index_count ==
		    (mask == 0 || mask == 7 ? 0u : 1u), SCENARIO_ACCESS,
		    "pattern %u should build only the index it reads",
		    (unsigned int)mask);

		fixture_release(&fixture);
	}

	if (fixture_load(&fixture, "s(a).\nt(a, b, c).\n"
	    "r(Y) :- s(X), t(X, Y, _).\n", 0, false) &&
	    fixture_plan_rule(&fixture, 2, RELIC_PLAN_NO_DELTA, NULL) &&
	    fixture.plan.op_count == 3) {
		check(fixture.plan.ops[0].kind == RELIC_OP_SCAN &&
		    fixture.plan.ops[1].kind == RELIC_OP_LOOKUP &&
		    fixture.plan.ops[1].u.atom.key_mask == 1 &&
		    fixture.plan.ops[1].u.atom.terms[0].action ==
		    RELIC_PLAN_BOUND &&
		    fixture.plan.ops[1].u.atom.terms[1].action ==
		    RELIC_PLAN_BIND &&
		    fixture.plan.ops[1].u.atom.terms[2].action ==
		    RELIC_PLAN_ANY, SCENARIO_ACCESS,
		    "a column an earlier literal bound is looked up by");
	} else {
		check(false, SCENARIO_ACCESS, "the joined rule is planned");
	}
	fixture_release(&fixture);
}

/*
 * A three-literal body over a program that makes tiny/1 the smallest
 * relation and huge/2 the largest:
 *
 *	result(X) :- huge(X, Y), tiny(X), medium(Y).
 *
 * Nothing is bound at first, so the estimates decide and tiny(X) is scanned.
 * huge(X, Y) then has a bound argument and medium(Y) none, so huge follows,
 * looked up by X, and medium(Y) is last, a membership test.
 */
static void
bound_arguments_then_small_relations_go_first(void)
{
	static const enum relic_op_kind kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_LOOKUP, RELIC_OP_MEMBER, RELIC_OP_EMIT
	};
	static const char *const order[] = {
		"tiny", "huge", "medium", "result"
	};
	fixture_t fixture;
	char program[4096];
	char fact[32];
	int i;

	program[0] = '\0';
	for (i = 0; i < 2; i++) {
		snprintf(fact, sizeof(fact), "tiny(%d).\n", i);
		strlcat(program, fact, sizeof(program));
	}
	for (i = 0; i < 10; i++) {
		snprintf(fact, sizeof(fact), "medium(%d).\n", i);
		strlcat(program, fact, sizeof(program));
	}
	for (i = 0; i < 40; i++) {
		snprintf(fact, sizeof(fact), "huge(%d, %d).\n", i, i % 10);
		strlcat(program, fact, sizeof(program));
	}
	strlcat(program, "result(X) :- huge(X, Y), tiny(X), medium(Y).\n",
	    sizeof(program));

	if (fixture_load(&fixture, program, 0, true) &&
	    fixture_plan_rule(&fixture, 52, RELIC_PLAN_NO_DELTA, NULL)) {
		check(kinds_are(&fixture.plan, kinds, 4) &&
		    reads_in_order(&fixture, order, 4), SCENARIO_GREEDY,
		    "tiny is scanned, huge looked up, medium tested");
		check(fixture.plan.ops[1].u.atom.key_mask == 1,
		    SCENARIO_GREEDY, "huge is looked up by X");
	} else {
		check(false, SCENARIO_GREEDY, "the rule is planned");
	}
	fixture_release(&fixture);
}

/*
 * A bound argument outranks a smaller estimate, which the body above cannot
 * show, because there huge also estimates less than medium once X is bound:
 *
 *	r(X, W) :- tiny(Y), lots(Y, X), pair(W).
 *
 * After tiny, lots(Y, X) has Y bound but every tuple of lots holds the same
 * Y, so it estimates 20, while pair(W) binds nothing and estimates 5.  The
 * bound argument wins, and lots goes before pair.
 */
static void
a_bound_argument_outranks_a_smaller_estimate(void)
{
	static const enum relic_op_kind kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_LOOKUP, RELIC_OP_SCAN, RELIC_OP_EMIT
	};
	static const char *const order[] = { "tiny", "lots", "pair", "r" };
	fixture_t fixture;
	char program[4096];
	char fact[32];
	int i;

	program[0] = '\0';
	for (i = 0; i < 2; i++) {
		snprintf(fact, sizeof(fact), "tiny(%d).\n", i);
		strlcat(program, fact, sizeof(program));
	}
	for (i = 0; i < 20; i++) {
		snprintf(fact, sizeof(fact), "lots(0, %d).\n", i);
		strlcat(program, fact, sizeof(program));
	}
	for (i = 0; i < 5; i++) {
		snprintf(fact, sizeof(fact), "pair(%d).\n", i);
		strlcat(program, fact, sizeof(program));
	}
	strlcat(program, "r(X, W) :- tiny(Y), lots(Y, X), pair(W).\n",
	    sizeof(program));

	if (fixture_load(&fixture, program, 0, true) &&
	    fixture_plan_rule(&fixture, 27, RELIC_PLAN_NO_DELTA, NULL)) {
		check(kinds_are(&fixture.plan, kinds, 4) &&
		    reads_in_order(&fixture, order, 4), SCENARIO_GREEDY,
		    "a bound argument outranks a smaller estimate");
	} else {
		check(false, SCENARIO_GREEDY, "the rule is planned");
	}
	fixture_release(&fixture);
}

/*
 * Of two literals with the same number of bound arguments, the one with the
 * smaller estimate goes first; of two with the same estimate, the one written
 * first.
 *
 *	r(Y, Z) :- k(X), many(X, Z), few(X, Y).
 *
 * k is a single tuple and goes first.  many and few then each have X bound
 * and hold four tuples, but many holds one value of X and few four, so a
 * binding of X is estimated to match 4 tuples of many and 1 of few: few goes
 * before many, though written after it.  An empty relation estimates zero
 * and goes before any that is not.
 */
static void
the_smaller_estimate_goes_first(void)
{
	static const char *const selective[] = { "k", "few", "many", "r" };
	static const char *const empty_first[] = { "none", "k", "s" };
	fixture_t fixture;

	if (fixture_load(&fixture, "k(0).\n"
	    "many(0, 1).\nmany(0, 2).\nmany(0, 3).\nmany(0, 4).\n"
	    "few(0, 1).\nfew(1, 2).\nfew(2, 3).\nfew(3, 4).\n"
	    "r(Y, Z) :- k(X), many(X, Z), few(X, Y).\n", 0, true) &&
	    fixture_plan_rule(&fixture, 9, RELIC_PLAN_NO_DELTA, NULL)) {
		check(reads_in_order(&fixture, selective, 4), SCENARIO_ESTIMATE,
		    "the more selective lookup goes first");
	} else {
		check(false, SCENARIO_ESTIMATE, "the selective rule is planned");
	}
	fixture_release(&fixture);

	if (fixture_load(&fixture, "k(0).\nnone(1) :- k(2).\n"
	    "s(X) :- k(X), none(X).\n", 0, true) &&
	    fixture_plan_rule(&fixture, 2, RELIC_PLAN_NO_DELTA, NULL)) {
		check(reads_in_order(&fixture, empty_first, 3),
		    SCENARIO_ESTIMATE, "an empty relation goes first");
	} else {
		check(false, SCENARIO_ESTIMATE, "the empty rule is planned");
	}
	fixture_release(&fixture);
}

/*
 * A relation the component being planned is still deriving is empty, and is
 * assumed to hold RELIC_PLAN_UNKNOWN_TUPLES tuples rather than measured as
 * empty.  It therefore goes after a complete relation smaller than that and
 * before one larger, and marked complete instead it would go first.
 */
static void
a_relation_being_derived_is_assumed(void)
{
	static const char *const small_first[] = { "small", "grow", "grow" };
	static const char *const derived_first[] = { "grow", "big", "grow" };
	static const char *const measured_first[] = { "grow", "small", "grow" };
	fixture_t fixture;
	bool incomplete[8];
	relic_pred_id_t grow;
	char *program;
	char fact[32];
	size_t size;
	int i;

	size = 32 * (RELIC_PLAN_UNKNOWN_TUPLES + 16);
	program = malloc(size);
	if (program == NULL) {
		check(false, SCENARIO_INCOMPLETE, "the program is built");
		return;
	}

	program[0] = '\0';
	for (i = 0; i < 3; i++) {
		snprintf(fact, sizeof(fact), "small(%d).\n", i);
		strlcat(program, fact, size);
	}
	for (i = 0; i < (int)RELIC_PLAN_UNKNOWN_TUPLES + 1; i++) {
		snprintf(fact, sizeof(fact), "big(%d).\n", i);
		strlcat(program, fact, size);
	}
	strlcat(program, "grow(X) :- grow(X), small(X).\n"
	    "grow(X) :- grow(X), big(X).\n", size);

	if (!fixture_load(&fixture, program, 0, false)) {
		check(false, SCENARIO_INCOMPLETE, "the program is loaded");
		fixture_release(&fixture);
		free(program);
		return;
	}
	free(program);

	/*
	 * The facts are complete once materialized; grow derives nothing and
	 * is marked as still being derived, as its own component would be.
	 */
	if (relic_materialize(fixture.ctx) != 0 ||
	    relic_pred_count(fixture.ctx) > 8) {
		check(false, SCENARIO_INCOMPLETE, "the facts are materialized");
		fixture_release(&fixture);
		return;
	}
	bzero(incomplete, sizeof(incomplete));
	grow = fixture.ctx->program.rules.entries[
	    fixture.ctx->program.rules.count - 1].head;
	incomplete[grow - 1] = true;

	check(fixture_plan_rule(&fixture, fixture.ctx->program.rules.count - 2,
	    RELIC_PLAN_NO_DELTA, incomplete) &&
	    reads_in_order(&fixture, small_first, 3), SCENARIO_INCOMPLETE,
	    "a complete relation smaller than the assumption goes first");
	check(fixture_plan_rule(&fixture, fixture.ctx->program.rules.count - 1,
	    RELIC_PLAN_NO_DELTA, incomplete) &&
	    reads_in_order(&fixture, derived_first, 3), SCENARIO_INCOMPLETE,
	    "a complete relation larger than the assumption goes after");
	check(fixture_plan_rule(&fixture, fixture.ctx->program.rules.count - 2,
	    RELIC_PLAN_NO_DELTA, NULL) &&
	    reads_in_order(&fixture, measured_first, 3), SCENARIO_INCOMPLETE,
	    "measured as empty, the same relation goes first");

	fixture_release(&fixture);
}

/*
 * A relation being derived is assumed to hold RELIC_PLAN_UNKNOWN_TUPLES
 * tuples, divided by RELIC_PLAN_UNKNOWN_DISTINCT for each bound column, just
 * as a measured relation is divided by its distinct counts:
 *
 *	link(Y, X) :- k(Y), half(Y, X), link(Y, _).
 *
 * After k, both half and link have Y bound.  half holds HALF_TUPLES tuples,
 * all with the same Y, so it estimates HALF_TUPLES; link, still being
 * derived, estimates 1000 / 10 = 100, which is less, so link goes first.
 * Undivided, it would estimate 1000 and go after half.
 */
static void
an_assumed_relation_divides_per_bound_column(void)
{
	static const char *const divided[] = { "k", "link", "half", "link" };
	fixture_t fixture;
	bool incomplete[8];
	relic_pred_id_t link;
	char *program;
	char fact[32];
	size_t size;
	int i;

	size = 32 * (HALF_TUPLES + 4);
	program = malloc(size);
	if (program == NULL) {
		check(false, SCENARIO_INCOMPLETE, "the program is built");
		return;
	}

	strlcpy(program, "k(0).\n", size);
	for (i = 0; i < HALF_TUPLES; i++) {
		snprintf(fact, sizeof(fact), "half(0, %d).\n", i);
		strlcat(program, fact, size);
	}
	strlcat(program, "link(Y, X) :- k(Y), half(Y, X), link(Y, _).\n", size);

	if (!fixture_load(&fixture, program, 0, true) ||
	    relic_pred_count(fixture.ctx) > 8) {
		check(false, SCENARIO_INCOMPLETE, "the facts are materialized");
		fixture_release(&fixture);
		free(program);
		return;
	}
	free(program);

	bzero(incomplete, sizeof(incomplete));
	link = fixture.ctx->program.rules.entries[
	    fixture.ctx->program.rules.count - 1].head;
	incomplete[link - 1] = true;

	check(fixture_plan_rule(&fixture, fixture.ctx->program.rules.count - 1,
	    RELIC_PLAN_NO_DELTA, incomplete) &&
	    reads_in_order(&fixture, divided, 4), SCENARIO_INCOMPLETE,
	    "the assumption is divided for the bound column");

	fixture_release(&fixture);
}

/*
 * -X no-index makes every lookup a scan matching the same columns and builds
 * no index; -X no-reorder keeps the written order.  Neither touches the
 * other's choice, and a fully bound literal is a MEMBER under both.
 */
static void
the_references_change_only_their_choice(void)
{
	static const char *const program = "k(0).\nbig(0, 1).\nbig(1, 2).\n"
	    "r(Y) :- big(X, Y), k(X), k(Y).\n";
	static const enum relic_op_kind no_index[] = {
		RELIC_OP_SCAN, RELIC_OP_SCAN, RELIC_OP_MEMBER, RELIC_OP_EMIT
	};
	static const enum relic_op_kind no_reorder[] = {
		RELIC_OP_SCAN, RELIC_OP_MEMBER, RELIC_OP_MEMBER, RELIC_OP_EMIT
	};
	static const char *const reordered[] = { "k", "big", "k", "r" };
	static const char *const written[] = { "big", "k", "k", "r" };
	fixture_t fixture;

	if (fixture_load(&fixture, program, RELIC_F_NO_INDEX, true) &&
	    fixture_plan_rule(&fixture, 3, RELIC_PLAN_NO_DELTA, NULL)) {
		check(kinds_are(&fixture.plan, no_index, 4) &&
		    reads_in_order(&fixture, reordered, 4) &&
		    fixture.plan.ops[1].u.atom.terms[0].action ==
		    RELIC_PLAN_BOUND, SCENARIO_REFERENCES,
		    "without indexes big is scanned, matching X");
		check(fixture.plan.ops[1].u.atom.relation->index_count == 0,
		    SCENARIO_REFERENCES, "without indexes none is built");
	} else {
		check(false, SCENARIO_REFERENCES, "the no-index rule is planned");
	}
	fixture_release(&fixture);

	if (fixture_load(&fixture, program, RELIC_F_NO_REORDER, true) &&
	    fixture_plan_rule(&fixture, 3, RELIC_PLAN_NO_DELTA, NULL)) {
		check(kinds_are(&fixture.plan, no_reorder, 4) &&
		    reads_in_order(&fixture, written, 4), SCENARIO_REFERENCES,
		    "without reordering the written order is kept");
	} else {
		check(false, SCENARIO_REFERENCES,
		    "the no-reorder rule is planned");
	}
	fixture_release(&fixture);
}

/*
 * A number in [0, bound) from a linear congruential sequence, so that every
 * run generates the same programs on every platform.
 */
static uint32_t
next_random(uint32_t *state, uint32_t bound)
{
	*state = *state * UINT32_C(1103515245) + UINT32_C(12345);

	return ((*state >> 16) % bound);
}

static void
text_append(text_t *text, const char *format, ...)
{
	va_list arguments;
	size_t room;
	int written;

	room = sizeof(text->buffer) - text->length;

	va_start(arguments, format);
	written = vsnprintf(text->buffer + text->length, room, format,
	    arguments);
	va_end(arguments);

	if (written < 0 || (size_t)written >= room) {
		text->overflowed = true;
		return;
	}

	text->length += (size_t)written;
}

/*
 * The arity of generated relation r<relation>: 1, 2 or 3, so that the bodies
 * mix literals that bind one variable with literals that bind several.
 */
static uint16_t
relation_arity(size_t relation)
{
	return ((uint16_t)(1 + relation % 3));
}

/*
 * Facts for every generated relation, each drawing its values from a range
 * of its own so that tuple counts and distinct counts differ between
 * relations and between columns.  A relation may be empty, which estimates
 * zero.
 */
static void
generate_facts(text_t *text, uint32_t *state)
{
	uint32_t tuples;
	uint32_t values;
	uint32_t t;
	uint16_t column;
	size_t relation;

	for (relation = 0; relation < RANKED_RELATIONS; relation++) {
		tuples = next_random(state, RANKED_MAX_TUPLES + 1);
		values = 1 + next_random(state, RANKED_MAX_VALUES);

		for (t = 0; t < tuples; t++) {
			text_append(text, "r%zu(", relation);
			for (column = 0; column < relation_arity(relation);
			    column++)
				text_append(text, "%s%u", column > 0 ? ", " : "",
				    (unsigned int)next_random(state, values));
			text_append(text, ").\n");
		}

		/*
		 * A rule deriving nothing new defines the predicate even
		 * when it has no facts.
		 */
		text_append(text, "r%zu(", relation);
		for (column = 0; column < relation_arity(relation); column++)
			text_append(text, "%sX%u", column > 0 ? ", " : "",
			    (unsigned int)column);
		text_append(text, ") :- r%zu(", relation);
		for (column = 0; column < relation_arity(relation); column++)
			text_append(text, "%sX%u", column > 0 ? ", " : "",
			    (unsigned int)column);
		text_append(text, ").\n");
	}
}

/*
 * A variable, one already in `variables` -- a bit per variable -- so that
 * a test reads only what some positive literal binds, which keeps the rule
 * safe.
 */
static void
generate_variable(text_t *text, uint32_t *state, uint32_t variables)
{
	uint32_t variable;

	do
		variable = next_random(state, RANKED_VARIABLES);
	while ((variables & (UINT32_C(1) << variable)) == 0);

	text_append(text, "X%u", (unsigned int)variable);
}

/*
 * A positive literal over a generated relation: each argument a variable,
 * a constant or `_`, the first always a variable so that the literal binds
 * something the head can read.  Adds the variables it mentions to
 * `variables`.
 */
static void
generate_positive(text_t *text, uint32_t *state, uint32_t *variables)
{
	uint32_t relation;
	uint32_t choice;
	uint32_t variable;
	uint16_t column;

	relation = next_random(state, RANKED_RELATIONS);
	text_append(text, "r%u(", (unsigned int)relation);

	for (column = 0; column < relation_arity(relation); column++) {
		text_append(text, "%s", column > 0 ? ", " : "");

		choice = column == 0 ? 0 : next_random(state, 10);
		if (choice < 7) {
			variable = next_random(state, RANKED_VARIABLES);
			*variables |= UINT32_C(1) << variable;
			text_append(text, "X%u", (unsigned int)variable);
		} else if (choice < 9) {
			text_append(text, "%u", (unsigned int)next_random(state,
			    RANKED_MAX_VALUES));
		} else {
			text_append(text, "_");
		}
	}

	text_append(text, ")");
}

/*
 * A test reading only variables in `variables`: a comparison of a variable
 * with another or with a constant, or a negated literal over a generated
 * relation, which is complete.
 */
static void
generate_test(text_t *text, uint32_t *state, uint32_t variables)
{
	uint32_t relation;
	uint16_t column;

	switch (next_random(state, 3)) {
	case 0:
		generate_variable(text, state, variables);
		text_append(text, " < ");
		generate_variable(text, state, variables);
		return;
	case 1:
		generate_variable(text, state, variables);
		text_append(text, " != %u", (unsigned int)next_random(state,
		    RANKED_MAX_VALUES));
		return;
	default:
		break;
	}

	relation = next_random(state, RANKED_RELATIONS);
	text_append(text, "not r%u(", (unsigned int)relation);
	for (column = 0; column < relation_arity(relation); column++) {
		text_append(text, "%s", column > 0 ? ", " : "");
		generate_variable(text, state, variables);
	}
	text_append(text, ")");
}

/*
 * A rule h<number>(X) of positive literals and tests, the tests written at
 * the end so that each can read any variable of the body; the planner
 * places a test where its slots are first bound whatever its written
 * position.  Repeated relations and variables give the ranking ties to
 * break and placements that bind nothing new.
 */
static void
generate_rule(text_t *text, uint32_t *state, size_t number)
{
	uint32_t variables;
	uint32_t positives;
	uint32_t tests;
	uint32_t i;
	text_t body;

	bzero(&body, sizeof(body));
	variables = 0;

	positives = 1 + next_random(state, RANKED_MAX_POSITIVES);
	for (i = 0; i < positives; i++) {
		text_append(&body, "%s", i > 0 ? ", " : "");
		generate_positive(&body, state, &variables);
	}

	tests = next_random(state, RANKED_MAX_TESTS + 1);
	for (i = 0; i < tests; i++) {
		text_append(&body, ", ");
		generate_test(&body, state, variables);
	}

	text_append(text, "h%zu(", number);
	generate_variable(text, state, variables);
	text_append(text, ") :- %s.\n", body.buffer);
	text->overflowed |= body.overflowed;
}

/*
 * The slots a run of arguments reads.
 */
static uint64_t
reference_slots(const relic_arg_t *args, uint16_t count)
{
	uint64_t slots;
	uint16_t i;

	slots = 0;
	for (i = 0; i < count; i++) {
		if (args[i].kind == RELIC_ARG_VARIABLE)
			slots |= UINT64_C(1) << args[i].u.slot;
	}

	return (slots);
}

/*
 * A positive literal's score against `bound`, as the planner's contract
 * states it: the bound argument count, and the tuple count divided, rounding
 * up, by the distinct values of each bound column -- or, for a relation
 * `incomplete` marks, the assumed size divided by the assumed selectivity.
 * Which columns are bound is asked of relic_plan_bound_columns(), whose own
 * scenarios above pin it down; what is checked here is the order built on
 * it.
 */
static bool
reference_score(relic_t *ctx, const relic_atom_t *atom, uint64_t bound,
    const bool *incomplete, uint16_t *bound_argsp, size_t *estimatep)
{
	relic_relation_t *relation;
	const relic_pred_t *pred;
	uint32_t columns;
	size_t distinct;
	size_t estimate;
	uint16_t column;
	bool assumed;

	pred = relic_pred_of(ctx, atom->pred);
	if (pred == NULL)
		return (false);

	assumed = incomplete != NULL && incomplete[atom->pred - 1];
	relation = NULL;
	if (!assumed && (relation = relic_relation_for(ctx, atom->pred)) ==
	    NULL)
		return (false);

	columns = relic_plan_bound_columns(atom, pred->arity, bound);

	*bound_argsp = 0;
	estimate = assumed ? RELIC_PLAN_UNKNOWN_TUPLES :
	    relic_relation_count(relation);
	for (column = 0; column < pred->arity; column++) {
		if ((columns & (UINT32_C(1) << column)) == 0)
			continue;

		(*bound_argsp)++;
		if (assumed) {
			estimate = (estimate + RELIC_PLAN_UNKNOWN_DISTINCT - 1) /
			    RELIC_PLAN_UNKNOWN_DISTINCT;
			continue;
		}
		if (estimate == 0)
			continue;

		if (relic_relation_distinct(ctx, relation, column,
		    &distinct) != 0)
			return (false);
		estimate = (estimate + distinct - 1) / distinct;
	}

	*estimatep = estimate;

	return (true);
}

static bool
reference_better(uint16_t bound_args, size_t estimate,
    uint16_t best_bound_args, size_t best_estimate)
{
	if (bound_args != best_bound_args)
		return (bound_args > best_bound_args);

	return (estimate < best_estimate);
}

/*
 * Whether a test reads only slots in `bound`.  A negated literal holding `_`
 * would count as ready here and never in the planner, which never places
 * one; the generated negations hold variables only.
 */
static bool
reference_test_ready(relic_t *ctx, const relic_literal_t *literal,
    uint64_t bound)
{
	const relic_pred_t *pred;
	uint64_t slots;

	switch (literal->kind) {
	case RELIC_LITERAL_COMPARISON:
		slots = reference_slots(&literal->u.comparison.left, 1) |
		    reference_slots(&literal->u.comparison.right, 1);
		return ((slots & ~bound) == 0);
	case RELIC_LITERAL_NEGATED:
		pred = relic_pred_of(ctx, literal->u.atom.pred);
		return (pred != NULL && (reference_slots(literal->u.atom.args,
		    pred->arity) & ~bound) == 0);
	case RELIC_LITERAL_POSITIVE:
		break;
	}

	return (false);
}

/*
 * Append every test not yet placed that `bound` makes ready, in body order,
 * and return the new length of the order.
 */
static size_t
reference_place_tests(relic_t *ctx, const relic_rule_t *rule, uint64_t bound,
    bool *placed, size_t *order, size_t count)
{
	size_t i;

	for (i = 0; i < rule->body_length; i++) {
		if (placed[i] || !reference_test_ready(ctx, &rule->body[i],
		    bound))
			continue;

		placed[i] = true;
		order[count] = i;
		count++;
	}

	return (count);
}

/*
 * The greedy order computed one step at a time: at every step each positive
 * literal not yet placed is scored against the slots bound so far, in body
 * order, and the first of the best is placed, followed by every test it
 * made ready.  Under RELIC_F_NO_REORDER the first positive literal not yet
 * placed is chosen instead.  This is the definition the planner's passes
 * must reproduce, written without them.
 */
static bool
reference_order(relic_t *ctx, const relic_rule_t *rule, uint64_t bound,
    const bool *incomplete, size_t *order)
{
	bool placed[RANKED_MAX_POSITIVES + RANKED_MAX_TESTS];
	uint16_t best_bound_args;
	uint16_t bound_args;
	size_t best_estimate;
	size_t estimate;
	size_t count;
	size_t best;
	size_t i;
	bool reorder;

	if (rule->body_length > sizeof(placed) / sizeof(placed[0]))
		return (false);

	reorder = (ctx->config.flags & RELIC_F_NO_REORDER) == 0;
	bzero(placed, sizeof(placed));
	count = reference_place_tests(ctx, rule, bound, placed, order, 0);

	while (count < rule->body_length) {
		best = SIZE_MAX;
		best_bound_args = 0;
		best_estimate = 0;

		for (i = 0; i < rule->body_length; i++) {
			if (placed[i] ||
			    rule->body[i].kind != RELIC_LITERAL_POSITIVE)
				continue;

			if (!reorder) {
				best = i;
				break;
			}

			if (!reference_score(ctx, &rule->body[i].u.atom, bound,
			    incomplete, &bound_args, &estimate))
				return (false);

			if (best == SIZE_MAX || reference_better(bound_args,
			    estimate, best_bound_args, best_estimate)) {
				best = i;
				best_bound_args = bound_args;
				best_estimate = estimate;
			}
		}

		if (best == SIZE_MAX)
			return (false);

		placed[best] = true;
		order[count] = best;
		count++;
		bound |= reference_slots(rule->body[best].u.atom.args,
		    relic_pred_of(ctx, rule->body[best].u.atom.pred)->arity);
		count = reference_place_tests(ctx, rule, bound, placed, order,
		    count);
	}

	return (true);
}

/*
 * Generate one program, load its facts and evaluate them so that the
 * planner has statistics to measure, then load its rules without evaluating
 * them -- a generated body can be a cross product far too large to run --
 * and compare the planner's order of each rule with the reference's three
 * ways: from nothing bound, from a generated set of bound slots, and from
 * that set with a generated choice of relations taken to be still derived.
 */
static void
check_ranked_program(uint32_t *state, unsigned int flags)
{
	size_t expected[RANKED_MAX_POSITIVES + RANKED_MAX_TESTS];
	size_t actual[RANKED_MAX_POSITIVES + RANKED_MAX_TESTS];
	const relic_rule_t *rule;
	bool *incomplete;
	bool *assumed;
	fixture_t fixture;
	text_t facts;
	text_t rules;
	uint64_t bound;
	size_t first_rule;
	size_t pred_count;
	size_t p;
	size_t r;
	int start;

	bzero(&facts, sizeof(facts));
	bzero(&rules, sizeof(rules));
	generate_facts(&facts, state);
	for (r = 0; r < RANKED_RULES; r++)
		generate_rule(&rules, state, r);

	if (facts.overflowed || rules.overflowed) {
		check(false, SCENARIO_RANKING, "a generated program fits");
		return;
	}

	if (!fixture_load(&fixture, facts.buffer, flags, true) ||
	    relic_load_string(fixture.ctx, "rules.dl", rules.buffer) != 0 ||
	    relic_check(fixture.ctx) != 0) {
		checkf(false, SCENARIO_RANKING, "the generated program "
		    "loads:\n%s%s", facts.buffer, rules.buffer);
		fixture_release(&fixture);
		return;
	}

	pred_count = relic_pred_count(fixture.ctx);
	incomplete = calloc(pred_count, sizeof(*incomplete));
	if (incomplete == NULL) {
		check(false, SCENARIO_RANKING, "the incomplete marks fit");
		fixture_release(&fixture);
		return;
	}

	first_rule = fixture.ctx->program.rules.count - RANKED_RULES;
	for (r = first_rule; r < fixture.ctx->program.rules.count; r++) {
		rule = &fixture.ctx->program.rules.entries[r];

		for (p = 0; p < pred_count; p++)
			incomplete[p] = next_random(state, 3) == 0;

		for (start = 0; start < 3; start++) {
			bound = 0;
			if (start > 0 && rule->variable_count > 0)
				bound = next_random(state,
				    UINT32_C(1) << rule->variable_count);
			assumed = start == 2 ? incomplete : NULL;

			bzero(expected, sizeof(expected));
			bzero(actual, sizeof(actual));

			if (!reference_order(fixture.ctx, rule, bound, assumed,
			    expected) ||
			    relic_plan_order(fixture.ctx, &fixture.arena, rule,
			    bound, assumed, actual) != 0) {
				checkf(false, SCENARIO_RANKING, "rule %zu of "
				    "this program is ordered:\n%s%s", r,
				    facts.buffer, rules.buffer);
				continue;
			}

			checkf(bcmp(expected, actual, rule->body_length *
			    sizeof(expected[0])) == 0, SCENARIO_RANKING,
			    "rule %zu from bound set %#llx%s should be ordered "
			    "as the step-by-step comparison orders it, with "
			    "flags %#x:\n%s%s", r, (unsigned long long)bound,
			    assumed != NULL ? " with relations assumed" : "",
			    flags, facts.buffer, rules.buffer);
		}
	}

	free(incomplete);
	fixture_release(&fixture);
}

/*
 * The planner orders a body in passes, one per state of the bound set,
 * placing literals from a ranking while the bound set is unchanged.  That
 * must choose exactly what comparing every unplaced literal at every step
 * chooses, under the default planner and under -X no-reorder, from nothing
 * bound, from bound slots a Magic caller would start with, and with some
 * relations taken to be still derived.  Generated
 * bodies repeat relations and variables, hold constants, wildcards, empty
 * relations and tests written anywhere, so ties, placements that bind
 * nothing and passes that end early all occur.
 */
static void
the_ranking_is_the_step_by_step_order(void)
{
	uint32_t state;
	size_t i;

	state = 20260923;
	for (i = 0; i < RANKED_PROGRAMS; i++) {
		check_ranked_program(&state, 0);
		check_ranked_program(&state, RELIC_F_NO_REORDER);
	}
}

int
main(void)
{
	a_scans_columns_bind_match_or_ignore();
	a_fully_bound_literal_is_a_membership_test();
	a_comparison_goes_where_its_operands_are_bound();
	a_delta_variant_scans_its_delta_first();
	a_delta_literal_must_be_positive();
	the_emit_writes_the_relation_it_is_given();
	a_fact_is_a_lone_emit_of_constants();
	a_negated_literal_is_placed_once_bound();
	each_binding_pattern_gets_its_access_path();
	bound_arguments_then_small_relations_go_first();
	a_bound_argument_outranks_a_smaller_estimate();
	the_smaller_estimate_goes_first();
	a_relation_being_derived_is_assumed();
	an_assumed_relation_divides_per_bound_column();
	the_references_change_only_their_choice();
	the_ranking_is_the_step_by_step_order();

	return (check_summary("test_plan"));
}
