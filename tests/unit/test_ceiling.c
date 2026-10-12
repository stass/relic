/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the identifier ceilings of the symbol pool and the
 * integer table.
 *
 * A domain identifier keeps its type in its top two bits, so each table
 * numbers its entries below them, and an entry numbered past the index mask
 * would run into the type bits and turn one value into another.  Each table
 * refuses the entry that would pass its ceiling.  No test can intern a
 * billion values, so this program compiles the library with both ceilings
 * lowered, and checks what the real ceilings are held to: the entry past
 * the ceiling is refused with a diagnostic, and the table is left as it
 * was, its values interning to and decoding as the identifiers they had.
 * An evaluation computing more integers than the table can number is
 * refused so too, and a round run deferred is refused where the serial
 * round is, leaving what it leaves.  The longest string a value may hold is
 * lowered as well, so that a concatenation past it, which the real limit of
 * four gigabytes puts out of any test's reach, fails an evaluation.
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

#define	SCENARIO_INTEGERS	"the integer table stops at its ceiling"
#define	SCENARIO_SYMBOLS	"the symbol pool stops at its ceiling"
#define	SCENARIO_DEFERRED	"a deferred round stops at the integer ceiling"
#define	SCENARIO_TOO_LONG	"a string too long to be a value fails"
#define	SCENARIO_LEXED		"a name or literal too long is refused as read"
#define	SCENARIO_IMPORTED	"an imported field too long is refused at it"

/*
 * Forty integers loaded and a rule computing a new one from each, which
 * together pass the table's ceiling of 64 in the rule's one round.
 */
#define	COMPUTED_FACTS		40

/*
 * A string of half the longest length a value may have, which this program
 * is compiled with at 64, and that length as text: twice the string is
 * exactly the longest, and three times it is too long.
 */
#define	STRING_HALF		"0123456789abcdef0123456789abcdef"
#define	STRING_LIMIT_TEXT	"64"

_Static_assert(RELIC_STRING_LENGTH_MAX == 2 * (sizeof(STRING_HALF) - 1),
    "twice the half string is the longest a value may be");

/* The errors an import reports, and where the last of them is. */
typedef struct import_errors {
	size_t		errors;
	uint32_t	line;
	uint32_t	column;
} import_errors_t;

static void	 a_deferred_round_stops_as_serially(size_t threads);
static void	 a_string_too_long_fails_the_evaluation(void);
static void	 a_name_or_literal_too_long_is_refused(void);
static void	 an_imported_field_too_long_is_refused(void);
static void	 note_error(void *callback_ctx, enum relic_diag_level level,
		    const relic_location_t *where, const char *message);
static bool	 imports_with_error(relic_t *ctx, const char *text,
		    const char *expected, uint32_t line, uint32_t column);
static void	 count_error(void *callback_ctx, enum relic_diag_level level,
		    const relic_location_t *where, const char *message);
static bool	 loads_with_errors(relic_t *ctx, const char *text,
		    size_t errors, const char *expected);
static relic_t	*computing_context(unsigned int flags, size_t threads);
static int	 count_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static int	 query_rows(relic_t *ctx, const char *text, size_t *rowsp);

#define	SYMBOL_TEXT_MAX		32

static bool	 diagnosed(relic_t *ctx, const char *expected);
static void	 symbol_text(char *text, size_t size, size_t ordinal);
static void	 the_integer_table_stops_at_its_ceiling(void);
static void	 the_symbol_pool_stops_at_its_ceiling(void);

/*
 * Symbols are interned until the pool refuses one.  Exactly RELIC_SYMBOL_MAX
 * are admitted, counting any the context holds already, and the refusal
 * says why.  A string of new bytes is refused too, since strings share the
 * pool, but a string of bytes the pool holds is admitted: it needs no new
 * entry.  The symbols admitted keep their identifiers and their bytes.
 */
static void
the_symbol_pool_stops_at_its_ceiling(void)
{
	char text[SYMBOL_TEXT_MAX];
	relic_t *ctx;
	relic_value_t decoded;
	relic_dom_t first;
	relic_dom_t last;
	relic_dom_t dom;
	size_t admitted;
	size_t held;
	bool decoded_last;
	int status;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_SYMBOLS, "a context should be created");
	if (ctx == NULL)
		return;

	held = relic_symbol_count(ctx);
	first = RELIC_ID_NONE;
	last = RELIC_ID_NONE;
	for (admitted = 0; admitted <= RELIC_SYMBOL_MAX; admitted++) {
		symbol_text(text, sizeof(text), admitted);
		if (relic_value_intern_bytes(ctx, RELIC_SYMBOL, text,
		    strlen(text), &dom) != 0)
			break;
		if (first == RELIC_ID_NONE)
			first = dom;
		last = dom;
	}

	checkf(held + admitted == RELIC_SYMBOL_MAX, SCENARIO_SYMBOLS,
	    "the pool should admit %d symbols, not %zu", RELIC_SYMBOL_MAX,
	    held + admitted);
	check(diagnosed(ctx, "too many distinct symbols and strings"),
	    SCENARIO_SYMBOLS, "the refusal should say the pool is full");

	status = relic_value_intern_bytes(ctx, RELIC_STRING, "new", 3, &dom);
	check(status != 0, SCENARIO_SYMBOLS,
	    "a string of new bytes should be refused by the full pool");

	symbol_text(text, sizeof(text), 0);
	status = relic_value_intern_bytes(ctx, RELIC_STRING, text,
	    strlen(text), &dom);
	check(status == 0 && relic_value_decode(ctx, dom, &decoded) &&
	    decoded.type == RELIC_STRING, SCENARIO_SYMBOLS,
	    "a string of bytes the pool holds should need no new entry");
	status = relic_value_intern_bytes(ctx, RELIC_SYMBOL, text,
	    strlen(text), &dom);
	check(status == 0 && dom == first, SCENARIO_SYMBOLS,
	    "the first symbol should intern to its identifier again");

	symbol_text(text, sizeof(text), admitted - 1);
	decoded_last = relic_value_decode(ctx, last, &decoded);
	check(decoded_last && decoded.type == RELIC_SYMBOL &&
	    strcmp(relic_string(ctx, decoded.v.id, NULL), text) == 0,
	    SCENARIO_SYMBOLS,
	    "the last symbol admitted should decode to its bytes");

	relic_destroy(ctx);
}

/*
 * Integers are interned until the table refuses one.  Exactly
 * RELIC_INTEGER_MAX are admitted, the refusal says why, and the integers
 * admitted keep their identifiers.
 */
static void
the_integer_table_stops_at_its_ceiling(void)
{
	relic_t *ctx;
	relic_value_t decoded;
	relic_dom_t first;
	relic_dom_t last;
	relic_dom_t dom;
	size_t admitted;
	bool decoded_last;
	int status;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_INTEGERS, "a context should be created");
	if (ctx == NULL)
		return;

	first = RELIC_ID_NONE;
	last = RELIC_ID_NONE;
	for (admitted = 0; admitted <= RELIC_INTEGER_MAX; admitted++) {
		if (relic_value_intern_integer(ctx, -(int64_t)admitted,
		    &dom) != 0)
			break;
		if (first == RELIC_ID_NONE)
			first = dom;
		last = dom;
	}

	checkf(admitted == RELIC_INTEGER_MAX, SCENARIO_INTEGERS,
	    "the table should admit %d integers, not %zu", RELIC_INTEGER_MAX,
	    admitted);
	check(diagnosed(ctx, "too many distinct integers"), SCENARIO_INTEGERS,
	    "the refusal should say the table is full");

	status = relic_value_intern_integer(ctx, 0, &dom);
	check(status == 0 && dom == first, SCENARIO_INTEGERS,
	    "the first integer should intern to its identifier again");
	decoded_last = relic_value_decode(ctx, last, &decoded);
	check(decoded_last && decoded.type == RELIC_INTEGER &&
	    decoded.v.integer == -(int64_t)(admitted - 1), SCENARIO_INTEGERS,
	    "the last integer admitted should decode to its value");

	relic_destroy(ctx);
}

/*
 * A round that would give the table more integers than it can number fails
 * the evaluation with the table's message, the serial evaluator refusing
 * the batch before inserting any of it.  Run deferred, the round's merge
 * finds the table full before it has grown any relation, and the round
 * runs again serially: the two contexts end with the same storage.
 */
static void
a_deferred_round_stops_as_serially(size_t threads)
{
	relic_stats_t serial_stats;
	relic_stats_t deferred_stats;
	relic_t *serial;
	relic_t *deferred;
	int serial_status;
	int deferred_status;

	serial = computing_context(0, 0);
	deferred = computing_context(RELIC_F_PARALLEL_ALWAYS, threads);
	if (serial != NULL && deferred != NULL) {
		serial_status = relic_materialize(serial);
		deferred_status = relic_materialize(deferred);
		checkf(serial_status != 0 && deferred_status != 0,
		    SCENARIO_DEFERRED, "on %zu thread(s), both fail", threads);
		checkf(diagnosed(serial, "too many distinct integers") &&
		    diagnosed(deferred, "too many distinct integers"),
		    SCENARIO_DEFERRED, "on %zu thread(s), both say the table "
		    "is full", threads);
		checkf(deferred->pool != NULL &&
		    deferred->pool->abandoned_rounds > 0, SCENARIO_DEFERRED,
		    "on %zu thread(s), the round is abandoned", threads);

		relic_stats(serial, &serial_stats);
		relic_stats(deferred, &deferred_stats);
		checkf(serial_stats.storage_bytes ==
		    deferred_stats.storage_bytes &&
		    serial_stats.integer_values ==
		    deferred_stats.integer_values, SCENARIO_DEFERRED,
		    "on %zu thread(s), both hold %zu bytes and %zu integers, "
		    "not %zu and %zu", threads, serial_stats.storage_bytes,
		    serial_stats.integer_values, deferred_stats.storage_bytes,
		    deferred_stats.integer_values);
	}

	relic_destroy(serial);
	relic_destroy(deferred);
}

/*
 * A concatenation longer than a string value can be fails the evaluation
 * with an error naming the rule, and derives nothing from the binding.  One
 * reaching exactly the longest length is a value like any other.  Under
 * negation the error is not a dropped binding: the rule negating the failed
 * one derives nothing either, and a query over it fails with the query's
 * own message when its plan is the one that fails.
 */
static void
a_string_too_long_fails_the_evaluation(void)
{
	static const char program[] =
	    "w(\"" STRING_HALF "\").\n"
	    "twice(T) :- w(S), T = cat(S, S).\n"
	    "thrice(T) :- twice(S), w(W), T = cat(S, W).\n"
	    "long(S) :- w(S), cat(S, cat(S, S)) != \"\".\n"
	    "short(S) :- w(S), not long(S).\n";
	size_t rows;
	relic_t *ctx;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_TOO_LONG, "a context should be created");
	if (ctx == NULL)
		return;
	if (relic_load_string(ctx, "long.dl", program) != 0) {
		check(false, SCENARIO_TOO_LONG, "the program should load");
		relic_destroy(ctx);
		return;
	}

	check(relic_materialize(ctx) != 0 &&
	    diagnosed(ctx, "string too long in rule "), SCENARIO_TOO_LONG,
	    "a materialization should fail on the long string");

	check(query_rows(ctx, "twice(T), length(T) = " STRING_LIMIT_TEXT "?",
	    &rows) == 0 && rows == 1, SCENARIO_TOO_LONG,
	    "a string of exactly the longest length should be derived");
	check(query_rows(ctx, "thrice(T)?", &rows) != 0 && rows == 0 &&
	    diagnosed(ctx, "string too long in rule thrice/1"),
	    SCENARIO_TOO_LONG, "a rule concatenating past the limit should "
	    "fail its query, naming it, with no rows");
	check(query_rows(ctx, "short(S)?", &rows) != 0 && rows == 0 &&
	    diagnosed(ctx, "string too long in rule long/1"),
	    SCENARIO_TOO_LONG, "the negation of a failed rule should derive "
	    "nothing");
	check(query_rows(ctx, "w(S), T = cat(S, cat(S, S))?", &rows) != 0 &&
	    rows == 0 && diagnosed(ctx, "string too long in the query"),
	    SCENARIO_TOO_LONG, "a query concatenating past the limit should "
	    "fail with the query's message");

	relic_destroy(ctx);
}

/*
 * Count an error diagnostic in the size_t the callback context points at.
 */
static void
count_error(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_TEST_UNUSED,
    const char *message RELIC_TEST_UNUSED)
{
	size_t *errors;

	errors = callback_ctx;
	if (level == RELIC_ERROR)
		(*errors)++;
}

/*
 * Whether loading `text` draws exactly `errors` errors, the last naming
 * `expected` when one is expected, and succeeds exactly when it draws none.
 */
static bool
loads_with_errors(relic_t *ctx, const char *text, size_t errors,
    const char *expected)
{
	size_t counted;
	int status;

	counted = 0;
	relic_diag_set(ctx, count_error, &counted);
	status = relic_load_string(ctx, "lexed.dl", text);
	relic_diag_set(ctx, NULL, NULL);

	if (counted != errors || (status == 0) != (errors == 0))
		return (false);

	return (errors == 0 || diagnosed(ctx, expected));
}

/*
 * The lexer refuses a name or a string literal longer than a value may be
 * as it reads it, a literal by the length its escapes decode to, with one
 * message, and reads on: the statement after it is read as usual.  A name
 * or a literal of exactly the longest length is read.
 */
static void
a_name_or_literal_too_long_is_refused(void)
{
	char longest_name[] = "n" STRING_HALF STRING_HALF;
	char text[256];
	relic_t *ctx;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_LEXED, "a context should be created");
	if (ctx == NULL)
		return;

	longest_name[RELIC_STRING_LENGTH_MAX] = '\0';
	snprintf(text, sizeof(text), "%s(1).\n", longest_name);
	check(loads_with_errors(ctx, text, 0, NULL), SCENARIO_LEXED,
	    "a name of the longest length should be read");

	snprintf(text, sizeof(text), "%sx(1).\nok(1).\n", longest_name);
	check(loads_with_errors(ctx, text, 1, "a name of 65 bytes is longer "
	    "than the " STRING_LIMIT_TEXT " a name may hold"), SCENARIO_LEXED,
	    "a name one byte longer should draw one error");

	check(loads_with_errors(ctx, "s(\"" STRING_HALF
	    "0123456789abcdef0123456789abcd\\n\\t\").\n", 0, NULL),
	    SCENARIO_LEXED, "a literal decoding to the longest length "
	    "should be read");
	check(loads_with_errors(ctx, "s(\"" STRING_HALF
	    "0123456789abcdef0123456789abcde\\n\\t\").\nbad(.\n", 2,
	    "expected"), SCENARIO_LEXED,
	    "a literal decoding one byte longer should draw one error, and "
	    "the statement after it its own");
	check(loads_with_errors(ctx, "s(\"" STRING_HALF
	    "0123456789abcdef0123456789abcde\\n\\t\").\n", 1,
	    "a string literal of 65 bytes is longer than the "
	    STRING_LIMIT_TEXT " a value may hold"), SCENARIO_LEXED,
	    "a literal decoding one byte longer should be refused by its "
	    "decoded length");

	relic_destroy(ctx);
}

/*
 * An imported string or symbol longer than a value may be fails the import
 * at the field, with the field's location, as a malformed field does.  A
 * symbol field that is no symbol is refused as that, however long it is,
 * and a field of exactly the longest length is imported.
 */
static void
an_imported_field_too_long_is_refused(void)
{
	char text[512];
	char longest[] = STRING_HALF STRING_HALF;
	relic_t *ctx;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_IMPORTED, "a context should be created");
	if (ctx == NULL)
		return;

	snprintf(text, sizeof(text), "%s,a%s\n", longest, longest + 1);
	check(imports_with_error(ctx, text, NULL, 0, 0), SCENARIO_IMPORTED,
	    "a string and a symbol of the longest length should be imported");

	snprintf(text, sizeof(text), "ok,ok\nx%s,ok\n", longest);
	check(imports_with_error(ctx, text, "a symbol or string of 65 bytes "
	    "is longer than the " STRING_LIMIT_TEXT " a value may hold", 2, 1),
	    SCENARIO_IMPORTED, "a string one byte longer should be refused at "
	    "its field");

	snprintf(text, sizeof(text), "ok,a%s\n", longest);
	check(imports_with_error(ctx, text, "a symbol or string of 65 bytes "
	    "is longer than the " STRING_LIMIT_TEXT " a value may hold", 1, 4),
	    SCENARIO_IMPORTED, "a symbol one byte longer should be refused at "
	    "its field");

	snprintf(text, sizeof(text), "ok,A%s\n", longest);
	check(imports_with_error(ctx, text, "expected a symbol, found", 1, 4),
	    SCENARIO_IMPORTED, "a long field that is no symbol should be "
	    "refused as no symbol");

	relic_destroy(ctx);
}

/* Count an error, and keep where it is. */
static void
note_error(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message RELIC_TEST_UNUSED)
{
	import_errors_t *seen;

	seen = callback_ctx;
	if (level != RELIC_ERROR)
		return;

	seen->errors++;
	seen->line = where != NULL ? where->line : 0;
	seen->column = where != NULL ? where->column : 0;
}

/*
 * Whether importing `text` as csv facts of row(string, symbol) draws no
 * error and succeeds, when `expected` is NULL, or fails with one error,
 * naming `expected`, at `line` and `column`.
 */
static bool
imports_with_error(relic_t *ctx, const char *text, const char *expected,
    uint32_t line, uint32_t column)
{
	static const enum relic_column columns[] = {
		RELIC_COLUMN_STRING, RELIC_COLUMN_SYMBOL
	};
	relic_import_t import;
	import_errors_t seen;
	FILE *stream;
	int status;

	stream = tmpfile();
	if (stream == NULL || fputs(text, stream) == EOF ||
	    fseek(stream, 0, SEEK_SET) != 0) {
		if (stream != NULL)
			fclose(stream);
		return (false);
	}

	bzero(&import, sizeof(import));
	import.predicate = "row";
	import.columns = columns;
	import.column_count = sizeof(columns) / sizeof(columns[0]);
	import.format = RELIC_FORMAT_CSV;
	bzero(&seen, sizeof(seen));
	relic_diag_set(ctx, note_error, &seen);
	status = relic_import_file(ctx, "rows.csv", stream, &import);
	relic_diag_set(ctx, NULL, NULL);
	fclose(stream);

	if (expected == NULL)
		return (status == 0 && seen.errors == 0);

	return (status == -1 && seen.errors == 1 && seen.line == line &&
	    seen.column == column && diagnosed(ctx, expected));
}

/*
 * Answer a query, counting its rows into *rowsp.  Returns what
 * relic_query_run() returns, or -1 when the query was not prepared.
 */
static int
query_rows(relic_t *ctx, const char *text, size_t *rowsp)
{
	relic_query_t *query;
	int status;

	*rowsp = 0;
	query = relic_query_prepare(ctx, NULL, text);
	if (query == NULL)
		return (-1);

	status = relic_query_run(ctx, query, count_row, rowsp);
	relic_query_destroy(query);

	return (status);
}

static int
count_row(void *callback_ctx, const relic_value_t *values RELIC_TEST_UNUSED,
    size_t count RELIC_TEST_UNUSED)
{
	size_t *rowsp;

	rowsp = callback_ctx;
	(*rowsp)++;

	return (0);
}

/*
 * A context holding the forty integers and the rule computing from them.
 */
static relic_t *
computing_context(unsigned int flags, size_t threads)
{
	char program[COMPUTED_FACTS * 16 + 64];
	relic_config_t config;
	relic_t *ctx;
	size_t used;
	int i;

	used = 0;
	for (i = 0; i < COMPUTED_FACTS; i++)
		used += (size_t)snprintf(program + used, sizeof(program) - used,
		    "num(%d).\n", i);
	snprintf(program + used, sizeof(program) - used,
	    "far(N, M) :- num(N), M = N * 1000 + 1.\n");

	bzero(&config, sizeof(config));
	config.flags = flags;
	config.threads = threads;
	ctx = relic_create_config(&config);
	check(ctx != NULL, SCENARIO_DEFERRED, "a context should be created");
	if (ctx != NULL && relic_load_string(ctx, "far.dl", program) != 0) {
		check(false, SCENARIO_DEFERRED, "the program should load");
		relic_destroy(ctx);
		return (NULL);
	}

	return (ctx);
}

static bool
diagnosed(relic_t *ctx, const char *expected)
{
	const char *reported;

	reported = relic_error(ctx);

	return (reported != NULL && strstr(reported, expected) != NULL);
}

/*
 * The text of the `ordinal`th symbol a test interns.
 */
static void
symbol_text(char *text, size_t size, size_t ordinal)
{
	(void)snprintf(text, size, "s%zu", ordinal);
}

int
main(void)
{
	the_integer_table_stops_at_its_ceiling();
	the_symbol_pool_stops_at_its_ceiling();
	a_deferred_round_stops_as_serially(1);
	a_deferred_round_stops_as_serially(4);
	a_string_too_long_fails_the_evaluation();
	a_name_or_literal_too_long_is_refused();
	an_imported_field_too_long_is_refused();

	return (check_summary("ceiling"));
}
