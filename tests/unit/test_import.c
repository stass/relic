/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for importing csv and tsv tables.
 *
 * The reader is tested with no context at all, over byte strings: each
 * table's events are written out as a transcript and compared with one
 * written by hand from the formats' rules.  Every table is read twice, fed
 * whole and fed a byte at a time, and both readings must give the same
 * transcript, since where the input is cut must not change what a table
 * holds.
 *
 * The importer is tested through the library's entry points, over tables in
 * temporary files: what each column type accepts at its boundaries, what a
 * refused field is called in its message, every refusal of an import's
 * description, and the promises an import shares with a load -- all or
 * nothing, duplicates held once, a limit refusing at the commit, derived
 * facts discarded, caller code refused.
 */

#include "config.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_ALL_OR_NOTHING	"an import is all or nothing"
#define	SCENARIO_BOUNDED	"a refused import stops reading"
#define	SCENARIO_DERIVED	"an import discards what was derived"
#define	SCENARIO_DESCRIPTION	"a malformed description is refused unread"
#define	SCENARIO_DUPLICATES	"a fact is held once"
#define	SCENARIO_HEADER		"a header is read and not imported"
#define	SCENARIO_INTEGER	"an integer column holds the language's integers"
#define	SCENARIO_LOCATION	"a mistake is located in its source"
#define	SCENARIO_MESSAGE	"a message names the field it refuses"
#define	SCENARIO_READER		"the reader reads each format by its rules"
#define	SCENARIO_REENTRY	"a stream's read function cannot call in"
#define	SCENARIO_SKIP		"a skipped column interns nothing"
#define	SCENARIO_STRING		"a string column holds any bytes"
#define	SCENARIO_SYMBOL		"a symbol column holds the language's symbols"

/* A byte string with a length, for tables that hold NUL. */
#define	TABLE(text)	(text), (sizeof(text) - 1)

/* The longest transcript and message a test compares. */
#define	TRANSCRIPT_MAX	1024
#define	MESSAGE_MAX	512

typedef struct capture capture_t;
typedef struct reader_case reader_case_t;
typedef struct reentering_read reentering_read_t;

/*
 * One table and the transcript its reading must give.  `width` is the number
 * of fields each record must have.
 */
struct reader_case {
	const char		*table;
	size_t			 length;
	enum relic_format	 format;
	size_t			 width;
	const char		*transcript;
};

/*
 * The diagnostics a context reported: how many errors, and the message and
 * location of the last one.
 */
struct capture {
	char		message[MESSAGE_MAX];
	char		file[64];
	uint32_t	line;
	uint32_t	column;
	size_t		errors;
};

static void	 append_text(char *out, size_t size, const char *text);
static void	 check_tables(const reader_case_t *cases, size_t count);
static void	 capture_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static relic_t	*context_with_capture(const relic_config_t *config,
		    capture_t *capture);
static void	 describe_event(const relic_table_event_t *event, char *out,
		    size_t size);
static int	 import_table(relic_t *ctx, const char *name,
		    enum relic_format format, unsigned int flags,
		    const char *predicate, const enum relic_column *columns,
		    size_t column_count, const char *table, size_t length);
static ssize_t	 reenter_and_read(reentering_read_t *reading, char *buffer,
		    size_t size);
static FILE	*reentering_stream(reentering_read_t *reading);
static size_t	 rows_of(relic_t *ctx, const char *query);
static int	 import_counting(relic_t *ctx, const char *table,
		    size_t length, long *readp);
static char	*pairs_table(size_t rows, size_t values, size_t *lengthp);
static bool	 interned(const relic_t *ctx, const char *name);
static size_t	 row_offset(const char *table, size_t rows);
static FILE	*table_stream(const char *table, size_t length);
static void	 transcribe(const reader_case_t *table, size_t piece,
		    char *out, size_t size);

static void	 csv_is_read_by_its_rules(void);
static void	 every_refusal_reads_nothing(void);
static void	 facts_are_held_once(void);
static void	 a_failed_import_adds_nothing(void);
static void	 a_refused_import_stops_reading(void);
static void	 a_header_is_skipped(void);
static void	 a_long_field_crosses_reads(void);
static void	 a_description_is_read_once(void);
static void	 a_failed_read_adds_nothing(void);
static void	 a_path_is_opened_and_named(void);
static void	 a_stream_cannot_call_in(void);
static void	 an_import_discards_derived_facts(void);
static void	 integer_columns_hold_the_language_integers(void);
static void	 locations_stop_at_their_largest_value(void);
static void	 messages_quote_only_what_is_safe(void);
static void	 mistakes_name_their_source(void);
static void	 skipped_columns_intern_nothing(void);
static void	 string_columns_hold_any_bytes(void);
static void	 symbol_columns_hold_the_language_symbols(void);
static void	 tsv_is_read_by_its_rules(void);

/*
 * Append text to a transcript, cutting it at the buffer's end.
 */
static void
append_text(char *out, size_t size, const char *text)
{
	strlcat(out, text, size);
}

/*
 * One event, as a transcript writes it: "field 0 1:1 a", "record",
 * "end 2:1", or a mistake's name, location and detail.  A field's bytes are
 * written as they are when printable, and as \xHH otherwise, a space and a
 * backslash among the escaped so that a transcript is unambiguous.
 */
static void
describe_event(const relic_table_event_t *event, char *out, size_t size)
{
	static const char *const mistakes[] = {
		[RELIC_TABLE_TOO_MANY_FIELDS] = "too-many",
		[RELIC_TABLE_TOO_FEW_FIELDS] = "too-few",
		[RELIC_TABLE_UNKNOWN_ESCAPE] = "unknown-escape",
		[RELIC_TABLE_ESCAPE_AT_END] = "escape-at-end",
		[RELIC_TABLE_STRAY_RETURN] = "stray-return",
		[RELIC_TABLE_QUOTE_IN_UNQUOTED] = "quote-in-unquoted",
		[RELIC_TABLE_AFTER_CLOSING_QUOTE] = "after-closing-quote",
		[RELIC_TABLE_UNTERMINATED_QUOTE] = "unterminated"
	};
	char piece[64];
	size_t i;

	out[0] = '\0';

	switch (event->kind) {
	case RELIC_TABLE_FIELD:
		snprintf(piece, sizeof(piece), "field %zu %" PRIu32 ":%" PRIu32
		    " ", event->position, event->where.line,
		    event->where.column);
		append_text(out, size, piece);
		for (i = 0; i < event->length; i++) {
			unsigned char byte;

			byte = (unsigned char)event->bytes[i];
			if (byte > ' ' && byte <= '~' && byte != '\\')
				snprintf(piece, sizeof(piece), "%c", byte);
			else
				snprintf(piece, sizeof(piece), "\\x%02x", byte);
			append_text(out, size, piece);
		}
		return;
	case RELIC_TABLE_RECORD:
		append_text(out, size, "record");
		return;
	case RELIC_TABLE_END:
		snprintf(out, size, "end %" PRIu32 ":%" PRIu32,
		    event->where.line, event->where.column);
		return;
	case RELIC_TABLE_MISTAKE:
		snprintf(out, size, "%s %" PRIu32 ":%" PRIu32,
		    mistakes[event->mistake], event->where.line,
		    event->where.column);
		if (event->mistake == RELIC_TABLE_TOO_FEW_FIELDS) {
			snprintf(piece, sizeof(piece), " found %zu",
			    event->fields_found);
			append_text(out, size, piece);
		}
		if (event->mistake == RELIC_TABLE_UNKNOWN_ESCAPE ||
		    event->mistake == RELIC_TABLE_AFTER_CLOSING_QUOTE) {
			snprintf(piece, sizeof(piece), " 0x%02x", event->byte);
			append_text(out, size, piece);
		}
		return;
	case RELIC_TABLE_NO_MEMORY:
		append_text(out, size, "no-memory");
		return;
	case RELIC_TABLE_MORE:
		break;
	}

	append_text(out, size, "more");
}

/*
 * Read a table, feeding it `piece` bytes at a time, and write the events as
 * a transcript separated by " | ", up to and including the one that stops
 * the reader.
 */
static void
transcribe(const reader_case_t *table, size_t piece, char *out, size_t size)
{
	relic_table_reader_t reader;
	relic_table_event_t event;
	char described[256];
	size_t fed;

	out[0] = '\0';
	fed = 0;
	relic_table_reader_init(&reader, table->format, table->width, "t");

	for (;;) {
		enum relic_table_event_kind kind;

		kind = relic_table_next(&reader, &event);
		if (kind == RELIC_TABLE_MORE) {
			size_t length;

			length = table->length - fed;
			if (length > piece)
				length = piece;
			relic_table_feed(&reader, table->table + fed, length,
			    fed + length == table->length);
			fed += length;
			continue;
		}

		describe_event(&event, described, sizeof(described));
		if (out[0] != '\0')
			append_text(out, size, " | ");
		append_text(out, size, described);

		if (kind != RELIC_TABLE_FIELD && kind != RELIC_TABLE_RECORD)
			break;
	}

	relic_table_reader_reset(&reader);
}

/*
 * Check each table's transcript, read whole and read a byte at a time.
 */
static void
check_tables(const reader_case_t *cases, size_t count)
{
	char whole[TRANSCRIPT_MAX];
	char bytewise[TRANSCRIPT_MAX];
	size_t i;

	for (i = 0; i < count; i++) {
		transcribe(&cases[i], cases[i].length + 1, whole,
		    sizeof(whole));
		transcribe(&cases[i], 1, bytewise, sizeof(bytewise));

		checkf(strcmp(whole, cases[i].transcript) == 0, SCENARIO_READER,
		    "table %zu should read as \"%s\", not \"%s\"", i,
		    cases[i].transcript, whole);
		checkf(strcmp(bytewise, whole) == 0, SCENARIO_READER,
		    "table %zu fed a byte at a time should read as \"%s\", "
		    "not \"%s\"", i, whole, bytewise);
	}
}

static void
tsv_is_read_by_its_rules(void)
{
	static const reader_case_t cases[] = {
		/* Records, and the end of the input in each state. */
		{ TABLE(""), RELIC_FORMAT_TSV, 1, "end 1:1" },
		{ TABLE("a\tb\n"), RELIC_FORMAT_TSV, 2,
		    "field 0 1:1 a | field 1 1:3 b | record | end 2:1" },
		{ TABLE("a\tb"), RELIC_FORMAT_TSV, 2,
		    "field 0 1:1 a | field 1 1:3 b | record | end 1:4" },
		{ TABLE("a\t"), RELIC_FORMAT_TSV, 2,
		    "field 0 1:1 a | field 1 1:3  | record | end 1:3" },
		{ TABLE("\n"), RELIC_FORMAT_TSV, 1,
		    "field 0 1:1  | record | end 2:1" },
		{ TABLE("a\r\nb\r\n"), RELIC_FORMAT_TSV, 1,
		    "field 0 1:1 a | record | field 0 2:1 b | record | "
		    "end 3:1" },
		{ TABLE("\r\n"), RELIC_FORMAT_TSV, 1,
		    "field 0 1:1  | record | end 2:1" },

		/* The four escapes, and the refused ones. */
		{ TABLE("\\\\\\t\\n\\r"), RELIC_FORMAT_TSV, 1,
		    "field 0 1:1 \\x5c\\x09\\x0a\\x0d | record | end 1:9" },
		{ TABLE("a\\q"), RELIC_FORMAT_TSV, 1,
		    "unknown-escape 1:3 0x71" },
		{ TABLE("a\\"), RELIC_FORMAT_TSV, 1, "escape-at-end 1:3" },
		{ TABLE("a\\\tb"), RELIC_FORMAT_TSV, 2,
		    "unknown-escape 1:3 0x09" },
		{ TABLE("a\\\n"), RELIC_FORMAT_TSV, 1,
		    "unknown-escape 1:3 0x0a" },
		{ TABLE("\\N"), RELIC_FORMAT_TSV, 1,
		    "unknown-escape 1:2 0x4e" },

		/* A carriage return is part of a line end or a mistake. */
		{ TABLE("a\rb"), RELIC_FORMAT_TSV, 1, "stray-return 1:2" },
		{ TABLE("a\r"), RELIC_FORMAT_TSV, 1, "stray-return 1:2" },
		{ TABLE("a\r\r\n"), RELIC_FORMAT_TSV, 1, "stray-return 1:2" },

		/* A quote, a comma, NUL and bytes above 0x7f are data. */
		{ TABLE("\"a,b\"\t\0\xff"), RELIC_FORMAT_TSV, 2,
		    "field 0 1:1 \"a,b\" | field 1 1:7 \\x00\\xff | record | "
		    "end 1:9" },

		/* A record's width, and the first of two mistakes. */
		{ TABLE("a\tb\tc\n"), RELIC_FORMAT_TSV, 2,
		    "field 0 1:1 a | field 1 1:3 b | too-many 1:5" },
		{ TABLE("a\tb\t"), RELIC_FORMAT_TSV, 2,
		    "field 0 1:1 a | field 1 1:3 b | too-many 1:5" },
		{ TABLE("a\n"), RELIC_FORMAT_TSV, 2,
		    "field 0 1:1 a | too-few 1:2 found 1" },
		{ TABLE("\n"), RELIC_FORMAT_TSV, 2,
		    "field 0 1:1  | too-few 1:1 found 1" },
		{ TABLE("a"), RELIC_FORMAT_TSV, 3,
		    "field 0 1:1 a | too-few 1:2 found 1" },
		{ TABLE("a\r\n"), RELIC_FORMAT_TSV, 2,
		    "field 0 1:1 a | too-few 1:2 found 1" },
		{ TABLE("a\\q\tb\tc"), RELIC_FORMAT_TSV, 2,
		    "unknown-escape 1:3 0x71" },
		{ TABLE("a\tb\tc\\q"), RELIC_FORMAT_TSV, 2,
		    "field 0 1:1 a | field 1 1:3 b | too-many 1:5" }
	};

	check_tables(cases, sizeof(cases) / sizeof(cases[0]));
}

static void
csv_is_read_by_its_rules(void)
{
	static const reader_case_t cases[] = {
		{ TABLE(""), RELIC_FORMAT_CSV, 1, "end 1:1" },
		{ TABLE("a,b\r\nc,d"), RELIC_FORMAT_CSV, 2,
		    "field 0 1:1 a | field 1 1:3 b | record | "
		    "field 0 2:1 c | field 1 2:3 d | record | end 2:4" },
		{ TABLE(","), RELIC_FORMAT_CSV, 2,
		    "field 0 1:1  | field 1 1:2  | record | end 1:2" },

		/* Quoted fields hold separators, quotes and line ends. */
		{ TABLE("a,\"b,c\"\n"), RELIC_FORMAT_CSV, 2,
		    "field 0 1:1 a | field 1 1:3 b,c | record | end 2:1" },
		{ TABLE("\"a\"\"b\""), RELIC_FORMAT_CSV, 1,
		    "field 0 1:1 a\"b | record | end 1:7" },
		{ TABLE("\"\"\"\""), RELIC_FORMAT_CSV, 1,
		    "field 0 1:1 \" | record | end 1:5" },
		{ TABLE("\"\",\"\""), RELIC_FORMAT_CSV, 2,
		    "field 0 1:1  | field 1 1:4  | record | end 1:6" },
		{ TABLE("\"a\r\nb\",c\n"), RELIC_FORMAT_CSV, 2,
		    "field 0 1:1 a\\x0d\\x0ab | field 1 2:4 c | record | "
		    "end 3:1" },
		{ TABLE("\"a\rb\""), RELIC_FORMAT_CSV, 1,
		    "field 0 1:1 a\\x0db | record | end 1:6" },
		{ TABLE("\"a\nb\"\nc"), RELIC_FORMAT_CSV, 1,
		    "field 0 1:1 a\\x0ab | record | field 0 3:1 c | record | "
		    "end 3:2" },
		{ TABLE("\"a\"\r\n"), RELIC_FORMAT_CSV, 1,
		    "field 0 1:1 a | record | end 2:1" },
		{ TABLE("\"\\\0\xff\""), RELIC_FORMAT_CSV, 1,
		    "field 0 1:1 \\x5c\\x00\\xff | record | end 1:6" },

		/* Misplaced quotes, and a quoted field left open. */
		{ TABLE("a\"b"), RELIC_FORMAT_CSV, 1,
		    "quote-in-unquoted 1:2" },
		{ TABLE("\"a\"x"), RELIC_FORMAT_CSV, 1,
		    "after-closing-quote 1:4 0x78" },
		{ TABLE("\"a\" ,b"), RELIC_FORMAT_CSV, 2,
		    "after-closing-quote 1:4 0x20" },
		{ TABLE("x,\"abc\ndef"), RELIC_FORMAT_CSV, 2,
		    "field 0 1:1 x | unterminated 1:3" },
		{ TABLE("\"a\"\rb"), RELIC_FORMAT_CSV, 1, "stray-return 1:4" },

		/* An unquoted field. */
		{ TABLE("a\rb"), RELIC_FORMAT_CSV, 1, "stray-return 1:2" },
		{ TABLE("a\\t b\n"), RELIC_FORMAT_CSV, 1,
		    "field 0 1:1 a\\x5ct\\x20b | record | end 2:1" },

		/* A record's width. */
		{ TABLE("a,b,c"), RELIC_FORMAT_CSV, 2,
		    "field 0 1:1 a | field 1 1:3 b | too-many 1:5" },
		{ TABLE("a\n"), RELIC_FORMAT_CSV, 2,
		    "field 0 1:1 a | too-few 1:2 found 1" },
		{ TABLE("a\"b,c,d"), RELIC_FORMAT_CSV, 2,
		    "quote-in-unquoted 1:2" }
	};

	check_tables(cases, sizeof(cases) / sizeof(cases[0]));
}

/*
 * A table longer than a location can count reports its later lines at the
 * last one it can name.  The reader's location is set near the end of its
 * range rather than reached, which would take four billion lines.
 */
static void
locations_stop_at_their_largest_value(void)
{
	relic_table_reader_t reader;
	relic_table_event_t event;
	static const char table[] = "a\nb\nc\\q";
	enum relic_table_event_kind kind;

	relic_table_reader_init(&reader, RELIC_FORMAT_TSV, 1, "t");
	reader.where.line = UINT32_MAX - 1;
	relic_table_feed(&reader, table, sizeof(table) - 1, true);

	do {
		kind = relic_table_next(&reader, &event);
	} while (kind == RELIC_TABLE_FIELD || kind == RELIC_TABLE_RECORD);

	check(kind == RELIC_TABLE_MISTAKE && event.where.line == UINT32_MAX &&
	    event.where.column == 3, SCENARIO_READER,
	    "a mistake past the last line should be at that line");

	relic_table_reader_reset(&reader);

	/* A line longer than a column can count stops at the last column. */
	relic_table_reader_init(&reader, RELIC_FORMAT_TSV, 1, "t");
	reader.where.column = UINT32_MAX - 1;
	relic_table_feed(&reader, "ab\\q", 5, true);

	kind = relic_table_next(&reader, &event);
	check(kind == RELIC_TABLE_MISTAKE && event.where.line == 1 &&
	    event.where.column == UINT32_MAX, SCENARIO_READER,
	    "a mistake past the last column should be at that column");

	relic_table_reader_reset(&reader);
}

/*
 * Record a diagnostic's message and location when it is an error.
 */
static void
capture_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	capture_t *capture;

	capture = callback_ctx;
	if (level != RELIC_ERROR)
		return;

	capture->errors++;
	strlcpy(capture->message, message, sizeof(capture->message));
	capture->file[0] = '\0';
	capture->line = 0;
	capture->column = 0;
	if (where != NULL && where->file != NULL) {
		strlcpy(capture->file, where->file, sizeof(capture->file));
		capture->line = where->line;
		capture->column = where->column;
	}
}

static relic_t *
context_with_capture(const relic_config_t *config, capture_t *capture)
{
	relic_t *ctx;

	bzero(capture, sizeof(*capture));

	ctx = relic_create_config(config);
	if (ctx == NULL)
		return (NULL);
	relic_diag_set(ctx, capture_diagnostic, capture);

	return (ctx);
}

/*
 * A stream holding the table, positioned at its start.  A temporary file
 * serves a table of any length, an empty one included.
 */
static FILE *
table_stream(const char *table, size_t length)
{
	FILE *stream;

	stream = tmpfile();
	if (stream == NULL)
		return (NULL);

	if (fwrite(table, 1, length, stream) != length ||
	    fseek(stream, 0, SEEK_SET) != 0) {
		fclose(stream);
		return (NULL);
	}

	return (stream);
}

/*
 * Import a table held in memory under `name`.  Returns the import's status,
 * or -2 when the table could not be put in a stream.
 */
static int
import_table(relic_t *ctx, const char *name, enum relic_format format,
    unsigned int flags, const char *predicate,
    const enum relic_column *columns, size_t column_count, const char *table,
    size_t length)
{
	relic_import_t import;
	FILE *stream;
	int status;

	stream = table_stream(table, length);
	if (stream == NULL)
		return (-2);

	bzero(&import, sizeof(import));
	import.predicate = predicate;
	import.columns = columns;
	import.column_count = column_count;
	import.format = format;
	import.flags = flags;

	status = relic_import_file(ctx, name, stream, &import);
	fclose(stream);

	return (status);
}

/*
 * How many rows a query answers, or SIZE_MAX when it cannot be answered.
 */
static size_t
rows_of(relic_t *ctx, const char *query)
{
	relic_query_t *prepared;
	relic_result_t *result;
	size_t rows;

	prepared = relic_query_prepare(ctx, NULL, query);
	if (prepared == NULL)
		return (SIZE_MAX);

	result = relic_query_collect(ctx, prepared);
	relic_query_destroy(prepared);
	if (result == NULL)
		return (SIZE_MAX);

	rows = relic_result_rows(result);
	relic_result_destroy(result);

	return (rows);
}

static void
symbol_columns_hold_the_language_symbols(void)
{
	static const enum relic_column symbol[] = { RELIC_COLUMN_SYMBOL };
	static const struct {
		const char	*field;
		const char	*message;	/* NULL when accepted */
	} cases[] = {
		{ "a", NULL },
		{ "aB_9", NULL },
		{ "not", NULL },
		{ "A", "expected a symbol, found \"A\"" },
		{ "_a", "expected a symbol, found \"_a\"" },
		{ "1a", "expected a symbol, found \"1a\"" },
		{ "", "expected a symbol, found an empty field" },
		{ "a b", "expected a symbol, found \"a b\"" }
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		capture_t capture;
		relic_t *ctx;
		char table[64];
		int status;

		ctx = context_with_capture(NULL, &capture);
		if (ctx == NULL)
			return;

		snprintf(table, sizeof(table), "%s\n", cases[i].field);
		status = import_table(ctx, "s.tsv", RELIC_FORMAT_TSV, 0, "s",
		    symbol, 1, table, strlen(table));

		if (cases[i].message == NULL) {
			checkf(status == 0 && rows_of(ctx, "s(X)?") == 1,
			    SCENARIO_SYMBOL, "%s should import as a symbol",
			    cases[i].field);
		} else {
			checkf(status == -1 &&
			    strcmp(capture.message, cases[i].message) == 0,
			    SCENARIO_SYMBOL, "\"%s\" should be refused with "
			    "'%s', not '%s'", cases[i].field, cases[i].message,
			    capture.message);
		}

		relic_destroy(ctx);
	}
}

static void
integer_columns_hold_the_language_integers(void)
{
	static const enum relic_column integer[] = { RELIC_COLUMN_INTEGER };
	static const struct {
		const char	*table;
		enum relic_format format;
		const char	*query;		/* holds when accepted */
		const char	*message;	/* NULL when accepted */
	} cases[] = {
		{ "9223372036854775807", RELIC_FORMAT_TSV,
		    "n(9223372036854775807)?", NULL },
		{ "-9223372036854775808", RELIC_FORMAT_TSV,
		    "n(-9223372036854775808)?", NULL },
		{ "-0", RELIC_FORMAT_TSV, "n(0)?", NULL },
		{ "007", RELIC_FORMAT_TSV, "n(7)?", NULL },
		{ "\"7\"", RELIC_FORMAT_CSV, "n(7)?", NULL },
		{ "9223372036854775808", RELIC_FORMAT_TSV, NULL,
		    "integer out of range: \"9223372036854775808\"" },
		{ "-9223372036854775809", RELIC_FORMAT_TSV, NULL,
		    "integer out of range: \"-9223372036854775809\"" },
		{ "-", RELIC_FORMAT_TSV, NULL,
		    "expected an integer, found \"-\"" },
		{ "+1", RELIC_FORMAT_TSV, NULL,
		    "expected an integer, found \"+1\"" },
		{ " 7", RELIC_FORMAT_TSV, NULL,
		    "expected an integer, found \" 7\"" },
		{ "7 ", RELIC_FORMAT_TSV, NULL,
		    "expected an integer, found \"7 \"" },
		{ "7x", RELIC_FORMAT_TSV, NULL,
		    "expected an integer, found \"7x\"" },
		{ "1-", RELIC_FORMAT_TSV, NULL,
		    "expected an integer, found \"1-\"" },
		{ "1.2", RELIC_FORMAT_TSV, NULL,
		    "expected an integer, found \"1.2\"" },
		{ "1e5", RELIC_FORMAT_TSV, NULL,
		    "expected an integer, found \"1e5\"" },
		{ "", RELIC_FORMAT_CSV, NULL,
		    "expected an integer, found an empty field" }
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		capture_t capture;
		relic_t *ctx;
		int status;

		ctx = context_with_capture(NULL, &capture);
		if (ctx == NULL)
			return;

		/*
		 * An empty table has no record, so the empty field is written
		 * as the empty line that is the one field it holds.
		 */
		if (cases[i].table[0] == '\0')
			status = import_table(ctx, "n", cases[i].format, 0,
			    "n", integer, 1, TABLE("\n"));
		else
			status = import_table(ctx, "n", cases[i].format, 0,
			    "n", integer, 1, cases[i].table,
			    strlen(cases[i].table));

		if (cases[i].message == NULL) {
			checkf(status == 0 && rows_of(ctx, cases[i].query) == 1,
			    SCENARIO_INTEGER, "%s should import as the integer "
			    "%s names", cases[i].table, cases[i].query);
		} else {
			checkf(status == -1 &&
			    strcmp(capture.message, cases[i].message) == 0,
			    SCENARIO_INTEGER, "\"%s\" should be refused with "
			    "'%s', not '%s'", cases[i].table, cases[i].message,
			    capture.message);
		}

		relic_destroy(ctx);
	}
}

static void
string_columns_hold_any_bytes(void)
{
	static const enum relic_column string[] = { RELIC_COLUMN_STRING };
	static const char with_bytes[] = { 'a', '\0', '\xff', ' ' };
	capture_t capture;
	relic_value_t value;
	relic_t *ctx;

	ctx = context_with_capture(NULL, &capture);
	if (ctx == NULL)
		return;

	check(import_table(ctx, "s", RELIC_FORMAT_CSV, 0, "s", string, 1,
	    TABLE("\n\"a\0\xff \"\n")) == 0, SCENARIO_STRING,
	    "an empty field and one of NUL and 0xff should import");

	check(relic_value_string(ctx, "", 0, &value) == 0 &&
	    relic_fact_add(ctx, "s", 1, &value) == 0 &&
	    relic_value_string(ctx, with_bytes, sizeof(with_bytes),
	    &value) == 0 && relic_fact_add(ctx, "s", 1, &value) == 0 &&
	    rows_of(ctx, "s(X)?") == 2, SCENARIO_STRING,
	    "the imported strings should be the strings those bytes build");

	relic_destroy(ctx);
}

/*
 * A skipped field is read and dropped: it interns no value, however many
 * distinct ones the column holds.
 */
static void
skipped_columns_intern_nothing(void)
{
	static const enum relic_column columns[] = {
		RELIC_COLUMN_SKIP, RELIC_COLUMN_SYMBOL, RELIC_COLUMN_SKIP
	};
	capture_t capture;
	relic_t *ctx;
	size_t before;

	ctx = context_with_capture(NULL, &capture);
	if (ctx == NULL)
		return;

	before = relic_value_count(ctx);
	check(import_table(ctx, "w", RELIC_FORMAT_TSV, 0, "w", columns, 3,
	    TABLE("one\ta\tx\\ty\ntwo\ta\tz\nthree\tb\t\n")) == 0,
	    SCENARIO_SKIP, "a table with skipped columns should import");
	check(relic_value_count(ctx) == before + 2, SCENARIO_SKIP,
	    "only the kept column's two values should be interned");
	check(rows_of(ctx, "w(X)?") == 2, SCENARIO_SKIP,
	    "the kept column should give w/1 two facts");

	/*
	 * An import of one skipped column has arity zero: an empty line is
	 * the row of no columns, and every record gives the one fact.
	 */
	check(import_table(ctx, "z", RELIC_FORMAT_CSV, 0, "z", columns, 1,
	    TABLE("\nx\n\n")) == 0 && rows_of(ctx, "z()?") == 1,
	    SCENARIO_SKIP, "records of one skipped column should give z/0 "
	    "its fact");

	relic_destroy(ctx);
}

/*
 * A field is quoted in a message only when every byte of it is printable
 * ASCII other than a quote or a backslash, and is otherwise named by the
 * first byte that is not.
 */
static void
messages_quote_only_what_is_safe(void)
{
	static const enum relic_column symbol[] = { RELIC_COLUMN_SYMBOL };
	static const struct {
		const char	*table;
		const char	*message;
	} cases[] = {
		{ "a\\tb\n", "expected a symbol, found a field holding 0x09" },
		{ "a\\\\b\n", "expected a symbol, found a field holding 0x5c" },
		{ "a\"b\n", "expected a symbol, found a field holding '\"'" },
		{ "Caf\xc3\xa9\n",
		    "expected a symbol, found a field holding 0xc3" },
		{ "A123456789012345678901234567890123456789XYZ\n",
		    "expected a symbol, found "
		    "\"A123456789012345678901234567890123456789\"..." },
		{ "A123456789012345678901234567890123456789\n",
		    "expected a symbol, found "
		    "\"A123456789012345678901234567890123456789\"" }
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		capture_t capture;
		relic_t *ctx;

		ctx = context_with_capture(NULL, &capture);
		if (ctx == NULL)
			return;

		import_table(ctx, "m", RELIC_FORMAT_TSV, 0, "m", symbol, 1,
		    cases[i].table, strlen(cases[i].table));
		checkf(strcmp(capture.message, cases[i].message) == 0,
		    SCENARIO_MESSAGE, "case %zu should say '%s', not '%s'", i,
		    cases[i].message, capture.message);

		relic_destroy(ctx);
	}
}

/*
 * A mistake is located in the source the import names, or in "<input>"
 * when it names none, at the byte the reader should look at.
 */
static void
mistakes_name_their_source(void)
{
	static const enum relic_column columns[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_INTEGER
	};
	capture_t capture;
	relic_t *ctx;

	ctx = context_with_capture(NULL, &capture);
	if (ctx == NULL)
		return;

	import_table(ctx, "people.csv", RELIC_FORMAT_CSV, 0, "p", columns, 2,
	    TABLE("ann,1\n\"bob\",x\n"));
	check(strcmp(capture.file, "people.csv") == 0 && capture.line == 2 &&
	    capture.column == 7 &&
	    strcmp(capture.message, "expected an integer, found \"x\"") == 0,
	    SCENARIO_LOCATION, "the refused field should be at people.csv:2:7");

	import_table(ctx, NULL, RELIC_FORMAT_CSV, 0, "p", columns, 2,
	    TABLE("ann,1,2\n"));
	check(strcmp(capture.file, "<input>") == 0 && capture.line == 1 &&
	    capture.column == 7 &&
	    strcmp(capture.message, "expected 2 fields, found more than 2") ==
	    0, SCENARIO_LOCATION,
	    "an extra field should be at <input>:1:7");

	import_table(ctx, "t", RELIC_FORMAT_CSV, 0, "p", columns, 2,
	    TABLE("ann\n"));
	check(capture.line == 1 && capture.column == 4 &&
	    strcmp(capture.message, "expected 2 fields, found 1") == 0,
	    SCENARIO_LOCATION, "a short record should end at t:1:4");

	import_table(ctx, "t", RELIC_FORMAT_CSV, 0, "p", columns, 2,
	    TABLE("a,1\n\"b,2\n"));
	check(capture.line == 2 && capture.column == 1 &&
	    strcmp(capture.message, "unterminated quoted field") == 0,
	    SCENARIO_LOCATION, "an open quote should be reported where it opens");

	import_table(ctx, "t", RELIC_FORMAT_TSV, 0, "p", columns, 2,
	    TABLE("a\\q\t1\n"));
	check(capture.line == 1 && capture.column == 3 &&
	    strcmp(capture.message,
	    "unknown escape: a backslash followed by 'q'") == 0,
	    SCENARIO_LOCATION, "an unknown escape should name its byte");

	import_table(ctx, "t", RELIC_FORMAT_CSV, 0, "p", columns, 2,
	    TABLE("\"a\"\\,1\n"));
	check(capture.line == 1 && capture.column == 4 &&
	    strcmp(capture.message, "expected ',' or the end of the line "
	    "after a closing quote, found 0x5c") == 0, SCENARIO_LOCATION,
	    "a backslash after a closing quote should be named by its value");

	/*
	 * A field is typed when it ends, before the next field begins, so a
	 * refused field is reported ahead of the extra field after it.
	 */
	import_table(ctx, "t", RELIC_FORMAT_CSV, 0, "p", columns, 2,
	    TABLE("a,x,3\n"));
	check(capture.line == 1 && capture.column == 3 &&
	    strcmp(capture.message, "expected an integer, found \"x\"") == 0,
	    SCENARIO_LOCATION, "the first of two mistakes should be reported");

	relic_destroy(ctx);
}

/*
 * The header record is read by the format's rules and must have a field per
 * column, and is then dropped.  The flag says a header is there, so a table
 * with no record at all is refused.
 */
static void
a_header_is_skipped(void)
{
	static const enum relic_column columns[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_INTEGER
	};
	capture_t capture;
	relic_t *ctx;

	ctx = context_with_capture(NULL, &capture);
	if (ctx == NULL)
		return;

	check(import_table(ctx, "h", RELIC_FORMAT_CSV, RELIC_IMPORT_HEADER,
	    "h", columns, 2, TABLE("Name,Age\nann,7\n")) == 0 &&
	    rows_of(ctx, "h(X, Y)?") == 1, SCENARIO_HEADER,
	    "a header over an integer column should be skipped, not typed");

	check(import_table(ctx, "h", RELIC_FORMAT_CSV, RELIC_IMPORT_HEADER,
	    "h", columns, 2, TABLE("Name,Age,Town\nbob,8\n")) == -1 &&
	    capture.line == 1 && capture.column == 10 &&
	    rows_of(ctx, "h(X, Y)?") == 1, SCENARIO_HEADER,
	    "a header of the wrong width should be refused");

	check(import_table(ctx, "h", RELIC_FORMAT_CSV, RELIC_IMPORT_HEADER,
	    "h", columns, 2, TABLE("")) == -1 && capture.line == 1 &&
	    capture.column == 1 && strcmp(capture.message,
	    "expected a header, found the end of the input") == 0,
	    SCENARIO_HEADER, "an empty table should lack its header");

	check(import_table(ctx, "h", RELIC_FORMAT_CSV, RELIC_IMPORT_HEADER,
	    "h", columns, 2, TABLE("Name,Age")) == 0 &&
	    rows_of(ctx, "h(X, Y)?") == 1, SCENARIO_HEADER,
	    "a table of a header alone should import nothing");

	relic_destroy(ctx);
}

/*
 * Every refusal of a description: nothing is read from the stream, nothing
 * is added, and the reason is reported.  A predicate name or a number of
 * columns the language cannot have is rejected, as it would be in text;
 * every other mistake is in how the description was filled in, and is a
 * malformed call.
 */
static void
every_refusal_reads_nothing(void)
{
	static const enum relic_column two[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_SYMBOL
	};
	static const enum relic_column zeroed[] = {
		RELIC_COLUMN_SYMBOL, 0
	};
	static const enum relic_column past[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_SKIP + 1
	};
	enum relic_column wide[RELIC_MAX_ARITY + 2];
	relic_import_t good;
	relic_import_t import;
	capture_t capture;
	relic_t *ctx;
	FILE *stream;
	size_t symbols;
	size_t i;

	for (i = 0; i < RELIC_MAX_ARITY + 2; i++)
		wide[i] = RELIC_COLUMN_STRING;
	wide[0] = RELIC_COLUMN_SKIP;

	bzero(&good, sizeof(good));
	good.predicate = "e";
	good.columns = two;
	good.column_count = 2;
	good.format = RELIC_FORMAT_TSV;

	ctx = context_with_capture(NULL, &capture);
	if (ctx == NULL)
		return;

	stream = table_stream(TABLE("a\tb\n"));
	if (stream == NULL) {
		relic_destroy(ctx);
		return;
	}

	for (i = 0;; i++) {
		enum relic_error_class expected_class;
		const char *expected;

		import = good;
		expected_class = RELIC_ERROR_ARGUMENT;
		switch (i) {
		case 0:
			bzero(&import, sizeof(import));
			expected = "an import needs a predicate name";
			break;
		case 1:
			import.predicate = "Edge";
			expected = "a predicate name is";
			expected_class = RELIC_ERROR_REJECTED;
			break;
		case 2:
			import.predicate = "not";
			expected = "a predicate name is";
			expected_class = RELIC_ERROR_REJECTED;
			break;
		case 3:
			import.column_count = 0;
			expected = "an import needs at least one column";
			break;
		case 4:
			import.columns = NULL;
			expected = "an import needs at least one column";
			break;
		case 5:
			import.columns = zeroed;
			expected = "column 2 of the import has no type";
			break;
		case 6:
			import.columns = past;
			expected = "column 2 of the import has no type";
			break;
		case 7:
			import.columns = wide;
			import.column_count = RELIC_MAX_ARITY + 2;
			expected = "an import may keep at most 32 columns";
			expected_class = RELIC_ERROR_REJECTED;
			break;
		case 8:
			import.format = 0;
			expected = "the import's format is neither csv nor tsv";
			break;
		case 9:
			import.format = RELIC_FORMAT_TSV + 1;
			expected = "the import's format is neither csv nor tsv";
			break;
		case 10:
			import.flags = 0x2u;
			expected = "the import's flags hold bits this library "
			    "does not define: 0x2";
			break;
		default:
			expected = NULL;
			break;
		}
		if (expected == NULL)
			break;

		capture.errors = 0;
		checkf(relic_import_file(ctx, "e", stream, &import) == -1 &&
		    capture.errors == 1 && strncmp(capture.message, expected,
		    strlen(expected)) == 0 && ftell(stream) == 0,
		    SCENARIO_DESCRIPTION, "description %zu should be refused "
		    "with '%s' and nothing read, not '%s'", i, expected,
		    capture.message);
		checkf(relic_error_class(ctx) == expected_class,
		    SCENARIO_DESCRIPTION, "description %zu should be refused "
		    "as %s", i, expected_class == RELIC_ERROR_REJECTED ?
		    "a name or arity the language rejects" :
		    "a malformed call");
	}

	check(relic_import_file(ctx, "e", stream, NULL) == -1 &&
	    strcmp(capture.message, "no import description was given") == 0 &&
	    ftell(stream) == 0, SCENARIO_DESCRIPTION,
	    "no description should be refused");

	/*
	 * No stream and no path are refused before the description is read,
	 * so a predicate the context has never seen is not interned.
	 */
	import = good;
	import.predicate = "unread";
	symbols = relic_symbol_count(ctx);
	check(relic_import_file(ctx, "e", NULL, &import) == -1 &&
	    strcmp(capture.message, "no input stream was given") == 0,
	    SCENARIO_DESCRIPTION, "no stream should be refused");
	check(relic_import_path(ctx, NULL, &import) == -1 &&
	    strcmp(capture.message, "no path was given") == 0,
	    SCENARIO_DESCRIPTION, "no path should be refused");
	check(relic_symbol_count(ctx) == symbols, SCENARIO_DESCRIPTION,
	    "no stream and no path should intern nothing");

	/* The description is checked before the path is opened. */
	import = good;
	import.format = 0;
	check(relic_import_path(ctx, "/nonexistent/e.tsv", &import) == -1 &&
	    strcmp(capture.message,
	    "the import's format is neither csv nor tsv") == 0,
	    SCENARIO_DESCRIPTION, "a bad description should not open its path");

	/* Thirty-two kept columns, and a zeroed description, beside them. */
	import = good;
	import.columns = wide;
	import.column_count = RELIC_MAX_ARITY + 1;
	check(relic_import_file(ctx, "e", stream, &import) == -1 &&
	    strcmp(capture.message, "expected 33 fields, found 2") == 0,
	    SCENARIO_DESCRIPTION, "thirty-two kept columns should be read");

	check(rows_of(ctx, "e(X, Y)?") == 0, SCENARIO_DESCRIPTION,
	    "no refused import should add a fact");

	fclose(stream);
	relic_destroy(ctx);
}

/*
 * A failed import adds none of its facts, whether a record is malformed or
 * a limit refuses the commit, and the base stores stay as they were.
 */
static void
a_failed_import_adds_nothing(void)
{
	static const enum relic_column columns[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_SYMBOL
	};
	relic_config_t config;
	capture_t capture;
	relic_t *ctx;

	ctx = context_with_capture(NULL, &capture);
	if (ctx == NULL)
		return;

	check(import_table(ctx, "e", RELIC_FORMAT_TSV, 0, "e", columns, 2,
	    TABLE("a\tb\nb\tc\nc\n")) == -1 && rows_of(ctx, "e(X, Y)?") == 0,
	    SCENARIO_ALL_OR_NOTHING,
	    "a malformed last record should add none of the good ones");

	relic_destroy(ctx);

	bzero(&config, sizeof(config));
	config.max_tuples = 2;
	ctx = context_with_capture(&config, &capture);
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "f.dl", "e(z, z).\n") == 0,
	    SCENARIO_ALL_OR_NOTHING, "one fact should load under the limit");
	check(import_table(ctx, "e", RELIC_FORMAT_TSV, 0, "e", columns, 2,
	    TABLE("a\tb\nb\tc\n")) == -1 &&
	    strcmp(capture.message,
	    "limit exceeded: tuples=2 while loading e/2") == 0,
	    SCENARIO_ALL_OR_NOTHING,
	    "two more facts should be refused as a load's are");
	check(rows_of(ctx, "e(X, Y)?") == 1, SCENARIO_ALL_OR_NOTHING,
	    "a refused commit should leave the fact before it alone");

	relic_destroy(ctx);
}

/*
 * The rows a refused import is given, and the distinct values of each of
 * its columns.
 */
#define	BOUNDED_ROWS	40000
#define	BOUNDED_VALUES	200

/* The limit on tuples the first refusal is held to. */
#define	BOUNDED_TUPLES	10

/*
 * An import a limit refuses stops as soon as the facts it has read take the
 * base stores past the limit, rather than staging and interning the whole
 * table first.  Each table ends in a malformed record, which an import
 * reading on would reach and report in place of the limit, and where the
 * import stopped is read from the stream, which it reads a chunk at a time.
 *
 * Under the tuple limit, a table whose every row is new is refused at its
 * eleventh row, having interned the predicate's name and that row's symbols
 * but no later row's: e, a0 and b0 to b10.  Under the memory limit, a table
 * of distinct pairs over a few values, all interned beforehand so that the
 * import interns nothing, is refused at the row whose columns take the floor
 * past what the limit leaves, the one after half the table.
 */
static void
a_refused_import_stops_reading(void)
{
	char expected[TRANSCRIPT_MAX];
	char name[32];
	relic_config_t config;
	capture_t capture;
	relic_dom_t dom;
	relic_t *ctx;
	size_t symbols;
	size_t length;
	size_t cutoff;
	size_t i;
	long read;
	char *table;
	int status;

	table = pairs_table(BOUNDED_ROWS, BOUNDED_ROWS, &length);
	if (table == NULL) {
		check(false, SCENARIO_BOUNDED, "the table is built");
		return;
	}
	bzero(&config, sizeof(config));
	config.max_tuples = BOUNDED_TUPLES;
	ctx = context_with_capture(&config, &capture);
	if (ctx != NULL) {
		symbols = relic_symbol_count(ctx);
		status = import_counting(ctx, table, length, &read);
		checkf(status == -1 && strcmp(capture.message,
		    "limit exceeded: tuples=10 while loading e/2") == 0,
		    SCENARIO_BOUNDED, "the tuple limit should refuse the "
		    "import, not \"%s\"", capture.message);
		checkf(relic_symbol_count(ctx) - symbols ==
		    BOUNDED_TUPLES + 3 && interned(ctx, "e") &&
		    interned(ctx, "b10") && !interned(ctx, "b11"),
		    SCENARIO_BOUNDED, "the refused import should intern e, a0 "
		    "and b0 to b10, %d symbols, not %zu", BOUNDED_TUPLES + 3,
		    relic_symbol_count(ctx) - symbols);
		checkf(read >= 0 && (size_t)read <= RELIC_IMPORT_READ_CHUNK,
		    SCENARIO_BOUNDED, "the refused import should read one "
		    "chunk of the table, not %ld bytes", read);
		relic_destroy(ctx);
	}
	free(table);

	table = pairs_table(BOUNDED_ROWS, BOUNDED_VALUES, &length);
	if (table == NULL) {
		check(false, SCENARIO_BOUNDED, "the table is built");
		return;
	}
	ctx = context_with_capture(NULL, &capture);
	if (ctx != NULL) {
		for (i = 0; i < BOUNDED_VALUES; i++) {
			snprintf(name, sizeof(name), "a%zu", i);
			check(relic_value_intern_bytes(ctx, RELIC_SYMBOL, name,
			    strlen(name), &dom) == 0, SCENARIO_BOUNDED,
			    "a value is interned");
			snprintf(name, sizeof(name), "b%zu", i);
			check(relic_value_intern_bytes(ctx, RELIC_SYMBOL, name,
			    strlen(name), &dom) == 0, SCENARIO_BOUNDED,
			    "a value is interned");
		}
		ctx->account.limit = ctx->account.store_bytes +
		    ctx->account.value_bytes + ctx->account.pool_bytes +
		    BOUNDED_ROWS / 2 * 2 * sizeof(relic_dom_t);
		snprintf(expected, sizeof(expected),
		    "limit exceeded: memory=%zu while loading e/2",
		    ctx->account.limit);
		status = import_counting(ctx, table, length, &read);
		checkf(status == -1 && strcmp(capture.message, expected) == 0,
		    SCENARIO_BOUNDED, "the memory limit should refuse the "
		    "import with \"%s\", not \"%s\"", expected,
		    capture.message);
		cutoff = row_offset(table, BOUNDED_ROWS / 2 + 1);
		checkf(read >= 0 && (size_t)read >= cutoff &&
		    (size_t)read <= cutoff + RELIC_IMPORT_READ_CHUNK,
		    SCENARIO_BOUNDED, "the refused import should stop within "
		    "a chunk of row %d, byte %zu, not at byte %ld",
		    BOUNDED_ROWS / 2 + 1, cutoff, read);
		relic_destroy(ctx);
	}
	free(table);
}

/*
 * Import a table held in memory as e/2, of two symbol columns, and give in
 * *readp how many of its bytes the import read.  Returns the import's
 * status, or -2 when the table could not be put in a stream.
 */
static int
import_counting(relic_t *ctx, const char *table, size_t length, long *readp)
{
	static const enum relic_column columns[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_SYMBOL
	};
	relic_import_t import;
	FILE *stream;
	int status;

	*readp = -1;
	stream = table_stream(table, length);
	if (stream == NULL)
		return (-2);

	bzero(&import, sizeof(import));
	import.predicate = "e";
	import.columns = columns;
	import.column_count = 2;
	import.format = RELIC_FORMAT_TSV;

	status = relic_import_file(ctx, "e", stream, &import);
	*readp = ftell(stream);
	fclose(stream);

	return (status);
}

/* Whether `name` has been interned as a symbol of the context. */
static bool
interned(const relic_t *ctx, const char *name)
{
	return (relic_symbol_find(ctx, name, strlen(name),
	    relic_symbol_hash(ctx, name, strlen(name))) != RELIC_ID_NONE);
}

/* The offset of the byte after the first `rows` rows of a table. */
static size_t
row_offset(const char *table, size_t rows)
{
	const char *end;
	size_t i;

	end = table;
	for (i = 0; i < rows; i++)
		end = strchr(end, '\n') + 1;

	return ((size_t)(end - table));
}

/*
 * A table of `rows` distinct pairs a<i>, b<j> over `values` values of each
 * column, followed by a malformed record of one field, in a new string whose
 * length is in *lengthp, or NULL when memory ran out.
 */
static char *
pairs_table(size_t rows, size_t values, size_t *lengthp)
{
	size_t capacity;
	size_t length;
	size_t i;
	char *table;

	capacity = rows * 48 + 8;
	table = malloc(capacity);
	if (table == NULL)
		return (NULL);

	length = 0;
	for (i = 0; i < rows; i++)
		length += (size_t)snprintf(table + length, capacity - length,
		    "a%zu\tb%zu\n", i / values % values, i % values);
	length += (size_t)snprintf(table + length, capacity - length, "x\n");

	*lengthp = length;
	return (table);
}

/*
 * A fact is held once, within an import and against the facts the program
 * already states, and an import of nothing new leaves the relations as
 * current as they were.
 */
static void
facts_are_held_once(void)
{
	static const enum relic_column columns[] = { RELIC_COLUMN_SYMBOL };
	capture_t capture;
	relic_t *ctx;

	ctx = context_with_capture(NULL, &capture);
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "f.dl", "e(a).\np(X) :- e(X).\n") == 0 &&
	    import_table(ctx, "e", RELIC_FORMAT_TSV, 0, "e", columns, 1,
	    TABLE("b\na\nb\n")) == 0 && rows_of(ctx, "e(X)?") == 2,
	    SCENARIO_DUPLICATES,
	    "a, b and b again over e(a) should hold a and b");

	check(relic_materialize(ctx) == 0 && relic_relations_current(ctx),
	    SCENARIO_DUPLICATES, "the program should materialize");
	check(import_table(ctx, "e", RELIC_FORMAT_TSV, 0, "e", columns, 1,
	    TABLE("a\nb\n")) == 0 && relic_relations_current(ctx),
	    SCENARIO_DUPLICATES,
	    "an import of facts already held should leave the relations current");

	relic_destroy(ctx);
}

/*
 * An import after materialization changes the program, so what was derived
 * is discarded, and the next query sees the new facts: answered through
 * its Magic rewrite first, then from the program materialized again.
 */
static void
an_import_discards_derived_facts(void)
{
	static const enum relic_column columns[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_SYMBOL
	};
	capture_t capture;
	relic_t *ctx;

	ctx = context_with_capture(NULL, &capture);
	if (ctx == NULL)
		return;

	check(relic_load_string(ctx, "p.dl", "e(a, b).\n"
	    "path(X, Y) :- e(X, Y).\n"
	    "path(X, Z) :- path(X, Y), e(Y, Z).\n") == 0 &&
	    relic_materialize(ctx) == 0 && rows_of(ctx, "path(a, X)?") == 1,
	    SCENARIO_DERIVED, "path(a, X) should first reach b");

	check(import_table(ctx, "e", RELIC_FORMAT_CSV, 0, "e", columns, 2,
	    TABLE("b,c\nc,d\n")) == 0 && !relic_relations_current(ctx),
	    SCENARIO_DERIVED, "the import should discard what was derived");
	check(rows_of(ctx, "path(a, X)?") == 3, SCENARIO_DERIVED,
	    "the rewritten query should reach b, c and d");
	check(relic_materialize(ctx) == 0 && rows_of(ctx, "path(a, X)?") == 3,
	    SCENARIO_DERIVED, "the materialized program should reach them too");

	relic_destroy(ctx);
}

/*
 * A field longer than a read, and a table of many reads, are read whole:
 * the reader holds the field across the reads that deliver it.
 */
static void
a_long_field_crosses_reads(void)
{
	static const enum relic_column columns[] = {
		RELIC_COLUMN_INTEGER, RELIC_COLUMN_STRING
	};
	enum { LONG_FIELD = 150000 };
	relic_value_t values[2];
	capture_t capture;
	relic_t *ctx;
	char *table;
	char *field;

	table = malloc(LONG_FIELD + 16);
	field = malloc(LONG_FIELD);
	if (table == NULL || field == NULL) {
		free(table);
		free(field);
		check(false, SCENARIO_STRING, "the long table should fit");
		return;
	}
	memset(field, 'x', LONG_FIELD);
	field[LONG_FIELD / 2] = ',';
	table[0] = '1';
	table[1] = ',';
	table[2] = '"';
	bcopy(field, table + 3, LONG_FIELD);
	table[LONG_FIELD + 3] = '"';
	table[LONG_FIELD + 4] = '\n';

	ctx = context_with_capture(NULL, &capture);
	if (ctx != NULL) {
		values[0].type = RELIC_INTEGER;
		values[0].v.integer = 1;
		check(import_table(ctx, "l", RELIC_FORMAT_CSV, 0, "l", columns,
		    2, table, LONG_FIELD + 5) == 0 &&
		    relic_value_string(ctx, field, LONG_FIELD,
		    &values[1]) == 0 && relic_fact_add(ctx, "l", 2,
		    values) == 0 && rows_of(ctx, "l(X, Y)?") == 1,
		    SCENARIO_STRING,
		    "a field spanning reads should import as the whole string");
		relic_destroy(ctx);
	}

	free(table);
	free(field);
}

/*
 * A stream's read function of the caller's own.  On its first read it tries
 * to materialize the context, and when `columns` is set it changes every one
 * of the `column_count` columns it points at to a string column.  It then
 * serves `table`, or fails every read with `fail_errno` when that is set.
 */
struct reentering_read {
	relic_t			*ctx;
	const char		*table;
	enum relic_column	*columns;
	size_t			 column_count;
	char			*predicate;
	size_t			 offset;
	int			 fail_errno;
	int			 refusals;
	int			 calls;
};

static ssize_t
reenter_and_read(reentering_read_t *reading, char *buffer, size_t size)
{
	size_t remaining;
	size_t i;

	reading->calls++;
	if (reading->calls == 1) {
		if (relic_materialize(reading->ctx) == -1)
			reading->refusals++;
		for (i = 0; reading->columns != NULL &&
		    i < reading->column_count; i++)
			reading->columns[i] = RELIC_COLUMN_STRING;
		if (reading->predicate != NULL)
			reading->predicate[0] = 'v';
	}

	if (reading->fail_errno != 0) {
		errno = reading->fail_errno;
		return (-1);
	}

	remaining = strlen(reading->table) - reading->offset;
	if (size > remaining)
		size = remaining;
	bcopy(reading->table + reading->offset, buffer, size);
	reading->offset += size;

	return ((ssize_t)size);
}

#if defined(HAVE_FUNOPEN)
static int
funopen_read(void *cookie, char *buffer, int size)
{
	return ((int)reenter_and_read(cookie, buffer, (size_t)size));
}

static FILE *
reentering_stream(reentering_read_t *reading)
{
	return (funopen(reading, funopen_read, NULL, NULL, NULL));
}
#elif defined(HAVE_FOPENCOOKIE)
static ssize_t
cookie_read(void *cookie, char *buffer, size_t size)
{
	return (reenter_and_read(cookie, buffer, size));
}

static FILE *
reentering_stream(reentering_read_t *reading)
{
	cookie_io_functions_t functions = { cookie_read, NULL, NULL, NULL };

	return (fopencookie(reading, "r", functions));
}
#else
static FILE *
reentering_stream(reentering_read_t *reading RELIC_UNUSED)
{
	return (NULL);
}
#endif

/*
 * The read function behind an imported stream is caller code: its call into
 * the context is refused, and the import it runs inside completes.
 */
static void
a_stream_cannot_call_in(void)
{
	static const enum relic_column columns[] = { RELIC_COLUMN_SYMBOL };
	reentering_read_t reading;
	relic_import_t import;
	capture_t capture;
	FILE *stream;

	bzero(&reading, sizeof(reading));
	reading.table = "a\nb\n";
	reading.ctx = context_with_capture(NULL, &capture);
	if (reading.ctx == NULL)
		return;

	stream = reentering_stream(&reading);
	if (stream == NULL) {
		printf("skip: %s: no stream with a read function of the "
		    "caller's own\n", SCENARIO_REENTRY);
		relic_destroy(reading.ctx);
		return;
	}

	bzero(&import, sizeof(import));
	import.predicate = "r";
	import.columns = columns;
	import.column_count = 1;
	import.format = RELIC_FORMAT_TSV;

	check(relic_import_file(reading.ctx, "r", stream, &import) == 0 &&
	    reading.refusals == 1, SCENARIO_REENTRY,
	    "the read function's call should be refused");
	check(rows_of(reading.ctx, "r(X)?") == 2, SCENARIO_REENTRY,
	    "the import it ran inside should complete");

	fclose(stream);
	relic_destroy(reading.ctx);
}

/*
 * The description is borrowed, and caller code runs while the table is read,
 * so the import reads the description once, when it checks it.  A read
 * function that turns the skipped columns of a 32-column import into kept
 * ones, and renames its predicate, changes nothing: the fact is the checked
 * predicate's, with the 32 values checked, and the eight skipped fields are
 * not among them.
 */
static void
a_description_is_read_once(void)
{
	enum relic_column columns[RELIC_MAX_ARITY + 8];
	char predicate[] = "w";
	char table[(RELIC_MAX_ARITY + 8) * 4 + 1];
	char query[RELIC_MAX_ARITY * 8 + 8];
	reentering_read_t reading;
	relic_import_t import;
	capture_t capture;
	FILE *stream;
	size_t i;

	bzero(&reading, sizeof(reading));
	reading.ctx = context_with_capture(NULL, &capture);
	if (reading.ctx == NULL)
		return;

	/* Fields k0..k31 are kept, and s0..s7 after them skipped. */
	table[0] = '\0';
	query[0] = '\0';
	append_text(query, sizeof(query), "w(");
	for (i = 0; i < RELIC_MAX_ARITY + 8; i++) {
		char piece[16];
		bool kept;

		kept = i < RELIC_MAX_ARITY;
		columns[i] = kept ? RELIC_COLUMN_STRING : RELIC_COLUMN_SKIP;

		snprintf(piece, sizeof(piece), "%s%c%zu", i > 0 ? "\t" : "",
		    kept ? 'k' : 's', kept ? i : i - RELIC_MAX_ARITY);
		append_text(table, sizeof(table), piece);
		if (kept) {
			snprintf(piece, sizeof(piece), "%s\"k%zu\"",
			    i > 0 ? ", " : "", i);
			append_text(query, sizeof(query), piece);
		}
	}
	append_text(table, sizeof(table), "\n");
	append_text(query, sizeof(query), ")?");

	reading.table = table;
	reading.columns = columns;
	reading.column_count = RELIC_MAX_ARITY + 8;
	reading.predicate = predicate;
	stream = reentering_stream(&reading);
	if (stream == NULL) {
		printf("skip: %s: no stream with a read function of the "
		    "caller's own\n", SCENARIO_DESCRIPTION);
		relic_destroy(reading.ctx);
		return;
	}

	bzero(&import, sizeof(import));
	import.predicate = predicate;
	import.columns = columns;
	import.column_count = RELIC_MAX_ARITY + 8;
	import.format = RELIC_FORMAT_TSV;

	check(relic_import_file(reading.ctx, "w", stream, &import) == 0 &&
	    reading.calls > 0 &&
	    columns[RELIC_MAX_ARITY] == RELIC_COLUMN_STRING &&
	    predicate[0] == 'v',
	    SCENARIO_DESCRIPTION, "an import whose columns change while it "
	    "reads should succeed as checked");
	check(rows_of(reading.ctx, query) == 1, SCENARIO_DESCRIPTION,
	    "the fact should hold the columns kept when the import began");

	fclose(stream);
	relic_destroy(reading.ctx);
}

/*
 * A stream that fails to read fails the import, naming the source and the
 * system's reason, and adds nothing.
 */
static void
a_failed_read_adds_nothing(void)
{
	static const enum relic_column columns[] = { RELIC_COLUMN_SYMBOL };
	reentering_read_t reading;
	relic_import_t import;
	capture_t capture;
	FILE *stream;

	bzero(&reading, sizeof(reading));
	reading.table = "";
	reading.fail_errno = EIO;
	reading.ctx = context_with_capture(NULL, &capture);
	if (reading.ctx == NULL)
		return;

	stream = reentering_stream(&reading);
	if (stream == NULL) {
		printf("skip: %s: no stream with a read function of the "
		    "caller's own\n", SCENARIO_ALL_OR_NOTHING);
		relic_destroy(reading.ctx);
		return;
	}

	bzero(&import, sizeof(import));
	import.predicate = "r";
	import.columns = columns;
	import.column_count = 1;
	import.format = RELIC_FORMAT_TSV;

	check(relic_import_file(reading.ctx, "broken", stream, &import) ==
	    -1 && capture.errors == 1, SCENARIO_ALL_OR_NOTHING,
	    "a failed read should fail the import with one error");
	checkf(strncmp(capture.message, "cannot read broken: ",
	    strlen("cannot read broken: ")) == 0 &&
	    strlen(capture.message) > strlen("cannot read broken: "),
	    SCENARIO_ALL_OR_NOTHING, "a failed read should name the source and "
	    "a reason, not \"%s\"", capture.message);

	fclose(stream);
	relic_destroy(reading.ctx);
}

/*
 * relic_import_path() opens the file it names, reads it by the description
 * and closes it, and a path that cannot be opened is reported by its name.
 */
static void
a_path_is_opened_and_named(void)
{
	static const enum relic_column columns[] = { RELIC_COLUMN_SYMBOL };
	static const char missing[] = "/nonexistent/relic/table.tsv";
	static const char unopened[] =
	    "cannot open /nonexistent/relic/table.tsv: ";
	relic_import_t import;
	capture_t capture;
	relic_t *ctx;

	ctx = context_with_capture(NULL, &capture);
	if (ctx == NULL)
		return;

	bzero(&import, sizeof(import));
	import.predicate = "r";
	import.columns = columns;
	import.column_count = 1;
	import.format = RELIC_FORMAT_TSV;

	check(relic_import_path(ctx, "/dev/null", &import) == 0 &&
	    capture.errors == 0 && rows_of(ctx, "r(X)?") == 0,
	    SCENARIO_LOCATION, "an empty file should import no facts");

	import.flags = RELIC_IMPORT_HEADER;
	check(relic_import_path(ctx, "/dev/null", &import) == -1 &&
	    strcmp(capture.file, "/dev/null") == 0,
	    SCENARIO_LOCATION, "a mistake in a file should name its path");

	import.flags = 0;
	check(relic_import_path(ctx, missing, &import) == -1 &&
	    strncmp(capture.message, unopened, sizeof(unopened) - 1) == 0,
	    SCENARIO_LOCATION, "a file that cannot be opened should be named");

	relic_destroy(ctx);
}

int
main(void)
{
	tsv_is_read_by_its_rules();
	csv_is_read_by_its_rules();
	locations_stop_at_their_largest_value();
	symbol_columns_hold_the_language_symbols();
	integer_columns_hold_the_language_integers();
	string_columns_hold_any_bytes();
	skipped_columns_intern_nothing();
	messages_quote_only_what_is_safe();
	mistakes_name_their_source();
	a_header_is_skipped();
	every_refusal_reads_nothing();
	a_failed_import_adds_nothing();
	a_refused_import_stops_reading();
	facts_are_held_once();
	an_import_discards_derived_facts();
	a_long_field_crosses_reads();
	a_stream_cannot_call_in();
	a_description_is_read_once();
	a_failed_read_adds_nothing();
	a_path_is_opened_and_named();

	return (check_summary("import"));
}
