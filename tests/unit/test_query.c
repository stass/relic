/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for queries: how one becomes a rule, and the interface a
 * caller prepares, enumerates and runs them through.
 *
 * The golden language tests compare the rows and the messages; this suite
 * examines what they cannot see -- the rule a query compiles to and the name
 * of its answer, that answering queries leaves the context as it was, that a
 * query that fails to prepare leaves the program as it was, that a callback
 * can stop the rows, and that a query cannot be run in a context it does not
 * belong to.
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

#define	SCENARIO_COLUMNS	"a query's columns are its named variables"
#define	SCENARIO_FOREIGN	"a query runs only in its own context"
#define	SCENARIO_GROWTH		"answering queries does not grow the context"
#define	SCENARIO_LIMIT		"an answer refused by a limit is given back"
#define	SCENARIO_LOADED		"the program's queries are enumerated"
#define	SCENARIO_NAMES		"answers are named in the order queries compile"
#define	SCENARIO_REFUSED	"a refused query leaves the program valid"
#define	SCENARIO_STOP		"a callback can stop the rows"
#define	SCENARIO_UNDEFINED	"a query warns of a predicate nothing defines"
#define	SCENARIO_UNSAFE		"an unsafe query makes the program invalid"

/* How many times the growth test prepares, runs and destroys its query. */
#define	GROWTH_ROUNDS		64

/* Room for one line of explain output about a small query. */
#define	EXPLAIN_TEXT_MAX	128

typedef struct context_size context_size_t;
typedef struct diag_log diag_log_t;
typedef struct explained explained_t;

/*
 * What a context holds that answering a query could leave behind: its
 * predicates, the slots of its relation table, and the tuples and bytes its
 * relations hold.
 */
struct context_size {
	size_t	predicates;
	size_t	relation_slots;
	size_t	tuples;
	size_t	storage_bytes;
};

struct diag_log {
	size_t	errors;
	size_t	warnings;
};

/*
 * The lines of explain output that name a query's answer: the heading that
 * writes the query's rule, and the EMIT of its plan, last seen of each.
 */
struct explained {
	char	query[EXPLAIN_TEXT_MAX];
	char	emit[EXPLAIN_TEXT_MAX];
};

static bool	 answer_once(relic_t *ctx, const char *text);
static bool	 answer_is_named(relic_t *ctx, relic_query_t *query,
		    explained_t *explained, const char *name);
static context_size_t context_size(relic_t *ctx);
static void	 growth_under(unsigned int flags, const char *strategy);
static relic_t	*context_with(const char *program, diag_log_t *log);
static int	 count_rows(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static void	 record_explained(void *callback_ctx, const char *line);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static int	 stop_after_one(void *callback_ctx,
		    const relic_value_t *values, size_t count);
static void	 a_callback_can_stop_the_rows(void);
static void	 a_query_runs_only_in_its_own_context(void);
static void	 a_query_warns_of_an_undefined_predicate(void);
static void	 a_refused_query_leaves_the_program_valid(void);
static void	 an_answer_refused_by_a_limit_is_given_back(void);
static void	 an_unsafe_query_makes_the_program_invalid(void);
static void	 answers_are_named_in_the_order_queries_compile(void);
static void	 answering_queries_does_not_grow_the_context(void);
static void	 columns_are_the_named_variables(void);
static void	 the_programs_queries_are_enumerated(void);

static void
record_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_UNUSED,
    const char *message RELIC_UNUSED)
{
	diag_log_t *log;

	log = callback_ctx;
	if (level == RELIC_ERROR)
		log->errors++;
	else if (level == RELIC_WARNING)
		log->warnings++;
}

static relic_t *
context_with(const char *program, diag_log_t *log)
{
	relic_t *ctx;

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

static int
count_rows(void *callback_ctx, const relic_value_t *values RELIC_UNUSED,
    size_t count RELIC_UNUSED)
{
	size_t *rows;

	rows = callback_ctx;
	(*rows)++;

	return (0);
}

static int
stop_after_one(void *callback_ctx, const relic_value_t *values RELIC_UNUSED,
    size_t count RELIC_UNUSED)
{
	size_t *rows;

	rows = callback_ctx;
	(*rows)++;

	return (1);
}

/*
 * Keep the explain lines that name a query's answer, leading spaces removed.
 */
static void
record_explained(void *callback_ctx, const char *line)
{
	explained_t *explained;

	explained = callback_ctx;

	while (*line == ' ')
		line++;

	if (strncmp(line, "query ", 6) == 0)
		strlcpy(explained->query, line, sizeof(explained->query));
	else if (strncmp(line, "emit ", 5) == 0)
		strlcpy(explained->emit, line, sizeof(explained->emit));
}

/*
 * Whether answering `query` explains it under `name`: the query's rule is
 * headed by the name and its plan emits into the relation of that name.  The
 * queries of this suite's naming test have no columns, so the name is the
 * whole of the head.
 */
static bool
answer_is_named(relic_t *ctx, relic_query_t *query, explained_t *explained,
    const char *name)
{
	char heading[EXPLAIN_TEXT_MAX];
	char emit[EXPLAIN_TEXT_MAX];
	size_t rows;

	bzero(explained, sizeof(*explained));
	rows = 0;
	if (query == NULL ||
	    relic_query_run(ctx, query, count_rows, &rows) != 0)
		return (false);

	snprintf(heading, sizeof(heading), "query %s :- ", name);
	snprintf(emit, sizeof(emit), "emit %s", name);

	return (strncmp(explained->query, heading, strlen(heading)) == 0 &&
	    strcmp(explained->emit, emit) == 0);
}

/*
 * A query's rule has one head argument per named variable, in order of first
 * occurrence, and `_` contributes none.  The head names no predicate, and the
 * answer relation has a column per argument.  A variable that occurs once is
 * an output column rather than a singleton, so it draws no warning.
 */
static void
columns_are_the_named_variables(void)
{
	relic_t *ctx;
	relic_query_t *query;
	const relic_rule_t *rule;
	const char *spelling;
	diag_log_t log;
	uint16_t column;
	static const char *const expected[] = { "Z", "X", "Y" };

	ctx = context_with("e(a, b).\nf(b, c).\n", &log);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, "f(Z, _), e(X, _), e(X, Y)?");
	if (query == NULL) {
		check(false, SCENARIO_COLUMNS, "the query is prepared");
		relic_destroy(ctx);
		return;
	}

	rule = &query->rule;
	check(rule->head == RELIC_ID_NONE, SCENARIO_COLUMNS,
	    "the head names no predicate");
	check(rule->head_arity == 3 && query->answer->arity == 3,
	    SCENARIO_COLUMNS, "the head and the answer have three columns");

	for (column = 0; column < rule->head_arity; column++) {
		spelling = relic_symbol_bytes(ctx,
		    rule->variables[rule->head_args[column].u.slot].name, NULL);
		checkf(spelling != NULL &&
		    strcmp(spelling, expected[column]) == 0,
		    SCENARIO_COLUMNS, "column %u should be %s",
		    (unsigned int)column, expected[column]);
	}

	checkf(log.warnings == 0, SCENARIO_COLUMNS,
	    "no warning should be reported, %zu were", log.warnings);

	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * A query's answer is named `$q` and the query's serial number, which counts
 * the queries the context has compiled in the order it compiled them.  The
 * program's own queries are compiled by the check and take $q0, $q1 in load
 * order; a query prepared afterwards takes the next number, and one that is
 * refused takes none; a query written in a later load takes the number after
 * the prepared one, since loads and preparations share one sequence.  Each
 * name is read from the explain output, which names the answer as the head
 * of the query's rule and as the relation its plan emits into.
 */
static void
answers_are_named_in_the_order_queries_compile(void)
{
	relic_t *ctx;
	relic_query_t *first;
	relic_query_t *refused;
	relic_query_t *prepared;
	relic_query_t *later;
	relic_query_t *last;
	explained_t explained;
	diag_log_t log;

	ctx = context_with("e(a).\ne(X)?\ne(a)?\n", &log);
	if (ctx == NULL)
		return;
	relic_explain_set(ctx, record_explained, &explained);

	first = relic_query_loaded(ctx, 1);
	refused = relic_query_prepare(ctx, NULL, "X = 1?");
	prepared = relic_query_prepare(ctx, NULL, "e(b)?");

	check(answer_is_named(ctx, first, &explained, "$q1"), SCENARIO_NAMES,
	    "the second query in the program is $q1");
	check(refused == NULL, SCENARIO_NAMES, "the unsafe query is refused");
	check(answer_is_named(ctx, prepared, &explained, "$q2"),
	    SCENARIO_NAMES, "the prepared query after the refused one is $q2");

	check(relic_load_string(ctx, "later.dl", "e(c).\ne(c)?\n") == 0,
	    SCENARIO_NAMES, "a second program loads");
	later = relic_query_loaded(ctx, 2);
	last = relic_query_prepare(ctx, NULL, "e(a)?");

	check(answer_is_named(ctx, later, &explained, "$q3"), SCENARIO_NAMES,
	    "the query of the second load, after the prepared one, is $q3");
	check(answer_is_named(ctx, last, &explained, "$q4"), SCENARIO_NAMES,
	    "a query prepared after it is $q4");

	relic_query_destroy(first);
	relic_query_destroy(prepared);
	relic_query_destroy(later);
	relic_query_destroy(last);
	relic_destroy(ctx);
}

static context_size_t
context_size(relic_t *ctx)
{
	context_size_t size;
	relic_stats_t stats;

	relic_stats(ctx, &stats);

	size.predicates = relic_pred_count(ctx);
	size.relation_slots = ctx->relations.count;
	size.tuples = stats.tuples;
	size.storage_bytes = stats.storage_bytes;

	return (size);
}

/*
 * Prepare a query, run it to its last row and destroy it, as an interactive
 * session does with every line.  Returns whether all three succeeded.
 */
static bool
answer_once(relic_t *ctx, const char *text)
{
	relic_query_t *query;
	size_t rows;
	int status;

	query = relic_query_prepare(ctx, NULL, text);
	if (query == NULL)
		return (false);

	rows = 0;
	status = relic_query_run(ctx, query, count_rows, &rows);
	relic_query_destroy(query);

	return (status == 0 && rows == 3);
}

/*
 * A query's answer belongs to the query, so answering one leaves the context
 * holding what it held before: after the first query, which may leave behind
 * the predicates its rewrite generates, every later one adds no predicate, no
 * relation slot, and no tuple or byte of storage.  Were each query given a
 * predicate of its own, a session of many queries would pay, on every answer,
 * for every query before it.  Both ways of answering are covered: through
 * the Magic rewrite, and from the whole fixed point.
 */
static void
answering_queries_does_not_grow_the_context(void)
{
	growth_under(0, "through the rewrite");
	growth_under(RELIC_F_NO_MAGIC, "from the whole fixed point");
}

/*
 * One strategy's run of answering_queries_does_not_grow_the_context().
 */
static void
growth_under(unsigned int flags, const char *strategy)
{
	relic_config_t config;
	relic_t *ctx;
	context_size_t first;
	context_size_t later;
	bool answered;
	size_t round;

	bzero(&config, sizeof(config));
	config.flags = flags;

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, SCENARIO_GROWTH, "a context is created");
		return;
	}

	if (relic_load_string(ctx, "test.dl",
	    "edge(a, b).\nedge(b, c).\nedge(c, d).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n") != 0) {
		check(false, SCENARIO_GROWTH, "the program loads");
		relic_destroy(ctx);
		return;
	}

	answered = answer_once(ctx, "path(a, X)?");
	first = context_size(ctx);

	for (round = 1; answered && round < GROWTH_ROUNDS; round++)
		answered = answer_once(ctx, "path(a, X)?");
	later = context_size(ctx);

	checkf(answered, SCENARIO_GROWTH,
	    "answered %s, every query should answer three rows", strategy);
	checkf(later.predicates == first.predicates, SCENARIO_GROWTH,
	    "answered %s, the context should hold %zu predicates, as after "
	    "the first query, not %zu", strategy, first.predicates,
	    later.predicates);
	checkf(later.relation_slots == first.relation_slots, SCENARIO_GROWTH,
	    "answered %s, the relation table should have %zu slots, as after "
	    "the first query, not %zu", strategy, first.relation_slots,
	    later.relation_slots);
	checkf(later.tuples == first.tuples, SCENARIO_GROWTH,
	    "answered %s, the relations should hold %zu tuples, as after the "
	    "first query, not %zu", strategy, first.tuples, later.tuples);
	checkf(later.storage_bytes == first.storage_bytes, SCENARIO_GROWTH,
	    "answered %s, the relations should hold %zu bytes, as after the "
	    "first query, not %zu", strategy, first.storage_bytes,
	    later.storage_bytes);

	relic_destroy(ctx);
}

/*
 * An answer counts against the tuple limit like any relation, and one that
 * the limit refuses part way is emptied and gives back what it counted: the
 * totals afterwards are what the program's own relations held before it.  The query is
 * destroyed after its context, which reads nothing of the context, and
 * leaves nothing for AddressSanitizer to report.
 */
static void
an_answer_refused_by_a_limit_is_given_back(void)
{
	relic_config_t config;
	relic_query_t *query;
	relic_stats_t before;
	relic_stats_t after;
	relic_t *ctx;
	diag_log_t log;
	size_t rows;

	bzero(&config, sizeof(config));
	config.max_tuples = 4;
	bzero(&log, sizeof(log));

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, SCENARIO_LIMIT, "a context is created");
		return;
	}
	relic_diag_set(ctx, record_diagnostic, &log);

	query = NULL;
	if (relic_load_string(ctx, "test.dl", "e(a).\ne(b).\ne(c).\n") == 0 &&
	    relic_materialize(ctx) == 0)
		query = relic_query_prepare(ctx, NULL, "e(X)?");
	relic_stats(ctx, &before);

	rows = 0;
	check(query != NULL &&
	    relic_query_run(ctx, query, count_rows, &rows) != 0 && rows == 0,
	    SCENARIO_LIMIT, "the query fails without delivering a row");
	check(strstr(relic_error(ctx), "while deriving $q0/1") != NULL,
	    SCENARIO_LIMIT, "the limit names the answer relation");

	relic_stats(ctx, &after);
	checkf(before.tuples == 3 && after.tuples == before.tuples,
	    SCENARIO_LIMIT, "the relations should hold the 3 facts, not %zu "
	    "tuples", after.tuples);
	checkf(after.storage_bytes == before.storage_bytes, SCENARIO_LIMIT,
	    "the relations should hold the facts' %zu bytes, not %zu",
	    before.storage_bytes, after.storage_bytes);
	check(query != NULL && relic_relation_count(query->answer) == 0 &&
	    relic_relation_bytes(query->answer) == 0, SCENARIO_LIMIT,
	    "the answer holds nothing");

	relic_destroy(ctx);
	relic_query_destroy(query);
}

/*
 * relic_query_count() counts the queries written in the program, and
 * relic_query_loaded() hands each one out; an index past the end is an
 * error rather than a fault.
 */
static void
the_programs_queries_are_enumerated(void)
{
	relic_t *ctx;
	relic_query_t *query;
	size_t before;
	size_t rows;
	diag_log_t log;

	ctx = context_with("e(a).\ne(b).\ne(X)?\ne(a)?\ne(c)?\n", &log);
	if (ctx == NULL)
		return;

	checkf(relic_query_count(ctx) == 3, SCENARIO_LOADED,
	    "three queries should be counted, %zu were",
	    relic_query_count(ctx));

	query = relic_query_loaded(ctx, 0);
	rows = 0;
	check(query != NULL &&
	    relic_query_run(ctx, query, count_rows, &rows) == 0 && rows == 2,
	    SCENARIO_LOADED, "the first query answers two rows");
	relic_query_destroy(query);

	check(relic_query_loaded(ctx, 3) == NULL && log.errors == 1,
	    SCENARIO_LOADED, "a fourth query is an error");

	relic_query_destroy(NULL);
	relic_destroy(ctx);

	/*
	 * A program that fails its check hands out none of its queries, not
	 * even the ones that compiled.
	 */
	ctx = context_with("e(a).\ne(X)?\nbad(X) :- e(a).\n", &log);
	if (ctx == NULL)
		return;

	check(relic_query_count(ctx) == 1 && relic_query_loaded(ctx, 0) == NULL,
	    SCENARIO_LOADED, "an invalid program's query is not handed out");

	/*
	 * Every failure reports at least one error, including a second
	 * refusal of a program whose mistakes were reported the first time.
	 */
	before = log.errors;
	check(relic_query_loaded(ctx, 0) == NULL && log.errors > before,
	    SCENARIO_LOADED, "a second refusal reports an error too");
	before = log.errors;
	check(relic_query_prepare(ctx, NULL, "e(X)?") == NULL &&
	    log.errors > before, SCENARIO_LOADED,
	    "preparing against it reports an error");
	before = log.errors;
	check(relic_materialize(ctx) != 0 && log.errors > before,
	    SCENARIO_LOADED, "evaluating it reports an error");

	relic_destroy(ctx);
}

/*
 * An unsafe query in the program is a mistake in the program: the check
 * reports it and fails, exactly as for an unsafe rule.
 */
static void
an_unsafe_query_makes_the_program_invalid(void)
{
	relic_t *ctx;
	diag_log_t log;

	ctx = context_with("e(a).\ne(X), Y < X?\n", &log);
	if (ctx == NULL)
		return;

	check(relic_check(ctx) != 0, SCENARIO_UNSAFE,
	    "the check fails");
	checkf(log.errors == 1, SCENARIO_UNSAFE,
	    "one error should be reported, %zu were", log.errors);
	check(strstr(relic_error(ctx), "variable Y in the query") != NULL,
	    SCENARIO_UNSAFE, "the error names the variable and the query");

	relic_destroy(ctx);
}

/*
 * Preparing a query must not change the program, so each way of getting one
 * wrong is refused with one error and leaves the program as valid as it was.
 */
static void
a_refused_query_leaves_the_program_valid(void)
{
	static const char *const refusals[] = {
		"",
		"e(a).",
		"e(X) :- e(X).",
		"e(X)? e(a)?",
		"e(X",
		"X = 1?",
		"e(X), not e(Y)?",
		"e(_), _ < 1?",
		"w(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, "
		    "A14, A15, A16, A17, A18, A19, A20, A21, A22, A23, A24, "
		    "A25, A26, A27, A28, A29, A30, A31, A32), e(A33)?"
	};
	relic_t *ctx;
	diag_log_t log;
	size_t before;
	size_t i;

	ctx = context_with("e(a).\n", &log);
	if (ctx == NULL)
		return;

	for (i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
		before = log.errors;
		checkf(relic_query_prepare(ctx, NULL, refusals[i]) == NULL,
		    SCENARIO_REFUSED, "query %zu should be refused", i);
		checkf(log.errors == before + 1, SCENARIO_REFUSED,
		    "query %zu should draw one error, it drew %zu", i,
		    log.errors - before);
		checkf(relic_check(ctx) == 0, SCENARIO_REFUSED,
		    "the program should stay valid after query %zu", i);
	}

	check(relic_query_prepare(ctx, NULL, NULL) == NULL, SCENARIO_REFUSED,
	    "no text is refused");

	/*
	 * A refused query has still named its predicates, and one nothing
	 * defines is most likely the typo behind the refusal.
	 */
	before = log.warnings;
	check(relic_query_prepare(ctx, NULL, "ee(X), X < Y?") == NULL,
	    SCENARIO_REFUSED, "the query naming ee/1 is refused");
	checkf(log.warnings == before + 1, SCENARIO_REFUSED,
	    "ee/1 should draw one warning, it drew %zu",
	    log.warnings - before);

	relic_destroy(ctx);
}

/*
 * A query naming a predicate nothing defines is prepared, and the typo it
 * most likely is gets the same warning a rule's would.
 */
static void
a_query_warns_of_an_undefined_predicate(void)
{
	relic_t *ctx;
	relic_query_t *query;
	diag_log_t log;
	size_t rows;

	ctx = context_with("e(a).\n", &log);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, "ee(X)?");
	check(query != NULL, SCENARIO_UNDEFINED, "the query is prepared");
	checkf(log.warnings == 1, SCENARIO_UNDEFINED,
	    "one warning should be reported, %zu were", log.warnings);

	rows = 0;
	check(query != NULL &&
	    relic_query_run(ctx, query, count_rows, &rows) == 0 && rows == 0,
	    SCENARIO_UNDEFINED, "the query answers no rows");

	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * A callback that returns nonzero receives no further rows, and the run still
 * reports success: the caller asked to stop and nothing failed.
 */
static void
a_callback_can_stop_the_rows(void)
{
	relic_t *ctx;
	relic_query_t *query;
	diag_log_t log;
	size_t rows;

	ctx = context_with("e(a).\ne(b).\ne(c).\n", &log);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, "e(X)?");
	rows = 0;
	check(query != NULL &&
	    relic_query_run(ctx, query, stop_after_one, &rows) == 0,
	    SCENARIO_STOP, "the run succeeds");
	checkf(rows == 1, SCENARIO_STOP,
	    "one row should be delivered, %zu were", rows);

	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * A query's predicates and constants are identifiers of the context it was
 * prepared in, so running it in another is refused rather than answered with
 * whatever those identifiers mean there.
 */
static void
a_query_runs_only_in_its_own_context(void)
{
	relic_t *ctx;
	relic_t *other;
	relic_query_t *query;
	diag_log_t log;
	diag_log_t other_log;
	size_t rows;

	ctx = context_with("e(a).\n", &log);
	other = context_with("e(b).\n", &other_log);
	if (ctx == NULL || other == NULL) {
		relic_destroy(ctx);
		relic_destroy(other);
		return;
	}

	query = relic_query_prepare(ctx, NULL, "e(X)?");
	rows = 0;
	check(query != NULL &&
	    relic_query_run(other, query, count_rows, &rows) != 0 &&
	    rows == 0, SCENARIO_FOREIGN, "the run is refused");
	checkf(other_log.errors == 1, SCENARIO_FOREIGN,
	    "one error should be reported, %zu were", other_log.errors);
	check(relic_query_run(ctx, query, NULL, NULL) != 0, SCENARIO_FOREIGN,
	    "a run with no callback is refused");

	relic_query_destroy(query);
	relic_destroy(ctx);
	relic_destroy(other);
}

int
main(void)
{
	columns_are_the_named_variables();
	answers_are_named_in_the_order_queries_compile();
	answering_queries_does_not_grow_the_context();
	an_answer_refused_by_a_limit_is_given_back();
	the_programs_queries_are_enumerated();
	an_unsafe_query_makes_the_program_invalid();
	a_refused_query_leaves_the_program_valid();
	a_query_warns_of_an_undefined_predicate();
	a_callback_can_stop_the_rows();
	a_query_runs_only_in_its_own_context();

	return (check_summary("test_query"));
}
