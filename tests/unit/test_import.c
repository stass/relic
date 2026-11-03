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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
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
#include "window.h"

#define	SCENARIO_ALL_OR_NOTHING	"an import is all or nothing"
#define	SCENARIO_BATCH		"a batch import ends as a scalar one does"
#define	SCENARIO_BOUNDED	"a refused import stops reading"
#define	SCENARIO_DERIVED	"an import discards what was derived"
#define	SCENARIO_DESCRIPTION	"a malformed description is refused unread"
#define	SCENARIO_DUPLICATES	"a fact is held once"
#define	SCENARIO_FRAMING \
	"a window is framed alike however its framing is stepped"
#define	SCENARIO_HEADER		"a header is read and not imported"
#define	SCENARIO_INPUT		"a field is held to the input limit"
#define	SCENARIO_INTEGER	"an integer column holds the language's integers"
#define	SCENARIO_LOCATION	"a mistake is located in its source"
#define	SCENARIO_MESSAGE	"a message names the field it refuses"
#define	SCENARIO_POLICY		"a table is batched only where batching repays"
#define	SCENARIO_READER		"the reader reads each format by its rules"
#define	SCENARIO_RECORD_CAP	"a window holds at most its records"
#define	SCENARIO_RESUME		"windows go on after a declined window"
#define	SCENARIO_REENTRY	"a stream's read function cannot call in"
#define	SCENARIO_SKIP		"a skipped column interns nothing"
#define	SCENARIO_STRING		"a string column holds any bytes"
#define	SCENARIO_SYMBOL		"a symbol column holds the language's symbols"
#define	SCENARIO_WAITING \
	"a producer that waits hears of its mistake"

/*
 * How long an import from a producer that waits, and the producer itself,
 * may take before it is taken to have hung.
 */
#define	PRODUCER_SECONDS	20

/* A byte string with a length, for tables that hold NUL. */
#define	TABLE(text)	(text), (sizeof(text) - 1)

/* The same, and an array of columns with its length, as a batch case's. */
#define	BATCH_TABLE(text)	.table = (text), .length = sizeof(text) - 1
#define	BATCH_COLUMNS(array)	.columns = (array), \
	.column_count = sizeof(array) / sizeof((array)[0])

/*
 * The context flag that stages a table a window at a time however small
 * it is and however few threads read it.
 */
#define	BATCHED		RELIC_UNSTABLE_PARALLEL_ALWAYS

/* The longest transcript and message a test compares. */
#define	TRANSCRIPT_MAX	1024
#define	MESSAGE_MAX	512

typedef struct batch_case batch_case_t;
typedef struct capture capture_t;
typedef struct framing framing_t;
typedef struct framing_case framing_case_t;
typedef struct import_outcome import_outcome_t;
typedef struct reader_case reader_case_t;
typedef struct reentering_read reentering_read_t;
typedef struct window_tally window_tally_t;
typedef struct batch_way batch_way_t;
typedef struct producer producer_t;
typedef struct producer_channel producer_channel_t;

/*
 * The horizon and the piece size the framing scenarios frame windows
 * under, and the most pieces a window of theirs is cut into.
 */
#define	FRAMING_HORIZON		32
#define	FRAMING_PIECE		8
#define	FRAMING_PIECES_MAX	8

/*
 * A table the framing scenarios frame, as a window from its first byte,
 * in csv or, when `tsv` is set, tsv, holding at most `most_records`
 * records when that is set, and the window it must be framed into:
 * `tuples` tuples in `pieces` pieces, ending `length` bytes in, the
 * table's last when `ends_input` is set, or no window at all when
 * `unframed` is.  A window of FRAMING_HORIZON bytes holds fewer records
 * than that, so a case that sets no cap is framed by its horizon alone.
 */
struct framing_case {
	const char	*label;
	const char	*table;
	size_t		 table_length;
	size_t		 most_records;
	size_t		 tuples;
	size_t		 pieces;
	size_t		 length;
	bool		 tsv;
	bool		 header;
	bool		 ends_input;
	bool		 unframed;
};

#define	FRAMING_TABLE(text)	.table = text, .table_length = sizeof(text) - 1

/* A window as a framer framed it. */
struct framing {
	relic_window_piece_t	 pieces[FRAMING_PIECES_MAX];
	relic_location_t	 end;
	size_t			 length;
	size_t			 piece_count;
	size_t			 tuple_count;
	enum relic_framing	 state;
	bool			 ends_input;
};

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

/*
 * A table imported a window at a time and a field at a time, as e/N of the
 * description's columns, by import `flags`.  `program` is loaded first,
 * `max_tuples` limits the context, `max_memory` the memory the import
 * may take it to and `max_input` its fields, each when it is set.  `pad`,
 * when set, is what follows a first field of symbol p and filler in a
 * record of the same columns, which is put before the table to move where
 * the reads and the windows cut it.  A table that is `whole` is well formed
 * and fits every limit, so a batch import stages every window of it and
 * declines none.
 */
struct batch_case {
	const char		*label;
	const char		*program;
	const char		*table;
	size_t			 length;
	const char		*pad;
	enum relic_format	 format;
	unsigned int		 flags;
	const enum relic_column	*columns;
	size_t			 column_count;
	size_t			 max_tuples;
	size_t			 max_memory;
	size_t			 max_input;
	bool			 whole;
};

/*
 * A way a table is read besides a field at a time: the context's flags and
 * threads, and what to call it.
 */
struct batch_way {
	unsigned int	 flags;
	size_t		 threads;
	const char	*name;
};

/*
 * The ways a table is compared with its reading a field at a time: a
 * window at a time on one thread and on several, and as the context
 * chooses on enough threads to batch a large table, its first window read
 * to tell.
 */
static const batch_way_t batch_ways[] = {
	{ BATCHED, 1, "one thread, batched" },
	{ BATCHED, 4, "four threads, batched" },
	{ 0, RELIC_IMPORT_BATCH_WORKERS, "batching threads, by choice" },
};

#ifdef RELIC_IMPORT_REGULAR_FILE
/*
 * The failing stream lib/import.c takes for a regular file while one is
 * imported, or NULL.
 */
static FILE	*regular_stand_in;

bool		 relic_test_regular_file(FILE *stream);
#endif

/*
 * A producer that waits, once start_producer() has forked it: its process,
 * the read end of its pipe until the import takes it (-1 for a FIFO, or
 * once taken), and the write end of the channel it waits on.
 */
struct producer {
	pid_t	pid;
	int	data;
	int	control;
};

/*
 * The channels a producer that waits sends its table through: a pipe, or
 * a FIFO the import opens by its path.
 */
struct producer_channel {
	const char	*label;
	bool		 by_path;
};

static const producer_channel_t producer_channels[] = {
	{ "a pipe", false },
	{ "a FIFO", true },
};

/* The columns of the table a producer that waits sends. */
static const enum relic_column waiting_columns[] = {
	RELIC_COLUMN_SYMBOL, RELIC_COLUMN_INTEGER
};

/*
 * The windows batch imports staged and declined, and the steps of framing
 * they ran as the filler of a window's jobs.
 */
struct window_tally {
	uint64_t	staged;
	uint64_t	declined;
	uint64_t	framed_ahead;
};

/*
 * What a table imported one way left: the context, its diagnostics, the
 * status of the first import and that of a second import of the same
 * table, which meets the facts the first added.
 */
struct import_outcome {
	relic_t		*ctx;
	capture_t	 capture;
	int		 status;
	int		 again;
};

static void	 a_waiting_producer_hears_its_mistake(void);
static bool	 make_fifo_directory(char *directory, size_t size);
static char	*malformed_chunk(void);
static int	 import_chunk_from_file(const char *chunk, const char *name,
		    import_outcome_t *outcome);
static int	 import_from_producer(const char *chunk, const char *fifo,
		    const batch_way_t *way, import_outcome_t *outcome,
		    bool *sentp);
static int	 start_producer(producer_t *producer, const char *chunk,
		    const char *fifo);
static void	 close_channel(const int data[2], const char *fifo);
_Noreturn static void produce_and_wait(const char *chunk, int descriptor,
		    int control);
static int	 import_from_channel(relic_t *ctx, producer_t *producer,
		    const char *fifo, int *statusp);
static bool	 stop_producer(producer_t *producer, const char *fifo);
static void	 append_text(char *out, size_t size, const char *text);
static void	 check_same_outcome(const import_outcome_t *batched,
		    const import_outcome_t *scalar, const char *label,
		    const batch_way_t *way, const char *imports);
static void	 compare_batched_import(const batch_case_t *batch,
		    const char *table, size_t length, const char *label,
		    const batch_way_t *way, window_tally_t *tally);
static void	 compare_import_ways(const batch_case_t *batch,
		    const char *table, size_t length, const char *label,
		    window_tally_t *tally);
static void	 compare_shifted_imports(const batch_case_t *batch,
		    window_tally_t *tally);
static size_t	 accounted_bytes(const relic_t *ctx);
static void	 compare_generated_imports(const char *label,
		    size_t odd_row, const char *odd, size_t max_tuples,
		    size_t max_memory, window_tally_t *tally);
static void	 compare_memory_limits(window_tally_t *tally);
static const char *context_difference(const relic_t *a, const relic_t *b);
static const char *facts_difference(const relic_t *a, const relic_t *b);
static char	*generated_table(size_t rows, size_t odd_row,
		    const char *odd, size_t *lengthp);
static void	 import_again(const batch_case_t *batch, const char *table,
		    size_t length, import_outcome_t *outcome);
static int	 import_first(const batch_case_t *batch, const char *table,
		    size_t length, unsigned int flags, size_t threads,
		    import_outcome_t *outcome);
static char	*faults_a_window_apart(void);
static char	*record_longer_than_a_window(void);
static char	*shifted_table(const batch_case_t *batch, size_t shift,
		    size_t *lengthp);
static const char *values_difference(const relic_t *a, const relic_t *b);
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
static void	 transcribe(const reader_case_t *table, size_t field_max,
		    size_t piece, bool frames, bool fields, char *out,
		    size_t size);

static void	 csv_is_read_by_its_rules(void);
static void	 every_refusal_reads_nothing(void);
static void	 facts_are_held_once(void);
static void	 a_failed_import_adds_nothing(void);
static void	 a_refused_import_stops_reading(void);
static void	 a_long_field_stops_reading(void);
static void	 batching_follows_the_threads_and_the_table(void);
static void	 check_batching(const char *table, size_t length,
		    unsigned int flags, size_t threads, bool batched,
		    const char *label);
static void	 refused_import_stops_reading(unsigned, size_t, const char *);
static void	 a_header_is_skipped(void);
static void	 a_long_field_crosses_reads(void);
static void	 a_description_is_read_once(void);
static void	 a_source_name_is_read_once(void);
static void	 a_failed_read_adds_nothing(void);
static void	 a_path_is_opened_and_named(void);
static void	 a_stream_cannot_call_in(void);
static void	 an_import_discards_derived_facts(void);
static void	 a_batch_import_matches_the_scalar_import(void);
static void	 a_window_holds_at_most_its_records(void);
static void	 check_capped_import(const char *label, const char *table,
		    size_t length);
static void	 windows_go_on_after_a_declined_window(void);
static void	 a_window_is_framed_alike_however_stepped(void);
static void	 frame_case(const framing_case_t *test, size_t buffered,
		    bool ended, size_t most, framing_t *framingp);
static void	 frame_every_step(const framing_case_t *test,
		    size_t buffered, bool ended, const framing_t *reference);
static bool	 same_framing(const framing_t *a, const framing_t *b);
static void	 a_failed_read_ahead_waits_its_turn(void);
static void	 compare_failed_reads(const char *label, const char *table,
		    size_t fail_at);
static int	 import_failing(const char *table, size_t fail_at,
		    unsigned int flags, size_t threads,
		    import_outcome_t *outcome);
static void	 integer_columns_hold_the_language_integers(void);
static void	 compare_long_field_late(window_tally_t *tally);
static void	 compare_long_field_after_resuming(window_tally_t *tally);
static void	 fields_are_held_to_their_bound(void);
static void	 locations_stop_at_their_largest_value(void);
static void	 messages_quote_only_what_is_safe(void);
static void	 mistakes_name_their_source(void);
static void	 skipped_columns_intern_nothing(void);
static void	 string_columns_hold_any_bytes(void);
static void	 the_input_limit_bounds_a_field(void);
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
	case RELIC_TABLE_TOO_LONG:
		snprintf(out, size, "too-long %" PRIu32 ":%" PRIu32,
		    event->where.line, event->where.column);
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
 * Read a table, its fields held to `field_max` bytes, feeding it `piece`
 * bytes at a time, and write the events as
 * a transcript separated by " | ", up to and including the one that stops
 * the reader, leaving out its FIELD events unless `fields` is set.  A reader
 * that `frames` reads only the table's structure.  A reader that asks for
 * more once it has been given the last of the table is given nothing, and
 * the transcript ends in "more".
 */
static void
transcribe(const reader_case_t *table, size_t field_max, size_t piece,
    bool frames, bool fields, char *out, size_t size)
{
	relic_table_reader_t reader;
	relic_table_event_t event;
	char described[256];
	size_t fed;
	bool fed_last;

	out[0] = '\0';
	fed = 0;
	fed_last = false;
	relic_table_reader_init(&reader, table->format, table->width,
	    field_max, "t");
	if (frames)
		relic_table_reader_frame_only(&reader);

	for (;;) {
		enum relic_table_event_kind kind;

		kind = relic_table_next(&reader, &event);
		if (kind == RELIC_TABLE_MORE && !fed_last) {
			size_t length;

			length = table->length - fed;
			if (length > piece)
				length = piece;
			fed_last = fed + length == table->length;
			relic_table_feed(&reader, table->table + fed, length,
			    fed_last);
			fed += length;
			continue;
		}

		if (kind != RELIC_TABLE_FIELD || fields) {
			describe_event(&event, described, sizeof(described));
			if (out[0] != '\0')
				append_text(out, size, " | ");
			append_text(out, size, described);
		}

		if (kind != RELIC_TABLE_FIELD && kind != RELIC_TABLE_RECORD)
			break;
	}

	relic_table_reader_reset(&reader);
}

/*
 * Check each table's transcript, read whole and read a byte at a time, and
 * that a reader framing it, fed either way, finds the same records and the
 * same mistake or end in the same places.
 */
static void
check_tables(const reader_case_t *cases, size_t count)
{
	char whole[TRANSCRIPT_MAX];
	char bytewise[TRANSCRIPT_MAX];
	char structure[TRANSCRIPT_MAX];
	char framed[TRANSCRIPT_MAX];
	size_t i;

	for (i = 0; i < count; i++) {
		transcribe(&cases[i], SIZE_MAX, cases[i].length + 1, false,
		    true, whole, sizeof(whole));
		transcribe(&cases[i], SIZE_MAX, 1, false, true, bytewise,
		    sizeof(bytewise));
		transcribe(&cases[i], SIZE_MAX, cases[i].length + 1, false,
		    false, structure, sizeof(structure));

		checkf(strcmp(whole, cases[i].transcript) == 0, SCENARIO_READER,
		    "table %zu should read as \"%s\", not \"%s\"", i,
		    cases[i].transcript, whole);
		checkf(strcmp(bytewise, whole) == 0, SCENARIO_READER,
		    "table %zu fed a byte at a time should read as \"%s\", "
		    "not \"%s\"", i, whole, bytewise);

		transcribe(&cases[i], SIZE_MAX, cases[i].length + 1, true,
		    true, framed, sizeof(framed));
		checkf(strcmp(framed, structure) == 0, SCENARIO_READER,
		    "table %zu framed should read as \"%s\", not \"%s\"", i,
		    structure, framed);
		transcribe(&cases[i], SIZE_MAX, 1, true, true, framed,
		    sizeof(framed));
		checkf(strcmp(framed, structure) == 0, SCENARIO_READER,
		    "table %zu framed a byte at a time should read as \"%s\", "
		    "not \"%s\"", i, structure, framed);
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

	relic_table_reader_init(&reader, RELIC_FORMAT_TSV, 1, SIZE_MAX, "t");
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
	relic_table_reader_init(&reader, RELIC_FORMAT_TSV, 1, SIZE_MAX, "t");
	reader.where.column = UINT32_MAX - 1;
	relic_table_feed(&reader, "ab\\q", 5, true);

	kind = relic_table_next(&reader, &event);
	check(kind == RELIC_TABLE_MISTAKE && event.where.line == 1 &&
	    event.where.column == UINT32_MAX, SCENARIO_READER,
	    "a mistake past the last column should be at that column");

	relic_table_reader_reset(&reader);
}

/* A field bound a few bytes of a table pass, so each case stays short. */
#define	READER_FIELD_MAX	3

/*
 * A field holds at most the reader's bound of decoded bytes, a quoted field
 * left open among them: the reader stops at the byte that would pass it,
 * located at the field's first byte, read whole or a byte at a time.  A
 * reader that only frames holds no field, so the bound changes nothing it
 * finds.
 */
static void
fields_are_held_to_their_bound(void)
{
	static const reader_case_t cases[] = {
		{ TABLE("abc\n"), RELIC_FORMAT_TSV, 1,
		    "field 0 1:1 abc | record | end 2:1" },
		{ TABLE("abcd\n"), RELIC_FORMAT_TSV, 1, "too-long 1:1" },
		{ TABLE("a\tbcde"), RELIC_FORMAT_TSV, 2,
		    "field 0 1:1 a | too-long 1:3" },
		{ TABLE("ab\nabcd\n"), RELIC_FORMAT_TSV, 1,
		    "field 0 1:1 ab | record | too-long 2:1" },
		{ TABLE("\\t\\t\\t\n"), RELIC_FORMAT_TSV, 1,
		    "field 0 1:1 \\x09\\x09\\x09 | record | end 2:1" },
		{ TABLE("ab\\t\\t"), RELIC_FORMAT_TSV, 1, "too-long 1:1" },
		{ TABLE("abc,\n"), RELIC_FORMAT_CSV, 2,
		    "field 0 1:1 abc | field 1 1:5  | record | end 2:1" },
		{ TABLE("\"abc\""), RELIC_FORMAT_CSV, 1,
		    "field 0 1:1 abc | record | end 1:6" },
		{ TABLE("\"a\"\"b\""), RELIC_FORMAT_CSV, 1,
		    "field 0 1:1 a\"b | record | end 1:7" },
		{ TABLE("\"a\"\"bc\""), RELIC_FORMAT_CSV, 1, "too-long 1:1" },
		{ TABLE("x,\"abc"), RELIC_FORMAT_CSV, 2,
		    "field 0 1:1 x | unterminated 1:3" },
		{ TABLE("x,\"abcd"), RELIC_FORMAT_CSV, 2,
		    "field 0 1:1 x | too-long 1:3" },
		{ TABLE("x,\"ab\ncd\""), RELIC_FORMAT_CSV, 2,
		    "field 0 1:1 x | too-long 1:3" }
	};
	char whole[TRANSCRIPT_MAX];
	char bytewise[TRANSCRIPT_MAX];
	char framed[TRANSCRIPT_MAX];
	char unbounded[TRANSCRIPT_MAX];
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		transcribe(&cases[i], READER_FIELD_MAX, cases[i].length + 1,
		    false, true, whole, sizeof(whole));
		transcribe(&cases[i], READER_FIELD_MAX, 1, false, true,
		    bytewise, sizeof(bytewise));
		checkf(strcmp(whole, cases[i].transcript) == 0, SCENARIO_INPUT,
		    "bounded table %zu should read as \"%s\", not \"%s\"", i,
		    cases[i].transcript, whole);
		checkf(strcmp(bytewise, whole) == 0, SCENARIO_INPUT,
		    "bounded table %zu fed a byte at a time should read as "
		    "\"%s\", not \"%s\"", i, whole, bytewise);

		transcribe(&cases[i], READER_FIELD_MAX, 1, true, true, framed,
		    sizeof(framed));
		transcribe(&cases[i], SIZE_MAX, 1, true, true, unbounded,
		    sizeof(unbounded));
		checkf(strcmp(framed, unbounded) == 0, SCENARIO_INPUT,
		    "bounded table %zu framed should read as \"%s\", not "
		    "\"%s\"", i, unbounded, framed);
	}
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

	ctx = relic_create_config(config, sizeof(*config));
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

	status = relic_import_file(ctx, name, stream, &import, sizeof(import));
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
 * The input limit refuses an import with a field that decodes to more
 * bytes than the limit, adding nothing of the table, and locates the
 * refusal at the field; a field at the limit imports, however many bytes
 * its quoting or escapes are written in.  A header's field and a skipped
 * column's are held to it too.  Zero and SIZE_MAX set no limit.
 */
static void
the_input_limit_bounds_a_field(void)
{
	static const enum relic_column columns[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_STRING
	};
	static const enum relic_column skipping[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_SKIP
	};
	static const size_t unlimited[] = { 0, SIZE_MAX };
	static const char refused[] =
	    "limit exceeded: input=4 while importing t";
	char table[1024];
	relic_config_t config;
	capture_t capture;
	relic_t *ctx;
	size_t i;
	int status;

	bzero(&config, sizeof(config));
	config.max_input = 4;
	ctx = context_with_capture(&config, &capture);
	if (ctx == NULL) {
		check(false, SCENARIO_INPUT, "a context is made");
		return;
	}

	status = import_table(ctx, "t", RELIC_FORMAT_CSV, 0, "p", columns, 2,
	    TABLE("ab,\"abcd\"\nc,\"a\"\"bcd\"\n"));
	check(status == -1 && strcmp(capture.message, refused) == 0 &&
	    strcmp(capture.file, "t") == 0 && capture.line == 2 &&
	    capture.column == 3, SCENARIO_INPUT,
	    "a field decoding to five bytes should be refused at t:2:3");
	check(rows_of(ctx, "p(X, Y)?") == 0, SCENARIO_INPUT,
	    "the refused import should add nothing");

	check(import_table(ctx, "t", RELIC_FORMAT_CSV, 0, "p", columns, 2,
	    TABLE("ab,\"abcdefgh")) == -1 &&
	    strcmp(capture.message, refused) == 0 && capture.line == 1 &&
	    capture.column == 4, SCENARIO_INPUT,
	    "a quoted field left open should be refused at its quote");

	check(import_table(ctx, "t", RELIC_FORMAT_CSV, RELIC_IMPORT_HEADER,
	    "p", columns, 2, TABLE("names,x\nab,c\n")) == -1 &&
	    strcmp(capture.message, refused) == 0 && capture.line == 1 &&
	    capture.column == 1, SCENARIO_INPUT,
	    "a header's field should be held to the limit");

	check(import_table(ctx, "t", RELIC_FORMAT_CSV, 0, "q", skipping, 2,
	    TABLE("a,abcde\n")) == -1 &&
	    strcmp(capture.message, refused) == 0 && capture.line == 1 &&
	    capture.column == 3, SCENARIO_INPUT,
	    "a skipped column's field should be held to the limit");

	status = import_table(ctx, "t", RELIC_FORMAT_CSV, 0, "p", columns, 2,
	    TABLE("abcd,\"a\"\"bc\"\n"));
	check(status == 0, SCENARIO_INPUT, "fields at the limit should import");
	check(rows_of(ctx, "p(X, Y)?") == 1, SCENARIO_INPUT,
	    "fields at the limit should be a fact");
	status = import_table(ctx, "t", RELIC_FORMAT_TSV, 0, "p", columns, 2,
	    TABLE("a\t\\t\\t\\t\\t\n"));
	check(status == 0, SCENARIO_INPUT,
	    "four escapes should be four bytes of the limit, not eight");
	check(rows_of(ctx, "p(X, Y)?") == 2, SCENARIO_INPUT,
	    "four escapes at the limit should be a fact");

	relic_destroy(ctx);

	table[0] = 'a';
	table[1] = ',';
	memset(table + 2, 'x', sizeof(table) - 3);
	table[sizeof(table) - 1] = '\n';
	for (i = 0; i < sizeof(unlimited) / sizeof(unlimited[0]); i++) {
		config.max_input = unlimited[i];
		ctx = context_with_capture(&config, &capture);
		if (ctx == NULL) {
			check(false, SCENARIO_INPUT, "a context is made");
			return;
		}
		status = import_table(ctx, "t", RELIC_FORMAT_CSV, 0, "p",
		    columns, 2, table, sizeof(table));
		checkf(status == 0, SCENARIO_INPUT,
		    "an input limit of %zu should hold a field to none",
		    unlimited[i]);
		check(rows_of(ctx, "p(X, Y)?") == 1, SCENARIO_INPUT,
		    "the long field should be a fact");
		relic_destroy(ctx);
	}
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
		relic_error_class_t expected_class;
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
		checkf(relic_import_file(ctx, "e", stream, &import,
		    sizeof(import)) == -1 &&
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

	check(relic_import_file(ctx, "e", stream, NULL, 0) == -1 &&
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
	check(relic_import_file(ctx, "e", NULL, &import,
	    sizeof(import)) == -1 &&
	    strcmp(capture.message, "no input stream was given") == 0,
	    SCENARIO_DESCRIPTION, "no stream should be refused");
	check(relic_import_path(ctx, NULL, &import, sizeof(import)) == -1 &&
	    strcmp(capture.message, "no path was given") == 0,
	    SCENARIO_DESCRIPTION, "no path should be refused");
	check(relic_symbol_count(ctx) == symbols, SCENARIO_DESCRIPTION,
	    "no stream and no path should intern nothing");

	/* The description is checked before the path is opened. */
	import = good;
	import.format = 0;
	check(relic_import_path(ctx, "/nonexistent/e.tsv", &import,
	    sizeof(import)) == -1 &&
	    strcmp(capture.message,
	    "the import's format is neither csv nor tsv") == 0,
	    SCENARIO_DESCRIPTION, "a bad description should not open its path");

	/* Thirty-two kept columns, and a zeroed description, beside them. */
	import = good;
	import.columns = wide;
	import.column_count = RELIC_MAX_ARITY + 1;
	check(relic_import_file(ctx, "e", stream, &import,
	    sizeof(import)) == -1 &&
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
 * The rows a refused import is given, enough for its tables to span several
 * windows and for half of them to need more memory than the import's own
 * bookkeeping, and the distinct values of each of its columns, enough for
 * that many distinct pairs.
 */
#define	BOUNDED_ROWS	(RELIC_IMPORT_WINDOW_BYTES / 4 + 40000)
#define	BOUNDED_VALUES	1024

/* The limit on tuples the first refusal is held to. */
#define	BOUNDED_TUPLES	10

/*
 * An import a limit refuses stops as soon as the facts it has read take the
 * base stores past the limit, rather than staging and interning the whole
 * table first.  Each table ends in a malformed record, which an import
 * reading on would reach and report in place of the limit, and where the
 * import stopped is read from the stream.  A batch import reads at most
 * two windows and the chunk that crossed the second's horizon past the
 * facts it staged; an import with batching off reads a chunk at a time.
 */
static void
a_refused_import_stops_reading(void)
{
	refused_import_stops_reading(BATCHED,
	    2 * RELIC_IMPORT_WINDOW_BYTES + RELIC_IMPORT_READ_CHUNK,
	    "two windows and a chunk");
	refused_import_stops_reading(RELIC_UNSTABLE_NO_BATCH_IMPORT,
	    RELIC_IMPORT_READ_CHUNK, "a chunk");
}

/* An input limit far below a chunk, so reading past it is measurable. */
#define	LONG_FIELD_MAX	100

/*
 * An import refused for a field past the input limit stops reading near
 * it, however long the field goes on: within a chunk of the byte that
 * passes the limit read a field at a time, and within two windows and a
 * chunk read a window at a time.
 */
static void
a_long_field_stops_reading(void)
{
	static const struct {
		unsigned int	 flags;
		size_t		 ahead;
		const char	*unit;
	} ways[] = {
		{ RELIC_UNSTABLE_NO_BATCH_IMPORT, RELIC_IMPORT_READ_CHUNK,
		    "a chunk" },
		{ BATCHED, 2 * RELIC_IMPORT_WINDOW_BYTES +
		    RELIC_IMPORT_READ_CHUNK, "two windows and a chunk" }
	};
	char expected[TRANSCRIPT_MAX];
	relic_config_t config;
	capture_t capture;
	relic_t *ctx;
	size_t length;
	size_t cutoff;
	size_t i;
	long read;
	char *table;
	int status;

	length = 2 + LONG_FIELD_MAX + 2 * RELIC_IMPORT_WINDOW_BYTES +
	    2 * RELIC_IMPORT_READ_CHUNK;
	table = malloc(length);
	if (table == NULL) {
		check(false, SCENARIO_INPUT, "the table is built");
		return;
	}
	table[0] = 'a';
	table[1] = '\t';
	memset(table + 2, 'x', length - 2);
	cutoff = 2 + LONG_FIELD_MAX + 1;
	snprintf(expected, sizeof(expected),
	    "limit exceeded: input=%d while importing e", LONG_FIELD_MAX);

	for (i = 0; i < sizeof(ways) / sizeof(ways[0]); i++) {
		bzero(&config, sizeof(config));
		config.flags = ways[i].flags;
		config.max_input = LONG_FIELD_MAX;
		ctx = context_with_capture(&config, &capture);
		if (ctx == NULL) {
			check(false, SCENARIO_INPUT, "a context is made");
			break;
		}
		status = import_counting(ctx, table, length, &read);
		checkf(status == -1 && strcmp(capture.message, expected) == 0 &&
		    capture.line == 1 && capture.column == 3, SCENARIO_INPUT,
		    "a field past the limit should be refused at e:1:3 with "
		    "\"%s\", not at %u:%u with \"%s\"", expected, capture.line,
		    capture.column, capture.message);
		checkf(read >= 0 && (size_t)read <= cutoff + ways[i].ahead,
		    SCENARIO_INPUT, "the refused import should stop within %s "
		    "of byte %zu, not at byte %ld", ways[i].unit, cutoff, read);
		relic_destroy(ctx);
	}

	free(table);
}

/*
 * Check a refused import, made with context `flags`, reads no more than
 * `ahead` bytes, named by `unit`, past the row it is refused at.
 *
 * Under the tuple limit, a table whose every row is new is refused at its
 * eleventh row, having interned the predicate and that row's symbols but no
 * later row's: e/2, and a0 and b0 to b10, a predicate's name being no
 * symbol.  Under the memory limit, a table of distinct pairs over a few
 * values, all interned beforehand so that the import interns nothing, is
 * refused at the row whose columns take the floor past what the limit
 * leaves, the one after half the table.
 */
static void
refused_import_stops_reading(unsigned flags, size_t ahead, const char *unit)
{
	char expected[TRANSCRIPT_MAX];
	char name[32];
	relic_config_t config;
	capture_t capture;
	relic_dom_t dom;
	relic_t *ctx;
	size_t predicates;
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
	config.flags = flags;
	config.max_tuples = BOUNDED_TUPLES;
	ctx = context_with_capture(&config, &capture);
	if (ctx != NULL) {
		symbols = relic_symbol_count(ctx);
		predicates = relic_pred_count(ctx);
		status = import_counting(ctx, table, length, &read);
		checkf(status == -1 && strcmp(capture.message,
		    "limit exceeded: tuples=10 while loading e/2") == 0,
		    SCENARIO_BOUNDED, "the tuple limit should refuse the "
		    "import, not \"%s\"", capture.message);
		checkf(relic_symbol_count(ctx) - symbols ==
		    BOUNDED_TUPLES + 2 && !interned(ctx, "e") &&
		    interned(ctx, "b10") && !interned(ctx, "b11"),
		    SCENARIO_BOUNDED, "the refused import should intern a0 "
		    "and b0 to b10, %d symbols, not %zu", BOUNDED_TUPLES + 2,
		    relic_symbol_count(ctx) - symbols);
		check(relic_pred_count(ctx) - predicates == 1,
		    SCENARIO_BOUNDED, "the refused import should intern e/2");
		cutoff = row_offset(table, BOUNDED_TUPLES + 1);
		checkf(read >= 0 && (size_t)read >= cutoff &&
		    (size_t)read <= cutoff + ahead,
		    SCENARIO_BOUNDED, "the refused import should stop within "
		    "%s of row %d, byte %zu, not at byte %ld", unit,
		    BOUNDED_TUPLES + 1, cutoff, read);
		relic_destroy(ctx);
	}
	free(table);

	table = pairs_table(BOUNDED_ROWS, BOUNDED_VALUES, &length);
	if (table == NULL) {
		check(false, SCENARIO_BOUNDED, "the table is built");
		return;
	}
	bzero(&config, sizeof(config));
	config.flags = flags;
	ctx = context_with_capture(&config, &capture);
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
		    (size_t)read <= cutoff + ahead,
		    SCENARIO_BOUNDED, "the refused import should stop within "
		    "%s of row %zu, byte %zu, not at byte %ld", unit,
		    (size_t)BOUNDED_ROWS / 2 + 1, cutoff, read);
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

	status = relic_import_file(ctx, "e", stream, &import, sizeof(import));
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

/*
 * The offset of the byte after the first `rows` rows of a table.  The
 * table is measured once: a sanitizer checking string arguments measures
 * the rest of the table at each strchr(3), which over a table of windows
 * is quadratic.
 */
static size_t
row_offset(const char *table, size_t rows)
{
	const char *end;
	const char *limit;
	size_t i;

	end = table;
	limit = table + strlen(table);
	for (i = 0; i < rows; i++)
		end = (const char *)memchr(end, '\n', (size_t)(limit - end)) + 1;

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
 * A table is staged a window at a time only where that repays: on
 * RELIC_IMPORT_BATCH_WORKERS threads or more, and when it is larger than
 * the window its first read fills.  RELIC_UNSTABLE_PARALLEL_ALWAYS batches any
 * table on any number of threads, and RELIC_UNSTABLE_NO_BATCH_IMPORT none.
 * Whether an import was batched shows in the windows it staged; the facts
 * are the same either way.
 */
static void
batching_follows_the_threads_and_the_table(void)
{
	static const char small[] = "a\tb\nc\td\n";
	char *large;
	size_t length;

	large = pairs_table(BOUNDED_ROWS, BOUNDED_VALUES, &length);
	if (large == NULL) {
		check(false, SCENARIO_POLICY, "the table is built");
		return;
	}
	/* Without the malformed record pairs_table() ends it with. */
	length -= sizeof("x\n") - 1;

	check_batching(large, length, 0, 1, false,
	    "a large table on one thread");
	check_batching(large, length, 0, RELIC_IMPORT_BATCH_WORKERS - 1, false,
	    "a large table on too few threads");
	check_batching(small, sizeof(small) - 1, 0, RELIC_IMPORT_BATCH_WORKERS,
	    false, "a small table on enough threads");
	check_batching(large, length, 0, RELIC_IMPORT_BATCH_WORKERS, true,
	    "a large table on enough threads");
	check_batching(small, sizeof(small) - 1,
	    RELIC_UNSTABLE_PARALLEL_ALWAYS, 1, true,
	    "a small table on one thread, batching forced");
	check_batching(large, length,
	    RELIC_UNSTABLE_PARALLEL_ALWAYS | RELIC_UNSTABLE_NO_BATCH_IMPORT,
	    RELIC_IMPORT_BATCH_WORKERS, false,
	    "a large table on enough threads, batching off");

	free(large);
}

/*
 * Check an import of `table` into a context of `flags` and `threads` is
 * staged a window at a time exactly when `batched` says.  A library built
 * without threads has one, so on enough threads it batches only when
 * forced to.
 */
static void
check_batching(const char *table, size_t length, unsigned int flags,
    size_t threads, bool batched, const char *label)
{
	static const enum relic_column columns[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_SYMBOL
	};
	relic_config_t config;
	capture_t capture;
	relic_t *ctx;
	int status;

	bzero(&config, sizeof(config));
	config.flags = flags;
	config.threads = threads;
	ctx = context_with_capture(&config, &capture);
	if (ctx == NULL) {
		check(false, SCENARIO_POLICY, "a context is made");
		return;
	}
	if (ctx->config.threads < threads &&
	    (flags & RELIC_UNSTABLE_PARALLEL_ALWAYS) == 0)
		batched = false;

	status = import_table(ctx, "table", RELIC_FORMAT_TSV, 0, "e",
	    columns, 2, table, length);
	checkf(status == 0 && capture.errors == 0, SCENARIO_POLICY,
	    "%s: the import should succeed", label);
	checkf((ctx->counters.windows_staged > 0) == batched &&
	    ctx->counters.windows_declined == 0, SCENARIO_POLICY,
	    "%s: the import should %s, not stage %zu and decline %zu",
	    label, batched ? "stage windows" : "stage no window",
	    (size_t)ctx->counters.windows_staged,
	    (size_t)ctx->counters.windows_declined);

	relic_destroy(ctx);
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
 * serves `table`, or, when `fail_errno` is set, serves it up to byte
 * `fail_at` and fails every read from there with `fail_errno`.
 */
struct reentering_read {
	relic_t			*ctx;
	const char		*table;
	enum relic_column	*columns;
	size_t			 column_count;
	char			*predicate;
	char			*source_name;
	size_t			 offset;
	size_t			 fail_at;
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
		if (reading->source_name != NULL)
			reading->source_name[0] = 'X';
	}

	if (reading->fail_errno != 0 && reading->offset >= reading->fail_at) {
		errno = reading->fail_errno;
		return (-1);
	}

	remaining = strlen(reading->table) - reading->offset;
	if (reading->fail_errno != 0 &&
	    remaining > reading->fail_at - reading->offset)
		remaining = reading->fail_at - reading->offset;
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

	check(relic_import_file(reading.ctx, "r", stream, &import,
	    sizeof(import)) == 0 &&
	    reading.refusals == 1, SCENARIO_REENTRY,
	    "the read function's call should be refused");
	check(rows_of(reading.ctx, "r(X)?") == 2, SCENARIO_REENTRY,
	    "the import it ran inside should complete");

	fclose(stream);
	relic_destroy(reading.ctx);
}

/*
 * The source name is borrowed too, and a read function that rewrites it
 * while the table is read changes nothing: a mistake in the table is
 * located in the source as it was named when the import began.
 */
static void
a_source_name_is_read_once(void)
{
	static const enum relic_column columns[] = { RELIC_COLUMN_INTEGER };
	char source_name[] = "original.csv";
	reentering_read_t reading;
	relic_import_t import;
	capture_t capture;
	FILE *stream;

	bzero(&reading, sizeof(reading));
	reading.ctx = context_with_capture(NULL, &capture);
	if (reading.ctx == NULL)
		return;

	reading.table = "1\nx\n";
	reading.source_name = source_name;
	stream = reentering_stream(&reading);
	if (stream == NULL) {
		printf("skip: %s: no stream with a read function of the "
		    "caller's own\n", SCENARIO_LOCATION);
		relic_destroy(reading.ctx);
		return;
	}

	bzero(&import, sizeof(import));
	import.predicate = "n";
	import.columns = columns;
	import.column_count = 1;
	import.format = RELIC_FORMAT_TSV;

	check(relic_import_file(reading.ctx, source_name, stream,
	    &import, sizeof(import)) != 0 && source_name[0] == 'X',
	    SCENARIO_LOCATION,
	    "an import of a field that is not an integer fails");
	checkf(strcmp(capture.file, "original.csv") == 0 && capture.line == 2,
	    SCENARIO_LOCATION, "the mistake should be located at "
	    "original.csv:2, not %s:%u", capture.file,
	    (unsigned int)capture.line);

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

	check(relic_import_file(reading.ctx, "w", stream, &import,
	    sizeof(import)) == 0 &&
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

	check(relic_import_file(reading.ctx, "broken", stream, &import,
	    sizeof(import)) ==
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

	check(relic_import_path(ctx, "/dev/null", &import,
	    sizeof(import)) == 0 &&
	    capture.errors == 0 && rows_of(ctx, "r(X)?") == 0,
	    SCENARIO_LOCATION, "an empty file should import no facts");

	import.flags = RELIC_IMPORT_HEADER;
	check(relic_import_path(ctx, "/dev/null", &import,
	    sizeof(import)) == -1 &&
	    strcmp(capture.file, "/dev/null") == 0,
	    SCENARIO_LOCATION, "a mistake in a file should name its path");

	import.flags = 0;
	check(relic_import_path(ctx, missing, &import, sizeof(import)) == -1 &&
	    strncmp(capture.message, unopened, sizeof(unopened) - 1) == 0,
	    SCENARIO_LOCATION, "a file that cannot be opened should be named");

	relic_destroy(ctx);
}

/*
 * The lengths of filler put before a padded table, enough for its first
 * byte to fall at every offset within a read chunk of the import built to
 * read small ones, and a few more where the chunks are large.
 */
#define	BATCH_SHIFTS	(RELIC_IMPORT_READ_CHUNK < 32 ? \
	RELIC_IMPORT_READ_CHUNK + 1 : 33)

/*
 * The input limit the batch cases that set one are compared under, longer
 * than any padding field, and a field of exactly that many bytes.
 */
#define	BATCH_INPUT_MAX		40
#define	BATCH_INPUT_FIELD	"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

/*
 * The rows of the generated tables, enough to span several windows, and
 * the distinct symbols they repeat.
 */
#define	GENERATED_ROWS		(RELIC_IMPORT_WINDOW_BYTES / 6 + 200)
#define	GENERATED_SYMBOLS	5000

/*
 * The columns of the generated tables, and the program loaded before
 * them, which holds one of their values in each column.
 */
static const enum relic_column generated_columns[] = {
	RELIC_COLUMN_SYMBOL, RELIC_COLUMN_INTEGER, RELIC_COLUMN_STRING
};
#define	GENERATED_PROGRAM	"e(s3, 3, \"s4\").\n"

/* The memory limits a long import is compared under, past the first. */
#define	MEMORY_STEPS		8

/*
 * Every table is imported a window at a time and a field at a time, into
 * contexts made alike, and the two must end alike: the same status and
 * diagnostic, the same values interned in the same order with the same
 * uses, the same capacities and accounting, and the same facts in the same
 * order.  Among the tables are every quoting and escape, line ends of both
 * kinds, a header, skipped columns, symbols and strings of the same bytes,
 * fields holding NUL and bytes above 127, tuples of no columns, a type
 * error before a malformed record, a limit met before a malformed tail,
 * and tables long enough to span windows.
 */
static void
a_batch_import_matches_the_scalar_import(void)
{
	static const enum relic_column symbol_string[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_STRING
	};
	static const enum relic_column symbol_skip_integer[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_SKIP, RELIC_COLUMN_INTEGER
	};
	static const enum relic_column string_symbol[] = {
		RELIC_COLUMN_STRING, RELIC_COLUMN_SYMBOL
	};
	static const enum relic_column skipped[] = {
		RELIC_COLUMN_SKIP, RELIC_COLUMN_SKIP
	};
	static const batch_case_t cases[] = {
		{ .label = "tsv escapes", BATCH_TABLE("a\ta\nb\ta\\tb\n"
		    "a\t\\\\\nc\tline\\none\nb\ta\\tb\n\tempty\na\t\n"),
		    .pad = "\tq\n", .format = RELIC_FORMAT_TSV,
		    BATCH_COLUMNS(symbol_string) },
		{ .label = "csv quoting", BATCH_TABLE("a,\"x,y\"\r\n"
		    "b,\"say \"\"hi\"\"\"\nc,\"two\nlines\"\r\na,\"x,y\"\n"
		    "\"d\",\n"),
		    .pad = ",q\n", .format = RELIC_FORMAT_CSV,
		    BATCH_COLUMNS(symbol_string), .whole = true },
		{ .label = "nul and high bytes",
		    BATCH_TABLE("a,\"\0\xff\"\nb,\"x\0\"\nc,\"\xc3\xa9\"\n"
		    "a,\"x\0\"\nc,\"\0\xff\"\n"),
		    .format = RELIC_FORMAT_CSV, BATCH_COLUMNS(symbol_string),
		    .whole = true },
		{ .label = "csv header",
		    BATCH_TABLE("name,text\r\na,b\nb,\"a\"\n"),
		    .format = RELIC_FORMAT_CSV, .flags = RELIC_IMPORT_HEADER,
		    BATCH_COLUMNS(symbol_string), .whole = true },
		{ .label = "a header alone", BATCH_TABLE("name\tx\tn\n"),
		    .format = RELIC_FORMAT_TSV, .flags = RELIC_IMPORT_HEADER,
		    BATCH_COLUMNS(symbol_skip_integer), .whole = true },
		{ .label = "no final line end",
		    BATCH_TABLE("a\tskip\t1\nb\tskip\t-2"),
		    .pad = "\tq\t0\n", .format = RELIC_FORMAT_TSV,
		    BATCH_COLUMNS(symbol_skip_integer), .whole = true },
		{ .label = "a quoted last field and no final line end",
		    BATCH_TABLE("a,b\nc,\"d\""),
		    .pad = ",q\n", .format = RELIC_FORMAT_CSV,
		    BATCH_COLUMNS(symbol_string), .whole = true },
		{ .label = "an empty table", BATCH_TABLE(""),
		    .format = RELIC_FORMAT_TSV, BATCH_COLUMNS(symbol_string),
		    .whole = true },
		{ .label = "values of the program",
		    .program = "e(a, \"b\").\ne(c, \"a\").\n",
		    BATCH_TABLE("b\tb\na\tb\nc\ta\nb\tc\n"), .pad = "\tq\n",
		    .format = RELIC_FORMAT_TSV, BATCH_COLUMNS(symbol_string),
		    .whole = true },
		{ .label = "strings before symbols",
		    .program = "e(\"a\", b).\n",
		    BATCH_TABLE("a\ta\nb\tb\nc\ta\n"), .pad = "\tq\n",
		    .format = RELIC_FORMAT_TSV, BATCH_COLUMNS(string_symbol),
		    .whole = true },
		{ .label = "integers", BATCH_TABLE("x\t-\t9223372036854775807\n"
		    "y\t-\t-9223372036854775808\nx\t-\t0\nz\t-\t0\n"
		    "y\t-\t9223372036854775807\n"), .pad = "\t-\t7\n",
		    .format = RELIC_FORMAT_TSV,
		    BATCH_COLUMNS(symbol_skip_integer), .whole = true },
		{ .label = "no columns", BATCH_TABLE("a\tb\nc\td\n"),
		    .pad = "\tq\n", .format = RELIC_FORMAT_TSV,
		    BATCH_COLUMNS(skipped), .whole = true },
		{ .label = "a type error before a malformed record",
		    BATCH_TABLE("a\t-\t1\nb\t-\tx\nc\t-\t3\nd\n"),
		    .pad = "\t-\t0\n", .format = RELIC_FORMAT_TSV,
		    BATCH_COLUMNS(symbol_skip_integer) },
		{ .label = "a symbol column refusing a field",
		    BATCH_TABLE("a\tb\nB\tc\n"), .pad = "\tq\n",
		    .format = RELIC_FORMAT_TSV, BATCH_COLUMNS(symbol_string) },
		{ .label = "a malformed tail", BATCH_TABLE("a,b\nc,d\ne,\"f\n"),
		    .pad = ",q\n", .format = RELIC_FORMAT_CSV,
		    BATCH_COLUMNS(symbol_string) },
		{ .label = "a quote in an unquoted field",
		    BATCH_TABLE("a,b\nc,d\"e\n"), .pad = ",q\n",
		    .format = RELIC_FORMAT_CSV, BATCH_COLUMNS(symbol_string) },
		{ .label = "a limit before a malformed tail",
		    BATCH_TABLE("a\tb\nc\td\ne\tf\ng\th\ni\n"),
		    .pad = "\tq\n", .format = RELIC_FORMAT_TSV,
		    BATCH_COLUMNS(symbol_string), .max_tuples = 3 },
		{ .label = "a limit met by the program",
		    .program = "e(a, \"b\").\n", BATCH_TABLE("a\tb\nc\td\n"),
		    .pad = "\tq\n", .format = RELIC_FORMAT_TSV,
		    BATCH_COLUMNS(symbol_string), .max_tuples = 2 },
		{ .label = "a field at the input limit",
		    BATCH_TABLE("a,\"" BATCH_INPUT_FIELD "\"\nb,c\n"),
		    .pad = ",q\n", .format = RELIC_FORMAT_CSV,
		    BATCH_COLUMNS(symbol_string),
		    .max_input = BATCH_INPUT_MAX, .whole = true },
		{ .label = "a field past the input limit",
		    BATCH_TABLE("a\tb\nc\t" BATCH_INPUT_FIELD "y\nd\te\n"),
		    .pad = "\tq\n", .format = RELIC_FORMAT_TSV,
		    BATCH_COLUMNS(symbol_string),
		    .max_input = BATCH_INPUT_MAX },
		{ .label = "an open quote past the input limit",
		    BATCH_TABLE("a,b\nc,\"" BATCH_INPUT_FIELD "y\n"),
		    .pad = ",q\n", .format = RELIC_FORMAT_CSV,
		    BATCH_COLUMNS(symbol_string),
		    .max_input = BATCH_INPUT_MAX },
		{ .label = "a header past the input limit",
		    BATCH_TABLE("name," BATCH_INPUT_FIELD "y\na,b\n"),
		    .format = RELIC_FORMAT_CSV, .flags = RELIC_IMPORT_HEADER,
		    BATCH_COLUMNS(symbol_string),
		    .max_input = BATCH_INPUT_MAX },
	};
	window_tally_t tally;
	char *long_record;
	char *faults;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bzero(&tally, sizeof(tally));
		compare_shifted_imports(&cases[i], &tally);
		checkf(!cases[i].whole || tally.declined == 0, SCENARIO_BATCH,
		    "%s: a well-formed table should have every window staged, "
		    "not %" PRIu64 " declined", cases[i].label, tally.declined);
	}

	bzero(&tally, sizeof(tally));
	compare_generated_imports("a long table", 0, NULL, 0, 0, &tally);
	checkf(tally.staged >= 4 && tally.declined == 0, SCENARIO_BATCH,
	    "a long table imported twice should be staged in two windows or "
	    "more each and none declined, not %" PRIu64 " staged and %"
	    PRIu64 " declined", tally.staged, tally.declined);
#ifdef RELIC_THREADS
	checkf(tally.framed_ahead > 0, SCENARIO_BATCH, "a long table imported "
	    "on four threads should frame windows ahead, not %" PRIu64
	    " steps", tally.framed_ahead);
#endif
	compare_generated_imports("a type error in a late window",
	    GENERATED_ROWS - 100, "s1\tx\tq\n", 0, 0, &tally);
	compare_generated_imports("a short record in a late window",
	    GENERATED_ROWS / 2, "s1\t2\n", 0, 0, &tally);
	long_record = record_longer_than_a_window();
	if (long_record == NULL)
		check(false, SCENARIO_BATCH, "the record is built");
	else
		compare_generated_imports("a record longer than a window",
		    GENERATED_ROWS / 2, long_record, 0, 0, &tally);
	free(long_record);
	faults = faults_a_window_apart();
	if (faults == NULL)
		check(false, SCENARIO_BATCH, "the records are built");
	else
		compare_generated_imports("a type error a window before a "
		    "short record", GENERATED_ROWS / 4, faults, 0, 0, &tally);
	free(faults);
	compare_generated_imports("a tuple limit in a late window", 0, NULL,
	    GENERATED_ROWS / 2, 0, &tally);
	compare_memory_limits(&tally);
	compare_long_field_late(&tally);
	compare_long_field_after_resuming(&tally);
	checkf(tally.staged > 0 && tally.declined > 0, SCENARIO_BATCH,
	    "the long tables should have windows staged and windows declined, "
	    "not %" PRIu64 " and %" PRIu64, tally.staged, tally.declined);
}

/*
 * Compare the two ways of importing a generated table whose record half way
 * through holds a field past the input limit.
 */
static void
compare_long_field_late(window_tally_t *tally)
{
	static const char label[] = "a field past the input limit in a late "
	    "window";
	static const char odd[] = "s1\t1\t" BATCH_INPUT_FIELD "y\n";
	batch_case_t batch;
	size_t length;
	char *table;

	table = generated_table(GENERATED_ROWS, GENERATED_ROWS / 2, odd,
	    &length);
	if (table == NULL) {
		check(false, SCENARIO_BATCH, "the table is built");
		return;
	}

	bzero(&batch, sizeof(batch));
	batch.label = label;
	batch.program = GENERATED_PROGRAM;
	batch.table = table;
	batch.length = length;
	batch.format = RELIC_FORMAT_TSV;
	batch.columns = generated_columns;
	batch.column_count = 3;
	batch.max_input = BATCH_INPUT_MAX;
	compare_import_ways(&batch, table, length, label, tally);

	free(table);
}

/*
 * Compare the two ways of importing a generated table whose first record
 * is longer than a window, its two symbols each at the input limit, so
 * that its window is declined and windows resume after it, and whose
 * record half way through holds a field one byte past the limit, short
 * enough to be framed in a window after resuming.
 */
static void
compare_long_field_after_resuming(window_tally_t *tally)
{
	static const char label[] = "a field past the input limit after "
	    "windows resume";
	window_tally_t local;
	batch_case_t batch;
	size_t generated;
	size_t length;
	size_t limit;
	size_t prefix;
	size_t w;
	char *odd;
	char *rows;
	char *table;
	const batch_way_t *way;

	_Static_assert(RELIC_IMPORT_WINDOW_BYTES / 2 + RELIC_IMPORT_READ_CHUNK
	    + 8 < RELIC_IMPORT_WINDOW_BYTES, "a field past the limit fits a "
	    "window");

	limit = RELIC_IMPORT_WINDOW_BYTES / 2 + RELIC_IMPORT_READ_CHUNK;
	prefix = 2 * limit + 4;

	odd = malloc(limit + 8);
	if (odd == NULL) {
		check(false, SCENARIO_BATCH, "the table is built");
		return;
	}
	bcopy("s1\t1\t", odd, 5);
	memset(odd + 5, 'x', limit + 1);
	bcopy("\n", odd + 5 + limit + 1, 2);
	rows = generated_table(GENERATED_ROWS, GENERATED_ROWS / 2, odd,
	    &generated);
	free(odd);
	table = rows != NULL ? malloc(prefix + generated) : NULL;
	if (table == NULL) {
		check(false, SCENARIO_BATCH, "the table is built");
		free(rows);
		return;
	}
	memset(table, 'y', limit);
	bcopy("\t1\t", table + limit, 3);
	memset(table + limit + 3, 'y', limit);
	table[prefix - 1] = '\n';
	bcopy(rows, table + prefix, generated);
	free(rows);
	length = prefix + generated;

	bzero(&batch, sizeof(batch));
	batch.label = label;
	batch.program = GENERATED_PROGRAM;
	batch.table = table;
	batch.length = length;
	batch.format = RELIC_FORMAT_TSV;
	batch.columns = generated_columns;
	batch.column_count = 3;
	batch.max_input = limit;
	for (w = 0; w < sizeof(batch_ways) / sizeof(batch_ways[0]); w++) {
		way = &batch_ways[w];
		bzero(&local, sizeof(local));
		compare_batched_import(&batch, table, length, label, way,
		    &local);
		checkf((way->flags & BATCHED) == 0 ||
		    (local.declined >= 4 && local.staged >= 2), SCENARIO_BATCH,
		    "%s, %s: each of two imports should decline a window at "
		    "the long record and one at the long field and stage "
		    "windows between them, not %" PRIu64 " declined and %"
		    PRIu64 " staged in all", label, way->name, local.declined,
		    local.staged);
		tally->staged += local.staged;
		tally->declined += local.declined;
		tally->framed_ahead += local.framed_ahead;
	}

	free(table);
}

/*
 * A record of the generated tables' columns longer than a window, in a new
 * string, or NULL when memory ran out.
 */
static char *
record_longer_than_a_window(void)
{
	size_t length;
	char *record;

	length = RELIC_IMPORT_WINDOW_BYTES + RELIC_IMPORT_READ_CHUNK;
	record = malloc(length + 1);
	if (record == NULL)
		return (NULL);
	memset(record, 'y', length);
	bcopy("s1\t1\t", record, 5);
	record[length - 1] = '\n';
	record[length] = '\0';

	return (record);
}

/*
 * Records of the generated tables' columns, in a new string, or NULL when
 * memory ran out: a type error, then more than a window of good records,
 * then a record short of a field.  The window holding the type error is
 * analyzed while the next is framed, and that framing meets the short
 * record; the type error, before it, is what the import reports.
 */
static char *
faults_a_window_apart(void)
{
	static const char type_error[] = "s1\tx\tq\n";
	static const char good[] = "s2\t2\ts3\n";
	static const char short_record[] = "s1\t2\n";
	char *at;
	char *records;
	size_t i;
	size_t length;
	size_t rows;

	rows = (RELIC_IMPORT_WINDOW_BYTES + RELIC_IMPORT_READ_CHUNK) /
	    (sizeof(good) - 1) + 1;
	length = sizeof(type_error) - 1 + rows * (sizeof(good) - 1) +
	    sizeof(short_record) - 1;
	records = malloc(length + 1);
	if (records == NULL)
		return (NULL);

	at = records;
	bcopy(type_error, at, sizeof(type_error) - 1);
	at += sizeof(type_error) - 1;
	for (i = 0; i < rows; i++) {
		bcopy(good, at, sizeof(good) - 1);
		at += sizeof(good) - 1;
	}
	bcopy(short_record, at, sizeof(short_record));

	return (records);
}

/*
 * Compare the two ways of importing the long generated table under memory
 * limits stepped from what the context holds before the import to what it
 * holds after, so that some limit refuses the import at each stage of it.
 */
static void
compare_memory_limits(window_tally_t *tally)
{
	char label[64];
	relic_config_t config;
	capture_t capture;
	relic_t *ctx;
	size_t before;
	size_t after;
	size_t length;
	size_t step;
	char *table;

	table = generated_table(GENERATED_ROWS, 0, NULL, &length);
	if (table == NULL) {
		check(false, SCENARIO_BATCH, "the table is built");
		return;
	}
	bzero(&config, sizeof(config));
	config.flags = RELIC_UNSTABLE_NO_BATCH_IMPORT;
	ctx = context_with_capture(&config, &capture);
	before = 0;
	after = 0;
	if (ctx != NULL && relic_load_string(ctx, "program.dl",
	    GENERATED_PROGRAM) == 0) {
		before = accounted_bytes(ctx);
		if (import_table(ctx, "table", RELIC_FORMAT_TSV, 0, "e",
		    generated_columns, 3, table, length) == 0)
			after = accounted_bytes(ctx);
	}
	relic_destroy(ctx);
	free(table);
	if (after <= before) {
		check(false, SCENARIO_BATCH, "the long table is imported");
		return;
	}

	for (step = 0; step <= MEMORY_STEPS; step++) {
		snprintf(label, sizeof(label), "a memory limit %zu/%d of the "
		    "way", step, MEMORY_STEPS);
		compare_generated_imports(label, 0, NULL, 0,
		    before + (after - before) * step / MEMORY_STEPS, tally);
	}
}

/* The bytes a context's account holds against its memory limit. */
static size_t
accounted_bytes(const relic_t *ctx)
{
	return (ctx->account.store_bytes + ctx->account.value_bytes +
	    ctx->account.pool_bytes);
}

/*
 * Compare the two ways of importing a table, behind every length of
 * filler a padded table takes.
 */
static void
compare_shifted_imports(const batch_case_t *batch, window_tally_t *tally)
{
	char label[128];
	size_t shifts;
	size_t shift;
	size_t length;
	char *table;

	shifts = batch->pad != NULL ? BATCH_SHIFTS : 1;
	for (shift = 0; shift < shifts; shift++) {
		table = shifted_table(batch, shift, &length);
		if (table == NULL) {
			check(false, SCENARIO_BATCH, "the table is built");
			return;
		}
		snprintf(label, sizeof(label), "%s behind %zu bytes",
		    batch->label, length - batch->length);
		compare_import_ways(batch, table, length, label, tally);
		free(table);
	}
}

/*
 * The table of `batch` behind a padding record whose first field is `shift`
 * bytes long, or behind none at shift 0.  NULL when memory ran out.
 */
static char *
shifted_table(const batch_case_t *batch, size_t shift, size_t *lengthp)
{
	size_t pad_length;
	size_t rest;
	char *table;

	rest = shift > 0 ? strlen(batch->pad) : 0;
	pad_length = shift > 0 ? shift + rest : 0;
	table = malloc(pad_length + batch->length + 1);
	if (table == NULL)
		return (NULL);
	if (shift > 0) {
		table[0] = 'p';
		memset(table + 1, 'x', shift - 1);
		bcopy(batch->pad, table + shift, rest);
	}
	bcopy(batch->table, table + pad_length, batch->length);
	table[pad_length + batch->length] = '\0';

	*lengthp = pad_length + batch->length;
	return (table);
}

/*
 * Compare the two ways of importing a generated table of a symbol, an
 * integer and a string column, holding `odd` in place of row `odd_row` when
 * it is set, under a tuple limit of `max_tuples` and a memory limit of
 * `max_memory` when each is set.
 */
static void
compare_generated_imports(const char *label, size_t odd_row,
    const char *odd, size_t max_tuples, size_t max_memory,
    window_tally_t *tally)
{
	batch_case_t batch;
	size_t length;
	char *table;

	table = generated_table(GENERATED_ROWS, odd_row, odd, &length);
	if (table == NULL) {
		check(false, SCENARIO_BATCH, "the table is built");
		return;
	}

	bzero(&batch, sizeof(batch));
	batch.label = label;
	batch.program = GENERATED_PROGRAM;
	batch.table = table;
	batch.length = length;
	batch.format = RELIC_FORMAT_TSV;
	batch.columns = generated_columns;
	batch.column_count = 3;
	batch.max_tuples = max_tuples;
	batch.max_memory = max_memory;
	compare_import_ways(&batch, table, length, label, tally);

	free(table);
}

/*
 * A table of `rows` records repeating a few symbols, integers and strings,
 * with duplicate records among them, the record at `odd_row` replaced by
 * `odd` when that is set.  NULL when memory ran out.
 */
static char *
generated_table(size_t rows, size_t odd_row, const char *odd,
    size_t *lengthp)
{
	size_t capacity;
	size_t length;
	size_t i;
	char *table;

	capacity = rows * 32 + (odd != NULL ? strlen(odd) : 0) + 1;
	table = malloc(capacity);
	if (table == NULL)
		return (NULL);

	length = 0;
	for (i = 0; i < rows; i++) {
		if (odd != NULL && i == odd_row)
			length += (size_t)snprintf(table + length,
			    capacity - length, "%s", odd);
		else
			length += (size_t)snprintf(table + length,
			    capacity - length, "s%zu\t%zu\ts%zu\n",
			    i * 7 % GENERATED_SYMBOLS, i % 13,
			    i * 3 % GENERATED_SYMBOLS);
	}

	*lengthp = length;
	return (table);
}

/*
 * Import a table each of the batch_ways and check each ends as the import
 * a field at a time does, adding the batch imports' windows to `tally`.
 * The windows' jobs are divided among the threads differently each time,
 * and nothing they write may depend on how.
 */
static void
compare_import_ways(const batch_case_t *batch, const char *table,
    size_t length, const char *label, window_tally_t *tally)
{
	size_t w;

	for (w = 0; w < sizeof(batch_ways) / sizeof(batch_ways[0]); w++)
		compare_batched_import(batch, table, length, label,
		    &batch_ways[w], tally);
}

/*
 * Import a table `way` and a field at a time, each into a context of its
 * own, and check the two end alike: after the first import, and again
 * after a second, which meets the facts the first added.  Adds the batch
 * import's windows to `tally`.
 */
static void
compare_batched_import(const batch_case_t *batch, const char *table,
    size_t length, const char *label, const batch_way_t *way,
    window_tally_t *tally)
{
	import_outcome_t batched;
	import_outcome_t scalar;

	bzero(&batched, sizeof(batched));
	if (import_first(batch, table, length,
	    RELIC_UNSTABLE_NO_BATCH_IMPORT, 1, &scalar) != 0 ||
	    import_first(batch, table, length, way->flags,
	    way->threads, &batched) != 0) {
		check(false, SCENARIO_BATCH, "a context is made and loaded");
		relic_destroy(scalar.ctx);
		relic_destroy(batched.ctx);
		return;
	}
	checkf(scalar.ctx->counters.windows_staged == 0 &&
	    scalar.ctx->counters.windows_declined == 0, SCENARIO_BATCH,
	    "%s, %s: an import a field at a time should read no window",
	    label, way->name);

	checkf(batched.status == scalar.status, SCENARIO_BATCH, "%s, %s: a "
	    "batch import should end %d, as one a field at a time does, not "
	    "%d", label, way->name, scalar.status, batched.status);
	check_same_outcome(&batched, &scalar, label, way, "one import");

	import_again(batch, table, length, &scalar);
	import_again(batch, table, length, &batched);
	checkf(batched.again == scalar.again, SCENARIO_BATCH, "%s, %s: a "
	    "second batch import should end %d, not %d", label, way->name,
	    scalar.again, batched.again);
	check_same_outcome(&batched, &scalar, label, way, "two imports");

	tally->staged += batched.ctx->counters.windows_staged;
	tally->declined += batched.ctx->counters.windows_declined;
	tally->framed_ahead += batched.ctx->counters.frames_ahead;

	relic_destroy(scalar.ctx);
	relic_destroy(batched.ctx);
}

/*
 * Check a batch import has reported what the import a field at a time
 * reported, as many errors with the last at the same place, and left its
 * context as the other left its own, after `imports`.
 */
static void
check_same_outcome(const import_outcome_t *batched,
    const import_outcome_t *scalar, const char *label,
    const batch_way_t *way, const char *imports)
{
	const capture_t *reported;
	const capture_t *expected;
	const char *difference;

	reported = &batched->capture;
	expected = &scalar->capture;
	checkf(reported->errors == expected->errors &&
	    strcmp(reported->message, expected->message) == 0 &&
	    strcmp(reported->file, expected->file) == 0 &&
	    reported->line == expected->line &&
	    reported->column == expected->column, SCENARIO_BATCH,
	    "%s, %s: after %s, a batch import should have reported %zu "
	    "errors, the last \"%s\" at %u:%u, not %zu, the last \"%s\" at "
	    "%u:%u", label, way->name, imports, expected->errors,
	    expected->message, expected->line, expected->column,
	    reported->errors, reported->message, reported->line,
	    reported->column);

	difference = context_difference(batched->ctx, scalar->ctx);
	checkf(difference == NULL, SCENARIO_BATCH, "%s, %s: after %s, a "
	    "batch import should leave the context as one a field at a time "
	    "does, but its %s differ", label, way->name, imports,
	    difference != NULL ? difference : "");
}

/*
 * Import the table of `batch` into a new context of `flags` and `threads`,
 * made and loaded as `batch` says.  Returns 0, or -1 when the context
 * could not be made or loaded, with outcome->ctx then NULL or to be
 * destroyed.
 */
static int
import_first(const batch_case_t *batch, const char *table, size_t length,
    unsigned int flags, size_t threads, import_outcome_t *outcome)
{
	relic_config_t config;

	bzero(outcome, sizeof(*outcome));
	bzero(&config, sizeof(config));
	config.flags = flags;
	config.threads = threads;
	config.max_tuples = batch->max_tuples;
	config.max_input = batch->max_input;
	outcome->ctx = context_with_capture(&config, &outcome->capture);
	if (outcome->ctx == NULL)
		return (-1);
	if (batch->program != NULL && relic_load_string(outcome->ctx,
	    "program.dl", batch->program) != 0)
		return (-1);
	if (batch->max_memory != 0)
		outcome->ctx->account.limit = batch->max_memory;

	outcome->status = import_table(outcome->ctx, "table", batch->format,
	    batch->flags, "e", batch->columns, batch->column_count, table,
	    length);

	return (0);
}

/* Import the table of `batch` again into the context of `outcome`. */
static void
import_again(const batch_case_t *batch, const char *table, size_t length,
    import_outcome_t *outcome)
{
	outcome->again = import_table(outcome->ctx, "table", batch->format,
	    batch->flags, "e", batch->columns, batch->column_count, table,
	    length);
}

/*
 * What differs between two contexts' values, accounting and facts, or NULL
 * when nothing does.
 */
static const char *
context_difference(const relic_t *a, const relic_t *b)
{
	const char *difference;

	difference = values_difference(a, b);
	if (difference != NULL)
		return (difference);

	if (a->account.store_bytes != b->account.store_bytes ||
	    a->account.value_bytes != b->account.value_bytes ||
	    a->account.pool_bytes != b->account.pool_bytes)
		return ("accounted bytes");

	return (facts_difference(a, b));
}

/*
 * What differs between the values two contexts interned: their bytes,
 * integers and order, the types each was interned as, and the capacities
 * of the tables holding them.
 */
static const char *
values_difference(const relic_t *a, const relic_t *b)
{
	const char *a_bytes;
	const char *b_bytes;
	size_t a_length;
	size_t b_length;
	size_t i;

	if (a->symbols.count != b->symbols.count ||
	    a->symbols.capacity != b->symbols.capacity)
		return ("symbol counts or capacities");
	for (i = 1; i <= a->symbols.count; i++) {
		a_bytes = relic_symbol_bytes(a, (uint32_t)i, &a_length);
		b_bytes = relic_symbol_bytes(b, (uint32_t)i, &b_length);
		if (a_length != b_length ||
		    bcmp(a_bytes, b_bytes, a_length) != 0)
			return ("symbols");
	}

	if (a->values.symbol_use_capacity != b->values.symbol_use_capacity ||
	    a->values.symbol_values != b->values.symbol_values ||
	    a->values.string_values != b->values.string_values)
		return ("value counts or use capacities");
	for (i = 0; i < a->values.symbol_use_capacity; i++)
		if (a->values.symbol_uses[i] != b->values.symbol_uses[i])
			return ("symbol uses");

	if (a->values.integer_count != b->values.integer_count ||
	    a->values.integer_capacity != b->values.integer_capacity)
		return ("integer counts or capacities");
	for (i = 0; i < a->values.integer_count; i++)
		if (a->values.integers[i] != b->values.integers[i])
			return ("integers");

	return (NULL);
}

/*
 * What differs between the facts two contexts hold: their predicates, and
 * each base store's tuples in order.
 */
static const char *
facts_difference(const relic_t *a, const relic_t *b)
{
	const relic_relation_t *a_store;
	const relic_relation_t *b_store;
	size_t count;
	size_t pred;
	size_t i;

	if (relic_pred_count(a) != relic_pred_count(b) ||
	    a->store.bases.count != b->store.bases.count)
		return ("predicates");

	for (pred = 1; pred <= a->store.bases.count; pred++) {
		a_store = relic_base_store(a, (relic_pred_id_t)pred);
		b_store = relic_base_store(b, (relic_pred_id_t)pred);
		if ((a_store == NULL) != (b_store == NULL))
			return ("base stores");
		if (a_store == NULL)
			continue;
		count = relic_relation_count(a_store);
		if (count != relic_relation_count(b_store) ||
		    a_store->arity != b_store->arity)
			return ("fact counts");
		for (i = 1; i <= count && a_store->arity > 0; i++)
			if (bcmp(relic_relation_tuple(a_store,
			    (relic_tuple_id_t)i), relic_relation_tuple(b_store,
			    (relic_tuple_id_t)i), a_store->arity *
			    sizeof(relic_dom_t)) != 0)
				return ("facts");
	}

	return (NULL);
}

/*
 * The records of the table the record cap is tested on: three windows'
 * worth at the cap and one more, each a digit and a line feed, which lie
 * well within one window's horizon.
 */
#define	CAPPED_ROWS	(3 * (size_t)RELIC_IMPORT_WINDOW_RECORDS + 1)
#define	CAPPED_WINDOWS	4

_Static_assert(2 * CAPPED_ROWS < RELIC_IMPORT_WINDOW_BYTES,
    "the capped table lies within one window's horizon");

/*
 * A window holds at most RELIC_IMPORT_WINDOW_RECORDS records, however few
 * bytes they take.  A table of CAPPED_ROWS records of two bytes each, all
 * within the first window's horizon, is staged in CAPPED_WINDOWS windows
 * when its import is batched -- three at the cap, then the last record --
 * and in none when the context chooses, since its first window's horizon
 * holds it whole.  The same table without its last line feed is staged
 * alike, its last window ending the input.  Every way ends as the import
 * a field at a time does.
 */
static void
a_window_holds_at_most_its_records(void)
{
	size_t length;
	size_t i;
	char *table;

	length = 2 * CAPPED_ROWS;
	table = malloc(length);
	if (table == NULL) {
		check(false, SCENARIO_RECORD_CAP, "the table is built");
		return;
	}
	for (i = 0; i < CAPPED_ROWS; i++) {
		table[2 * i] = (char)('0' + i % 10);
		table[2 * i + 1] = '\n';
	}

	check_capped_import("two-byte records", table, length);
	check_capped_import("two-byte records, the last without a line feed",
	    table, length - 1);

	free(table);
}

/*
 * Check `table`, of CAPPED_ROWS records of one integer column, is staged
 * in CAPPED_WINDOWS windows each of the batch_ways that batches, and in
 * none by choice, ending as the import a field at a time does.
 */
static void
check_capped_import(const char *label, const char *table, size_t length)
{
	static const enum relic_column columns[] = { RELIC_COLUMN_INTEGER };
	import_outcome_t batched;
	import_outcome_t scalar;
	batch_case_t batch;
	uint64_t expected;
	uint64_t staged;
	size_t w;
	const batch_way_t *way;

	bzero(&batch, sizeof(batch));
	batch.label = label;
	batch.format = RELIC_FORMAT_TSV;
	batch.columns = columns;
	batch.column_count = 1;
	for (w = 0; w < sizeof(batch_ways) / sizeof(batch_ways[0]); w++) {
		way = &batch_ways[w];
		if (import_first(&batch, table, length,
		    RELIC_UNSTABLE_NO_BATCH_IMPORT, 1, &scalar) != 0) {
			check(false, SCENARIO_RECORD_CAP,
			    "a context is made and loaded");
			relic_destroy(scalar.ctx);
			break;
		}
		if (import_first(&batch, table, length, way->flags,
		    way->threads, &batched) != 0) {
			check(false, SCENARIO_RECORD_CAP,
			    "a context is made and loaded");
			relic_destroy(scalar.ctx);
			relic_destroy(batched.ctx);
			break;
		}

		expected = (way->flags & BATCHED) != 0 ? CAPPED_WINDOWS : 0;
		staged = batched.ctx->counters.windows_staged;
		checkf(batched.status == 0 && staged == expected &&
		    batched.ctx->counters.windows_declined == 0,
		    SCENARIO_RECORD_CAP, "%s, %s: %zu records should be "
		    "staged in %" PRIu64 " windows of at most %zu, not %"
		    PRIu64 " with %" PRIu64 " declined", label, way->name,
		    CAPPED_ROWS, expected, (size_t)RELIC_IMPORT_WINDOW_RECORDS,
		    staged, batched.ctx->counters.windows_declined);
		check_same_outcome(&batched, &scalar, label, way,
		    "one import");

		relic_destroy(scalar.ctx);
		relic_destroy(batched.ctx);
	}
}

/*
 * A declined window does not end batching: the reader reads its records a
 * field at a time and windows go on from the record boundary after them.
 * A generated table whose first record is longer than a window has its
 * first window declined, unframed, and the rest staged in two windows or
 * more on every way that batches it.  Read with a header, that record is
 * the header, which the reader reads before windows go on without one.
 * Every way ends as the import a field at a time does.
 */
static void
windows_go_on_after_a_declined_window(void)
{
	static const unsigned int header_flags[] = { 0, RELIC_IMPORT_HEADER };
	import_outcome_t batched;
	import_outcome_t scalar;
	batch_case_t batch;
	uint64_t declined;
	uint64_t staged;
	size_t f;
	size_t length;
	size_t w;
	char *long_record;
	char *table;
	const batch_way_t *way;

	long_record = record_longer_than_a_window();
	table = long_record != NULL ? generated_table(GENERATED_ROWS, 0,
	    long_record, &length) : NULL;
	free(long_record);
	if (table == NULL) {
		check(false, SCENARIO_RESUME, "the table is built");
		return;
	}

	bzero(&batch, sizeof(batch));
	batch.program = GENERATED_PROGRAM;
	batch.table = table;
	batch.length = length;
	batch.format = RELIC_FORMAT_TSV;
	batch.columns = generated_columns;
	batch.column_count = 3;
	for (f = 0; f < sizeof(header_flags) / sizeof(header_flags[0]); f++) {
		batch.flags = header_flags[f];
		batch.label = batch.flags != 0 ?
		    "a header longer than a window" :
		    "a first record longer than a window";
		for (w = 0; w < sizeof(batch_ways) / sizeof(batch_ways[0]);
		    w++) {
			way = &batch_ways[w];
			if (import_first(&batch, table, length,
			    RELIC_UNSTABLE_NO_BATCH_IMPORT, 1, &scalar) != 0) {
				check(false, SCENARIO_RESUME,
				    "a context is made and loaded");
				relic_destroy(scalar.ctx);
				break;
			}
			if (import_first(&batch, table, length, way->flags,
			    way->threads, &batched) != 0) {
				check(false, SCENARIO_RESUME,
				    "a context is made and loaded");
				relic_destroy(scalar.ctx);
				relic_destroy(batched.ctx);
				break;
			}

			staged = batched.ctx->counters.windows_staged;
			declined = batched.ctx->counters.windows_declined;
			checkf((way->flags & BATCHED) == 0 ||
			    (batched.status == 0 && declined == 1 &&
			    staged >= 2), SCENARIO_RESUME, "%s, %s: one window "
			    "should be declined and two or more staged after "
			    "it, not %" PRIu64 " declined and %" PRIu64
			    " staged", batch.label, way->name, declined,
			    staged);
			check_same_outcome(&batched, &scalar, batch.label, way,
			    "one import");

			relic_destroy(scalar.ctx);
			relic_destroy(batched.ctx);
		}
	}

	free(table);
}

/*
 * A read that fails after a batch import has read ahead is reported where
 * an import a field at a time would meet it: after the records read before
 * it are staged and their values interned, and never ahead of an earlier
 * refusal.  The bytes the failing read returned are not read.  Each table
 * is read both ways, failing at the same byte, and must end alike.  A
 * failing stream has no descriptor, so it is read a field at a time,
 * however the context batches, unless the build takes it for a regular
 * file, as the small-window build does (Makefile.am).
 */
static void
a_failed_read_ahead_waits_its_turn(void)
{
	size_t length;
	char *table;

	table = generated_table(GENERATED_ROWS, 0, NULL, &length);
	if (table == NULL) {
		check(false, SCENARIO_BATCH, "the table is built");
		return;
	}
	compare_failed_reads("a read failing at once", table, 0);
	compare_failed_reads("a read failing in the first record", table, 3);
	compare_failed_reads("a read failing after a chunk", table,
	    RELIC_IMPORT_READ_CHUNK + 5);
	compare_failed_reads("a read failing after a window", table,
	    RELIC_IMPORT_WINDOW_BYTES + RELIC_IMPORT_READ_CHUNK + 5);
	compare_failed_reads("a read failing at the end", table, length);
	free(table);

	table = generated_table(GENERATED_ROWS, GENERATED_ROWS / 2,
	    "s1\tx\ts2\n", &length);
	if (table == NULL) {
		check(false, SCENARIO_BATCH, "the table is built");
		return;
	}
	compare_failed_reads("a read failing after a type error", table,
	    row_offset(table, GENERATED_ROWS / 2 + 1) + 7);
	free(table);
}

/*
 * Import `table` from a stream failing at byte `fail_at`, each of the
 * batch_ways and a field at a time, and check they end alike.
 */
static void
compare_failed_reads(const char *label, const char *table, size_t fail_at)
{
	import_outcome_t batched;
	import_outcome_t scalar;
	const batch_way_t *way;
	size_t w;
	int made;

	made = import_failing(table, fail_at, RELIC_UNSTABLE_NO_BATCH_IMPORT, 1,
	    &scalar);
	if (made > 0) {
		printf("skip: %s: no stream with a read function of the "
		    "caller's own\n", SCENARIO_BATCH);
		return;
	}
	if (made < 0) {
		check(false, SCENARIO_BATCH, "a context and a stream are made");
		return;
	}
	checkf(scalar.status == -1, SCENARIO_BATCH, "%s: the import should "
	    "fail", label);

	for (w = 0; w < sizeof(batch_ways) / sizeof(batch_ways[0]); w++) {
		way = &batch_ways[w];
		if (import_failing(table, fail_at, way->flags, way->threads,
		    &batched) != 0) {
			check(false, SCENARIO_BATCH,
			    "a context and a stream are made");
			break;
		}
		checkf(batched.status == scalar.status, SCENARIO_BATCH,
		    "%s, %s: a batch import should end %d, not %d", label,
		    way->name, scalar.status, batched.status);
		check_same_outcome(&batched, &scalar, label, way,
		    "a failed read");
#ifdef RELIC_IMPORT_REGULAR_FILE
		checkf((way->flags & BATCHED) == 0 ||
		    fail_at <= RELIC_IMPORT_WINDOW_BYTES ||
		    batched.ctx->counters.windows_staged > 0, SCENARIO_BATCH,
		    "%s, %s: a stream taken for a regular file should be "
		    "batched", label, way->name);
#else
		checkf(batched.ctx->counters.windows_staged == 0 &&
		    batched.ctx->counters.windows_declined == 0,
		    SCENARIO_BATCH, "%s, %s: a stream without a descriptor "
		    "should be read a field at a time, not stage %zu windows",
		    label, way->name,
		    (size_t)batched.ctx->counters.windows_staged);
#endif
		relic_destroy(batched.ctx);
	}

	relic_destroy(scalar.ctx);
}

/*
 * Import `table` as the generated tables are imported, into a new context
 * of `flags` and `threads`, from a stream that fails at byte `fail_at`.
 * Returns 0, -1 when the context or the stream could not be made, or 1
 * when no stream with a read function of the caller's own can be.
 */
static int
import_failing(const char *table, size_t fail_at, unsigned int flags,
    size_t threads, import_outcome_t *outcome)
{
	relic_config_t config;
	reentering_read_t reading;
	relic_import_t import;
	FILE *stream;

	bzero(outcome, sizeof(*outcome));
	bzero(&config, sizeof(config));
	config.flags = flags;
	config.threads = threads;
	outcome->ctx = context_with_capture(&config, &outcome->capture);
	if (outcome->ctx == NULL)
		return (-1);

	bzero(&reading, sizeof(reading));
	reading.ctx = outcome->ctx;
	reading.table = table;
	reading.fail_at = fail_at;
	reading.fail_errno = EIO;
	stream = reentering_stream(&reading);
	if (stream == NULL) {
		relic_destroy(outcome->ctx);
		outcome->ctx = NULL;
		return (1);
	}

	bzero(&import, sizeof(import));
	import.predicate = "e";
	import.columns = generated_columns;
	import.column_count = 3;
	import.format = RELIC_FORMAT_TSV;
#ifdef RELIC_IMPORT_REGULAR_FILE
	regular_stand_in = stream;
#endif
	outcome->status = relic_import_file(outcome->ctx, "table", stream,
	    &import, sizeof(import));
#ifdef RELIC_IMPORT_REGULAR_FILE
	regular_stand_in = NULL;
#endif
	fclose(stream);

	return (0);
}

#ifdef RELIC_IMPORT_REGULAR_FILE
/* Whether lib/import.c takes `stream` for a regular file. */
bool
relic_test_regular_file(FILE *stream)
{
	return (stream != NULL && stream == regular_stand_in);
}
#endif

/*
 * A table read from a pipe, or from a FIFO it is named by, is read a field
 * at a time however the context batches, so a producer that sends one
 * transport chunk holding a malformed record and then waits, its end still
 * open, has the mistake reported without sending more: without a window's
 * worth of the table, or its end.  The import ends as one a field at a
 * time of the same bytes from a file does, with the same diagnostics, the
 * same values interned and no fact added, and stages no window.  An
 * import that waits for more is ended by SIGALRM after PRODUCER_SECONDS,
 * which fails the suite.
 */
static void
a_waiting_producer_hears_its_mistake(void)
{
	import_outcome_t produced;
	import_outcome_t scalar;
	char directory[1024];
	char fifo[1024 + sizeof("/table")];
	const producer_channel_t *channel;
	const batch_way_t *way;
	const char *path;
	char *chunk;
	size_t c;
	size_t w;
	bool sent;

	signal(SIGALRM, SIG_DFL);
	chunk = malformed_chunk();
	if (chunk == NULL || !make_fifo_directory(directory,
	    sizeof(directory))) {
		check(false, SCENARIO_WAITING, "a chunk and a directory are "
		    "made");
		free(chunk);
		return;
	}
	snprintf(fifo, sizeof(fifo), "%s/table", directory);

	for (c = 0; c < sizeof(producer_channels) /
	    sizeof(producer_channels[0]); c++) {
		channel = &producer_channels[c];
		path = channel->by_path ? fifo : NULL;
		if (import_chunk_from_file(chunk, path != NULL ? path : "table",
		    &scalar) != 0) {
			check(false, SCENARIO_WAITING, "a context is made");
			break;
		}
		checkf(scalar.status == -1 && scalar.capture.errors == 1,
		    SCENARIO_WAITING, "the chunk's malformed record should "
		    "fail an import from a file with one error, not %zu",
		    scalar.capture.errors);

		for (w = 0; w < sizeof(batch_ways) / sizeof(batch_ways[0]);
		    w++) {
			way = &batch_ways[w];
			if (import_from_producer(chunk, path, way, &produced,
			    &sent) != 0) {
				check(false, SCENARIO_WAITING, "a context, a "
				    "producer and its channel are made");
				break;
			}
			checkf(sent, SCENARIO_WAITING, "%s, %s: the producer "
			    "should send its chunk and wait until the import "
			    "returns", channel->label, way->name);
			checkf(produced.status == scalar.status &&
			    produced.ctx->counters.windows_staged == 0 &&
			    produced.ctx->counters.windows_declined == 0,
			    SCENARIO_WAITING, "%s, %s: the import should end "
			    "%d staging no window, not %d staging %zu",
			    channel->label, way->name, scalar.status,
			    produced.status,
			    (size_t)produced.ctx->counters.windows_staged);
			check_same_outcome(&produced, &scalar, channel->label,
			    way, "an import from a producer that waits");
			relic_destroy(produced.ctx);
		}
		relic_destroy(scalar.ctx);
	}

	rmdir(directory);
	free(chunk);
}

/*
 * Make a directory of the user's own for the FIFO, under $TMPDIR or /tmp,
 * its path in `directory`.  Returns whether it was made.
 */
static bool
make_fifo_directory(char *directory, size_t size)
{
	const char *base;
	int length;

	base = getenv("TMPDIR");
	if (base == NULL || base[0] == '\0')
		base = "/tmp";
	length = snprintf(directory, size, "%s/relic-fifo-XXXXXX", base);
	if (length < 0 || (size_t)length >= size)
		return (false);

	return (mkdtemp(directory) != NULL);
}

/*
 * One transport chunk of a two-column table, a symbol and an integer to a
 * record, whose middle record's integer is malformed.  Returns the chunk,
 * RELIC_IMPORT_READ_CHUNK bytes the caller frees, or NULL.
 */
static char *
malformed_chunk(void)
{
	char *chunk;
	size_t record;
	size_t i;

	_Static_assert(RELIC_IMPORT_READ_CHUNK % 8 == 0,
	    "each half of a chunk holds whole four-byte records");
	chunk = malloc(RELIC_IMPORT_READ_CHUNK);
	if (chunk == NULL)
		return (NULL);
	for (i = 0; i < RELIC_IMPORT_READ_CHUNK; i += 4) {
		record = i / 4;
		chunk[i] = (char)('a' + record % 26);
		chunk[i + 1] = '\t';
		chunk[i + 2] = i == RELIC_IMPORT_READ_CHUNK / 2 ? 'x' :
		    (char)('0' + record % 10);
		chunk[i + 3] = '\n';
	}

	return (chunk);
}

/*
 * Import the chunk from a file, a field at a time, into a new context,
 * naming it `name`.  Returns 0, or -1 when the context could not be made.
 */
static int
import_chunk_from_file(const char *chunk, const char *name,
    import_outcome_t *outcome)
{
	relic_config_t config;

	bzero(outcome, sizeof(*outcome));
	bzero(&config, sizeof(config));
	config.flags = RELIC_UNSTABLE_NO_BATCH_IMPORT;
	config.threads = 1;
	outcome->ctx = context_with_capture(&config, &outcome->capture);
	if (outcome->ctx == NULL)
		return (-1);
	outcome->status = import_table(outcome->ctx, name, RELIC_FORMAT_TSV, 0,
	    "e", waiting_columns, 2, chunk, RELIC_IMPORT_READ_CHUNK);

	return (0);
}

/*
 * Import the chunk into a new context configured `way` as a producer sends
 * it: through the FIFO at `fifo`, or through a pipe when `fifo` is NULL.
 * The producer waits after the chunk, its end open, until the import has
 * returned; *sentp says whether it sent the chunk and then exited of its
 * own accord.  Returns 0, or -1 when the context, the channel or the
 * producer could not be made.
 */
static int
import_from_producer(const char *chunk, const char *fifo,
    const batch_way_t *way, import_outcome_t *outcome, bool *sentp)
{
	relic_config_t config;
	producer_t producer;
	int imported;

	bzero(outcome, sizeof(*outcome));
	bzero(&config, sizeof(config));
	config.flags = way->flags;
	config.threads = way->threads;
	outcome->ctx = context_with_capture(&config, &outcome->capture);
	if (outcome->ctx == NULL)
		return (-1);
	if (start_producer(&producer, chunk, fifo) != 0) {
		relic_destroy(outcome->ctx);
		outcome->ctx = NULL;
		return (-1);
	}

	alarm(PRODUCER_SECONDS);
	imported = import_from_channel(outcome->ctx, &producer, fifo,
	    &outcome->status);
	alarm(0);

	*sentp = stop_producer(&producer, fifo);
	if (imported != 0) {
		relic_destroy(outcome->ctx);
		outcome->ctx = NULL;
		return (-1);
	}

	return (0);
}

/*
 * Make the channel, a FIFO at `fifo` or a pipe when it is NULL, and fork
 * the producer that sends the chunk through it.  Returns 0, or -1 having
 * made nothing that lasts.
 */
static int
start_producer(producer_t *producer, const char *chunk, const char *fifo)
{
	int control[2];
	int data[2];
	int descriptor;

	data[0] = -1;
	data[1] = -1;
	if (fifo != NULL) {
		if (mkfifo(fifo, 0600) != 0)
			return (-1);
	} else if (pipe(data) != 0)
		return (-1);
	if (pipe(control) != 0) {
		close_channel(data, fifo);
		return (-1);
	}

	producer->pid = fork();
	if (producer->pid == 0) {
		close(control[1]);
		if (fifo != NULL) {
			alarm(PRODUCER_SECONDS);
			descriptor = open(fifo, O_WRONLY);
			alarm(0);
		} else {
			close(data[0]);
			descriptor = data[1];
		}
		produce_and_wait(chunk, descriptor, control[0]);
	}

	close(control[0]);
	if (producer->pid < 0) {
		close(control[1]);
		close_channel(data, fifo);
		return (-1);
	}
	if (data[1] >= 0)
		close(data[1]);
	producer->data = data[0];
	producer->control = control[1];

	return (0);
}

/* Undo a channel start_producer() made: unlink the FIFO, or close the pipe. */
static void
close_channel(const int data[2], const char *fifo)
{
	if (fifo != NULL) {
		unlink(fifo);
		return;
	}
	close(data[0]);
	close(data[1]);
}

/*
 * The producer: send the chunk through `descriptor`, then wait, the
 * channel open, until `control` ends.  Ends the process, with status 0
 * when the whole chunk was sent.
 */
static void
produce_and_wait(const char *chunk, int descriptor, int control)
{
	size_t written;
	ssize_t wrote;
	char byte;

	if (descriptor < 0)
		_exit(1);
	for (written = 0; written < RELIC_IMPORT_READ_CHUNK;
	    written += (size_t)wrote) {
		wrote = write(descriptor, chunk + written,
		    RELIC_IMPORT_READ_CHUNK - written);
		if (wrote < 0 && errno == EINTR)
			wrote = 0;
		else if (wrote <= 0)
			_exit(1);
	}
	while (read(control, &byte, 1) < 0 && errno == EINTR)
		continue;
	_exit(0);
}

/*
 * Import from the producer's channel into `ctx`, setting *statusp to what
 * the import returned.  Returns 0, or -1 when no stream could be made of
 * the pipe.
 */
static int
import_from_channel(relic_t *ctx, producer_t *producer, const char *fifo,
    int *statusp)
{
	relic_import_t import;
	FILE *stream;

	bzero(&import, sizeof(import));
	import.predicate = "e";
	import.columns = waiting_columns;
	import.column_count = 2;
	import.format = RELIC_FORMAT_TSV;

	if (fifo != NULL) {
		*statusp = relic_import_path(ctx, fifo, &import, sizeof(import));
		return (0);
	}

	stream = fdopen(producer->data, "r");
	if (stream == NULL)
		return (-1);
	producer->data = -1;
	*statusp = relic_import_file(ctx, "table", stream, &import,
	    sizeof(import));
	fclose(stream);

	return (0);
}

/*
 * Release the producer from its wait and reap it, closing what the import
 * left of the channel.  Returns whether it exited with status 0.
 */
static bool
stop_producer(producer_t *producer, const char *fifo)
{
	int status;

	close(producer->control);
	if (producer->data >= 0)
		close(producer->data);
	status = -1;
	while (waitpid(producer->pid, &status, 0) < 0 && errno == EINTR)
		continue;
	if (fifo != NULL)
		unlink(fifo);

	return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/*
 * Frame the window a case's table begins with, of which `buffered` bytes
 * are in memory, `ended` saying the stream ended after them, in steps of
 * at most `most` bytes, into *framingp.  A window cut into more pieces
 * than a framing holds is recorded as unframed.
 */
static void
frame_case(const framing_case_t *test, size_t buffered, bool ended,
    size_t most, framing_t *framingp)
{
	relic_import_plan_t plan;
	relic_location_t start;
	relic_framer_t framer;
	size_t most_records;

	bzero(&plan, sizeof(plan));
	plan.format = test->tsv ? RELIC_FORMAT_TSV : RELIC_FORMAT_CSV;
	plan.column_count = 2;
	start.file = "t";
	start.line = 1;
	start.column = 1;
	most_records = test->most_records != 0 ? test->most_records :
	    FRAMING_HORIZON;

	relic_framer_init(&framer, FRAMING_HORIZON, most_records,
	    FRAMING_PIECE);
	relic_framer_begin(&framer, &plan, "t", &start, test->header,
	    buffered, ended);
	while (relic_framer_step(&framer, test->table, most))
		continue;

	bzero(framingp, sizeof(*framingp));
	framingp->state = framer.state;
	if (framer.state == RELIC_FRAMED &&
	    framer.piece_count > FRAMING_PIECES_MAX)
		framingp->state = RELIC_UNFRAMED;
	if (framingp->state == RELIC_FRAMED) {
		bcopy(framer.pieces, framingp->pieces,
		    framer.piece_count * sizeof(framer.pieces[0]));
		framingp->end = framer.end;
		framingp->length = framer.length;
		framingp->piece_count = framer.piece_count;
		framingp->tuple_count = framer.tuple_count;
		framingp->ends_input = framer.ends_input;
	}
	relic_framer_reset(&framer);
}

/* Whether two framings frame the same window into the same pieces. */
static bool
same_framing(const framing_t *a, const framing_t *b)
{
	const relic_window_piece_t *p;
	const relic_window_piece_t *q;
	size_t i;

	if (a->state != b->state)
		return (false);
	if (a->state != RELIC_FRAMED)
		return (true);
	if (a->length != b->length || a->piece_count != b->piece_count ||
	    a->tuple_count != b->tuple_count ||
	    a->ends_input != b->ends_input || a->end.line != b->end.line ||
	    a->end.column != b->end.column)
		return (false);
	for (i = 0; i < a->piece_count; i++) {
		p = &a->pieces[i];
		q = &b->pieces[i];
		if (p->start != q->start || p->length != q->length ||
		    p->first_tuple != q->first_tuple ||
		    p->tuples != q->tuples || p->header != q->header ||
		    p->where.line != q->where.line ||
		    p->where.column != q->where.column)
			return (false);
	}

	return (true);
}

/*
 * Check that a case's window, `buffered` bytes of it held and its end
 * found when `ended` is set, frames as `reference` in steps of every size
 * from one byte to past the horizon.
 */
static void
frame_every_step(const framing_case_t *test, size_t buffered, bool ended,
    const framing_t *reference)
{
	framing_t framing;
	size_t most;

	for (most = 1; most <= FRAMING_HORIZON + 1; most++) {
		frame_case(test, buffered, ended, most, &framing);
		checkf(same_framing(&framing, reference), SCENARIO_FRAMING,
		    "%s: %zu bytes held%s, framed %zu at a time, should "
		    "frame the same window", test->label, buffered,
		    ended ? " and the end found" : "", most);
	}
}

/*
 * A window is the whole records before its horizon, cut into pieces by
 * their bytes, however the framing is stepped and however far past the
 * horizon the stream has been read.  The reference framing has the whole
 * table in memory, its end found, and frames it in one step; it must
 * frame the window each case expects, and so must every step size from
 * one byte to past the horizon, with as little of the stream in memory as
 * the importer ever holds, its horizon, and with every amount more.  An
 * end found at or after the horizon decides nothing: a record no line
 * feed ends lying up to the horizon is left to the next window whether
 * the end was found there or not, and a quoted line feed or a carriage
 * return before the horizon ends no record there.
 */
static void
a_window_is_framed_alike_however_stepped(void)
{
	static const framing_case_t cases[] = {
		{ .label = "a quoted line feed across the horizon",
		    FRAMING_TABLE("a,b\na,b\na,b\na,b\na,b\na,b\n"
		    "c,\"x\nyyyyyy\"\nd,e\n"),
		    .tuples = 6, .pieces = 3, .length = 24 },
		{ .label = "a quoted line feed across the horizon, "
		    "after a header",
		    FRAMING_TABLE("h,k\na,b\na,b\na,b\na,b\na,b\n"
		    "c,\"x\nyyyyyy\"\nd,e\n"),
		    .header = true, .tuples = 5, .pieces = 3, .length = 24 },
		{ .label = "a closing quote before the horizon, its line feed "
		    "at it", FRAMING_TABLE("a,b\na,b\na,b\na,b\na,b\na,b\n"
		    "c,\"xyzw\"\nd,e\n"),
		    .tuples = 6, .pieces = 3, .length = 24 },
		{ .label = "a carriage return before the horizon, its line "
		    "feed at it", FRAMING_TABLE("a,b\na,b\na,b\na,b\na,b\na,b\n"
		    "c,ddddd\r\nd,e\n"),
		    .tuples = 6, .pieces = 3, .length = 24 },
		{ .label = "a tsv escape split by the horizon",
		    FRAMING_TABLE("a\tb\na\tb\na\tb\na\tb\na\tb\na\tb\n"
		    "a\tb\nc\tx\\n\nd\te\n"), .tsv = true,
		    .tuples = 7, .pieces = 4, .length = 28 },
		{ .label = "a last record without a line feed up to the "
		    "horizon", FRAMING_TABLE("a,b\na,b\na,b\na,b\na,b\na,b\n"
		    "c,dddddd"),
		    .tuples = 6, .pieces = 3, .length = 24 },
		{ .label = "a last line feed at the horizon",
		    FRAMING_TABLE("a,b\na,b\na,b\na,b\na,b\na,b\na,b\nc,d\n"),
		    .tuples = 8, .pieces = 4, .length = 32 },
		{ .label = "a last record without a line feed before the "
		    "horizon", FRAMING_TABLE("a,b\nc,d"),
		    .tuples = 2, .pieces = 1, .length = 7, .ends_input = true },
		{ .label = "a quoted last field before the horizon",
		    FRAMING_TABLE("a,b\nc,\"d\ne\""),
		    .tuples = 2, .pieces = 1, .length = 11,
		    .ends_input = true },
		{ .label = "a record longer than the horizon",
		    FRAMING_TABLE("a,xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"
		    "b,c\n"), .unframed = true },
		{ .label = "an unterminated quote before the horizon",
		    FRAMING_TABLE("a,b\nc,\"d"), .unframed = true },
		{ .label = "the record cap before the horizon",
		    FRAMING_TABLE("a,b\na,b\na,b\na,b\na,b\na,b\nc,d\n"),
		    .most_records = 3, .tuples = 3, .pieces = 2,
		    .length = 12 },
		{ .label = "the record cap counting a header",
		    FRAMING_TABLE("h,k\na,b\na,b\na,b\na,b\na,b\nc,d\n"),
		    .header = true, .most_records = 3, .tuples = 2,
		    .pieces = 2, .length = 12 },
		{ .label = "the record cap at a header",
		    FRAMING_TABLE("h,k\na,b\n"), .header = true,
		    .most_records = 1, .tuples = 0, .pieces = 1,
		    .length = 4 },
		{ .label = "the record cap a record before the end",
		    FRAMING_TABLE("a,b\nc,d\ne,f\n"),
		    .most_records = 2, .tuples = 2, .pieces = 1,
		    .length = 8 },
		{ .label = "the record cap at a last line feed",
		    FRAMING_TABLE("a,b\nc,d\n"),
		    .most_records = 2, .tuples = 2, .pieces = 1,
		    .length = 8, .ends_input = true },
		{ .label = "the record cap at a last record without a line "
		    "feed", FRAMING_TABLE("a,b\nc,d"),
		    .most_records = 2, .tuples = 2, .pieces = 1,
		    .length = 7, .ends_input = true },
		{ .label = "the record cap at a quoted last field",
		    FRAMING_TABLE("a,b\nc,\"d\ne\""),
		    .most_records = 2, .tuples = 2, .pieces = 1,
		    .length = 11, .ends_input = true },
		{ .label = "the record cap and a last line feed at the horizon",
		    FRAMING_TABLE("a,b\na,b\na,b\na,b\na,b\na,b\na,b\nc,d\n"),
		    .most_records = 8, .tuples = 8, .pieces = 4,
		    .length = 32 },
		{ .label = "the record cap before an unterminated quote",
		    FRAMING_TABLE("a,b\nc,d\ne,\"f"),
		    .most_records = 2, .tuples = 2, .pieces = 1,
		    .length = 8 },
	};
	static const framing_case_t cut_short = {
		.label = "a read failed short of the horizon",
		FRAMING_TABLE("a,b\nc,d"),
	};
	static const framing_case_t capped_short = {
		.label = "the record cap where a failed read cut the input",
		FRAMING_TABLE("a,b\nc,d\n"), .most_records = 2,
	};
	const framing_case_t *test;
	framing_t short_of_horizon;
	framing_t reference;
	size_t buffered;
	size_t first;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		test = &cases[i];
		frame_case(test, test->table_length, true, SIZE_MAX,
		    &reference);
		if (test->unframed) {
			checkf(reference.state == RELIC_UNFRAMED,
			    SCENARIO_FRAMING, "%s: no window should be "
			    "framed", test->label);
		} else {
			checkf(reference.state == RELIC_FRAMED &&
			    reference.tuple_count == test->tuples &&
			    reference.piece_count == test->pieces &&
			    reference.length == test->length &&
			    reference.ends_input == test->ends_input,
			    SCENARIO_FRAMING, "%s: the window should hold %zu "
			    "tuples in %zu pieces and %zu bytes, %s the "
			    "table, not %zu in %zu and %zu, %s", test->label,
			    test->tuples, test->pieces, test->length,
			    test->ends_input ? "ending" : "not ending",
			    reference.tuple_count, reference.piece_count,
			    reference.length,
			    reference.ends_input ? "ending" : "not ending");
		}

		/*
		 * The importer holds a window's horizon, or the whole table
		 * and its end; a table held whole may yet go on only where
		 * that cannot change the window.
		 */
		first = test->table_length < FRAMING_HORIZON ?
		    test->table_length : FRAMING_HORIZON;
		for (buffered = first; buffered <= test->table_length;
		    buffered++) {
			if (buffered < test->table_length ||
			    test->table_length >= FRAMING_HORIZON)
				frame_every_step(test, buffered, false,
				    &reference);
			if (buffered == test->table_length)
				frame_every_step(test, buffered, true,
				    &reference);
		}
	}

	/*
	 * A read that failed short of the horizon leaves the window its
	 * whole records, the last no end of input completes.
	 */
	frame_case(&cut_short, cut_short.table_length, true, SIZE_MAX,
	    &reference);
	frame_case(&cut_short, cut_short.table_length, false, SIZE_MAX,
	    &short_of_horizon);
	checkf(reference.state == RELIC_FRAMED && reference.tuple_count == 2 &&
	    short_of_horizon.state == RELIC_FRAMED &&
	    short_of_horizon.tuple_count == 1 &&
	    short_of_horizon.length == 4 && !short_of_horizon.ends_input,
	    SCENARIO_FRAMING, "%s: held short of the horizon without its end, "
	    "the window should hold 1 tuple in 4 bytes, not %zu in %zu",
	    cut_short.label, short_of_horizon.tuple_count,
	    short_of_horizon.length);
	frame_every_step(&cut_short, cut_short.table_length, false,
	    &short_of_horizon);

	/*
	 * A window the cap ends where the bytes held end is the table's last
	 * only when the stream's end was found after them; after a failed
	 * read it is not, and the failure is left for the reader to report.
	 */
	frame_case(&capped_short, capped_short.table_length, false, SIZE_MAX,
	    &short_of_horizon);
	checkf(short_of_horizon.state == RELIC_FRAMED &&
	    short_of_horizon.tuple_count == 2 &&
	    short_of_horizon.length == 8 && !short_of_horizon.ends_input,
	    SCENARIO_FRAMING, "%s: the window should hold 2 tuples in 8 bytes "
	    "and not end the table, not %zu in %zu, %s", capped_short.label,
	    short_of_horizon.tuple_count, short_of_horizon.length,
	    short_of_horizon.ends_input ? "ending it" : "not ending it");
	frame_every_step(&capped_short, capped_short.table_length, false,
	    &short_of_horizon);
}

int
main(void)
{
	tsv_is_read_by_its_rules();
	csv_is_read_by_its_rules();
	locations_stop_at_their_largest_value();
	fields_are_held_to_their_bound();
	symbol_columns_hold_the_language_symbols();
	integer_columns_hold_the_language_integers();
	string_columns_hold_any_bytes();
	skipped_columns_intern_nothing();
	messages_quote_only_what_is_safe();
	mistakes_name_their_source();
	the_input_limit_bounds_a_field();
	a_header_is_skipped();
	every_refusal_reads_nothing();
	a_failed_import_adds_nothing();
	a_refused_import_stops_reading();
	a_long_field_stops_reading();
	batching_follows_the_threads_and_the_table();
	facts_are_held_once();
	an_import_discards_derived_facts();
	a_long_field_crosses_reads();
	a_stream_cannot_call_in();
	a_description_is_read_once();
	a_source_name_is_read_once();
	a_failed_read_adds_nothing();
	a_path_is_opened_and_named();
	a_batch_import_matches_the_scalar_import();
	a_window_holds_at_most_its_records();
	windows_go_on_after_a_declined_window();
	a_window_is_framed_alike_however_stepped();
	a_failed_read_ahead_waits_its_turn();
	a_waiting_producer_hears_its_mistake();

	return (check_summary("import"));
}
