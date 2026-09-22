/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Evaluation: the answers a program's fixed point gives.
 *
 * Each test loads a program, asks a query through the public interface, and
 * compares the rows against an answer written out by hand.  The expected
 * answers are small enough to verify by reading the program, which is what
 * makes them an independent oracle: nothing here recomputes a fixed point.
 *
 * Rows are rendered into one string, "b|c|d" for three one-column rows or
 * "a b|b c" for two two-column rows, and sorted before they are compared.
 * The order a fixed point delivers rows in is deterministic, but it is the
 * order that evaluation strategy derived them in, and a different strategy
 * derives the same set in a different order.  A query's answer is the set,
 * so the set is what is compared.
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

#define	SCENARIO_BINDINGS	"every binding pattern of a query is answered"
#define	SCENARIO_COMPARE	"comparisons follow the language's type rules"
#define	SCENARIO_CURRENT	"a later load extends the fixed point"
#define	SCENARIO_DUPLICATES	"duplicate facts and derivations collapse"
#define	SCENARIO_LIMIT		"a query over a limit delivers no rows"
#define	SCENARIO_NEGATION	"evaluation refuses a negated literal"
#define	SCENARIO_ORDER		"the total order sorts across types"
#define	SCENARIO_RECURSION	"recursion reaches the least fixed point"

/* Room for the rendered answers any test below expects. */
#define	RENDERED_MAX	1024
#define	ROWS_MAX	64

typedef struct rendering rendering_t;

/*
 * The rows a query delivered, rendered as text, and how many there were.
 */
struct rendering {
	relic_t	*ctx;
	char	 text[RENDERED_MAX];
	size_t	 length;
	size_t	 rows;
};

typedef struct diag_log diag_log_t;

struct diag_log {
	size_t	errors;
	size_t	warnings;
};

static void	 append_text(rendering_t *rendering, const char *text,
		    size_t length);
static bool	 answers_are(const char *scenario, relic_t *ctx,
		    const char *query, const char *expected);
static relic_t	*context_with(const char *program, diag_log_t *log);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static int	 compare_text(const void *left, const void *right);
static int	 render_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static int	 run_query(relic_t *ctx, const char *text,
		    rendering_t *rendering);
static void	 sort_rows(char *text);
static void	 comparisons_follow_the_type_rules(void);
static void	 a_later_load_extends_the_fixed_point(void);
static void	 a_query_over_a_limit_delivers_no_rows(void);
static void	 duplicates_collapse(void);
static void	 evaluation_refuses_negation(void);
static void	 every_binding_pattern_is_answered(void);
static void	 recursion_reaches_the_least_fixed_point(void);
static void	 the_total_order_sorts_across_types(void);

static void
record_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_UNUSED,
    const char *message RELIC_UNUSED)
{
	diag_log_t *log;

	log = callback_ctx;
	if (log == NULL)
		return;

	if (level == RELIC_ERROR)
		log->errors++;
	else if (level == RELIC_WARNING)
		log->warnings++;
}

/*
 * A context holding a loaded program, or NULL having recorded a failure when
 * it does not load.  Diagnostics are counted into `log`, which may be NULL.
 */
static relic_t *
context_with(const char *program, diag_log_t *log)
{
	relic_t *ctx;

	if (log != NULL)
		bzero(log, sizeof(*log));

	ctx = relic_create();
	if (ctx == NULL) {
		check(false, "creating a context", "a context is created");
		return (NULL);
	}
	relic_diag_set(ctx, record_diagnostic, log);

	if (relic_load_string(ctx, "test.dl", program) != 0) {
		check(false, "loading a test program", "the program loads");
		relic_destroy(ctx);
		return (NULL);
	}

	return (ctx);
}

static void
append_text(rendering_t *rendering, const char *text, size_t length)
{
	if (length > sizeof(rendering->text) - 1 - rendering->length)
		length = sizeof(rendering->text) - 1 - rendering->length;

	bcopy(text, rendering->text + rendering->length, length);
	rendering->length += length;
	rendering->text[rendering->length] = '\0';
}

/*
 * The row callback: append the row to the rendering, rows separated by `|`
 * and values by a space.  A string is written in quotes, so that a symbol
 * and a string with the same bytes render differently.
 */
static int
render_row(void *callback_ctx, const relic_value_t *values, size_t count)
{
	rendering_t *rendering;
	const char *bytes;
	char number[32];
	size_t length;
	size_t i;

	rendering = callback_ctx;

	if (rendering->rows > 0)
		append_text(rendering, "|", 1);
	rendering->rows++;

	for (i = 0; i < count; i++) {
		if (i > 0)
			append_text(rendering, " ", 1);

		if (values[i].type == RELIC_INTEGER) {
			snprintf(number, sizeof(number), "%" PRId64,
			    values[i].v.integer);
			append_text(rendering, number, strlen(number));
			continue;
		}

		bytes = relic_string(rendering->ctx, values[i].v.id, &length);
		if (values[i].type == RELIC_STRING)
			append_text(rendering, "\"", 1);
		append_text(rendering, bytes, length);
		if (values[i].type == RELIC_STRING)
			append_text(rendering, "\"", 1);
	}

	return (0);
}

/*
 * Prepare and run a query, rendering its rows.  Returns what the run
 * returned, or -1 when the query could not be prepared.
 */
static int
run_query(relic_t *ctx, const char *text, rendering_t *rendering)
{
	relic_query_t *query;
	int status;

	bzero(rendering, sizeof(*rendering));
	rendering->ctx = ctx;

	query = relic_query_prepare(ctx, NULL, text);
	if (query == NULL)
		return (-1);

	status = relic_query_run(ctx, query, render_row, rendering);
	relic_query_destroy(query);

	return (status);
}

static int
compare_text(const void *left, const void *right)
{
	return (strcmp(*(const char *const *)left,
	    *(const char *const *)right));
}

/*
 * Sort a rendering's rows bytewise, in place.
 */
static void
sort_rows(char *text)
{
	char copy[RENDERED_MAX];
	char *rows[ROWS_MAX];
	char *cursor;
	size_t count;
	size_t i;

	if (text[0] == '\0')
		return;

	strlcpy(copy, text, sizeof(copy));

	count = 0;
	cursor = copy;
	while (cursor != NULL && count < ROWS_MAX)
		rows[count++] = strsep(&cursor, "|");

	qsort(rows, count, sizeof(rows[0]), compare_text);

	text[0] = '\0';
	for (i = 0; i < count; i++) {
		if (i > 0)
			strlcat(text, "|", RENDERED_MAX);
		strlcat(text, rows[i], RENDERED_MAX);
	}
}

/*
 * Whether a query succeeds with exactly these rows, which are written in
 * sorted order.  A mismatch is reported with what was delivered, so a
 * failing run names the query and both answers.
 */
static bool
answers_are(const char *scenario, relic_t *ctx, const char *query,
    const char *expected)
{
	rendering_t rendering;

	if (run_query(ctx, query, &rendering) != 0) {
		checkf(false, scenario, "%s should succeed", query);
		return (false);
	}

	sort_rows(rendering.text);

	if (strcmp(rendering.text, expected) == 0)
		return (true);

	checkf(false, scenario, "%s gave \"%s\", expected \"%s\"", query,
	    rendering.text, expected);

	return (false);
}

/*
 * Transitive closure over a chain, a cycle and a complete graph, mutual
 * recursion, and a rule with two recursive literals.  Each answer is small
 * enough to check by reading the program.
 */
static void
recursion_reaches_the_least_fixed_point(void)
{
	relic_t *ctx;

	ctx = context_with(
	    "edge(a, b).\nedge(b, c).\nedge(c, d).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n"
	    "ring(1, 2).\nring(2, 3).\nring(3, 1).\n"
	    "reach(X, Y) :- ring(X, Y).\n"
	    "reach(X, Z) :- reach(X, Y), reach(Y, Z).\n"
	    "node(p).\nnode(q).\n"
	    "full(X, Y) :- node(X), node(Y).\n"
	    "succ(0, 1).\nsucc(1, 2).\nsucc(2, 3).\nsucc(3, 4).\n"
	    "even(0).\n"
	    "odd(X) :- succ(Y, X), even(Y).\n"
	    "even(X) :- succ(Y, X), odd(Y).\n", NULL);
	if (ctx == NULL)
		return;

	answers_are(SCENARIO_RECURSION, ctx, "path(a, X)?", "b|c|d");
	answers_are(SCENARIO_RECURSION, ctx, "path(X, d)?", "a|b|c");
	answers_are(SCENARIO_RECURSION, ctx, "reach(1, X)?", "1|2|3");
	answers_are(SCENARIO_RECURSION, ctx, "reach(X, X)?", "1|2|3");
	answers_are(SCENARIO_RECURSION, ctx, "full(p, X)?", "p|q");
	answers_are(SCENARIO_RECURSION, ctx, "even(X)?", "0|2|4");
	answers_are(SCENARIO_RECURSION, ctx, "odd(X)?", "1|3");

	relic_destroy(ctx);
}

/*
 * A query binds any subset of its arguments, and each subset is answered:
 * free, bound, both bound, the same variable twice, and anonymous.  A query
 * with no variables asks whether something holds, and answers one row of no
 * values or none.
 */
static void
every_binding_pattern_is_answered(void)
{
	relic_t *ctx;
	rendering_t rendering;

	ctx = context_with(
	    "e(a, b).\ne(b, b).\ne(b, c).\nf(b, 1).\nf(c, 2).\n", NULL);
	if (ctx == NULL)
		return;

	answers_are(SCENARIO_BINDINGS, ctx, "e(X, Y)?", "a b|b b|b c");
	answers_are(SCENARIO_BINDINGS, ctx, "e(a, X)?", "b");
	answers_are(SCENARIO_BINDINGS, ctx, "e(X, b)?", "a|b");
	answers_are(SCENARIO_BINDINGS, ctx, "e(X, X)?", "b");
	answers_are(SCENARIO_BINDINGS, ctx, "e(_, X)?", "b|c");
	answers_are(SCENARIO_BINDINGS, ctx, "e(X, _Ignored)?",
	    "a b|b b|b c");
	answers_are(SCENARIO_BINDINGS, ctx, "e(X, Y), f(Y, N)?",
	    "a b 1|b b 1|b c 2");
	answers_are(SCENARIO_BINDINGS, ctx, "f(Y, N), e(X, Y)?",
	    "b 1 a|b 1 b|c 2 b");

	if (run_query(ctx, "e(a, b)?", &rendering) == 0)
		check(rendering.rows == 1 && rendering.length == 0,
		    SCENARIO_BINDINGS, "a query that holds has one empty row");
	else
		check(false, SCENARIO_BINDINGS, "e(a, b)? runs");

	if (run_query(ctx, "e(c, a)?", &rendering) == 0)
		check(rendering.rows == 0, SCENARIO_BINDINGS,
		    "a query that does not hold has no rows");
	else
		check(false, SCENARIO_BINDINGS, "e(c, a)? runs");

	relic_destroy(ctx);
}

/*
 * A relation is a set, so a fact written twice is one tuple, and an answer
 * derived along two paths is one row.
 */
static void
duplicates_collapse(void)
{
	relic_t *ctx;

	ctx = context_with(
	    "e(a, b).\ne(a, b).\ne(a, c).\ne(c, b).\n"
	    "r(Y) :- e(_, Y).\n", NULL);
	if (ctx == NULL)
		return;

	answers_are(SCENARIO_DUPLICATES, ctx, "e(a, X)?", "b|c");
	answers_are(SCENARIO_DUPLICATES, ctx, "r(X)?", "b|c");

	relic_destroy(ctx);
}

/*
 * Equality is exact and total: values of different types are unequal.
 * Ordering is numeric for integers and bytewise for symbols and strings, and
 * an ordering comparison between values of different types filters the
 * binding out rather than failing.
 */
static void
comparisons_follow_the_type_rules(void)
{
	relic_t *ctx;

	ctx = context_with(
	    "v(-9223372036854775808).\nv(-1).\nv(10).\nv(9).\n"
	    "v(apple).\nv(banana).\nv(\"apple\").\nv(\"b\").\n", NULL);
	if (ctx == NULL)
		return;

	answers_are(SCENARIO_COMPARE, ctx, "v(X), X < 9?",
	    "-1|-9223372036854775808");
	answers_are(SCENARIO_COMPARE, ctx, "v(X), X >= 9?", "10|9");
	answers_are(SCENARIO_COMPARE, ctx, "v(X), X <= -1?",
	    "-1|-9223372036854775808");
	answers_are(SCENARIO_COMPARE, ctx, "v(X), X > apple?", "banana");
	answers_are(SCENARIO_COMPARE, ctx, "v(X), X >= \"apple\"?",
	    "\"apple\"|\"b\"");
	answers_are(SCENARIO_COMPARE, ctx, "v(X), X = apple?", "apple");
	answers_are(SCENARIO_COMPARE, ctx, "v(X), X = \"apple\"?",
	    "\"apple\"");
	answers_are(SCENARIO_COMPARE, ctx, "v(X), v(Y), X = Y, X < 0?",
	    "-1 -1|-9223372036854775808 -9223372036854775808");
	answers_are(SCENARIO_COMPARE, ctx, "v(X), X != 10, X > 0?", "9");
	answers_are(SCENARIO_COMPARE, ctx, "v(X), 1 < 2, X = banana?",
	    "banana");
	answers_are(SCENARIO_COMPARE, ctx, "v(X), 2 < 1?", "");

	relic_destroy(ctx);
}

/*
 * A limit that fires during evaluation fails the query and delivers no rows,
 * because a truncated answer would be indistinguishable from a complete one.
 */
static void
a_query_over_a_limit_delivers_no_rows(void)
{
	relic_config_t config;
	relic_t *ctx;
	diag_log_t log;
	rendering_t rendering;

	bzero(&config, sizeof(config));
	config.max_tuples = 8;

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, SCENARIO_LIMIT, "a context is created");
		return;
	}
	bzero(&log, sizeof(log));
	relic_diag_set(ctx, record_diagnostic, &log);

	if (relic_load_string(ctx, "test.dl",
	    "edge(a, b).\nedge(b, c).\nedge(c, d).\nedge(d, e).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n") != 0) {
		check(false, SCENARIO_LIMIT, "the program loads");
		relic_destroy(ctx);
		return;
	}

	check(run_query(ctx, "path(a, X)?", &rendering) != 0, SCENARIO_LIMIT,
	    "the query fails");
	checkf(rendering.rows == 0, SCENARIO_LIMIT,
	    "no row should be delivered, %zu were", rendering.rows);
	checkf(log.errors == 1, SCENARIO_LIMIT,
	    "one error should name the limit, %zu were reported", log.errors);
	check(strstr(relic_error(ctx), "limit exceeded: tuples=8") != NULL,
	    SCENARIO_LIMIT, "the error names the limit and its value");

	relic_destroy(ctx);
}

/*
 * The relations are a fixed point of the rules loaded so far.  A later load
 * adds rules and facts, and the next query continues the fixed point over
 * everything, rather than answering from what was computed before.
 */
static void
a_later_load_extends_the_fixed_point(void)
{
	relic_t *ctx;

	ctx = context_with("edge(a, b).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n", NULL);
	if (ctx == NULL)
		return;

	answers_are(SCENARIO_CURRENT, ctx, "path(a, X)?", "b");

	check(relic_materialize(ctx) == 0 &&
	    ctx->materialized_rules == ctx->program.rules.count,
	    SCENARIO_CURRENT, "the relations are current after a query");

	if (relic_load_string(ctx, "more.dl", "edge(b, c).\n") != 0) {
		check(false, SCENARIO_CURRENT, "the second load succeeds");
		relic_destroy(ctx);
		return;
	}

	answers_are(SCENARIO_CURRENT, ctx, "path(a, X)?", "b|c");
	answers_are(SCENARIO_CURRENT, ctx, "path(a, X)?", "b|c");

	relic_destroy(ctx);
}

/*
 * A negated literal cannot be evaluated, so evaluation fails naming the rule
 * while checking the same program succeeds.
 */
static void
evaluation_refuses_negation(void)
{
	relic_t *ctx;
	diag_log_t log;

	ctx = context_with("todo(a).\nblocked(b).\n"
	    "ready(X) :- todo(X), not blocked(X).\n", &log);
	if (ctx == NULL)
		return;

	check(relic_check(ctx) == 0, SCENARIO_NEGATION,
	    "the program passes its check");
	check(relic_materialize(ctx) != 0, SCENARIO_NEGATION,
	    "evaluating it fails");
	checkf(log.errors == 1, SCENARIO_NEGATION,
	    "one error should be reported, %zu were", log.errors);
	check(strstr(relic_error(ctx), "ready/1") != NULL, SCENARIO_NEGATION,
	    "the error names the rule");

	relic_destroy(ctx);
}

/*
 * The order -s sorts by: every integer before every symbol before every
 * string, then numerically or bytewise, with a shorter byte string before a
 * longer one it begins.
 */
static void
the_total_order_sorts_across_types(void)
{
	relic_t *ctx;
	relic_value_t unknown;
	relic_value_t values[6];
	uint32_t id;
	size_t i;

	ctx = relic_create();
	if (ctx == NULL) {
		check(false, SCENARIO_ORDER, "a context is created");
		return;
	}

	bzero(values, sizeof(values));
	values[0].type = RELIC_INTEGER;
	values[0].v.integer = INT64_MIN;
	values[1].type = RELIC_INTEGER;
	values[1].v.integer = 3;
	values[2].type = RELIC_SYMBOL;
	relic_symbol_intern(ctx, "a", 1, &id);
	values[2].v.id = id;
	values[3].type = RELIC_SYMBOL;
	relic_symbol_intern(ctx, "ab", 2, &id);
	values[3].v.id = id;
	values[4].type = RELIC_STRING;
	relic_symbol_intern(ctx, "", 0, &id);
	values[4].v.id = id;
	values[5].type = RELIC_STRING;
	relic_symbol_intern(ctx, "a", 1, &id);
	values[5].v.id = id;

	for (i = 0; i + 1 < sizeof(values) / sizeof(values[0]); i++) {
		checkf(relic_value_compare(ctx, &values[i],
		    &values[i + 1]) < 0, SCENARIO_ORDER,
		    "value %zu should sort before value %zu", i, i + 1);
		checkf(relic_value_compare(ctx, &values[i + 1],
		    &values[i]) > 0, SCENARIO_ORDER,
		    "value %zu should sort after value %zu", i + 1, i);
		checkf(relic_value_compare(ctx, &values[i], &values[i]) == 0,
		    SCENARIO_ORDER, "value %zu should equal itself", i);
	}

	/*
	 * A symbol the context never assigned is ordered rather than read:
	 * before every symbol it knows, and equal to another it does not.
	 */
	unknown = values[2];
	unknown.v.id = UINT32_MAX;
	check(relic_value_compare(ctx, &unknown, &values[2]) < 0 &&
	    relic_value_compare(ctx, &values[2], &unknown) > 0 &&
	    relic_value_compare(ctx, &unknown, &unknown) == 0,
	    SCENARIO_ORDER, "an unknown symbol sorts before every known one");

	relic_destroy(ctx);
}

int
main(void)
{
	recursion_reaches_the_least_fixed_point();
	every_binding_pattern_is_answered();
	duplicates_collapse();
	comparisons_follow_the_type_rules();
	a_query_over_a_limit_delivers_no_rows();
	a_later_load_extends_the_fixed_point();
	evaluation_refuses_negation();
	the_total_order_sorts_across_types();

	return (check_summary("test_eval"));
}
