/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for diagnostics.
 *
 * Three obligations are checked.  A caller that installs no callback still
 * gets nothing on its stderr and can still recover the reason for a failure.
 * A caller that installs one receives every diagnostic with its level and its
 * location intact.  And the formatter the tool installs never passes an
 * unprintable byte through to whoever is reading, because both the message
 * and the file name in a diagnostic can quote text that came out of a source
 * file or a command line.
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
#define	SCENARIO_ESCAPE		"the stderr formatter escapes what it prints"
#define	SCENARIO_LIMIT		"a refusal by the memory limit is reported as one"
#define	SCENARIO_SILENT		"a context with no callback stays silent"
#define	SCENARIO_QUOTE		"a quoted string is written as its literal"

/*
 * What the recording callback saw.  Only the last diagnostic is kept in
 * full, since that is what the tests ask about; the counts prove nothing was
 * dropped along the way.
 */
typedef struct record record_t;

struct record {
	char			 message[256];
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
static void	 a_callback_sees_level_location_and_text(void);
static void	 a_silent_context_still_remembers_its_error(void);
static void	 a_refusal_by_the_limit_is_reported_as_the_limit(void);
static bool	 last_error_is(relic_t *ctx, const char *expected);
static void	 the_stderr_formatter_escapes_unprintable_bytes(void);

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

	relic_diagf(ctx, RELIC_NOTE, NULL, "a note about %s", "nothing");
	check(record.total == 1 && record.level == RELIC_NOTE,
	    SCENARIO_CALLBACK, "a note should arrive with its level");
	check(strcmp(record.message, "a note about nothing") == 0,
	    SCENARIO_CALLBACK, "the format arguments should be substituted");
	check(record.located == 0, SCENARIO_CALLBACK,
	    "an unlocated diagnostic should carry no location");

	relic_diagf(ctx, RELIC_ERROR, &at_rule, "%s/%d is not stratified",
	    "ready", 1);
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

	relic_diagf(ctx, RELIC_WARNING, NULL, "a warning");
	check(relic_error(ctx) != NULL &&
	    strcmp(relic_error(ctx), "ready/1 is not stratified") == 0,
	    SCENARIO_CALLBACK, "a later warning should not replace it");

	/*
	 * Removing the callback stops delivery without disturbing anything
	 * already recorded.
	 */
	relic_diag_set(ctx, NULL, NULL);
	relic_diagf(ctx, RELIC_ERROR, NULL, "unheard");
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

	relic_diagf(ctx, RELIC_WARNING, NULL, "only a warning");
	check(relic_error(ctx) == NULL, SCENARIO_SILENT,
	    "a warning alone should not make the context look failed");

	relic_diagf(ctx, RELIC_ERROR, NULL, "could not open %s", "graph.dl");
	check(relic_error(ctx) != NULL &&
	    strcmp(relic_error(ctx), "could not open graph.dl") == 0,
	    SCENARIO_SILENT,
	    "the error should be recoverable with no callback installed");

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
 * the activity in progress, once.  Beginning an activity, or an operation,
 * forgets a refusal nothing reported, so a later failure is not blamed on
 * it.  The refusal is set by hand: what sets it is the arena's business,
 * and test_arena's.
 */
static void
a_refusal_by_the_limit_is_reported_as_the_limit(void)
{
	relic_config_t config;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	config.max_memory = 65536;
	ctx = relic_create_config(&config);
	check(ctx != NULL, SCENARIO_LIMIT, "a context should be created");
	if (ctx == NULL)
		return;

	relic_diag_no_memory(ctx);
	check(last_error_is(ctx, "out of memory"), SCENARIO_LIMIT,
	    "without a refusal, exhaustion is reported");

	relic_diag_activity(ctx, "planning", "path/2");
	ctx->account.refused = true;
	relic_diag_no_memory(ctx);
	checkf(last_error_is(ctx,
	    "limit exceeded: memory=65536 while planning path/2"),
	    SCENARIO_LIMIT, "the refusal should name the limit and the "
	    "activity, not \"%s\"", relic_error(ctx));

	relic_diag_no_memory(ctx);
	check(last_error_is(ctx, "out of memory"), SCENARIO_LIMIT,
	    "the refusal is reported once");

	ctx->account.refused = true;
	relic_diag_activity(ctx, "rewriting", "$q0/1");
	relic_diag_no_memory(ctx);
	check(last_error_is(ctx, "out of memory"), SCENARIO_LIMIT,
	    "a new activity forgets a refusal nothing reported");

	ctx->account.refused = true;
	check(relic_load_string(ctx, "test.dl", "e(a).\n") == 0 &&
	    !ctx->account.refused, SCENARIO_LIMIT,
	    "so does the beginning of an operation");

	relic_destroy(ctx);
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
	a_refusal_by_the_limit_is_reported_as_the_limit();
	the_stderr_formatter_escapes_unprintable_bytes();
	a_quoted_string_is_escaped_and_cut_whole();

	return (check_summary("diag"));
}
