/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the parser and the loading entry points.
 *
 * The golden language tests under tests/lang/ compare the diagnostics a
 * malformed file produces, byte for byte, and are the record of what the
 * front end says.  What they cannot see is the tree, so that is what this
 * suite examines: the shape a statement takes, the values its terms were
 * interned to, and the two properties a caller depends on and no message
 * reveals -- that a failed load adds nothing at all, and that a successful
 * one appends to whatever was loaded before it.
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

#define	SCENARIO_APPEND		"loads append to one program"
#define	SCENARIO_ATOMIC		"a failed load changes nothing"
#define	SCENARIO_LIMIT		"a structural limit is enforced where written"
#define	SCENARIO_NEGATION	"`not` is a keyword only where a literal begins"
#define	SCENARIO_RECOVERY	"one broken statement costs one statement"
#define	SCENARIO_SHAPE		"a statement is the shape it was written in"
#define	SCENARIO_TERMS		"a term holds the value it spells"

typedef struct diag_log diag_log_t;

/*
 * Everything the context reported during a load, so that a test can count
 * the messages without a callback that writes to stderr.  Only the count and
 * the first message are kept; a test that wants the exact text is a golden
 * language test rather than this.
 */
struct diag_log {
	size_t	errors;
	char	first[256];
};

static relic_ast_stmt_t *statement_at(relic_t *ctx, size_t index);
static size_t	 statement_count(const relic_t *ctx);
static void	 a_failed_load_adds_nothing(void);
static void	 arity_is_limited_where_the_argument_is_written(void);
static void	 loads_append_to_one_program(void);
static void	 not_is_a_keyword_only_where_a_literal_begins(void);
static void	 one_broken_statement_costs_one_statement(void);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static relic_t	*context_logging_to(diag_log_t *log);
static size_t	 facts_held(relic_t *ctx, const char *name, uint16_t arity);
static void	 statements_keep_the_shape_they_were_written_in(void);
static void	 terms_hold_the_values_they_spell(void);

static void
record_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_UNUSED, const char *message)
{
	diag_log_t *log;

	if (level != RELIC_ERROR)
		return;

	log = callback_ctx;
	if (log->errors == 0)
		strlcpy(log->first, message, sizeof(log->first));
	log->errors++;
}

static relic_t *
context_logging_to(diag_log_t *log)
{
	relic_t *ctx;

	bzero(log, sizeof(*log));

	ctx = relic_create();
	if (ctx == NULL)
		return (NULL);

	relic_diag_set(ctx, record_diagnostic, log);

	return (ctx);
}

static size_t
statement_count(const relic_t *ctx)
{
	return (ctx->ast.count);
}

/*
 * The facts the base store of name/arity holds: where a loaded fact goes,
 * rather than into the statements.
 */
static size_t
facts_held(relic_t *ctx, const char *name, uint16_t arity)
{
	relic_pred_id_t pred;
	uint32_t symbol;

	if (relic_symbol_intern(ctx, name, strlen(name), &symbol) != 0 ||
	    relic_pred_intern(ctx, symbol, arity, NULL, &pred) != 0)
		return (0);

	return (relic_base_count(ctx, pred));
}

/*
 * The statement at `index` in load order, or NULL when there are fewer than
 * that many.  Statements are a list rather than an array, so every test that
 * wants the third one walks to it through here instead of open-coding the
 * traversal.
 */
static relic_ast_stmt_t *
statement_at(relic_t *ctx, size_t index)
{
	relic_ast_stmt_t *statement;
	size_t position;

	position = 0;
	TAILQ_FOREACH(statement, &ctx->ast.statements, link) {
		if (position == index)
			return (statement);
		position++;
	}

	return (NULL);
}

/*
 * Facts, rules and a query, and what each one becomes.  A fact becomes a
 * tuple of its predicate's base store and no statement; a rule keeps its
 * head and body, and a query has a body and no head.
 */
static void
statements_keep_the_shape_they_were_written_in(void)
{
	static const char program[] =
	    "edge(a, b).\n"
	    "initialized.\n"
	    "started :- initialized.\n"
	    "ready(X) :- todo(X), not blocked(X), X != done.\n"
	    "path(a, Y)?\n";

	relic_ast_stmt_t *zero_arity;
	relic_ast_stmt_t *rule;
	relic_ast_stmt_t *query;
	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_SHAPE, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "shape.dl", program) == 0, SCENARIO_SHAPE,
	    "the program should load");
	check(log.errors == 0, SCENARIO_SHAPE,
	    "a well-formed program should report nothing");
	check(statement_count(ctx) == 3, SCENARIO_SHAPE,
	    "three statements should have been stored");
	check(facts_held(ctx, "edge", 2) == 1 &&
	    facts_held(ctx, "initialized", 0) == 1, SCENARIO_SHAPE,
	    "each fact should be a tuple of its predicate's base store");

	zero_arity = statement_at(ctx, 0);
	rule = statement_at(ctx, 1);
	query = statement_at(ctx, 2);

	check(zero_arity != NULL && rule != NULL && query != NULL,
	    SCENARIO_SHAPE, "each of the three statements should be reachable");
	if (zero_arity == NULL || rule == NULL || query == NULL) {
		relic_destroy(ctx);
		return;
	}

	check(zero_arity->where.line == 3, SCENARIO_SHAPE,
	    "a statement should point at where it began");
	check(zero_arity->head.arity == 0 && zero_arity->head.terms == NULL,
	    SCENARIO_SHAPE,
	    "a predicate with no argument list should have arity zero");

	check(rule->body_length == 3, SCENARIO_SHAPE,
	    "a rule should keep every body literal");
	check(rule->body[0].kind == RELIC_LITERAL_POSITIVE &&
	    rule->body[1].kind == RELIC_LITERAL_NEGATED &&
	    rule->body[2].kind == RELIC_LITERAL_COMPARISON, SCENARIO_SHAPE,
	    "each body literal should keep its own kind");
	check(rule->body[2].u.comparison.op == RELIC_CMP_NE, SCENARIO_SHAPE,
	    "a comparison should keep its operator");

	check(query->kind == RELIC_STMT_QUERY && query->body_length == 1,
	    SCENARIO_SHAPE, "a query should be a body with no head");

	relic_destroy(ctx);
}

/*
 * A constant becomes the domain identifier a tuple column will hold, and two
 * spellings of one value become one identifier.  A variable keeps its name,
 * and a bare `_` is marked anonymous because every occurrence of one is a
 * distinct variable however identical the names are.
 */
static void
terms_hold_the_values_they_spell(void)
{
	static const char program[] = "f(a, \"a\", 120, X, _, _Kept, a).\n";

	relic_value_t value;
	relic_ast_stmt_t *statement;
	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_TERMS, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "terms.dl", program) == 0, SCENARIO_TERMS,
	    "the program should load");

	statement = statement_at(ctx, 0);
	check(statement != NULL && statement->head.arity == 7, SCENARIO_TERMS,
	    "seven arguments should have been stored");
	if (statement == NULL || statement->head.arity != 7) {
		relic_destroy(ctx);
		return;
	}

	check(statement->head.terms[0].kind == RELIC_TERM_CONSTANT &&
	    relic_value_decode(ctx, statement->head.terms[0].u.constant,
	    &value) && value.type == RELIC_SYMBOL, SCENARIO_TERMS,
	    "a bare identifier should be interned as a symbol");

	check(relic_value_decode(ctx, statement->head.terms[1].u.constant,
	    &value) && value.type == RELIC_STRING, SCENARIO_TERMS,
	    "a quoted literal should be interned as a string");
	check(statement->head.terms[0].u.constant !=
	    statement->head.terms[1].u.constant, SCENARIO_TERMS,
	    "the symbol a and the string \"a\" should be different values");

	check(relic_value_decode(ctx, statement->head.terms[2].u.constant,
	    &value) && value.type == RELIC_INTEGER &&
	    value.v.integer == 120, SCENARIO_TERMS,
	    "an integer literal should be interned as its value");

	check(statement->head.terms[3].kind == RELIC_TERM_VARIABLE &&
	    !statement->head.terms[3].u.variable.anonymous, SCENARIO_TERMS,
	    "X should be an ordinary named variable");
	check(statement->head.terms[4].kind == RELIC_TERM_VARIABLE &&
	    statement->head.terms[4].u.variable.anonymous, SCENARIO_TERMS,
	    "a bare _ should be marked anonymous");
	check(statement->head.terms[5].kind == RELIC_TERM_VARIABLE &&
	    !statement->head.terms[5].u.variable.anonymous, SCENARIO_TERMS,
	    "_Kept is a named variable rather than an anonymous one");

	check(statement->head.terms[0].u.constant ==
	    statement->head.terms[6].u.constant, SCENARIO_TERMS,
	    "one symbol written twice should be one value");

	relic_destroy(ctx);
}

/*
 * `not` introduces a negated literal only where a literal may begin, so it
 * remains an ordinary symbol value everywhere else.
 */
static void
not_is_a_keyword_only_where_a_literal_begins(void)
{
	static const char program[] =
	    "status(foo, not).\n"
	    "ready(X) :- todo(X), not blocked(X).\n";

	relic_ast_stmt_t *statement;
	const relic_relation_t *store;
	relic_value_t value;
	relic_pred_id_t pred;
	uint32_t symbol;
	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_NEGATION, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "not.dl", program) == 0, SCENARIO_NEGATION,
	    "both statements should load");

	store = NULL;
	if (relic_symbol_intern(ctx, "status", 6, &symbol) == 0 &&
	    relic_pred_intern(ctx, symbol, 2, NULL, &pred) == 0)
		store = relic_base_store(ctx, pred);
	check(store != NULL && store->count == 1, SCENARIO_NEGATION,
	    "a fact with `not` as an argument should be stored");
	if (store == NULL || store->count != 1) {
		relic_destroy(ctx);
		return;
	}

	check(relic_value_decode(ctx,
	    relic_relation_tuple(store, RELIC_TUPLE_FIRST)[1], &value) &&
	    value.type == RELIC_SYMBOL, SCENARIO_NEGATION,
	    "`not` as a term should be a symbol");

	statement = statement_at(ctx, 0);
	check(statement != NULL && statement->body_length == 2 &&
	    statement->body[1].kind == RELIC_LITERAL_NEGATED,
	    SCENARIO_NEGATION,
	    "`not` at the start of a literal should negate it");

	relic_destroy(ctx);
}

/*
 * Recovery discards to the next statement terminator and reports once, so
 * the number of messages is the number of broken statements rather than the
 * number of confused tokens.
 */
static void
one_broken_statement_costs_one_statement(void)
{
	static const char program[] =
	    "good(a).\n"
	    "broken(a b c d).\n"
	    "also$broken(a).\n"
	    "fine(b).\n";

	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_RECOVERY, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "recover.dl", program) != 0,
	    SCENARIO_RECOVERY, "a program with mistakes should fail to load");
	check(log.errors == 2, SCENARIO_RECOVERY,
	    "the two broken statements should produce two messages");

	relic_destroy(ctx);
}

/*
 * The arity limit is structural: an index key is a 32-bit column bitmask, so
 * a 33rd column could not be named by one.  It is enforced where the
 * argument is written, which is also what lets an atom's terms be collected
 * without allocating.
 */
static void
arity_is_limited_where_the_argument_is_written(void)
{
	char program[512];
	diag_log_t log;
	relic_t *ctx;
	size_t used;
	int column;

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_LIMIT, "a context should be created");
	if (ctx == NULL)
		return;

	/* Exactly the limit is accepted; one more is not. */
	used = strlcpy(program, "wide(a", sizeof(program));
	for (column = 1; column < RELIC_MAX_ARITY; column++)
		used = strlcat(program, ", a", sizeof(program));
	used = strlcat(program, ").\n", sizeof(program));
	check(used < sizeof(program), SCENARIO_LIMIT,
	    "the generated program should fit its buffer");

	check(relic_load_string(ctx, "wide.dl", program) == 0, SCENARIO_LIMIT,
	    "an atom of the greatest permitted arity should load");

	relic_destroy(ctx);

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_LIMIT, "a second context should be created");
	if (ctx == NULL)
		return;

	used = strlcpy(program, "wide(a", sizeof(program));
	for (column = 1; column <= RELIC_MAX_ARITY; column++)
		used = strlcat(program, ", a", sizeof(program));
	used = strlcat(program, ").\n", sizeof(program));
	check(used < sizeof(program), SCENARIO_LIMIT,
	    "the generated program should fit its buffer");

	check(relic_load_string(ctx, "wide.dl", program) != 0, SCENARIO_LIMIT,
	    "one argument beyond the limit should be rejected");
	check(log.errors == 1 && strstr(log.first, "more than") != NULL,
	    SCENARIO_LIMIT, "the message should name the limit that was passed");

	relic_destroy(ctx);
}

/*
 * A load either adds every statement in its input or none of them.  A
 * program that is missing a rule is not a smaller program; it is a different
 * one, and a caller that checked the return value would have no way to
 * discover which statements survived.
 */
static void
a_failed_load_adds_nothing(void)
{
	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_ATOMIC, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "first.dl",
	    "kept(a).\nfollows(X) :- kept(X).\n") == 0,
	    SCENARIO_ATOMIC, "the first load should succeed");
	check(statement_count(ctx) == 1 && facts_held(ctx, "kept", 1) == 1,
	    SCENARIO_ATOMIC, "its rule and its fact should be in the program");

	check(relic_load_string(ctx, "second.dl",
	    "kept(b).\naccepted(a).\nlater(X) :- kept(X).\n"
	    "rejected(a b).\n") != 0, SCENARIO_ATOMIC,
	    "a load with a mistake in it should fail");
	check(statement_count(ctx) == 1 && facts_held(ctx, "kept", 1) == 1 &&
	    facts_held(ctx, "accepted", 1) == 0, SCENARIO_ATOMIC,
	    "no statement and no fact of a failed load should be in the "
	    "program");

	relic_destroy(ctx);
}

static void
loads_append_to_one_program(void)
{
	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_APPEND, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "facts.dl", "edge(a, b).\n") == 0,
	    SCENARIO_APPEND, "the facts should load");
	check(relic_load_string(ctx, "rules.dl",
	    "path(X, Y) :- edge(X, Y).\n") == 0, SCENARIO_APPEND,
	    "the rules should load");
	check(statement_count(ctx) == 1 && facts_held(ctx, "edge", 2) == 1,
	    SCENARIO_APPEND, "the program should hold the rule and the fact");

	/* An empty input is a program of no statements, not a failure. */
	check(relic_load_string(ctx, "empty.dl", "") == 0, SCENARIO_APPEND,
	    "an empty input should load");
	check(relic_load_string(ctx, "comment.dl", "% nothing here") == 0,
	    SCENARIO_APPEND, "an input of only comments should load");
	check(statement_count(ctx) == 1 && facts_held(ctx, "edge", 2) == 1,
	    SCENARIO_APPEND, "an input with no statements should add none");

	relic_destroy(ctx);
}

int
main(void)
{
	statements_keep_the_shape_they_were_written_in();
	terms_hold_the_values_they_spell();
	not_is_a_keyword_only_where_a_literal_begins();
	one_broken_statement_costs_one_statement();
	arity_is_limited_where_the_argument_is_written();
	a_failed_load_adds_nothing();
	loads_append_to_one_program();

	return (check_summary("parse"));
}
