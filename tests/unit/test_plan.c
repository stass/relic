/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
#define	SCENARIO_AGGREGATE	"a fold goes where its key is bound"
#define	SCENARIO_FOLD_ACCESS	"a fold reads its group by its key"
#define	SCENARIO_ASSIGN		"an assignment binds a slot of its kind"
#define	SCENARIO_EQUALITY	"an equality of two literals' variables assigns"
#define	SCENARIO_COMPARISONS	"a comparison goes where its operands are bound"
#define	SCENARIO_FAMILY		"a variant keeps both its join orders"
#define	SCENARIO_GUARD		"a guard of any size keeps the input-first plan"
#define	SCENARIO_RETAINED	"estimates never discard a candidate"
#define	SCENARIO_WRITTEN	"a textual variant keeps its written order"
#define	SCENARIO_DELTA_REFUSED	"a delta literal must be a member literal"
#define	SCENARIO_DRAFT		"a draft names its indexes and builds none"
#define	SCENARIO_DISTINCT	"a plan keeping every binding derives distinct rows"
#define	SCENARIO_EMIT_TARGET	"the EMIT writes the relation it is given"
#define	SCENARIO_ESTIMATE	"the smaller estimate goes first"
#define	SCENARIO_FACT		"a body-less rule is a lone EMIT of constants"
#define	SCENARIO_GREEDY		"bound arguments go first, then small relations"
#define	SCENARIO_INCOMPLETE	"a relation being derived is assumed, not measured"
#define	SCENARIO_MEMBER		"a fully bound literal is a membership test"
#define	SCENARIO_EXISTS		"a literal binding nothing read is a check"
#define	SCENARIO_LIVE		"a variable mentioned twice is not dead"
#define	SCENARIO_CHECK_PLACED	"a literal is a check where it is placed so"
#define	SCENARIO_CHECK_VARIANT	"a delta literal may be a check in one plan"
#define	SCENARIO_NO_EXISTS	"without checks every literal is planned as before"
#define	SCENARIO_NEGATION	"a negated literal is an ANTI_MEMBER, placed early"
#define	SCENARIO_PROBE		"a negated literal probes by its named columns"
#define	SCENARIO_RANKING	"passes choose the step-by-step greedy order"
#define	SCENARIO_REFERENCES	"the reference switches change only their choice"
#define	SCENARIO_TERMS		"a scan's columns bind, match or ignore"
#define	SCENARIO_STACK		"a program holds the stack its shape needs"
#define	SCENARIO_TEXTS		"each operation builds its strings above a base"

/* The switches that make the default planner the textual one. */
#define	TEXTUAL_FLAGS		\
	(RELIC_UNSTABLE_NO_INDEX | RELIC_UNSTABLE_NO_REORDER)

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
		    const bool *incomplete);
static bool	 fixture_plan_variants(fixture_t *fixture,
		    size_t rule_index, size_t delta_literal,
		    const bool *incomplete, relic_plan_family_t *family);
static bool	 fixture_plan_variant(fixture_t *fixture, const char *program,
		    unsigned int flags, size_t rule_index,
		    size_t delta_literal);
static bool	 fixture_prepare(fixture_t *fixture,
		    const relic_plan_draft_t *draft);
static bool	 kinds_are(const relic_plan_t *plan,
		    const enum relic_op_kind *expected, size_t count);
static bool	 draft_kinds_are(const relic_plan_draft_t *draft,
		    const enum relic_op_kind *expected, size_t count);
static bool	 ops_kinds_are(const relic_op_t *ops, size_t op_count,
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
		    uint32_t *variables);
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
		    uint64_t *boundp, bool *placed, size_t *order,
		    size_t count);
static bool	 reference_test_ready(relic_t *ctx,
		    const relic_literal_t *literal, uint64_t bound);
static uint64_t	 reference_key_slots(const relic_aggregate_t *aggregate);
static bool	 reference_assigns(const relic_literal_t *literal,
		    uint64_t bound, uint16_t *slotp);
static uint64_t	 reference_slots(const relic_arg_t *args, uint16_t count);
static uint64_t	 reference_expr_slots(const relic_expr_t *expr);
static void	 check_ranked_program(uint32_t *state, unsigned int flags);
static void	 a_comparison_goes_where_its_operands_are_bound(void);
static void	 an_assignment_binds_a_slot_of_its_kind(void);
static void	 an_equality_of_two_literals_variables_assigns(void);
static void	 a_delta_literal_must_be_positive(void);
static void	 a_variant_keeps_both_orders(void);
static void	 a_guard_of_any_size_keeps_the_input_first_plan(void);
static char	*guarded_program(size_t guards, bool permuted);
static bool	 recipe_is(const relic_plan_candidate_t *candidate,
		    const enum relic_cost_kind *expected, size_t count);
static void	 a_textual_variant_keeps_its_written_order(void);
static void	 estimates_never_discard_a_candidate(void);
static void	 the_emit_carries_the_head_schema(void);
static void	 a_bodyless_rule_is_a_lone_emit_of_constants(void);
static void	 a_fully_bound_literal_is_a_membership_test(void);
static void	 a_literal_binding_nothing_read_is_a_check(void);
static void	 a_variable_mentioned_twice_is_not_dead(void);
static void	 a_literal_is_a_check_where_it_is_placed_so(void);
static void	 a_delta_literal_may_be_a_check_in_one_plan(void);
static void	 without_checks_every_literal_is_planned_as_before(void);
static bool	 terms_are(const relic_op_atom_t *atom,
		    const enum relic_term_action *expected);
static void	 a_negated_literal_is_placed_once_bound(void);
static void	 a_negated_literal_probes_by_its_named_columns(void);
static void	 a_relation_being_derived_is_assumed(void);
static void	 an_assumed_relation_divides_per_bound_column(void);
static void	 a_scans_columns_bind_match_or_ignore(void);
static void	 a_bound_argument_outranks_a_smaller_estimate(void);
static void	 bound_arguments_then_small_relations_go_first(void);
static void	 each_binding_pattern_gets_its_access_path(void);
static void	 the_references_change_only_their_choice(void);
static void	 the_ranking_is_the_step_by_step_order(void);
static void	 the_smaller_estimate_goes_first(void);
static void	 a_plan_keeping_every_binding_derives_distinct_rows(void);
static void	 a_fold_goes_where_its_key_is_bound(void);
static void	 a_fold_reads_its_group_by_its_key(void);
static void	 a_program_holds_the_stack_its_shape_needs(void);
static void	 expect_depth(const char *expression, unsigned int depth);
static void	 nested_expression(char *text, size_t size, const char *open,
		    const char *inner, unsigned int levels);
static void	 each_operation_builds_its_strings_above_a_base(void);
static void	 a_fold_in_place_reads_its_key_columns(void);
static void	 a_draft_names_its_indexes_and_builds_none(void);
static const relic_relation_t *draft_relation(const relic_plan_draft_t *draft,
		    size_t op);
static bool	 accesses_are(const relic_plan_draft_t *draft,
		    const size_t *ops, size_t count);
static bool	 positions_bound(const relic_plan_t *plan);
static const relic_op_aggregate_t *fold_planned(fixture_t *fixture,
		    const char *program, unsigned int flags, size_t op);

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
	relic_arena_init(&fixture->arena, RELIC_ARENA_ANY_OBJECT);

	bzero(&config, sizeof(config));
	config.flags = flags;

	fixture->ctx = relic_create_config(&config, sizeof(config));
	if (fixture->ctx == NULL)
		return (false);
	relic_diag_set(fixture->ctx, count_error, fixture);

	if (relic_load_string(fixture->ctx, "test.dl", program) != 0 ||
	    relic_check(fixture->ctx) != 0)
		return (false);

	return (!evaluate || relic_materialize(fixture->ctx) == 0);
}

/*
 * Plan the rule at `rule_index` among the program's rules, in load order,
 * with the relations `incomplete` marks taken to be
 * still derived. Returns whether the plan was built; a test that expects a
 * refusal reads the error count.
 */
static bool
fixture_plan_rule(fixture_t *fixture, size_t rule_index,
    const bool *incomplete)
{
	const relic_rule_t *rule;

	if (rule_index >= fixture->ctx->program.rules.count)
		return (false);

	rule = &fixture->ctx->program.rules.entries[rule_index];
	return (relic_plan_rule(fixture->ctx, &fixture->arena, rule,
	    incomplete, &fixture->plan) == 0);
}

/*
 * Load a program without evaluating it under `flags` and plan one delta
 * variant of one of its rules.  The variant is planned with the relation
 * its delta literal reads marked as still being derived, as the member of a
 * component it would be; a delta literal that is not a positive literal
 * marks nothing, for the planner to refuse.
 */
static bool
fixture_plan_variant(fixture_t *fixture, const char *program,
    unsigned int flags, size_t rule_index, size_t delta_literal)
{
	relic_plan_family_t family;
	const relic_rule_t *rule;
	bool incomplete[8];

	if (!fixture_load(fixture, program, flags, false) ||
	    rule_index >= fixture->ctx->program.rules.count ||
	    relic_pred_count(fixture->ctx) > 8)
		return (false);

	bzero(incomplete, sizeof(incomplete));
	rule = &fixture->ctx->program.rules.entries[rule_index];
	if (delta_literal < rule->body_length &&
	    rule->body[delta_literal].kind == RELIC_LITERAL_POSITIVE)
		incomplete[rule->body[delta_literal].u.atom.pred - 1] = true;

	return (fixture_plan_variants(fixture, rule_index, delta_literal,
	    incomplete, &family));
}

/*
 * Plan the delta variant of the rule at `rule_index` for the body literal at
 * `delta_literal`, the whole family of its plans.
 */
static bool
fixture_plan_variants(fixture_t *fixture, size_t rule_index,
    size_t delta_literal, const bool *incomplete, relic_plan_family_t *family)
{
	const relic_rule_t *rule;

	bzero(family, sizeof(*family));
	if (rule_index >= fixture->ctx->program.rules.count)
		return (false);

	rule = &fixture->ctx->program.rules.entries[rule_index];
	return (relic_plan_variant(fixture->ctx, &fixture->arena, rule,
	    delta_literal, incomplete, family) == 0);
}

static bool
fixture_plan(fixture_t *fixture, const char *program, size_t rule_index)
{
	return (fixture_load(fixture, program, TEXTUAL_FLAGS, false) &&
	    fixture_plan_rule(fixture, rule_index, NULL));
}

static void
fixture_release(fixture_t *fixture)
{
	relic_arena_reset(&fixture->arena);
	relic_destroy(fixture->ctx);
}

/*
 * Make the fixture's plan the plan a candidate's draft prepares to, as an
 * evaluation does when a round first runs it.
 */
static bool
fixture_prepare(fixture_t *fixture, const relic_plan_draft_t *draft)
{
	return (relic_plan_prepare(fixture->ctx, &fixture->arena, draft,
	    &fixture->plan) == 0);
}

/*
 * Whether a plan's, or a draft's, operations are exactly these kinds, in
 * this order.
 */
static bool
kinds_are(const relic_plan_t *plan, const enum relic_op_kind *expected,
    size_t count)
{
	return (ops_kinds_are(plan->ops, plan->op_count, expected, count));
}

static bool
draft_kinds_are(const relic_plan_draft_t *draft,
    const enum relic_op_kind *expected, size_t count)
{
	return (ops_kinds_are(draft->ops, draft->op_count, expected, count));
}

static bool
ops_kinds_are(const relic_op_t *ops, size_t op_count,
    const enum relic_op_kind *expected, size_t count)
{
	size_t i;

	if (op_count != count)
		return (false);

	for (i = 0; i < count; i++) {
		if (ops[i].kind != expected[i])
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

		if (op->kind == RELIC_OP_EMIT)
			name = relic_pred_of(fixture->ctx,
			    op->u.emit.pred)->name.bytes;
		else
			name = relic_pred_of(fixture->ctx,
			    op->u.atom.relation->pred)->name.bytes;
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
		spelling = rule->variables[slot].name.bytes;
		if (strcmp(spelling, name) == 0)
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
	    "p(X, Y) :- e(X, a, _, X, Y).\n", 0)) {
		check(false, SCENARIO_TERMS, "the rule is planned");
		fixture_release(&fixture);
		return;
	}

	check(kinds_are(&fixture.plan, kinds, 2), SCENARIO_TERMS,
	    "the plan is a SCAN and an EMIT");

	x = slot_named(&fixture, 0, "X");
	y = slot_named(&fixture, 0, "Y");
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

	terms = fixture.plan.ops[1].u.emit.terms;
	check(terms[0].action == RELIC_PLAN_BOUND && terms[0].u.slot == x &&
	    terms[1].action == RELIC_PLAN_BOUND && terms[1].u.slot == y,
	    SCENARIO_TERMS, "the EMIT reads X and Y");

	fixture_release(&fixture);
}

/*
 * A literal whose every argument is a constant or a bound slot asks whether a
 * tuple is present, which the full-tuple index answers without a scan.  An
 * atom of arity zero is always fully bound.  A literal with a wildcard is
 * never a membership test: with its other columns bound it asks whether some
 * tuple agrees with them, which is an existence check.
 */
static void
a_fully_bound_literal_is_a_membership_test(void)
{
	static const enum relic_op_kind bound_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_MEMBER, RELIC_OP_MEMBER,
		RELIC_OP_MEMBER, RELIC_OP_EXISTS, RELIC_OP_EMIT
	};
	fixture_t fixture;

	if (!fixture_plan(&fixture, "e(a, b).\nf(b, a).\ng(a).\nready.\n"
	    "p(X) :- e(X, Y), f(Y, X), g(a), ready, f(Y, _).\n", 0)) {
		check(false, SCENARIO_MEMBER, "the rule is planned");
		fixture_release(&fixture);
		return;
	}

	check(kinds_are(&fixture.plan, bound_kinds, 6), SCENARIO_MEMBER,
	    "bound literals are tests and the wildcard literal a check");

	fixture_release(&fixture);
}

/*
 * Whether an operation's terms have exactly these actions, column by
 * column, for as many columns as its relation has.
 */
static bool
terms_are(const relic_op_atom_t *atom, const enum relic_term_action *expected)
{
	uint16_t i;

	for (i = 0; i < atom->arity; i++) {
		if (atom->terms[i].action != expected[i])
			return (false);
	}

	return (true);
}

/*
 * A positive literal whose columns not bound before it hold `_` or a
 * variable the rule mentions nowhere else binds nothing the rule reads, and
 * is an EXISTS keyed on its bound columns.  In p(X) :- q(X), e(X, _, c),
 * written order placing q first, e is keyed on X and c, through the index on
 * exactly those columns, and its middle column is ANY.  A named dead
 * variable, _Y, gives the same plan as `_`.  Without indexes the check asks
 * by scanning, its key empty and its terms unchanged.
 */
static void
a_literal_binding_nothing_read_is_a_check(void)
{
	static const enum relic_op_kind kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_EXISTS, RELIC_OP_EMIT
	};
	static const enum relic_term_action actions[] = {
		RELIC_PLAN_BOUND, RELIC_PLAN_ANY, RELIC_PLAN_CONSTANT
	};
	static const char *const program = "q(a).\ne(a, b, c).\ne(a, d, c).\n"
	    "p(X) :- q(X), e(X, _, c).\n"
	    "p(X) :- q(X), e(X, _Y, c).\n";
	const relic_op_atom_t *check_atom;
	const relic_index_t *index;
	fixture_t fixture;
	size_t r;

	for (r = 0; r < 2; r++) {
		if (!fixture_load(&fixture, program, RELIC_UNSTABLE_NO_REORDER, true) ||
		    !fixture_plan_rule(&fixture, r, NULL) ||
		    !kinds_are(&fixture.plan, kinds, 3)) {
			checkf(false, SCENARIO_EXISTS, "rule %zu is a scan, a "
			    "check and an EMIT", r);
			fixture_release(&fixture);
			continue;
		}

		check_atom = &fixture.plan.ops[1].u.atom;
		index = relic_relation_index_at(check_atom->relation,
		    check_atom->index_position);
		checkf(check_atom->key_mask == UINT32_C(5) &&
		    relic_index_key_mask(index) == UINT32_C(5) &&
		    check_atom->view == RELIC_VIEW_ALL, SCENARIO_EXISTS,
		    "rule %zu is keyed on columns 0 and 2 through their index",
		    r);
		checkf(terms_are(check_atom, actions) &&
		    check_atom->terms[0].u.slot == slot_named(&fixture, r, "X"),
		    SCENARIO_EXISTS, "rule %zu reads X and c and ignores the "
		    "middle column", r);
		fixture_release(&fixture);

		if (!fixture_load(&fixture, program, TEXTUAL_FLAGS, true) ||
		    !fixture_plan_rule(&fixture, r, NULL) ||
		    !kinds_are(&fixture.plan, kinds, 3)) {
			checkf(false, SCENARIO_EXISTS, "rule %zu without indexes "
			    "is a scan, a check and an EMIT", r);
			fixture_release(&fixture);
			continue;
		}

		check_atom = &fixture.plan.ops[1].u.atom;
		checkf(check_atom->key_mask == 0 &&
		    terms_are(check_atom, actions), SCENARIO_EXISTS,
		    "rule %zu without indexes scans for X and c", r);
		fixture_release(&fixture);
	}
}

/*
 * A variable is dead only when the rule mentions it once.  In e2(Y, Y) the
 * literal mentions Y twice, so its scan binds Y at the first column and
 * matches it at the second, with an empty key.  In Y = b, e2(X, Y) the
 * assignment binds Y before the literal, so its column is a key column of a
 * lookup.  A variable an aggregate's body shares with the rule is mentioned
 * by the aggregate, so e2(X, Y) before count { f(Y) } binds Y.
 */
static void
a_variable_mentioned_twice_is_not_dead(void)
{
	static const enum relic_op_kind repeated_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_SCAN, RELIC_OP_EMIT
	};
	static const enum relic_term_action repeated[] = {
		RELIC_PLAN_BIND, RELIC_PLAN_BOUND
	};
	static const enum relic_op_kind assigned_kinds[] = {
		RELIC_OP_ASSIGN, RELIC_OP_LOOKUP, RELIC_OP_EMIT
	};
	static const enum relic_term_action assigned[] = {
		RELIC_PLAN_BIND, RELIC_PLAN_BOUND
	};
	static const enum relic_op_kind shared_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_LOOKUP, RELIC_OP_AGGREGATE, RELIC_OP_EMIT
	};
	static const enum relic_term_action shared[] = {
		RELIC_PLAN_BOUND, RELIC_PLAN_BIND
	};
	static const char *const program = "q(a).\ne2(a, b).\ne2(b, b).\n"
	    "f(b).\n"
	    "r(X) :- q(X), e2(Y, Y).\n"
	    "r(X) :- Y = b, e2(X, Y).\n"
	    "s(X, N) :- q(X), e2(X, Y), N = count { f(Y) }.\n";
	fixture_t fixture;

	if (fixture_load(&fixture, program, RELIC_UNSTABLE_NO_REORDER, true) &&
	    fixture_plan_rule(&fixture, 0, NULL)) {
		check(kinds_are(&fixture.plan, repeated_kinds, 3) &&
		    fixture.plan.ops[1].u.atom.key_mask == 0 &&
		    terms_are(&fixture.plan.ops[1].u.atom, repeated),
		    SCENARIO_LIVE, "e2(Y, Y) binds Y and matches it");
	} else
		check(false, SCENARIO_LIVE, "the repeated rule is planned");
	fixture_release(&fixture);

	if (fixture_load(&fixture, program, RELIC_UNSTABLE_NO_REORDER, true) &&
	    fixture_plan_rule(&fixture, 1, NULL)) {
		check(kinds_are(&fixture.plan, assigned_kinds, 3) &&
		    fixture.plan.ops[1].u.atom.key_mask == UINT32_C(2) &&
		    terms_are(&fixture.plan.ops[1].u.atom, assigned),
		    SCENARIO_LIVE, "the assigned Y is a key column");
	} else
		check(false, SCENARIO_LIVE, "the assigned rule is planned");
	fixture_release(&fixture);

	if (fixture_load(&fixture, program, RELIC_UNSTABLE_NO_REORDER, true) &&
	    fixture_plan_rule(&fixture, fixture.ctx->program.rules.count - 1,
	    NULL)) {
		check(kinds_are(&fixture.plan, shared_kinds, 4) &&
		    terms_are(&fixture.plan.ops[1].u.atom, shared),
		    SCENARIO_LIVE, "the Y the aggregate shares is bound");
	} else
		check(false, SCENARIO_LIVE, "the aggregate's rule is planned");
	fixture_release(&fixture);
}

/*
 * Whether a literal is a check depends on what is bound where it is placed.
 * Alone, r(X) :- e2(X, Z) scans e2, binding X and ignoring the dead Z.
 * Placed after q(X), which the ranking puts first as the smaller relation,
 * the same literal binds nothing the rule reads and is a check keyed on X.
 */
static void
a_literal_is_a_check_where_it_is_placed_so(void)
{
	static const enum relic_op_kind alone_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_EMIT
	};
	static const enum relic_term_action alone[] = {
		RELIC_PLAN_BIND, RELIC_PLAN_ANY
	};
	static const enum relic_op_kind last_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_EXISTS, RELIC_OP_EMIT
	};
	static const char *const last_reads[] = { "q", "e2", "r" };
	static const char *const program = "q(a).\ne2(a, b).\ne2(c, d).\n"
	    "r(X) :- e2(X, Z).\n"
	    "r(X) :- e2(X, Z), q(X).\n";
	fixture_t fixture;

	if (fixture_load(&fixture, program, 0, true) &&
	    fixture_plan_rule(&fixture, 0, NULL)) {
		check(kinds_are(&fixture.plan, alone_kinds, 2) &&
		    terms_are(&fixture.plan.ops[0].u.atom, alone),
		    SCENARIO_CHECK_PLACED, "alone, e2 binds X and ignores Z");
	} else
		check(false, SCENARIO_CHECK_PLACED, "the lone rule is planned");
	fixture_release(&fixture);

	if (fixture_load(&fixture, program, 0, true) &&
	    fixture_plan_rule(&fixture, 1, NULL)) {
		check(kinds_are(&fixture.plan, last_kinds, 3) &&
		    reads_in_order(&fixture, last_reads, 3) &&
		    fixture.plan.ops[1].u.atom.key_mask == UINT32_C(1),
		    SCENARIO_CHECK_PLACED, "after q, e2 is a check keyed on X");
	} else
		check(false, SCENARIO_CHECK_PLACED, "the joined rule is planned");
	fixture_release(&fixture);
}

/*
 * The two plans of a variant place its literals in different orders, so a
 * delta literal can be a check in one and a generator in the other.  In
 * r(X) :- m(X), p(X, _) with p's delta, neither literal has a bound column,
 * so the input-first plan scans m and checks the delta for each X; the
 * delta-first plan scans the delta, binding X, and tests m for it.
 */
static void
a_delta_literal_may_be_a_check_in_one_plan(void)
{
	static const enum relic_op_kind input_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_EXISTS, RELIC_OP_EMIT
	};
	static const enum relic_op_kind first_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_MEMBER, RELIC_OP_EMIT
	};
	static const enum relic_term_action scanned[] = {
		RELIC_PLAN_BIND, RELIC_PLAN_ANY
	};
	relic_plan_family_t family;
	const relic_plan_draft_t *input_first;
	const relic_plan_draft_t *delta_first;
	const relic_rule_t *rule;
	fixture_t fixture;
	bool incomplete[8];
	const relic_op_atom_t *delta;

	if (!fixture_load(&fixture, "m(a).\nm(b).\nm(c).\np(a, x).\n"
	    "r(X) :- m(X), p(X, _).\n", 0, true) ||
	    relic_pred_count(fixture.ctx) > 8) {
		check(false, SCENARIO_CHECK_VARIANT, "the program is loaded");
		fixture_release(&fixture);
		return;
	}

	bzero(incomplete, sizeof(incomplete));
	rule = &fixture.ctx->program.rules.entries[0];
	incomplete[rule->body[1].u.atom.pred - 1] = true;
	if (!fixture_plan_variants(&fixture, 0, 1, incomplete, &family) ||
	    family.count != 2) {
		check(false, SCENARIO_CHECK_VARIANT, "the variant is planned "
		    "with two plans");
		fixture_release(&fixture);
		return;
	}

	input_first = &family.candidates[RELIC_FAMILY_INPUT_FIRST].draft;
	delta_first = &family.candidates[RELIC_FAMILY_DELTA_FIRST].draft;
	if (draft_kinds_are(input_first, input_kinds, 3)) {
		delta = &input_first->ops[1].u.atom;
		check(delta->view == RELIC_VIEW_DELTA &&
		    delta->key_mask == UINT32_C(1), SCENARIO_CHECK_VARIANT,
		    "the input-first plan checks the delta for X");
	} else
		check(false, SCENARIO_CHECK_VARIANT,
		    "the input-first plan is a scan, a check and an EMIT");
	if (draft_kinds_are(delta_first, first_kinds, 3)) {
		delta = &delta_first->ops[0].u.atom;
		check(delta->view == RELIC_VIEW_DELTA &&
		    terms_are(delta, scanned), SCENARIO_CHECK_VARIANT,
		    "the delta-first plan scans the delta, binding X");
	} else
		check(false, SCENARIO_CHECK_VARIANT,
		    "the delta-first plan is a scan, a member and an EMIT");

	fixture_release(&fixture);
}

/*
 * Under RELIC_UNSTABLE_NO_EXISTS no variable is dead and no literal is a check:
 * a literal binding nothing the rule reads is a lookup, `_` matching anything
 * and a named variable binding its slot, which nothing reads.
 */
static void
without_checks_every_literal_is_planned_as_before(void)
{
	static const enum relic_op_kind kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_LOOKUP, RELIC_OP_EMIT
	};
	static const enum relic_term_action wildcard[] = {
		RELIC_PLAN_BOUND, RELIC_PLAN_ANY, RELIC_PLAN_CONSTANT
	};
	static const enum relic_term_action named[] = {
		RELIC_PLAN_BOUND, RELIC_PLAN_BIND, RELIC_PLAN_CONSTANT
	};
	static const char *const program = "q(a).\ne(a, b, c).\ne(a, d, c).\n"
	    "p(X) :- q(X), e(X, _, c).\n"
	    "p(X) :- q(X), e(X, _Y, c).\n";
	fixture_t fixture;
	size_t r;

	for (r = 0; r < 2; r++) {
		if (fixture_load(&fixture, program,
		    RELIC_UNSTABLE_NO_EXISTS | RELIC_UNSTABLE_NO_REORDER,
		    true) &&
		    fixture_plan_rule(&fixture, r, NULL)) {
			checkf(kinds_are(&fixture.plan, kinds, 3) &&
			    fixture.plan.ops[1].u.atom.key_mask ==
			    UINT32_C(5) && terms_are(&fixture.plan.ops[1].u.atom,
			    r == 0 ? wildcard : named), SCENARIO_NO_EXISTS,
			    "rule %zu looks e up by X and c", r);
		} else
			checkf(false, SCENARIO_NO_EXISTS, "rule %zu is planned",
			    r);
		fixture_release(&fixture);
	}
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
	    "p(X) :- N >= 0, X <= M, 1 < 2, e(X, N), X != b, f(M).\n", 0)) {
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
 * An assignment goes where its value's slots are bound, whatever its written
 * position, and one it makes placeable goes a pass later, after those
 * placeable already: C = B * 2 waits for B = A + 1 and follows D = A,
 * although written first.  A slot assigned an expression, or another number
 * slot, is a number slot, which the EMIT reads as a number; one assigned a
 * domain slot is a domain slot, read as any bound slot is.  A plan that
 * evaluates an operator computes, and one that only copies does not.
 */
static void
an_assignment_binds_a_slot_of_its_kind(void)
{
	static const enum relic_op_kind kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_ASSIGN, RELIC_OP_ASSIGN,
		RELIC_OP_ASSIGN, RELIC_OP_ASSIGN, RELIC_OP_ASSIGN, RELIC_OP_EMIT
	};
	static const char *const names[] = { "B", "D", "C", "E", "F" };
	static const bool numbers[] = { true, false, true, false, true };
	static const char *const program = "n(1).\n"
	    "h(C, D, E, F) :- C = B * 2, n(A), B = A + 1, D = A, E = D, "
	    "F = C.\n"
	    "g(Y) :- n(X), Y = X.\n";
	enum relic_term_action emitted[4];
	const relic_op_t *op;
	fixture_t fixture;
	size_t i;

	if (!fixture_plan(&fixture, program, 0)) {
		check(false, SCENARIO_ASSIGN, "the rule is planned");
		fixture_release(&fixture);
		return;
	}

	check(kinds_are(&fixture.plan, kinds, 7), SCENARIO_ASSIGN,
	    "a scan and five assignments precede the EMIT");
	for (i = 0; i < 5 && fixture.plan.op_count == 7; i++) {
		op = &fixture.plan.ops[i + 1];
		checkf(op->u.assign.slot == slot_named(&fixture, 0, names[i]),
		    SCENARIO_ASSIGN, "assignment %zu should bind %s", i,
		    names[i]);
		checkf((op->u.assign.kind == RELIC_SLOT_NUMBER) == numbers[i],
		    SCENARIO_ASSIGN, "%s should be a %s slot", names[i],
		    numbers[i] ? "number" : "domain");
	}

	if (fixture.plan.op_count == 7) {
		for (i = 0; i < 4; i++)
			emitted[i] = fixture.plan.ops[6].u.emit.terms[i].action;
		check(emitted[0] == RELIC_PLAN_NUMBER &&
		    emitted[1] == RELIC_PLAN_BOUND &&
		    emitted[2] == RELIC_PLAN_BOUND &&
		    emitted[3] == RELIC_PLAN_NUMBER, SCENARIO_ASSIGN,
		    "the EMIT reads C and F as numbers, D and E as bound");
	}
	check(relic_plan_computes(&fixture.plan), SCENARIO_ASSIGN,
	    "a plan evaluating an operator computes");
	fixture_release(&fixture);

	if (!fixture_plan(&fixture, program, 1)) {
		check(false, SCENARIO_ASSIGN, "the copying rule is planned");
		fixture_release(&fixture);
		return;
	}
	check(fixture.plan.op_count == 3 &&
	    fixture.plan.ops[1].kind == RELIC_OP_ASSIGN &&
	    fixture.plan.ops[1].u.assign.kind == RELIC_SLOT_DOMAIN &&
	    !relic_plan_computes(&fixture.plan), SCENARIO_ASSIGN,
	    "a plan that copies a slot binds a domain slot and computes "
	    "nothing");
	fixture_release(&fixture);
}

/*
 * In p(X), q(Y), X = Y the equality is placed once p(X) binds X, before
 * q(Y) binds Y, so it assigns Y, and q(Y), fully bound, is a membership
 * test rather than a scan compared afterwards.
 */
static void
an_equality_of_two_literals_variables_assigns(void)
{
	static const enum relic_op_kind kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_ASSIGN, RELIC_OP_MEMBER, RELIC_OP_EMIT
	};
	fixture_t fixture;

	if (!fixture_plan(&fixture, "p(a).\nq(a).\n"
	    "r(X) :- p(X), q(Y), X = Y.\n", 0)) {
		check(false, SCENARIO_EQUALITY, "the rule is planned");
		fixture_release(&fixture);
		return;
	}

	check(kinds_are(&fixture.plan, kinds, 4), SCENARIO_EQUALITY,
	    "the equality assigns Y and q(Y) is a membership test");
	check(fixture.plan.op_count == 4 &&
	    fixture.plan.ops[1].u.assign.slot == slot_named(&fixture, 0, "Y") &&
	    fixture.plan.ops[1].u.assign.kind == RELIC_SLOT_DOMAIN,
	    SCENARIO_EQUALITY,
	    "Y is a domain slot");

	fixture_release(&fixture);
}

/*
 * The textual planner places positive literals as they are written, the
 * delta literal among them, which is the textbook semi-naive scheme: one
 * plan, which every round runs.
 *
 * In p(X, Z) :- p(X, Y), p(Y, Z), with p still being derived and the second
 * literal as the delta, the first literal scans p's old view and binds X
 * and Y, and the delta is scanned matching Y and binding Z.  With the first
 * literal as the delta, the second reads p's current view, being written
 * after it.
 *
 * In r(X) :- e(X, Y), e(a, b), X != Y with the constant literal as the
 * delta, e(X, Y), written before it, reads e's old view, the comparison
 * follows the scan that binds its operands, and the constant literal is a
 * MEMBER of the delta.
 */
static void
a_textual_variant_keeps_its_written_order(void)
{
	static const enum relic_op_kind recursive_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_SCAN, RELIC_OP_EMIT
	};
	static const enum relic_op_kind constant_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_FILTER, RELIC_OP_MEMBER, RELIC_OP_EMIT
	};
	static const char *const recursive = "e(a, b).\n"
	    "p(X, Y) :- e(X, Y).\n"
	    "p(X, Z) :- p(X, Y), p(Y, Z).\n";
	relic_plan_family_t family;
	fixture_t fixture;
	const relic_op_atom_t *old;
	const relic_op_atom_t *delta;
	bool incomplete[8];
	uint16_t x;
	uint16_t y;
	uint16_t z;

	bzero(incomplete, sizeof(incomplete));
	if (!fixture_load(&fixture, recursive, TEXTUAL_FLAGS, false) ||
	    relic_pred_count(fixture.ctx) > 8) {
		check(false, SCENARIO_WRITTEN, "the recursive rule is loaded");
		fixture_release(&fixture);
		return;
	}
	incomplete[fixture.ctx->program.rules.entries[1].head - 1] = true;

	check(fixture_plan_variants(&fixture, 1, 0, incomplete, &family) &&
	    family.candidates[0].draft.ops[0].u.atom.view == RELIC_VIEW_DELTA &&
	    family.candidates[0].draft.ops[1].u.atom.view == RELIC_VIEW_CURRENT,
	    SCENARIO_WRITTEN, "a member literal after the delta literal reads "
	    "the current view");

	if (fixture_plan_variants(&fixture, 1, 1, incomplete, &family) &&
	    fixture_prepare(&fixture, &family.candidates[0].draft)) {
		check(kinds_are(&fixture.plan, recursive_kinds, 3) &&
		    family.count == 1 &&
		    family.candidates[0].recipe.step_count == 0,
		    SCENARIO_WRITTEN, "the variant is one plan of two SCANs "
		    "and an EMIT, with no recipe to choose by");

		x = slot_named(&fixture, 1, "X");
		y = slot_named(&fixture, 1, "Y");
		z = slot_named(&fixture, 1, "Z");
		old = &fixture.plan.ops[0].u.atom;
		delta = &fixture.plan.ops[1].u.atom;

		check(old->view == RELIC_VIEW_OLD &&
		    delta->view == RELIC_VIEW_DELTA, SCENARIO_WRITTEN,
		    "the member literal written before the delta reads the old "
		    "view, and the delta literal the delta");
		check(old->terms[0].action == RELIC_PLAN_BIND &&
		    old->terms[0].u.slot == x &&
		    old->terms[1].action == RELIC_PLAN_BIND &&
		    old->terms[1].u.slot == y, SCENARIO_WRITTEN,
		    "p(X, Y) binds X and Y");
		check(delta->terms[0].action == RELIC_PLAN_BOUND &&
		    delta->terms[0].u.slot == y &&
		    delta->terms[1].action == RELIC_PLAN_BIND &&
		    delta->terms[1].u.slot == z, SCENARIO_WRITTEN,
		    "the delta literal p(Y, Z) matches Y and binds Z");
	} else {
		check(false, SCENARIO_WRITTEN, "the recursive variant is "
		    "planned");
	}
	fixture_release(&fixture);

	if (fixture_load(&fixture, "e(a, b).\n"
	    "r(X) :- e(X, Y), e(a, b), X != Y.\n", TEXTUAL_FLAGS, false) &&
	    relic_pred_count(fixture.ctx) <= 8) {
		bzero(incomplete, sizeof(incomplete));
		incomplete[fixture.ctx->program.rules.entries[0].body[0].u.atom.
		    pred - 1] = true;
		if (fixture_plan_variants(&fixture, 0, 1, incomplete,
		    &family) && family.count == 1 &&
		    fixture_prepare(&fixture, &family.candidates[0].draft)) {
			check(kinds_are(&fixture.plan, constant_kinds, 4) &&
			    fixture.plan.ops[0].u.atom.view ==
			    RELIC_VIEW_OLD &&
			    fixture.plan.ops[2].u.atom.view ==
			    RELIC_VIEW_DELTA, SCENARIO_WRITTEN, "the constant "
			    "literal is a member of the delta, after the old "
			    "view is scanned and compared");
		} else {
			check(false, SCENARIO_WRITTEN, "the constant variant "
			    "is planned");
		}
	} else {
		check(false, SCENARIO_WRITTEN, "the constant variant is "
		    "loaded");
	}
	fixture_release(&fixture);

	check(fixture_plan(&fixture, "e(a, b).\n"
	    "r(X) :- e(X, Y), e(a, b), X != Y.\n", 0) &&
	    fixture.plan.ops[2].kind == RELIC_OP_MEMBER &&
	    fixture.plan.ops[0].u.atom.view == RELIC_VIEW_ALL &&
	    fixture.plan.ops[2].u.atom.view == RELIC_VIEW_ALL,
	    SCENARIO_WRITTEN, "without a delta the constant literal is a "
	    "membership test");
	fixture_release(&fixture);
}

/*
 * A delta variant keeps two plans: one with its delta literal placed first,
 * and one ordered like any other rule except that the delta literal loses
 * every tie on bound columns to the literals it could follow.
 *
 *	m(a).  m(b).
 *	par(a, c).  par(a, d).  par(b, c).  par(b, d).  par(c, e).  par(d, e).
 *	tc(X, Y) :- m(X), par(X, Y).
 *	tc(X, Z) :- m(X), par(X, Y), tc(Y, Z).
 *
 * Input-first, m and par outrank the unbound delta whatever it is assumed
 * to hold; m, measured at 2, goes before par at 6 and binds X, par follows
 * with X bound, and the delta comes last, looked up on Y: the loop nest a
 * magic relation guarding its rule calls for.  Delta-first, the delta is
 * scanned, par looked up on Y and m tested.  Each plan has a recipe of one
 * step per operation, counted as its operation is.
 *
 * Without indexes a delta placed after other literals would be scanned
 * whole once for each binding that reaches it, where the delta-first plan
 * scans it once, so under RELIC_UNSTABLE_NO_INDEX the variant has the
 * delta-first plan alone, and no recipe.
 *
 * A fully bound delta literal outranks every other literal under both
 * policies, so both place it first as a MEMBER of the delta, and the one
 * order is kept once.  In p(X, Z) :- p(X, Y), p(Y, Z) the delta and the
 * other member literal tie on bound columns, so the policies part: the
 * delta-first plan scans the delta and looks the other view up, and the
 * input-first plan scans the other view and looks the delta up.
 */
static void
a_variant_keeps_both_orders(void)
{
	static const enum relic_op_kind input_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_LOOKUP, RELIC_OP_LOOKUP, RELIC_OP_EMIT
	};
	static const enum relic_op_kind first_kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_LOOKUP, RELIC_OP_MEMBER, RELIC_OP_EMIT
	};
	static const enum relic_cost_kind input_costs[] = {
		RELIC_COST_SCAN, RELIC_COST_LOOKUP, RELIC_COST_LOOKUP,
		RELIC_COST_EMIT
	};
	static const enum relic_cost_kind first_costs[] = {
		RELIC_COST_SCAN, RELIC_COST_LOOKUP, RELIC_COST_PROBE,
		RELIC_COST_EMIT
	};
	static const char *const input_reads[] = { "m", "par", "tc", "tc" };
	static const char *const program = "m(a).\nm(b).\n"
	    "par(a, c).\npar(a, d).\npar(b, c).\npar(b, d).\n"
	    "par(c, e).\npar(d, e).\n"
	    "tc(X, Y) :- m(X), par(X, Y).\n"
	    "tc(X, Z) :- m(X), par(X, Y), tc(Y, Z).\n";
	relic_plan_family_t family;
	const relic_plan_candidate_t *candidate;
	fixture_t fixture;
	bool incomplete[8];
	const relic_op_atom_t *delta;
	const relic_plan_draft_t *draft;

	bzero(incomplete, sizeof(incomplete));
	if (!fixture_load(&fixture, program, 0, true) ||
	    relic_pred_count(fixture.ctx) > 8) {
		check(false, SCENARIO_FAMILY, "the guarded closure is loaded");
		fixture_release(&fixture);
		return;
	}
	incomplete[fixture.ctx->program.rules.entries[1].head - 1] = true;

	if (fixture_plan_variants(&fixture, 1, 2, incomplete, &family) &&
	    family.count == 2 && fixture_prepare(&fixture,
	    &family.candidates[RELIC_FAMILY_INPUT_FIRST].draft)) {
		candidate = &family.candidates[RELIC_FAMILY_INPUT_FIRST];
		delta = &fixture.plan.ops[2].u.atom;
		check(kinds_are(&fixture.plan, input_kinds, 4) &&
		    reads_in_order(&fixture, input_reads, 4), SCENARIO_FAMILY,
		    "input-first, the guard and its join go before the delta");
		check(delta->view == RELIC_VIEW_DELTA && delta->key_mask == 1u,
		    SCENARIO_FAMILY, "input-first, the delta is looked up on "
		    "its first column");
		check(recipe_is(candidate, input_costs, 4), SCENARIO_FAMILY,
		    "the input-first recipe counts each of its operations");

		candidate = &family.candidates[RELIC_FAMILY_DELTA_FIRST];
		draft = &candidate->draft;
		check(draft_kinds_are(draft, first_kinds, 4) &&
		    draft->ops[0].u.atom.view == RELIC_VIEW_DELTA &&
		    draft->ops[1].u.atom.key_mask == 2u,
		    SCENARIO_FAMILY, "delta-first, the delta is scanned and "
		    "par looked up on its second column");
		check(recipe_is(candidate, first_costs, 4), SCENARIO_FAMILY,
		    "the delta-first recipe counts each of its operations");
	} else {
		check(false, SCENARIO_FAMILY, "the variant's two plans are "
		    "built");
	}
	fixture_release(&fixture);

	if (fixture_load(&fixture, program, RELIC_UNSTABLE_NO_INDEX, true) &&
	    relic_pred_count(fixture.ctx) <= 8) {
		check(fixture_plan_variants(&fixture, 1, 2, incomplete,
		    &family) && family.count == 1 &&
		    family.candidates[0].recipe.step_count == 0 &&
		    family.candidates[0].draft.ops[0].u.atom.view ==
		    RELIC_VIEW_DELTA,
		    SCENARIO_FAMILY, "without indexes a delta placed later "
		    "would be scanned once per binding before it, so the "
		    "variant is its delta-first plan alone");
	} else {
		check(false, SCENARIO_FAMILY, "the guarded closure is loaded "
		    "without indexes");
	}
	fixture_release(&fixture);

	if (fixture_load(&fixture, "e(a, b).\np(X, Y) :- e(X, Y).\n"
	    "p(X, Z) :- p(X, Y), p(Y, Z).\n"
	    "r(X) :- p(X, Y), p(a, b).\n", 0, false) &&
	    relic_pred_count(fixture.ctx) <= 8) {
		bzero(incomplete, sizeof(incomplete));
		incomplete[fixture.ctx->program.rules.entries[0].head - 1] =
		    true;
		check(fixture_plan_variants(&fixture, 1, 1, incomplete,
		    &family) && family.count == 2 &&
		    family.candidates[RELIC_FAMILY_DELTA_FIRST].draft.ops[0].
		    u.atom.view == RELIC_VIEW_DELTA &&
		    family.candidates[RELIC_FAMILY_DELTA_FIRST].draft.ops[1].
		    u.atom.view == RELIC_VIEW_OLD &&
		    family.candidates[RELIC_FAMILY_INPUT_FIRST].draft.ops[0].
		    u.atom.view == RELIC_VIEW_OLD &&
		    family.candidates[RELIC_FAMILY_INPUT_FIRST].draft.ops[1].
		    u.atom.view == RELIC_VIEW_DELTA,
		    SCENARIO_FAMILY, "against the old view, the self-join "
		    "keeps the delta first and the old view first");
		check(fixture_plan_variants(&fixture, 1, 0, incomplete,
		    &family) && family.count == 2 &&
		    family.candidates[RELIC_FAMILY_DELTA_FIRST].draft.ops[1].
		    u.atom.view == RELIC_VIEW_CURRENT &&
		    family.candidates[RELIC_FAMILY_INPUT_FIRST].draft.ops[0].
		    u.atom.view == RELIC_VIEW_CURRENT,
		    SCENARIO_FAMILY, "against the current view, the self-join "
		    "keeps both orders too");
		check(fixture_plan_variants(&fixture, 2, 1, incomplete,
		    &family) && family.count == 1 &&
		    family.candidates[0].recipe.step_count == 0 &&
		    family.candidates[0].draft.ops[0].kind == RELIC_OP_MEMBER &&
		    family.candidates[0].draft.ops[0].u.atom.view ==
		    RELIC_VIEW_DELTA,
		    SCENARIO_FAMILY, "a fully bound delta literal is a MEMBER "
		    "of the delta, first in the one order both policies "
		    "reach");
	} else {
		check(false, SCENARIO_FAMILY, "the self-join is loaded");
	}
	fixture_release(&fixture);
}

/*
 * Whether a candidate's recipe has a step for each of its plan's `count`
 * operations, in order, counted as `expected` says.
 */
static bool
recipe_is(const relic_plan_candidate_t *candidate,
    const enum relic_cost_kind *expected, size_t count)
{
	const relic_cost_recipe_t *recipe;
	size_t i;

	recipe = &candidate->recipe;
	if (recipe->step_count != count || candidate->draft.op_count != count)
		return (false);

	for (i = 0; i < count; i++) {
		if (recipe->steps[i].op != &candidate->draft.ops[i] ||
		    recipe->steps[i].kind != expected[i])
			return (false);
	}

	return (true);
}

/*
 * The guarded closure of a_variant_keeps_both_orders() over `guards`
 * guard tuples, each of which has two par successors that have none, so
 * that materializing it is cheap at any size.  Permuted, the guard is
 * named zz rather than m and the recursive rule's body is written in the
 * reverse order, the delta literal first.  Returns the program, which the
 * caller frees, or NULL when it could not be allocated.
 */
static char *
guarded_program(size_t guards, bool permuted)
{
	static const char *const rules[2] = {
		"tc(X, Y) :- m(X), par(X, Y).\n"
		"tc(X, Z) :- m(X), par(X, Y), tc(Y, Z).\n",
		"tc(X, Y) :- zz(X), par(X, Y).\n"
		"tc(X, Z) :- tc(Y, Z), par(X, Y), zz(X).\n"
	};
	char *program;
	size_t capacity;
	size_t length;
	size_t i;
	int written;

	capacity = guards * 96 + 256;
	program = malloc(capacity);
	if (program == NULL)
		return (NULL);

	written = snprintf(program, capacity, "%s", rules[permuted]);
	length = (size_t)written;
	for (i = 0; i < guards && written >= 0; i++) {
		written = snprintf(program + length, capacity - length,
		    "%s(%zu).\npar(%zu, %zu).\npar(%zu, %zu).\n",
		    permuted ? "zz" : "m", i, i, i + 100000, i, i + 200000);
		if (written < 0 || (size_t)written >= capacity - length) {
			free(program);
			return (NULL);
		}
		length += (size_t)written;
	}

	return (program);
}

/*
 * Whether the input-first plan exists does not depend on what the delta is
 * assumed to hold.  The delta loses ties on bound columns before any
 * estimate is compared, so a guard measured below, at or above the
 * RELIC_PLAN_UNKNOWN_TUPLES the planner assumes of a relation it is still
 * deriving goes first in the input-first plan, and the delta is looked up
 * last.  Neither the guard's name nor where the delta literal is written
 * makes the difference.
 */
static void
a_guard_of_any_size_keeps_the_input_first_plan(void)
{
	static const size_t sizes[] = {
		RELIC_PLAN_UNKNOWN_TUPLES - 1, RELIC_PLAN_UNKNOWN_TUPLES,
		RELIC_PLAN_UNKNOWN_TUPLES + 1, 2 * RELIC_PLAN_UNKNOWN_TUPLES
	};
	static const char *const reads[2][4] = {
		{ "m", "par", "tc", "tc" },
		{ "zz", "par", "tc", "tc" }
	};
	relic_plan_family_t family;
	const relic_op_atom_t *delta;
	fixture_t fixture;
	bool incomplete[8];
	char *program;
	size_t delta_literal;
	size_t i;
	size_t p;

	for (p = 0; p < 2; p++) {
		delta_literal = p == 0 ? 2 : 0;
		for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
			program = guarded_program(sizes[i], p == 1);
			if (program == NULL) {
				check(false, SCENARIO_GUARD, "the program is "
				    "allocated");
				return;
			}
			if (!fixture_load(&fixture, program, 0, true) ||
			    relic_pred_count(fixture.ctx) > 8) {
				checkf(false, SCENARIO_GUARD, "the closure "
				    "over %zu guards is loaded", sizes[i]);
				fixture_release(&fixture);
				free(program);
				continue;
			}
			free(program);

			bzero(incomplete, sizeof(incomplete));
			incomplete[fixture.ctx->program.rules.entries[1].
			    head - 1] = true;
			if (fixture_plan_variants(&fixture, 1, delta_literal,
			    incomplete, &family) && family.count == 2 &&
			    fixture_prepare(&fixture, &family.candidates[
			    RELIC_FAMILY_INPUT_FIRST].draft)) {
				delta = &fixture.plan.ops[2].u.atom;
				checkf(reads_in_order(&fixture, reads[p], 4) &&
				    fixture.plan.ops[2].kind ==
				    RELIC_OP_LOOKUP &&
				    delta->view == RELIC_VIEW_DELTA,
				    SCENARIO_GUARD, "over %zu guards, %s, "
				    "the input-first plan reads the guard "
				    "first and looks the delta up last",
				    sizes[i], p == 0 ? "as written" :
				    "renamed and permuted");
			} else {
				checkf(false, SCENARIO_GUARD, "over %zu "
				    "guards, %s, the variant keeps two plans",
				    sizes[i], p == 0 ? "as written" :
				    "renamed and permuted");
			}
			fixture_release(&fixture);
		}
	}
}

/*
 * A candidate is kept whatever its work would be estimated at: an
 * estimate is approximate, and the sizes it is made from change from
 * round to round.
 *
 *	loop(X) :- e(X, X).
 *	r(a, Z) :- e(a, Y), r(Y, Z), loop(Z).
 *	r(X, Z) :- loop(X), e(X, Y), r(Y, Z).
 *
 * With no loops in e, loop is empty.  In the first rule the delta-first
 * plan tests each delta tuple against the empty loop, and in the second
 * the input-first plan scans the empty loop first and reads nothing more.
 * Each variant still keeps both of its plans, each with its recipe.
 */
static void
estimates_never_discard_a_candidate(void)
{
	relic_plan_family_t family;
	fixture_t fixture;
	bool incomplete[8];
	size_t rule;

	bzero(incomplete, sizeof(incomplete));
	if (!fixture_load(&fixture, "e(a, b).\ne(b, c).\ne(c, d).\n"
	    "loop(X) :- e(X, X).\n"
	    "r(X, Y) :- e(X, Y).\n"
	    "r(a, Z) :- e(a, Y), r(Y, Z), loop(Z).\n"
	    "r(X, Z) :- loop(X), e(X, Y), r(Y, Z).\n", 0, true) ||
	    relic_pred_count(fixture.ctx) > 8) {
		check(false, SCENARIO_RETAINED, "the program is loaded");
		fixture_release(&fixture);
		return;
	}
	incomplete[fixture.ctx->program.rules.entries[1].head - 1] = true;

	for (rule = 2; rule <= 3; rule++) {
		checkf(fixture_plan_variants(&fixture, rule, rule == 2 ? 1 : 2,
		    incomplete, &family) && family.count == 2 &&
		    family.candidates[0].recipe.step_count ==
		    family.candidates[0].draft.op_count &&
		    family.candidates[1].recipe.step_count ==
		    family.candidates[1].draft.op_count, SCENARIO_RETAINED,
		    "rule %zu keeps both plans beside an empty relation, each "
		    "with its recipe", rule);
	}
	fixture_release(&fixture);
}

/*
 * A delta literal that is not a positive literal of the rule -- past the end
 * of the body, a comparison, or a negated literal -- is refused with one
 * error rather than read as an atom it is not, and so is one over a relation
 * the planner is not told is being derived, which would leave the variant's
 * other member literals reading the whole relation, and a variant asked for
 * with no delta literal at all.  Each of the three families a variant can
 * have -- the written order, the delta-first order alone without indexes,
 * and both orders -- refuses alike, and holds no candidate after.
 */
static void
a_delta_literal_must_be_positive(void)
{
	static const char *const program = "e(a, b).\n"
	    "r(X) :- e(X, Y), X != Y.\n";
	static const unsigned int flag_sets[] = {
		TEXTUAL_FLAGS, RELIC_UNSTABLE_NO_INDEX, 0
	};
	relic_plan_family_t family;
	fixture_t fixture;
	size_t i;

	for (i = 0; i < sizeof(flag_sets) / sizeof(flag_sets[0]); i++) {
		checkf(!fixture_plan_variant(&fixture, program, flag_sets[i],
		    0, 2) && fixture.errors == 1, SCENARIO_DELTA_REFUSED,
		    "a literal past the body is refused with one error, "
		    "flags %#x", flag_sets[i]);
		fixture_release(&fixture);

		checkf(!fixture_plan_variant(&fixture, program, flag_sets[i],
		    0, 1) && fixture.errors == 1, SCENARIO_DELTA_REFUSED,
		    "a comparison is refused with one error, flags %#x",
		    flag_sets[i]);
		fixture_release(&fixture);

		checkf(!fixture_plan_variant(&fixture, "e(a, b).\nf(b).\n"
		    "r(X) :- e(X, Y), not f(Y).\n", flag_sets[i], 0, 1) &&
		    fixture.errors == 1, SCENARIO_DELTA_REFUSED,
		    "a negated literal is refused with one error, flags %#x",
		    flag_sets[i]);
		fixture_release(&fixture);

		checkf(fixture_load(&fixture, program, flag_sets[i], false) &&
		    !fixture_plan_variants(&fixture, 0, 0, NULL, &family) &&
		    fixture.errors == 1 && family.count == 0,
		    SCENARIO_DELTA_REFUSED,
		    "a delta literal over a relation not being derived is "
		    "refused with one error, flags %#x", flag_sets[i]);
		fixture_release(&fixture);
	}

	check(fixture_load(&fixture, program, 0, false) &&
	    !fixture_plan_variants(&fixture, 0, RELIC_PLAN_NO_DELTA, NULL,
	    &family) && fixture.errors == 1 && family.count == 0,
	    SCENARIO_DELTA_REFUSED,
	    "a variant with no delta literal is refused with one error");
	fixture_release(&fixture);
}

/* The EMIT schema is independent of any physical destination. */
static void
the_emit_carries_the_head_schema(void)
{
	fixture_t fixture;
	const relic_rule_t *rule;
	const relic_op_t *emit;

	if (!fixture_load(&fixture, "e(a, b).\nr(X) :- e(X, _).\n",
	    TEXTUAL_FLAGS, false)) {
		check(false, SCENARIO_EMIT_TARGET, "the program loads");
		fixture_release(&fixture);
		return;
	}
	rule = &fixture.ctx->program.rules.entries[0];
	check(relic_plan_rule(fixture.ctx, &fixture.arena, rule,
	    NULL, &fixture.plan) == 0,
	    SCENARIO_EMIT_TARGET, "a rule plans without a destination");
	emit = &fixture.plan.ops[fixture.plan.op_count - 1];
	check(emit->kind == RELIC_OP_EMIT && relic_op_atom(emit) == NULL &&
	    emit->u.emit.arity == 1 && emit->u.emit.terms != NULL &&
	    emit->u.emit.pred == rule->head,
	    SCENARIO_EMIT_TARGET, "the EMIT carries the head and no relation");
	fixture_release(&fixture);
}

/*
 * A rule with no body, as the Magic rewrite generates its seeds, plans to an
 * EMIT of its head's constants and nothing else: its turn inserts its tuple
 * with no join.  A program's facts are not rules and are never planned.
 */
static void
a_bodyless_rule_is_a_lone_emit_of_constants(void)
{
	static const enum relic_op_kind kinds[] = { RELIC_OP_EMIT };
	fixture_t fixture;
	relic_relation_t *target;
	const relic_plan_term_t *terms;
	relic_arg_t args[2];
	relic_rule_t rule;

	bzero(args, sizeof(args));
	bzero(&rule, sizeof(rule));
	args[0].kind = RELIC_ARG_CONSTANT;
	args[1].kind = RELIC_ARG_CONSTANT;

	target = NULL;
	if (fixture_load(&fixture, "e(X, Y) :- f(X, Y).\n", TEXTUAL_FLAGS,
	    false) &&
	    relic_value_intern_bytes(fixture.ctx, RELIC_SYMBOL, "a", 1,
	    &args[0].u.constant) == 0 &&
	    relic_value_intern_integer(fixture.ctx, 7,
	    &args[1].u.constant) == 0) {
		rule.head = fixture.ctx->program.rules.entries[0].head;
		rule.head_arity = 2;
		rule.head_args = args;
		rule.origin = RELIC_ORIGIN_QUERY;
		target = relic_relation_written(fixture.ctx, rule.head);
	}

	if (target == NULL || relic_plan_rule(fixture.ctx, &fixture.arena,
	    &rule, NULL, &fixture.plan) != 0) {
		check(false, SCENARIO_FACT, "the rule is planned");
		fixture_release(&fixture);
		return;
	}

	check(kinds_are(&fixture.plan, kinds, 1), SCENARIO_FACT,
	    "the plan is one EMIT");

	terms = fixture.plan.ops[0].u.emit.terms;
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
	    "not frozen.\n", 0)) {
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
	    f->terms[0].u.slot == slot_named(&fixture, 0, "Y"),
	    SCENARIO_NEGATION, "not f(Y) reads the Y the scan bound");
	check(g->terms[0].action == RELIC_PLAN_BOUND &&
	    g->terms[0].u.slot == slot_named(&fixture, 0, "X"),
	    SCENARIO_NEGATION, "not g(X) reads the X the scan bound");
	check(done->view == RELIC_VIEW_ALL && f->view == RELIC_VIEW_ALL,
	    SCENARIO_NEGATION, "no negated literal reads a delta");

	fixture_release(&fixture);
}

/*
 * A negated literal's ANTI_MEMBER is keyed on the columns that are not `_`
 * and reads no `_`: every column is a key the canonical index answers, some
 * columns are the key of the index the relation built on exactly them, and
 * none is a probe that scans.  Without indexes, a key of some columns is
 * dropped and the probe scans, matching the named columns by its terms.  A
 * relation of no columns is probed through the canonical index, its every
 * column being none.
 */
static void
a_negated_literal_probes_by_its_named_columns(void)
{
	static const struct {
		const char		*literal;
		unsigned int		 flags;
		uint32_t		 wildcards;	/* the `_` columns */
		uint32_t		 key_mask;
		enum relic_probe_shape	 shape;
	} cases[] = {
		{ "not t(X, b, c)", 0, 0, 7, RELIC_PROBE_MEMBER },
		{ "not t(_, X, _)", 0, 5, 2, RELIC_PROBE_INDEX },
		{ "not t(a, _, X)", 0, 2, 5, RELIC_PROBE_INDEX },
		{ "not t(_, _, _)", 0, 7, 0, RELIC_PROBE_SCAN },
		{ "not t(X, b, c)", RELIC_UNSTABLE_NO_INDEX, 0, 7,
		    RELIC_PROBE_MEMBER },
		{ "not t(_, X, _)", RELIC_UNSTABLE_NO_INDEX, 5, 0,
		    RELIC_PROBE_SCAN },
		{ "not z", 0, 0, 0, RELIC_PROBE_MEMBER },
	};
	enum relic_probe_shape shape;
	const relic_op_atom_t *probe;
	fixture_t fixture;
	char rule[128];
	uint16_t column;
	size_t i;
	size_t op;
	bool wildcard;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		snprintf(rule, sizeof(rule), "t(a, b, c).\ns(a).\nz.\n"
		    "p(X) :- s(X), %s.\n", cases[i].literal);

		if (!fixture_load(&fixture, rule, cases[i].flags, false) ||
		    !fixture_plan_rule(&fixture, 0, NULL)) {
			checkf(false, SCENARIO_PROBE, "%s should be planned",
			    cases[i].literal);
			fixture_release(&fixture);
			continue;
		}

		probe = NULL;
		for (op = 0; op < fixture.plan.op_count; op++) {
			if (fixture.plan.ops[op].kind == RELIC_OP_ANTI_MEMBER)
				probe = &fixture.plan.ops[op].u.atom;
		}
		if (probe == NULL) {
			checkf(false, SCENARIO_PROBE, "%s should be an "
			    "ANTI_MEMBER", cases[i].literal);
			fixture_release(&fixture);
			continue;
		}

		checkf(probe->key_mask == cases[i].key_mask, SCENARIO_PROBE,
		    "%s%s should be keyed on mask %u, not %u",
		    cases[i].literal,
		    cases[i].flags != 0 ? " without indexes" : "",
		    (unsigned int)cases[i].key_mask,
		    (unsigned int)probe->key_mask);
		shape = relic_plan_probe_shape(probe->key_mask, probe->arity);
		checkf(shape == cases[i].shape, SCENARIO_PROBE,
		    "%s%s should be probed in shape %u, not %u",
		    cases[i].literal,
		    cases[i].flags != 0 ? " without indexes" : "",
		    (unsigned int)cases[i].shape, (unsigned int)shape);

		if (cases[i].shape == RELIC_PROBE_INDEX)
			checkf(probe->index_position <
			    probe->relation->index_count &&
			    probe->relation->indexes[probe->index_position]
			    .key_mask == cases[i].key_mask, SCENARIO_PROBE,
			    "%s should probe the index on its key",
			    cases[i].literal);

		for (column = 0; column < probe->arity; column++) {
			wildcard = (cases[i].wildcards &
			    (UINT32_C(1) << column)) != 0;
			checkf((probe->terms[column].action ==
			    RELIC_PLAN_ANY) == wildcard, SCENARIO_PROBE,
			    "%s should read column %u %s", cases[i].literal,
			    (unsigned int)column,
			    wildcard ? "as ANY" : "by its value");
		}

		fixture_release(&fixture);
	}
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
		    !fixture_plan_rule(&fixture, 0, NULL) ||
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
	    fixture_plan_rule(&fixture, 0, NULL) &&
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
	    fixture_plan_rule(&fixture, 0, NULL)) {
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
	    fixture_plan_rule(&fixture, 0, NULL)) {
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
	    fixture_plan_rule(&fixture, 0, NULL)) {
		check(reads_in_order(&fixture, selective, 4), SCENARIO_ESTIMATE,
		    "the more selective lookup goes first");
	} else {
		check(false, SCENARIO_ESTIMATE, "the selective rule is planned");
	}
	fixture_release(&fixture);

	if (fixture_load(&fixture, "k(0).\nnone(1) :- k(2).\n"
	    "s(X) :- k(X), none(X).\n", 0, true) &&
	    fixture_plan_rule(&fixture, 1, NULL)) {
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

	check(fixture_plan_rule(&fixture,
	    fixture.ctx->program.rules.count - 2, incomplete) &&
	    reads_in_order(&fixture, small_first, 3), SCENARIO_INCOMPLETE,
	    "a complete relation smaller than the assumption goes first");
	check(fixture_plan_rule(&fixture,
	    fixture.ctx->program.rules.count - 1, incomplete) &&
	    reads_in_order(&fixture, derived_first, 3), SCENARIO_INCOMPLETE,
	    "a complete relation larger than the assumption goes after");
	check(fixture_plan_rule(&fixture,
	    fixture.ctx->program.rules.count - 2, NULL) &&
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

	check(fixture_plan_rule(&fixture,
	    fixture.ctx->program.rules.count - 1, incomplete) &&
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

	if (fixture_load(&fixture, program, RELIC_UNSTABLE_NO_INDEX, true) &&
	    fixture_plan_rule(&fixture, 0, NULL)) {
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

	if (fixture_load(&fixture, program, RELIC_UNSTABLE_NO_REORDER, true) &&
	    fixture_plan_rule(&fixture, 0, NULL)) {
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
 * with another or with a constant, a negated literal over a generated
 * relation, which is complete, or an equality of any variable with an
 * expression over those in `variables`.  The equality's variable is added
 * to `variables`: when no positive literal binds it, the equality is the
 * assignment that does, and when one does, whether the equality assigns it
 * or tests it depends on which the plan places first.
 */
static void
generate_test(text_t *text, uint32_t *state, uint32_t *variables)
{
	uint32_t relation;
	uint32_t target;
	uint16_t column;

	switch (next_random(state, 4)) {
	case 0:
		generate_variable(text, state, *variables);
		text_append(text, " < ");
		generate_variable(text, state, *variables);
		return;
	case 1:
		generate_variable(text, state, *variables);
		text_append(text, " != %u", (unsigned int)next_random(state,
		    RANKED_MAX_VALUES));
		return;
	case 2:
		target = next_random(state, RANKED_VARIABLES);
		text_append(text, "X%u = ", (unsigned int)target);
		generate_variable(text, state, *variables);
		if (next_random(state, 2) == 0)
			text_append(text, " + %u", (unsigned int)next_random(
			    state, RANKED_MAX_VALUES));
		*variables |= UINT32_C(1) << target;
		return;
	default:
		break;
	}

	relation = next_random(state, RANKED_RELATIONS);
	text_append(text, "not r%u(", (unsigned int)relation);
	for (column = 0; column < relation_arity(relation); column++) {
		text_append(text, "%s", column > 0 ? ", " : "");
		generate_variable(text, state, *variables);
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
		generate_test(&body, state, &variables);
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
 * The slots an expression tree reads, by a walk of its own.
 */
static uint64_t
reference_expr_slots(const relic_expr_t *expr)
{
	uint64_t slots;
	uint8_t i;

	switch (expr->kind) {
	case RELIC_EXPR_VARIABLE:
		return (UINT64_C(1) << expr->u.slot);
	case RELIC_EXPR_NEGATE:
		return (reference_expr_slots(expr->u.operand));
	case RELIC_EXPR_BINARY:
		return (reference_expr_slots(expr->u.binary.left) |
		    reference_expr_slots(expr->u.binary.right));
	case RELIC_EXPR_CALL:
		slots = 0;
		for (i = 0; i < expr->u.call.argc; i++)
			slots |= reference_expr_slots(expr->u.call.args[i]);
		return (slots);
	case RELIC_EXPR_CONSTANT:
		break;
	}

	return (0);
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
	if (!assumed && (relation = relic_relation_read(ctx, atom->pred)) ==
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
 * The slots an aggregate's key reads.
 */
static uint64_t
reference_key_slots(const relic_aggregate_t *aggregate)
{
	uint64_t slots;
	uint16_t i;

	slots = 0;
	for (i = 0; i < aggregate->shared_count; i++)
		slots |= UINT64_C(1) << aggregate->shared[i];

	return (slots);
}

/*
 * Whether an equality assigns a variable where `bound` is bound: one side a
 * variable not in `bound`, the left one first, and the other side reading
 * only slots in it; or an aggregate binds its result, its key in `bound`
 * and its result not.  The slot assigned is stored in *slotp.
 */
static bool
reference_assigns(const relic_literal_t *literal, uint64_t bound,
    uint16_t *slotp)
{
	const relic_comparison_t *comparison;
	const relic_aggregate_t *aggregate;
	const relic_expr_t *sides[2];
	size_t i;

	if (literal->kind == RELIC_LITERAL_AGGREGATE) {
		aggregate = &literal->u.aggregate;
		if ((reference_key_slots(aggregate) & ~bound) != 0 ||
		    (bound & (UINT64_C(1) << aggregate->result)) != 0)
			return (false);
		*slotp = aggregate->result;
		return (true);
	}

	if (literal->kind != RELIC_LITERAL_COMPARISON ||
	    literal->u.comparison.op != RELIC_CMP_EQ)
		return (false);

	comparison = &literal->u.comparison;
	sides[0] = comparison->left;
	sides[1] = comparison->right;
	for (i = 0; i < 2; i++) {
		if (sides[i]->kind == RELIC_EXPR_VARIABLE &&
		    (bound & (UINT64_C(1) << sides[i]->u.slot)) == 0 &&
		    (reference_expr_slots(sides[1 - i]) & ~bound) == 0) {
			*slotp = sides[i]->u.slot;
			return (true);
		}
	}

	return (false);
}

/*
 * Whether a test can be placed where `bound` is bound: it reads only slots
 * in `bound`, or it is an assignment.  A negated literal holding `_` would
 * count as ready here and never in the planner, which never places one; the
 * generated negations hold variables only.
 */
static bool
reference_test_ready(relic_t *ctx, const relic_literal_t *literal,
    uint64_t bound)
{
	const relic_pred_t *pred;
	uint64_t slots;
	uint16_t slot;

	switch (literal->kind) {
	case RELIC_LITERAL_COMPARISON:
		slots = reference_expr_slots(literal->u.comparison.left) |
		    reference_expr_slots(literal->u.comparison.right);
		return ((slots & ~bound) == 0 ||
		    reference_assigns(literal, bound, &slot));
	case RELIC_LITERAL_NEGATED:
		pred = relic_pred_of(ctx, literal->u.atom.pred);
		return (pred != NULL && (reference_slots(literal->u.atom.args,
		    pred->arity) & ~bound) == 0);
	case RELIC_LITERAL_AGGREGATE:
		return ((reference_key_slots(&literal->u.aggregate) &
		    ~bound) == 0);
	case RELIC_LITERAL_POSITIVE:
		break;
	}

	return (false);
}

/*
 * Append every test not yet placed that *boundp makes ready, in body order,
 * adding to *boundp what each assignment binds, and again as long as one
 * bound something; return the new length of the order.  A test an
 * assignment makes ready is placed after every test ready without it,
 * since it is placeable only from a later point: each round asks what the
 * slots bound when it began make ready.
 */
static size_t
reference_place_tests(relic_t *ctx, const relic_rule_t *rule,
    uint64_t *boundp, bool *placed, size_t *order, size_t count)
{
	uint64_t point;
	uint16_t slot;
	size_t i;

	do {
		point = *boundp;

		for (i = 0; i < rule->body_length; i++) {
			if (placed[i] || !reference_test_ready(ctx,
			    &rule->body[i], point))
				continue;

			placed[i] = true;
			order[count] = i;
			count++;
			if (reference_assigns(&rule->body[i], *boundp, &slot))
				*boundp |= UINT64_C(1) << slot;
		}
	} while (*boundp != point);

	return (count);
}

/*
 * The greedy order computed one step at a time: at every step each positive
 * literal not yet placed is scored against the slots bound so far, in body
 * order, and the first of the best is placed, followed by every test it
 * made ready.  Under RELIC_UNSTABLE_NO_REORDER the first positive literal not
 * yet placed is chosen instead.  This is the definition the planner's passes
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

	reorder = (ctx->config.flags & RELIC_UNSTABLE_NO_REORDER) == 0;
	bzero(placed, sizeof(placed));
	count = reference_place_tests(ctx, rule, &bound, placed, order, 0);

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
		count = reference_place_tests(ctx, rule, &bound, placed,
		    order, count);
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
		check_ranked_program(&state, RELIC_UNSTABLE_NO_REORDER);
	}
}

/*
 * A plan is known to derive each head tuple once when no generator matches
 * an argument with `_` and its EMIT reads every slot a generator binds.  A
 * rule that drops a variable from its head, or matches with `_`, can derive
 * a tuple twice, whichever operation reads the variable; one that keeps every
 * variable -- repeated in an atom, tested by a membership, a negation or a
 * comparison, or with nothing to bind -- cannot, and neither can one whose
 * `_` or dead variable is in a literal placed after f(X) binds X, which is
 * a check.  The answer is read off the
 * plan, so each rule is planned under every combination of the index and
 * reordering switches, which change the operations it is planned into.
 */
static void
a_plan_keeping_every_binding_derives_distinct_rows(void)
{
	static const struct {
		const char	*rule;
		bool		 distinct;
	} rules[] = {
		{ "p(X) :- e(X, Y).\n", false },
		{ "p(X) :- e(X, _).\n", false },
		{ "p(X) :- e(X, Y), e(Y, Z).\n", false },
		{ "p(X, Z) :- e(X, Y), e(Y, Z).\n", false },
		{ "p(X) :- e(X, Y), not f(Y).\n", false },
		{ "p(X, Y) :- e(X, Y).\n", true },
		{ "p(X) :- e(X, X).\n", true },
		{ "p(X, Y) :- e(X, Y), e(Y, X).\n", true },
		{ "p(X, Y) :- e(X, Y), not f(Y), X != Y.\n", true },
		{ "p :- f(1).\n", true },
		{ "p(X) :- f(X), e(_, X).\n", true },
		{ "p(X) :- f(X), e(X, Y).\n", true },
	};
	static const unsigned int flag_sets[] = {
		0, RELIC_UNSTABLE_NO_INDEX, RELIC_UNSTABLE_NO_REORDER,
		TEXTUAL_FLAGS,
	};

	fixture_t fixture;
	char program[256];
	size_t r;
	size_t f;

	for (r = 0; r < sizeof(rules) / sizeof(rules[0]); r++) {
		for (f = 0; f < sizeof(flag_sets) / sizeof(flag_sets[0]); f++) {
			bool planned;

			(void)snprintf(program, sizeof(program),
			    "e(1, 2).\ne(2, 1).\ne(1, 1).\nf(1).\n%s",
			    rules[r].rule);

			planned = fixture_load(&fixture, program,
			    flag_sets[f], false);
			if (planned)
				planned = fixture_plan_rule(&fixture, 0, NULL);
			checkf(planned, SCENARIO_DISTINCT,
			    "%s should plan under flags %#x", rules[r].rule,
			    flag_sets[f]);
			if (planned)
				checkf(relic_plan_derives_distinct(
				    &fixture.plan) == rules[r].distinct,
				    SCENARIO_DISTINCT,
				    "the plan of %s under flags %#x should %s",
				    rules[r].rule, flag_sets[f],
				    rules[r].distinct ?
				    "derive each row once" :
				    "be able to derive a row twice");

			fixture_release(&fixture);
		}
	}
}

/*
 * Load `program` under `flags` and plan its last rule, which holds an
 * aggregate, after the group rule the aggregate reads when it has one; the
 * fold is the operation at `op`.  Returns NULL, having failed the check,
 * when the rule is not planned or the operation there is not a fold.
 */
static const relic_op_aggregate_t *
fold_planned(fixture_t *fixture, const char *program, unsigned int flags,
    size_t op)
{
	size_t outer;

	if (!fixture_load(fixture, program, flags, false)) {
		checkf(false, SCENARIO_AGGREGATE, "the program is accepted: %s",
		    program);
		return (NULL);
	}

	outer = fixture->ctx->program.rules.count - 1;
	if (!fixture_plan_rule(fixture, outer, NULL) ||
	    op >= fixture->plan.op_count ||
	    fixture->plan.ops[op].kind != RELIC_OP_AGGREGATE) {
		checkf(false, SCENARIO_AGGREGATE, "operation %zu of the rule "
		    "is its fold: %s", op, program);
		return (NULL);
	}

	return (&fixture->plan.ops[op].u.aggregate);
}

/*
 * An aggregate is a test: it is placed where every shared variable is
 * bound, which may be after a literal written later, and it binds its
 * result unless an earlier operation bound it, when it compares.  A count
 * and a sum bind a number slot; a min and a max a domain slot.
 */
static void
a_fold_goes_where_its_key_is_bound(void)
{
	static const enum relic_op_kind later[] = {
		RELIC_OP_SCAN, RELIC_OP_AGGREGATE, RELIC_OP_EMIT
	};
	const relic_op_aggregate_t *fold;
	fixture_t fixture;

	fold = fold_planned(&fixture, "e(a, b).\ns(a).\n"
	    "r(X, N) :- N = count { e(X, Y) }, s(X).\n", TEXTUAL_FLAGS, 1);
	if (fold != NULL) {
		check(kinds_are(&fixture.plan, later, 3), SCENARIO_AGGREGATE,
		    "a fold written first follows the scan binding its key");
		check(fold->binds && fold->kind == RELIC_SLOT_NUMBER && fold->fn ==
		    RELIC_AGG_COUNT && fold->slot ==
		    slot_named(&fixture, 0, "N"), SCENARIO_AGGREGATE,
		    "a count binds its result as a number");
		check(fold->key_count == 1 && fold->key_mask == 1 &&
		    fold->key_slots[0] == slot_named(&fixture, 0, "X") &&
		    fold->arity == 2, SCENARIO_AGGREGATE,
		    "the key is the shared X, the atom's first column");
	}
	fixture_release(&fixture);

	fold = fold_planned(&fixture, "e(a, b).\ns(a, 1).\n"
	    "r(X) :- s(X, N), N = count { e(X, Y) }.\n", TEXTUAL_FLAGS, 1);
	if (fold != NULL)
		check(!fold->binds && fold->slot ==
		    slot_named(&fixture, 0, "N"), SCENARIO_AGGREGATE,
		    "a fold whose result is bound compares");
	fixture_release(&fixture);

	fold = fold_planned(&fixture, "e(a, 1).\n"
	    "r(M) :- M = min(V) { e(K, V) }.\n", TEXTUAL_FLAGS, 0);
	if (fold != NULL)
		check(fold->binds && fold->kind == RELIC_SLOT_DOMAIN && fold->fn ==
		    RELIC_AGG_MIN && fold->column == 1,
		    SCENARIO_AGGREGATE, "a min binds a domain slot from the "
		    "column of its variable");
	fixture_release(&fixture);

	fold = fold_planned(&fixture, "e(a, 1).\n"
	    "r(S) :- S = sum(V) { e(K, V) }.\n", TEXTUAL_FLAGS, 0);
	if (fold != NULL)
		check(fold->binds && fold->kind == RELIC_SLOT_NUMBER && fold->fn ==
		    RELIC_AGG_SUM, SCENARIO_AGGREGATE,
		    "a sum binds a number slot");
	fixture_release(&fixture);
}

/*
 * The access path of a fold: cardinality for an unkeyed count; the index
 * keyed on the key columns when there are local columns too; a probe when
 * every column is a key column; and a walk whatever the key under
 * RELIC_UNSTABLE_NO_INDEX, as a generator is scanned under it.
 */
static void
a_fold_reads_its_group_by_its_key(void)
{
	static const char *const keyed = "e(a, b).\ns(a).\n"
	    "r(X, N) :- s(X), N = count { e(X, Y) }.\n";
	const relic_op_aggregate_t *fold;
	fixture_t fixture;

	fold = fold_planned(&fixture, "e(a, b).\n"
	    "r(N) :- N = count { e(X, Y) }.\n", 0, 0);
	if (fold != NULL)
		check(fold->access == RELIC_FOLD_CARDINALITY && fold->key_count == 0,
		    SCENARIO_FOLD_ACCESS, "an unkeyed count reads the tuple "
		    "count");
	fixture_release(&fixture);

	fold = fold_planned(&fixture, keyed, 0, 1);
	if (fold != NULL)
		check(fold->access == RELIC_FOLD_LOOKUP &&
		    fold->index_position < fold->relation->index_count &&
		    fold->relation->indexes[fold->index_position].key_mask ==
		    1, SCENARIO_FOLD_ACCESS, "a keyed fold reads the index "
		    "keyed on its key");
	fixture_release(&fixture);

	fold = fold_planned(&fixture, "e(a, b).\ns(a, b).\n"
	    "r(X, Y, N) :- s(X, Y), N = count { e(X, Y) }.\n", 0, 1);
	if (fold != NULL)
		check(fold->access == RELIC_FOLD_PROBE &&
		    fold->key_count == 2 && fold->arity == 2,
		    SCENARIO_FOLD_ACCESS,
		    "a fold keyed on every column probes");
	fixture_release(&fixture);

	fold = fold_planned(&fixture, keyed, RELIC_UNSTABLE_NO_INDEX, 1);
	if (fold != NULL)
		check(fold->access == RELIC_FOLD_SCAN && fold->key_count == 1,
		    SCENARIO_FOLD_ACCESS, "a keyed fold walks the relation "
		    "without indexes");
	fixture_release(&fixture);
}

/*
 * A fold in place is keyed on whichever columns its shared variables stand
 * in, which need not be a prefix, and reaches its group as a positive
 * literal bound in those columns would: through the index keyed on exactly
 * them, which a second fold on the same columns finds built; by a walk
 * matching them under RELIC_UNSTABLE_NO_INDEX; and by a probe when every column
 * is a key column.  The relation it walks is the atom's own.
 */
static void
a_fold_in_place_reads_its_key_columns(void)
{
	static const char *const program = "e(a, b).\ns(b).\nt(b, a).\n"
	    "q(X, M) :- s(X), M = count { e(Z, X) }.\n"
	    "r(X, N) :- s(X), N = count { e(Y, X) }.\n";
	const relic_op_aggregate_t *fold;
	const relic_relation_t *edges;
	fixture_t fixture;
	uint32_t first_position;
	size_t first_count;

	fold = fold_planned(&fixture, program, 0, 1);
	if (fold != NULL) {
		edges = fold->relation;
		first_position = fold->index_position;
		first_count = edges->index_count;
		check(fold->access == RELIC_FOLD_LOOKUP && fold->key_mask ==
		    0x2 && fold->key_count == 1 && fold->key_slots[0] ==
		    slot_named(&fixture, 1, "X") && fold->arity == 2 &&
		    fold->index_position < edges->index_count &&
		    edges->indexes[fold->index_position].key_mask == 0x2,
		    SCENARIO_FOLD_ACCESS, "a fold keyed on the atom's second "
		    "column reads the index keyed on that column alone");
		check(fixture_plan_rule(&fixture, 0, NULL) &&
		    fixture.plan.ops[1].kind == RELIC_OP_AGGREGATE &&
		    fixture.plan.ops[1].u.aggregate.relation == edges &&
		    fixture.plan.ops[1].u.aggregate.index_position ==
		    first_position && edges->index_count == first_count,
		    SCENARIO_FOLD_ACCESS, "a second fold on the same columns "
		    "finds the index built");
	}
	fixture_release(&fixture);

	fold = fold_planned(&fixture, program, RELIC_UNSTABLE_NO_INDEX, 1);
	if (fold != NULL)
		check(fold->access == RELIC_FOLD_SCAN && fold->key_mask == 0x2,
		    SCENARIO_FOLD_ACCESS, "without indexes a fold in place "
		    "walks the atom's relation matching its second column");
	fixture_release(&fixture);

	fold = fold_planned(&fixture, "e(a, b).\nt(b, a).\n"
	    "r(X, Y, N) :- t(X, Y), N = count { e(Y, X) }.\n", 0, 1);
	if (fold != NULL)
		check(fold->access == RELIC_FOLD_PROBE &&
		    fold->key_mask == 0x3 &&
		    fold->key_slots[0] == slot_named(&fixture, 0, "Y") &&
		    fold->key_slots[1] == slot_named(&fixture, 0, "X"),
		    SCENARIO_FOLD_ACCESS, "a fold in place keyed on every column "
		    "probes, its key slots in column order");
	fixture_release(&fixture);
}

/*
 * The stack an assignment's program needs follows the recurrence over its
 * tree: a leaf one cell, an operator the greater of its left operand's need
 * and one more than its right's, and a call the greatest of each argument's
 * need plus the cells of the arguments before it.  A call nested in its own
 * third argument at the parser's height bound needs the most any tree the
 * parser admits can, 63 cells, and operators nested to the right around
 * a call need one cell per operator and one for the call.
 */
static void
a_program_holds_the_stack_its_shape_needs(void)
{
	static const struct {
		const char	*expression;
		unsigned int	 depth;
	} cases[] = {
		{ "S", 1 },
		{ "length(S)", 1 },
		{ "cat(S, S)", 2 },
		{ "substr(S, 1, 2)", 3 },
		{ "cat(cat(S, S), S)", 2 },
		{ "cat(S, cat(S, S))", 3 },
		{ "1 + length(S)", 2 },
		{ "length(S) + 1", 2 },
		{ "substr(S, 0, index(S, cat(S, S)))", 5 },
		{ "str(substr(cat(S, S), 1))", 2 }
	};
	char expression[512];
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		expect_depth(cases[i].expression, cases[i].depth);

	nested_expression(expression, sizeof(expression), "substr(S, 0, ",
	    "S", RELIC_MAX_EXPRESSION_DEPTH - 1);
	expect_depth(expression, RELIC_POSTFIX_DEPTH_MAX);

	nested_expression(expression, sizeof(expression), "1 + (",
	    "length(S)", RELIC_MAX_EXPRESSION_DEPTH - 2);
	expect_depth(expression, RELIC_MAX_EXPRESSION_DEPTH - 1);
}

/*
 * Write `levels` copies of `open`, then `inner`, then as many `)`.
 */
static void
nested_expression(char *text, size_t size, const char *open,
    const char *inner, unsigned int levels)
{
	size_t used;
	unsigned int i;

	used = 0;
	for (i = 0; i < levels; i++)
		used += (size_t)snprintf(text + used, size - used, "%s", open);
	used += (size_t)snprintf(text + used, size - used, "%s", inner);
	for (i = 0; i < levels; i++)
		used += (size_t)snprintf(text + used, size - used, ")");
}

/*
 * Plan `Y = expression` over a scanned S, and check that its program needs
 * `depth` cells.
 */
static void
expect_depth(const char *expression, unsigned int depth)
{
	char program[640];
	fixture_t fixture;
	unsigned int held;

	snprintf(program, sizeof(program),
	    "s(\"ab\").\nr(Y) :- s(S), Y = %s.\n", expression);
	if (!fixture_plan(&fixture, program, 0) ||
	    fixture.plan.op_count != 3 ||
	    fixture.plan.ops[1].kind != RELIC_OP_ASSIGN) {
		checkf(false, SCENARIO_STACK, "Y = %.40s... should plan to a "
		    "scan, an assignment and an EMIT", expression);
		fixture_release(&fixture);
		return;
	}

	held = fixture.plan.ops[1].u.assign.value.depth;
	checkf(held == depth, SCENARIO_STACK, "Y = %.40s... should need %u "
	    "cells, not %u", expression, depth, held);
	fixture_release(&fixture);
}

/*
 * A text slot is bound by an assignment of a string-valued call, and each
 * operation after it builds its strings above the last text assignment
 * placed before it: a filter or an assignment records that assignment's
 * position plus one as its base, or zero when there is none.  The EMIT
 * writes a text slot's string through its own action.
 */
static void
each_operation_builds_its_strings_above_a_base(void)
{
	static const enum relic_op_kind kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_FILTER, RELIC_OP_ASSIGN, RELIC_OP_FILTER,
		RELIC_OP_ASSIGN, RELIC_OP_ASSIGN, RELIC_OP_FILTER, RELIC_OP_EMIT
	};
	static const char *const program = "s(\"ab\").\n"
	    "r(T, N) :- s(S), length(S) > 0, T = cat(S, S), length(T) > 1, "
	    "N = length(T), U = cat(T, S), index(U, T) = 0.\n";
	const relic_plan_t *plan;
	fixture_t fixture;

	if (!fixture_plan(&fixture, program, 0)) {
		check(false, SCENARIO_TEXTS, "the rule is planned");
		fixture_release(&fixture);
		return;
	}

	plan = &fixture.plan;
	if (!kinds_are(plan, kinds, sizeof(kinds) / sizeof(kinds[0]))) {
		check(false, SCENARIO_TEXTS, "the operations are placed in "
		    "the order written");
		fixture_release(&fixture);
		return;
	}

	check(plan->ops[1].u.filter.text_base == 0, SCENARIO_TEXTS,
	    "a filter before any text assignment builds from the bottom");
	check(plan->ops[2].u.assign.kind == RELIC_SLOT_TEXT &&
	    plan->ops[2].u.assign.text_base == 0, SCENARIO_TEXTS,
	    "the first text assignment binds a text slot from the bottom");
	check(plan->ops[3].u.filter.text_base == 3, SCENARIO_TEXTS,
	    "a filter after it builds above its string");
	check(plan->ops[4].u.assign.kind == RELIC_SLOT_NUMBER &&
	    plan->ops[4].u.assign.text_base == 3, SCENARIO_TEXTS,
	    "a number assignment after it builds above its string");
	check(plan->ops[5].u.assign.kind == RELIC_SLOT_TEXT &&
	    plan->ops[5].u.assign.text_base == 3, SCENARIO_TEXTS,
	    "a second text assignment builds above the first's string");
	check(plan->ops[6].u.filter.text_base == 6, SCENARIO_TEXTS,
	    "a filter after both builds above the second's string");
	check(plan->ops[7].u.emit.terms[0].action == RELIC_PLAN_TEXT &&
	    plan->ops[7].u.emit.terms[1].action == RELIC_PLAN_NUMBER,
	    SCENARIO_TEXTS, "the EMIT writes the text slot as a string");

	fixture_release(&fixture);
}

/*
 * The relation operation `op` of a draft reads.
 */
static const relic_relation_t *
draft_relation(const relic_plan_draft_t *draft, size_t op)
{
	if (draft->ops[op].kind == RELIC_OP_AGGREGATE)
		return (draft->ops[op].u.aggregate.relation);
	if (draft->ops[op].kind == RELIC_OP_EMIT)
		return (NULL);

	return (draft->ops[op].u.atom.relation);
}

/*
 * Whether a draft names exactly the indexes operations `ops` read, in that
 * order, each keyed on the first column.
 */
static bool
accesses_are(const relic_plan_draft_t *draft, const size_t *ops,
    size_t count)
{
	size_t i;

	if (draft->access_count != count)
		return (false);

	for (i = 0; i < count; i++) {
		if (draft->accesses[i].relation != draft_relation(draft,
		    ops[i]) || draft->accesses[i].key_mask != UINT32_C(1))
			return (false);
	}

	return (true);
}

/*
 * Whether every operation of a plan that reads a secondary index names, by
 * position, an index of its relation keyed on its mask: a LOOKUP, an EXISTS
 * or ANTI_MEMBER of the index shape, and a fold through a lookup.
 */
static bool
positions_bound(const relic_plan_t *plan)
{
	const relic_relation_t *relation;
	const relic_index_t *index;
	const relic_op_t *op;
	uint32_t key_mask;
	uint32_t position;
	size_t i;

	for (i = 0; i < plan->op_count; i++) {
		op = &plan->ops[i];
		switch (op->kind) {
		case RELIC_OP_EXISTS:
		case RELIC_OP_ANTI_MEMBER:
			if (relic_plan_probe_shape(op->u.atom.key_mask,
			    op->u.atom.arity) != RELIC_PROBE_INDEX)
				continue;
			/* FALLTHROUGH */
		case RELIC_OP_LOOKUP:
			relation = op->u.atom.relation;
			key_mask = op->u.atom.key_mask;
			position = op->u.atom.index_position;
			break;
		case RELIC_OP_AGGREGATE:
			if (op->u.aggregate.access != RELIC_FOLD_LOOKUP)
				continue;
			relation = op->u.aggregate.relation;
			key_mask = op->u.aggregate.key_mask;
			position = op->u.aggregate.index_position;
			break;
		default:
			continue;
		}
		if (position >= relation->index_count)
			return (false);
		index = relic_relation_index_at(relation, position);
		if (index->key_mask != key_mask)
			return (false);
	}

	return (true);
}

/*
 * A variant's candidates are drafts: planning one builds no index, and its
 * draft names every index its operations read, once, where they first read
 * it, whatever reads it -- a lookup, a probe or a looked-up fold.  In the
 * written order of
 *
 *	r(X, Z, U, N) :- a(X), e(X, Y), f(Y, Z), e(X, W), f(W, U), g(W),
 *	    m(X, _), not h(X, _), not g(Y), N = count { k(X, V) }.
 *
 * the delta of a binds X, so the negated h and the fold go next, each
 * keyed on X, then e is looked up on X, which binds Y for the negated g, a
 * test of the whole tuple that reads no index, then f on Y, e on X again
 * and f on W, which name no second index, then g is a membership test and
 * m a check keyed on X.  Preparing the draft builds each index it names
 * and gives every operation the position of its own; preparing it again
 * builds nothing more.
 */
static void
a_draft_names_its_indexes_and_builds_none(void)
{
	static const enum relic_op_kind kinds[] = {
		RELIC_OP_SCAN, RELIC_OP_ANTI_MEMBER, RELIC_OP_AGGREGATE,
		RELIC_OP_LOOKUP, RELIC_OP_ANTI_MEMBER, RELIC_OP_LOOKUP,
		RELIC_OP_LOOKUP, RELIC_OP_LOOKUP, RELIC_OP_MEMBER,
		RELIC_OP_EXISTS, RELIC_OP_EMIT
	};
	static const size_t first_readers[] = { 1, 2, 3, 5, 9 };
	static const char *const program = "a(1).\ne(1, 2).\nf(2, 3).\n"
	    "g(2).\nh(1, 1).\nk(1, 4).\nm(1, 5).\n"
	    "r(X, Z, U, N) :- a(X), e(X, Y), f(Y, Z), e(X, W), f(W, U), "
	    "g(W), m(X, _), not h(X, _), not g(Y), "
	    "N = count { k(X, V) }.\n";
	const relic_plan_draft_t *draft;
	const relic_relation_t *relation;
	relic_plan_family_t family;
	const relic_rule_t *rule;
	relic_relation_t *e;
	fixture_t fixture;
	bool incomplete[16];
	uint32_t position;
	size_t indexes;
	size_t i;

	bzero(incomplete, sizeof(incomplete));
	if (!fixture_load(&fixture, program, RELIC_UNSTABLE_NO_REORDER,
	    false) ||
	    relic_pred_count(fixture.ctx) > 16) {
		check(false, SCENARIO_DRAFT, "the program is loaded");
		fixture_release(&fixture);
		return;
	}
	rule = &fixture.ctx->program.rules.entries[0];
	incomplete[rule->body[0].u.atom.pred - 1] = true;

	if (!fixture_plan_variants(&fixture, 0, 0, incomplete, &family) ||
	    family.count != 1 ||
	    !draft_kinds_are(&family.candidates[0].draft, kinds, 11)) {
		check(false, SCENARIO_DRAFT, "the variant is one draft of a "
		    "scan, a negated probe, a fold, a lookup, a negated "
		    "member, three lookups, a member, a check and an EMIT");
		fixture_release(&fixture);
		return;
	}
	draft = &family.candidates[0].draft;

	indexes = 0;
	for (i = 0; i < draft->op_count; i++) {
		relation = draft_relation(draft, i);
		if (relation != NULL)
			indexes += relation->index_count;
	}
	check(indexes == 0, SCENARIO_DRAFT, "planning the variant builds no "
	    "index");
	check(accesses_are(draft, first_readers, 5), SCENARIO_DRAFT,
	    "the draft names h, k, e, f and m on their first column, e and "
	    "f once each");

	/*
	 * An index of e on its second column, which the draft does not read,
	 * puts the one it does at position 1, so a position left unbound at
	 * 0 names the wrong index.
	 */
	e = draft->accesses[2].relation;
	if (relic_relation_index(fixture.ctx, e, UINT32_C(2), &position) !=
	    0 || position != 0) {
		check(false, SCENARIO_DRAFT,
		    "e is indexed on its second column");
		fixture_release(&fixture);
		return;
	}

	if (!fixture_prepare(&fixture, draft)) {
		check(false, SCENARIO_DRAFT, "the draft is prepared");
		fixture_release(&fixture);
		return;
	}
	check(positions_bound(&fixture.plan), SCENARIO_DRAFT, "preparing the "
	    "draft gives each operation the position of its index");
	check(fixture.plan.ops[3].u.atom.index_position == 1, SCENARIO_DRAFT,
	    "the lookup of e names e's first-column index, at position 1");
	for (i = 0; i < draft->access_count; i++) {
		relation = draft->accesses[i].relation;
		checkf(relation->index_count == (relation == e ? 2u : 1u),
		    SCENARIO_DRAFT, "access %zu's relation has %u index(es), "
		    "not %zu", i, relation == e ? 2u : 1u,
		    relation->index_count);
	}

	check(fixture_prepare(&fixture, draft) &&
	    positions_bound(&fixture.plan), SCENARIO_DRAFT, "preparing the "
	    "draft again binds the same indexes");
	indexes = 0;
	for (i = 0; i < draft->access_count; i++)
		indexes += draft->accesses[i].relation->index_count;
	check(indexes == 6, SCENARIO_DRAFT, "preparing again builds no index");

	fixture_release(&fixture);
}

int
main(void)
{
	a_scans_columns_bind_match_or_ignore();
	a_fully_bound_literal_is_a_membership_test();
	a_literal_binding_nothing_read_is_a_check();
	a_variable_mentioned_twice_is_not_dead();
	a_literal_is_a_check_where_it_is_placed_so();
	a_delta_literal_may_be_a_check_in_one_plan();
	without_checks_every_literal_is_planned_as_before();
	a_comparison_goes_where_its_operands_are_bound();
	an_assignment_binds_a_slot_of_its_kind();
	an_equality_of_two_literals_variables_assigns();
	a_textual_variant_keeps_its_written_order();
	a_variant_keeps_both_orders();
	a_guard_of_any_size_keeps_the_input_first_plan();
	estimates_never_discard_a_candidate();
	a_delta_literal_must_be_positive();
	the_emit_carries_the_head_schema();
	a_bodyless_rule_is_a_lone_emit_of_constants();
	a_negated_literal_is_placed_once_bound();
	a_negated_literal_probes_by_its_named_columns();
	each_binding_pattern_gets_its_access_path();
	bound_arguments_then_small_relations_go_first();
	a_bound_argument_outranks_a_smaller_estimate();
	the_smaller_estimate_goes_first();
	a_relation_being_derived_is_assumed();
	an_assumed_relation_divides_per_bound_column();
	the_references_change_only_their_choice();
	the_ranking_is_the_step_by_step_order();
	a_plan_keeping_every_binding_derives_distinct_rows();
	a_fold_goes_where_its_key_is_bound();
	a_fold_reads_its_group_by_its_key();
	a_fold_in_place_reads_its_key_columns();
	a_program_holds_the_stack_its_shape_needs();
	each_operation_builds_its_strings_above_a_base();
	a_draft_names_its_indexes_and_builds_none();

	return (check_summary("test_plan"));
}
