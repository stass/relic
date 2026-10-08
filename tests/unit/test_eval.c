/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
 *
 * All of it then runs again with every round's insertions deferred, on the
 * calling thread alone and shared among DEFERRAL_THREADS threads, against
 * the same answers and the same counters.
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
#define	SCENARIO_DUPLICATES	"duplicate facts and derivations collapse"
#define	SCENARIO_LIMIT		"a query over a limit delivers no rows"
#define	SCENARIO_NEGATION	"a negated literal keeps what is absent"
#define	SCENARIO_ONCE		"every derivation is made exactly once"
#define	SCENARIO_ORDER		"the total order sorts across types"
#define	SCENARIO_RECURSION	"recursion reaches the least fixed point"
#define	SCENARIO_STATS		"the counters record the work each strategy did"
#define	SCENARIO_SNAPSHOT	"a naive pass walks a chain one edge further"
#define	SCENARIO_BATCHES	"a plan reads none of its own batches"
#define	SCENARIO_UNREACHED	"a rule no query reaches costs it no memory"
#define	SCENARIO_SHARED		"a round is shared when it repays the threads"

/*
 * The fan-out of the batch scenario: more tuples than one batch holds, so
 * that a single plan inserts a full batch while it runs.  FAN_LINE_MAX
 * bounds a line of the program it writes, whose longest, an edge between
 * two nodes numbered below 1000, takes 16 bytes.
 */
#define	FAN_OUT			100
#define	FAN_LINE_MAX		32

/*
 * The length of the rule a_rule_no_query_reaches_costs_it_no_memory() never
 * plans, and the memory limit its query runs under.
 */
#define	UNREACHED_LENGTH	2000u
#define	UNREACHED_LIMIT		24000u

/*
 * The threads the scenarios' rounds are shared among the third time they
 * run, and the layered graph of rounds_are_shared_when_they_repay_it():
 * LAYERS layers of LAYER_WIDTH nodes, every node of a layer joined to every
 * node of the next.
 */
#define	DEFERRAL_THREADS	4
#define	LAYERS			12
#define	LAYER_WIDTH		24
#define	LAYERED_PATHS		(LAYER_WIDTH * LAYER_WIDTH * LAYERS * \
				    (LAYERS - 1) / 2)
#define	LAYERED_EDGES		(LAYER_WIDTH * LAYER_WIDTH * (LAYERS - 1))

_Static_assert(FAN_OUT > RELIC_INSERT_BATCH, "the fan-out fills a batch");
_Static_assert(FAN_OUT < 1000, "a line of the fan fits FAN_LINE_MAX");

/*
 * The executor is handed the context, the deltas and every relation and
 * index read-only, and writes nothing but its own scratch and the frontier
 * of a key it looks up.  That is what lets the plans of a round run on
 * several threads over a store none of them can write.  lib/exec.c sees the
 * context, relations and indexes as the incomplete types of frozen.h, and
 * refuses to compile beside the headers that complete them, so it can reach
 * them only through the functions frozen.h declares.  The assertions below
 * hold the rest of the contract: the handles the executor is given, and the
 * lookup that stores a frontier, stop compiling the moment one of them
 * becomes writable, so that dropping a const fails a build rather than a
 * review.
 */
_Static_assert(_Generic(&relic_exec_start,
    void (*)(relic_scratch_t *, const relic_exec_env_t *,
    const relic_plan_t *): 1, default: 0),
    "the executor starts a plan from read-only handles");
_Static_assert(_Generic(&relic_exec_run,
    enum relic_exec_status (*)(relic_scratch_t *): 1, default: 0),
    "the executor runs a plan from its scratch alone");
_Static_assert(_Generic(((const relic_scratch_t *)NULL)->plan,
    const relic_plan_t *: 1, default: 0),
    "the scratch holds the plan it runs read-only");
_Static_assert(_Generic(((const relic_scratch_t *)NULL)->env,
    const relic_exec_env_t *: 1, default: 0),
    "the scratch holds what the plan reads read-only");
_Static_assert(_Generic(((const relic_exec_env_t *)NULL)->ctx,
    const relic_t *: 1, default: 0),
    "the executor reads the context read-only");
_Static_assert(_Generic(((const relic_exec_env_t *)NULL)->deltas,
    const relic_tuple_range_t *: 1, default: 0),
    "the executor reads the deltas read-only");
_Static_assert(_Generic(((const relic_op_atom_t *)NULL)->relation,
    const relic_relation_t *: 1, default: 0),
    "a plan holds the relations it reads read-only");
_Static_assert(_Generic(&relic_relation_index_at,
    const relic_index_t *(*)(const relic_relation_t *, uint32_t): 1,
    default: 0),
    "a relation hands the executor its indexes read-only");
_Static_assert(_Generic(&relic_index_first_since,
    relic_tuple_id_t (*)(const relic_index_t *, const relic_relation_t *,
    const relic_dom_t *, relic_tuple_id_t): 1, default: 0),
    "a lookup of a delta holds its index read-only");

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

/*
 * The RELIC_F_ flags every context a scenario creates is given, which is how
 * main() runs the scenarios under each evaluation strategy in turn.
 */
static unsigned int strategy_flags;

/*
 * RELIC_F_PARALLEL_ALWAYS while main() runs every scenario again with each
 * round's insertions deferred to its end, and zero the first time, and the
 * threads the deferred rounds are shared among.  Every context is given
 * both, whatever else it is given: a deferred round inserts what the serial
 * one does, in the order it does, on any number of threads, so no answer
 * and no counter written below may change.
 */
static unsigned int deferral_flags;
static size_t deferral_threads;

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
static int	 render_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static int	 run_query(relic_t *ctx, const char *text,
		    rendering_t *rendering);
static void	 sort_rows(char *text);
static void	 comparisons_follow_the_type_rules(void);
static void	 counters_are(const char *scenario, const char *what,
		    const relic_stats_t *stats, counters_t expected);
static void	 materialized_counters_are(const char *scenario,
		    const char *program, unsigned int flags, const char *what,
		    counters_t expected);
static void	 a_delta_is_emptied_when_its_member_rests(void);
static void	 a_naive_pass_walks_a_chain_one_edge_further(void);
static void	 a_plan_reads_none_of_its_own_batches(void);
static void	 a_rule_no_query_reaches_costs_it_no_memory(void);
static void	 fan_counters_are(const char *what, bool binary,
		    unsigned int flags, counters_t expected);
static void	 a_discard_gives_back_what_the_limits_count(void);
static void	 a_later_load_can_make_a_derived_tuple_false(void);
static void	 a_later_load_extends_the_fixed_point(void);
static void	 a_query_over_a_limit_delivers_no_rows(void);
static void	 duplicates_collapse(void);
static void	 negation_keeps_what_is_absent(void);
static void	 every_binding_pattern_is_answered(void);
static void	 every_derivation_is_made_once(void);
static void	 recursion_reaches_the_least_fixed_point(void);
static relic_t	*context_threaded(const char *program, unsigned int flags,
		    size_t threads);
static char	*layered_program(void);
static void	 rounds_are_shared_when_they_repay_it(void);
static void	 run_everything(void);
static void	 run_scenarios(const char *strategy, unsigned int flags);
static void	 the_counters_record_the_work_done(void);
static void	 the_total_order_sorts_across_types(void);
static void	 two_member_literals_derive_once(void);

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
	config.flags = flags | deferral_flags;
	config.threads = deferral_threads;

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
	config.flags = strategy_flags | deferral_flags;

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
	    relic_relations_current(ctx),
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
 * The program is a small project-tracking task graph: a task is blocked
 * while something it depends on is not done, and ready when it is not
 * blocked.  ship waits on build, which is not done, so ship is blocked;
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
 * fit exactly, where the eight a discard that kept its count would reach do
 * not.  Once delivered the answers are released too, leaving the facts.
 */
static void
a_discard_gives_back_what_the_limits_count(void)
{
	relic_config_t config;
	relic_stats_t stats;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	config.max_tuples = 6;
	config.flags = strategy_flags | deferral_flags;

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
	checkf(stats.tuples == 3, SCENARIO_RELEASE,
	    "the context should hold 3 tuples, not %zu", stats.tuples);

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
	 * A symbol the context never assigned is reported rather than read,
	 * and compares equal to everything; a collected result's value is
	 * named as one.
	 */
	unknown = values[2];
	unknown.v.id = RELIC_DOM_INDEX_MASK;
	check(relic_value_compare(ctx, &unknown, &values[2]) == 0 &&
	    relic_value_compare(ctx, &values[2], &unknown) == 0 &&
	    strstr(relic_error(ctx), "not one this context holds") != NULL,
	    SCENARIO_ORDER, "an unknown symbol is reported and compares equal");
	unknown.v.id = RELIC_RESULT_ID_BIT | 1;
	check(relic_value_compare(ctx, &values[2], &unknown) == 0 &&
	    strstr(relic_error(ctx), "relic_result_compare()") != NULL,
	    SCENARIO_ORDER, "a result's symbol is reported as a result's");

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
 * and tuples looked up.  Insertions are 6 throughout: the 6 paths of the
 * fixed point.  edge/2 is a predicate of facts alone, read from its base
 * store, so evaluation neither derives nor inserts its 3 tuples; the
 * context holds them all the same.
 *
 * The recursive rule's body is satisfied 3 times over the fixed point, by
 * ab and bc, bc and cd, and ac and cd, and the base rule's 3 times, so the
 * semi-naive fixed point, which makes every derivation once, makes 6 under
 * either planner.
 *
 * Semi-naively, round 0 runs the base rule alone: 3 edges scanned, deriving
 * ab, bc and cd.  The recursive rule runs only as its variant, which reads
 * the delta of path.  Round 1's delta is those 3 paths, round 2's is ac and
 * bd, and round 3's is ad, which extends nothing, so round 3 is the last: 4
 * rounds.
 *
 * The textual planner (-X no-index, -X no-reorder) scans edge for every
 * path of the delta: 3 + 12 + 8 + 4 = 27 scanned.
 *
 * Naively, every pass runs both plans over everything, and the recursive
 * rule joins path first and scans edge for every path.  Pass 1: the base
 * rule scans 3 edges and derives ab, bc and cd; the recursive rule scans
 * those 3 paths, 3 edges for each, and derives ac and bd from the paths the
 * base rule's plan derived before it started.  Pass 2 is 3 edges
 * and 3 derivations for the base rule, and 5 paths and 15 edges and 3
 * derivations, one of them ad, for the recursive one.  Pass 3 is the same
 * over 6 paths and finds nothing new: 3 + 6 + 18 scanned, 6 derived.  3
 * passes, 5 + 6 + 6 = 17 derivations, 65 scanned.
 *
 * The default planner measures edge, complete with 3 tuples, against path,
 * still being derived and assumed to hold 1000.  The variant scans the delta
 * of path and looks edge(Y, Z) up by Y: bc for ab, cd for bc in round 1, cd
 * for ac in round 2.  3 + 3 + 2 + 1 = 9 scanned and 3 looked up.
 *
 * Naively, the recursive rule's plan scans edge(Y, Z) and looks path(X, Y)
 * up by Y.  In pass 1 the base rule derives ab, bc and cd as before, and the
 * recursive rule scans the 3 edges: for ab it looks up paths ending in a and
 * finds none, for bc those ending in b and finds ab, deriving ac, and for cd
 * those ending in c and finds bc, deriving bd; ac is derived by the plan
 * that is running, which reads path as it stood when it started.  3 + 3
 * scanned, 2 looked up, 3 + 2 derived.  Pass 2 derives the same over 5
 * paths, and for cd finds bc and ac, deriving bd and the new ad: 3 + 3
 * scanned, 3 looked up, 3 + 3 derived.  Pass 3 is pass 2 again and finds
 * nothing new.  3 passes, 5 + 6 + 6 = 17 derivations, 18 scanned, 8
 * looked up.
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
		    (counters_t){ .iterations = 4, .derivations = 6,
		    .inserted = 6, .scanned = 9, .looked_up = 3 });
		check(stats.tuples == 9 && stats.symbol_values == 4 &&
		    stats.string_values == 0 && stats.integer_values == 0 &&
		    stats.storage_bytes > 0, SCENARIO_STATS,
		    "the context holds 9 tuples over 4 symbols");

		/* Nothing was loaded, so nothing is evaluated again. */
		check(relic_materialize(ctx) == 0, SCENARIO_STATS,
		    "a second materialization succeeds");
		relic_stats(ctx, &stats);
		check(stats.iterations == 4 && stats.derivations == 6,
		    SCENARIO_STATS, "a current program costs nothing");
	} else {
		check(false, SCENARIO_STATS, "the chain is materialized");
	}
	relic_destroy(ctx);

	materialized_counters_are(SCENARIO_STATS, program, RELIC_F_NAIVE,
	    "naive", (counters_t){ .iterations = 3, .derivations = 17,
	    .inserted = 6, .scanned = 18, .looked_up = 8 });
	materialized_counters_are(SCENARIO_STATS, program, textual,
	    "textual semi-naive", (counters_t){ .iterations = 4,
	    .derivations = 6, .inserted = 6, .scanned = 27, .looked_up = 0 });
	materialized_counters_are(SCENARIO_STATS, program,
	    textual | RELIC_F_NAIVE, "textual naive",
	    (counters_t){ .iterations = 3, .derivations = 17, .inserted = 6,
	    .scanned = 65, .looked_up = 0 });
}

/*
 * Under the naive fixed point a pass extends a chain by one edge: a rule
 * that walks the chain it derives into sees none of what it derives in the
 * same pass.  The counters, computed by hand, pin the naive evaluator's
 * work on a small recursive program:
 *
 *	s(a).  seed(a, n0).  seed(a, m0).
 *	e(n0, n1).  e(n1, n2).  e(n2, n3).
 *	r(X, Y) :- seed(X, Y).
 *	r(X, Y) :- s(X), r(X, Z), e(Z, Y).
 *
 * The recursive rule scans s, looks r up by X and e up by Z, and emits into r
 * under the key X it is walking.  The 6 facts are read from base stores, so
 * they are neither derived nor inserted.  Every pass scans the 2 seeds and
 * derives an0 and am0, and scans s: 3 scanned and 2 derived.  In pass 1 the
 * recursive rule then walks the chain for a, which holds those 2 tuples: an0
 * finds e(n0, n1) and derives an1, am0 finds no edge, and an1 is past the end
 * the chain had when the plan started, so the walk stops.  2 + 1 looked up, 1
 * derived.  Each later pass walks one more tuple of the chain and finds one
 * more edge: pass 2 looks up 3 + 2 and derives an1 and an2, pass 3 looks up 4 +
 * 3 and derives an1, an2 and an3, and pass 4 looks up 5 + 3, derives the same
 * three and finds nothing new.  4 passes, 3 + 4 + 5 + 5 = 17 derivations, 5
 * insertions, 12 scanned, 23 looked up.
 *
 * Each plan here derives fewer tuples than a batch holds, so an1 waits in
 * the batch until the plan ends and the walk never meets it;
 * a_plan_reads_none_of_its_own_batches() is the scenario in which a batch
 * is inserted while a walk runs.
 */
static void
a_naive_pass_walks_a_chain_one_edge_further(void)
{
	static const char *const program =
	    "s(a).\nseed(a, n0).\nseed(a, m0).\n"
	    "e(n0, n1).\ne(n1, n2).\ne(n2, n3).\n"
	    "r(X, Y) :- seed(X, Y).\n"
	    "r(X, Y) :- s(X), r(X, Z), e(Z, Y).\n";

	materialized_counters_are(SCENARIO_SNAPSHOT, program, RELIC_F_NAIVE,
	    "naive", (counters_t){ .iterations = 4, .derivations = 17,
	    .inserted = 5, .scanned = 12, .looked_up = 23 });
}

/*
 * A plan that derives more tuples than a batch holds, so that one batch is
 * inserted while the plan runs, reads none of them: what it reads of its
 * head relation stops where the relation ended when the plan started, not
 * where the last batch left it.  With the fan-out F = FAN_OUT, the program
 * is written with r unary, read by membership, and with r binary, read by a
 * lookup or, without indexes, by a scan:
 *
 *	e(s0, t0).  ...  e(sF-1, tF-1).
 *	e(t0, u0).  ...  e(tF-1, uF-1).
 *	r(s0).  ...  r(sF-1).		r(s0, w).  ...  r(sF-1, w).
 *	r(Y) :- e(Z, Y), r(Z).		r(Y, W) :- e(Z, Y), r(Z, W).
 *
 * Under the naive fixed point in written order, the recursive rule scans the
 * 2F edges and, for each, tests its source for membership in r, which is not
 * counted as looked up, looks r up by it, or scans the whole of r.  The 2F
 * edges are read from e's base store, neither derived nor inserted.  r's F
 * facts seed its relation before the first pass, F derivations and
 * insertions made once, and then:
 *
 *	pass 1: s edges derive the F tuples tI, the t edges find no tI in r
 *	pass 2: s edges derive tI again, the t edges the F new tuples uI
 *	pass 3: both derive what r holds, and nothing is new
 *
 * which is 3 passes, F + F + 2F + 2F derivations, F + F + F insertions and
 * 3 * 2F edges scanned.  The lookups find one sI in pass 1,
 * and one sI and one tI for each of theirs in passes 2 and 3: F + 2F + 2F
 * looked up.  The scans of r read the F, 2F and 3F tuples r held when each
 * pass's plan started, once per edge: 2F * (F + 2F + 3F) = 12F^2 more
 * scanned.  A plan that read the batch it inserted at the
 * RELIC_INSERT_BATCH-th derivation would find those tI in r in pass 1 and
 * make more derivations.
 */
static void
a_plan_reads_none_of_its_own_batches(void)
{
	static const unsigned int flags = RELIC_F_NAIVE | RELIC_F_NO_REORDER;

	fan_counters_are("a membership test", false, flags,
	    (counters_t){ .iterations = 3, .derivations = 6 * FAN_OUT,
	    .inserted = 3 * FAN_OUT, .scanned = 6 * FAN_OUT,
	    .looked_up = 0 });
	fan_counters_are("a lookup", true, flags,
	    (counters_t){ .iterations = 3, .derivations = 6 * FAN_OUT,
	    .inserted = 3 * FAN_OUT, .scanned = 6 * FAN_OUT,
	    .looked_up = 5 * FAN_OUT });
	fan_counters_are("a scan", true, flags | RELIC_F_NO_INDEX,
	    (counters_t){ .iterations = 3, .derivations = 6 * FAN_OUT,
	    .inserted = 3 * FAN_OUT,
	    .scanned = 6 * FAN_OUT + 12 * FAN_OUT * FAN_OUT,
	    .looked_up = 0 });
}

/*
 * An evaluation holds room for the operations of the plans it builds, and
 * for no rule it never plans.  The program holds a rule of UNREACHED_LENGTH
 * literals that the query never reaches, so the query's evaluation plans
 * only the component it reads.  Room for the long rule's plan, sixteen
 * bytes an operation, would take more than UNREACHED_LIMIT bytes on its
 * own, while everything the query needs, its rewrite included, fits within
 * the limit.
 */
static void
a_rule_no_query_reaches_costs_it_no_memory(void)
{
	relic_config_t config;
	rendering_t rendering;
	relic_t *ctx;
	char *program;
	size_t used;
	size_t size;
	unsigned int i;

	size = 64 + UNREACHED_LENGTH * sizeof("e(X), ");
	program = malloc(size);
	if (program == NULL) {
		check(false, SCENARIO_UNREACHED, "the program is built");
		return;
	}
	used = (size_t)snprintf(program, size, "q(a).\ne(b).\nr(X) :- e(X)");
	for (i = 1; i < UNREACHED_LENGTH; i++)
		used += (size_t)snprintf(program + used, size - used, ", e(X)");
	(void)snprintf(program + used, size - used, ".\n");

	bzero(&config, sizeof(config));
	config.max_memory = UNREACHED_LIMIT;
	config.flags = deferral_flags;

	ctx = relic_create_config(&config);
	if (ctx == NULL || relic_load_string(ctx, "test.dl", program) != 0) {
		check(false, SCENARIO_UNREACHED, "the program loads");
		relic_destroy(ctx);
		free(program);
		return;
	}

	checkf(run_query(ctx, "q(X)?", &rendering) == 0 &&
	    strcmp(rendering.text, "a") == 0, SCENARIO_UNREACHED,
	    "q(X)? should answer a under memory=%u: %s", UNREACHED_LIMIT,
	    relic_error(ctx));

	relic_destroy(ctx);
	free(program);
}

/*
 * Materialize the fan of a_plan_reads_none_of_its_own_batches(), with r of
 * arity 2 when `binary` and of arity 1 otherwise, and compare the counters
 * with `expected`.  Every line of the program is well under
 * FAN_LINE_MAX bytes.
 */
static void
fan_counters_are(const char *what, bool binary, unsigned int flags,
    counters_t expected)
{
	char *program;
	size_t length;
	size_t capacity;
	int i;

	capacity = (size_t)FAN_OUT * 3 * FAN_LINE_MAX + FAN_LINE_MAX;
	program = malloc(capacity);
	if (program == NULL) {
		check(false, SCENARIO_BATCHES, "the program is written");
		return;
	}

	length = 0;
	for (i = 0; i < FAN_OUT; i++)
		length += (size_t)snprintf(program + length, capacity - length,
		    "e(s%d, t%d).\n", i, i);
	for (i = 0; i < FAN_OUT; i++)
		length += (size_t)snprintf(program + length, capacity - length,
		    "e(t%d, u%d).\n", i, i);
	for (i = 0; i < FAN_OUT; i++)
		length += (size_t)snprintf(program + length, capacity - length,
		    binary ? "r(s%d, w).\n" : "r(s%d).\n", i);
	snprintf(program + length, capacity - length, "%s", binary ?
	    "r(Y, W) :- e(Z, Y), r(Z, W).\n" : "r(Y) :- e(Z, Y), r(Z).\n");

	materialized_counters_are(SCENARIO_BATCHES, program, flags, what,
	    expected);
	free(program);
}

/*
 * The complete graph on 40 nodes, loops included, closed transitively:
 *
 *	par(nI, nJ) for every I and J from 0 to 39
 *	tc(X, Y) :- par(X, Y).
 *	tc(X, Z) :- par(X, Y), tc(Y, Z).
 *
 * par's 1600 facts are read from its base store.  Round 0 derives the 1600
 * tuples of the exit rule, which are the whole closure.  Round 1 scans those
 * 1600 as the delta of tc and looks up the 40 tuples of par ending in each
 * one's first node: 64,000 derivations, none new, and the fixed point is
 * reached.  Every derivation is one of the 64,000 ways the recursive rule's
 * body can be satisfied, made once.  The query then scans tc and derives its
 * 1600 rows, which it delivers as it derives them and inserts nowhere.  2
 * rounds, 67,200 derivations, 1600 insertions, 4800 scanned, 64,000 looked
 * up.  A round that could see what its own rules inserted derives most of
 * the closure twice.
 */
static void
every_derivation_is_made_once(void)
{
	rendering_t rendering;
	relic_stats_t stats;
	relic_t *ctx;
	char *program;
	size_t length;
	size_t capacity;
	int i;
	int j;

	capacity = 40 * 40 * 32 + 128;
	program = malloc(capacity);
	if (program == NULL) {
		check(false, SCENARIO_ONCE, "the program is written");
		return;
	}

	length = 0;
	for (i = 0; i < 40; i++) {
		for (j = 0; j < 40; j++)
			length += (size_t)snprintf(program + length,
			    capacity - length, "par(n%d, n%d).\n", i, j);
	}
	snprintf(program + length, capacity - length,
	    "tc(X, Y) :- par(X, Y).\ntc(X, Z) :- par(X, Y), tc(Y, Z).\n");

	ctx = context_flagged(program, NULL, 0);
	free(program);

	if (ctx != NULL && relic_materialize(ctx) == 0 &&
	    run_query(ctx, "tc(X, Y)?", &rendering) == 0) {
		check(rendering.rows == 1600, SCENARIO_ONCE,
		    "the closure is every ordered pair");
		relic_stats(ctx, &stats);
		counters_are(SCENARIO_ONCE, "the complete graph", &stats,
		    (counters_t){ .iterations = 2, .derivations = 67200,
		    .inserted = 1600, .scanned = 4800, .looked_up = 64000 });
	} else {
		check(false, SCENARIO_ONCE, "the complete graph is closed");
	}
	relic_destroy(ctx);
}

/*
 * A rule whose body reads its own predicate twice, over a chain of 20
 * nodes:
 *
 *	edge(n0, n1).  ...  edge(n18, n19).
 *	p(X, Y) :- edge(X, Y).
 *	p(X, Z) :- p(X, Y), p(Y, Z).
 *
 * p holds a pair for every two nodes in chain order, and the recursive
 * rule's body is satisfied once for every three: X, Y and Z in chain order.
 * Its two variants, one reading the first literal's delta and the second's
 * current view, the other the first literal's old view and the second's
 * delta, between them make each of those 20 choose 3 = 1140 derivations
 * once.  With the exit rule's 19 derivations and the query's 20 choose 2 =
 * 190 rows, that is 1349 derivations; the 19 facts are read from edge's
 * base store and are not derived.
 *
 * Round 0 derives the 19 pairs one edge apart, and each round after it
 * derives the pairs up to twice as far apart as the round before: 2 in round
 * 1, 3 to 4 in round 2, then 5 to 8, 9 to 16, and 17 to 19 in round 5, so
 * round 6 finds nothing: 7 rounds.  Both variants scan their delta first.
 * The first runs in rounds 1 to 6 and scans every pair once; the second,
 * whose old view is empty until round 1 has run, only in rounds 2 to 6, and
 * scans every pair but the 19 of round 0.  With round 0's 19 edges and the
 * query's 190 rows, that is 19 + 190 + 171 + 190 = 570 scanned.
 */
static void
two_member_literals_derive_once(void)
{
	rendering_t rendering;
	relic_stats_t stats;
	relic_t *ctx;
	char program[1024];
	size_t length;
	int i;

	length = 0;
	for (i = 0; i < 19; i++)
		length += (size_t)snprintf(program + length,
		    sizeof(program) - length, "edge(n%d, n%d).\n", i, i + 1);
	snprintf(program + length, sizeof(program) - length,
	    "p(X, Y) :- edge(X, Y).\np(X, Z) :- p(X, Y), p(Y, Z).\n");

	ctx = context_flagged(program, NULL, 0);
	if (ctx != NULL && relic_materialize(ctx) == 0 &&
	    run_query(ctx, "p(X, Y)?", &rendering) == 0) {
		check(rendering.rows == 190, SCENARIO_ONCE,
		    "p holds every pair in chain order");
		relic_stats(ctx, &stats);
		checkf(stats.iterations == 7 && stats.derivations == 1349 &&
		    stats.tuples_scanned == 570, SCENARIO_ONCE,
		    "the chain should take 7 1349 570, took %" PRIu64 " %"
		    PRIu64 " %" PRIu64, stats.iterations, stats.derivations,
		    stats.tuples_scanned);
	} else {
		check(false, SCENARIO_ONCE, "the chain is closed");
	}
	relic_destroy(ctx);
}

/*
 * Two members that gain in alternate rounds, and a rule that reads one
 * through its old view in a round the other gained in:
 *
 *	s(a).  e(a, b).  e(b, c).  e(c, d).  e(d, e).  j(b, c, z).
 *	p(X) :- s(X).
 *	q(Y) :- p(X), e(X, Y).
 *	p(Y) :- q(X), e(X, Y).
 *	p(Z) :- q(X), p(Y), j(X, Y, Z).
 *
 * Round 0 derives p(a), and the rounds after it alternate: q(b) in round 1,
 * p(c) in round 2, q(d) in round 3, p(e) in round 4.  q gains nothing in
 * round 2, so at the boundary before round 3 its delta, q(b), must become
 * empty and q(b) move into q's old view.  In round 3 the last rule's variant
 * reading p's delta, p(c), then finds q(b) in that old view and derives
 * p(z).  Nothing else can derive it: in round 2 the other variant finds q(b)
 * in its delta but p(c) is not yet in p's current view.  Round 5 finds
 * nothing.
 *
 * The derivations are the ways the bodies are satisfied, each once: p(a),
 * q from p(a) and p(c), p from q(b) and q(d), and p(z).  The 6 facts are
 * read from base stores and are not derived.  6 rounds, 6 derivations, 6
 * insertions.
 */
static void
a_delta_is_emptied_when_its_member_rests(void)
{
	static const char *const program =
	    "s(a).\ne(a, b).\ne(b, c).\ne(c, d).\ne(d, e).\nj(b, c, z).\n"
	    "p(X) :- s(X).\n"
	    "q(Y) :- p(X), e(X, Y).\n"
	    "p(Y) :- q(X), e(X, Y).\n"
	    "p(Z) :- q(X), p(Y), j(X, Y, Z).\n";
	relic_stats_t stats;
	relic_t *ctx;

	ctx = context_flagged(program, NULL, 0);
	if (ctx != NULL && relic_materialize(ctx) == 0) {
		relic_stats(ctx, &stats);
		checkf(stats.iterations == 6 && stats.derivations == 6 &&
		    stats.tuples_inserted == 6, SCENARIO_ONCE,
		    "the alternating members should take 6 6 6, took %"
		    PRIu64 " %" PRIu64 " %" PRIu64, stats.iterations,
		    stats.derivations, stats.tuples_inserted);
		answers_are(SCENARIO_ONCE, ctx, "p(X)?", "a|c|e|z");
		answers_are(SCENARIO_ONCE, ctx, "q(X)?", "b|d");
	} else {
		check(false, SCENARIO_ONCE, "the alternating members are "
		    "materialized");
	}
	relic_destroy(ctx);
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
}

/*
 * Every scenario, under every strategy.
 */
static void
run_everything(void)
{
	run_scenarios("the semi-naive fixed point", 0);
	run_scenarios("the naive fixed point", RELIC_F_NAIVE);
	run_scenarios("the planner without indexes", RELIC_F_NO_INDEX);
	run_scenarios("the planner in written order", RELIC_F_NO_REORDER);
	run_scenarios("the naive fixed point and the textual planner",
	    RELIC_F_NAIVE | RELIC_F_NO_INDEX | RELIC_F_NO_REORDER);

	the_counters_record_the_work_done();
	a_naive_pass_walks_a_chain_one_edge_further();
	a_plan_reads_none_of_its_own_batches();
	a_rule_no_query_reaches_costs_it_no_memory();
	every_derivation_is_made_once();
	two_member_literals_derive_once();
	a_delta_is_emptied_when_its_member_rests();
	the_total_order_sorts_across_types();
}

/*
 * A context with `flags` and `threads`, and none of the flags or threads
 * the scenarios are running with, holding a loaded program, or NULL having
 * recorded a failure.
 */
static relic_t *
context_threaded(const char *program, unsigned int flags, size_t threads)
{
	relic_config_t config;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	config.flags = flags;
	config.threads = threads;

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, "creating a context", "a context is created");
		return (NULL);
	}

	if (relic_load_string(ctx, "test.dl", program) != 0) {
		check(false, "loading a test program", "the program loads");
		relic_destroy(ctx);
		return (NULL);
	}

	return (ctx);
}

/*
 * The paths of a graph of LAYERS layers of LAYER_WIDTH nodes, every node of
 * a layer joined to every node of the next, or NULL when memory is short.
 */
static char *
layered_program(void)
{
	char *text;
	size_t size;
	size_t used;
	unsigned int layer;
	unsigned int from;
	unsigned int to;

	size = 128 + (size_t)LAYERED_EDGES * 32;
	text = malloc(size);
	if (text == NULL)
		return (NULL);

	used = (size_t)snprintf(text, size,
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- path(X, Y), edge(Y, Z).\n");
	for (layer = 0; layer + 1 < LAYERS; layer++) {
		for (from = 0; from < LAYER_WIDTH; from++) {
			for (to = 0; to < LAYER_WIDTH && used < size; to++)
				used += (size_t)snprintf(text + used,
				    size - used, "edge(l%u_%u, l%u_%u).\n",
				    layer, from, layer + 1, to);
		}
	}
	if (used >= size) {
		free(text);
		return (NULL);
	}

	return (text);
}

/*
 * A round is shared among a context's threads only when it holds enough
 * work to repay them, and parallel_rounds counts exactly the rounds that
 * were.
 *
 * Under RELIC_F_PARALLEL_ALWAYS every round that may be deferred is, and it
 * is shared only when the context has more than one thread: a program of
 * four rounds counts none at one thread and some at two.
 *
 * Without the switch, the paths of the layered graph take a round for each
 * length.  The first two rounds are not shared: the base pass copies the
 * edges, and the recursive rule's first round has no round before it to
 * predict its work and is estimated at the paths it reads; each reads
 * LAYERED_EDGES tuples, 6 336, below the threshold.  The rounds after them
 * extend hundreds or thousands of paths by LAYER_WIDTH edges each, or are
 * predicted from a round that did, and are shared.  So some rounds and not all are
 * counted, and the relation is what the serial evaluator derives.  A build
 * without threads shares none.
 */
static void
rounds_are_shared_when_they_repay_it(void)
{
	static const char *const chain =
	    "edge(a, b).\nedge(b, c).\nedge(c, d).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- path(X, Y), edge(Y, Z).\n";
	relic_stats_t stats;
	relic_t *ctx;
	char *layered;
	bool threads_available;

#ifdef RELIC_THREADS
	threads_available = true;
#else
	threads_available = false;
#endif

	ctx = context_threaded(chain, RELIC_F_PARALLEL_ALWAYS, 1);
	if (ctx != NULL && relic_materialize(ctx) == 0) {
		relic_stats(ctx, &stats);
		checkf(stats.parallel_rounds == 0, SCENARIO_SHARED,
		    "on one thread no round is shared, not %" PRIu64,
		    stats.parallel_rounds);
	} else {
		check(false, SCENARIO_SHARED, "the chain materializes");
	}
	relic_destroy(ctx);

	ctx = context_threaded(chain, RELIC_F_PARALLEL_ALWAYS, 2);
	if (ctx != NULL && relic_materialize(ctx) == 0) {
		relic_stats(ctx, &stats);
		checkf((stats.parallel_rounds > 0) == threads_available,
		    SCENARIO_SHARED, "on two threads rounds are shared, "
		    "%" PRIu64 " of them", stats.parallel_rounds);
	} else {
		check(false, SCENARIO_SHARED, "the chain materializes");
	}
	relic_destroy(ctx);

	layered = layered_program();
	if (layered == NULL) {
		check(false, SCENARIO_SHARED, "the layered graph is written");
		return;
	}

	ctx = context_threaded(layered, 0, DEFERRAL_THREADS);
	if (ctx != NULL && relic_materialize(ctx) == 0) {
		relic_stats(ctx, &stats);
		checkf(stats.tuples == LAYERED_EDGES + LAYERED_PATHS,
		    SCENARIO_SHARED, "the layered graph has %d paths, not %zu",
		    LAYERED_PATHS, stats.tuples - LAYERED_EDGES);
		if (threads_available) {
			checkf(stats.parallel_rounds > 0 &&
			    stats.parallel_rounds < stats.iterations,
			    SCENARIO_SHARED, "some of %" PRIu64 " rounds are "
			    "shared and not all, not %" PRIu64,
			    stats.iterations, stats.parallel_rounds);
			check(ctx->pool != NULL && ctx->pool->thread_tasks > 0,
			    SCENARIO_SHARED, "the pool's threads run some of "
			    "the shared rounds' tasks");
		} else
			checkf(stats.parallel_rounds == 0, SCENARIO_SHARED,
			    "without threads no round is shared, not %" PRIu64,
			    stats.parallel_rounds);
	} else {
		check(false, SCENARIO_SHARED, "the layered graph materializes");
	}
	relic_destroy(ctx);

	free(layered);
}

int
main(void)
{
	run_everything();

	fprintf(stderr, "test_eval: every round deferred\n");
	deferral_flags = RELIC_F_PARALLEL_ALWAYS;
	run_everything();

	fprintf(stderr, "test_eval: every round deferred, on %d threads\n",
	    DEFERRAL_THREADS);
	deferral_threads = DEFERRAL_THREADS;
	run_everything();

	rounds_are_shared_when_they_repay_it();

	return (check_summary("test_eval"));
}
