/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for relic_interrupt(), written as an embedder writes them:
 * through <relic.h> alone, linked against the shared library.
 *
 * An interrupt is made from caller code the operation runs -- the explain
 * callback, which runs as each component is planned, before it is
 * evaluated, and the row callback -- so that it lands at a chosen point,
 * and from another thread, as a deadline would, over an evaluation that
 * would otherwise not end.  Each scenario checks that the operation fails
 * with the interrupt's diagnostic, and that the context answers afterwards
 * as a fresh one does.  The scenarios that interrupt from a callback run
 * serially and again with every round shared among threads, where a worker
 * finds the interrupt and the calling thread reports it.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifdef RELIC_THREADS
#include <pthread.h>
#endif

#include <relic.h>

#include "harness.h"

#define	SCENARIO_BETWEEN	"an interrupt between operations is forgotten"
#define	SCENARIO_COLLECT	"an interrupted collection returns no result"
#define	SCENARIO_DEADLINE	"another thread stops an evaluation"
#define	SCENARIO_GATHERED	"a gathered answer stops between rows"
#define	SCENARIO_MATERIALIZE	"an interrupted materialization fails"
#define	SCENARIO_NULL		"interrupting no context does nothing"
#define	SCENARIO_QUERY		"an interrupted query fails"
#define	SCENARIO_STOPPED	"a callback that stops the rows succeeds"
#define	SCENARIO_STREAMED	"a streamed answer stops after its rows"

/*
 * A chain of CHAIN_LENGTH edges and its transitive closure: path(X, Y)
 * holds for CHAIN_PATHS pairs, more than a batch, so that a stream stopped
 * after its first row stops short of the whole answer.
 */
#define	CHAIN_LENGTH	100
#define	CHAIN_PATHS	(CHAIN_LENGTH * (CHAIN_LENGTH + 1) / 2)

/*
 * A join of two relations of CROSS_SIDE facts each whose comparison holds
 * for no pair: CROSS_SIDE squared bindings, each rejected, and nothing
 * derived, which no evaluation finishes in the time a test has.
 */
#define	CROSS_SIDE	100000

typedef struct deadline deadline_t;
typedef struct interrupter interrupter_t;

/*
 * The thread the deadline scenario interrupts its context from, started
 * from inside the run: whether it was started, or could not be.
 */
struct deadline {
	relic_t		*ctx;
#ifdef RELIC_THREADS
	pthread_t	 thread;
#endif
	bool		 started;
	bool		 failed;
};

/*
 * Caller code that interrupts its context: from the explain callback on
 * the `after`th line of a plan, or from the row callback on the `after`th
 * row.  `lines` and `rows` count what the callbacks saw, and `last_error`
 * is the text of the last error reported.
 */
struct interrupter {
	relic_t		*ctx;
	size_t		 after;
	size_t		 lines;
	size_t		 rows;
	char		 last_error[256];
};

static relic_t	*chain_context(unsigned int flags, size_t threads,
		    interrupter_t *interrupter);
static int	 count_rows(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static bool	 error_says(const interrupter_t *interrupter,
		    const char *expected);
static void	 interrupt_from_explain(void *callback_ctx, const char *line);
static int	 interrupt_from_row(void *callback_ctx,
		    const relic_value_t *values, size_t count);
static int	 interrupt_and_stop(void *callback_ctx,
		    const relic_value_t *values, size_t count);
static void	 a_callback_that_stops_the_rows_succeeds(unsigned int flags);
static void	 record_error(void *callback_ctx, enum relic_diag_level level,
		    const relic_location_t *where, const char *message);
static size_t	 answer_rows(relic_t *ctx, const char *query_text);
static void	 a_gathered_answer_stops_between_rows(unsigned int flags,
		    size_t threads);
static void	 a_streamed_answer_stops_after_its_rows(void);
static void	 an_interrupt_between_operations_is_forgotten(void);
static void	 an_interrupted_collection_returns_no_result(void);
static void	 an_interrupted_materialization_fails(unsigned int flags,
		    size_t threads);
static void	 an_interrupted_query_fails(unsigned int flags,
		    size_t threads);
static void	 another_thread_stops_an_evaluation(void);
static void	 interrupting_no_context_does_nothing(void);

static void
record_error(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_TEST_UNUSED, const char *message)
{
	interrupter_t *interrupter;

	interrupter = callback_ctx;
	if (level == RELIC_ERROR)
		strlcpy(interrupter->last_error, message,
		    sizeof(interrupter->last_error));
}

static void
interrupt_from_explain(void *callback_ctx, const char *line RELIC_TEST_UNUSED)
{
	interrupter_t *interrupter;

	interrupter = callback_ctx;
	interrupter->lines++;
	if (interrupter->lines == interrupter->after)
		relic_interrupt(interrupter->ctx);
}

static int
interrupt_from_row(void *callback_ctx,
    const relic_value_t *values RELIC_TEST_UNUSED,
    size_t count RELIC_TEST_UNUSED)
{
	interrupter_t *interrupter;

	interrupter = callback_ctx;
	interrupter->rows++;
	if (interrupter->rows == interrupter->after)
		relic_interrupt(interrupter->ctx);

	return (0);
}

/*
 * A row callback that interrupts its context and asks for no more rows on
 * the first row.
 */
static int
interrupt_and_stop(void *callback_ctx,
    const relic_value_t *values RELIC_TEST_UNUSED,
    size_t count RELIC_TEST_UNUSED)
{
	interrupter_t *interrupter;

	interrupter = callback_ctx;
	interrupter->rows++;
	relic_interrupt(interrupter->ctx);

	return (1);
}

static int
count_rows(void *callback_ctx, const relic_value_t *values RELIC_TEST_UNUSED,
    size_t count RELIC_TEST_UNUSED)
{
	size_t *rows;

	rows = callback_ctx;
	(*rows)++;

	return (0);
}

/*
 * Whether the last error the context reported is `expected`.
 */
static bool
error_says(const interrupter_t *interrupter, const char *expected)
{
	return (strcmp(interrupter->last_error, expected) == 0);
}

/*
 * A context holding the chain and its closure, created with `flags` and
 * `threads`, its diagnostics recorded in `interrupter`, which the caller
 * then points at the callback it interrupts from.  Returns NULL having
 * complained.
 */
static relic_t *
chain_context(unsigned int flags, size_t threads, interrupter_t *interrupter)
{
	relic_config_t config;
	relic_t *ctx;
	char fact[64];
	int i;

	bzero(&config, sizeof(config));
	config.flags = flags;
	config.threads = threads;

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, "creating a context", "a context is created");
		return (NULL);
	}

	bzero(interrupter, sizeof(*interrupter));
	interrupter->ctx = ctx;
	relic_diag_set(ctx, record_error, interrupter);

	if (relic_load_string(ctx, "chain.dl",
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- path(X, Y), edge(Y, Z).\n") != 0) {
		check(false, "loading the chain", "the rules load");
		relic_destroy(ctx);
		return (NULL);
	}
	for (i = 0; i < CHAIN_LENGTH; i++) {
		snprintf(fact, sizeof(fact), "edge(%d, %d).\n", i, i + 1);
		if (relic_load_string(ctx, "chain.dl", fact) != 0) {
			check(false, "loading the chain", "the edges load");
			relic_destroy(ctx);
			return (NULL);
		}
	}

	return (ctx);
}

/*
 * The number of rows a query answers, or SIZE_MAX when it fails.
 */
static size_t
answer_rows(relic_t *ctx, const char *query_text)
{
	relic_query_t *query;
	size_t rows;
	int status;

	query = relic_query_prepare(ctx, NULL, query_text);
	if (query == NULL)
		return (SIZE_MAX);

	rows = 0;
	status = relic_query_run(ctx, query, count_rows, &rows);
	relic_query_destroy(query);

	return (status == 0 ? rows : SIZE_MAX);
}

/*
 * An interrupt made as the closure's component is planned stops the
 * materialization before it derives a tuple of it; the next one, with
 * nothing asking it to stop, derives the closure whole.
 */
static void
an_interrupted_materialization_fails(unsigned int flags, size_t threads)
{
	interrupter_t interrupter;
	relic_t *ctx;

	ctx = chain_context(flags, threads, &interrupter);
	if (ctx == NULL)
		return;

	interrupter.after = 1;
	relic_explain_set(ctx, interrupt_from_explain, &interrupter);

	checkf(relic_materialize(ctx) == -1 &&
	    error_says(&interrupter, "interrupted while deriving path/2"),
	    SCENARIO_MATERIALIZE, "at %zu threads the materialization should "
	    "fail as interrupted, not with \"%s\"", threads,
	    interrupter.last_error);
	check(relic_error(ctx) != NULL &&
	    strcmp(relic_error(ctx), interrupter.last_error) == 0 &&
	    relic_error_class(ctx) == RELIC_ERROR_INTERRUPTED,
	    SCENARIO_MATERIALIZE,
	    "relic_error() and relic_error_class() give the interrupt");

	relic_explain_set(ctx, NULL, NULL);
	check(relic_materialize(ctx) == 0, SCENARIO_MATERIALIZE,
	    "the next materialization succeeds");
	checkf(answer_rows(ctx, "path(X, Y)?") == CHAIN_PATHS,
	    SCENARIO_MATERIALIZE, "at %zu threads the closure should hold "
	    "%d paths", threads, CHAIN_PATHS);

	relic_destroy(ctx);
}

/*
 * A query interrupted as its rewrite's components are planned fails and
 * delivers nothing; run again, it answers in full.
 */
static void
an_interrupted_query_fails(unsigned int flags, size_t threads)
{
	interrupter_t interrupter;
	relic_query_t *query;
	relic_t *ctx;
	size_t rows;

	ctx = chain_context(flags, threads, &interrupter);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, "path(0, Y)?");
	if (query == NULL) {
		check(false, SCENARIO_QUERY, "the query is prepared");
		relic_destroy(ctx);
		return;
	}

	interrupter.after = 1;
	relic_explain_set(ctx, interrupt_from_explain, &interrupter);
	rows = 0;
	checkf(relic_query_run(ctx, query, count_rows, &rows) == -1 &&
	    rows == 0 && relic_error_class(ctx) == RELIC_ERROR_INTERRUPTED,
	    SCENARIO_QUERY, "at %zu threads the query should fail as "
	    "interrupted with no rows, not %zu rows and \"%s\"", threads,
	    rows, interrupter.last_error);

	relic_explain_set(ctx, NULL, NULL);
	rows = 0;
	checkf(relic_query_run(ctx, query, count_rows, &rows) == 0 &&
	    rows == CHAIN_LENGTH, SCENARIO_QUERY, "at %zu threads the query "
	    "run again should answer %d rows, not %zu", threads, CHAIN_LENGTH,
	    rows);

	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * An interrupt made while no operation runs asks nothing to stop: every
 * operation forgets an interrupt made before it began.
 */
static void
an_interrupt_between_operations_is_forgotten(void)
{
	interrupter_t interrupter;
	relic_t *ctx;

	ctx = chain_context(0, 0, &interrupter);
	if (ctx == NULL)
		return;

	relic_interrupt(ctx);
	check(relic_materialize(ctx) == 0, SCENARIO_BETWEEN,
	    "a materialization after an interrupt succeeds");

	relic_interrupt(ctx);
	check(answer_rows(ctx, "path(X, Y)?") == CHAIN_PATHS,
	    SCENARIO_BETWEEN, "a query after an interrupt answers in full");

	relic_destroy(ctx);
}

/*
 * A query that streams, interrupted from its row callback, delivers the
 * rest of the batch in hand and then fails: the rows it delivered are a
 * prefix of the answer, at least the one that interrupted it and short of
 * the whole.  Run again, it answers in full.
 */
static void
a_streamed_answer_stops_after_its_rows(void)
{
	interrupter_t interrupter;
	relic_query_t *query;
	relic_t *ctx;

	ctx = chain_context(0, 0, &interrupter);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, "edge(X, Y), path(Y, Z)?");
	if (query == NULL || relic_materialize(ctx) != 0) {
		check(false, SCENARIO_STREAMED, "the program materializes");
		relic_query_destroy(query);
		relic_destroy(ctx);
		return;
	}

	interrupter.after = 1;
	checkf(relic_query_run(ctx, query, interrupt_from_row,
	    &interrupter) == -1 && interrupter.rows >= 1 &&
	    interrupter.rows < CHAIN_PATHS - CHAIN_LENGTH &&
	    error_says(&interrupter, "interrupted while deriving the query"),
	    SCENARIO_STREAMED, "the stream should fail as interrupted after "
	    "part of the answer, not after %zu rows with \"%s\"",
	    interrupter.rows, interrupter.last_error);

	interrupter.rows = 0;
	interrupter.after = 0;
	check(relic_query_run(ctx, query, interrupt_from_row,
	    &interrupter) == 0 &&
	    interrupter.rows == CHAIN_PATHS - CHAIN_LENGTH, SCENARIO_STREAMED,
	    "the query run again answers in full");

	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * A gathered answer, interrupted from its row callback, is looked at
 * before each row, so the delivery stops after the row that interrupted
 * it.  RELIC_F_NO_STREAM gathers every answer.
 */
static void
a_gathered_answer_stops_between_rows(unsigned int flags, size_t threads)
{
	interrupter_t interrupter;
	relic_query_t *query;
	relic_t *ctx;

	ctx = chain_context(flags | RELIC_F_NO_STREAM, threads, &interrupter);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, "path(X, Y)?");
	if (query == NULL) {
		check(false, SCENARIO_GATHERED, "the query is prepared");
		relic_destroy(ctx);
		return;
	}

	interrupter.after = 3;
	checkf(relic_query_run(ctx, query, interrupt_from_row,
	    &interrupter) == -1 && interrupter.rows == 3 &&
	    error_says(&interrupter, "interrupted while delivering the query"),
	    SCENARIO_GATHERED, "at %zu threads the delivery should stop after "
	    "the third row, not after %zu with \"%s\"", threads,
	    interrupter.rows, interrupter.last_error);

	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * relic_query_collect() is a run whose callback copies the rows; an
 * interrupt fails it, and it returns no result rather than the rows copied
 * so far.
 */
static void
an_interrupted_collection_returns_no_result(void)
{
	interrupter_t interrupter;
	relic_result_t *result;
	relic_query_t *query;
	relic_t *ctx;

	ctx = chain_context(0, 0, &interrupter);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, "path(X, Y)?");
	if (query == NULL) {
		check(false, SCENARIO_COLLECT, "the query is prepared");
		relic_destroy(ctx);
		return;
	}

	interrupter.after = 1;
	relic_explain_set(ctx, interrupt_from_explain, &interrupter);
	result = relic_query_collect(ctx, query);
	checkf(result == NULL &&
	    error_says(&interrupter, "interrupted while deriving path/2"),
	    SCENARIO_COLLECT, "the collection should fail as interrupted, not "
	    "with \"%s\"", interrupter.last_error);

	relic_result_destroy(result);
	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * A callback that asks for no more rows has had the answer it wanted, so
 * the run succeeds even when the callback interrupted it as well, whether
 * the rows stream or are gathered first.
 */
static void
a_callback_that_stops_the_rows_succeeds(unsigned int flags)
{
	interrupter_t interrupter;
	relic_query_t *query;
	relic_t *ctx;

	ctx = chain_context(flags, 0, &interrupter);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, "edge(X, Y), path(Y, Z)?");
	if (query == NULL || relic_materialize(ctx) != 0) {
		check(false, SCENARIO_STOPPED, "the program materializes");
		relic_query_destroy(query);
		relic_destroy(ctx);
		return;
	}

	checkf(relic_query_run(ctx, query, interrupt_and_stop,
	    &interrupter) == 0 && interrupter.rows == 1, SCENARIO_STOPPED,
	    "%s, the run should succeed after one row, not after %zu with "
	    "\"%s\"", (flags & RELIC_F_NO_STREAM) != 0 ? "gathered" :
	    "streamed", interrupter.rows, interrupter.last_error);

	relic_query_destroy(query);
	relic_destroy(ctx);
}

static void
interrupting_no_context_does_nothing(void)
{
	relic_interrupt(NULL);
	check(true, SCENARIO_NULL, "relic_interrupt(NULL) returns");
}

#ifdef RELIC_THREADS
/*
 * The thread that stands for a deadline: it waits a little, so that the
 * evaluation is under way, and interrupts the context.
 */
static void *
interrupt_later(void *argument)
{
	usleep(50000);
	relic_interrupt(argument);

	return (NULL);
}

/*
 * The explain callback of the deadline scenario: on the first line of a
 * plan, which comes while the query runs, it starts the thread that
 * interrupts it.  Started any earlier, the thread could interrupt before
 * the run began, and the run would forget the interrupt.  A system that
 * will not start the thread has the run interrupted from here, so that the
 * scenario ends and is reported as skipped.
 */
static void
start_deadline(void *callback_ctx, const char *line RELIC_TEST_UNUSED)
{
	deadline_t *deadline;

	deadline = callback_ctx;
	if (deadline->started || deadline->failed)
		return;

	if (pthread_create(&deadline->thread, NULL, interrupt_later,
	    deadline->ctx) == 0) {
		deadline->started = true;
		return;
	}

	/* With no thread, the run is ended here so that it ends at all. */
	deadline->failed = true;
	relic_interrupt(deadline->ctx);
}
#endif

/*
 * A join that rejects every binding derives nothing, so it never fills a
 * batch; the executor still returns every quantum, and an interrupt from
 * another thread stops it.  Without the quantum this scenario would not
 * end.
 */
static void
another_thread_stops_an_evaluation(void)
{
#ifdef RELIC_THREADS
	interrupter_t interrupter;
	deadline_t deadline;
	relic_query_t *query;
	relic_t *ctx;
	char fact[64];
	size_t rows;
	int i;

	ctx = relic_create();
	if (ctx == NULL) {
		check(false, SCENARIO_DEADLINE, "a context is created");
		return;
	}
	bzero(&interrupter, sizeof(interrupter));
	relic_diag_set(ctx, record_error, &interrupter);

	for (i = 0; i < CROSS_SIDE; i++) {
		snprintf(fact, sizeof(fact), "a(%d).\nb(%d).\n", i, i);
		if (relic_load_string(ctx, "cross.dl", fact) != 0) {
			check(false, SCENARIO_DEADLINE, "the facts load");
			relic_destroy(ctx);
			return;
		}
	}

	query = relic_query_prepare(ctx, NULL, "a(X), b(Y), X > Y + "
	    "1000000?");
	if (query == NULL) {
		check(false, SCENARIO_DEADLINE, "the query is prepared");
		relic_destroy(ctx);
		return;
	}

	bzero(&deadline, sizeof(deadline));
	deadline.ctx = ctx;
	relic_explain_set(ctx, start_deadline, &deadline);

	rows = 0;
	if (relic_query_run(ctx, query, count_rows, &rows) == -1 &&
	    deadline.failed) {
		printf("skip: %s: no thread to interrupt from\n",
		    SCENARIO_DEADLINE);
	} else {
		checkf(deadline.started && rows == 0 && error_says(&interrupter,
		    "interrupted while deriving the query"), SCENARIO_DEADLINE,
		    "the query should fail as interrupted, not with %zu rows "
		    "and \"%s\"", rows, interrupter.last_error);
	}

	if (deadline.started)
		(void)pthread_join(deadline.thread, NULL);
	relic_query_destroy(query);
	relic_destroy(ctx);
#else
	printf("skip: %s: a build without threads\n", SCENARIO_DEADLINE);
#endif
}

int
main(void)
{
	an_interrupted_materialization_fails(0, 0);
	an_interrupted_query_fails(0, 0);
	a_gathered_answer_stops_between_rows(0, 0);

	/*
	 * Again with every round shared among four threads: a worker finds
	 * the interrupt and abandons the round, and the calling thread
	 * reports it from the round's serial run, as it would at one thread.
	 */
	an_interrupted_materialization_fails(RELIC_F_PARALLEL_ALWAYS, 4);
	an_interrupted_query_fails(RELIC_F_PARALLEL_ALWAYS, 4);
	a_gathered_answer_stops_between_rows(RELIC_F_PARALLEL_ALWAYS, 4);

	an_interrupt_between_operations_is_forgotten();
	a_streamed_answer_stops_after_its_rows();
	a_callback_that_stops_the_rows_succeeds(0);
	a_callback_that_stops_the_rows_succeeds(RELIC_F_NO_STREAM);
	an_interrupted_collection_returns_no_result();
	interrupting_no_context_does_nothing();
	another_thread_stops_an_evaluation();

	return (check_summary("test_interrupt"));
}
