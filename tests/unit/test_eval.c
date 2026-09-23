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
 *
 * Every scenario runs twice, once under the semi-naive fixed point and once
 * under the naive one, against the same expected answers.  The counters are
 * the one thing the two are expected to differ in, and a scenario of their
 * own pins down both.
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
#define	SCENARIO_DISCARD	"a later load can make a derived tuple false"
#define	SCENARIO_RELEASE	"a discard gives back what the limits count"
#define	SCENARIO_REENTRY	"a callback that loads cannot overrun the stream"
#define	SCENARIO_DUPLICATES	"duplicate facts and derivations collapse"
#define	SCENARIO_LIMIT		"a query over a limit delivers no rows"
#define	SCENARIO_NEGATION	"a negated literal keeps what is absent"
#define	SCENARIO_ORDER		"the total order sorts across types"
#define	SCENARIO_RECURSION	"recursion reaches the least fixed point"
#define	SCENARIO_STATS		"the counters record the work each strategy did"
#define	SCENARIO_SNAPSHOT	"a lookup sees its relation as it started"

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

typedef struct counters counters_t;

/*
 * The counters of relic_stats_t that measure evaluation work.
 */
struct counters {
	uint64_t	iterations;
	uint64_t	derivations;
	uint64_t	inserted;
	uint64_t	scanned;
	uint64_t	looked_up;
};

typedef struct diag_log diag_log_t;

struct diag_log {
	size_t	errors;
	size_t	warnings;
};

typedef struct reentry reentry_t;

/*
 * What reenter_on_first_row() works with: the context, the query it runs
 * again from inside its own stream, and how many rows that stream delivered.
 */
struct reentry {
	relic_t		*ctx;
	relic_query_t	*query;
	size_t		 rows;
};

/*
 * The RELIC_F_ flags every context a scenario creates is given, which is how
 * main() runs the scenarios under each evaluation strategy in turn.
 */
static unsigned int strategy_flags;

static void	 append_text(rendering_t *rendering, const char *text,
		    size_t length);
static bool	 answers_are(const char *scenario, relic_t *ctx,
		    const char *query, const char *expected);
static relic_t	*context_with(const char *program, diag_log_t *log);
static relic_t	*context_flagged(const char *program, diag_log_t *log,
		    unsigned int flags);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static int	 compare_text(const void *left, const void *right);
static int	 count_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static int	 render_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static int	 reenter_on_first_row(void *callback_ctx,
		    const relic_value_t *values, size_t count);
static int	 run_query(relic_t *ctx, const char *text,
		    rendering_t *rendering);
static void	 sort_rows(char *text);
static void	 comparisons_follow_the_type_rules(void);
static void	 counters_are(const char *scenario, const char *what,
		    const relic_stats_t *stats, counters_t expected);
static void	 materialized_counters_are(const char *scenario,
		    const char *program, unsigned int flags, const char *what,
		    counters_t expected);
static void	 a_lookup_sees_its_relation_as_it_started(void);
static void	 a_callback_that_loads_cannot_overrun_the_stream(void);
static void	 a_discard_gives_back_what_the_limits_count(void);
static void	 a_later_load_can_make_a_derived_tuple_false(void);
static void	 a_later_load_extends_the_fixed_point(void);
static void	 a_query_over_a_limit_delivers_no_rows(void);
static void	 duplicates_collapse(void);
static void	 negation_keeps_what_is_absent(void);
static void	 every_binding_pattern_is_answered(void);
static void	 recursion_reaches_the_least_fixed_point(void);
static void	 run_scenarios(const char *strategy, unsigned int flags);
static void	 the_counters_record_the_work_done(void);
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
 * A context created with `flags` and holding a loaded program, or NULL
 * having recorded a failure when it does not load.  Diagnostics are counted
 * into `log`, which may be NULL.
 */
static relic_t *
context_flagged(const char *program, diag_log_t *log, unsigned int flags)
{
	relic_config_t config;
	relic_t *ctx;

	if (log != NULL)
		bzero(log, sizeof(*log));

	bzero(&config, sizeof(config));
	config.flags = flags;

	ctx = relic_create_config(&config);
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

/*
 * A context under the strategy the scenarios are running with.
 */
static relic_t *
context_with(const char *program, diag_log_t *log)
{
	return (context_flagged(program, log, strategy_flags));
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

	/*
	 * Same generation joins the relation it derives in the middle of its
	 * body, so under the default planner sg is looked up by one column
	 * while the fixed point is still inserting into it.
	 */
	ctx = context_with(
	    "par(a, b).\npar(a, c).\npar(b, d).\npar(c, e).\n"
	    "par(d, f).\npar(e, g).\n"
	    "sg(X, X) :- par(X, _).\n"
	    "sg(X, X) :- par(_, X).\n"
	    "sg(X, Y) :- par(A, X), sg(A, B), par(B, Y).\n", NULL);
	if (ctx == NULL)
		return;

	answers_are(SCENARIO_RECURSION, ctx, "sg(d, X)?", "d|e");
	answers_are(SCENARIO_RECURSION, ctx, "sg(X, g)?", "f|g");
	answers_are(SCENARIO_RECURSION, ctx, "sg(b, X)?", "b|c");

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
	config.flags = strategy_flags;

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
 * adds rules and facts, and the next query answers over everything, rather
 * than from what was computed before.
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
 * A negated literal lets a binding through exactly when the tuple it names
 * is absent from a relation the schedule has already completed.
 *
 * The program is the task graph of architecture section 72: a task is
 * blocked while something it depends on is not done, and ready when it is
 * not blocked.  ship waits on build, which is not done, so ship is blocked;
 * build waits only on design, which is done; design waits on nothing.  That
 * puts done/1 in the lowest stratum, blocked/1 above it and ready/1 above
 * that, so ready/1 negates a relation that itself holds a negation.
 *
 * The queries cover a negation over a relation nothing derives into, one
 * whose arguments are all constants, one written before the literal that
 * binds its variable, a query that negates, and a negation of a relation of
 * arity zero.
 */
static void
negation_keeps_what_is_absent(void)
{
	relic_t *ctx;

	ctx = context_with(
	    "todo(design).\ntodo(build).\ntodo(ship).\n"
	    "depends(build, design).\ndepends(ship, build).\n"
	    "done(design).\n"
	    "blocked(X) :- todo(X), depends(X, Y), not done(Y).\n"
	    "ready(X) :- todo(X), not blocked(X).\n"
	    "unowned(X) :- todo(X), not owner(X).\n"
	    "frozen.\n"
	    "movable(X) :- todo(X), not frozen.\n"
	    "early(X) :- not done(X), todo(X), not done(ship).\n", NULL);
	if (ctx == NULL)
		return;

	answers_are(SCENARIO_NEGATION, ctx, "blocked(X)?", "ship");
	answers_are(SCENARIO_NEGATION, ctx, "ready(X)?", "build|design");
	answers_are(SCENARIO_NEGATION, ctx, "unowned(X)?", "build|design|ship");
	answers_are(SCENARIO_NEGATION, ctx, "early(X)?", "build|ship");
	answers_are(SCENARIO_NEGATION, ctx, "movable(X)?", "");
	answers_are(SCENARIO_NEGATION, ctx,
	    "todo(X), not ready(X), not done(X)?", "ship");
	answers_are(SCENARIO_NEGATION, ctx, "todo(X), not todo(X)?", "");

	relic_destroy(ctx);
}

/*
 * A program with negation is not monotone: a fact loaded after an evaluation
 * can make false a tuple that evaluation derived.  Here ready(build) holds
 * after the first load, and the second gives build a dependency that is not
 * done, which blocks it.  The next query must see build blocked and not
 * ready, which it can only do if the tuples derived before the load are
 * discarded rather than continued from.
 */
static void
a_later_load_can_make_a_derived_tuple_false(void)
{
	relic_t *ctx;

	ctx = context_with("todo(design).\ntodo(build).\n"
	    "done(design).\n"
	    "blocked(X) :- todo(X), depends(X, Y), not done(Y).\n"
	    "ready(X) :- todo(X), not blocked(X).\n", NULL);
	if (ctx == NULL)
		return;

	answers_are(SCENARIO_DISCARD, ctx, "ready(X)?", "build|design");

	if (relic_load_string(ctx, "more.dl",
	    "todo(test).\ndepends(build, test).\n") != 0) {
		check(false, SCENARIO_DISCARD, "the second load succeeds");
		relic_destroy(ctx);
		return;
	}

	answers_are(SCENARIO_DISCARD, ctx, "blocked(X)?", "build");
	answers_are(SCENARIO_DISCARD, ctx, "ready(X)?", "design|test");

	relic_destroy(ctx);
}

/*
 * Discarding the relations returns the totals the limits are enforced
 * against, so a program re-evaluated after a load is held to its own size and
 * not to the sum of every evaluation before it.
 *
 * With a limit of six tuples, two facts and the two answers of e(X)? fit.
 * After a third fact, the three facts and the three answers of a second query
 * fit exactly -- the first query's answers were discarded with everything
 * else -- where the ten a discard that kept its count would reach do not.
 */
static void
a_discard_gives_back_what_the_limits_count(void)
{
	relic_config_t config;
	relic_stats_t stats;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	config.max_tuples = 6;
	config.flags = strategy_flags;

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, SCENARIO_RELEASE, "a context is created");
		return;
	}

	if (relic_load_string(ctx, "test.dl", "e(a).\ne(b).\n") != 0) {
		check(false, SCENARIO_RELEASE, "the program loads");
		relic_destroy(ctx);
		return;
	}
	answers_are(SCENARIO_RELEASE, ctx, "e(X)?", "a|b");

	if (relic_load_string(ctx, "more.dl", "e(c).\n") != 0) {
		check(false, SCENARIO_RELEASE, "the second load succeeds");
		relic_destroy(ctx);
		return;
	}
	answers_are(SCENARIO_RELEASE, ctx, "e(X)?", "a|b|c");

	relic_stats(ctx, &stats);
	checkf(stats.tuples == 6, SCENARIO_RELEASE,
	    "the context should hold 6 tuples, not %zu", stats.tuples);

	relic_destroy(ctx);
}

static int
count_row(void *callback_ctx, const relic_value_t *values RELIC_UNUSED,
    size_t count RELIC_UNUSED)
{
	size_t *rows;

	rows = callback_ctx;
	(*rows)++;

	return (0);
}

/*
 * A row callback that breaks the contract of relic_query_run(): on the first
 * row it loads facts that empty the answer and runs the query again, inside
 * the stream that is delivering it.
 */
static int
reenter_on_first_row(void *callback_ctx, const relic_value_t *values
    RELIC_UNUSED, size_t count RELIC_UNUSED)
{
	reentry_t *reentry;
	size_t inner_rows;

	reentry = callback_ctx;
	reentry->rows++;
	if (reentry->rows > 1)
		return (0);

	inner_rows = 0;
	if (relic_load_string(reentry->ctx, "inner.dl", "c(x).\nc(y).\n") == 0)
		relic_query_run(reentry->ctx, reentry->query, count_row,
		    &inner_rows);
	check(inner_rows == 0, SCENARIO_REENTRY,
	    "the inner run finds the answer emptied");

	return (0);
}

/*
 * A callback may not call into the context, but one that does must not make
 * the library read past a relation.  Here the inner run discards the answer
 * relation the outer stream is walking and refills it with nothing, so the
 * outer stream's next identifier names no tuple; the stream stops there
 * rather than reading one.
 */
static void
a_callback_that_loads_cannot_overrun_the_stream(void)
{
	reentry_t reentry;

	bzero(&reentry, sizeof(reentry));

	reentry.ctx = context_with("a(x).\na(y).\n"
	    "b(X) :- a(X), not c(X).\n", NULL);
	if (reentry.ctx == NULL)
		return;

	reentry.query = relic_query_prepare(reentry.ctx, NULL, "b(X)?");
	if (reentry.query == NULL) {
		check(false, SCENARIO_REENTRY, "the query is prepared");
		relic_destroy(reentry.ctx);
		return;
	}

	check(relic_query_run(reentry.ctx, reentry.query,
	    reenter_on_first_row, &reentry) == 0, SCENARIO_REENTRY,
	    "the outer run completes");
	checkf(reentry.rows == 1, SCENARIO_REENTRY,
	    "the outer stream should stop after 1 row, not %zu", reentry.rows);

	relic_query_destroy(reentry.query);
	relic_destroy(reentry.ctx);
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

/*
 * Whether a materialization's counters are the ones expected, reporting what
 * they were when they are not.  Counters are written as iterations,
 * derivations, insertions, tuples scanned and tuples looked up.
 */
static void
counters_are(const char *scenario, const char *what,
    const relic_stats_t *stats, counters_t expected)
{
	checkf(stats->iterations == expected.iterations &&
	    stats->derivations == expected.derivations &&
	    stats->tuples_inserted == expected.inserted &&
	    stats->tuples_scanned == expected.scanned &&
	    stats->tuples_looked_up == expected.looked_up, scenario,
	    "%s should be %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
	    " %" PRIu64 ", was %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
	    " %" PRIu64, what, expected.iterations, expected.derivations,
	    expected.inserted, expected.scanned, expected.looked_up,
	    stats->iterations, stats->derivations, stats->tuples_inserted,
	    stats->tuples_scanned, stats->tuples_looked_up);
}

/*
 * Materialize `program` under `flags` and compare its counters.
 */
static void
materialized_counters_are(const char *scenario, const char *program,
    unsigned int flags, const char *what, counters_t expected)
{
	relic_stats_t stats;
	relic_t *ctx;

	ctx = context_flagged(program, NULL, flags);
	if (ctx != NULL && relic_materialize(ctx) == 0) {
		relic_stats(ctx, &stats);
		counters_are(scenario, what, &stats, expected);
	} else {
		checkf(false, scenario, "%s: the program is materialized",
		    what);
	}
	relic_destroy(ctx);
}

/*
 * The counters of one materialization of a chain, under each strategy and
 * each planner, computed by hand.
 *
 *	edge(a, b).  edge(b, c).  edge(c, d).
 *	path(X, Y) :- edge(X, Y).
 *	path(X, Z) :- path(X, Y), edge(Y, Z).
 *
 * Counters are given as iterations, derivations, insertions, tuples scanned
 * and tuples looked up.  Insertions are 9 throughout: 3 facts and the 6
 * paths of the fixed point.
 *
 * The textual planner (-X no-index, -X no-reorder) joins path first and
 * scans edge for every path.  The facts are one pass: 3 derivations.  In the
 * path component's first round the base rule scans 3 edges and derives ab,
 * bc and cd; the recursive rule scans those 3 paths, 3 edges for each, and
 * derives ac and bd.  That is 5 derivations over 15 scanned tuples; ad is
 * still missing.
 *
 * Semi-naively, round 1 joins the 5 new paths with the edges: 20 scanned,
 * deriving ac and bd again and ad for the first time.  Round 2 joins ad
 * alone: 4 scanned, nothing derived, so it is the last.  3 rounds, 11
 * derivations, 39 scanned.
 *
 * Naively, pass 2 reruns both rules over everything: 3 edges and 3
 * derivations for the base rule, 5 paths and 15 edges and 3 derivations,
 * one of them ad, for the recursive one.  Pass 3 is the same over 6 paths
 * and finds nothing new: 3 + 6 + 18 scanned, 6 derived.  3 passes, 20
 * derivations, 65 scanned.
 *
 * The default planner measures edge, complete with 3 tuples, against path,
 * still being derived and assumed to hold 1000, so the recursive rule scans
 * edge(Y, Z) and looks path(X, Y) up by Y; its delta variant scans the delta
 * of path and looks edge(Y, Z) up by Y.  In the first round the base rule
 * derives ab, bc and cd as before, and the recursive rule scans the 3 edges:
 * for ab it looks up paths ending in a and finds none, for bc those ending
 * in b and finds ab, deriving ac, and for cd those ending in c and finds bc
 * and the ac just inserted, deriving bd and ad.  6 scanned, 3 looked up, 6
 * derivations, and the fixed point is already reached.
 *
 * Semi-naively, round 1 scans the 6 new paths and looks up the edges leaving
 * each one's end: bc for ab, cd for bc and for ac, nothing for the rest.  3
 * looked up, 3 derivations, none new.  2 rounds, 12 derivations, 12 scanned,
 * 6 looked up.
 *
 * Naively, pass 2 reruns both rules: 3 edges scanned and 3 derived by the
 * base rule, and the recursive rule's pass 1 again, 3 edges scanned, 3 paths
 * looked up and 3 derived.  2 passes, 15 derivations, 12 scanned, 6 looked
 * up.
 *
 * Under the textual planner the iteration counts agree, as they do on a
 * chain, and what semi-naive evaluation saves is the rederivation, so a
 * regression to naive behaviour shows in the derivations and the scans.
 */
static void
the_counters_record_the_work_done(void)
{
	static const char *const program =
	    "edge(a, b).\nedge(b, c).\nedge(c, d).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- path(X, Y), edge(Y, Z).\n";
	static const unsigned int textual =
	    RELIC_F_NO_INDEX | RELIC_F_NO_REORDER;
	relic_stats_t stats;
	relic_t *ctx;

	ctx = context_flagged(program, NULL, 0);
	if (ctx != NULL && relic_materialize(ctx) == 0) {
		relic_stats(ctx, &stats);
		counters_are(SCENARIO_STATS, "semi-naive", &stats,
		    (counters_t){ .iterations = 2, .derivations = 12,
		    .inserted = 9, .scanned = 12, .looked_up = 6 });
		check(stats.tuples == 9 && stats.symbol_values == 4 &&
		    stats.string_values == 0 && stats.integer_values == 0 &&
		    stats.storage_bytes > 0, SCENARIO_STATS,
		    "the context holds 9 tuples over 4 symbols");

		/* Nothing was loaded, so nothing is evaluated again. */
		check(relic_materialize(ctx) == 0, SCENARIO_STATS,
		    "a second materialization succeeds");
		relic_stats(ctx, &stats);
		check(stats.iterations == 2 && stats.derivations == 12,
		    SCENARIO_STATS, "a current program costs nothing");
	} else {
		check(false, SCENARIO_STATS, "the chain is materialized");
	}
	relic_destroy(ctx);

	materialized_counters_are(SCENARIO_STATS, program, RELIC_F_NAIVE,
	    "naive", (counters_t){ .iterations = 2, .derivations = 15,
	    .inserted = 9, .scanned = 12, .looked_up = 6 });
	materialized_counters_are(SCENARIO_STATS, program, textual,
	    "textual semi-naive", (counters_t){ .iterations = 3,
	    .derivations = 11, .inserted = 9, .scanned = 39, .looked_up = 0 });
	materialized_counters_are(SCENARIO_STATS, program,
	    textual | RELIC_F_NAIVE, "textual naive",
	    (counters_t){ .iterations = 3, .derivations = 20, .inserted = 9,
	    .scanned = 65, .looked_up = 0 });
}

/*
 * A lookup stops at the end its relation had when the lookup started, even
 * when the rule below it inserts into the very chain it is walking.  The
 * counters of this program, computed by hand, are what show it:
 *
 *	s(a).  seed(a, n0).  seed(a, m0).
 *	e(n0, n1).  e(n1, n2).  e(n2, n3).
 *	r(X, Y) :- seed(X, Y).
 *	r(X, Y) :- s(X), r(X, Z), e(Z, Y).
 *
 * The recursive rule scans s, looks r up by X and e up by Z, and emits into
 * r under the key X it is walking.  The 6 facts are 6 derivations and 6
 * insertions.  In the first round of r the base rule scans 2 seeds and
 * derives an0 and am0, so the chain for a holds 2 tuples.  The recursive
 * rule scans s, then walks that chain: an0 finds e(n0, n1) and inserts an1
 * at the end of the chain, am0 finds no edge, and an1 is past the end the
 * walk started with, so the walk stops.  3 scanned, 3 looked up, 3 derived.
 *
 * The rule's one delta variant then extends r one edge per round: round 1
 * scans the 3 new tuples and finds 2 edges, deriving an1 again and an2;
 * round 2 scans an2 and derives an3; round 3 scans an3 and finds nothing.
 * 4 rounds, 12 derivations, 11 insertions, 8 scanned, 6 looked up.
 *
 * A walk that did not stop would go on to an1, inserted while the walk was
 * at an0, derive an2 in the first round, and reach the fixed point a round
 * sooner with a derivation and two lookups more.
 */
static void
a_lookup_sees_its_relation_as_it_started(void)
{
	static const char *const program =
	    "s(a).\nseed(a, n0).\nseed(a, m0).\n"
	    "e(n0, n1).\ne(n1, n2).\ne(n2, n3).\n"
	    "r(X, Y) :- seed(X, Y).\n"
	    "r(X, Y) :- s(X), r(X, Z), e(Z, Y).\n";

	materialized_counters_are(SCENARIO_SNAPSHOT, program, 0, "default",
	    (counters_t){ .iterations = 4, .derivations = 12, .inserted = 11,
	    .scanned = 8, .looked_up = 6 });
}

/*
 * Every scenario that checks answers, under one evaluation strategy.  The
 * name goes to the log, so that a failure says which strategy it was under.
 */
static void
run_scenarios(const char *strategy, unsigned int flags)
{
	fprintf(stderr, "test_eval: under %s\n", strategy);
	strategy_flags = flags;

	recursion_reaches_the_least_fixed_point();
	every_binding_pattern_is_answered();
	duplicates_collapse();
	comparisons_follow_the_type_rules();
	a_query_over_a_limit_delivers_no_rows();
	a_later_load_extends_the_fixed_point();
	negation_keeps_what_is_absent();
	a_later_load_can_make_a_derived_tuple_false();
	a_discard_gives_back_what_the_limits_count();
	a_callback_that_loads_cannot_overrun_the_stream();
}

int
main(void)
{
	run_scenarios("the semi-naive fixed point", 0);
	run_scenarios("the naive fixed point", RELIC_F_NAIVE);
	run_scenarios("the planner without indexes", RELIC_F_NO_INDEX);
	run_scenarios("the planner in written order", RELIC_F_NO_REORDER);
	run_scenarios("the naive fixed point and the textual planner",
	    RELIC_F_NAIVE | RELIC_F_NO_INDEX | RELIC_F_NO_REORDER);

	the_counters_record_the_work_done();
	a_lookup_sees_its_relation_as_it_started();
	the_total_order_sorts_across_types();

	return (check_summary("test_eval"));
}
