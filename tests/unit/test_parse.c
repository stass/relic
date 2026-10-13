/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_AGGREGATE	"an aggregate is the literal it was written as"
#define	SCENARIO_AGG_ERROR	"an aggregate's mistake is reported at its byte"
#define	SCENARIO_AGG_NAMES	"a function name is one only before ( or {"
#define	SCENARIO_CALL		"a call is the tree its arguments give it"
#define	SCENARIO_CALL_DEPTH	"a call counts toward both nesting bounds"
#define	SCENARIO_CALL_ERROR	"a call's mistake is reported where written"
#define	SCENARIO_CLOEXEC	"a file opened to read is closed on exec"
#define	SCENARIO_APPEND		"loads append to one program"
#define	SCENARIO_ATOM_OR_CALL	"what follows `name(...)` makes it an atom or a call"
#define	SCENARIO_ATOMIC		"a failed load changes nothing"
#define	SCENARIO_DEPTH		"an expression's nesting is bounded both ways"
#define	SCENARIO_EXPRESSION	"an expression is the tree its precedence builds"
#define	SCENARIO_EXPR_ERROR	"an expression's mistake is reported where written"
#define	SCENARIO_INPUT		"a load's text is held to the input limit"
#define	SCENARIO_LIMIT		"a structural limit is enforced where written"
#define	SCENARIO_MOD		"`mod` is an operator only after an operand"
#define	SCENARIO_NEGATION	"`not` is a keyword only where a literal begins"
#define	SCENARIO_NEGATIVE	"a negative literal is one term wherever written"
#define	SCENARIO_RECOVERY	"one broken statement costs one statement"
#define	SCENARIO_SHAPE		"a statement is the shape it was written in"
#define	SCENARIO_TERMS		"a term holds the value it spells"

typedef struct diag_log diag_log_t;
typedef struct failing_read failing_read_t;

/*
 * Everything the context reported during a load, so that a test can count
 * the messages without a callback that writes to stderr.  Only the count and
 * the first message and its column are kept; a test that wants the exact
 * text is a golden language test rather than this.
 */
struct diag_log {
	size_t		errors;
	uint32_t	first_column;
	char		first[256];
};

/*
 * A stream's reads: the first fails, and the later ones serve `text`, from
 * `offset` on.
 */
struct failing_read {
	const char	*text;
	size_t		 offset;
	size_t		 calls;
};

static relic_ast_stmt_t *statement_at(relic_t *ctx, size_t index);
static size_t	 statement_count(const relic_t *ctx);
static void	 a_failed_load_adds_nothing(void);
static void	 append_repeated(char *out, size_t size, size_t *usedp,
		    const char *text, size_t count);
static void	 arity_is_limited_where_the_argument_is_written(void);
static void	 expression_mistakes_are_reported_where_written(void);
static void	 expressions_take_the_shape_their_precedence_gives(void);
static bool	 loads_alone(const char *program, diag_log_t *log);
static void	 loads_append_to_one_program(void);
static void	 mod_is_an_operator_only_after_an_operand(void);
static void	 negative_literals_are_terms_wherever_written(void);
static bool	 nested_loads(const char *open, size_t count,
		    const char *middle, const char *close, diag_log_t *log);
static void	 nesting_is_bounded_in_the_text_and_in_the_tree(void);
static void	 not_is_a_keyword_only_where_a_literal_begins(void);
static void	 one_broken_statement_costs_one_statement(void);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static bool	 refused_for_nesting(const diag_log_t *log, uint32_t column);
static void	 render_comparison(relic_t *ctx,
		    const relic_ast_comparison_t *comparison, char *out,
		    size_t size);
static bool	 constant_spells(const relic_constant_t *constant,
		    enum relic_type type, const char *bytes);
static void	 render_constant(const relic_constant_t *constant, char *out,
		    size_t size);
static void	 render_expr(relic_t *ctx, const relic_ast_expr_t *expr,
		    char *out, size_t size);
static relic_t	*context_logging_to(diag_log_t *log);
static relic_t	*context_configured(const relic_config_t *config,
		    diag_log_t *log);
static void	 loads_are_held_to_the_input_limit(void);
static ssize_t	 fail_then_read(failing_read_t *reading, char *buffer,
		    size_t size);
static FILE	*failing_stream(failing_read_t *reading);
static size_t	 facts_held(relic_t *ctx, const char *name, uint16_t arity);
static void	 statements_keep_the_shape_they_were_written_in(void);
static void	 terms_hold_the_values_they_spell(void);
static void	 aggregates_are_the_literals_they_were_written_as(void);
static void	 an_opened_input_is_closed_on_exec(void);
static void	 function_names_are_names_elsewhere(void);
static void	 aggregate_mistakes_are_reported_where_written(void);
static void	 atoms_and_calls_are_told_apart_by_what_follows(void);
static bool	 body_literal_is(relic_t *ctx, size_t statement_index,
		    size_t literal_index, enum relic_ast_literal_kind kind,
		    const char *name);
static void	 call_mistakes_are_reported_where_written(void);
static void	 calls_count_toward_both_nesting_bounds(void);
static void	 calls_take_the_shape_their_arguments_give(void);
static bool	 nested_call_loads(const char *prefix, const char *open,
		    size_t count, const char *middle, const char *close,
		    const char *suffix, diag_log_t *log);

static void
record_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	diag_log_t *log;

	if (level != RELIC_ERROR)
		return;

	log = callback_ctx;
	if (log->errors == 0) {
		strlcpy(log->first, message, sizeof(log->first));
		log->first_column = where != NULL ? where->column : 0;
	}
	log->errors++;
}

static relic_t *
context_logging_to(diag_log_t *log)
{
	return (context_configured(NULL, log));
}

static relic_t *
context_configured(const relic_config_t *config, diag_log_t *log)
{
	relic_t *ctx;

	bzero(log, sizeof(*log));

	ctx = relic_create_config(config);
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

	if (relic_pred_intern(ctx, name, strlen(name), arity, NULL,
	    &pred) != 0)
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
 * A constant is held as the value it spells, of the type its token says,
 * and nothing is interned by reading it: a statement the tree keeps adds no
 * value to the context.  A variable keeps its name, and a bare `_` is
 * marked anonymous because every occurrence of one is a distinct variable
 * however identical the names are.
 */
static void
terms_hold_the_values_they_spell(void)
{
	static const char program[] = "f(a, \"a\", 120, X, _, _Kept, a).\n";

	relic_ast_stmt_t *statement;
	diag_log_t log;
	relic_t *ctx;
	size_t values;

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_TERMS, "a context should be created");
	if (ctx == NULL)
		return;

	values = relic_value_count(ctx);
	check(relic_load_string(ctx, "terms.dl", program) == 0, SCENARIO_TERMS,
	    "the program should load");
	checkf(relic_value_count(ctx) == values && relic_symbol_count(ctx) == 0,
	    SCENARIO_TERMS, "reading the statement should intern nothing, not "
	    "%zu values and %zu symbols", relic_value_count(ctx) - values,
	    relic_symbol_count(ctx));

	statement = statement_at(ctx, 0);
	check(statement != NULL && statement->head.arity == 7, SCENARIO_TERMS,
	    "seven arguments should have been stored");
	if (statement == NULL || statement->head.arity != 7) {
		relic_destroy(ctx);
		return;
	}

	check(statement->head.terms[0].kind == RELIC_TERM_CONSTANT &&
	    constant_spells(&statement->head.terms[0].u.constant,
	    RELIC_SYMBOL, "a"), SCENARIO_TERMS,
	    "a bare identifier should be a symbol");
	check(statement->head.terms[1].kind == RELIC_TERM_CONSTANT &&
	    constant_spells(&statement->head.terms[1].u.constant,
	    RELIC_STRING, "a"), SCENARIO_TERMS,
	    "a quoted literal should be a string");
	check(statement->head.terms[2].kind == RELIC_TERM_CONSTANT &&
	    statement->head.terms[2].u.constant.type == RELIC_INTEGER &&
	    statement->head.terms[2].u.constant.integer == 120,
	    SCENARIO_TERMS, "an integer literal should be its value");

	check(statement->head.terms[3].kind == RELIC_TERM_VARIABLE &&
	    !statement->head.terms[3].u.variable.anonymous, SCENARIO_TERMS,
	    "X should be an ordinary named variable");
	check(statement->head.terms[4].kind == RELIC_TERM_VARIABLE &&
	    statement->head.terms[4].u.variable.anonymous, SCENARIO_TERMS,
	    "a bare _ should be marked anonymous");
	check(statement->head.terms[5].kind == RELIC_TERM_VARIABLE &&
	    !statement->head.terms[5].u.variable.anonymous, SCENARIO_TERMS,
	    "_Kept is a named variable rather than an anonymous one");

	check(constant_spells(&statement->head.terms[6].u.constant,
	    RELIC_SYMBOL, "a"), SCENARIO_TERMS,
	    "one symbol written twice should be the same symbol");

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
	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_NEGATION, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "not.dl", program) == 0, SCENARIO_NEGATION,
	    "both statements should load");

	store = NULL;
	if (relic_pred_intern(ctx, "status", 6, 2, NULL, &pred) == 0)
		store = relic_base_store(ctx, pred);
	check(store != NULL && relic_relation_count(store) == 1,
	    SCENARIO_NEGATION,
	    "a fact with `not` as an argument should be stored");
	if (store == NULL || relic_relation_count(store) != 1) {
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

/*
 * Append `count` copies of `text` to the program being built in `out`, whose
 * first *usedp bytes are already written.  A program that would not fit is
 * truncated and marked by *usedp reaching `size`, which the caller checks
 * once when the program is complete.
 */
static void
append_repeated(char *out, size_t size, size_t *usedp, const char *text,
    size_t count)
{
	size_t length;

	length = strlen(text);
	while (count-- > 0) {
		if (*usedp + length >= size) {
			*usedp = size;
			return;
		}
		bcopy(text, out + *usedp, length);
		*usedp += length;
		out[*usedp] = '\0';
	}
}

/*
 * Load `program` into a context of its own, recording what it reports in
 * `log`, and say whether it loaded.
 */
static bool
loads_alone(const char *program, diag_log_t *log)
{
	relic_t *ctx;
	bool loaded;

	ctx = context_logging_to(log);
	if (ctx == NULL)
		return (false);

	loaded = relic_load_string(ctx, "alone.dl", program) == 0;
	relic_destroy(ctx);

	return (loaded);
}

/*
 * Append the text of one value to `out`: an integer in decimal, a symbol as
 * its name and a string in quotes.
 */
static void
render_constant(const relic_constant_t *constant, char *out, size_t size)
{
	char text[64];

	switch (constant->type) {
	case RELIC_INTEGER:
		snprintf(text, sizeof(text), "%" PRId64, constant->integer);
		strlcat(out, text, size);
		return;
	case RELIC_STRING:
		strlcat(out, "\"", size);
		strlcat(out, constant->bytes, size);
		strlcat(out, "\"", size);
		return;
	case RELIC_SYMBOL:
		strlcat(out, constant->bytes, size);
		return;
	}
}

/*
 * Whether a constant the tree keeps is a symbol or a string of the bytes of
 * the C string `bytes`.
 */
static bool
constant_spells(const relic_constant_t *constant, enum relic_type type,
    const char *bytes)
{
	return (constant->type == type && constant->length == strlen(bytes) &&
	    bcmp(constant->bytes, bytes, constant->length) == 0);
}

/*
 * Append an expression tree to `out` in a prefix form that shows its shape
 * with nothing left to precedence: an operator node as `(op left right)`, a
 * negation as `(neg operand)`, a call as `(name arg ...)`, and a leaf as the
 * constant or variable it holds.  Two trees render alike exactly when they have the same shape and
 * the same leaves.
 */
static void
render_expr(relic_t *ctx, const relic_ast_expr_t *expr, char *out,
    size_t size)
{
	static const char *const operators[] = {
		[RELIC_ARITH_ADD] = "+",
		[RELIC_ARITH_SUBTRACT] = "-",
		[RELIC_ARITH_MULTIPLY] = "*",
		[RELIC_ARITH_DIVIDE] = "/",
		[RELIC_ARITH_REMAINDER] = "mod"
	};
	const relic_ast_term_t *term;
	size_t i;

	switch (expr->kind) {
	case RELIC_AST_EXPR_TERM:
		term = &expr->u.term;
		if (term->kind == RELIC_TERM_VARIABLE)
			strlcat(out, term->u.variable.name.bytes, size);
		else
			render_constant(&term->u.constant, out, size);
		return;
	case RELIC_AST_EXPR_NEGATE:
		strlcat(out, "(neg ", size);
		render_expr(ctx, expr->u.operand, out, size);
		strlcat(out, ")", size);
		return;
	case RELIC_AST_EXPR_BINARY:
		strlcat(out, "(", size);
		strlcat(out, operators[expr->u.binary.op], size);
		strlcat(out, " ", size);
		render_expr(ctx, expr->u.binary.left, out, size);
		strlcat(out, " ", size);
		render_expr(ctx, expr->u.binary.right, out, size);
		strlcat(out, ")", size);
		return;
	case RELIC_AST_EXPR_CALL:
		strlcat(out, "(", size);
		strlcat(out, expr->u.call.name.bytes, size);
		for (i = 0; i < expr->u.call.argc &&
		    i < RELIC_MAX_FUNCTION_ARITY; i++) {
			strlcat(out, " ", size);
			render_expr(ctx, expr->u.call.args[i], out, size);
		}
		strlcat(out, ")", size);
		return;
	}
}

/*
 * A comparison in the prefix form of render_expr(): `(op left right)`.
 */
static void
render_comparison(relic_t *ctx, const relic_ast_comparison_t *comparison,
    char *out, size_t size)
{
	static const char *const comparators[] = {
		[RELIC_CMP_EQ] = "=",
		[RELIC_CMP_NE] = "!=",
		[RELIC_CMP_LT] = "<",
		[RELIC_CMP_LE] = "<=",
		[RELIC_CMP_GT] = ">",
		[RELIC_CMP_GE] = ">="
	};

	out[0] = '\0';
	strlcat(out, "(", size);
	strlcat(out, comparators[comparison->op], size);
	strlcat(out, " ", size);
	render_expr(ctx, comparison->left, out, size);
	strlcat(out, " ", size);
	render_expr(ctx, comparison->right, out, size);
	strlcat(out, ")", size);
}

/*
 * Every operator and every level of precedence, against the tree each must
 * build.  Binary operators associate to the left, `*`, `/` and `mod` bind
 * tighter than `+` and `-`, and unary minus tighter than any.  A `-` directly
 * before digits is folded into a negative constant, with or without space,
 * and is a negation before anything else, a parenthesis included, so the
 * fold never changes which value an expression has, only how the tree
 * holds it.
 */
static void
expressions_take_the_shape_their_precedence_gives(void)
{
	static const struct {
		const char	*comparison;
		const char	*tree;
	} cases[] = {
		{ "X < 1 + 2 * 3",		"(< X (+ 1 (* 2 3)))" },
		{ "X < (1 + 2) * 3",		"(< X (* (+ 1 2) 3))" },
		{ "X < 1 - 2 - 3",		"(< X (- (- 1 2) 3))" },
		{ "X < 1 - (2 - 3)",		"(< X (- 1 (- 2 3)))" },
		{ "X < 8 / 4 / 2",		"(< X (/ (/ 8 4) 2))" },
		{ "X < 7 mod 3 * 2",		"(< X (* (mod 7 3) 2))" },
		{ "X < Y + Z mod 2",		"(< X (+ Y (mod Z 2)))" },
		{ "X < Y - Z / 2 + 1",		"(< X (+ (- Y (/ Z 2)) 1))" },
		{ "X < -Y * 2",			"(< X (* (neg Y) 2))" },
		{ "X < Y * -Z",			"(< X (* Y (neg Z)))" },
		{ "X < -(Y + 1)",		"(< X (neg (+ Y 1)))" },
		{ "X < -1",			"(< X -1)" },
		{ "X < - 1",			"(< X -1)" },
		{ "X < -(1)",			"(< X (neg 1))" },
		{ "X < - -1",			"(< X (neg -1))" },
		{ "X < Y - -1",			"(< X (- Y -1))" },
		{ "X < Y-1",			"(< X (- Y 1))" },
		{ "X < 2-1",			"(< X (- 2 1))" },
		{ "X < ((Y))",			"(< X Y)" },
		{ "X < -9223372036854775808",	"(< X -9223372036854775808)" },
		{ "X < Y - -9223372036854775808",
		    "(< X (- Y -9223372036854775808))" },
		{ "X < 9223372036854775807 + Y",
		    "(< X (+ 9223372036854775807 Y))" },
		{ "Y mod 2 = X",		"(= (mod Y 2) X)" },
		{ "1 + Y >= Z * 2",		"(>= (+ 1 Y) (* Z 2))" },
		{ "X = Y",			"(= X Y)" },
		{ "X != mod",			"(!= X mod)" },
		{ "a != b",			"(!= a b)" },
		{ "\"left\" < \"right\"",	"(< \"left\" \"right\")" }
	};

	relic_ast_stmt_t *statement;
	char program[256];
	char tree[256];
	diag_log_t log;
	relic_t *ctx;
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		ctx = context_logging_to(&log);
		check(ctx != NULL, SCENARIO_EXPRESSION,
		    "a context should be created");
		if (ctx == NULL)
			return;

		snprintf(program, sizeof(program), "r :- p(X, Y, Z), %s.\n",
		    cases[index].comparison);
		checkf(relic_load_string(ctx, "tree.dl", program) == 0,
		    SCENARIO_EXPRESSION, "`%s` should parse",
		    cases[index].comparison);

		statement = statement_at(ctx, 0);
		if (statement == NULL || statement->body_length != 2 ||
		    statement->body[1].kind != RELIC_LITERAL_COMPARISON) {
			checkf(false, SCENARIO_EXPRESSION,
			    "`%s` should be a comparison",
			    cases[index].comparison);
			relic_destroy(ctx);
			continue;
		}

		render_comparison(ctx, &statement->body[1].u.comparison, tree,
		    sizeof(tree));
		checkf(strcmp(tree, cases[index].tree) == 0,
		    SCENARIO_EXPRESSION, "`%s` should build %s, not %s",
		    cases[index].comparison, cases[index].tree, tree);

		relic_destroy(ctx);
	}
}

/*
 * A `-` and the digits after it are two tokens that the parser joins into
 * one negative literal wherever a term stands: a fact, a head, an argument.
 * So `-1` and `- 1` are one value, and INT64_MIN, whose magnitude is out of
 * range until the sign is known, can be written; one past it cannot.
 */
static void
negative_literals_are_terms_wherever_written(void)
{
	relic_ast_stmt_t *statement;
	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_NEGATIVE, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "negative.dl",
	    "offset(start, -1).\n"
	    "offset(start, - 1).\n"
	    "least(-9223372036854775808).\n"
	    "h(-2) :- p(X, -3).\n") == 0, SCENARIO_NEGATIVE,
	    "negative literals should load wherever a term stands");
	check(facts_held(ctx, "offset", 2) == 1, SCENARIO_NEGATIVE,
	    "-1 and - 1 should be one fact");
	check(facts_held(ctx, "least", 1) == 1, SCENARIO_NEGATIVE,
	    "INT64_MIN should be a writable term");

	statement = statement_at(ctx, 0);
	check(statement != NULL && statement->head.arity == 1 &&
	    statement->head.terms[0].u.constant.type == RELIC_INTEGER &&
	    statement->head.terms[0].u.constant.integer == -2,
	    SCENARIO_NEGATIVE, "a head should hold the negative constant");
	check(statement != NULL && statement->body_length == 1 &&
	    statement->body[0].u.atom.arity == 2 &&
	    statement->body[0].u.atom.terms[1].u.constant.type ==
	    RELIC_INTEGER &&
	    statement->body[0].u.atom.terms[1].u.constant.integer == -3,
	    SCENARIO_NEGATIVE, "an argument should hold the negative constant");

	relic_destroy(ctx);

	check(!loads_alone("f(9223372036854775808).\n", &log) &&
	    log.errors == 1 && strstr(log.first, "out of range") != NULL &&
	    log.first_column == 3, SCENARIO_NEGATIVE,
	    "INT64_MAX + 1 should be out of range at its digits");
	check(!loads_alone("f(-9223372036854775809).\n", &log) &&
	    log.errors == 1 && strstr(log.first, "out of range") != NULL &&
	    log.first_column == 3, SCENARIO_NEGATIVE,
	    "INT64_MIN - 1 should be out of range at its sign");
	check(!loads_alone("f(- a).\n", &log) && log.errors == 1 &&
	    strstr(log.first, "expected an integer literal") != NULL &&
	    log.first_column == 5, SCENARIO_NEGATIVE,
	    "a minus before a symbol should not be a term");
}

/*
 * `mod` is the remainder operator only where an operator may stand, after
 * an operand.  Everywhere else it is the symbol it always was: an argument,
 * a comparison's operand, and a predicate's name.
 */
static void
mod_is_an_operator_only_after_an_operand(void)
{
	relic_ast_stmt_t *statement;
	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_MOD, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "mod.dl",
	    "status(foo, mod).\n"
	    "mod(X) :- p(X), X mod 2 = 0.\n"
	    "r(X) :- mod(X), mod != X.\n") == 0, SCENARIO_MOD,
	    "every use of mod should load");
	check(facts_held(ctx, "status", 2) == 1, SCENARIO_MOD,
	    "mod should be a symbol as an argument");

	statement = statement_at(ctx, 0);
	check(statement != NULL && statement->body_length == 2 &&
	    statement->body[1].kind == RELIC_LITERAL_COMPARISON &&
	    statement->body[1].u.comparison.left->kind ==
	    RELIC_AST_EXPR_BINARY &&
	    statement->body[1].u.comparison.left->u.binary.op ==
	    RELIC_ARITH_REMAINDER, SCENARIO_MOD,
	    "mod after an operand should be the remainder");

	statement = statement_at(ctx, 1);
	check(statement != NULL && statement->body_length == 2 &&
	    statement->body[0].kind == RELIC_LITERAL_POSITIVE &&
	    statement->body[1].kind == RELIC_LITERAL_COMPARISON &&
	    statement->body[1].u.comparison.left->kind == RELIC_AST_EXPR_TERM,
	    SCENARIO_MOD,
	    "mod should name a predicate, and be a comparison's operand");

	relic_destroy(ctx);

	check(!loads_alone("r :- p(X), X < mod mod 2.\n", &log) &&
	    log.errors == 1 &&
	    strstr(log.first, "symbol mod cannot be an operand of mod") !=
	    NULL && log.first_column == 16, SCENARIO_MOD,
	    "the symbol mod should be refused as an operand");
}

/*
 * The mistakes the parser can see in an expression, each reported once, at
 * the byte the reader should look at: an operand whose type its token
 * already shows is not an integer, an operand or a parenthesis missing, and
 * an expression where only a term may stand.  Of several, the first written
 * is the one reported.
 */
static void
expression_mistakes_are_reported_where_written(void)
{
	static const struct {
		const char	*program;
		const char	*message;
		uint32_t	 column;
	} cases[] = {
		{ "r :- p(X), X < foo + 1.\n",
		    "symbol foo cannot be an operand of +", 16 },
		{ "r :- p(X), X < 1 * \"a\".\n",
		    "a string literal cannot be an operand of *", 20 },
		{ "r :- p(X), X < -foo.\n",
		    "symbol foo cannot be an operand of -", 17 },
		{ "r :- p(X), foo - 1 < X.\n",
		    "symbol foo cannot be an operand of -", 12 },
		{ "r :- p(X), X < X + .\n",
		    "expected an operand, found '.'", 20 },
		{ "r :- p(X), X < (X + 1.\n",
		    "expected an operator or ')', found '.'", 22 },
		{ "r :- p(X), X = .\n",
		    "expected an operand, found '.'", 16 },
		{ "r :- p(X), X + 1.\n",
		    "expected a comparison operator, found '.'", 17 },
		{ "r :- p(X), X < 1 2.\n",
		    "expected ',' or '.', found an integer literal", 18 },
		{ "succ(X + 1) :- n(X).\n",
		    "expected ',' or ')', found '+'", 8 },
		{ "p(X) :- q(X, Y + 1).\n",
		    "expected ',' or ')', found '+'", 16 },
		{ "r :- p(X), X < foo + bar * \"a\".\n",
		    "symbol foo cannot be an operand of +", 16 }
	};

	diag_log_t log;
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		checkf(!loads_alone(cases[index].program, &log) &&
		    log.errors == 1 &&
		    strstr(log.first, cases[index].message) != NULL &&
		    log.first_column == cases[index].column,
		    SCENARIO_EXPR_ERROR,
		    "`%.*s` should report once, \"%s\" at column %u; it "
		    "reported %zu, \"%s\" at column %u",
		    (int)strlen(cases[index].program) - 1,
		    cases[index].program, cases[index].message,
		    (unsigned int)cases[index].column, log.errors, log.first,
		    (unsigned int)log.first_column);
	}

	check(!loads_alone(
	    "r :- p(X), X < foo + 1.\n"
	    "s :- p(X), X < (X.\n"
	    "t(X + 1).\n"
	    "fine(a).\n", &log) && log.errors == 3, SCENARIO_EXPR_ERROR,
	    "three broken statements should produce three messages");
}

/*
 * Load `r :- p(X), X < <expression>.` built of `open` repeated `count`
 * times, then `middle`, then `close` repeated `count` times, and report
 * whether it loaded.  The expression begins at column 16.
 */
static bool
nested_loads(const char *open, size_t count, const char *middle,
    const char *close, diag_log_t *log)
{
	char program[4096];
	size_t used;

	used = strlcpy(program, "r :- p(X), X < ", sizeof(program));
	append_repeated(program, sizeof(program), &used, open, count);
	append_repeated(program, sizeof(program), &used, middle, 1);
	append_repeated(program, sizeof(program), &used, close, count);
	append_repeated(program, sizeof(program), &used, ".\n", 1);
	if (used >= sizeof(program)) {
		check(false, SCENARIO_DEPTH,
		    "the generated program should fit its buffer");
		return (false);
	}

	return (loads_alone(program, log));
}

/*
 * Whether a load was refused once for nesting, at `column`.
 */
static bool
refused_for_nesting(const diag_log_t *log, uint32_t column)
{
	return (log->errors == 1 &&
	    strstr(log->first, "nested more than 32 levels deep") != NULL &&
	    log->first_column == column);
}

/*
 * Both of the bounds RELIC_MAX_EXPRESSION_DEPTH sets, met and exceeded by
 * one in every shape that reaches them.  The descent of the text counts the
 * parentheses and unary minuses the parser is inside, and is refused at the
 * 33rd where it opens; the height of the tree counts a leaf as one and each
 * operator or negation as one more than its tallest operand, and is refused
 * at the operator of the 33rd level.  A `-` folded into a literal is part of
 * the leaf, so 32 minuses before digits build a tree 32 high and 32 before a
 * variable one 33 high.
 */
static void
nesting_is_bounded_in_the_text_and_in_the_tree(void)
{
	char *deep;
	diag_log_t log;
	size_t count;
	size_t size;
	size_t used;

	check(nested_loads("(", 32, "X", ")", &log), SCENARIO_DEPTH,
	    "32 parentheses around a variable should parse");
	check(!nested_loads("(", 33, "X", ")", &log) &&
	    refused_for_nesting(&log, 16 + 32), SCENARIO_DEPTH,
	    "the 33rd parenthesis should be refused where it opens");

	check(nested_loads("-", 32, "1", "", &log), SCENARIO_DEPTH,
	    "32 minuses before a literal should build a tree 32 high");
	check(!nested_loads("-", 33, "1", "", &log) &&
	    refused_for_nesting(&log, 16), SCENARIO_DEPTH,
	    "33 minuses before a literal should be refused at the outermost");
	check(nested_loads("-", 31, "X", "", &log), SCENARIO_DEPTH,
	    "31 minuses before a variable should build a tree 32 high");
	check(!nested_loads("-", 32, "X", "", &log) &&
	    refused_for_nesting(&log, 16), SCENARIO_DEPTH,
	    "32 minuses before a variable should be refused for height");
	check(!nested_loads("-", 33, "X", "", &log) &&
	    refused_for_nesting(&log, 16 + 32), SCENARIO_DEPTH,
	    "the 33rd minus should be refused where it opens");

	check(nested_loads("X + ", 31, "X", "", &log), SCENARIO_DEPTH,
	    "a chain of 32 operands should build a tree 32 high");
	check(!nested_loads("X + ", 32, "X", "", &log) &&
	    refused_for_nesting(&log, 16 + 31 * 4 + 2), SCENARIO_DEPTH,
	    "the operator joining the 33rd operand should be refused");

	/*
	 * Each `X + X * (` wraps the expression inside it in two more
	 * levels and one more parenthesis, so fifteen of them around `X + X`
	 * reach 32 and sixteen around `X` reach 33, the outermost `+`
	 * being the operator refused.
	 */
	check(nested_loads("X + X * (", 15, "X + X", ")", &log),
	    SCENARIO_DEPTH, "X + X * (...) should parse at height 32");
	check(!nested_loads("X + X * (", 16, "X", ")", &log) &&
	    refused_for_nesting(&log, 18), SCENARIO_DEPTH,
	    "X + X * (...) should be refused at height 33");

	/*
	 * Malformed input at each depth: no node is ever built, so only the
	 * descent can refuse it, and the message is whichever mistake comes
	 * first.
	 */
	check(!nested_loads("(", 32, "", "", &log) && log.errors == 1 &&
	    strstr(log.first, "expected an operand") != NULL, SCENARIO_DEPTH,
	    "32 unclosed parentheses should report the missing operand");
	check(!nested_loads("(", 33, "", "", &log) &&
	    refused_for_nesting(&log, 16 + 32), SCENARIO_DEPTH,
	    "33 unclosed parentheses should be refused for nesting");
	check(!nested_loads("- ", 32, "", "", &log) && log.errors == 1 &&
	    strstr(log.first, "expected an operand") != NULL, SCENARIO_DEPTH,
	    "32 dangling minuses should report the missing operand");
	check(!nested_loads("- ", 33, "", "", &log) &&
	    refused_for_nesting(&log, 16 + 64), SCENARIO_DEPTH,
	    "33 dangling minuses should be refused for nesting");

	/*
	 * A hundred thousand parentheses must cost one message and bounded
	 * recursion, however they are closed or not.
	 */
	count = 100000;
	size = count + 64;
	deep = malloc(size);
	check(deep != NULL, SCENARIO_DEPTH, "the deep program should fit");
	if (deep == NULL)
		return;

	used = strlcpy(deep, "r :- p(X), X < ", size);
	memset(deep + used, '(', count);
	used += count;
	strlcpy(deep + used, "X.\n", size - used);
	check(!loads_alone(deep, &log) && refused_for_nesting(&log, 16 + 32),
	    SCENARIO_DEPTH,
	    "a hundred thousand parentheses should be refused once");

	free(deep);
}

/*
 * Each function, with and without its variable, becomes an aggregate
 * literal holding its result, its column and its body; a body holds
 * positive and negated literals, comparisons, assignments and `_`.
 */
static void
aggregates_are_the_literals_they_were_written_as(void)
{
	static const struct {
		const char		*text;
		enum relic_aggregate_fn	 fn;
		size_t			 body_length;
	} rules[] = {
		{ "r(N) :- N = count { p(X) }.\n", RELIC_AGG_COUNT, 1 },
		{ "r(S) :- S = sum(V) { p(V), not q(V), V > 1 }.\n",
		    RELIC_AGG_SUM, 3 },
		{ "r(M) :- M = min(W) { p(V), W = V * 10 }.\n", RELIC_AGG_MIN,
		    2 },
		{ "r(M) :- M = max(V) { p(V, _) }.\n", RELIC_AGG_MAX, 1 }
	};
	const relic_ast_aggregate_t *aggregate;
	relic_ast_stmt_t *statement;
	diag_log_t log;
	relic_t *ctx;
	size_t i;

	for (i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
		ctx = context_logging_to(&log);
		if (ctx == NULL)
			return;

		statement = NULL;
		if (relic_load_string(ctx, "agg.dl", rules[i].text) == 0)
			statement = statement_at(ctx, 0);
		if (statement == NULL || statement->body_length != 1 ||
		    statement->body[0].kind != RELIC_LITERAL_AGGREGATE) {
			checkf(false, SCENARIO_AGGREGATE, "%s should be one "
			    "aggregate literal", rules[i].text);
			relic_destroy(ctx);
			continue;
		}

		aggregate = &statement->body[0].u.aggregate;
		checkf(aggregate->fn == rules[i].fn &&
		    aggregate->body_length == rules[i].body_length &&
		    aggregate->result.kind == RELIC_TERM_VARIABLE &&
		    (rules[i].fn == RELIC_AGG_COUNT ||
		    aggregate->column.kind == RELIC_TERM_VARIABLE),
		    SCENARIO_AGGREGATE, "%s should hold its function, its "
		    "variables and a body of %zu literals", rules[i].text,
		    rules[i].body_length);
		checkf(aggregate->where.column == 13, SCENARIO_AGGREGATE,
		    "%s should be located at its function name, not column "
		    "%u", rules[i].text, (unsigned int)aggregate->where.column);

		if (rules[i].fn == RELIC_AGG_SUM)
			check(aggregate->body[1].kind ==
			    RELIC_LITERAL_NEGATED && aggregate->body[2].kind ==
			    RELIC_LITERAL_COMPARISON, SCENARIO_AGGREGATE,
			    "a body holds a negation and a comparison");

		relic_destroy(ctx);
	}
}

/*
 * count, sum, min and max are ordinary names wherever an aggregate is not
 * written: as predicates, as symbol arguments, and as the right side of an
 * equality followed by `,` or `.`.
 */
static void
function_names_are_names_elsewhere(void)
{
	relic_ast_stmt_t *statement;
	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "names.dl",
	    "count(x). sum(y). label(min). label(max).\n"
	    "a(X) :- label(X), X = count, sum(X).\n"
	    "b(X) :- X = max.\n"
	    "c(X) :- min(X), max(X).\n") == 0, SCENARIO_AGG_NAMES,
	    "every use of the names should load");
	check(facts_held(ctx, "count", 1) == 1 &&
	    facts_held(ctx, "label", 1) == 2, SCENARIO_AGG_NAMES,
	    "the names should be predicates and symbol arguments");

	statement = statement_at(ctx, 0);
	check(statement != NULL && statement->body_length == 3 &&
	    statement->body[1].kind == RELIC_LITERAL_COMPARISON &&
	    statement->body[2].kind == RELIC_LITERAL_POSITIVE,
	    SCENARIO_AGG_NAMES, "X = count followed by , is a comparison");

	statement = statement_at(ctx, 1);
	check(statement != NULL && statement->body_length == 1 &&
	    statement->body[0].kind == RELIC_LITERAL_COMPARISON,
	    SCENARIO_AGG_NAMES, "X = max followed by . is a comparison");

	relic_destroy(ctx);
}

/*
 * Every mistake in writing an aggregate is reported once, at the byte it
 * is at, and costs its own statement only.
 */
static void
aggregate_mistakes_are_reported_where_written(void)
{
	static const struct {
		const char	*text;
		const char	*message;
		uint32_t	 column;
	} mistakes[] = {
		{ "r(N) :- N < count { p(X) }.\n",
		    "an aggregate is written V = count { ... }", 13 },
		{ "r(N) :- count { p(X) } = N.\n",
		    "an aggregate is written V = count { ... }", 9 },
		{ "r(N) :- N = count(X) { p(X) }.\n",
		    "count takes no variable", 18 },
		{ "r(N) :- N = sum { p(X) }.\n",
		    "sum takes one variable in parentheses", 17 },
		{ "r(N) :- N = min(3) { p(X) }.\n",
		    "expected a variable, found an integer literal", 17 },
		{ "r(N) :- N = max(_) { p(X) }.\n",
		    "max takes a named variable, not _", 17 },
		{ "r(N) :- N = count { M = count { p(X) } }.\n",
		    "an aggregate body cannot hold an aggregate", 25 },
		{ "r(N) :- N = count { }.\n", "expected a literal, found '}'",
		    21 },
		{ "r(N) :- N = count { p(X) .\n", "expected ',' or '}'", 26 },
		{ "r(N) :- 3 = count { p(X) }.\n",
		    "an aggregate is written V = count { ... }", 13 }
	};
	diag_log_t log;
	size_t i;

	for (i = 0; i < sizeof(mistakes) / sizeof(mistakes[0]); i++) {
		checkf(!loads_alone(mistakes[i].text, &log) &&
		    log.errors == 1 && strstr(log.first,
		    mistakes[i].message) != NULL &&
		    log.first_column == mistakes[i].column,
		    SCENARIO_AGG_ERROR, "%s should be refused once at column "
		    "%u with \"%s\", not %zu time(s) at %u with \"%s\"",
		    mistakes[i].text, (unsigned int)mistakes[i].column,
		    mistakes[i].message, log.errors,
		    (unsigned int)log.first_column, log.first);
	}
}

/*
 * Every function at every arity, a call as an operand of every operator and
 * as an argument of a call, and a call beginning a literal before every
 * comparator and operator, each builds the tree its text gives.
 */
static void
calls_take_the_shape_their_arguments_give(void)
{
	static const struct {
		const char	*comparison;
		const char	*tree;
	} cases[] = {
		{ "X = length(Y)",		"(= X (length Y))" },
		{ "X = cat(Y, Z)",		"(= X (cat Y Z))" },
		{ "X = substr(Y, 1)",		"(= X (substr Y 1))" },
		{ "X = substr(Y, 1, 2)",	"(= X (substr Y 1 2))" },
		{ "X = index(Y, \"a\")",	"(= X (index Y \"a\"))" },
		{ "X = match(Y, \"a*\")",	"(= X (match Y \"a*\"))" },
		{ "X = str(Y)",			"(= X (str Y))" },
		{ "X = str(foo)",		"(= X (str foo))" },
		{ "X = str(-1)",		"(= X (str -1))" },
		{ "X < length(Y) + 1",		"(< X (+ (length Y) 1))" },
		{ "X < 1 - length(Y)",		"(< X (- 1 (length Y)))" },
		{ "X < length(Y) * 2",		"(< X (* (length Y) 2))" },
		{ "X < 2 / length(Y)",		"(< X (/ 2 (length Y)))" },
		{ "X < length(Y) mod 2",	"(< X (mod (length Y) 2))" },
		{ "X < -length(Y)",		"(< X (neg (length Y)))" },
		{ "X < (length(Y))",		"(< X (length Y))" },
		{ "X = cat(cat(Y, Z), str(1 + 2))",
		    "(= X (cat (cat Y Z) (str (+ 1 2))))" },
		{ "X = substr(Y, 0, length(Y) - 2)",
		    "(= X (substr Y 0 (- (length Y) 2)))" },
		{ "length(Y) = X",		"(= (length Y) X)" },
		{ "length(Y) != X",		"(!= (length Y) X)" },
		{ "length(Y) < X",		"(< (length Y) X)" },
		{ "length(Y) <= X",		"(<= (length Y) X)" },
		{ "length(Y) > X",		"(> (length Y) X)" },
		{ "length(Y) >= X",		"(>= (length Y) X)" },
		{ "length(Y) + 1 > X",		"(> (+ (length Y) 1) X)" },
		{ "length(Y) - 1 > X",		"(> (- (length Y) 1) X)" },
		{ "length(Y) * 2 > X",		"(> (* (length Y) 2) X)" },
		{ "length(Y) / 2 > X",		"(> (/ (length Y) 2) X)" },
		{ "length(Y) mod 2 > X",	"(> (mod (length Y) 2) X)" },
		{ "index(Y, cat(Z, \"..\")) >= 0",
		    "(>= (index Y (cat Z \"..\")) 0)" },
		{ "substr(Y, -1, 2) = X",	"(= (substr Y -1 2) X)" }
	};

	relic_ast_stmt_t *statement;
	char program[256];
	char tree[256];
	diag_log_t log;
	relic_t *ctx;
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		ctx = context_logging_to(&log);
		check(ctx != NULL, SCENARIO_CALL,
		    "a context should be created");
		if (ctx == NULL)
			return;

		snprintf(program, sizeof(program), "r :- p(X, Y, Z), %s.\n",
		    cases[index].comparison);
		checkf(relic_load_string(ctx, "call.dl", program) == 0,
		    SCENARIO_CALL, "`%s` should parse, not report \"%s\"",
		    cases[index].comparison, log.errors > 0 ? log.first : "");

		statement = statement_at(ctx, 0);
		if (statement == NULL || statement->body_length != 2 ||
		    statement->body[1].kind != RELIC_LITERAL_COMPARISON) {
			checkf(false, SCENARIO_CALL,
			    "`%s` should be the rule's second literal",
			    cases[index].comparison);
			relic_destroy(ctx);
			continue;
		}

		tree[0] = '\0';
		render_comparison(ctx, &statement->body[1].u.comparison, tree,
		    sizeof(tree));
		checkf(strcmp(tree, cases[index].tree) == 0, SCENARIO_CALL,
		    "`%s` should build %s, and built %s",
		    cases[index].comparison, cases[index].tree, tree);

		relic_destroy(ctx);
	}
}

/*
 * Whether literal `literal_index` of statement `statement_index` is of
 * `kind`, and, for an atom, of the predicate spelled `name`.
 */
static bool
body_literal_is(relic_t *ctx, size_t statement_index, size_t literal_index,
    enum relic_ast_literal_kind kind, const char *name)
{
	const relic_ast_stmt_t *statement;
	const relic_ast_literal_t *literal;

	statement = statement_at(ctx, statement_index);
	if (statement == NULL || literal_index >= statement->body_length)
		return (false);

	literal = &statement->body[literal_index];
	if (literal->kind != kind)
		return (false);
	if (kind != RELIC_LITERAL_POSITIVE)
		return (true);

	return (strcmp(literal->u.atom.name.bytes, name) == 0);
}

/*
 * A literal that begins `name(` is a call when an operator or a comparator
 * follows its `)`, and an atom when anything else does: a `,`, a `.`, a
 * `?`, an aggregate's `}` or a rule's `:-`.  No name is reserved, so the
 * functions' names still name predicates.
 */
static void
atoms_and_calls_are_told_apart_by_what_follows(void)
{
	relic_ast_stmt_t *statement;
	diag_log_t log;
	relic_t *ctx;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	checkf(relic_load_string(ctx, "atoms.dl",
	    "length(a, 4). cat(b).\n"
	    "length(X) :- word(X), length(X, 3).\n"
	    "long(X) :- word(X), length(X) > 3.\n"
	    "short(X) :- word(X), length(X), length(X) < 3.\n"
	    "n(N) :- N = count { length(X) }.\n"
	    "length(X)?\n"
	    "word(X), str(X) = \"a\"?\n") == 0, SCENARIO_ATOM_OR_CALL,
	    "every use of the names should load, not report \"%s\"",
	    log.errors > 0 ? log.first : "");
	check(facts_held(ctx, "length", 2) == 1 && facts_held(ctx, "cat", 1) == 1,
	    SCENARIO_ATOM_OR_CALL, "function names should name facts");

	statement = statement_at(ctx, 0);
	check(statement != NULL && statement->kind == RELIC_STMT_RULE &&
	    strcmp(statement->head.name.bytes, "length") == 0 &&
	    statement->head.arity == 1,
	    SCENARIO_ATOM_OR_CALL, "length(X) :- ... should define length/1");
	check(body_literal_is(ctx, 0, 1, RELIC_LITERAL_POSITIVE, "length"),
	    SCENARIO_ATOM_OR_CALL, "length(X, 3) followed by . is an atom");
	check(body_literal_is(ctx, 1, 1, RELIC_LITERAL_COMPARISON, NULL),
	    SCENARIO_ATOM_OR_CALL, "length(X) > 3 is a comparison");
	check(body_literal_is(ctx, 2, 1, RELIC_LITERAL_POSITIVE, "length") &&
	    body_literal_is(ctx, 2, 2, RELIC_LITERAL_COMPARISON, NULL),
	    SCENARIO_ATOM_OR_CALL,
	    "length(X) followed by , is an atom, and < makes a call");

	statement = statement_at(ctx, 3);
	check(statement != NULL && statement->body_length == 1 &&
	    statement->body[0].kind == RELIC_LITERAL_AGGREGATE &&
	    statement->body[0].u.aggregate.body_length == 1 &&
	    statement->body[0].u.aggregate.body[0].kind ==
	    RELIC_LITERAL_POSITIVE, SCENARIO_ATOM_OR_CALL,
	    "length(X) followed by } is an atom");
	check(body_literal_is(ctx, 4, 0, RELIC_LITERAL_POSITIVE, "length"),
	    SCENARIO_ATOM_OR_CALL, "length(X) followed by ? is an atom");
	check(body_literal_is(ctx, 5, 1, RELIC_LITERAL_COMPARISON, NULL),
	    SCENARIO_ATOM_OR_CALL, "str(X) = \"a\" in a query is a call");

	relic_destroy(ctx);
}

/*
 * Every mistake in writing a call is reported once, at the byte it is at:
 * a name that is no function's, a function at an arity it does not have,
 * an aggregate's name, a constant of the wrong type in each argument
 * position of each function; and an atom, a head or a fact holding an
 * expression or a call, where an atom's parser has always stopped.
 */
static void
call_mistakes_are_reported_where_written(void)
{
	static const struct {
		const char	*text;
		const char	*message;
		uint32_t	 column;
	} mistakes[] = {
		{ "r :- p(X), foo(X) > 1.\n", "foo is not a function", 12 },
		{ "r :- p(X), X = foo(X).\n", "foo is not a function", 16 },
		{ "r :- p(X), X = 1 + bar().\n", "bar is not a function", 20 },
		{ "r :- p(X), X = substr(X, 1, 2, 3).\n",
		    "substr takes 2 or 3 arguments, not 4", 16 },
		{ "r :- p(X), X = substr(X).\n",
		    "substr takes 2 or 3 arguments, not 1", 16 },
		{ "r :- p(X), X = length(X, X).\n",
		    "length takes 1 argument, not 2", 16 },
		{ "r :- p(X), X = length().\n",
		    "length takes 1 argument, not 0", 16 },
		{ "r :- p(X), X = cat(X).\n", "cat takes 2 arguments, not 1",
		    16 },
		{ "r :- p(X), str(X, X) = X.\n", "str takes 1 argument, not 2",
		    12 },
		{ "r :- p(X), X < count(X).\n",
		    "an aggregate is written V = count { ... }", 16 },
		{ "r :- p(X), sum(X) > 1.\n",
		    "an aggregate is written V = count { ... }", 12 },
		{ "r :- p(X), X = length(42).\n",
		    "integer 42 cannot be an argument of length", 23 },
		{ "r :- p(X), X = length(foo).\n",
		    "symbol foo cannot be an argument of length", 23 },
		{ "r :- p(X), X = cat(foo, X).\n",
		    "symbol foo cannot be an argument of cat", 20 },
		{ "r :- p(X), X = cat(X, -1).\n",
		    "integer -1 cannot be an argument of cat", 23 },
		{ "r :- p(X), X = substr(bar, 1).\n",
		    "symbol bar cannot be an argument of substr", 23 },
		{ "r :- p(X), X = substr(X, \"0\").\n",
		    "string \"0\" cannot be an argument of substr", 26 },
		{ "r :- p(X), X = substr(X, 0, x).\n",
		    "symbol x cannot be an argument of substr", 29 },
		{ "r :- p(X), X = index(7, X).\n",
		    "integer 7 cannot be an argument of index", 22 },
		{ "r :- p(X), X = index(X, a).\n",
		    "symbol a cannot be an argument of index", 25 },
		{ "r :- p(X), X = match(a, X).\n",
		    "symbol a cannot be an argument of match", 22 },
		{ "r :- p(X), X = match(X, 1).\n",
		    "integer 1 cannot be an argument of match", 25 },
		{ "r :- p(X), X = cat(foo(X), 1).\n",
		    "foo is not a function", 20 },
		{ "r :- p(X), X = length(\"a\" + 1).\n",
		    "a string literal cannot be an operand of +", 23 },
		{ "r :- p(X), q(X, cat(X, X)).\n",
		    "expected ',' or ')', found '('", 20 },
		{ "r :- p(X), q(X, length(X) + 1).\n",
		    "expected ',' or ')', found '('", 23 },
		{ "r :- p(X), q((X)).\n", "expected a term, found '('", 14 },
		{ "r :- p(X), q(-X).\n",
		    "expected an integer literal, found a variable", 15 },
		{ "r :- p(X), q(X mod 2).\n",
		    "expected ',' or ')', found a symbol", 16 },
		{ "r :- p(X), q(X, \"a\" + 1).\n",
		    "expected ',' or ')', found '+'", 21 },
		{ "object(cat(A, B)) :- source(A, B).\n",
		    "expected ',' or ')', found '('", 11 },
		{ "p(length(\"a\")).\n", "expected ',' or ')', found '('", 9 },
		{ "r :- p(X), not length(X) > 3.\n",
		    "expected ',' or '.', found a comparison operator", 26 },
		{ "r :- p(X), q(X) = 1.\n", "q is not a function", 12 }
	};

	diag_log_t log;
	size_t index;

	for (index = 0; index < sizeof(mistakes) / sizeof(mistakes[0]);
	    index++) {
		check(!loads_alone(mistakes[index].text, &log),
		    SCENARIO_CALL_ERROR, "a mistaken call should not load");
		checkf(log.errors == 1 &&
		    strstr(log.first, mistakes[index].message) != NULL &&
		    log.first_column == mistakes[index].column,
		    SCENARIO_CALL_ERROR,
		    "`%s` should report once, \"%s\" at column %u; it "
		    "reported %zu, \"%s\" at column %u",
		    mistakes[index].text, mistakes[index].message,
		    (unsigned int)mistakes[index].column, log.errors,
		    log.first, (unsigned int)log.first_column);
	}
}

/*
 * Load a rule whose body is `prefix`, `count` copies of `open`, `middle`,
 * `count` copies of `close`, and `suffix`, into a context of its own.
 */
static bool
nested_call_loads(const char *prefix, const char *open, size_t count,
    const char *middle, const char *close, const char *suffix,
    diag_log_t *log)
{
	char program[4096];
	size_t used;

	used = strlcpy(program, "r :- p(X), ", sizeof(program));
	append_repeated(program, sizeof(program), &used, prefix, 1);
	append_repeated(program, sizeof(program), &used, open, count);
	append_repeated(program, sizeof(program), &used, middle, 1);
	append_repeated(program, sizeof(program), &used, close, count);
	append_repeated(program, sizeof(program), &used, suffix, 1);
	append_repeated(program, sizeof(program), &used, ".\n", 1);
	if (used >= sizeof(program)) {
		check(false, SCENARIO_CALL_DEPTH,
		    "the generated program should fit its buffer");
		return (false);
	}

	return (loads_alone(program, log));
}

/*
 * A call's `(` counts one level of the text's descent and a call node one
 * level of the tree's height, so thirty-one calls around a leaf build a
 * tree 32 high, thirty-two are refused at the outermost call's name for
 * height, and thirty-three at the 33rd `(` for descent; in operand position
 * and at the start of a literal alike, and in each argument position of
 * substr.  "r :- p(X), " is eleven bytes, and "X < " four more.
 */
static void
calls_count_toward_both_nesting_bounds(void)
{
	static const struct {
		const char	*open;
		const char	*middle;
		const char	*close;
	} shapes[] = {
		{ "str(", "X", ")" },
		{ "cat(", "X", ", X)" },
		{ "cat(X, ", "X", ")" },
		{ "substr(", "X", ", 1, 2)" },
		{ "substr(X, ", "1", ", 2)" },
		{ "substr(X, 1, ", "2", ")" }
	};
	diag_log_t log;
	size_t length;
	size_t index;

	for (index = 0; index < sizeof(shapes) / sizeof(shapes[0]); index++) {
		length = strlen(shapes[index].open);

		checkf(nested_call_loads("X < ", shapes[index].open, 31,
		    shapes[index].middle, shapes[index].close, "", &log),
		    SCENARIO_CALL_DEPTH, "31 nested `%s` should parse",
		    shapes[index].open);
		checkf(!nested_call_loads("X < ", shapes[index].open, 32,
		    shapes[index].middle, shapes[index].close, "", &log) &&
		    refused_for_nesting(&log, 16), SCENARIO_CALL_DEPTH,
		    "32 nested `%s` should be refused for height",
		    shapes[index].open);
		checkf(!nested_call_loads("X < ", shapes[index].open, 33,
		    shapes[index].middle, shapes[index].close, "", &log) &&
		    refused_for_nesting(&log,
		    (uint32_t)(16 + 32 * length + strcspn(shapes[index].open,
		    "("))), SCENARIO_CALL_DEPTH,
		    "the 33rd `%s` should be refused where its ( opens",
		    shapes[index].open);

		checkf(nested_call_loads("", shapes[index].open, 31,
		    shapes[index].middle, shapes[index].close, " = X", &log),
		    SCENARIO_CALL_DEPTH,
		    "31 nested `%s` beginning a literal should parse",
		    shapes[index].open);
		checkf(!nested_call_loads("", shapes[index].open, 32,
		    shapes[index].middle, shapes[index].close, " = X", &log) &&
		    refused_for_nesting(&log, 12), SCENARIO_CALL_DEPTH,
		    "32 nested `%s` beginning a literal should be refused for "
		    "height", shapes[index].open);
		checkf(!nested_call_loads("", shapes[index].open, 33,
		    shapes[index].middle, shapes[index].close, " = X", &log) &&
		    refused_for_nesting(&log,
		    (uint32_t)(12 + 32 * length + strcspn(shapes[index].open,
		    "("))), SCENARIO_CALL_DEPTH,
		    "the 33rd `%s` beginning a literal should be refused where "
		    "its ( opens", shapes[index].open);
	}
}

/*
 * A file a load or an import opens is not inherited by a child an embedder
 * starts while it is open, and one that cannot be opened is reported.
 */
static void
an_opened_input_is_closed_on_exec(void)
{
	diag_log_t log;
	FILE *stream;
	relic_t *ctx;
	int descriptor_flags;

	ctx = context_logging_to(&log);
	check(ctx != NULL, SCENARIO_CLOEXEC, "a context should be created");
	if (ctx == NULL)
		return;

	stream = relic_open_input(ctx, "/dev/null");
	check(stream != NULL, SCENARIO_CLOEXEC, "/dev/null should open");
	if (stream != NULL) {
		descriptor_flags = fcntl(fileno(stream), F_GETFD);
		check(descriptor_flags != -1 &&
		    (descriptor_flags & FD_CLOEXEC) != 0, SCENARIO_CLOEXEC,
		    "its descriptor should be closed on exec");
		fclose(stream);
	}

	stream = relic_open_input(ctx, "/nonexistent/input.dl");
	check(stream == NULL && log.errors == 1 &&
	    strstr(log.first, "cannot open /nonexistent/input.dl") != NULL,
	    SCENARIO_CLOEXEC, "a missing file should be reported");
	if (stream != NULL)
		fclose(stream);

	relic_destroy(ctx);
}

/*
 * Fail the first read, then serve the text.
 */
static ssize_t
fail_then_read(failing_read_t *reading, char *buffer, size_t size)
{
	size_t remaining;

	reading->calls++;
	if (reading->calls == 1) {
		errno = EIO;
		return (-1);
	}

	remaining = strlen(reading->text) - reading->offset;
	if (size > remaining)
		size = remaining;
	bcopy(reading->text + reading->offset, buffer, size);
	reading->offset += size;

	return ((ssize_t)size);
}

#if defined(HAVE_FUNOPEN)
static int
funopen_read(void *cookie, char *buffer, int size)
{
	return ((int)fail_then_read(cookie, buffer, (size_t)size));
}

/* A stream whose reads run fail_then_read(), or NULL. */
static FILE *
failing_stream(failing_read_t *reading)
{
	return (funopen(reading, funopen_read, NULL, NULL, NULL));
}
#elif defined(HAVE_FOPENCOOKIE)
static ssize_t
cookie_read(void *cookie, char *buffer, size_t size)
{
	return (fail_then_read(cookie, buffer, size));
}

/* A stream whose reads run fail_then_read(), or NULL. */
static FILE *
failing_stream(failing_read_t *reading)
{
	cookie_io_functions_t functions = { cookie_read, NULL, NULL, NULL };

	return (fopencookie(reading, "r", functions));
}
#else
/* No way to make a stream run caller code on this platform. */
static FILE *
failing_stream(failing_read_t *reading RELIC_TEST_UNUSED)
{
	return (NULL);
}
#endif

/*
 * A load whose text is longer than the input limit is refused with nothing
 * of it added, whether a string, a stream or a file gives it, and a text at
 * the limit loads.  A stream is read no further than one byte past the
 * limit, and one whose read had already failed is refused for the failure,
 * whatever it serves after.  A query's text is held to no input limit, and a
 * limit of zero or SIZE_MAX sets none.
 */
static void
loads_are_held_to_the_input_limit(void)
{
	static const char fits[] = "edge(a, b).\n";
	static const char longer[] = "edge(b, c). edge(c, d).\n";
	static const size_t unlimited[] = { 0, SIZE_MAX };
	static const char refused[] = "limit exceeded: input=12 while loading ";
	char path[] = "/tmp/relic-input-XXXXXX";
	failing_read_t reading;
	relic_config_t config;
	relic_query_t *query;
	diag_log_t log;
	FILE *stream;
	relic_t *ctx;
	size_t i;
	long read;
	int descriptor;
	int status;

	bzero(&config, sizeof(config));
	config.max_input = sizeof(fits) - 1;
	ctx = context_configured(&config, &log);
	check(ctx != NULL, SCENARIO_INPUT, "a context should be created");
	if (ctx == NULL)
		return;

	status = relic_load_string(ctx, "long.dl", longer);
	check(status != 0 &&
	    strncmp(log.first, refused, sizeof(refused) - 1) == 0 &&
	    strcmp(log.first + sizeof(refused) - 1, "long.dl") == 0,
	    SCENARIO_INPUT, "a text past the limit should be refused as the "
	    "input limit");
	check(statement_count(ctx) == 0 && facts_held(ctx, "edge", 2) == 0,
	    SCENARIO_INPUT, "a refused text should add nothing");

	status = relic_load_string(ctx, "fits.dl", fits);
	check(status == 0, SCENARIO_INPUT, "a text at the limit should load");
	check(facts_held(ctx, "edge", 2) == 1, SCENARIO_INPUT,
	    "the fact of a text at the limit should be held");

	stream = tmpfile();
	check(stream != NULL, SCENARIO_INPUT, "a stream should be made");
	if (stream != NULL) {
		for (i = 0; i < 4096; i++)
			fputs(longer, stream);
		rewind(stream);
		log.errors = 0;
		status = relic_load_file(ctx, "stream.dl", stream);
		check(status != 0 && log.errors == 1 &&
		    facts_held(ctx, "edge", 2) == 1, SCENARIO_INPUT,
		    "a stream past the limit should be refused");
		read = ftell(stream);
		checkf(read == (long)sizeof(fits), SCENARIO_INPUT,
		    "a stream should be read one byte past the limit, %zu "
		    "bytes, not %ld", sizeof(fits), read);
		fclose(stream);
	}

	bzero(&reading, sizeof(reading));
	reading.text = longer;
	stream = failing_stream(&reading);
	if (stream != NULL) {
		check(fgetc(stream) == EOF && ferror(stream), SCENARIO_INPUT,
		    "the stream's first read should fail");
		status = relic_load_file(ctx, "failed.dl", stream);
		check(status != 0 &&
		    relic_error_class(ctx) == RELIC_ERROR_IO, SCENARIO_INPUT,
		    "a stream whose read failed should be refused for the "
		    "failure, not for the input limit");
		fclose(stream);
	}

	descriptor = mkstemp(path);
	check(descriptor != -1, SCENARIO_INPUT, "a file should be made");
	if (descriptor != -1) {
		check(write(descriptor, longer, sizeof(longer) - 1) ==
		    (ssize_t)(sizeof(longer) - 1), SCENARIO_INPUT,
		    "the file should be written");
		close(descriptor);
		log.errors = 0;
		status = relic_load_path(ctx, path);
		check(status != 0 && log.errors == 1 &&
		    strncmp(log.first, refused, sizeof(refused) - 1) == 0 &&
		    facts_held(ctx, "edge", 2) == 1,
		    SCENARIO_INPUT, "a file past the limit should be refused");
		unlink(path);
	}

	query = relic_query_prepare(ctx, NULL, "edge(X, Y), edge(Y, Z)?");
	check(query != NULL, SCENARIO_INPUT,
	    "a query's text should be held to no input limit");
	relic_query_destroy(query);

	relic_destroy(ctx);

	for (i = 0; i < sizeof(unlimited) / sizeof(unlimited[0]); i++) {
		config.max_input = unlimited[i];
		ctx = context_configured(&config, &log);
		check(ctx != NULL, SCENARIO_INPUT,
		    "a context should be created");
		if (ctx == NULL)
			return;
		status = relic_load_string(ctx, "long.dl", longer);
		checkf(status == 0, SCENARIO_INPUT,
		    "an input limit of %zu should hold a text to none",
		    unlimited[i]);
		check(facts_held(ctx, "edge", 2) == 2, SCENARIO_INPUT,
		    "the facts of the long text should be held");
		relic_destroy(ctx);
	}
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
	expressions_take_the_shape_their_precedence_gives();
	negative_literals_are_terms_wherever_written();
	mod_is_an_operator_only_after_an_operand();
	expression_mistakes_are_reported_where_written();
	nesting_is_bounded_in_the_text_and_in_the_tree();
	aggregates_are_the_literals_they_were_written_as();
	function_names_are_names_elsewhere();
	aggregate_mistakes_are_reported_where_written();
	calls_take_the_shape_their_arguments_give();
	atoms_and_calls_are_told_apart_by_what_follows();
	call_mistakes_are_reported_where_written();
	calls_count_toward_both_nesting_bounds();
	an_opened_input_is_closed_on_exec();
	loads_are_held_to_the_input_limit();

	return (check_summary("parse"));
}
