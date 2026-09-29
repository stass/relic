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

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_APPEND		"loads append to one program"
#define	SCENARIO_ATOMIC		"a failed load changes nothing"
#define	SCENARIO_DEPTH		"an expression's nesting is bounded both ways"
#define	SCENARIO_EXPRESSION	"an expression is the tree its precedence builds"
#define	SCENARIO_EXPR_ERROR	"an expression's mistake is reported where written"
#define	SCENARIO_LIMIT		"a structural limit is enforced where written"
#define	SCENARIO_MOD		"`mod` is an operator only after an operand"
#define	SCENARIO_NEGATION	"`not` is a keyword only where a literal begins"
#define	SCENARIO_NEGATIVE	"a negative literal is one term wherever written"
#define	SCENARIO_RECOVERY	"one broken statement costs one statement"
#define	SCENARIO_SHAPE		"a statement is the shape it was written in"
#define	SCENARIO_TERMS		"a term holds the value it spells"

typedef struct diag_log diag_log_t;

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
static void	 render_constant(relic_t *ctx, relic_dom_t constant, char *out,
		    size_t size);
static void	 render_expr(relic_t *ctx, const relic_ast_expr_t *expr,
		    char *out, size_t size);
static relic_t	*context_logging_to(diag_log_t *log);
static size_t	 facts_held(relic_t *ctx, const char *name, uint16_t arity);
static void	 statements_keep_the_shape_they_were_written_in(void);
static void	 terms_hold_the_values_they_spell(void);

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
render_constant(relic_t *ctx, relic_dom_t constant, char *out, size_t size)
{
	relic_value_t value;
	char text[64];

	if (!relic_value_decode(ctx, constant, &value)) {
		strlcat(out, "?", size);
		return;
	}

	switch (value.type) {
	case RELIC_INTEGER:
		snprintf(text, sizeof(text), "%" PRId64, value.v.integer);
		strlcat(out, text, size);
		return;
	case RELIC_STRING:
		strlcat(out, "\"", size);
		strlcat(out, relic_string(ctx, value.v.id, NULL), size);
		strlcat(out, "\"", size);
		return;
	case RELIC_SYMBOL:
		strlcat(out, relic_string(ctx, value.v.id, NULL), size);
		return;
	}
}

/*
 * Append an expression tree to `out` in a prefix form that shows its shape
 * with nothing left to precedence: an operator node as `(op left right)`, a
 * negation as `(neg operand)`, and a leaf as the constant or variable it
 * holds.  Two trees render alike exactly when they have the same shape and
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

	switch (expr->kind) {
	case RELIC_AST_EXPR_TERM:
		term = &expr->u.term;
		if (term->kind == RELIC_TERM_VARIABLE)
			strlcat(out, relic_string(ctx, term->u.variable.name,
			    NULL), size);
		else
			render_constant(ctx, term->u.constant, out, size);
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
	relic_value_t value;
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
	    relic_value_decode(ctx, statement->head.terms[0].u.constant,
	    &value) && value.type == RELIC_INTEGER && value.v.integer == -2,
	    SCENARIO_NEGATIVE, "a head should hold the negative constant");
	check(statement != NULL && statement->body_length == 1 &&
	    statement->body[0].u.atom.arity == 2 &&
	    relic_value_decode(ctx, statement->body[0].u.atom.terms[1].u.constant,
	    &value) && value.type == RELIC_INTEGER && value.v.integer == -3,
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
 * an expression where only a term may stand.
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
		    "symbol bar cannot be an operand of *", 22 }
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

	return (check_summary("parse"));
}
