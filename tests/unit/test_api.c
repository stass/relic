/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the library interface, written as an embedder writes
 * them: through <relic.h> alone, linked against the shared library with its
 * hidden visibility and export list, so that every entry point used here is
 * one a caller can reach.
 *
 * The scenarios are the interface's promises that no golden test can see:
 * caller code that calls back into its context is refused and changes
 * nothing, a refusal does not hide why an operation failed, an answer does
 * not outlive its delivery, every diagnostic reaches the callback and none
 * reaches standard error, a value is built only from bytes its type can
 * hold, a fact added through the interface is the fact its text would load,
 * a collected result is the stream, kept, a mixed-type comparison is noted
 * once per rule and only when asked for, a missing argument is reported
 * rather than followed, and a prepared query runs over the program as it
 * stands when it runs.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifdef RELIC_THREADS
#include <pthread.h>
#endif

#include <relic.h>

#include "harness.h"

#define	SCENARIO_ARGUMENTS	"a missing argument is reported, not followed"
#define	SCENARIO_CLASSES	"every diagnostic reaches the callback"
#define	SCENARIO_COLLECT	"a collected result is the stream, kept"
#define	SCENARIO_FACT		"an added fact is the fact its text loads"
#define	SCENARIO_INVALIDATE	"an added fact invalidates what was derived"
#define	SCENARIO_LATER_LOAD \
	"a prepared query runs over the program as it stands"
#define	SCENARIO_MIXED		"a mixed comparison is noted once per rule"
#define	SCENARIO_REENTRY	"a callback cannot call into its context"
#define	SCENARIO_REFUSAL \
	"a refusal does not hide why an operation failed"
#define	SCENARIO_THREADS \
	"a context takes at most RELIC_THREADS_MAX threads"
#define	SCENARIO_TRANSIENT	"an answer does not outlive its delivery"
#define	SCENARIO_VALUES		"a value holds only what its type can"

/* The largest answer answer_text() renders, in rows and bytes per row. */
#define	ANSWER_ROWS	16
#define	ANSWER_WIDTH	128

/* The most notes a mixed_log_t keeps the line of. */
#define	MIXED_NOTES_MAX	8

/* What every note about a mixed-type comparison begins with. */
#define	MIXED_NOTE	"ordering comparison between "

/* The text relic_error() holds after a refused re-entry. */
#define	REENTRY_ERROR	"the context was called from one of its own callbacks"

/*
 * The program every re-entry scenario starts from: b(X)? answers x and y
 * for as long as nothing adds a c fact, and c/1 being defined by nothing
 * makes a check warn.
 */
#define	REENTRY_PROGRAM	"a(x).\na(y).\nb(X) :- a(X), not c(X).\nb(X)?\n"

typedef struct diag_log diag_log_t;
typedef struct first_error first_error_t;
typedef struct mixed_log mixed_log_t;
typedef struct reentry reentry_t;

/* How many diagnostics a context reported, by level. */
struct diag_log {
	size_t	notes;
	size_t	warnings;
	size_t	errors;
};

/*
 * How many errors a context reported, and where the first of them pointed:
 * the source name, cut to fit, and the line.  `file` is empty when the first
 * error pointed at no source.
 */
struct first_error {
	char		file[64];
	uint32_t	line;
	size_t		errors;
};

/*
 * The notes about mixed-type comparisons a context reported: how many, and
 * the line each of the first MIXED_NOTES_MAX pointed at.  `other` counts
 * every other diagnostic, and any delivered on a thread other than the one
 * running main(), which a scenario expects to be none.
 */
struct mixed_log {
	uint32_t	lines[MIXED_NOTES_MAX];
	size_t		notes;
	size_t		other;
};

/*
 * The flags and threads the mixed-comparison scenarios' contexts are given
 * besides their own, which main() sets to run them a second time under the
 * pool, and the thread running main(), the one every diagnostic must reach
 * the callback on.
 */
static unsigned int mixed_flags;
static size_t mixed_threads;

#ifdef RELIC_THREADS
static pthread_t main_thread;
#endif

/*
 * What caller code that breaks the contract works with: the context, the
 * query it may try to run or destroy, a value it may try to add as a fact,
 * and what it saw.  `from` names the kind of caller code, for messages.
 * `calls` counts how often that code ran; it re-enters on the first call
 * only.  `text` and `offset` are the program a stream's read function
 * serves.
 */
struct reentry {
	relic_t		*ctx;
	relic_query_t	*query;
	diag_log_t	 log;
	relic_value_t	 value;
	const char	*from;
	const char	*text;
	size_t		 offset;
	size_t		 calls;
	size_t		 refused;
	size_t		 attempted;
	size_t		 errors_seen;
};

static relic_t	*context_with(const char *program,
		    const relic_config_t *config, diag_log_t *log);
static int	 count_rows(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static void	 record_first_error(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static void	 check_argument_refused(relic_t *ctx,
		    const first_error_t *first, int status, const char *call,
		    const char *expected_error);
static bool	 refused(reentry_t *reentry, int status, const char *call);
static bool	 refused_object(reentry_t *reentry, const void *object,
		    const char *call);
static void	 reenter_every_way(reentry_t *reentry);
static bool	 reentry_begin(reentry_t *reentry, const char *from);
static void	 reentry_end(reentry_t *reentry);
static int	 reenter_from_row(void *callback_ctx,
		    const relic_value_t *values, size_t count);
static void	 reenter_from_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static void	 reenter_from_explain(void *callback_ctx, const char *line);
static size_t	 reenter_from_read(reentry_t *reentry, char *buffer,
		    size_t size);
static FILE	*reentering_stream(reentry_t *reentry);
static int	 refuse_from_row(void *callback_ctx,
		    const relic_value_t *values, size_t count);
static void	 refuse_from_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static void	 write_row(relic_t *ctx, const relic_value_t *row,
		    size_t columns, char *line);
static char	*answer_text(relic_t *ctx, const char *query_text);
static int	 compare_lines(const void *left, const void *right);
static void	 a_callback_cannot_call_into_its_context(void);
static void	 a_collected_result_is_the_stream_kept(void);
static void	 a_missing_argument_is_reported_not_followed(void);
static void	 a_prepared_query_runs_over_the_program_as_it_stands(void);
static void	 a_refusal_does_not_hide_why_an_operation_failed(void);
static void	 a_value_holds_only_what_its_type_can(void);
static void	 an_added_fact_invalidates_what_was_derived(void);
static void	 an_added_fact_is_the_fact_its_text_loads(void);
static void	 an_answer_does_not_outlive_its_delivery(void);
static void	 every_diagnostic_reaches_the_callback(void);
static void	 a_context_takes_at_most_the_maximum_threads(void);
static void	 a_mixed_comparison_is_noted_once_per_rule(void);
static void	 a_mixed_note_follows_the_rule_not_its_place(void);
static void	 a_query_and_its_rewrite_share_one_note(void);
static bool	 noted_line(const mixed_log_t *log, uint32_t line);
static void	 record_mixed_note(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);

static void
record_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_TEST_UNUSED,
    const char *message RELIC_TEST_UNUSED)
{
	diag_log_t *log;

	log = callback_ctx;
	if (level == RELIC_NOTE)
		log->notes++;
	else if (level == RELIC_WARNING)
		log->warnings++;
	else
		log->errors++;
}

static void
record_first_error(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message RELIC_TEST_UNUSED)
{
	first_error_t *first;

	first = callback_ctx;
	if (level != RELIC_ERROR)
		return;

	first->errors++;
	if (first->errors > 1 || where == NULL || where->file == NULL)
		return;

	snprintf(first->file, sizeof(first->file), "%s", where->file);
	first->line = where->line;
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
 * A context holding `program`, created with `config` or the defaults when it
 * is NULL, reporting its diagnostics to `log`.
 */
static relic_t *
context_with(const char *program, const relic_config_t *config,
    diag_log_t *log)
{
	relic_t *ctx;

	bzero(log, sizeof(*log));

	ctx = config != NULL ? relic_create_config(config) : relic_create();
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
 * Record one re-entering call that returns a status: it must fail, leave the
 * re-entry refusal as the context's last error, and say nothing through the
 * diagnostic callback, which is the caller already running or might be.
 */
static bool
refused(reentry_t *reentry, int status, const char *call)
{
	const char *error;
	bool ok;

	reentry->attempted++;
	error = relic_error(reentry->ctx);
	ok = status == -1 && error != NULL &&
	    strcmp(error, REENTRY_ERROR) == 0 &&
	    reentry->log.errors == reentry->errors_seen;
	checkf(ok, SCENARIO_REENTRY,
	    "%s from %s should be refused with \"%s\", not return %d with "
	    "\"%s\" and %zu errors reported", call, reentry->from,
	    REENTRY_ERROR, status, error != NULL ? error : "(none)",
	    reentry->log.errors - reentry->errors_seen);
	if (ok)
		reentry->refused++;

	return (ok);
}

/* The same, for a call that returns an object and fails with NULL. */
static bool
refused_object(reentry_t *reentry, const void *object, const char *call)
{
	return (refused(reentry, object == NULL ? -1 : 0, call));
}

/*
 * Call every entry point that changes a context, from caller code the
 * context is running.  Each must be refused.  The calls that return nothing
 * are refused by doing nothing: the diagnostic callback stays installed and
 * the context stays alive, which reentry_end() confirms.  The calls that
 * only read are allowed.
 */
static void
reenter_every_way(reentry_t *reentry)
{
	static const enum relic_column inner_columns[] = {
		RELIC_COLUMN_SYMBOL
	};
	static const relic_import_t inner_import = {
		.predicate = "c",
		.columns = inner_columns,
		.column_count = 1,
		.format = RELIC_FORMAT_TSV,
	};
	relic_stats_t stats;
	relic_value_t symbol;
	size_t inner_rows;
	FILE *stream;

	reentry->errors_seen = reentry->log.errors;
	inner_rows = 0;

	refused(reentry, relic_load_string(reentry->ctx, "inner.dl",
	    "c(x).\nc(y).\n"), "relic_load_string()");
	stream = tmpfile();
	if (stream != NULL) {
		fputs("c(x).\n", stream);
		rewind(stream);
		refused(reentry, relic_load_file(reentry->ctx, "inner.dl",
		    stream), "relic_load_file()");
		fclose(stream);
	}
	refused(reentry, relic_load_path(reentry->ctx, "/dev/null"),
	    "relic_load_path()");
	stream = tmpfile();
	if (stream != NULL) {
		fputs("x\n", stream);
		rewind(stream);
		refused(reentry, relic_import_file(reentry->ctx, "inner.tsv",
		    stream, &inner_import), "relic_import_file()");
		fclose(stream);
	}
	refused(reentry, relic_import_path(reentry->ctx, "/dev/null",
	    &inner_import), "relic_import_path()");
	refused(reentry, relic_check(reentry->ctx), "relic_check()");
	refused(reentry, relic_materialize(reentry->ctx),
	    "relic_materialize()");
	refused_object(reentry, relic_query_prepare(reentry->ctx, NULL,
	    "a(X)?"), "relic_query_prepare()");
	refused_object(reentry, relic_query_loaded(reentry->ctx, 0),
	    "relic_query_loaded()");
	refused(reentry, relic_query_run(reentry->ctx, reentry->query,
	    count_rows, &inner_rows), "relic_query_run()");
	refused_object(reentry, relic_query_collect(reentry->ctx,
	    reentry->query), "relic_query_collect()");
	refused(reentry, relic_fact_add(reentry->ctx, "c", 1, &reentry->value),
	    "relic_fact_add()");
	refused(reentry, relic_value_symbol(reentry->ctx, "z", 1, &symbol),
	    "relic_value_symbol()");
	refused(reentry, relic_value_string(reentry->ctx, "z", 1, &symbol),
	    "relic_value_string()");

	relic_diag_set(reentry->ctx, NULL, NULL);
	relic_explain_set(reentry->ctx, NULL, NULL);
	relic_destroy(reentry->ctx);

	relic_stats(reentry->ctx, &stats);
	checkf(relic_string(reentry->ctx, reentry->value.v.id, NULL) != NULL &&
	    relic_query_count(reentry->ctx) == 1, SCENARIO_REENTRY,
	    "%s can read strings and count queries", reentry->from);
}

/*
 * A context holding REENTRY_PROGRAM, with a value to try adding as a fact.
 * The program answers b(X)? with two rows as long as nothing is added to
 * it, which is how reentry_end() sees that every refused call changed
 * nothing.
 */
static bool
reentry_begin(reentry_t *reentry, const char *from)
{
	bzero(reentry, sizeof(*reentry));
	reentry->from = from;

	reentry->ctx = context_with(REENTRY_PROGRAM, NULL, &reentry->log);
	if (reentry->ctx == NULL)
		return (false);

	if (relic_value_symbol(reentry->ctx, "x", 1, &reentry->value) != 0) {
		check(false, SCENARIO_REENTRY, "a value is built");
		relic_destroy(reentry->ctx);
		return (false);
	}

	return (true);
}

/*
 * Confirm that every call was refused and that the context is as it was:
 * the query answers its two rows, and the diagnostic callback the caller
 * code tried to remove still hears about a load that fails.  Then release
 * everything, which the refused destroys did not.
 */
static void
reentry_end(reentry_t *reentry)
{
	size_t errors;
	size_t rows;

	checkf(reentry->attempted > 0 &&
	    reentry->refused == reentry->attempted, SCENARIO_REENTRY,
	    "every call from %s should be refused, but %zu of %zu were",
	    reentry->from, reentry->refused, reentry->attempted);

	if (reentry->query == NULL)
		reentry->query = relic_query_prepare(reentry->ctx, NULL,
		    "b(X)?");
	rows = 0;
	checkf(reentry->query != NULL && relic_query_run(reentry->ctx,
	    reentry->query, count_rows, &rows) == 0 && rows == 2,
	    SCENARIO_REENTRY, "after the calls from %s b(X)? should still "
	    "answer 2 rows, not %zu", reentry->from, rows);

	errors = reentry->log.errors;
	checkf(relic_load_string(reentry->ctx, "bad.dl", "p(X) :- .\n") != 0 &&
	    reentry->log.errors > errors, SCENARIO_REENTRY,
	    "the diagnostic callback %s tried to remove should still be "
	    "installed", reentry->from);

	relic_query_destroy(reentry->query);
	relic_destroy(reentry->ctx);
}

/*
 * A row callback re-entering on the first row.  It also tries to destroy the
 * query being answered, which must be refused like the rest.
 */
static int
reenter_from_row(void *callback_ctx,
    const relic_value_t *values RELIC_TEST_UNUSED,
    size_t count RELIC_TEST_UNUSED)
{
	reentry_t *reentry;

	reentry = callback_ctx;
	reentry->calls++;
	if (reentry->calls > 1)
		return (0);

	reenter_every_way(reentry);
	relic_query_destroy(reentry->query);

	return (0);
}

/* A diagnostic callback re-entering on the first diagnostic. */
static void
reenter_from_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	reentry_t *reentry;

	reentry = callback_ctx;
	record_diagnostic(&reentry->log, level, where, message);
	reentry->calls++;
	if (reentry->calls == 1)
		reenter_every_way(reentry);
}

/* An explain callback re-entering on the first line. */
static void
reenter_from_explain(void *callback_ctx, const char *line RELIC_TEST_UNUSED)
{
	reentry_t *reentry;

	reentry = callback_ctx;
	reentry->calls++;
	if (reentry->calls == 1)
		reenter_every_way(reentry);
}

/*
 * A stream's read function re-entering on its first read, then serving
 * `text` in pieces of at most `size` bytes.
 */
static size_t
reenter_from_read(reentry_t *reentry, char *buffer, size_t size)
{
	size_t remaining;

	reentry->calls++;
	if (reentry->calls == 1)
		reenter_every_way(reentry);

	remaining = strlen(reentry->text) - reentry->offset;
	if (size > remaining)
		size = remaining;
	bcopy(reentry->text + reentry->offset, buffer, size);
	reentry->offset += size;

	return (size);
}

#if defined(HAVE_FUNOPEN)
static int
funopen_read(void *cookie, char *buffer, int size)
{
	return ((int)reenter_from_read(cookie, buffer, (size_t)size));
}

/* A stream whose reads run reenter_from_read(), or NULL. */
static FILE *
reentering_stream(reentry_t *reentry)
{
	return (funopen(reentry, funopen_read, NULL, NULL, NULL));
}
#elif defined(HAVE_FOPENCOOKIE)
static ssize_t
cookie_read(void *cookie, char *buffer, size_t size)
{
	return ((ssize_t)reenter_from_read(cookie, buffer, size));
}

/* A stream whose reads run reenter_from_read(), or NULL. */
static FILE *
reentering_stream(reentry_t *reentry)
{
	cookie_io_functions_t functions = { cookie_read, NULL, NULL, NULL };

	return (fopencookie(reentry, "r", functions));
}
#else
/* No way to make a stream run caller code on this platform. */
static FILE *
reentering_stream(reentry_t *reentry RELIC_TEST_UNUSED)
{
	return (NULL);
}
#endif

/*
 * Caller code runs inside an operation in four ways -- a row, diagnostic or
 * explain callback, or the read function of a stream being loaded -- and
 * from each, every call that would change the context is refused while the
 * operation it is inside completes as though nothing had been asked.
 */
static void
a_callback_cannot_call_into_its_context(void)
{
	reentry_t reentry;
	FILE *stream;

	if (reentry_begin(&reentry, "a row callback")) {
		reentry.query = relic_query_prepare(reentry.ctx, NULL, "b(X)?");
		check(reentry.query != NULL && relic_query_run(reentry.ctx,
		    reentry.query, reenter_from_row, &reentry) == 0 &&
		    reentry.calls == 2, SCENARIO_REENTRY,
		    "the stream the row callback is inside delivers both rows");
		reentry_end(&reentry);
	}

	if (reentry_begin(&reentry, "a diagnostic callback")) {
		relic_diag_set(reentry.ctx, reenter_from_diagnostic, &reentry);
		reentry.query = relic_query_prepare(reentry.ctx, NULL, "b(X)?");
		check(reentry.query != NULL && reentry.calls > 0,
		    SCENARIO_REENTRY,
		    "the check that warns about c/1 completes");
		reentry_end(&reentry);
	}

	if (reentry_begin(&reentry, "an explain callback")) {
		relic_explain_set(reentry.ctx, reenter_from_explain, &reentry);
		check(relic_materialize(reentry.ctx) == 0 && reentry.calls > 1,
		    SCENARIO_REENTRY, "the materialization being explained "
		    "completes and is explained in full");
		reentry_end(&reentry);
	}

	if (reentry_begin(&reentry, "a stream's read function")) {
		reentry.text = "d(x).\n";
		stream = reentering_stream(&reentry);
		if (stream == NULL) {
			printf("skip: %s: no stream with a read function of "
			    "the caller's own\n", SCENARIO_REENTRY);
			relic_destroy(reentry.ctx);
			return;
		}
		check(relic_load_file(reentry.ctx, "stream.dl", stream) == 0 &&
		    reentry.calls > 0, SCENARIO_REENTRY,
		    "the load reading the stream completes");
		fclose(stream);
		reentry_end(&reentry);
	}
}

/*
 * A diagnostic callback that tries to materialize on every diagnostic, and
 * counts the refusals.
 */
static void
refuse_from_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	reentry_t *reentry;

	reentry = callback_ctx;
	record_diagnostic(&reentry->log, level, where, message);
	if (relic_materialize(reentry->ctx) == -1)
		reentry->refused++;
}

/* A row callback that tries to materialize on every row. */
static int
refuse_from_row(void *callback_ctx,
    const relic_value_t *values RELIC_TEST_UNUSED,
    size_t count RELIC_TEST_UNUSED)
{
	reentry_t *reentry;

	reentry = callback_ctx;
	if (relic_materialize(reentry->ctx) == -1)
		reentry->refused++;

	return (0);
}

/*
 * A refusal is the caller code's to learn about, and lasts only as long as
 * the operation that code runs inside.  The check behind a query over an
 * unsafe program reports the error, then warnings whose callback has a call
 * refused; once the query has returned, relic_error() must still give the
 * error that explains it.  And a query that succeeds after its row callback
 * had a call refused leaves no error behind.
 */
static void
a_refusal_does_not_hide_why_an_operation_failed(void)
{
	reentry_t reentry;
	relic_query_t *query;
	const char *error;

	bzero(&reentry, sizeof(reentry));
	reentry.ctx = relic_create();
	if (reentry.ctx == NULL) {
		check(false, SCENARIO_REFUSAL, "a context is created");
		return;
	}
	relic_diag_set(reentry.ctx, refuse_from_diagnostic, &reentry);

	check(relic_load_string(reentry.ctx, "unsafe.dl",
	    "p(X) :- q(Y).\n") == 0, SCENARIO_REFUSAL, "the program loads");
	query = relic_query_prepare(reentry.ctx, NULL, "p(X)?");
	error = relic_error(reentry.ctx);
	checkf(query == NULL && reentry.log.errors > 0 &&
	    reentry.log.warnings > 0 && reentry.refused > 0 && error != NULL &&
	    strcmp(error, REENTRY_ERROR) != 0, SCENARIO_REFUSAL,
	    "a query over an unsafe program should fail with the check's "
	    "error, not \"%s\"", error != NULL ? error : "(none)");

	relic_query_destroy(query);
	relic_destroy(reentry.ctx);

	bzero(&reentry, sizeof(reentry));
	reentry.ctx = context_with("a(x).\n", NULL, &reentry.log);
	if (reentry.ctx == NULL)
		return;
	query = relic_query_prepare(reentry.ctx, NULL, "a(X)?");
	check(query != NULL && relic_query_run(reentry.ctx, query,
	    refuse_from_row, &reentry) == 0 && reentry.refused == 1,
	    SCENARIO_REFUSAL, "the query succeeds while its row callback has a "
	    "call refused");
	error = relic_error(reentry.ctx);
	checkf(error == NULL, SCENARIO_REFUSAL,
	    "a query that succeeded should leave no error, not \"%s\"",
	    error != NULL ? error : "");

	relic_query_destroy(query);
	relic_destroy(reentry.ctx);
}

/*
 * Once delivered, an answer is gone: the context holds only what the
 * program derives, so answering a query many times neither grows it nor
 * spends the tuple limit on answers already delivered.
 *
 * Three facts and a limit of six tuples leave room for one answer of three
 * rows at a time; a second answer kept alongside the first would exceed it.
 */
static void
an_answer_does_not_outlive_its_delivery(void)
{
	relic_config_t config;
	relic_stats_t stats;
	relic_query_t *query;
	diag_log_t log;
	relic_t *ctx;
	size_t rows;
	int round;

	bzero(&config, sizeof(config));
	config.max_tuples = 6;

	ctx = context_with("e(a).\ne(b).\ne(c).\n", &config, &log);
	if (ctx == NULL)
		return;

	for (round = 0; round < 3; round++) {
		query = relic_query_prepare(ctx, NULL, "e(X)?");
		rows = 0;
		checkf(query != NULL && relic_query_run(ctx, query,
		    count_rows, &rows) == 0 && rows == 3, SCENARIO_TRANSIENT,
		    "query %d should answer 3 rows under a limit of 6 tuples, "
		    "not %zu, with \"%s\"", round + 1, rows,
		    relic_error(ctx) != NULL ? relic_error(ctx) : "no error");
		relic_query_destroy(query);

		relic_stats(ctx, &stats);
		checkf(stats.tuples == 3, SCENARIO_TRANSIENT,
		    "after query %d the context should hold the 3 facts, not "
		    "%zu tuples", round + 1, stats.tuples);
	}

	relic_destroy(ctx);
}

/*
 * A library must not write to its caller's standard error.  A program with
 * an unsafe rule, a variable named once and a predicate nothing defines
 * reports an error, a warning and a note, all of which must reach the
 * callback and none standard error, which is redirected to a file for the
 * duration and must be empty afterwards.
 */
static void
every_diagnostic_reaches_the_callback(void)
{
	diag_log_t log;
	relic_t *ctx;
	FILE *captured;
	long written;
	int saved;

	captured = tmpfile();
	saved = dup(STDERR_FILENO);
	if (captured == NULL || saved == -1 ||
	    dup2(fileno(captured), STDERR_FILENO) == -1) {
		check(false, SCENARIO_CLASSES, "standard error is redirected");
		return;
	}

	bzero(&log, sizeof(log));
	ctx = relic_create();
	if (ctx != NULL) {
		relic_diag_set(ctx, record_diagnostic, &log);
		(void)relic_load_string(ctx, "classes.dl",
		    "p(X) :- q(X, Y).\nr(X) :- s(Y), q(Y, Y).\n");
		(void)relic_check(ctx);
		relic_destroy(ctx);
	}

	/* A context with no callback says nothing at all. */
	ctx = relic_create();
	if (ctx != NULL) {
		(void)relic_load_string(ctx, "classes.dl",
		    "p(X) :- q(X, Y).\nr(X) :- s(Y), q(Y, Y).\n");
		(void)relic_check(ctx);
		relic_destroy(ctx);
	}

	fflush(stderr);
	written = ftell(captured);
	dup2(saved, STDERR_FILENO);
	close(saved);
	fclose(captured);

	checkf(log.errors > 0 && log.warnings > 0 && log.notes > 0,
	    SCENARIO_CLASSES,
	    "the callback should receive errors, warnings and notes, not %zu, "
	    "%zu and %zu", log.errors, log.warnings, log.notes);
	checkf(written == 0, SCENARIO_CLASSES,
	    "the library should write nothing to standard error, not %ld "
	    "bytes", written);
}

/*
 * A symbol is built only from bytes the language could write as one, and
 * anything else is a string; the same bytes intern to the same identifier
 * either way.  A symbol value filled in by hand with a string's identifier is
 * refused where it enters the program.
 */
static void
a_value_holds_only_what_its_type_can(void)
{
	static const char *const not_symbols[] = {
		"", "Foo", "_foo", "9lives", "a b", "a-b", "caf\xc3\xa9"
	};
	relic_value_t first;
	relic_value_t second;
	relic_value_t forged;
	diag_log_t log;
	relic_t *ctx;
	size_t length;
	size_t i;

	ctx = context_with("", NULL, &log);
	if (ctx == NULL)
		return;

	check(relic_value_symbol(ctx, "foo_Bar9", 8, &first) == 0 &&
	    first.type == RELIC_SYMBOL &&
	    relic_value_symbol(ctx, "foo_Bar9", 8, &second) == 0 &&
	    second.v.id == first.v.id, SCENARIO_VALUES,
	    "foo_Bar9 is a symbol, and interning it twice gives one "
	    "identifier");
	check(relic_value_symbol(ctx, "not", 3, &first) == 0, SCENARIO_VALUES,
	    "not is a symbol value, reserved only as a predicate name");

	for (i = 0; i < sizeof(not_symbols) / sizeof(not_symbols[0]); i++) {
		log.errors = 0;
		checkf(relic_value_symbol(ctx, not_symbols[i],
		    strlen(not_symbols[i]), &first) == -1 && log.errors == 1,
		    SCENARIO_VALUES, "\"%s\" should be refused as a symbol",
		    not_symbols[i]);
		checkf(relic_value_string(ctx, not_symbols[i],
		    strlen(not_symbols[i]), &first) == 0 &&
		    first.type == RELIC_STRING, SCENARIO_VALUES,
		    "\"%s\" should make a string", not_symbols[i]);
	}

	check(relic_value_string(ctx, "a\0b", 3, &first) == 0 &&
	    relic_string(ctx, first.v.id, &length) != NULL && length == 3,
	    SCENARIO_VALUES, "a string keeps a NUL byte and its length");

	check(relic_value_string(ctx, "hello world", 11, &forged) == 0,
	    SCENARIO_VALUES, "a string is built to forge a symbol from");
	forged.type = RELIC_SYMBOL;
	log.errors = 0;
	check(relic_fact_add(ctx, "p", 1, &forged) == -1 && log.errors == 1,
	    SCENARIO_VALUES,
	    "a symbol value holding a string's bytes is refused by a fact");

	relic_destroy(ctx);
}

/*
 * The answer to a query as its rows, sorted, one per line with the values
 * separated by spaces, or NULL when the query failed or its answer is larger
 * than ANSWER_ROWS rows of ANSWER_WIDTH bytes.  Integers are written in
 * decimal and symbols and strings as their bytes.  The caller frees it.
 */
static char *
answer_text(relic_t *ctx, const char *query_text)
{
	char lines[ANSWER_ROWS][ANSWER_WIDTH];
	relic_query_t *query;
	relic_result_t *result;
	const relic_value_t *row;
	char *text;
	size_t rows;
	size_t used;
	size_t i;

	query = relic_query_prepare(ctx, NULL, query_text);
	if (query == NULL)
		return (NULL);
	result = relic_query_collect(ctx, query);
	relic_query_destroy(query);
	if (result == NULL)
		return (NULL);

	rows = relic_result_rows(result);
	if (rows > ANSWER_ROWS) {
		relic_result_destroy(result);
		return (NULL);
	}
	for (i = 0; i < rows; i++) {
		row = relic_result_row(result, i);
		write_row(ctx, row, relic_result_columns(result), lines[i]);
	}
	relic_result_destroy(result);

	qsort(lines, rows, sizeof(lines[0]), compare_lines);

	text = malloc(rows * ANSWER_WIDTH + 1);
	if (text == NULL)
		return (NULL);
	used = 0;
	text[0] = '\0';
	for (i = 0; i < rows; i++)
		used += (size_t)snprintf(text + used,
		    rows * ANSWER_WIDTH + 1 - used, "%s\n", lines[i]);

	return (text);
}

/*
 * One row as its values separated by spaces, in a line of ANSWER_WIDTH
 * bytes; a longer row is cut, which the rows these scenarios compare never
 * are.
 */
static void
write_row(relic_t *ctx, const relic_value_t *row, size_t columns, char *line)
{
	size_t used;
	size_t column;

	line[0] = '\0';
	used = 0;
	for (column = 0; column < columns && used < ANSWER_WIDTH; column++) {
		if (row[column].type == RELIC_INTEGER)
			used += (size_t)snprintf(line + used,
			    ANSWER_WIDTH - used, "%s%lld",
			    column > 0 ? " " : "",
			    (long long)row[column].v.integer);
		else
			used += (size_t)snprintf(line + used,
			    ANSWER_WIDTH - used, "%s%s", column > 0 ? " " : "",
			    relic_string(ctx, row[column].v.id, NULL));
	}
}

static int
compare_lines(const void *left, const void *right)
{
	return (strcmp(left, right));
}

/*
 * A fact added through the interface is the fact its text would load: added
 * facts and loaded text interleave into one program, which answers as the
 * same program written out does.  A refused fact adds nothing.
 */
static void
an_added_fact_is_the_fact_its_text_loads(void)
{
	static const char rules[] =
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n";
	static const char written[] =
	    "edge(a, b).\nedge(b, c).\nweight(a, 7, \"x y\").\nready.\n";
	relic_value_t edge[2];
	relic_value_t weight[3];
	relic_value_t wide[33];
	diag_log_t log;
	relic_t *added;
	relic_t *loaded;
	char *from_added;
	char *from_loaded;
	size_t refusals;

	added = context_with("", NULL, &log);
	loaded = context_with(written, NULL, &log);
	if (added == NULL || loaded == NULL) {
		relic_destroy(added);
		relic_destroy(loaded);
		return;
	}

	check(relic_value_symbol(added, "a", 1, &edge[0]) == 0 &&
	    relic_value_symbol(added, "b", 1, &edge[1]) == 0 &&
	    relic_fact_add(added, "edge", 2, edge) == 0,
	    SCENARIO_FACT, "edge(a, b) is added");
	check(relic_load_string(added, "rules.dl", rules) == 0 &&
	    relic_load_string(loaded, "rules.dl", rules) == 0,
	    SCENARIO_FACT, "rules load between added facts");
	check(relic_value_symbol(added, "b", 1, &edge[0]) == 0 &&
	    relic_value_symbol(added, "c", 1, &edge[1]) == 0 &&
	    relic_fact_add(added, "edge", 2, edge) == 0,
	    SCENARIO_FACT, "edge(b, c) is added after the rules");

	weight[1].type = RELIC_INTEGER;
	weight[1].v.integer = 7;
	check(relic_value_symbol(added, "a", 1, &weight[0]) == 0 &&
	    relic_value_string(added, "x y", 3, &weight[2]) == 0 &&
	    relic_fact_add(added, "weight", 3, weight) == 0 &&
	    relic_fact_add(added, "ready", 0, NULL) == 0,
	    SCENARIO_FACT,
	    "facts of every value type and of no arity are added");

	/* Every refusal reports once and adds nothing. */
	bzero(&log, sizeof(log));
	refusals = 0;
	refusals += relic_fact_add(added, "Edge", 2, edge) == -1;
	refusals += relic_fact_add(added, "not", 2, edge) == -1;
	refusals += relic_fact_add(added, "", 2, edge) == -1;
	refusals += relic_fact_add(added, "edge$base", 2, edge) == -1;
	refusals += relic_fact_add(added, "edge", 2, NULL) == -1;
	refusals += relic_fact_add(added, NULL, 2, edge) == -1;
	bzero(wide, sizeof(wide));
	wide[0].type = RELIC_INTEGER;
	refusals += relic_fact_add(added, "wide", 33, wide) == -1;
	edge[1].type = (enum relic_type)99;
	refusals += relic_fact_add(added, "edge", 2, edge) == -1;
	edge[1].type = RELIC_SYMBOL;
	edge[1].v.id = UINT32_MAX;
	refusals += relic_fact_add(added, "edge", 2, edge) == -1;
	checkf(refusals == 9 && log.errors == 9, SCENARIO_FACT,
	    "nine malformed facts should each be refused with one error, "
	    "not %zu refusals and %zu errors", refusals, log.errors);

	from_added = answer_text(added, "path(X, Y)?");
	from_loaded = answer_text(loaded, "path(X, Y)?");
	checkf(from_added != NULL && from_loaded != NULL &&
	    strcmp(from_added, from_loaded) == 0 &&
	    strcmp(from_added, "a b\na c\nb c\n") == 0, SCENARIO_FACT,
	    "added facts should answer path(X, Y)? as the loaded ones do, "
	    "not\n%s", from_added != NULL ? from_added : "(failed)");
	free(from_added);
	free(from_loaded);

	from_added = answer_text(added, "weight(A, B, C), ready?");
	checkf(from_added != NULL && strcmp(from_added, "a 7 x y\n") == 0,
	    SCENARIO_FACT, "the added weight and ready facts should answer "
	    "\"a 7 x y\", not\n%s", from_added != NULL ? from_added :
	    "(failed)");
	free(from_added);

	relic_destroy(added);
	relic_destroy(loaded);
}

/*
 * Adding a fact after evaluation discards everything derived and derives it
 * again, so a fact that makes a negated literal false retracts what was
 * derived through it, both from a materialized program and from one answered
 * query by query.
 */
static void
an_added_fact_invalidates_what_was_derived(void)
{
	relic_value_t value;
	diag_log_t log;
	relic_t *ctx;
	char *rows;
	int materialized;

	for (materialized = 0; materialized <= 1; materialized++) {
		ctx = context_with("node(a).\nnode(b).\n"
		    "free(X) :- node(X), not taken(X).\n", NULL, &log);
		if (ctx == NULL)
			return;

		if (materialized)
			check(relic_materialize(ctx) == 0,
			    SCENARIO_INVALIDATE, "the program materializes");
		rows = answer_text(ctx, "free(X)?");
		checkf(rows != NULL && strcmp(rows, "a\nb\n") == 0,
		    SCENARIO_INVALIDATE, "free(X)? should answer a and b "
		    "first, not\n%s", rows != NULL ? rows : "(failed)");
		free(rows);

		check(relic_value_symbol(ctx, "a", 1, &value) == 0 &&
		    relic_fact_add(ctx, "taken", 1, &value) == 0,
		    SCENARIO_INVALIDATE, "taken(a) is added");
		if (materialized)
			check(relic_materialize(ctx) == 0,
			    SCENARIO_INVALIDATE,
			    "the program materializes again");

		rows = answer_text(ctx, "free(X)?");
		checkf(rows != NULL && strcmp(rows, "b\n") == 0,
		    SCENARIO_INVALIDATE, "after taken(a), free(X)? %s should "
		    "answer only b, not\n%s", materialized ?
		    "over a materialized program" : "answered directly",
		    rows != NULL ? rows : "(failed)");
		free(rows);

		relic_destroy(ctx);
	}
}

static void
record_mixed_note(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	mixed_log_t *log;

	log = callback_ctx;
#ifdef RELIC_THREADS
	if (!pthread_equal(pthread_self(), main_thread)) {
		log->other++;
		return;
	}
#endif
	if (level != RELIC_NOTE ||
	    strncmp(message, MIXED_NOTE, strlen(MIXED_NOTE)) != 0 ||
	    where == NULL) {
		log->other++;
		return;
	}

	if (log->notes < MIXED_NOTES_MAX)
		log->lines[log->notes] = where->line;
	log->notes++;
}

/*
 * Whether one of the notes a log kept pointed at `line`.  The order of the
 * notes is the order components are evaluated in, which the scenario does not
 * depend on.
 */
static bool
noted_line(const mixed_log_t *log, uint32_t line)
{
	size_t i;

	for (i = 0; i < log->notes && i < MIXED_NOTES_MAX; i++) {
		if (log->lines[i] == line)
			return (true);
	}

	return (false);
}

/*
 * An ordering comparison between values of different types is false, and
 * RELIC_F_NOTE_MIXED asks for a note about the first in each rule.  The
 * program has three rules: `small` compares once, `between` twice, and
 * `before` compares values of one type only, so a full evaluation notes the
 * first two rules once each, at the line of the first comparison that mixed
 * types, and says nothing about the third.  Nothing is noted without the
 * switch, and the answers are the same with it.
 *
 * `lt(1, Y)?` is answered through its Magic rewrite, whose copy of the `lt`
 * rule counts as that rule: it is noted once, and neither a second query
 * through the rewrite nor a later materialization notes it again.
 */
static void
a_mixed_comparison_is_noted_once_per_rule(void)
{
	static const char program[] =
	    "v(1).\nv(a).\nv(\"s\").\n"
	    "small(X) :- v(X),\n    X < 5.\n"
	    "between(X) :- v(X),\n    X > 0,\n    X < 9.\n"
	    "before(X) :- v(X), v(Y), X = 1, Y = 1, X <= Y.\n"
	    "lt(X, Y) :- v(X), v(Y),\n    X < Y.\n";
	relic_config_t config;
	mixed_log_t log;
	relic_t *ctx;
	char *rows;
	int noting;

	for (noting = 0; noting <= 1; noting++) {
		bzero(&config, sizeof(config));
		config.flags = mixed_flags;
		config.threads = mixed_threads;
		if (noting)
			config.flags |= RELIC_F_NOTE_MIXED;
		bzero(&log, sizeof(log));

		ctx = relic_create_config(&config);
		if (ctx == NULL) {
			check(false, SCENARIO_MIXED, "a context is created");
			return;
		}
		relic_diag_set(ctx, record_mixed_note, &log);
		check(relic_load_string(ctx, "mixed.dl", program) == 0,
		    SCENARIO_MIXED, "the program loads");

		check(relic_materialize(ctx) == 0, SCENARIO_MIXED,
		    "the program materializes");
		rows = answer_text(ctx, "between(X)?");
		checkf(rows != NULL && strcmp(rows, "1\n") == 0,
		    SCENARIO_MIXED, "between(X)? should answer 1 %s the "
		    "switch, not\n%s", noting ? "with" : "without",
		    rows != NULL ? rows : "(failed)");
		free(rows);

		if (!noting) {
			checkf(log.notes == 0 && log.other == 0,
			    SCENARIO_MIXED, "without the switch nothing is "
			    "noted, not %zu notes and %zu other diagnostics",
			    log.notes, log.other);
			relic_destroy(ctx);
			continue;
		}

		checkf(log.notes == 3 && log.other == 0, SCENARIO_MIXED,
		    "materializing notes small, between and lt once each, "
		    "not %zu notes and %zu other diagnostics", log.notes,
		    log.other);
		check(noted_line(&log, 5) && noted_line(&log, 7) &&
		    noted_line(&log, 11), SCENARIO_MIXED,
		    "each note points at its rule's first mixed comparison");
		relic_destroy(ctx);

		bzero(&log, sizeof(log));
		ctx = relic_create_config(&config);
		if (ctx == NULL) {
			check(false, SCENARIO_MIXED, "a context is created");
			return;
		}
		relic_diag_set(ctx, record_mixed_note, &log);
		check(relic_load_string(ctx, "mixed.dl", program) == 0,
		    SCENARIO_MIXED, "the program loads again");

		rows = answer_text(ctx, "lt(1, Y)?");
		free(rows);
		checkf(log.notes == 1 && log.lines[0] == 11, SCENARIO_MIXED,
		    "lt(1, Y)? notes the lt rule once, not %zu times",
		    log.notes);

		rows = answer_text(ctx, "lt(a, Y)?");
		free(rows);
		check(relic_materialize(ctx) == 0, SCENARIO_MIXED,
		    "the program materializes after the queries");
		checkf(log.notes == 3 && log.other == 0, SCENARIO_MIXED,
		    "a second query and a materialization note only small "
		    "and between, for %zu notes in all", log.notes);

		relic_destroy(ctx);
	}

	a_mixed_note_follows_the_rule_not_its_place();
	a_query_and_its_rewrite_share_one_note();
}

/*
 * The note belongs to a rule, not to the place it was written.  Two loads
 * under one source name put a rule of each at mixed.dl:3:1, and both rules
 * are noted.  A query is noted each time it is answered, since each answer is
 * a question the reader asked afresh.
 */
static void
a_mixed_note_follows_the_rule_not_its_place(void)
{
	relic_config_t config;
	mixed_log_t log;
	relic_t *ctx;
	char *rows;

	bzero(&config, sizeof(config));
	config.flags = mixed_flags | RELIC_F_NOTE_MIXED;
	config.threads = mixed_threads;
	bzero(&log, sizeof(log));

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, SCENARIO_MIXED, "a context is created");
		return;
	}
	relic_diag_set(ctx, record_mixed_note, &log);
	check(relic_load_string(ctx, "mixed.dl",
	    "v(1).\nv(a).\np(X) :- v(X), X < 5.\n") == 0 &&
	    relic_load_string(ctx, "mixed.dl",
	    "w(1).\nw(a).\nq(X) :- w(X), X > 0.\n") == 0,
	    SCENARIO_MIXED, "two loads under one name succeed");

	check(relic_materialize(ctx) == 0, SCENARIO_MIXED,
	    "the program materializes");
	checkf(log.notes == 2, SCENARIO_MIXED, "two rules written at the "
	    "same place are noted apart, not %zu times", log.notes);

	rows = answer_text(ctx, "v(X), X < 9?");
	free(rows);
	rows = answer_text(ctx, "v(X), X < 9?");
	free(rows);
	checkf(log.notes == 4 && log.other == 0, SCENARIO_MIXED,
	    "a query is noted once per answer, for %zu notes in all",
	    log.notes);

	relic_destroy(ctx);
}

/*
 * The Magic rewrite of a query copies the query's comparison into the seed
 * of a magic predicate, so one answer runs the comparison in two rules.
 * Both are the query's, and the answer is noted once.
 */
static void
a_query_and_its_rewrite_share_one_note(void)
{
	relic_config_t config;
	mixed_log_t log;
	relic_t *ctx;
	char *rows;

	bzero(&config, sizeof(config));
	config.flags = mixed_flags | RELIC_F_NOTE_MIXED;
	config.threads = mixed_threads;
	bzero(&log, sizeof(log));

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, SCENARIO_MIXED, "a context is created");
		return;
	}
	relic_diag_set(ctx, record_mixed_note, &log);
	check(relic_load_string(ctx, "seed.dl",
	    "e(1, 2).\ne(2, 3).\ne(1, b).\n"
	    "r(X, Y) :- e(X, Y).\nr(X, Z) :- e(X, Y), r(Y, Z).\n") == 0,
	    SCENARIO_MIXED, "a recursive program loads");

	rows = answer_text(ctx, "e(1, Y), Y < a, r(Y, Z)?");
	free(rows);
	checkf(log.notes == 1 && log.other == 0, SCENARIO_MIXED,
	    "a query and the seed rewritten from it are noted once, not "
	    "%zu times", log.notes);

	rows = answer_text(ctx, "e(1, Y), Y < a, r(Y, Z)?");
	free(rows);
	checkf(log.notes == 2, SCENARIO_MIXED,
	    "the next answer is noted once more, for %zu notes in all",
	    log.notes);

	relic_destroy(ctx);
}

/*
 * A collected result holds what the stream delivers, in its order, and
 * reports its width even with no rows; a query with no output columns
 * collects rows of no values; a failed query collects nothing; and a result
 * outlives later loads and the context itself.  So may a query, which is
 * destroyed here after its context, where only AddressSanitizer would see a
 * destroy that read the context.
 */
static void
a_collected_result_is_the_stream_kept(void)
{
	relic_config_t config;
	relic_query_t *query;
	relic_result_t *result;
	const relic_value_t *row;
	diag_log_t log;
	relic_t *ctx;

	ctx = context_with("n(1).\nn(2).\nn(3).\nlink(1, 2).\n", NULL, &log);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, "link(X, Y)?");
	result = query != NULL ? relic_query_collect(ctx, query) : NULL;
	checkf(result != NULL && relic_result_rows(result) == 1 &&
	    relic_result_columns(result) == 2 &&
	    relic_result_row(result, 1) == NULL, SCENARIO_COLLECT,
	    "link(X, Y)? should collect 1 row of 2 columns, not %zu of %zu",
	    result != NULL ? relic_result_rows(result) : 0,
	    result != NULL ? relic_result_columns(result) : 0);

	check(relic_load_string(ctx, "more.dl", "link(2, 3).\n") == 0,
	    SCENARIO_COLLECT, "more facts load after collecting");
	row = result != NULL ? relic_result_row(result, 0) : NULL;
	check(row != NULL && row[0].type == RELIC_INTEGER &&
	    row[0].v.integer == 1 && row[1].v.integer == 2, SCENARIO_COLLECT,
	    "a result keeps its row after the program grows");
	relic_result_destroy(result);
	relic_query_destroy(query);

	query = relic_query_prepare(ctx, NULL, "link(X, 1)?");
	result = query != NULL ? relic_query_collect(ctx, query) : NULL;
	check(result != NULL && relic_result_rows(result) == 0 &&
	    relic_result_columns(result) == 1, SCENARIO_COLLECT,
	    "a result with no rows still has the query's one column");
	relic_result_destroy(result);
	relic_query_destroy(query);

	query = relic_query_prepare(ctx, NULL, "link(1, 2)?");
	result = query != NULL ? relic_query_collect(ctx, query) : NULL;
	check(result != NULL && relic_result_rows(result) == 1 &&
	    relic_result_columns(result) == 0 &&
	    relic_result_row(result, 0) != NULL, SCENARIO_COLLECT,
	    "a query with no columns collects one row of no values");

	relic_destroy(ctx);
	check(result != NULL && relic_result_rows(result) == 1,
	    SCENARIO_COLLECT, "a result survives its context");
	relic_result_destroy(result);
	relic_query_destroy(query);

	bzero(&config, sizeof(config));
	config.max_tuples = 4;
	ctx = context_with("n(1).\nn(2).\nn(3).\nm(X) :- n(X).\n", &config,
	    &log);
	if (ctx == NULL)
		return;
	query = relic_query_prepare(ctx, NULL, "m(X)?");
	log.errors = 0;
	check(query != NULL && relic_query_collect(ctx, query) == NULL &&
	    log.errors == 1, SCENARIO_COLLECT,
	    "a query over the tuple limit collects nothing, with one error");
	relic_query_destroy(query);
	relic_destroy(ctx);
	relic_result_destroy(NULL);
}

/*
 * Confirm that `call`, which was given NULL for something it needs and
 * returned `status`, failed with -1 after reporting exactly one error,
 * `expected_error`, which is also the context's last error.
 */
static void
check_argument_refused(relic_t *ctx, const first_error_t *first, int status,
    const char *call, const char *expected_error)
{
	const char *error;

	error = relic_error(ctx);
	checkf(status == -1 && first->errors == 1 && error != NULL &&
	    strcmp(error, expected_error) == 0, SCENARIO_ARGUMENTS,
	    "%s should fail with \"%s\", not return %d with \"%s\" and %zu "
	    "errors", call, expected_error, status,
	    error != NULL ? error : "(none)", first->errors);
}

/*
 * An entry point given NULL where it needs a program, a stream, a path or
 * bytes fails with an error saying so, rather than reading through the
 * pointer, and the context stays usable.  The one NULL that is not missing
 * anything is a source name: it names the input "<input>", which is what
 * the diagnostics of a load without a name point at.
 */
static void
a_missing_argument_is_reported_not_followed(void)
{
	first_error_t first;
	relic_value_t value;
	relic_query_t *query;
	relic_t *ctx;
	size_t rows;
	int status;

	ctx = relic_create();
	if (ctx == NULL) {
		check(false, SCENARIO_ARGUMENTS, "a context is created");
		return;
	}
	relic_diag_set(ctx, record_first_error, &first);

	bzero(&first, sizeof(first));
	status = relic_load_string(ctx, NULL, "p(1).\nq(X) :- .\n");
	checkf(status == -1 && first.errors == 1 &&
	    strcmp(first.file, "<input>") == 0 && first.line == 2,
	    SCENARIO_ARGUMENTS, "a load with no source name should report "
	    "its error at <input>:2, not at %s:%u",
	    first.file[0] != '\0' ? first.file : "(none)",
	    (unsigned int)first.line);

	bzero(&first, sizeof(first));
	status = relic_load_string(ctx, "none.dl", NULL);
	check_argument_refused(ctx, &first, status,
	    "relic_load_string() with no text", "no program text was given");

	bzero(&first, sizeof(first));
	status = relic_load_file(ctx, "none.dl", NULL);
	check_argument_refused(ctx, &first, status,
	    "relic_load_file() with no stream", "no input stream was given");

	bzero(&first, sizeof(first));
	status = relic_load_path(ctx, NULL);
	check_argument_refused(ctx, &first, status,
	    "relic_load_path() with no path", "no path was given");

	bzero(&first, sizeof(first));
	status = relic_value_symbol(ctx, NULL, 3, &value);
	check_argument_refused(ctx, &first, status,
	    "relic_value_symbol() with no bytes",
	    "a value needs its bytes and somewhere to be stored");

	bzero(&first, sizeof(first));
	status = relic_load_string(ctx, NULL, "p(1).\n");
	check(status == 0, SCENARIO_ARGUMENTS,
	    "after the refusals a program loads without a name");
	query = relic_query_prepare(ctx, NULL, "p(X)?");
	rows = 0;
	status = -1;
	if (query != NULL)
		status = relic_query_run(ctx, query, count_rows, &rows);
	check(status == 0 && rows == 1 && first.errors == 0,
	    SCENARIO_ARGUMENTS, "after the refusals the program answers");

	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * A query is prepared against the program loaded so far, and whenever it
 * runs it answers the program as it stands then.  A later load that leaves
 * the program with no stratification makes the prepared query fail, with the
 * reason as the context's error, even though what the query reads does not
 * depend on the new rule: the program as a whole has no meaning, so the
 * answers derived before the load are not given either.
 */
static void
a_prepared_query_runs_over_the_program_as_it_stands(void)
{
	relic_query_t *query;
	const char *error;
	diag_log_t log;
	relic_t *ctx;
	size_t rows;
	int status;

	ctx = context_with("p(1).\np(2).\n", NULL, &log);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, "p(X)?");
	rows = 0;
	status = -1;
	if (query != NULL)
		status = relic_query_run(ctx, query, count_rows, &rows);
	checkf(status == 0 && rows == 2, SCENARIO_LATER_LOAD,
	    "p(X)? should answer 2 rows before the load, not %zu", rows);

	status = relic_load_string(ctx, "later.dl",
	    "r(X) :- p(X), not r(X).\n");
	check(status == 0, SCENARIO_LATER_LOAD,
	    "a rule negating its own head loads");

	rows = 0;
	log.errors = 0;
	status = -1;
	if (query != NULL)
		status = relic_query_run(ctx, query, count_rows, &rows);
	error = relic_error(ctx);
	checkf(status == -1 && rows == 0 && log.errors > 0 && error != NULL &&
	    strcmp(error, "program is not stratified") == 0,
	    SCENARIO_LATER_LOAD, "after the load p(X)? should fail as not "
	    "stratified, not return %d with %zu rows and \"%s\"", status, rows,
	    error != NULL ? error : "(none)");

	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * A context configured with more threads than RELIC_THREADS_MAX is refused
 * rather than given fewer, and one with the maximum is created.  Neither
 * starts a thread, since neither evaluates.
 */
static void
a_context_takes_at_most_the_maximum_threads(void)
{
	relic_config_t config;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	config.threads = RELIC_THREADS_MAX + 1;
	ctx = relic_create_config(&config);
	check(ctx == NULL, SCENARIO_THREADS,
	    "a context of RELIC_THREADS_MAX + 1 threads is refused");
	relic_destroy(ctx);

	config.threads = SIZE_MAX;
	ctx = relic_create_config(&config);
	check(ctx == NULL, SCENARIO_THREADS,
	    "a context of SIZE_MAX threads is refused");
	relic_destroy(ctx);

	config.threads = RELIC_THREADS_MAX;
	ctx = relic_create_config(&config);
	check(ctx != NULL, SCENARIO_THREADS,
	    "a context of RELIC_THREADS_MAX threads is created");
	relic_destroy(ctx);
}

int
main(void)
{
#ifdef RELIC_THREADS
	main_thread = pthread_self();
#endif

	a_callback_cannot_call_into_its_context();
	a_refusal_does_not_hide_why_an_operation_failed();
	an_answer_does_not_outlive_its_delivery();
	every_diagnostic_reaches_the_callback();
	a_mixed_comparison_is_noted_once_per_rule();

	/*
	 * The notes again, with every round deferred and shared among four
	 * threads: they come as the serial evaluator's do, from the merge on
	 * the calling thread.
	 */
	mixed_flags = RELIC_F_PARALLEL_ALWAYS;
	mixed_threads = 4;
	a_mixed_comparison_is_noted_once_per_rule();
	mixed_flags = 0;
	mixed_threads = 0;

	a_value_holds_only_what_its_type_can();
	an_added_fact_is_the_fact_its_text_loads();
	an_added_fact_invalidates_what_was_derived();
	a_collected_result_is_the_stream_kept();
	a_missing_argument_is_reported_not_followed();
	a_prepared_query_runs_over_the_program_as_it_stands();
	a_context_takes_at_most_the_maximum_threads();

	return (check_summary("test_api"));
}
