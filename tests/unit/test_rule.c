/*
 * Copyright (c) 2026 Stanislav Sedov
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
#define	SCENARIO_CACHED		"a check compiles only what is new"
#define	SCENARIO_REJECTED	"a rejected statement leaves no rule behind"
#define	SCENARIO_SLOTS		"variables are numbered as they are written"
#define	SCENARIO_STICKY		"a program that held a bad rule stays refused"
#define	SCENARIO_WILDCARD	"`_` takes a slot only where it is illegal"

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
static uint32_t	 symbol(relic_t *ctx, const char *name);
static void	 a_check_compiles_only_what_is_new(void);
static void	 a_literal_resolves_to_the_arguments_it_names(void);
static void	 a_program_that_held_a_bad_rule_stays_refused(void);
static void	 a_rejected_statement_leaves_no_rule_behind(void);
static void	 an_anonymous_variable_takes_a_slot_only_where_illegal(void);
static void	 variables_are_numbered_as_they_are_written(void);

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
 * The identifier a spelling was interned under, or RELIC_ID_NONE when this
 * context never saw it.  Interning would assign one, so a test that asks
 * about a name the program did not mention must not use this to find out.
 */
static uint32_t
symbol(relic_t *ctx, const char *name)
{
	uint32_t id;

	if (relic_symbol_intern(ctx, name, strlen(name), &id) != 0)
		return (RELIC_ID_NONE);

	return (id);
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

	rule = rule_at(ctx, 1);
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

	checkf(rule->variables[0].name == symbol(ctx, "X") &&
	    rule->variables[1].name == symbol(ctx, "Z") &&
	    rule->variables[2].name == symbol(ctx, "Y"), SCENARIO_SLOTS,
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

	rule = rule_at(ctx, 1);
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
	    pred->name == symbol(ctx, "size"), SCENARIO_ARGUMENTS,
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
	check(relic_value_of(ctx, literal->u.atom.args[1].u.constant) != NULL &&
	    relic_value_of(ctx, domain)->v.integer == expected.v.integer,
	    SCENARIO_ARGUMENTS, "the constant reads back as the value written");

	literal = &rule->body[1];
	check(literal->kind == RELIC_LITERAL_NEGATED, SCENARIO_ARGUMENTS,
	    "small/1 is a negated literal");

	relic_destroy(ctx);
}

/*
 * `_` in an argument of a positive body literal is a wildcard and takes no
 * slot, so a rule full of them holds no variables at all.  Anywhere else it
 * can never be bound, and the rule is rejected.
 */
static void
an_anonymous_variable_takes_a_slot_only_where_illegal(void)
{
	relic_t *ctx;
	const relic_rule_t *rule;
	diag_log_t log;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load(ctx, "edge(a, b).\nany :- edge(_, _).\n") ||
	    relic_check(ctx) != 0) {
		check(false, SCENARIO_WILDCARD, "the program is accepted");
		relic_destroy(ctx);
		return;
	}

	rule = rule_at(ctx, 1);
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

	checkf(log.errors == 0 && log.warnings == 0, SCENARIO_WILDCARD,
	    "a wildcard is neither an error nor a singleton: %zu and %zu",
	    log.errors, log.warnings);

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

	checkf(rule_count(ctx) == 2, SCENARIO_REJECTED,
	    "the fact and the good rule were compiled, but %zu rules exist",
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
 * after it cannot make the program usable again.
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

	check(relic_check(ctx) != 0, SCENARIO_STICKY,
	    "the unsafe rule is refused");

	if (!load(ctx, "good(X) :- bar(X).\n")) {
		check(false, SCENARIO_STICKY, "the second program loads");
		relic_destroy(ctx);
		return;
	}

	check(relic_check(ctx) != 0, SCENARIO_STICKY,
	    "a blameless load does not repair the program");

	checkf(log.errors == 1, SCENARIO_STICKY,
	    "the unsafe rule was reported %zu times", log.errors);

	relic_destroy(ctx);
}

int
main(void)
{
	variables_are_numbered_as_they_are_written();
	a_literal_resolves_to_the_arguments_it_names();
	an_anonymous_variable_takes_a_slot_only_where_illegal();
	a_rejected_statement_leaves_no_rule_behind();
	a_check_compiles_only_what_is_new();
	a_program_that_held_a_bad_rule_stays_refused();

	return (check_summary("test_rule"));
}
