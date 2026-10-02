/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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

#define	SCENARIO_COLUMNS	"a query's columns are its named variables"
#define	SCENARIO_FACTS		"a fact costs its tuple and comes first"
#define	SCENARIO_FOREIGN	"a query runs only in its own context"
#define	SCENARIO_GROUPS		"a query's groups refused by a limit report once"
#define	SCENARIO_GROWTH		"answering queries does not grow the context"
#define	SCENARIO_LIMIT		"an answer refused by a limit is given back"
#define	SCENARIO_LOADED		"the program's queries are enumerated"
#define	SCENARIO_NAMES		"answers are named in the order queries compile"
#define	SCENARIO_PLANNING	"planning is charged to the memory limit"
#define	SCENARIO_PREPARING	"preparing queries stays within the memory limit"
#define	SCENARIO_REFUSED	"a refused query leaves the program valid"
#define	SCENARIO_STOP		"a callback can stop the rows"
#define	SCENARIO_STREAMED	"a streamed answer holds nothing"
#define	SCENARIO_UNDEFINED	"a query warns of a predicate nothing defines"
#define	SCENARIO_UNSAFE		"an unsafe query makes the program invalid"

/* How many times the growth test prepares, runs and destroys its query. */
#define	GROWTH_ROUNDS		64

/*
 * The memory limit of the planning scenario, which admits every relation
 * and ordinary plan the scenario's programs have, and the lengths of the
 * bodies whose plans it does not admit: a recursive rule's 201 plans of 201
 * operations, and a query's one plan of 4000.
 */
#define	PLANNING_LIMIT		65536
#define	PLANNING_RECURSIVE	200
#define	PREPARING_LIMIT		65536
#define	PREPARING_MOST		1000000
#define	PLANNING_QUERY		4000

/*
 * A limit that admits the first chunk of one charged arena and not of two:
 * a chunk is a little over 4 KB.
 */
#define	FIRST_WANTED_LIMIT	6000

/*
 * How many facts the fact scenario loads, and how many bytes past what their
 * relation holds its memory limit allows: room for an evaluation's
 * per-component and per-predicate arrays, and not for a plan per fact.
 */
#define	FACTS_LOADED		20000
#define	FACTS_MARGIN		16384
#define	FACT_TEXT_MAX		16	/* the longest "e(N).\n" line */

/*
 * How many facts the memory case of the limit scenario loads, and the limit
 * it runs under: the facts and the plan fit, and the four-way product of the
 * facts -- 160000 rows of four columns -- does not.
 */
#define	LIMITED_FACTS		20
#define	LIMITED_MEMORY		1048576

/*
 * How many rows the stop scenario's query answers: three whole batches and a
 * few rows more, so that a stop can fall inside any whole batch, on the
 * boundary between two, or among the rows of the partial batch the plan
 * delivers when its join ends.
 */
#define	STOP_ROWS		(3 * RELIC_INSERT_BATCH + 3)

/* Room for one line of explain output about a small query. */
#define	EXPLAIN_TEXT_MAX	128

typedef struct context_size context_size_t;
typedef struct diag_log diag_log_t;
typedef struct explained explained_t;
typedef struct integer_rows integer_rows_t;
typedef struct stopping_rows stopping_rows_t;

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

/* The first column of each row a query delivered, when it is an integer. */
struct integer_rows {
	int64_t	values[8];
	size_t	count;
};

/*
 * The rows a callback asks for before it stops them, how many it was given,
 * and whether each was the integer that is its position among them.
 */
struct stopping_rows {
	size_t	wanted;
	size_t	delivered;
	bool	in_order;
};

/*
 * The lines of explain output that name a query's answer, last seen of each:
 * the heading that writes the query's rule, and what the EMIT of its plan
 * names, whether the plan writes that relation or streams its rows.
 */
struct explained {
	char	query[EXPLAIN_TEXT_MAX];
	char	emitted[EXPLAIN_TEXT_MAX];
};

static bool	 answer_once(relic_t *ctx, const char *text);
static bool	 answer_is_named(relic_t *ctx, relic_query_t *query,
		    explained_t *explained, const char *name);
static context_size_t context_size(relic_t *ctx);
static void	 growth_under(unsigned int flags, const char *strategy);
static relic_t	*context_with(const char *program, diag_log_t *log);
static relic_t	*limited_context(size_t allowance, const char *program,
		    diag_log_t *log);
static bool	 ends_with(const char *text, const char *suffix);
static char	*numbered_facts(size_t count);
static char	*repeated(const char *first, const char *each, size_t count,
		    const char *last);
static bool	 refused_while(relic_t *ctx, const char *activity);
static int	 count_rows(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static int	 keep_integers(void *callback_ctx,
		    const relic_value_t *values, size_t count);
static void	 a_fact_costs_its_tuple_and_comes_first(void);
static void	 record_explained(void *callback_ctx, const char *line);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static int	 stop_when_wanted(void *callback_ctx,
		    const relic_value_t *values, size_t count);
static void	 a_callback_can_stop_the_rows(void);
static void	 stopped_under(unsigned int flags, const char *strategy);
static void	 a_streamed_answer_holds_nothing(void);
static void	 streamed_under(size_t max_tuples, size_t max_memory,
		    const char *limits);
static void	 refused_under(size_t max_tuples, size_t max_memory,
		    const char *program, const char *text,
		    const char *expected);
static void	 a_query_runs_only_in_its_own_context(void);
static void	 a_query_warns_of_an_undefined_predicate(void);
static void	 a_refused_query_leaves_the_program_valid(void);
static void	 an_answer_refused_by_a_limit_is_given_back(void);
static void	 planning_is_charged_to_the_memory_limit(void);
static void	 preparing_queries_stays_within_the_memory_limit(void);
static void	 a_querys_groups_refused_by_a_limit_report_once(void);
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

/*
 * Count a row, noting whether it is the one integer that is its position
 * among the rows, and ask for no more rows once as many as wanted are
 * counted.
 */
static int
stop_when_wanted(void *callback_ctx, const relic_value_t *values,
    size_t count)
{
	stopping_rows_t *rows;

	rows = callback_ctx;
	if (count != 1 || values[0].type != RELIC_INTEGER ||
	    values[0].v.integer != (int64_t)rows->delivered)
		rows->in_order = false;
	rows->delivered++;

	return (rows->delivered >= rows->wanted ? 1 : 0);
}

/*
 * Keep the explain lines that name a query's answer, leading spaces removed,
 * and of an EMIT's line only the relation it names.
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
		strlcpy(explained->emitted, line + 5,
		    sizeof(explained->emitted));
	else if (strncmp(line, "stream ", 7) == 0)
		strlcpy(explained->emitted, line + 7,
		    sizeof(explained->emitted));
}

/*
 * Whether answering `query` explains it under `name`: the query's rule is
 * headed by the name and its plan's EMIT names the relation of that name.
 * The queries of this suite's naming test have no columns, so the name is
 * the whole of the head.
 */
static bool
answer_is_named(relic_t *ctx, relic_query_t *query, explained_t *explained,
    const char *name)
{
	char heading[EXPLAIN_TEXT_MAX];
	size_t rows;

	bzero(explained, sizeof(*explained));
	rows = 0;
	if (query == NULL ||
	    relic_query_run(ctx, query, count_rows, &rows) != 0)
		return (false);

	snprintf(heading, sizeof(heading), "query %s :- ", name);

	return (strncmp(explained->query, heading, strlen(heading)) == 0 &&
	    strcmp(explained->emitted, name) == 0);
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
 * of the query's rule and as the relation its plan's EMIT names.
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
	refused = relic_query_prepare(ctx, NULL, "X > 1?");
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
	size.relation_slots = ctx->store.derived.count +
	    ctx->store.bases.count;
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
 * A limit bounds every answer.  A query whose answer outgrows the context's
 * tuple or memory limit fails without delivering a row, and the relations
 * hold what they held before, whether its rows must be gathered first or
 * could be delivered as they are derived.
 */
static void
an_answer_refused_by_a_limit_is_given_back(void)
{
	char *program;

	refused_under(4, 0, "e(a).\ne(b).\ne(c).\n", "e(X), e(_)?",
	    "tuples=4 while deriving the query");
	refused_under(4, 0, "e(a).\ne(b).\ne(c).\n", "e(X), e(Y)?",
	    "tuples=4 while deriving the query");

	program = numbered_facts(LIMITED_FACTS);
	if (program == NULL) {
		check(false, SCENARIO_LIMIT, "the program is written");
		return;
	}
	refused_under(0, LIMITED_MEMORY, program, "e(A), e(B), e(C), e(D)?",
	    "memory=1048576 while deriving the query");
	free(program);
}

/*
 * Answer `text` over `program` in a context with the given limits, and
 * confirm that a limit refuses the answer with a diagnostic ending in
 * `expected`, that no row was delivered, and that the relations, the answer
 * among them, hold what they held before.  The query is destroyed after its
 * context, which reads nothing of the context, and leaves nothing for
 * AddressSanitizer to report.
 */
static void
refused_under(size_t max_tuples, size_t max_memory, const char *program,
    const char *text, const char *expected)
{
	relic_config_t config;
	relic_query_t *query;
	relic_stats_t before;
	relic_stats_t after;
	const char *reported;
	relic_t *ctx;
	diag_log_t log;
	size_t rows;

	bzero(&config, sizeof(config));
	config.max_tuples = max_tuples;
	config.max_memory = max_memory;
	bzero(&log, sizeof(log));

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, SCENARIO_LIMIT, "a context is created");
		return;
	}
	relic_diag_set(ctx, record_diagnostic, &log);

	query = NULL;
	if (relic_load_string(ctx, "test.dl", program) == 0 &&
	    relic_materialize(ctx) == 0)
		query = relic_query_prepare(ctx, NULL, text);
	relic_stats(ctx, &before);

	rows = 0;
	checkf(query != NULL &&
	    relic_query_run(ctx, query, count_rows, &rows) != 0 && rows == 0,
	    SCENARIO_LIMIT, "%s should fail without delivering a row, and "
	    "%zu were delivered", text, rows);
	reported = relic_error(ctx);
	checkf(reported != NULL && ends_with(reported, expected), SCENARIO_LIMIT,
	    "the diagnostic should end \"%s\", not read \"%s\"", expected,
	    reported == NULL ? "(none)" : reported);

	relic_stats(ctx, &after);
	checkf(after.tuples == before.tuples, SCENARIO_LIMIT,
	    "the relations should hold their %zu tuples, not %zu",
	    before.tuples, after.tuples);
	checkf(after.storage_bytes == before.storage_bytes, SCENARIO_LIMIT,
	    "the relations should hold their %zu bytes, not %zu",
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
		"X > 1?",
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
 * reports success: the caller asked to stop and nothing failed.  The rows
 * delivered are exactly those before the stop, wherever it falls among the
 * batches the rows are derived in, whether the query streams its rows as it
 * derives them or gathers its answer first.  A streamed query also stops
 * deriving: its join ends with the batch the stop fell in, rather than
 * running to completion and dropping what it derives.
 */
static void
a_callback_can_stop_the_rows(void)
{
	stopped_under(0, "streamed");
	stopped_under(RELIC_F_NO_STREAM, "gathered");
}

/*
 * Stop e(X)? over STOP_ROWS facts in a context created with `flags`, which
 * the checks name as answered by `strategy`: inside the first batch, at its
 * last row, just after it, just after the second, and inside the partial
 * batch at the end.  The query's plan scans e in the order of its facts, so
 * the rows delivered are the integers from 0 up to the stop.
 *
 * A streamed run derives its rows a batch at a time and delivers each batch
 * before deriving the next, so the scan has read no further than the end of
 * the batch the stop fell in.  A gathered run reads every fact before it
 * delivers a row.
 */
static void
stopped_under(unsigned int flags, const char *strategy)
{
	static const size_t stops[] = {
		2, RELIC_INSERT_BATCH, RELIC_INSERT_BATCH + 1,
		2 * RELIC_INSERT_BATCH + 1, 3 * RELIC_INSERT_BATCH + 2
	};
	relic_config_t config;
	stopping_rows_t rows;
	relic_query_t *query;
	relic_stats_t before;
	relic_stats_t after;
	relic_t *ctx;
	char *program;
	diag_log_t log;
	size_t i;
	bool streams;

	bzero(&config, sizeof(config));
	config.flags = flags;
	bzero(&log, sizeof(log));

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, SCENARIO_STOP, "a context is created");
		return;
	}
	relic_diag_set(ctx, record_diagnostic, &log);

	query = NULL;
	program = numbered_facts(STOP_ROWS);
	if (program != NULL && relic_load_string(ctx, "test.dl", program) == 0)
		query = relic_query_prepare(ctx, NULL, "e(X)?");
	free(program);
	streams = (flags & RELIC_F_NO_STREAM) == 0;

	for (i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
		size_t batches;
		size_t expected_scanned;
		uint64_t scanned;

		bzero(&rows, sizeof(rows));
		rows.wanted = stops[i];
		rows.in_order = true;

		relic_stats(ctx, &before);
		checkf(query != NULL &&
		    relic_query_run(ctx, query, stop_when_wanted, &rows) == 0,
		    SCENARIO_STOP, "the %s run succeeds", strategy);
		relic_stats(ctx, &after);

		checkf(rows.delivered == rows.wanted && rows.in_order,
		    SCENARIO_STOP, "the %s rows should be the integers below "
		    "%zu, and %zu were delivered%s", strategy, rows.wanted,
		    rows.delivered, rows.in_order ? "" : " out of order");

		batches = (stops[i] + RELIC_INSERT_BATCH - 1) /
		    RELIC_INSERT_BATCH;
		expected_scanned = streams ? batches * RELIC_INSERT_BATCH :
		    STOP_ROWS;
		if (expected_scanned > STOP_ROWS)
			expected_scanned = STOP_ROWS;
		scanned = after.tuples_scanned - before.tuples_scanned;
		checkf(scanned == expected_scanned, SCENARIO_STOP,
		    "the %s run stopping at %zu should read %zu facts, not "
		    "%" PRIu64, strategy, stops[i], expected_scanned, scanned);
	}

	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * A query whose plan derives each of its rows once, in a context that sets
 * no limit, delivers its rows as it derives them, and its answer relation is
 * never written: the relations hold what they held, and each row is counted
 * as a derivation but not as an inserted tuple.  A limit of SIZE_MAX sets
 * no limit, as zero does, and leaves the query streaming.
 */
static void
a_streamed_answer_holds_nothing(void)
{
	streamed_under(0, 0, "no limit");
	streamed_under(SIZE_MAX, SIZE_MAX, "limits of SIZE_MAX");
}

/*
 * Answer e(X)? over three facts in a context with the given limits, which
 * the checks name as `limits`, and confirm that the answer was streamed.
 */
static void
streamed_under(size_t max_tuples, size_t max_memory, const char *limits)
{
	relic_config_t config;
	relic_query_t *query;
	relic_stats_t before;
	relic_stats_t after;
	relic_t *ctx;
	diag_log_t log;
	size_t rows;

	bzero(&config, sizeof(config));
	config.max_tuples = max_tuples;
	config.max_memory = max_memory;
	bzero(&log, sizeof(log));

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, SCENARIO_STREAMED, "a context is created");
		return;
	}
	relic_diag_set(ctx, record_diagnostic, &log);

	query = NULL;
	if (relic_load_string(ctx, "test.dl", "e(a).\ne(b).\ne(c).\n") == 0 &&
	    relic_materialize(ctx) == 0)
		query = relic_query_prepare(ctx, NULL, "e(X)?");
	relic_stats(ctx, &before);

	rows = 0;
	checkf(query != NULL &&
	    relic_query_run(ctx, query, count_rows, &rows) == 0,
	    SCENARIO_STREAMED, "the query succeeds under %s", limits);
	checkf(rows == 3, SCENARIO_STREAMED,
	    "three rows should be delivered under %s, %zu were", limits, rows);

	relic_stats(ctx, &after);
	checkf(after.tuples == before.tuples &&
	    after.tuples_inserted == before.tuples_inserted, SCENARIO_STREAMED,
	    "nothing should be inserted under %s, %" PRIu64 " tuples were",
	    limits, after.tuples_inserted - before.tuples_inserted);
	checkf(after.derivations == before.derivations + 3, SCENARIO_STREAMED,
	    "each row should be counted as a derivation under %s, %" PRIu64
	    " were", limits, after.derivations - before.derivations);
	check(query != NULL && relic_relation_count(query->answer) == 0 &&
	    relic_relation_bytes(query->answer) == 0, SCENARIO_STREAMED,
	    "the answer holds nothing");

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

/*
 * A context holding a program, under a memory limit of `allowance` bytes
 * past what its values -- the integer table and the symbol pool, which its
 * names and constants fill -- hold once it is loaded.  The limit is set
 * after the load, so that it measures what the scenarios exercise and not
 * the few kilobytes a pool holding a handful of names takes, which a
 * scenario's small limit would otherwise be spent on.
 */
static relic_t *
limited_context(size_t allowance, const char *program, diag_log_t *log)
{
	relic_t *ctx;

	bzero(log, sizeof(*log));

	ctx = relic_create();
	if (ctx == NULL) {
		check(false, SCENARIO_PLANNING, "a context is created");
		return (NULL);
	}
	relic_diag_set(ctx, record_diagnostic, log);

	if (relic_load_string(ctx, "test.dl", program) != 0) {
		check(false, SCENARIO_PLANNING, "the program loads");
		relic_destroy(ctx);
		return (NULL);
	}
	ctx->account.limit = relic_value_storage_bytes(ctx) + allowance;

	return (ctx);
}

/*
 * Whether `text` ends with `suffix`.
 */
static bool
ends_with(const char *text, const char *suffix)
{
	size_t text_length;
	size_t suffix_length;

	text_length = strlen(text);
	suffix_length = strlen(suffix);

	return (text_length >= suffix_length &&
	    strcmp(text + text_length - suffix_length, suffix) == 0);
}

/*
 * The facts e(0) to e(count - 1), one to a line, in a string the caller
 * frees, or NULL when there was no memory for it.
 */
static char *
numbered_facts(size_t count)
{
	char *program;
	size_t written;
	size_t i;

	program = malloc(count * FACT_TEXT_MAX + 1);
	if (program == NULL)
		return (NULL);

	written = 0;
	for (i = 0; i < count; i++)
		written += (size_t)snprintf(program + written, FACT_TEXT_MAX + 1,
		    "e(%zu).\n", i);
	program[written] = '\0';

	return (program);
}

/*
 * `first`, then `count` copies of `each`, then `last`, in a string the
 * caller frees, or NULL when there was no memory for it.
 */
static char *
repeated(const char *first, const char *each, size_t count, const char *last)
{
	size_t length;
	size_t i;
	char *text;
	char *end;

	length = strlen(first) + count * strlen(each) + strlen(last) + 1;
	text = malloc(length);
	if (text == NULL)
		return (NULL);

	end = stpcpy(text, first);
	for (i = 0; i < count; i++)
		end = stpcpy(end, each);
	stpcpy(end, last);

	return (text);
}

/*
 * Whether the last error is the memory limit's, refusing the named activity,
 * with nothing left charged to the account by the evaluation arena or a
 * rewrite's once the operation has returned.
 */
static bool
refused_while(relic_t *ctx, const char *activity)
{
	char expected[128];
	const char *reported;

	snprintf(expected, sizeof(expected),
	    "limit exceeded: memory=%zu while %s", ctx->account.limit, activity);
	reported = relic_error(ctx);

	checkf(reported != NULL && strcmp(reported, expected) == 0,
	    SCENARIO_PLANNING, "the diagnostic should read \"%s\", not \"%s\"",
	    expected, reported == NULL ? "(none)" : reported);

	return (reported != NULL && strcmp(reported, expected) == 0 &&
	    ctx->account.arena_bytes == 0);
}

/*
 * Preparing a query interns its constants, and destroying it gives none of
 * them back, since the tables are never shrunk; so a caller preparing
 * queries with distinct constants grows the tables with each one.  Each
 * query here holds a new integer and a new string, so that both the
 * integer table and the pool grow.  The limit bounds both whole, and must
 * refuse a preparation before the total passes it, naming the limit and the
 * query being prepared, however many preparations come first.
 */
static void
preparing_queries_stays_within_the_memory_limit(void)
{
	char expected[96];
	char text[64];
	relic_query_t *query;
	const char *reported;
	diag_log_t log;
	relic_t *ctx;
	bool within;
	long n;

	ctx = limited_context(PREPARING_LIMIT, "p(0).\n", &log);
	if (ctx == NULL)
		return;

	within = true;
	query = NULL;
	for (n = 1; n <= PREPARING_MOST; n++) {
		snprintf(text, sizeof(text), "p(%ld), p(\"s%ld\")?", n, n);
		query = relic_query_prepare(ctx, NULL, text);
		if (relic_account_exceeds(&ctx->account, 0, 0))
			within = false;
		if (query == NULL)
			break;
		relic_query_destroy(query);
	}

	checkf(query == NULL, SCENARIO_PREPARING,
	    "a preparation should be refused once the tables fill the limit, "
	    "and %ld were prepared", n - 1);
	checkf(within, SCENARIO_PREPARING,
	    "the account should stay within %zu bytes, and the tables hold %zu",
	    ctx->account.limit, relic_value_storage_bytes(ctx));

	snprintf(expected, sizeof(expected),
	    "limit exceeded: memory=%zu while preparing <query>",
	    ctx->account.limit);
	reported = relic_error(ctx);
	checkf(reported != NULL && strcmp(reported, expected) == 0,
	    SCENARIO_PREPARING, "the diagnostic should read \"%s\", not \"%s\"",
	    expected, reported == NULL ? "(none)" : reported);

	relic_destroy(ctx);
}

/*
 * The evaluation arena and the arena of a query's rewrite are charged to the
 * memory limit, so a limit that admits every tuple a program derives still
 * refuses plans that do not fit, before any is built, and names what was
 * being planned: a component by its first member, a query by its answer.  A
 * rewrite refused by the limit fails the query rather than being abandoned
 * for the whole program as a budget would be.  Under a limit that admits
 * them, the same programs are answered, and every operation gives back what
 * its arenas held.
 */
static void
planning_is_charged_to_the_memory_limit(void)
{
	relic_query_t *query;
	diag_log_t log;
	relic_t *ctx;
	char *program;
	char *text;
	size_t rows;

	program = repeated("p(a).\np(X) :- p(X)", ", p(X)",
	    PLANNING_RECURSIVE - 1, ".\n");
	text = repeated("e(X)", ", e(X)", PLANNING_QUERY - 1, "?");
	if (program == NULL || text == NULL) {
		check(false, SCENARIO_PLANNING, "the programs are written");
		free(program);
		free(text);
		return;
	}

	ctx = limited_context(PLANNING_LIMIT, program, &log);
	if (ctx != NULL) {
		check(relic_materialize(ctx) != 0 &&
		    refused_while(ctx, "planning p/1"),
		    SCENARIO_PLANNING, "a long recursive body's variants are "
		    "refused while planning its component");
		relic_destroy(ctx);
	}

	ctx = limited_context(PLANNING_LIMIT, "e(a).\n", &log);
	if (ctx != NULL) {
		query = relic_materialize(ctx) == 0 ?
		    relic_query_prepare(ctx, NULL, text) : NULL;
		rows = 0;
		check(query != NULL &&
		    relic_query_run(ctx, query, count_rows, &rows) != 0 &&
		    rows == 0 &&
		    refused_while(ctx, "planning the query"),
		    SCENARIO_PLANNING, "a long query's plan is refused while "
		    "planning the query, the program's having fitted");
		relic_query_destroy(query);
		relic_destroy(ctx);
	}

	/*
	 * A limit a byte past the values once the query is prepared: e/1 and
	 * f/1 derive each other and nothing else, and the rewrite's first
	 * chunk does not fit.  The limit is lowered after the preparation,
	 * which interns the query's constant and would itself be refused a
	 * byte past the program's values.
	 */
	ctx = limited_context(PLANNING_LIMIT, "e(X) :- f(X).\nf(X) :- e(X).\n"
	    "p(X) :- e(X).\n", &log);
	if (ctx != NULL) {
		query = relic_query_prepare(ctx, NULL, "p(a)?");
		ctx->account.limit = relic_value_storage_bytes(ctx) + 1;
		rows = 0;
		check(query != NULL &&
		    relic_query_run(ctx, query, count_rows, &rows) != 0 &&
		    rows == 0 && log.warnings == 0 &&
		    refused_while(ctx, "rewriting the query"), SCENARIO_PLANNING,
		    "a rewrite refused by the limit fails the query");
		relic_query_destroy(query);
		relic_destroy(ctx);
	}

	/*
	 * The rewrite's first chunk fits under this limit and the evaluation
	 * arena's does not, so the evaluation of what the query reads
	 * completely is refused, and names the first component it evaluates:
	 * e/1, which the query negates, and which is the schedule's second
	 * component, not the first the schedule holds.
	 */
	ctx = limited_context(FIRST_WANTED_LIMIT,
	    "b(1).\na(X) :- b(X).\nf(1).\ne(X) :- f(X).\ng(1).\n"
	    "p(X) :- g(X), not e(X).\n", &log);
	if (ctx != NULL) {
		query = relic_query_prepare(ctx, NULL, "p(1)?");
		rows = 0;
		check(query != NULL &&
		    relic_query_run(ctx, query, count_rows, &rows) != 0 &&
		    rows == 0 &&
		    refused_while(ctx, "planning e/1"),
		    SCENARIO_PLANNING, "an evaluation for a rewrite names the "
		    "first component it evaluates");
		relic_query_destroy(query);
		relic_destroy(ctx);
	}

	ctx = limited_context(PLANNING_LIMIT, "e(a).\np(X) :- e(X).\n", &log);
	if (ctx != NULL) {
		query = relic_query_prepare(ctx, NULL, "p(a)?");
		rows = 0;
		check(query != NULL &&
		    relic_query_run(ctx, query, count_rows, &rows) == 0 &&
		    rows == 1 && ctx->account.arena_bytes == 0 &&
		    ctx->account.store_bytes == relic_relation_total_bytes(ctx),
		    SCENARIO_PLANNING, "a rewrite the limit admits answers, and "
		    "gives back what its arenas held");
		relic_query_destroy(query);
		relic_destroy(ctx);
	}

	free(program);
	free(text);
}

static int
keep_integers(void *callback_ctx, const relic_value_t *values, size_t count)
{
	integer_rows_t *rows;

	rows = callback_ctx;
	if (count > 0 && values[0].type == RELIC_INTEGER &&
	    rows->count < sizeof(rows->values) / sizeof(rows->values[0])) {
		rows->values[rows->count] = values[0].v.integer;
		rows->count++;
	}

	return (0);
}

/*
 * A fact is a tuple of its predicate's base store and has no plan.  So a
 * program of many facts evaluates under a memory limit a little above what
 * their relation and the integers they name hold, which a plan per fact --
 * several times the size of its tuple -- would exceed.  A derived predicate's relation is seeded with
 * all its facts before its rules run, so its rows begin with its facts, in
 * the order they were written, wherever they stand among its rules.
 */
static void
a_fact_costs_its_tuple_and_comes_first(void)
{
	integer_rows_t rows;
	relic_query_t *query;
	relic_stats_t stats;
	diag_log_t log;
	relic_t *ctx;
	char *program;
	size_t data_bytes;

	program = numbered_facts(FACTS_LOADED);
	if (program == NULL) {
		check(false, SCENARIO_FACTS, "the program is written");
		return;
	}

	data_bytes = 0;
	ctx = context_with(program, &log);
	if (ctx != NULL && relic_materialize(ctx) == 0) {
		relic_stats(ctx, &stats);
		data_bytes = stats.storage_bytes;
	}
	relic_destroy(ctx);
	check(data_bytes > 0, SCENARIO_FACTS, "the facts evaluate");

	ctx = limited_context(data_bytes + FACTS_MARGIN, program, &log);
	if (ctx != NULL) {
		checkf(relic_materialize(ctx) == 0 &&
		    relic_relation_total_tuples(ctx) == FACTS_LOADED,
		    SCENARIO_FACTS, "%d facts should evaluate within %zu bytes "
		    "of their storage's %zu: %s", FACTS_LOADED,
		    (size_t)FACTS_MARGIN, data_bytes,
		    relic_error(ctx) != NULL ? relic_error(ctx) : "");
		relic_destroy(ctx);
	}
	free(program);

	ctx = context_with("e(1).\np(3).\np(X) :- e(X).\np(5).\n", &log);
	if (ctx == NULL)
		return;

	bzero(&rows, sizeof(rows));
	query = relic_query_prepare(ctx, NULL, "p(X)?");
	check(query != NULL &&
	    relic_query_run(ctx, query, keep_integers, &rows) == 0 &&
	    rows.count == 3 && rows.values[0] == 3 && rows.values[1] == 5 &&
	    rows.values[2] == 1, SCENARIO_FACTS,
	    "a derived predicate's facts should come before what its rule "
	    "derives");
	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * A query holding many aggregates runs its group rules in an arena charged
 * to the memory limit.  Under limits from one that refuses the first of its
 * allocations to one that admits them all, each a sixty-fourth above the
 * last, the query either
 * delivers its one row or fails with exactly one error, and gives back
 * what it charged: the refusal is the one diagnostic, and nothing the
 * arena's failure then refuses reports again.  The sweep must reach a query
 * that was prepared and then refused while it ran, where the group rules
 * run, and one that is answered.
 */
static void
a_querys_groups_refused_by_a_limit_report_once(void)
{
	relic_query_t *query;
	diag_log_t log;
	relic_t *ctx;
	char text[1024];
	size_t length;
	size_t limit;
	size_t evaluating;
	size_t answered;
	size_t rows;
	int status;
	int i;

	length = 0;
	for (i = 0; i < 31; i++)
		length += (size_t)snprintf(text + length, sizeof(text) - length,
		    "%sN%d = count { p(_) }", i == 0 ? "" : ", ", i);
	snprintf(text + length, sizeof(text) - length, "?");

	evaluating = 0;
	answered = 0;
	for (limit = 4096; limit <= 1024 * 1024; limit += limit / 64) {
		ctx = limited_context(limit, "p(a).\n", &log);
		if (ctx == NULL)
			return;

		rows = 0;
		status = -1;
		query = relic_query_prepare(ctx, NULL, text);
		if (query != NULL)
			status = relic_query_run(ctx, query, count_rows, &rows);
		if (status != 0 && query != NULL)
			evaluating++;
		if (status == 0 && rows == 1)
			answered++;

		checkf(log.errors == (status != 0 ? 1u : 0u) &&
		    ctx->account.arena_bytes == 0, SCENARIO_GROUPS,
		    "under a memory limit %zu past the values, a query that %s "
		    "should report %d error(s), not %zu, and give back its "
		    "arenas",
		    limit, status != 0 ? "failed" : "succeeded",
		    status != 0 ? 1 : 0, log.errors);

		relic_query_destroy(query);
		relic_destroy(ctx);
	}

	checkf(evaluating > 0 && answered > 0, SCENARIO_GROUPS, "some limit "
	    "should refuse the prepared query while it runs, not %zu, and some "
	    "admit its one row, not %zu", evaluating, answered);
}

int
main(void)
{
	columns_are_the_named_variables();
	answers_are_named_in_the_order_queries_compile();
	answering_queries_does_not_grow_the_context();
	an_answer_refused_by_a_limit_is_given_back();
	planning_is_charged_to_the_memory_limit();
	preparing_queries_stays_within_the_memory_limit();
	a_querys_groups_refused_by_a_limit_report_once();
	a_fact_costs_its_tuple_and_comes_first();
	the_programs_queries_are_enumerated();
	an_unsafe_query_makes_the_program_invalid();
	a_refused_query_leaves_the_program_valid();
	a_query_warns_of_an_undefined_predicate();
	a_callback_can_stop_the_rows();
	a_streamed_answer_holds_nothing();
	a_query_runs_only_in_its_own_context();

	return (check_summary("test_query"));
}
