/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for rule compilation and relic_check().
 *
 * The golden language tests under tests/lang/ compare the diagnostics a
 * program produces, byte for byte, and are the record of what the checker
 * says.  What they cannot see is the compiled rule, so that is what this
 * suite examines: the slots a rule's variables were numbered into, the
 * arguments each literal was resolved to, and the three properties a caller
 * depends on and no message reveals -- that a check compiles only what is
 * new, that it stays refused once a program has held a bad rule, and that a
 * rejected statement leaves no rule behind.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_ARGUMENTS	"a literal resolves to the arguments it names"
#define	SCENARIO_GROUP		"an aggregate reads a group rule of its own"
#define	SCENARIO_GROUP_NAME	"a group predicate is named for where it is"
#define	SCENARIO_IN_PLACE	"a body that is a relation is folded in place"
#define	SCENARIO_KEY_COLUMNS	"a fold in place is keyed where its key stands"
#define	SCENARIO_CACHED		"a check compiles only what is new"
#define	SCENARIO_CALL		"a call compiles to a node over its arguments"
#define	SCENARIO_PATTERN	"a constant pattern is compiled with its rule"
#define	SCENARIO_CLOSURE	"an assignment binds what the closure reaches"
#define	SCENARIO_EXPRESSION	"a comparison compiles to two trees over slots"
#define	SCENARIO_PRINTED	"a rule prints as text that reads back as it"
#define	SCENARIO_REJECTED	"a rejected statement leaves no rule behind"
#define	SCENARIO_SLOTS		"variables are numbered as they are written"
#define	SCENARIO_STICKY		"a program that held a bad rule stays refused"
#define	SCENARIO_WILDCARD	"`_` takes a slot only where it is illegal"

/* The longest line an explain callback is given, and its NUL. */
#define	PRINTED_LINE_MAX	1024

/* The text of a check that fails with nothing new to report. */
#define	UNUSABLE_ERROR \
	"the program cannot be used: its errors were reported when it was " \
	"checked"

typedef struct diag_log diag_log_t;

/*
 * Everything the context reported during a check, so that a test can count
 * the messages without a callback that writes to stderr.  A test that wants
 * the exact text is a golden language test rather than this.
 */
struct diag_log {
	size_t	errors;
	size_t	warnings;
	size_t	notes;
};

static const relic_rule_t *rule_at(const relic_t *ctx, size_t index);
static relic_t	*context_logging_to(diag_log_t *log);
static bool	 load(relic_t *ctx, const char *text);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static size_t	 rule_count(const relic_t *ctx);
static bool	 spelled(const relic_spelling_t *spelling, const char *name);
static void	 a_check_compiles_only_what_is_new(void);
static void	 a_literal_resolves_to_the_arguments_it_names(void);
static void	 a_program_that_held_a_bad_rule_stays_refused(void);
static void	 a_rejected_statement_leaves_no_rule_behind(void);
static void	 an_assignment_binds_what_the_closure_reaches(void);
static void	 a_comparison_compiles_to_two_trees_over_slots(void);
static void	 a_call_compiles_to_a_node_over_its_arguments(void);
static void	 a_constant_pattern_is_compiled_with_its_rule(void);
static const relic_expr_call_t *compared_call(const relic_rule_t *rule);
static void	 checked_alone(const char *program, diag_log_t *log);
static bool	 printed_rule(const char *program, char *out, size_t size);
static void	 keep_printed_line(void *callback_ctx, const char *line);
static void	 a_rule_prints_as_text_that_reads_back_as_it(void);
static void	 a_rule_at_the_nesting_bound_prints_within_it(void);
static void	 an_anonymous_variable_takes_a_slot_only_where_illegal(void);
static void	 variables_are_numbered_as_they_are_written(void);
static void	 an_aggregate_reads_a_group_rule_of_its_own(void);
static void	 a_group_predicate_is_named_for_where_it_is(void);
static void	 a_body_that_is_a_relation_is_folded_in_place(void);
static void	 a_fold_in_place_is_keyed_where_its_key_stands(void);
static const relic_aggregate_t *last_aggregate(const relic_t *ctx);
static size_t	 group_predicates(const relic_t *ctx);
static const char *pred_spelling(const relic_t *ctx, relic_pred_id_t pred);

static void
record_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_UNUSED,
    const char *message RELIC_UNUSED)
{
	diag_log_t *log;

	log = callback_ctx;

	switch (level) {
	case RELIC_ERROR:
		log->errors++;
		break;
	case RELIC_WARNING:
		log->warnings++;
		break;
	case RELIC_NOTE:
		log->notes++;
		break;
	}
}

static relic_t *
context_logging_to(diag_log_t *log)
{
	relic_t *ctx;

	bzero(log, sizeof(*log));

	ctx = relic_create();
	if (ctx == NULL) {
		check(false, "creating a context", "a context is created");
		return (NULL);
	}

	relic_diag_set(ctx, record_diagnostic, log);

	return (ctx);
}

static bool
load(relic_t *ctx, const char *text)
{
	return (relic_load_string(ctx, "test.dl", text) == 0);
}

static size_t
rule_count(const relic_t *ctx)
{
	return (ctx->program.rules.count);
}

/*
 * The compiled rule at `index` in load order, or NULL when there are fewer
 * than that many.
 */
static const relic_rule_t *
rule_at(const relic_t *ctx, size_t index)
{
	if (index >= ctx->program.rules.count)
		return (NULL);

	return (&ctx->program.rules.entries[index]);
}

/*
 * Whether a spelling the compiler kept is `name`.
 */
static bool
spelled(const relic_spelling_t *spelling, const char *name)
{
	return (spelling->length == strlen(name) &&
	    bcmp(spelling->bytes, name, spelling->length) == 0);
}

/*
 * Slots are assigned in order of first occurrence, reading the head and then
 * the body.  The rule below is the recursive clause of the transitive
 * closure, whose slots are therefore X, Z and then Y -- the statement as
 * written, rather than the order the body happens to mention them in.
 */
static void
variables_are_numbered_as_they_are_written(void)
{
	relic_t *ctx;
	const relic_rule_t *rule;
	diag_log_t log;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load(ctx, "edge(a, b).\npath(X, Z) :- edge(X, Y), path(Y, Z).\n")
	    || relic_check(ctx) != 0) {
		check(false, SCENARIO_SLOTS, "the program is accepted");
		relic_destroy(ctx);
		return;
	}

	rule = rule_at(ctx, 0);
	if (rule == NULL) {
		check(false, SCENARIO_SLOTS, "the recursive rule was compiled");
		relic_destroy(ctx);
		return;
	}

	checkf(rule->variable_count == 3, SCENARIO_SLOTS,
	    "three variables were numbered into %u slots",
	    (unsigned int)rule->variable_count);

	if (rule->variable_count != 3) {
		relic_destroy(ctx);
		return;
	}

	checkf(spelled(&rule->variables[0].name, "X") &&
	    spelled(&rule->variables[1].name, "Z") &&
	    spelled(&rule->variables[2].name, "Y"), SCENARIO_SLOTS,
	    "the slots hold X, Z, Y in first-occurrence order");

	/*
	 * X occurs in the head and in edge/2; Y in edge/2 and path/2; Z in
	 * the head and in path/2.
	 */
	checkf(rule->variables[0].occurrences == 2 &&
	    rule->variables[1].occurrences == 2 &&
	    rule->variables[2].occurrences == 2, SCENARIO_SLOTS,
	    "each variable was counted twice, not %u, %u and %u",
	    rule->variables[0].occurrences, rule->variables[1].occurrences,
	    rule->variables[2].occurrences);

	relic_destroy(ctx);
}

/*
 * A compiled literal names a predicate identifier and arguments that are
 * either an interned constant or a slot.  Nothing in it is a name, which is
 * the whole reason the compiled form exists.
 */
static void
a_literal_resolves_to_the_arguments_it_names(void)
{
	relic_t *ctx;
	const relic_rule_t *rule;
	const relic_literal_t *literal;
	const relic_pred_t *pred;
	relic_value_t decoded;
	relic_value_t expected;
	relic_dom_t domain;
	diag_log_t log;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load(ctx, "size(a, 1).\nlarge(X) :- size(X, 100), not small(X).\n")
	    || relic_check(ctx) != 0) {
		check(false, SCENARIO_ARGUMENTS, "the program is accepted");
		relic_destroy(ctx);
		return;
	}

	rule = rule_at(ctx, 0);
	if (rule == NULL || rule->body_length != 2) {
		check(false, SCENARIO_ARGUMENTS, "the rule has two literals");
		relic_destroy(ctx);
		return;
	}

	literal = &rule->body[0];
	check(literal->kind == RELIC_LITERAL_POSITIVE, SCENARIO_ARGUMENTS,
	    "size/2 is a positive literal");

	pred = relic_pred_of(ctx, literal->u.atom.pred);
	checkf(pred != NULL && pred->arity == 2 &&
	    spelled(&pred->name, "size"), SCENARIO_ARGUMENTS,
	    "the first literal resolved to size/2");

	check(literal->u.atom.args[0].kind == RELIC_ARG_VARIABLE &&
	    literal->u.atom.args[0].u.slot == 0, SCENARIO_ARGUMENTS,
	    "size/2's first argument is the slot X was numbered into");

	if (relic_value_intern_integer(ctx, 100, &domain) != 0) {
		check(false, SCENARIO_ARGUMENTS, "100 interns");
		relic_destroy(ctx);
		return;
	}
	check(literal->u.atom.args[1].kind == RELIC_ARG_CONSTANT &&
	    literal->u.atom.args[1].u.constant == domain, SCENARIO_ARGUMENTS,
	    "size/2's second argument is the interned integer 100");

	expected.type = RELIC_INTEGER;
	expected.v.integer = 100;
	check(relic_value_decode(ctx, literal->u.atom.args[1].u.constant,
	    &decoded) && decoded.type == expected.type &&
	    decoded.v.integer == expected.v.integer,
	    SCENARIO_ARGUMENTS, "the constant reads back as the value written");

	literal = &rule->body[1];
	check(literal->kind == RELIC_LITERAL_NEGATED, SCENARIO_ARGUMENTS,
	    "small/1 is a negated literal");

	relic_destroy(ctx);
}

/*
 * `_` in an argument of a positive or a negated body literal is a wildcard
 * and takes no slot, so a rule full of them holds no variables at all, and
 * one beside a named variable under `not` leaves that variable the only
 * slot.  In the head it would be read and can never be bound, so it takes a
 * slot there and the rule is rejected.
 */
static void
an_anonymous_variable_takes_a_slot_only_where_illegal(void)
{
	relic_t *ctx;
	const relic_rule_t *rule;
	const relic_literal_t *negated;
	diag_log_t log;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load(ctx, "edge(a, b).\nany :- edge(_, _).\n"
	    "source(X) :- edge(X, _), not edge(_, X).\n") ||
	    relic_check(ctx) != 0) {
		check(false, SCENARIO_WILDCARD, "the program is accepted");
		relic_destroy(ctx);
		return;
	}

	rule = rule_at(ctx, 0);
	if (rule == NULL || rule->body_length != 1) {
		check(false, SCENARIO_WILDCARD, "the rule was compiled");
		relic_destroy(ctx);
		return;
	}

	checkf(rule->variable_count == 0, SCENARIO_WILDCARD,
	    "two wildcards took %u slots", (unsigned int)rule->variable_count);

	check(rule->body[0].u.atom.args[0].kind == RELIC_ARG_WILDCARD &&
	    rule->body[0].u.atom.args[1].kind == RELIC_ARG_WILDCARD,
	    SCENARIO_WILDCARD, "both arguments compiled to wildcards");

	rule = rule_at(ctx, 1);
	if (rule == NULL || rule->body_length != 2) {
		check(false, SCENARIO_WILDCARD,
		    "the negated rule was compiled");
		relic_destroy(ctx);
		return;
	}

	checkf(rule->variable_count == 1, SCENARIO_WILDCARD,
	    "X and two wildcards took %u slots",
	    (unsigned int)rule->variable_count);

	negated = &rule->body[1];
	check(negated->kind == RELIC_LITERAL_NEGATED &&
	    negated->u.atom.args[0].kind == RELIC_ARG_WILDCARD &&
	    negated->u.atom.args[1].kind == RELIC_ARG_VARIABLE &&
	    negated->u.atom.args[1].u.slot == 0,
	    SCENARIO_WILDCARD, "the negated `_` compiled to a wildcard and X "
	    "to its slot");

	checkf(log.errors == 0 && log.warnings == 0, SCENARIO_WILDCARD,
	    "a wildcard is neither an error nor a singleton: %zu and %zu",
	    log.errors, log.warnings);

	relic_destroy(ctx);

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	check(load(ctx, "edge(a, b).\nhead(_) :- edge(_, _).\n") &&
	    relic_check(ctx) != 0, SCENARIO_WILDCARD,
	    "`_` in the head is rejected");
	checkf(log.errors == 1, SCENARIO_WILDCARD,
	    "the head's `_` is one error, not %zu", log.errors);
	check(rule_at(ctx, 0) == NULL, SCENARIO_WILDCARD,
	    "the rejected rule was not compiled");

	relic_destroy(ctx);
}

/*
 * A statement whose check fails is not compiled into a rule.  The program is
 * refused, so nothing would ever plan it, and a half-checked rule reachable
 * through the program would be a rule the evaluator could reach without the
 * invariant every later stage assumes.
 */
static void
a_rejected_statement_leaves_no_rule_behind(void)
{
	relic_t *ctx;
	diag_log_t log;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load(ctx, "bar(a).\ngood(X) :- bar(X).\nbad(X) :- bar(a).\n")) {
		check(false, SCENARIO_REJECTED, "the program loads");
		relic_destroy(ctx);
		return;
	}

	check(relic_check(ctx) != 0, SCENARIO_REJECTED,
	    "a rule with an unbound head variable is refused");

	checkf(rule_count(ctx) == 1, SCENARIO_REJECTED,
	    "the good rule was compiled, but %zu rules exist",
	    rule_count(ctx));

	checkf(log.errors == 1, SCENARIO_REJECTED,
	    "one unbound variable was reported as %zu errors", log.errors);

	relic_destroy(ctx);
}

/*
 * A check compiles the statements loaded since the previous one and nothing
 * else, so calling it twice with nothing loaded in between reports nothing
 * the second time.
 */
static void
a_check_compiles_only_what_is_new(void)
{
	relic_t *ctx;
	diag_log_t log;
	size_t after_first;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	/*
	 * A program of no statements is a valid program.  Checking one that
	 * was never loaded must therefore accept it rather than refuse
	 * something it has not been given.
	 */
	check(relic_check(ctx) == 0, SCENARIO_CACHED,
	    "a context with nothing loaded is accepted");
	checkf(rule_count(ctx) == 0, SCENARIO_CACHED,
	    "checking nothing compiled %zu rules", rule_count(ctx));

	/*
	 * A load is all or nothing, so a load that failed added no
	 * statements and the check that follows has nothing new to compile.
	 */
	check(!load(ctx, "broken(a b).\n"), SCENARIO_CACHED,
	    "a malformed program fails to load");
	bzero(&log, sizeof(log));

	check(relic_check(ctx) == 0, SCENARIO_CACHED,
	    "a failed load leaves the program checkable");
	checkf(rule_count(ctx) == 0 && log.errors == 0, SCENARIO_CACHED,
	    "checking after a failed load compiled %zu rules and reported "
	    "%zu errors", rule_count(ctx), log.errors);

	if (!load(ctx, "edge(a, b).\nlonely(X) :- edge(X, Y).\n")) {
		check(false, SCENARIO_CACHED, "the first program loads");
		relic_destroy(ctx);
		return;
	}

	check(relic_check(ctx) == 0, SCENARIO_CACHED,
	    "a program with only a singleton is accepted");
	checkf(log.warnings == 1 && log.notes == 1, SCENARIO_CACHED,
	    "the singleton was reported once: %zu warnings, %zu notes",
	    log.warnings, log.notes);

	after_first = rule_count(ctx);

	check(relic_check(ctx) == 0, SCENARIO_CACHED,
	    "checking again is still accepted");
	checkf(log.warnings == 1 && log.notes == 1, SCENARIO_CACHED,
	    "checking again repeated the singleton: %zu warnings, %zu notes",
	    log.warnings, log.notes);
	checkf(rule_count(ctx) == after_first, SCENARIO_CACHED,
	    "checking again recompiled the program: %zu rules became %zu",
	    after_first, rule_count(ctx));

	/*
	 * A second load appends, and the next check compiles exactly the
	 * statements it added.
	 */
	if (!load(ctx, "pair(X, Y) :- edge(X, Y).\n")) {
		check(false, SCENARIO_CACHED, "the second program loads");
		relic_destroy(ctx);
		return;
	}

	check(relic_check(ctx) == 0, SCENARIO_CACHED,
	    "the appended rule is accepted");
	checkf(rule_count(ctx) == after_first + 1, SCENARIO_CACHED,
	    "one rule was appended, making %zu rather than %zu",
	    rule_count(ctx), after_first + 1);
	checkf(log.warnings == 1, SCENARIO_CACHED,
	    "the first program's singleton was reported again: %zu warnings",
	    log.warnings);

	relic_destroy(ctx);
}

/*
 * An unsafe rule stays in the tree and stays unsafe, so loading good rules
 * after it cannot make the program usable again.  The rule is reported once;
 * the later check, with nothing new to report, says only that the program
 * cannot be used, so that its failure too reports an error.
 */
static void
a_program_that_held_a_bad_rule_stays_refused(void)
{
	relic_t *ctx;
	diag_log_t log;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load(ctx, "bar(a).\nbad(X) :- bar(a).\n")) {
		check(false, SCENARIO_STICKY, "the first program loads");
		relic_destroy(ctx);
		return;
	}

	check(relic_check(ctx) != 0 && log.errors == 1, SCENARIO_STICKY,
	    "the unsafe rule is refused, with one error");

	if (!load(ctx, "good(X) :- bar(X).\n")) {
		check(false, SCENARIO_STICKY, "the second program loads");
		relic_destroy(ctx);
		return;
	}

	check(relic_check(ctx) != 0, SCENARIO_STICKY,
	    "a blameless load does not repair the program");

	checkf(log.errors == 2 && relic_error(ctx) != NULL &&
	    strcmp(relic_error(ctx), UNUSABLE_ERROR) == 0, SCENARIO_STICKY,
	    "the later check should report only that the program cannot be "
	    "used, not %zu errors in all ending with \"%s\"", log.errors,
	    relic_error(ctx) != NULL ? relic_error(ctx) : "(none)");

	relic_destroy(ctx);
}

/*
 * Load and check `program` in a context of its own, recording what the load
 * and the check report in `log`.
 */
static void
checked_alone(const char *program, diag_log_t *log)
{
	relic_t *ctx;

	ctx = context_logging_to(log);
	if (ctx == NULL)
		return;

	if (load(ctx, program))
		(void)relic_check(ctx);

	relic_destroy(ctx);
}

/*
 * A variable is bound by a positive literal, or by an equality whose other
 * side reads only bound variables, and the closure applies that second rule
 * until nothing more is bound, so the order the assignments are written in
 * does not matter.  An equality with both sides free binds neither, a
 * variable inside an expression is never solved for, and `_` is never an
 * assignment's target.  A variable assigned and never read is a singleton.
 * The query rule is checked by the same closure.
 */
static void
an_assignment_binds_what_the_closure_reaches(void)
{
	static const struct {
		const char	*program;
		size_t		 errors;
		size_t		 warnings;
		const char	*why;
	} cases[] = {
		{ "n(1).\nr(Y) :- n(X), Y = X + 1.\n", 0, 0,
		    "an assignment from a positive literal's variable" },
		{ "n(1).\nr(X, Y) :- n(X), Y = 42.\n", 0, 0,
		    "an assignment from a constant" },
		{ "n(1).\nr(X, Y) :- n(X), 7 * 6 = Y.\n", 0, 0,
		    "an assignment written right to left" },
		{ "n(1).\nr(Z) :- Z = Y * 2, Y = X + 1, n(X).\n", 0, 0,
		    "assignments written before what binds their operands" },
		{ "n(1).\nr(X, Y) :- n(X), Y = X, Y > 1.\n", 0, 0,
		    "an assignment of a bare variable, then a test of it" },
		{ "n(1).\nr :- n(A), A > 0, X = Y.\n", 2, 0,
		    "an equality with both sides free binds neither" },
		{ "n(1).\nr(Y) :- n(X), X + Y = 3.\n", 1, 0,
		    "a variable inside an expression is not solved for" },
		{ "n(1).\nr(X) :- n(X), X = Y + 1.\n", 1, 0,
		    "a bound variable does not bind the expression's" },
		{ "n(1).\nr(X) :- n(X), _ = X + 1.\n", 1, 0,
		    "`_` is never an assignment's target" },
		{ "n(1).\nr(X) :- n(X), Y = X + 1.\n", 0, 1,
		    "a variable assigned and never read is a singleton" },
		{ "n(1).\nr(X) :- n(X), Y = Z, Z = Y.\n", 2, 0,
		    "two equalities waiting on each other bind neither" },
		{ "v(1).\nv(X), Y = X * 2?\n", 0, 0,
		    "a query's output column bound by an assignment" },
		{ "v(1).\nv(X), Y = Z * 2?\n", 2, 0,
		    "a query's variables bound by nothing" }
	};

	diag_log_t log;
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		checked_alone(cases[index].program, &log);
		checkf(log.errors == cases[index].errors &&
		    log.warnings == cases[index].warnings, SCENARIO_CLOSURE,
		    "%s: expected %zu errors and %zu warnings, got %zu and %zu",
		    cases[index].why, cases[index].errors,
		    cases[index].warnings, log.errors, log.warnings);
	}
}

/*
 * A comparison holds two expression trees whose leaves are resolved as an
 * atom's arguments are, to constants and slots, and whose every node knows
 * its height and the size of the tree below it.
 */
static void
a_comparison_compiles_to_two_trees_over_slots(void)
{
	const relic_comparison_t *comparison;
	const relic_expr_t *sum;
	const relic_rule_t *rule;
	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load(ctx, "n(1).\nr(Y) :- n(X), Y = -(X + 1) * 2.\n") ||
	    relic_check(ctx) != 0) {
		check(false, SCENARIO_EXPRESSION, "the program is accepted");
		relic_destroy(ctx);
		return;
	}

	rule = rule_at(ctx, 0);
	if (rule == NULL || rule->body_length != 2 ||
	    rule->body[1].kind != RELIC_LITERAL_COMPARISON) {
		check(false, SCENARIO_EXPRESSION, "the rule was compiled");
		relic_destroy(ctx);
		return;
	}

	comparison = &rule->body[1].u.comparison;
	check(comparison->op == RELIC_CMP_EQ &&
	    comparison->left->kind == RELIC_EXPR_VARIABLE &&
	    comparison->left->u.slot == 0 && comparison->left->nodes == 1 &&
	    comparison->left->height == 1, SCENARIO_EXPRESSION,
	    "the assigned variable is a one-node tree over Y's slot");

	check(comparison->right->kind == RELIC_EXPR_BINARY &&
	    comparison->right->u.binary.op == RELIC_ARITH_MULTIPLY &&
	    comparison->right->nodes == 6 && comparison->right->height == 4,
	    SCENARIO_EXPRESSION,
	    "the product holds six nodes in a tree four high");

	sum = comparison->right->u.binary.left;
	check(sum->kind == RELIC_EXPR_NEGATE &&
	    sum->u.operand->kind == RELIC_EXPR_BINARY &&
	    sum->u.operand->u.binary.left->kind == RELIC_EXPR_VARIABLE &&
	    sum->u.operand->u.binary.left->u.slot == 1 &&
	    sum->u.operand->u.binary.right->kind == RELIC_EXPR_CONSTANT,
	    SCENARIO_EXPRESSION, "the negated sum reads X's slot and a constant");

	check(relic_comparison_slots(comparison) == 0x3, SCENARIO_EXPRESSION,
	    "the comparison reads both slots");

	relic_destroy(ctx);
}

/*
 * A call compiles to a node of its function over its arguments' trees, in
 * the order written, and counts toward the height and the size of the tree
 * holding it as any node does.
 */
static void
a_call_compiles_to_a_node_over_its_arguments(void)
{
	const relic_comparison_t *comparison;
	const relic_expr_t *cat;
	const relic_expr_t *substr;
	const relic_rule_t *rule;
	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load(ctx, "source(m, \"m.c\").\n"
	    "object(M, O) :- source(M, S), "
	    "O = cat(substr(S, 0, length(S) - 2), \".o\").\n") ||
	    relic_check(ctx) != 0) {
		check(false, SCENARIO_CALL, "the program is accepted");
		relic_destroy(ctx);
		return;
	}

	rule = rule_at(ctx, 0);
	if (rule == NULL || rule->body_length != 2 ||
	    rule->body[1].kind != RELIC_LITERAL_COMPARISON) {
		check(false, SCENARIO_CALL, "the rule was compiled");
		relic_destroy(ctx);
		return;
	}

	comparison = &rule->body[1].u.comparison;
	cat = comparison->right;
	check(cat->kind == RELIC_EXPR_CALL && cat->u.call.fn == RELIC_FN_CAT &&
	    cat->u.call.argc == 2 && cat->nodes == 9 && cat->height == 5,
	    SCENARIO_CALL, "cat holds nine nodes in a tree five high");

	substr = cat->u.call.args[0];
	check(substr->kind == RELIC_EXPR_CALL &&
	    substr->u.call.fn == RELIC_FN_SUBSTR3 &&
	    substr->u.call.argc == 3 &&
	    substr->u.call.args[0]->kind == RELIC_EXPR_VARIABLE &&
	    substr->u.call.args[0]->u.slot == 2 &&
	    substr->u.call.args[2]->kind == RELIC_EXPR_BINARY &&
	    substr->u.call.args[2]->u.binary.left->kind == RELIC_EXPR_CALL &&
	    substr->u.call.args[2]->u.binary.left->u.call.fn ==
	    RELIC_FN_LENGTH, SCENARIO_CALL,
	    "substr of three arguments reads S, a constant and a difference");
	check(cat->u.call.args[1]->kind == RELIC_EXPR_CONSTANT,
	    SCENARIO_CALL, "cat's second argument is the constant");

	check(relic_comparison_slots(comparison) == 0x6, SCENARIO_CALL,
	    "the comparison reads O's slot and S's");

	relic_destroy(ctx);
}

/*
 * The call on the left of a rule's first comparison, or NULL when the rule
 * has no comparison or its left side is no call.
 */
static const relic_expr_call_t *
compared_call(const relic_rule_t *rule)
{
	const relic_expr_t *left;
	size_t i;

	if (rule == NULL)
		return (NULL);

	for (i = 0; i < rule->body_length; i++) {
		if (rule->body[i].kind != RELIC_LITERAL_COMPARISON)
			continue;
		left = rule->body[i].u.comparison.left;
		return (left->kind == RELIC_EXPR_CALL ? &left->u.call : NULL);
	}

	return (NULL);
}

/*
 * A match whose pattern is a string constant carries the pattern compiled
 * with its rule, the program the matcher compiles from the same bytes; one
 * whose pattern is a variable carries none, and is compiled as it runs.  A
 * constant pattern that is not a regular expression is one error, and
 * leaves no rule behind.
 */
static void
a_constant_pattern_is_compiled_with_its_rule(void)
{
	static const char alternation[] = "a|b*";
	relic_regex_inst_t expected[3 * (sizeof(alternation) - 1) + 1];
	const relic_expr_call_t *constant;
	const relic_expr_call_t *variable;
	relic_regex_error_t error;
	diag_log_t log;
	uint32_t count;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load(ctx, "name(\"ab\"). pattern(\"b\").\n"
	    "fixed(N) :- name(N), match(N, \"a|b*\") >= 0.\n"
	    "given(N) :- name(N), pattern(P), match(N, P) >= 0.\n") ||
	    relic_check(ctx) != 0) {
		check(false, SCENARIO_PATTERN, "the program is accepted");
		relic_destroy(ctx);
		return;
	}

	constant = compared_call(rule_at(ctx, 0));
	count = relic_regex_compile(alternation, sizeof(alternation) - 1,
	    expected, &error);
	check(constant != NULL && constant->fn == RELIC_FN_MATCH &&
	    constant->pattern != NULL && constant->pattern->count == count &&
	    bcmp(constant->pattern->insts, expected,
	    count * sizeof(expected[0])) == 0, SCENARIO_PATTERN,
	    "the constant pattern's node holds its compiled program");

	variable = compared_call(rule_at(ctx, 1));
	check(variable != NULL && variable->fn == RELIC_FN_MATCH &&
	    variable->pattern == NULL, SCENARIO_PATTERN,
	    "a variable pattern's node holds none");

	relic_destroy(ctx);

	checked_alone("name(\"ab\").\n"
	    "bad(N) :- name(N), match(N, \"(\") >= 0.\n", &log);
	check(log.errors == 1, SCENARIO_PATTERN,
	    "a constant pattern that is not one is one error");

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;
	(void)load(ctx, "bad(N) :- name(N), match(N, \"[z-a]\") >= 0.\n");
	(void)relic_check(ctx);
	check(rule_count(ctx) == 0, SCENARIO_PATTERN,
	    "the refused rule leaves nothing behind");
	relic_destroy(ctx);
}

/*
 * Keep the line the explain callback was given, less its indentation and
 * the word introducing it, in the buffer of PRINTED_LINE_MAX bytes
 * `callback_ctx` points at.
 */
static void
keep_printed_line(void *callback_ctx, const char *line)
{
	char *out;

	out = callback_ctx;

	line += strspn(line, " ");
	if (strncmp(line, "rule ", 5) == 0)
		line += 5;

	strlcpy(out, line, PRINTED_LINE_MAX);
}

/*
 * Load and check `program`, then print its last rule as the explain output
 * writes a rule, into `out`, which is PRINTED_LINE_MAX bytes.
 */
static bool
printed_rule(const char *program, char *out, size_t size)
{
	diag_log_t log;
	relic_t *ctx;
	bool printed;

	bzero(out, size);

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return (false);

	printed = false;
	if (load(ctx, program) && relic_check(ctx) == 0 && rule_count(ctx) > 0) {
		relic_explain_set(ctx, keep_printed_line, out);
		relic_explain_rule(ctx, rule_at(ctx, rule_count(ctx) - 1), NULL,
		    0);
		printed = out[0] != '\0';
	}

	relic_destroy(ctx);

	return (printed);
}

/*
 * A rule is printed with exactly the parentheses its expressions' trees
 * need: parsing the printed text builds the tree that was printed, so a
 * second print is the first, and two spellings of one tree print alike.  A
 * right operand as tight as its operator keeps its parentheses, since
 * `2 * (3 / 2)` and `A + (B + C)` compute other values, or overflow at
 * other values, without them.
 */
static void
a_rule_prints_as_text_that_reads_back_as_it(void)
{
	static const char facts[] = "n(1, 2, 3).\n";
	static const struct {
		const char	*comparison;
		const char	*printed;
	} cases[] = {
		{ "Y = 2 * (3 / 2)",		"Y = 2 * (3 / 2)" },
		{ "Y = A + (B + C)",		"Y = A + (B + C)" },
		{ "Y = (A + B) + C",		"Y = A + B + C" },
		{ "Y = A - (B - C)",		"Y = A - (B - C)" },
		{ "Y = (A mod B) * C",		"Y = A mod B * C" },
		{ "Y = A mod (B * C)",		"Y = A mod (B * C)" },
		{ "Y = -(A + 1) * 2",		"Y = -(A + 1) * 2" },
		{ "Y = (-A) * 2",		"Y = -A * 2" },
		{ "Y = - -A",			"Y = - -A" },
		{ "Y = -(-A)",			"Y = - -A" },
		{ "Y = -(1)",			"Y = -(1)" },
		{ "Y = - 1",			"Y = -1" },
		{ "Y = -(-1)",			"Y = - -1" },
		{ "(not) != A",			"(not) != A" },
		{ "(a) != A",			"a != A" },
		{ "Y = A - -1",			"Y = A - -1" },
		{ "Y = ((A))",			"Y = A" },
		{ "Y = -9223372036854775808",	"Y = -9223372036854775808" },
		{ "A * (B + C) - (A - B) / -C < 2 * A",
		    "A * (B + C) - (A - B) / -C < 2 * A" },
		{ "Y = length(A)",		"Y = length(A)" },
		{ "Y = (length(A))",		"Y = length(A)" },
		{ "Y = length((A))",		"Y = length(A)" },
		{ "Y = -length(A)",		"Y = -length(A)" },
		{ "Y = - length(A)",		"Y = -length(A)" },
		{ "Y = - -length(A)",		"Y = - -length(A)" },
		{ "Y = (length(A) + 1) * 2",	"Y = (length(A) + 1) * 2" },
		{ "Y = str(-(1))",		"Y = str(-(1))" },
		{ "Y = substr(A, -1, (2 + 3) * B)",
		    "Y = substr(A, -1, (2 + 3) * B)" },
		{ "Y = cat(substr(A, 0, length(A) - 2), \".o\")",
		    "Y = cat(substr(A, 0, length(A) - 2), \".o\")" },
		{ "Y = match(A, \"\\\\.c$\")",
		    "Y = match(A, \"\\\\.c$\")" },
		{ "index(A, \"..\") >= 0",	"index(A, \"..\") >= 0" },
		{ "str(A) = cat(str(B), str(C))",
		    "str(A) = cat(str(B), str(C))" }
	};

	char again[PRINTED_LINE_MAX];
	char expected[PRINTED_LINE_MAX];
	char first[PRINTED_LINE_MAX];
	char program[PRINTED_LINE_MAX * 2];
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		snprintf(program, sizeof(program),
		    "%sr(Y) :- n(A, B, C), Y = A, %s.\n", facts,
		    cases[index].comparison);
		snprintf(expected, sizeof(expected),
		    "r(Y) :- n(A, B, C), Y = A, %s.", cases[index].printed);

		if (!printed_rule(program, first, sizeof(first))) {
			checkf(false, SCENARIO_PRINTED, "`%s` should print",
			    cases[index].comparison);
			continue;
		}

		checkf(strcmp(first, expected) == 0, SCENARIO_PRINTED,
		    "`%s` should print as `%s`, not `%s`",
		    cases[index].comparison, expected, first);

		snprintf(program, sizeof(program), "%s%s\n", facts, first);
		checkf(printed_rule(program, again, sizeof(again)) &&
		    strcmp(again, first) == 0, SCENARIO_PRINTED,
		    "`%s` should read back and print again as itself, not `%s`",
		    first, again);
	}
}

/*
 * A tree the parser accepted prints as text the parser accepts: the printed
 * parentheses and minuses nest no deeper than the tree does, so a rule at
 * the nesting bound reads back as itself.  Thirty-one negations over a
 * variable, `X + X * (...)` at height 32, thirty-one calls around a
 * variable, and ten negated calls nested each in the third argument of
 * the next, a tree 31 high, are each printed and read back.
 */
static void
a_rule_at_the_nesting_bound_prints_within_it(void)
{
	char again[PRINTED_LINE_MAX];
	char first[PRINTED_LINE_MAX];
	char program[PRINTED_LINE_MAX * 2];
	char expression[512];
	size_t shape;
	size_t i;

	for (shape = 0; shape < 4; shape++) {
		expression[0] = '\0';
		if (shape == 0) {
			for (i = 0; i < 31; i++)
				strlcat(expression, "-", sizeof(expression));
			strlcat(expression, "A", sizeof(expression));
		} else if (shape == 1) {
			for (i = 0; i < 15; i++)
				strlcat(expression, "A + A * (",
				    sizeof(expression));
			strlcat(expression, "A + A", sizeof(expression));
			for (i = 0; i < 15; i++)
				strlcat(expression, ")", sizeof(expression));
		} else if (shape == 2) {
			for (i = 0; i < 31; i++)
				strlcat(expression, "str(", sizeof(expression));
			strlcat(expression, "A", sizeof(expression));
			for (i = 0; i < 31; i++)
				strlcat(expression, ")", sizeof(expression));
		} else {
			for (i = 0; i < 10; i++)
				strlcat(expression, "-length(substr(A, 0, ",
				    sizeof(expression));
			strlcat(expression, "A", sizeof(expression));
			for (i = 0; i < 10; i++)
				strlcat(expression, "))", sizeof(expression));
		}

		snprintf(program, sizeof(program),
		    "n(1).\nr(Y) :- n(A), Y = %s.\n", expression);
		if (!printed_rule(program, first, sizeof(first))) {
			checkf(false, SCENARIO_PRINTED, "`%s` should print",
			    expression);
			continue;
		}

		snprintf(program, sizeof(program), "n(1).\n%s\n", first);
		checkf(printed_rule(program, again, sizeof(again)) &&
		    strcmp(again, first) == 0, SCENARIO_PRINTED,
		    "`%s` should read back and print again as itself, not `%s`",
		    first, again);
	}
}

static const char *
pred_spelling(const relic_t *ctx, relic_pred_id_t pred)
{
	return (relic_pred_of(ctx, pred)->name.bytes);
}

/*
 * An aggregate compiles to a group rule, published just before the rule
 * holding it with the same origin.  The group rule's head is the shared
 * variables in the order the outer rule numbered them, then the local
 * variables in the order the body writes them: dir_size's group is
 * (Dir, F, Size), keyed on its first column, folding its third.  The
 * aggregated variable is local; the result variable is the outer rule's.
 */
static void
an_aggregate_reads_a_group_rule_of_its_own(void)
{
	const relic_aggregate_t *aggregate;
	const relic_rule_t *group;
	const relic_rule_t *outer;
	relic_t *ctx;
	diag_log_t log;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load(ctx, "dir(src).\nfile(main, src).\nsize(main, 1200).\n"
	    "dir_size(Dir, S) :- dir(Dir),\n"
	    "    S = sum(Size) { file(F, Dir), size(F, Size) }.\n") ||
	    relic_check(ctx) != 0 || rule_count(ctx) != 2) {
		check(false, SCENARIO_GROUP, "the rule compiles to two rules");
		relic_destroy(ctx);
		return;
	}

	group = rule_at(ctx, 0);
	outer = rule_at(ctx, 1);
	if (outer->body_length != 2 ||
	    outer->body[1].kind != RELIC_LITERAL_AGGREGATE) {
		check(false, SCENARIO_GROUP, "the outer rule's second literal "
		    "is the aggregate");
		relic_destroy(ctx);
		return;
	}
	aggregate = &outer->body[1].u.aggregate;

	check(group->origin == outer->origin && group->head ==
	    aggregate->source && aggregate->body->head ==
	    aggregate->source, SCENARIO_GROUP,
	    "the group rule comes first, from the same statement");
	check(strcmp(pred_spelling(ctx, group->head), "agg$dir_size$2$0$2") ==
	    0 && group->head_arity == 3, SCENARIO_GROUP,
	    "the group is agg$dir_size$2$0$2/3");
	check(group->variable_count == 3 &&
	    spelled(&group->variables[0].name, "Dir") &&
	    spelled(&group->variables[1].name, "F") &&
	    spelled(&group->variables[2].name, "Size"), SCENARIO_GROUP,
	    "the group's columns are Dir, F, Size");
	check(group->head_args[0].kind == RELIC_ARG_VARIABLE &&
	    group->head_args[0].u.slot == 0 &&
	    group->head_args[2].kind == RELIC_ARG_VARIABLE &&
	    group->head_args[2].u.slot == 2, SCENARIO_GROUP,
	    "column c of the group is slot c of its rule");
	check(aggregate->fn == RELIC_AGG_SUM && aggregate->shared_count == 1 &&
	    aggregate->shared[0] == 0 && aggregate->key_mask == 1 &&
	    aggregate->column == 2, SCENARIO_GROUP, "the aggregate is keyed "
	    "on the outer Dir in the group's first column and folds its third");
	check(outer->variable_count == 2 &&
	    spelled(&outer->variables[aggregate->result].name, "S"),
	    SCENARIO_GROUP, "the outer rule numbers only Dir and S");
	check(relic_pred_of(ctx, outer->head)->program_rules == 1 &&
	    relic_pred_of(ctx, group->head)->program_rules == 1,
	    SCENARIO_GROUP, "each rule counts among its own head's program "
	    "rules");

	relic_destroy(ctx);
}

/*
 * A group predicate is named for the head's name and arity, the rule's
 * position among the head's program rules, and the aggregate's position in
 * the body, counted from 1: two aggregates in one rule and a second rule
 * of the same head each get their own.  A query's group is named for its
 * position alone, so every query reuses the one predicate at an arity, and
 * preparing the same query again interns nothing.  A statement the checker
 * rejects publishes none of its rules.
 */
static void
a_group_predicate_is_named_for_where_it_is(void)
{
	static const char *const names[] = {
		"agg$c$2$0$2", "agg$c$2$0$3", "agg$c$2$1$2"
	};
	relic_query_t *first;
	relic_query_t *second;
	relic_t *ctx;
	diag_log_t log;
	size_t preds;
	size_t i;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load(ctx, "e(a, b).\n"
	    "c(X, N) :- e(X, _), N = count { e(X, Y), e(Y, _) },\n"
	    "    N = count { e(Z, X), e(_, Z) }.\n"
	    "c(X, N) :- e(_, X), N = count { e(Y, X), e(_, Y) }.\n") ||
	    relic_check(ctx) != 0 || rule_count(ctx) != 5) {
		check(false, SCENARIO_GROUP_NAME, "the two rules compile to "
		    "five");
		relic_destroy(ctx);
		return;
	}

	for (i = 0; i < 3; i++)
		checkf(strcmp(pred_spelling(ctx, rule_at(ctx, i < 2 ? i :
		    3)->head), names[i]) == 0, SCENARIO_GROUP_NAME,
		    "group %zu is %s, not %s", i, names[i],
		    pred_spelling(ctx, rule_at(ctx, i < 2 ? i : 3)->head));

	first = relic_query_prepare(ctx, NULL,
	    "e(X, _), N = count { e(X, Y), e(Y, _) }?");
	preds = relic_pred_count(ctx);
	second = relic_query_prepare(ctx, NULL,
	    "e(X, _), N = count { e(X, Y), e(Y, _) }?");
	check(first != NULL && second != NULL && first->group_rule_count == 1 &&
	    second->group_rule_count == 1 && first->group_rules[0].head ==
	    second->group_rules[0].head && strcmp(pred_spelling(ctx,
	    first->group_rules[0].head), "agg$$q0$2") == 0,
	    SCENARIO_GROUP_NAME, "two queries fold one group predicate, "
	    "agg$$q0$2");
	check(relic_pred_count(ctx) == preds, SCENARIO_GROUP_NAME,
	    "preparing the query again interns no predicate");
	relic_query_destroy(first);
	relic_query_destroy(second);

	if (!load(ctx, "bad(N) :- N = count { e(X, Y), e(Y, _) }, M > 1.\n")) {
		check(false, SCENARIO_GROUP_NAME, "the unsafe rule loads");
		relic_destroy(ctx);
		return;
	}
	check(relic_check(ctx) != 0 && rule_count(ctx) == 5,
	    SCENARIO_GROUP_NAME, "an unsafe statement publishes neither its "
	    "rule nor its group rule");

	relic_destroy(ctx);
}

/*
 * The aggregate literal of the last rule in the table, the outer rule of
 * the last statement, or NULL when it holds none.
 */
static const relic_aggregate_t *
last_aggregate(const relic_t *ctx)
{
	const relic_rule_t *outer;
	size_t i;

	if (rule_count(ctx) == 0)
		return (NULL);

	outer = rule_at(ctx, rule_count(ctx) - 1);
	for (i = 0; i < outer->body_length; i++) {
		if (outer->body[i].kind == RELIC_LITERAL_AGGREGATE)
			return (&outer->body[i].u.aggregate);
	}

	return (NULL);
}

/*
 * The number of group predicates the context has interned.
 */
static size_t
group_predicates(const relic_t *ctx)
{
	size_t count;
	relic_pred_id_t id;

	count = 0;
	for (id = 1; id <= relic_pred_count(ctx); id++) {
		if (relic_pred_of(ctx, id)->kind == RELIC_PRED_GROUP)
			count++;
	}

	return (count);
}

/*
 * A body that is exactly one positive atom of pairwise distinct named
 * variables, a nullary atom included, denotes the atom's own relation, so
 * the statement compiles to its outer rule alone, no group predicate is
 * interned, and the literal folds the atom's predicate through a compiled
 * body that has no head.  A wildcard, a constant, a repeated variable, a
 * second literal, a comparison or a negation makes the body another
 * relation, and it gets a group rule; so does every body under
 * RELIC_F_GROUP_RULES.
 */
static void
a_body_that_is_a_relation_is_folded_in_place(void)
{
	static const struct {
		const char	*rule;
		bool		 in_place;
	} cases[] = {
		{ "r(X, N) :- f(X), N = count { e(X, Y) }.\n", true },
		{ "r(X, N) :- f(X), N = count { e(Y, X) }.\n", true },
		{ "r(N) :- N = count { ready }.\n", true },
		{ "r(M) :- M = max(Y) { e(X, Y) }.\n", true },
		{ "r(X, N) :- f(X), N = count { e(X, _) }.\n", false },
		{ "r(X, N) :- f(X), N = count { e(X, a) }.\n", false },
		{ "r(X, N) :- f(X), N = count { e(X, X) }.\n", false },
		{ "r(X, N) :- f(X), N = count { e(X, Y), f(Y) }.\n", false },
		{ "r(X, N) :- f(X), N = count { e(X, Y), Y != a }.\n", false },
		{ "r(X, N) :- f(X), N = count { e(X, Y), not f(Y) }.\n",
		    false }
	};
	const relic_aggregate_t *aggregate;
	relic_config_t config;
	relic_t *ctx;
	diag_log_t log;
	unsigned int flags;
	size_t i;
	bool in_place;

	bzero(&config, sizeof(config));
	for (flags = 0; flags <= RELIC_F_GROUP_RULES;
	    flags += RELIC_F_GROUP_RULES) {
		for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			in_place = cases[i].in_place && flags == 0;
			config.flags = flags;
			ctx = relic_create_config(&config, sizeof(config));
			if (ctx == NULL) {
				check(false, SCENARIO_IN_PLACE,
				    "a context is created");
				return;
			}
			bzero(&log, sizeof(log));
			relic_diag_set(ctx, record_diagnostic, &log);

			aggregate = NULL;
			if (load(ctx, "e(a, b).\nf(a).\nready.\n") &&
			    load(ctx, cases[i].rule) && relic_check(ctx) == 0)
				aggregate = last_aggregate(ctx);

			checkf(aggregate != NULL &&
			    rule_count(ctx) == (in_place ? 1u : 2u) &&
			    group_predicates(ctx) == (in_place ? 0u : 1u),
			    SCENARIO_IN_PLACE, "%s under flags %#x compiles to "
			    "%s", cases[i].rule, flags, in_place ?
			    "its rule alone" : "a group rule and its rule");
			if (aggregate == NULL) {
				relic_destroy(ctx);
				continue;
			}

			checkf(in_place ? aggregate->body->head ==
			    RELIC_ID_NONE && aggregate->body->head_arity == 0 &&
			    aggregate->body->head_args == NULL &&
			    aggregate->body->body_length == 1 &&
			    aggregate->body->body[0].u.atom.pred ==
			    aggregate->source :
			    aggregate->body->head == aggregate->source &&
			    rule_at(ctx, 0)->head == aggregate->source,
			    SCENARIO_IN_PLACE, "%s under flags %#x folds %s",
			    cases[i].rule, flags, in_place ? "the atom's "
			    "relation through a body with no head" :
			    "the group relation its group rule derives");

			relic_destroy(ctx);
		}

		/*
		 * An aggregated variable the atom does not name makes no
		 * relation of the atom: the body goes to a group rule, whose
		 * check reports the variable once, as it does under either
		 * flag.
		 */
		config.flags = flags;
		ctx = relic_create_config(&config, sizeof(config));
		if (ctx == NULL) {
			check(false, SCENARIO_IN_PLACE, "a context is created");
			return;
		}
		bzero(&log, sizeof(log));
		relic_diag_set(ctx, record_diagnostic, &log);
		checkf(load(ctx, "e(a, b).\nr(S) :- S = sum(W) { e(X, Y) }.\n") &&
		    relic_check(ctx) != 0 && log.errors == 1 &&
		    rule_count(ctx) == 0, SCENARIO_IN_PLACE, "under flags %#x "
		    "a sum of a variable its atom does not name is refused "
		    "once and publishes nothing", flags);
		relic_destroy(ctx);
	}
}

/*
 * A fold in place is keyed on the columns of the atom its shared variables
 * stand in, whichever they are, and `shared` lists the outer slot of each
 * key column in increasing column order, not in slot order: in w(Y, X, W)
 * under a rule numbering X before Y, column 0 is keyed from Y's slot.  The
 * compiled body numbers the atom's variables in argument order, so slot c
 * is column c of the relation folded and the aggregated column is the
 * aggregated variable's position.
 */
static void
a_fold_in_place_is_keyed_where_its_key_stands(void)
{
	static const struct {
		const char	*rule;
		uint32_t	 key_mask;
		const char	*key_names[2];
		uint16_t	 column;
	} cases[] = {
		{ "r(X, N) :- s(X, _), N = count { w(X, Y, Z) }.\n",
		    0x1, { "X", NULL }, 0 },
		{ "r(X, N) :- s(X, _), N = count { w(Y, X, Z) }.\n",
		    0x2, { "X", NULL }, 0 },
		{ "r(X, N) :- s(X, _), N = count { w(Y, Z, X) }.\n",
		    0x4, { "X", NULL }, 0 },
		{ "r(X, Y, S) :- s(X, Y), S = sum(W) { w(Y, X, W) }.\n",
		    0x3, { "Y", "X" }, 2 },
		{ "r(S) :- S = sum(W) { w(A, W, B) }.\n",
		    0x0, { NULL, NULL }, 1 }
	};
	const relic_aggregate_t *aggregate;
	const relic_rule_t *outer;
	const relic_rule_t *body;
	relic_t *ctx;
	diag_log_t log;
	size_t i;
	uint16_t k;
	uint16_t c;
	bool keyed;
	bool numbered;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		ctx = context_logging_to(&log);
		if (ctx == NULL)
			return;

		aggregate = NULL;
		if (load(ctx, "w(a, b, 1).\ns(a, b).\n") &&
		    load(ctx, cases[i].rule) && relic_check(ctx) == 0 &&
		    rule_count(ctx) == 1)
			aggregate = last_aggregate(ctx);
		if (aggregate == NULL) {
			checkf(false, SCENARIO_KEY_COLUMNS, "%s compiles to "
			    "one rule holding a fold", cases[i].rule);
			relic_destroy(ctx);
			continue;
		}
		outer = rule_at(ctx, 0);
		body = aggregate->body;

		keyed = aggregate->key_mask == cases[i].key_mask;
		for (k = 0; k < 2; k++) {
			if (cases[i].key_names[k] == NULL) {
				keyed = keyed && aggregate->shared_count == k;
				break;
			}
			keyed = keyed && aggregate->shared_count > k &&
			    spelled(&outer->variables[
			    aggregate->shared[k]].name, cases[i].key_names[k]);
		}
		checkf(keyed && (aggregate->shared_count > 0 ||
		    aggregate->shared == NULL), SCENARIO_KEY_COLUMNS,
		    "%s is keyed on columns %#x from the slots of its shared "
		    "variables in column order", cases[i].rule,
		    (unsigned int)cases[i].key_mask);

		numbered = body->variable_count == 3 &&
		    body->body[0].u.atom.pred == aggregate->source;
		for (c = 0; numbered && c < 3; c++)
			numbered = body->body[0].u.atom.args[c].kind ==
			    RELIC_ARG_VARIABLE &&
			    body->body[0].u.atom.args[c].u.slot == c;
		checkf(numbered && (aggregate->fn == RELIC_AGG_COUNT ||
		    aggregate->column == cases[i].column),
		    SCENARIO_KEY_COLUMNS, "%s numbers slot c as column c and "
		    "folds column %u", cases[i].rule,
		    (unsigned int)cases[i].column);

		relic_destroy(ctx);
	}
}

int
main(void)
{
	variables_are_numbered_as_they_are_written();
	an_aggregate_reads_a_group_rule_of_its_own();
	a_group_predicate_is_named_for_where_it_is();
	a_body_that_is_a_relation_is_folded_in_place();
	a_fold_in_place_is_keyed_where_its_key_stands();
	a_literal_resolves_to_the_arguments_it_names();
	an_anonymous_variable_takes_a_slot_only_where_illegal();
	a_rejected_statement_leaves_no_rule_behind();
	a_check_compiles_only_what_is_new();
	a_program_that_held_a_bad_rule_stays_refused();
	an_assignment_binds_what_the_closure_reaches();
	a_comparison_compiles_to_two_trees_over_slots();
	a_call_compiles_to_a_node_over_its_arguments();
	a_constant_pattern_is_compiled_with_its_rule();
	a_rule_prints_as_text_that_reads_back_as_it();
	a_rule_at_the_nesting_bound_prints_within_it();

	return (check_summary("test_rule"));
}
