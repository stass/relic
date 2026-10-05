/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for diagnostics.
 *
 * Four obligations are checked.  A caller that installs no callback still
 * gets nothing on its stderr and can still recover the reason for a failure.
 * A caller that installs one receives every diagnostic with its level and its
 * location intact.  Every error carries the class it was reported with, which
 * a caller reads beside its text.  And the formatter the tool installs never
 * passes an unprintable byte through to whoever is reading, because both the
 * message and the file name in a diagnostic can quote text that came out of
 * a source file or a command line.
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

#include "harness.h"
#include "internal.h"

#define	SCENARIO_CALLBACK	"a callback receives every diagnostic"
#define	SCENARIO_CLASS		"every error carries its class"
#define	SCENARIO_ESCAPE		"the stderr formatter escapes what it prints"
#define	SCENARIO_LIMIT		"a refusal by the memory limit is reported as one"
#define	SCENARIO_SILENT		"a context with no callback stays silent"
#define	SCENARIO_QUOTE		"a quoted string is written as its literal"

/* How many aggregates the refused check's program compiles. */
#define	CHECK_AGGREGATES	40

/*
 * What the recording callback saw.  Only the last diagnostic is kept in
 * full, since that is what the tests ask about; the counts prove nothing was
 * dropped along the way.
 */
typedef struct class_seen class_seen_t;
typedef struct record record_t;

/*
 * The class relic_error_class() gave from inside the callback, when it was
 * last handed an error by `ctx`.
 */
struct class_seen {
	relic_t			*ctx;
	enum relic_error_class	 seen;
};

struct record {
	char			 message[512];
	relic_location_t	 where;
	enum relic_diag_level	 level;
	unsigned int		 total;
	unsigned int		 located;
	bool			 overflowed;
};

static bool	 capture_diagnostic(char *buffer, size_t size,
		    const relic_location_t *where, const char *message);
static void	 record_diagnostic(void *ctx, enum relic_diag_level level,
		    const relic_location_t *where, const char *message);
static void	 record_class(void *callback_ctx, enum relic_diag_level level,
		    const relic_location_t *where, const char *message);
static void	 a_callback_sees_level_location_and_text(void);
static void	 a_silent_context_still_remembers_its_error(void);
static void	 every_error_carries_its_class(void);
static void	 a_refusal_by_the_limit_is_reported_as_the_limit(void);
static void	 an_operation_reports_running_out_of_memory_once(void);
static void	 an_operation_names_itself_in_a_refusal(void);
static void	 a_refused_check_names_the_check(void);
static relic_t	*create_limited(size_t max_memory, record_t *record);
static void	 check_refusal(relic_t *ctx, int status, const char *expected,
		    const char *what);
static FILE	*table_stream(const char *table);
static bool	 last_error_is(relic_t *ctx, const char *expected);
static void	 the_stderr_formatter_escapes_unprintable_bytes(void);
static bool	 quotes_as(const char *bytes, size_t length, size_t size,
		    const char *expected);
static void	 a_quoted_string_is_escaped_and_cut_whole(void);

static void
record_diagnostic(void *ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	record_t *record;

	record = ctx;
	record->total++;
	record->level = level;

	if (strlen(message) >= sizeof(record->message))
		record->overflowed = true;
	strlcpy(record->message, message, sizeof(record->message));

	if (where != NULL) {
		record->located++;
		record->where = *where;
	}
}

static void
a_callback_sees_level_location_and_text(void)
{
	static const relic_location_t at_rule = {
		"policy.dl", 18, 5
	};

	relic_t *ctx;
	record_t record;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_CALLBACK, "a context should be created");
	if (ctx == NULL)
		return;

	bzero(&record, sizeof(record));
	relic_diag_set(ctx, record_diagnostic, &record);

	relic_diag_note(ctx, NULL, "a note about %s", "nothing");
	check(record.total == 1 && record.level == RELIC_NOTE,
	    SCENARIO_CALLBACK, "a note should arrive with its level");
	check(strcmp(record.message, "a note about nothing") == 0,
	    SCENARIO_CALLBACK, "the format arguments should be substituted");
	check(record.located == 0, SCENARIO_CALLBACK,
	    "an unlocated diagnostic should carry no location");

	relic_diag_error(ctx, RELIC_ERROR_REJECTED, &at_rule,
	    "%s/%d is not stratified", "ready", 1);
	check(record.total == 2 && record.level == RELIC_ERROR,
	    SCENARIO_CALLBACK, "an error should arrive with its level");
	check(record.located == 1 && record.where.line == 18 &&
	    record.where.column == 5, SCENARIO_CALLBACK,
	    "the location should reach the callback unchanged");
	check(record.where.file != NULL &&
	    strcmp(record.where.file, "policy.dl") == 0, SCENARIO_CALLBACK,
	    "including the source name");

	/*
	 * Only an error is worth remembering as the reason a call failed; a
	 * note that followed one must not displace it.
	 */
	check(relic_error(ctx) != NULL &&
	    strcmp(relic_error(ctx), "ready/1 is not stratified") == 0,
	    SCENARIO_CALLBACK, "the error should be the recoverable one");

	relic_diag_warning(ctx, NULL, "a warning");
	check(relic_error(ctx) != NULL &&
	    strcmp(relic_error(ctx), "ready/1 is not stratified") == 0,
	    SCENARIO_CALLBACK, "a later warning should not replace it");

	/*
	 * Removing the callback stops delivery without disturbing anything
	 * already recorded.
	 */
	relic_diag_set(ctx, NULL, NULL);
	relic_diag_error(ctx, RELIC_ERROR_REJECTED, NULL, "unheard");
	check(record.total == 3, SCENARIO_CALLBACK,
	    "no diagnostic should arrive after the callback is removed");
	check(!record.overflowed, SCENARIO_CALLBACK,
	    "no message should have been truncated by the recorder");

	relic_destroy(ctx);
}

static void
a_silent_context_still_remembers_its_error(void)
{
	relic_t *ctx;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_SILENT, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_error(ctx) == NULL, SCENARIO_SILENT,
	    "a fresh context should report no error");

	relic_diag_warning(ctx, NULL, "only a warning");
	check(relic_error(ctx) == NULL, SCENARIO_SILENT,
	    "a warning alone should not make the context look failed");

	relic_diag_error(ctx, RELIC_ERROR_IO, NULL, "could not open %s",
	    "graph.dl");
	check(relic_error(ctx) != NULL &&
	    strcmp(relic_error(ctx), "could not open graph.dl") == 0,
	    SCENARIO_SILENT,
	    "the error should be recoverable with no callback installed");

	relic_destroy(ctx);
}

static void
record_class(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_TEST_UNUSED,
    const char *message RELIC_TEST_UNUSED)
{
	class_seen_t *class_seen;

	class_seen = callback_ctx;
	if (level == RELIC_ERROR)
		class_seen->seen = relic_error_class(class_seen->ctx);
}

/*
 * Every error carries the class it was reported with, recorded with its
 * text before the callback is handed it, and a note or a warning, which is
 * not a failure, leaves the last error's class alone.  The funnels name
 * their classes: an internal error is INTERNAL and carries its prefix, as
 * every error of that class does, a limit's refusal is LIMIT and the
 * parser's errors are REJECTED.  A call
 * refused because the context was busy reads as a misuse of the interface
 * until the operation it was made inside returns, and then the last error
 * reads again.
 */
static void
every_error_carries_its_class(void)
{
	static const char refused[] =
	    "the context was called from one of its own callbacks";
	class_seen_t class_seen;
	relic_t *ctx;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_CLASS, "a context should be created");
	if (ctx == NULL)
		return;

	bzero(&class_seen, sizeof(class_seen));
	class_seen.ctx = ctx;
	relic_diag_set(ctx, record_class, &class_seen);

	check(relic_error_class(ctx) == RELIC_ERROR_NONE, SCENARIO_CLASS,
	    "a fresh context should have no error class");

	relic_diag_note(ctx, NULL, "a note");
	relic_diag_warning(ctx, NULL, "a warning");
	check(relic_error(ctx) == NULL &&
	    relic_error_class(ctx) == RELIC_ERROR_NONE, SCENARIO_CLASS,
	    "a note and a warning should record no error");

	relic_diag_error(ctx, RELIC_ERROR_IO, NULL, "cannot open %s",
	    "graph.dl");
	check(last_error_is(ctx, "cannot open graph.dl") &&
	    relic_error_class(ctx) == RELIC_ERROR_IO, SCENARIO_CLASS,
	    "an error should be recorded with the class it was reported with");
	check(class_seen.seen == RELIC_ERROR_IO, SCENARIO_CLASS,
	    "the callback should read the class of the error it is handed");

	relic_diag_warning(ctx, NULL, "a later warning");
	check(relic_error_class(ctx) == RELIC_ERROR_IO, SCENARIO_CLASS,
	    "a later warning should leave the class alone");

	relic_diag_internal(ctx, NULL, "a plan of %d steps reads past its end",
	    3);
	check(last_error_is(ctx,
	    "internal error: a plan of 3 steps reads past its end") &&
	    relic_error_class(ctx) == RELIC_ERROR_INTERNAL, SCENARIO_CLASS,
	    "an internal error should carry its prefix and its class");
	relic_diag_error(ctx, RELIC_ERROR_INTERNAL, NULL, "a plan of %d steps",
	    4);
	check(last_error_is(ctx, "internal error: a plan of 4 steps"),
	    SCENARIO_CLASS, "every error of that class should carry the "
	    "prefix, however it was reported");

	relic_diag_limit(ctx, "tuples", 5, "deriving", "path/2");
	check(last_error_is(ctx, "limit exceeded: tuples=5 while deriving "
	    "path/2") && relic_error_class(ctx) == RELIC_ERROR_LIMIT,
	    SCENARIO_CLASS, "a limit's refusal should be of the limit's class");

	check(relic_load_string(ctx, "bad.dl", "p(X :- q(X).\n") == -1 &&
	    relic_error_class(ctx) == RELIC_ERROR_REJECTED &&
	    class_seen.seen == RELIC_ERROR_REJECTED, SCENARIO_CLASS,
	    "a syntax error should be rejected text");

	if (relic_enter(ctx, NULL, NULL) == 0) {
		relic_diag_record_refusal(ctx);
		check(last_error_is(ctx, refused) &&
		    relic_error_class(ctx) == RELIC_ERROR_ARGUMENT,
		    SCENARIO_CLASS,
		    "a refused call should read as a misuse of the interface");

		relic_diag_error(ctx, RELIC_ERROR_EVALUATION, NULL,
		    "division by zero");
		check(relic_error_class(ctx) == RELIC_ERROR_EVALUATION,
		    SCENARIO_CLASS,
		    "an error after a refusal should replace it");

		relic_diag_record_refusal(ctx);
		relic_leave(ctx);
		check(last_error_is(ctx, "division by zero") &&
		    relic_error_class(ctx) == RELIC_ERROR_EVALUATION,
		    SCENARIO_CLASS, "once the operation returns, its last "
		    "error should read again");
	}

	relic_destroy(ctx);
}

/*
 * Run the stderr formatter with stderr redirected to a temporary file and
 * return what it wrote in `buffer`.
 *
 * Redirecting the real stderr, rather than calling an inner function that
 * takes a stream, is deliberate: what is under test is the callback the tool
 * installs, exactly as the tool installs it.
 *
 * stderr is restored before returning, so that a check the caller makes
 * afterwards can still report a failure.  A test whose own failure messages
 * went into the capture would be a test that cannot fail visibly.
 */
static bool
capture_diagnostic(char *buffer, size_t size, const relic_location_t *where,
    const char *message)
{
	FILE *capture;
	size_t length;
	int saved_stderr;

	buffer[0] = '\0';

	capture = tmpfile();
	if (capture == NULL)
		return (false);

	fflush(stderr);
	saved_stderr = dup(fileno(stderr));
	if (saved_stderr == -1 || dup2(fileno(capture), fileno(stderr)) == -1) {
		if (saved_stderr != -1)
			close(saved_stderr);
		fclose(capture);
		return (false);
	}

	relic_diag_stderr(NULL, RELIC_ERROR, where, message);
	fflush(stderr);

	/*
	 * If stderr cannot be put back, every later failure message in this
	 * binary would disappear into the capture, so the suite says so on
	 * the one descriptor still known to work and stops.
	 */
	if (dup2(saved_stderr, fileno(stderr)) == -1) {
		fprintf(stdout, "could not restore stderr; aborting\n");
		close(saved_stderr);
		fclose(capture);
		exit(1);
	}
	close(saved_stderr);

	rewind(capture);
	length = fread(buffer, 1, size - 1, capture);
	buffer[length] = '\0';
	fclose(capture);

	return (true);
}

/*
 * A source file relic was asked to read can contain a terminal escape
 * sequence, in a string literal or in its own name, and a diagnostic quotes
 * both.  Printing one unexamined would let the file retitle the reader's
 * terminal or forge a diagnostic line of its own, so every byte outside
 * printable ASCII is written as an escape instead.
 */
static void
the_stderr_formatter_escapes_unprintable_bytes(void)
{
	static const relic_location_t hostile_file = {
		"graph\033]0;owned\007.dl", 3, 1
	};
	static const relic_location_t ordinary_file = {
		"graph.dl", 3, 1
	};

	char written[512];

	check(capture_diagnostic(written, sizeof(written), NULL,
	    "unexpected \033]0;owned\007 token"), SCENARIO_ESCAPE,
	    "the formatter's output should be capturable");
	check(strstr(written, "\\x1b") != NULL, SCENARIO_ESCAPE,
	    "an escape character in a message should be written as \\x1b");
	check(strchr(written, '\033') == NULL, SCENARIO_ESCAPE,
	    "and should not reach the stream itself");

	check(capture_diagnostic(written, sizeof(written), &hostile_file,
	    "bad rule"), SCENARIO_ESCAPE,
	    "the formatter's output should be capturable for a file name too");
	check(strstr(written, "\\x1b") != NULL, SCENARIO_ESCAPE,
	    "a file name should be escaped the same way");
	check(strchr(written, '\033') == NULL, SCENARIO_ESCAPE,
	    "and should not reach the stream either");

	/*
	 * A backslash is doubled so that an escape this formatter produced
	 * cannot be confused with one the text already contained.
	 */
	check(capture_diagnostic(written, sizeof(written), NULL,
	    "literal \\x1b here"), SCENARIO_ESCAPE,
	    "the formatter's output should be capturable once more");
	check(strstr(written, "\\\\x1b") != NULL, SCENARIO_ESCAPE,
	    "a backslash in the text should be doubled");

	check(capture_diagnostic(written, sizeof(written), &ordinary_file,
	    "bad rule"), SCENARIO_ESCAPE,
	    "an ordinary diagnostic should be capturable");
	check(strcmp(written, "graph.dl:3:1: error: bad rule\n") == 0,
	    SCENARIO_ESCAPE,
	    "a located diagnostic should read file:line:column: level: text");
}

static bool
last_error_is(relic_t *ctx, const char *expected)
{
	return (relic_error(ctx) != NULL &&
	    strcmp(relic_error(ctx), expected) == 0);
}

/*
 * A failed allocation reports as exhaustion unless the account records that
 * the memory limit refused, in which case it reports the limit, its value and
 * the activity in progress.  Beginning an activity, or an operation,
 * forgets a refusal nothing reported, so a later failure is not blamed on
 * it.  Each case is an operation of its own, as every report is made inside
 * one.  The refusal is set by hand: what sets it is the arena's business,
 * and test_arena's.
 */
static void
a_refusal_by_the_limit_is_reported_as_the_limit(void)
{
	record_t record;
	relic_t *ctx;

	ctx = create_limited(65536, &record);
	check(ctx != NULL, SCENARIO_LIMIT, "a context should be created");
	if (ctx == NULL)
		return;

	if (relic_enter(ctx, NULL, NULL) == 0) {
		relic_diag_no_memory(ctx);
		check(last_error_is(ctx, "out of memory") &&
		    relic_error_class(ctx) == RELIC_ERROR_MEMORY, SCENARIO_LIMIT,
		    "without a refusal, exhaustion is reported");
		relic_leave(ctx);
	}

	if (relic_enter(ctx, "planning", "path/2") == 0) {
		ctx->account.refused = true;
		relic_diag_no_memory(ctx);
		checkf(last_error_is(ctx,
		    "limit exceeded: memory=65536 while planning path/2"),
		    SCENARIO_LIMIT, "the refusal should name the limit and the "
		    "activity, not \"%s\"", relic_error(ctx));
		check(relic_error_class(ctx) == RELIC_ERROR_LIMIT,
		    SCENARIO_LIMIT, "and be of the limit's class");
		relic_leave(ctx);
	}

	if (relic_enter(ctx, NULL, NULL) == 0) {
		ctx->account.refused = true;
		relic_diag_activity(ctx, "rewriting", "$q0/1");
		relic_diag_no_memory(ctx);
		check(last_error_is(ctx, "out of memory"), SCENARIO_LIMIT,
		    "a new activity forgets a refusal nothing reported");
		relic_leave(ctx);
	}

	ctx->account.refused = true;
	check(relic_load_string(ctx, "test.dl", "e(a).\n") == 0 &&
	    !ctx->account.refused, SCENARIO_LIMIT,
	    "so does the beginning of an operation");

	relic_destroy(ctx);
}

/*
 * An operation reports running out of memory once.  An arena that refused
 * refuses every later request, so a caller that makes two allocations
 * before checking either meets one exhaustion twice, and a construction can
 * meet it in several arenas at once; a second report, whether it would read
 * as the limit or as exhaustion, says nothing the first did not, and is not
 * made.  A refusal by the limit met after the report is forgotten with it,
 * so that nothing later in the operation reads it.  The next operation
 * reports its own.
 */
static void
an_operation_reports_running_out_of_memory_once(void)
{
	static const char limit_message[] =
	    "limit exceeded: memory=65536 while planning path/2";
	record_t record;
	relic_t *ctx;
	size_t errors;

	ctx = create_limited(65536, &record);
	check(ctx != NULL, SCENARIO_LIMIT, "a context should be created");
	if (ctx == NULL)
		return;

	errors = ctx->diag.error_count;
	if (relic_enter(ctx, "planning", "path/2") == 0) {
		ctx->account.refused = true;
		relic_diag_no_memory(ctx);
		relic_diag_no_memory(ctx);
		ctx->account.refused = true;
		relic_diag_no_memory(ctx);

		checkf(ctx->diag.error_count == errors + 1 &&
		    record.total == 1 && last_error_is(ctx, limit_message),
		    SCENARIO_LIMIT, "one operation's exhaustion should be "
		    "reported once, not %zu times, as \"%s\"",
		    ctx->diag.error_count - errors, relic_error(ctx));
		check(!ctx->account.refused, SCENARIO_LIMIT,
		    "a refusal met after the report should be forgotten");
		relic_leave(ctx);
	}

	if (relic_enter(ctx, NULL, NULL) == 0) {
		relic_diag_no_memory(ctx);
		check(ctx->diag.error_count == errors + 2 &&
		    record.total == 2 && last_error_is(ctx, "out of memory"),
		    SCENARIO_LIMIT, "the next operation should report its own");
		relic_leave(ctx);
	}

	relic_destroy(ctx);
}

/*
 * A context whose memory limit is `max_memory` bytes and whose diagnostics
 * are recorded in `record`.
 */
static relic_t *
create_limited(size_t max_memory, record_t *record)
{
	relic_config_t config;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	config.max_memory = max_memory;
	ctx = relic_create_config(&config);
	if (ctx != NULL) {
		bzero(record, sizeof(*record));
		relic_diag_set(ctx, record_diagnostic, record);
	}

	return (ctx);
}

/*
 * Check that an operation which returned `status` was refused by a limit,
 * with `expected` as its error, describing the operation as `what`.  A
 * context that could not be created fails the check rather than being used.
 */
static void
check_refusal(relic_t *ctx, int status, const char *expected,
    const char *what)
{
	const char *error;

	error = ctx != NULL ? relic_error(ctx) : NULL;
	checkf(status != 0 && error != NULL && strcmp(error, expected) == 0 &&
	    relic_error_class(ctx) == RELIC_ERROR_LIMIT, SCENARIO_LIMIT,
	    "%s should be refused by the limit as \"%s\", not \"%s\"", what,
	    expected, error != NULL ? error : "(none)");
}

/*
 * A stream holding `table`, positioned at its start, or NULL when the
 * temporary file could not be made.
 */
static FILE *
table_stream(const char *table)
{
	FILE *stream;

	stream = tmpfile();
	if (stream == NULL)
		return (NULL);

	if (fputs(table, stream) < 0 || fseek(stream, 0, SEEK_SET) != 0) {
		fclose(stream);
		return (NULL);
	}

	return (stream);
}

/*
 * Every operation that can intern names itself when the memory limit
 * refuses one of its names, as an evaluation names what it plans: a load
 * and an import by their source, an added fact by its predicate, and a
 * query being prepared by the name diagnostics give it.  A limit of one
 * byte refuses the first name each interns, before anything else can.
 * Preparing a query checks the program first, and the query's own activity
 * stands once that check is done.  Building a value is an operation with
 * nothing to name, and reports the limit alone.  A source name too long for
 * the message is cut and marked, and the callback receives the message
 * whole.
 */
static void
an_operation_names_itself_in_a_refusal(void)
{
	static const enum relic_column column = RELIC_COLUMN_SYMBOL;
	relic_import_t import;
	relic_value_t value;
	record_t record;
	relic_t *ctx;
	FILE *stream;
	char long_name[300];
	const char *error;
	int status;

	ctx = create_limited(1, &record);
	status = ctx != NULL ? relic_load_string(ctx, "graph.dl",
	    "edge(a, b).\n") : -1;
	check_refusal(ctx, status,
	    "limit exceeded: memory=1 while loading graph.dl", "a load");
	relic_destroy(ctx);

	bzero(&import, sizeof(import));
	import.predicate = "edge";
	import.columns = &column;
	import.column_count = 1;
	import.format = RELIC_FORMAT_TSV;
	ctx = create_limited(1, &record);
	stream = table_stream("a\n");
	status = ctx != NULL && stream != NULL ?
	    relic_import_file(ctx, "edge.tsv", stream, &import) : -1;
	check_refusal(ctx, status,
	    "limit exceeded: memory=1 while importing edge.tsv", "an import");
	if (stream != NULL)
		fclose(stream);
	relic_destroy(ctx);

	value.type = RELIC_INTEGER;
	value.v.integer = 7;
	ctx = create_limited(1, &record);
	status = ctx != NULL ? relic_fact_add(ctx, "f", 1, &value) : -1;
	check_refusal(ctx, status,
	    "limit exceeded: memory=1 while adding f/1", "an added fact");
	relic_destroy(ctx);

	ctx = create_limited(1, &record);
	status = ctx != NULL &&
	    relic_query_prepare(ctx, NULL, "e(a)?") != NULL ? 0 : -1;
	check_refusal(ctx, status,
	    "limit exceeded: memory=1 while preparing <query>",
	    "a query with no source name");
	status = ctx != NULL &&
	    relic_query_prepare(ctx, "<query 2>", "e(a)?") != NULL ? 0 : -1;
	check_refusal(ctx, status,
	    "limit exceeded: memory=1 while preparing <query 2>",
	    "a query with a source name");
	relic_destroy(ctx);

	ctx = create_limited(1, &record);
	status = ctx != NULL ? relic_value_symbol(ctx, "alpha", 5, &value) : -1;
	check_refusal(ctx, status, "limit exceeded: memory=1",
	    "building a value");
	relic_destroy(ctx);

	memset(long_name, 'n', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';
	ctx = create_limited(1, &record);
	status = ctx != NULL ? relic_load_string(ctx, long_name, "e(a).\n") :
	    -1;
	error = ctx != NULL ? relic_error(ctx) : NULL;
	checkf(status != 0 && error != NULL && strncmp(error,
	    "limit exceeded: memory=1 while loading nnn", 42) == 0 &&
	    strlen(error) > 3 && strcmp(error + strlen(error) - 3, "...") == 0,
	    SCENARIO_LIMIT, "a long source name should be cut and marked, "
	    "not \"%s\"", error != NULL ? error : "(none)");
	check(error != NULL && !record.overflowed &&
	    strcmp(record.message, error) == 0, SCENARIO_LIMIT,
	    "the callback should receive the cut message whole");
	relic_destroy(ctx);
}

/*
 * A check the memory limit refuses names the check.  Compiling an aggregate
 * whose body is not one atom interns the predicate that holds its groups,
 * so a program of CHECK_AGGREGATES of them interns more while it is checked
 * than its load left room for.  The limits swept run from one the load
 * alone fits under past one the whole check fits under, and some of them
 * refuse the check part way.
 */
static void
a_refused_check_names_the_check(void)
{
	char program[CHECK_AGGREGATES * 80 + 16];
	char expected[96];
	record_t record;
	relic_t *ctx;
	size_t refused;
	size_t limit;
	size_t used;
	int i;
	int status;

	used = (size_t)snprintf(program, sizeof(program), "edge(a, b).\n");
	for (i = 0; i < CHECK_AGGREGATES; i++)
		used += (size_t)snprintf(program + used, sizeof(program) - used,
		    "d%d(X, N) :- edge(X, _), "
		    "N = count { edge(X, Y), edge(Y, _) }.\n", i);
	if (used >= sizeof(program)) {
		check(false, SCENARIO_LIMIT, "the program should be built");
		return;
	}

	refused = 0;
	status = -1;
	for (limit = 64; limit <= 65536 && status != 0; limit += 64) {
		ctx = create_limited(limit, &record);
		if (ctx == NULL) {
			check(false, SCENARIO_LIMIT, "a context should be created");
			return;
		}

		if (relic_load_string(ctx, "graph.dl", program) != 0) {
			relic_destroy(ctx);
			continue;
		}

		status = relic_check(ctx);
		if (status != 0) {
			refused++;
			snprintf(expected, sizeof(expected),
			    "limit exceeded: memory=%zu while checking the "
			    "program", limit);
			check_refusal(ctx, status, expected, "a check");
		}

		relic_destroy(ctx);
	}

	checkf(refused > 0 && status == 0, SCENARIO_LIMIT,
	    "the sweep should reach a refused check and a completed one, not "
	    "%zu refusals and %s", refused,
	    status == 0 ? "a completion" : "no completion");
}

/*
 * Whether relic_diag_string() writes `length` bytes of `bytes` into a
 * buffer of `size` bytes as exactly `expected`.  The buffer is one byte
 * longer than it is said to be, and that byte must be left alone.
 */
static bool
quotes_as(const char *bytes, size_t length, size_t size,
    const char *expected)
{
	char buffer[64];

	if (size + 1 > sizeof(buffer))
		return (false);

	memset(buffer, '#', sizeof(buffer));
	relic_diag_string(bytes, length, buffer, size);

	return (buffer[size] == '#' && strcmp(buffer, expected) == 0);
}

/*
 * A string in a message is the literal that spells it: a quote and a
 * backslash escaped, and every byte that is not printable ASCII -- a NUL,
 * a control byte, a byte above 0x7f -- as \xHH.  A literal that does not
 * fit is cut after its last whole piece and marked after the quote, and a
 * buffer too small even for that is left empty rather than written past.
 */
static void
a_quoted_string_is_escaped_and_cut_whole(void)
{
	char unused;

	check(quotes_as("", 0, 16, "\"\""), SCENARIO_QUOTE,
	    "the empty string should be a pair of quotes");
	check(quotes_as("a\"b\\c", 5, 32, "\"a\\\"b\\\\c\""),
	    SCENARIO_QUOTE, "a quote and a backslash should be escaped");
	check(quotes_as("a\0\n\x1b\xff", 5, 32,
	    "\"a\\x00\\x0a\\x1b\\xff\""), SCENARIO_QUOTE,
	    "a NUL, control bytes and a high byte should be written in hex");

	check(quotes_as("abc", 3, 6, "\"abc\""), SCENARIO_QUOTE,
	    "a literal that exactly fits should be written whole");
	check(quotes_as("abcde", 5, 7, "\"a\"..."), SCENARIO_QUOTE,
	    "a literal one byte too long should be cut and marked");
	check(quotes_as("\x01" "bc", 3, 8, "\"\"..."), SCENARIO_QUOTE,
	    "an escape that does not fit whole should not be split");
	check(quotes_as("abcd", 4, 6, "\"\"...") &&
	    quotes_as("", 0, 6, "\"\""), SCENARIO_QUOTE,
	    "six bytes should hold a quote, the mark and a terminator");
	check(quotes_as("abc", 3, 5, ""), SCENARIO_QUOTE,
	    "a buffer too small for the mark should be left empty");

	unused = '#';
	relic_diag_string("abc", 3, &unused, 0);
	check(unused == '#', SCENARIO_QUOTE,
	    "a buffer of no bytes should not be written");
}

int
main(void)
{
	a_callback_sees_level_location_and_text();
	a_silent_context_still_remembers_its_error();
	every_error_carries_its_class();
	a_refusal_by_the_limit_is_reported_as_the_limit();
	an_operation_reports_running_out_of_memory_once();
	an_operation_names_itself_in_a_refusal();
	a_refused_check_names_the_check();
	the_stderr_formatter_escapes_unprintable_bytes();
	a_quoted_string_is_escaped_and_cut_whole();

	return (check_summary("diag"));
}
