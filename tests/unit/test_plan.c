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
 * each literal became, what each column does, and where each comparison went
 * -- because those are the decisions every later planner inherits.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_COMPARISONS	"a comparison goes where its operands are bound"
#define	SCENARIO_FACT		"a fact is a lone EMIT of constants"
#define	SCENARIO_MEMBER		"a fully bound literal is a membership test"
#define	SCENARIO_NEGATION	"a negated literal is refused, naming the rule"
#define	SCENARIO_TERMS		"a scan's columns bind, match or ignore"

typedef struct fixture fixture_t;

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

static void	 count_error(void *callback_ctx, enum relic_diag_level level,
		    const relic_location_t *where, const char *message);
static void	 fixture_release(fixture_t *fixture);
static bool	 fixture_plan(fixture_t *fixture, const char *program,
		    size_t rule_index);
static bool	 kinds_are(const relic_plan_t *plan,
		    const enum relic_op_kind *expected, size_t count);
static uint16_t	 slot_named(const fixture_t *fixture, size_t rule_index,
		    const char *name);
static relic_dom_t symbol_value(relic_t *ctx, const char *name);
static void	 a_comparison_goes_where_its_operands_are_bound(void);
static void	 a_fact_is_a_lone_emit_of_constants(void);
static void	 a_fully_bound_literal_is_a_membership_test(void);
static void	 a_negated_literal_is_refused(void);
static void	 a_scans_columns_bind_match_or_ignore(void);

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
 * Load a program, check it, and plan the rule at `rule_index` in load order.
 * Returns whether the plan was built; a test that expects a refusal reads
 * the error count.
 */
static bool
fixture_plan(fixture_t *fixture, const char *program, size_t rule_index)
{
	bzero(fixture, sizeof(*fixture));
	relic_arena_init(&fixture->arena);

	fixture->ctx = relic_create();
	if (fixture->ctx == NULL)
		return (false);
	relic_diag_set(fixture->ctx, count_error, fixture);

	if (relic_load_string(fixture->ctx, "test.dl", program) != 0 ||
	    relic_check(fixture->ctx) != 0)
		return (false);

	if (rule_index >= fixture->ctx->program.rules.count)
		return (false);

	return (relic_plan_rule(fixture->ctx, &fixture->arena,
	    &fixture->ctx->program.rules.entries[rule_index],
	    &fixture->plan) == 0);
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
 * Absence is meaningful only in a relation that is complete, and nothing
 * the planner knows guarantees that, so a negated literal is refused with
 * one error rather than planned.
 */
static void
a_negated_literal_is_refused(void)
{
	fixture_t fixture;

	check(!fixture_plan(&fixture, "e(a).\nf(b).\n"
	    "p(X) :- e(X), not f(X).\n", 2), SCENARIO_NEGATION,
	    "the rule is not planned");
	checkf(fixture.errors == 1, SCENARIO_NEGATION,
	    "one error should be reported, %zu were", fixture.errors);

	fixture_release(&fixture);
}

int
main(void)
{
	a_scans_columns_bind_match_or_ignore();
	a_fully_bound_literal_is_a_membership_test();
	a_comparison_goes_where_its_operands_are_bound();
	a_fact_is_a_lone_emit_of_constants();
	a_negated_literal_is_refused();

	return (check_summary("test_plan"));
}
