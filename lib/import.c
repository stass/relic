/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"
#include "window.h"

/*
 * Importing facts from csv and tsv tables.
 *
 * An import is a load whose text is a table.  Its facts are staged as the
 * table is read and committed when the whole table has been, through the
 * commit a load uses, so an import is all or nothing and an imported fact is
 * exactly the fact the equivalent text would state.
 *
 * Reading has two halves, which meet in read_table().  The reader, the first
 * half of this file, is the state machine that decides where fields and
 * records end and what each field holds once its format is decoded; it
 * knows nothing of values or of a context.  The importer, the second half,
 * consumes the reader's events: it types each field by its column as the
 * field ends, interning it or dropping it, and stages the record's tuple
 * when the record ends.  Events are consumed in the order the reader emits
 * them, so a field its column refuses is reported before anything the
 * reader would find after it, and the mistake reported is always the first
 * the table holds.
 *
 * The first mistake ends an import.  A table is usually written by a
 * program, and a program's mistake -- a wrong delimiter, columns declared in
 * the wrong order -- breaks every record alike, so a message per record
 * would say the same thing a million times.
 */

/*
 * How much of a field a message may quote.  A message names one field, and
 * the reader needs enough of it to find it, not all of it.
 */
#define	IMPORT_QUOTED_FIELD_MAX	40

/*
 * Room for the longest description of a field: one quoted in full and cut,
 * with its quotes, the "..." after them and a terminator.  Naming an empty
 * field, or one by its byte, takes less.
 */
#define	IMPORT_FIELD_TEXT_MAX	(IMPORT_QUOTED_FIELD_MAX + sizeof("\"\"..."))

/*
 * How the reading of a table goes on after an event: read on, or stop
 * because the table has ended with every record staged, or because the
 * import has failed and why has been reported.
 */
enum import_progress {
	IMPORT_READ_ON,
	IMPORT_TABLE_ENDED,
	IMPORT_FAILED
};

/*
 * How staging a window went: staged, with more of the table to come, or
 * the table's last; declined, with nothing of it staged; failed, having
 * reported why; or not begun, since the input holds nothing more and the
 * reader is to say what its end means.
 */
enum window_outcome {
	WINDOW_STAGED,
	WINDOW_ENDED,
	WINDOW_DECLINED,
	WINDOW_FAILED,
	WINDOW_NONE
};

typedef struct import_framing import_framing_t;
typedef struct import_input import_input_t;
typedef struct importer importer_t;
typedef struct window_fill window_fill_t;

/*
 * What an import has read of its stream and not yet staged: the one
 * buffer through which both ways of reading a table read it, `length`
 * bytes in `capacity`.  `failed` says the read after them failed, with
 * `read_errno`, and `ended` that the stream ended after them.  Neither is
 * acted on before every byte held has been, so a window read ahead and
 * then declined is read again field by field exactly as it was read.
 */
struct import_input {
	FILE			*stream;
	char			*bytes;
	size_t			 length;
	size_t			 capacity;
	int			 read_errno;
	bool			 failed;
	bool			 ended;
};

/*
 * A window's framing: `framer`, over the window that begins `start` bytes
 * into `input`, and `frames_ahead`, how many steps it has run as the
 * filler of a window's jobs since the import last counted them.  A filler
 * steps it while the parts run, so the alignment of its first member
 * aligns and pads it to RELIC_CACHE_LINE (round.h), which keeps it off
 * every line a part reads.  That holds for this state only: the pieces the
 * framer fills and the stack its steps run on are left to the allocator,
 * where a step writes far less often.
 */
struct import_framing {
	_Alignas(RELIC_CACHE_LINE) relic_framer_t framer;
	const import_input_t	*input;
	size_t			 start;
	uint64_t		 frames_ahead;
};

/*
 * An import in progress.  `tuple` holds the values of the current record's
 * kept columns, `kept` of them so far.  `header_pending` is set while the
 * first record is still to be read as a header: its fields are neither typed
 * nor interned, and its end stages nothing.  `boundary` is the location of
 * the first byte the input holds, which begins a record: where the next
 * window, or the reader, begins.  `map` is what the value layer numbered
 * the window's values as, opened while `mapped` is set.
 *
 * `framing` frames the window after the one being staged while that one
 * is analyzed, or the window `boundary` begins once that one is staged.
 * It is over-aligned and points into the importer, so an importer is an
 * automatic object, neither allocated by malloc() nor copied.
 */
struct importer {
	relic_t			*ctx;
	const relic_import_plan_t *plan;
	relic_staging_t		 staging;
	relic_table_reader_t	 reader;
	import_input_t		 input;
	relic_window_t		 window;
	relic_pending_map_t	 map;
	relic_location_t	 boundary;
	relic_dom_t		 tuple[RELIC_MAX_ARITY];
	relic_pred_id_t		 pred;
	char			*source_name;	/* the importer's copy */
	uint16_t		 kept;
	bool			 header_pending;
	bool			 mapped;
	import_framing_t	 framing;
};

/*
 * What the parts filling a window's new tuples into the predicate's
 * builder read: where each stripe's tuples go, and the window's tuples as
 * a batch.
 */
struct window_fill {
	const relic_stripe_fill_t	*fills;
	const relic_builder_batch_t	*batch;
};

static void	 advance_location(relic_location_t *where, unsigned char byte);
static int	 analyze_window(importer_t *importer,
		    const relic_pool_filler_t *ahead, size_t *value_bytesp);
static enum relic_table_event_kind append_field_byte(
		    relic_table_reader_t *reader, unsigned char byte,
		    relic_table_event_t *event);
static bool	 may_batch(const relic_t *ctx);
static bool	 reads_a_regular_file(FILE *stream);
#ifdef RELIC_IMPORT_REGULAR_FILE
bool		 RELIC_IMPORT_REGULAR_FILE(FILE *stream);
#endif
static bool	 batching_forced(const relic_t *ctx);
static bool	 outgrows_window(const import_input_t *input);
static int	 prepare_batching(relic_t *ctx);
static void	 consume_input(import_input_t *input, size_t length);
static const char *describe_field(const char *bytes, size_t length,
		    char *buffer, size_t size);
static enum relic_table_event_kind end_field(relic_table_reader_t *reader,
		    enum relic_table_state next, relic_table_event_t *event);
static enum relic_table_event_kind end_of_input(relic_table_reader_t *reader,
		    relic_table_event_t *event);
static enum relic_table_event_kind end_record(relic_table_reader_t *reader,
		    relic_table_event_t *event);
static bool	 extra_field_begun(const relic_table_reader_t *reader);
static enum import_progress feed_reader(importer_t *importer);
static int	 fill_input(import_input_t *input, size_t target);
static bool	 field_value(relic_t *ctx, enum relic_column column,
		    const relic_table_event_t *event, relic_dom_t *valuep);
static int	 import_named_file(relic_t *ctx, const char *path,
		    const relic_import_plan_t *plan);
static bool	 import_plan_init(relic_t *ctx, const relic_import_t *import,
		    relic_import_plan_t *plan);
static void	 import_plan_reset(relic_import_plan_t *plan);
static int	 import_stream(relic_t *ctx, const char *source_name,
		    FILE *stream, const relic_import_plan_t *plan);
static void	 importer_init(importer_t *importer, relic_t *ctx,
		    const relic_import_plan_t *plan, FILE *stream);
static void	 importer_reset(importer_t *importer);
static bool	 is_delimiter(const relic_table_reader_t *reader,
		    unsigned char byte);
static bool	 is_plain(enum relic_format format,
		    enum relic_table_state state, unsigned char byte);
static size_t	 kept_columns(const enum relic_column *columns,
		    size_t column_count);
static void	 forget_window(importer_t *importer);
static void	 frame_rest(import_framing_t *framing);
static bool	 frame_ahead(void *argument);
static bool	 frame_step(import_framing_t *framing);
static enum window_outcome decline_window(importer_t *importer);
static void	 fill_stripe(void *argument, size_t stripe,
		    size_t worker_index);
static enum relic_table_event_kind mistake(relic_table_reader_t *reader,
		    enum relic_table_mistake which,
		    const relic_location_t *where, unsigned char byte,
		    relic_table_event_t *event);
static enum relic_table_event_kind next_event(relic_table_reader_t *reader,
		    relic_table_event_t *event);
static enum relic_table_event_kind out_of_memory(
		    relic_table_reader_t *reader, relic_table_event_t *event);
static int	 read_plain_run(relic_table_reader_t *reader);
static int	 publish_window(importer_t *importer);
static int	 read_chunk(import_input_t *input);
static enum import_progress read_fields(importer_t *importer,
		    size_t horizon, size_t most_records);
static int	 read_table(importer_t *importer);
static void	 report_mistake(relic_t *ctx, const relic_import_plan_t *plan,
		    const relic_table_event_t *event);
static unsigned char separator_of(enum relic_format format);
static int	 stage_table(importer_t *importer, const char *source_name);
static enum window_outcome stage_window(importer_t *importer);
static enum import_progress stage_windows(importer_t *importer);
static enum relic_table_event_kind step(relic_table_reader_t *reader,
		    unsigned char byte, const relic_location_t *here,
		    relic_table_event_t *event);
static enum relic_table_event_kind step_after_backslash(
		    relic_table_reader_t *reader, unsigned char byte,
		    const relic_location_t *here, relic_table_event_t *event);
static enum relic_table_event_kind step_after_quote(
		    relic_table_reader_t *reader, unsigned char byte,
		    const relic_location_t *here, relic_table_event_t *event);
static enum relic_table_event_kind step_after_return(
		    relic_table_reader_t *reader, unsigned char byte,
		    relic_table_event_t *event);
static enum relic_table_event_kind step_delimiter(
		    relic_table_reader_t *reader, unsigned char byte,
		    const relic_location_t *here, relic_table_event_t *event);
static enum relic_table_event_kind step_in_quotes(
		    relic_table_reader_t *reader, unsigned char byte,
		    relic_table_event_t *event);
static enum relic_table_event_kind step_unquoted(relic_table_reader_t *reader,
		    unsigned char byte, const relic_location_t *here,
		    relic_table_event_t *event);
static enum relic_table_event_kind stop(relic_table_reader_t *reader,
		    const relic_table_event_t *event);
static enum import_progress take_end(importer_t *importer,
		    const relic_table_event_t *event);
static enum import_progress take_event(importer_t *importer,
		    const relic_table_event_t *event);
static enum import_progress take_field(importer_t *importer,
		    const relic_table_event_t *event);
static enum import_progress take_record(importer_t *importer);
static bool	 validate_import(relic_t *ctx, const relic_import_t *import);
static bool	 window_admissible(const importer_t *importer,
		    size_t value_bytes);
static void	 window_growth(const relic_window_t *window,
		    relic_tuple_growth_t *growth);

/*
 * The reader.
 *
 * Each byte moves the reader from one state to the next, and every boundary
 * follows from the state the byte arrives in: a line feed ends a record
 * outside quotes and is data inside them, and nothing else -- no second
 * pass, no count of quotes -- decides whether a field is open.  A misplaced
 * byte is a mistake in the state it arrives in, so a stray quote or a lone
 * carriage return is reported where it is read, not after the lines that
 * follow it.
 *
 * A byte can end more than one thing: a line feed ends a field and then its
 * record, and the end of the input can end a field, a record and the table.
 * The reader emits one event per call, so what a byte ends beyond the first
 * is left for the next call in a state of its own: RECORD_ENDED says the
 * record's last field has ended and the record is to end next, and AT_FIELD
 * with every column's field ended says a field beyond the last column has
 * begun.  Both are acted on before another byte is read.
 */

void
relic_table_reader_init(relic_table_reader_t *reader, enum relic_format format,
    size_t width, const char *source_name)
{
	bzero(reader, sizeof(*reader));

	reader->format = format;
	reader->width = width;
	reader->state = RELIC_TABLE_AT_RECORD;
	reader->where.file = source_name;
	reader->where.line = 1;
	reader->where.column = 1;
}

void
relic_table_reader_frame_only(relic_table_reader_t *reader)
{
	assert(reader->field == NULL);

	reader->frames = true;
}

void
relic_table_reader_start_at(relic_table_reader_t *reader,
    const relic_location_t *where)
{
	assert(reader->state == RELIC_TABLE_AT_RECORD &&
	    reader->fields_ended == 0);

	reader->where = *where;
}

size_t
relic_table_unread(const relic_table_reader_t *reader)
{
	return (reader->input_length);
}

void
relic_table_reader_reset(relic_table_reader_t *reader)
{
	free(reader->field);
	bzero(reader, sizeof(*reader));
}

void
relic_table_feed(relic_table_reader_t *reader, const char *bytes,
    size_t length, bool last)
{
	reader->input = bytes;
	reader->input_length = length;
	reader->input_is_last = last;
}

/*
 * A reader that only frames drops its FIELD events here, the one place
 * where its events differ from a decoding reader's.
 */
enum relic_table_event_kind
relic_table_next(relic_table_reader_t *reader, relic_table_event_t *event)
{
	enum relic_table_event_kind kind;

	do
		kind = next_event(reader, event);
	while (kind == RELIC_TABLE_FIELD && reader->frames);

	return (kind);
}

/*
 * The next event the table's bytes cause, FIELD events included.
 */
static enum relic_table_event_kind
next_event(relic_table_reader_t *reader, relic_table_event_t *event)
{
	for (;;) {
		relic_location_t here;
		enum relic_table_event_kind kind;
		unsigned char byte;

		if (reader->state == RELIC_TABLE_STOPPED) {
			*event = reader->stopped;
			return (event->kind);
		}

		if (reader->state == RELIC_TABLE_RECORD_ENDED)
			return (end_record(reader, event));

		if (extra_field_begun(reader))
			return (mistake(reader, RELIC_TABLE_TOO_MANY_FIELDS,
			    &reader->where, 0, event));

		if (read_plain_run(reader) != 0)
			return (out_of_memory(reader, event));

		if (reader->input_length == 0) {
			if (!reader->input_is_last) {
				bzero(event, sizeof(*event));
				event->kind = RELIC_TABLE_MORE;
				return (RELIC_TABLE_MORE);
			}
			return (end_of_input(reader, event));
		}

		byte = (unsigned char)reader->input[0];
		reader->input++;
		reader->input_length--;
		here = reader->where;
		advance_location(&reader->where, byte);

		kind = step(reader, byte, &here, event);
		if (kind != RELIC_TABLE_MORE)
			return (kind);
	}
}

/*
 * Whether `byte`, read in `state`, is plain: data that changes nothing but
 * the field and the column, whatever comes before or after it in the run.
 * Outside quotes that is every byte but a delimiter and the format's
 * quote or escape, which step_unquoted() decides by this test; inside
 * quotes it is every byte but a quote and the line feed, which is data
 * too but begins a line.  No byte is plain in a state that waits for the
 * byte after a quote, an escape or a carriage return.
 */
static bool
is_plain(enum relic_format format, enum relic_table_state state,
    unsigned char byte)
{
	switch (state) {
	case RELIC_TABLE_AT_RECORD:
	case RELIC_TABLE_AT_FIELD:
	case RELIC_TABLE_IN_FIELD:
		return (byte != separator_of(format) && byte != '\n' &&
		    byte != '\r' &&
		    byte != (format == RELIC_FORMAT_CSV ? '"' : '\\'));
	case RELIC_TABLE_IN_QUOTES:
		return (byte != '"' && byte != '\n');
	case RELIC_TABLE_AFTER_QUOTE:
	case RELIC_TABLE_AFTER_BACKSLASH:
	case RELIC_TABLE_AFTER_RETURN:
	case RELIC_TABLE_RECORD_ENDED:
	case RELIC_TABLE_STOPPED:
		break;
	}

	return (false);
}

/*
 * Read the plain bytes the input begins with at once, as step() would read
 * them one at a time: the same bytes appended, the same column reached, a
 * field begun where it would begin one.  Most of a table's bytes are read
 * here.  Returns 0, or -1 when the field could not grow.
 */
static int
read_plain_run(relic_table_reader_t *reader)
{
	const unsigned char *input;
	enum relic_table_state state;
	enum relic_format format;
	size_t run;
	char *field;

	input = (const unsigned char *)reader->input;
	state = reader->state;
	format = reader->format;
	for (run = 0; run < reader->input_length &&
	    is_plain(format, state, input[run]); run++)
		continue;
	if (run == 0)
		return (0);

	if (state == RELIC_TABLE_AT_RECORD || state == RELIC_TABLE_AT_FIELD) {
		reader->field_where = reader->where;
		reader->state = RELIC_TABLE_IN_FIELD;
	}

	if (!reader->frames) {
		field = relic_grow_array(reader->field,
		    &reader->field_capacity, reader->field_length + run,
		    sizeof(*reader->field));
		if (field == NULL)
			return (-1);
		reader->field = field;
		bcopy(input, reader->field + reader->field_length, run);
		reader->field_length += run;
	}

	reader->input += run;
	reader->input_length -= run;
	if (run > UINT32_MAX - reader->where.column)
		reader->where.column = UINT32_MAX;
	else
		reader->where.column += (uint32_t)run;

	return (0);
}

/*
 * Whether a field has begun beyond the record's last column: a separator
 * ended the field of the last one.  The record already has too many fields,
 * whatever the next byte is.
 */
static bool
extra_field_begun(const relic_table_reader_t *reader)
{
	return (reader->state == RELIC_TABLE_AT_FIELD &&
	    reader->fields_ended == reader->width);
}

/*
 * Move a location past one byte.  A line is a physical line, so only a line
 * feed begins one, inside quotes or not, and columns count bytes.  Both
 * counts stop at the largest value a location holds rather than wrapping, so
 * a table longer than that reports its later mistakes at the last line a
 * location can name, never at an earlier one.
 */
static void
advance_location(relic_location_t *where, unsigned char byte)
{
	if (byte == '\n') {
		if (where->line < UINT32_MAX)
			where->line++;
		where->column = 1;
		return;
	}

	if (where->column < UINT32_MAX)
		where->column++;
}

static unsigned char
separator_of(enum relic_format format)
{
	return (format == RELIC_FORMAT_CSV ? ',' : '\t');
}

/*
 * Read one byte in the state the reader is in.  Returns the event it caused,
 * or RELIC_TABLE_MORE when it caused none.  `here` is the byte's location.
 */
static enum relic_table_event_kind
step(relic_table_reader_t *reader, unsigned char byte,
    const relic_location_t *here, relic_table_event_t *event)
{
	switch (reader->state) {
	case RELIC_TABLE_AT_RECORD:
	case RELIC_TABLE_AT_FIELD:
		/*
		 * The field begins with this byte, whatever it is: an empty
		 * field is located at the separator or line end that ends it.
		 * Only here may a quote open a quoted field.
		 */
		reader->field_where = *here;
		if (reader->format == RELIC_FORMAT_CSV && byte == '"') {
			reader->state = RELIC_TABLE_IN_QUOTES;
			return (RELIC_TABLE_MORE);
		}
		return (step_unquoted(reader, byte, here, event));
	case RELIC_TABLE_IN_FIELD:
		return (step_unquoted(reader, byte, here, event));
	case RELIC_TABLE_IN_QUOTES:
		return (step_in_quotes(reader, byte, event));
	case RELIC_TABLE_AFTER_QUOTE:
		return (step_after_quote(reader, byte, here, event));
	case RELIC_TABLE_AFTER_BACKSLASH:
		return (step_after_backslash(reader, byte, here, event));
	case RELIC_TABLE_AFTER_RETURN:
		return (step_after_return(reader, byte, event));
	case RELIC_TABLE_RECORD_ENDED:
	case RELIC_TABLE_STOPPED:
		break;
	}

	/* Unreachable: relic_table_next() acts on both before reading. */
	assert(!"a byte read in a state that reads none");
	return (RELIC_TABLE_MORE);
}

/*
 * Whether a byte outside quotes ends a field: a separator, a line feed, or
 * a carriage return, which must be the first of a line end.
 */
static bool
is_delimiter(const relic_table_reader_t *reader, unsigned char byte)
{
	return (byte == separator_of(reader->format) || byte == '\n' ||
	    byte == '\r');
}

/*
 * A byte that ends a field, whether the field was quoted or not.  A
 * separator ends the field, a line feed ends it and its record, and a
 * carriage return is left for the byte after it to decide.
 */
static enum relic_table_event_kind
step_delimiter(relic_table_reader_t *reader, unsigned char byte,
    const relic_location_t *here, relic_table_event_t *event)
{
	if (byte == '\n') {
		reader->record_end_where = *here;
		return (end_field(reader, RELIC_TABLE_RECORD_ENDED, event));
	}

	if (byte == '\r') {
		reader->return_where = *here;
		reader->state = RELIC_TABLE_AFTER_RETURN;
		return (RELIC_TABLE_MORE);
	}

	return (end_field(reader, RELIC_TABLE_AT_FIELD, event));
}

/*
 * A byte of an unquoted field, or the first byte of any field.  A delimiter
 * ends the field.  A csv quote cannot appear in an unquoted field, since a
 * reader that let one through would have to guess what `a"b,c"` meant; a
 * tsv backslash begins an escape.
 */
static enum relic_table_event_kind
step_unquoted(relic_table_reader_t *reader, unsigned char byte,
    const relic_location_t *here, relic_table_event_t *event)
{
	if (is_plain(reader->format, reader->state, byte)) {
		reader->state = RELIC_TABLE_IN_FIELD;
		return (append_field_byte(reader, byte, event));
	}

	if (is_delimiter(reader, byte))
		return (step_delimiter(reader, byte, here, event));

	assert(byte == (reader->format == RELIC_FORMAT_CSV ? '"' : '\\'));
	if (reader->format == RELIC_FORMAT_CSV)
		return (mistake(reader, RELIC_TABLE_QUOTE_IN_UNQUOTED, here, 0,
		    event));

	reader->state = RELIC_TABLE_AFTER_BACKSLASH;
	return (RELIC_TABLE_MORE);
}

/*
 * The byte after a carriage return outside quotes.  Only a line feed may
 * follow one, and the pair is a line end located at the carriage return.  A
 * carriage return alone is a byte its producer failed to escape, or a file
 * whose lines end in one, and neither should become data silently.
 */
static enum relic_table_event_kind
step_after_return(relic_table_reader_t *reader, unsigned char byte,
    relic_table_event_t *event)
{
	if (byte != '\n')
		return (mistake(reader, RELIC_TABLE_STRAY_RETURN,
		    &reader->return_where, 0, event));

	reader->record_end_where = reader->return_where;

	return (end_field(reader, RELIC_TABLE_RECORD_ENDED, event));
}

/*
 * The byte after a tsv backslash: one of the four escapes the tsv output
 * writes, and nothing else.  An unknown escape is refused rather than read as
 * a literal backslash, because that reading could never be taken back: once
 * `\q` imports as two bytes, giving it any other meaning changes what
 * existing files import as.
 */
static enum relic_table_event_kind
step_after_backslash(relic_table_reader_t *reader, unsigned char byte,
    const relic_location_t *here, relic_table_event_t *event)
{
	unsigned char decoded;

	switch (byte) {
	case '\\':
		decoded = '\\';
		break;
	case 't':
		decoded = '\t';
		break;
	case 'n':
		decoded = '\n';
		break;
	case 'r':
		decoded = '\r';
		break;
	default:
		return (mistake(reader, RELIC_TABLE_UNKNOWN_ESCAPE, here, byte,
		    event));
	}

	reader->state = RELIC_TABLE_IN_FIELD;

	return (append_field_byte(reader, decoded, event));
}

/*
 * A byte inside a quoted csv field.  Everything but a quote is data, line
 * feeds and carriage returns included; a quote either closes the field or is
 * the first of a doubled one, which the next byte decides.
 */
static enum relic_table_event_kind
step_in_quotes(relic_table_reader_t *reader, unsigned char byte,
    relic_table_event_t *event)
{
	if (byte == '"') {
		reader->state = RELIC_TABLE_AFTER_QUOTE;
		return (RELIC_TABLE_MORE);
	}

	return (append_field_byte(reader, byte, event));
}

/*
 * The byte after a quote inside a quoted field.  A second quote makes the
 * pair one quote of data.  Anything else means the quote closed the field,
 * which must then end: at a separator, or at a line end.
 */
static enum relic_table_event_kind
step_after_quote(relic_table_reader_t *reader, unsigned char byte,
    const relic_location_t *here, relic_table_event_t *event)
{
	if (byte == '"') {
		reader->state = RELIC_TABLE_IN_QUOTES;
		return (append_field_byte(reader, byte, event));
	}

	if (is_delimiter(reader, byte))
		return (step_delimiter(reader, byte, here, event));

	return (mistake(reader, RELIC_TABLE_AFTER_CLOSING_QUOTE, here, byte,
	    event));
}

/*
 * What the end of the input means in the reader's state.  At the start of a
 * record the table has ended.  Where a field has begun, or a quote may have
 * closed one, the field ends and then its record.  Anywhere else something
 * was left open.
 */
static enum relic_table_event_kind
end_of_input(relic_table_reader_t *reader, relic_table_event_t *event)
{
	switch (reader->state) {
	case RELIC_TABLE_AT_RECORD:
		bzero(event, sizeof(*event));
		event->kind = RELIC_TABLE_END;
		event->where = reader->where;
		return (stop(reader, event));
	case RELIC_TABLE_AT_FIELD:
		reader->field_where = reader->where;
		/* FALLTHROUGH */
	case RELIC_TABLE_IN_FIELD:
	case RELIC_TABLE_AFTER_QUOTE:
		reader->record_end_where = reader->where;
		return (end_field(reader, RELIC_TABLE_RECORD_ENDED, event));
	case RELIC_TABLE_IN_QUOTES:
		return (mistake(reader, RELIC_TABLE_UNTERMINATED_QUOTE,
		    &reader->field_where, 0, event));
	case RELIC_TABLE_AFTER_BACKSLASH:
		return (mistake(reader, RELIC_TABLE_ESCAPE_AT_END,
		    &reader->where, 0, event));
	case RELIC_TABLE_AFTER_RETURN:
		return (mistake(reader, RELIC_TABLE_STRAY_RETURN,
		    &reader->return_where, 0, event));
	case RELIC_TABLE_RECORD_ENDED:
	case RELIC_TABLE_STOPPED:
		break;
	}

	/* Unreachable: relic_table_next() acts on both before this. */
	assert(!"the end of the input met in a state that reads none");
	return (RELIC_TABLE_MORE);
}

/*
 * End the field being read, emitting its bytes, which are none when the
 * reader only frames, and go on in `next`.  The field's length is taken
 * into the event before it is cleared, so the bytes the event points at
 * stay as they are until the next byte is appended, which cannot happen
 * before the caller has seen the event.  Before the first byte of any
 * field the buffer does not exist, and an empty field's bytes are then an
 * empty string rather than NULL.
 */
static enum relic_table_event_kind
end_field(relic_table_reader_t *reader, enum relic_table_state next,
    relic_table_event_t *event)
{
	bzero(event, sizeof(*event));
	event->kind = RELIC_TABLE_FIELD;
	event->bytes = reader->field != NULL ? reader->field : "";
	event->length = reader->field_length;
	event->position = reader->fields_ended;
	event->where = reader->field_where;

	reader->field_length = 0;
	reader->fields_ended++;
	reader->state = next;

	return (RELIC_TABLE_FIELD);
}

/*
 * End a record whose last field has ended, which must have been the field
 * of its last column.
 */
static enum relic_table_event_kind
end_record(relic_table_reader_t *reader, relic_table_event_t *event)
{
	if (reader->fields_ended < reader->width)
		return (mistake(reader, RELIC_TABLE_TOO_FEW_FIELDS,
		    &reader->record_end_where, 0, event));

	bzero(event, sizeof(*event));
	event->kind = RELIC_TABLE_RECORD;
	event->where = reader->record_end_where;

	reader->fields_ended = 0;
	reader->state = RELIC_TABLE_AT_RECORD;

	return (RELIC_TABLE_RECORD);
}

/*
 * Append one decoded byte to the field.  Returns RELIC_TABLE_MORE, or stops
 * the reader with RELIC_TABLE_NO_MEMORY when the field cannot grow: memory
 * the reader could not have is not the table's mistake, and the caller
 * reports it as what it is.  The buffer grows only when it is full, which is
 * rarely, so every other byte costs a comparison and a store.  A reader
 * that only frames keeps no byte.
 */
static enum relic_table_event_kind
append_field_byte(relic_table_reader_t *reader, unsigned char byte,
    relic_table_event_t *event)
{
	if (reader->frames)
		return (RELIC_TABLE_MORE);

	if (reader->field_length == reader->field_capacity) {
		char *field;

		field = relic_grow_array(reader->field,
		    &reader->field_capacity, reader->field_length + 1,
		    sizeof(*reader->field));
		if (field == NULL)
			return (out_of_memory(reader, event));
		reader->field = field;
	}

	reader->field[reader->field_length] = (char)byte;
	reader->field_length++;

	return (RELIC_TABLE_MORE);
}

/*
 * Stop the reader with RELIC_TABLE_NO_MEMORY: the field could not grow.
 */
static enum relic_table_event_kind
out_of_memory(relic_table_reader_t *reader, relic_table_event_t *event)
{
	bzero(event, sizeof(*event));
	event->kind = RELIC_TABLE_NO_MEMORY;
	event->where = reader->where;

	return (stop(reader, event));
}

/*
 * Stop the reader on a mistake in the table, `which`, at `where`.  A byte
 * the mistake names is `byte`, and a record with too few fields says how
 * many it has.
 */
static enum relic_table_event_kind
mistake(relic_table_reader_t *reader, enum relic_table_mistake which,
    const relic_location_t *where, unsigned char byte,
    relic_table_event_t *event)
{
	bzero(event, sizeof(*event));
	event->kind = RELIC_TABLE_MISTAKE;
	event->mistake = which;
	event->where = *where;
	event->byte = byte;

	if (which == RELIC_TABLE_TOO_FEW_FIELDS)
		event->fields_found = reader->fields_ended;

	return (stop(reader, event));
}

/*
 * Make `event` the reader's last: every later call returns it again.
 */
static enum relic_table_event_kind
stop(relic_table_reader_t *reader, const relic_table_event_t *event)
{
	reader->stopped = *event;
	reader->state = RELIC_TABLE_STOPPED;

	return (event->kind);
}

/*
 * The importer.
 */

int
relic_import_file(relic_t *ctx, const char *source_name, FILE *stream,
    const relic_import_t *import)
{
	relic_import_plan_t plan;
	int status;

	if (relic_enter(ctx, "importing",
	    source_name != NULL ? source_name : RELIC_UNNAMED_SOURCE) != 0)
		return (-1);

	status = -1;
	if (stream == NULL) {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "no input stream was given");
	} else if (import_plan_init(ctx, import, &plan)) {
		status = import_stream(ctx, source_name, stream, &plan);
		import_plan_reset(&plan);
	}

	relic_leave(ctx);

	return (status);
}

int
relic_import_path(relic_t *ctx, const char *path, const relic_import_t *import)
{
	relic_import_plan_t plan;
	int status;

	if (relic_enter(ctx, "importing",
	    path != NULL ? path : RELIC_UNNAMED_SOURCE) != 0)
		return (-1);

	status = -1;
	if (path == NULL) {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "no path was given");
	} else if (import_plan_init(ctx, import, &plan)) {
		status = import_named_file(ctx, path, &plan);
		import_plan_reset(&plan);
	}

	relic_leave(ctx);

	return (status);
}

/*
 * Check an import's description and copy what the import reads into
 * `plan`.  Returns false having reported why the description cannot be
 * read, or memory.
 *
 * No caller code runs between the check and the copy, so the plan holds
 * exactly what was checked.  A plan that could not be made holds nothing.
 */
static bool
import_plan_init(relic_t *ctx, const relic_import_t *import,
    relic_import_plan_t *plan)
{
	bzero(plan, sizeof(*plan));

	if (!validate_import(ctx, import))
		return (false);

	plan->predicate_length = strlen(import->predicate);
	plan->predicate = malloc(plan->predicate_length + 1);
	plan->columns = reallocarray(NULL, import->column_count,
	    sizeof(*plan->columns));
	if (plan->predicate == NULL || plan->columns == NULL) {
		import_plan_reset(plan);
		relic_diag_no_memory(ctx);
		return (false);
	}
	bcopy(import->predicate, plan->predicate, plan->predicate_length + 1);
	bcopy(import->columns, plan->columns,
	    import->column_count * sizeof(*plan->columns));

	plan->column_count = import->column_count;
	plan->arity = kept_columns(plan->columns, plan->column_count);
	plan->format = import->format;
	plan->header = (import->flags & RELIC_IMPORT_HEADER) != 0;

	return (true);
}

static void
import_plan_reset(relic_import_plan_t *plan)
{
	free(plan->columns);
	free(plan->predicate);
	bzero(plan, sizeof(*plan));
}

/*
 * Open the file at `path`, stage its table and close it, then commit what
 * it held, naming it by its path.  The file is closed before the commit, so
 * a close that fails fails the import with nothing added.
 */
static int
import_named_file(relic_t *ctx, const char *path,
    const relic_import_plan_t *plan)
{
	importer_t importer;
	FILE *stream;
	int status;

	stream = relic_open_input(ctx, path);
	if (stream == NULL)
		return (-1);

	importer_init(&importer, ctx, plan, stream);
	status = stage_table(&importer, path);
	if (status != 0)
		fclose(stream);
	else
		status = relic_close_input(ctx, path, stream);
	if (status == 0)
		status = relic_staging_commit(&importer.staging);
	importer_reset(&importer);

	return (status);
}

/*
 * The number of columns an import keeps, which is its predicate's arity.
 */
static size_t
kept_columns(const enum relic_column *columns, size_t column_count)
{
	size_t kept;
	size_t i;

	kept = 0;
	for (i = 0; i < column_count; i++) {
		if (columns[i] != RELIC_COLUMN_SKIP)
			kept++;
	}

	return (kept);
}

/*
 * Whether an import's description can be read, reporting why it cannot.
 * Nothing is read or interned before every part of it has been checked.
 *
 * An unknown flag is refused, where an unknown configuration switch is
 * ignored.  No switch changes an answer, so a library may ignore one it
 * predates; an import flag changes what is read, and a library that ignored
 * a header flag it did not know would import the header as a fact.
 */
static bool
validate_import(relic_t *ctx, const relic_import_t *import)
{
	size_t i;

	if (import == NULL) {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "no import description was given");
		return (false);
	}

	if (import->predicate == NULL) {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "an import needs a predicate name");
		return (false);
	}

	if (!relic_lex_is_predicate_name(import->predicate,
	    strlen(import->predicate))) {
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, NULL,
		    "a predicate name is a lowercase letter followed by "
		    "letters, digits and underscores, and is not `not`");
		return (false);
	}

	if (import->columns == NULL || import->column_count == 0) {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "an import needs at least one column");
		return (false);
	}

	for (i = 0; i < import->column_count; i++) {
		if (import->columns[i] < RELIC_COLUMN_SYMBOL ||
		    import->columns[i] > RELIC_COLUMN_SKIP) {
			relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
			    "column %zu of the import has no type", i + 1);
			return (false);
		}
	}

	if (kept_columns(import->columns, import->column_count) >
	    RELIC_MAX_ARITY) {
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, NULL,
		    "an import may keep at most %d columns", RELIC_MAX_ARITY);
		return (false);
	}

	if (import->format != RELIC_FORMAT_CSV &&
	    import->format != RELIC_FORMAT_TSV) {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "the import's format is neither csv nor tsv");
		return (false);
	}

	if ((import->flags & ~RELIC_IMPORT_HEADER) != 0) {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "the import's flags hold bits this library does not "
		    "define: 0x%x", import->flags & ~RELIC_IMPORT_HEADER);
		return (false);
	}

	return (true);
}

/*
 * Import a table from a stream by `plan`: stage every record, then commit.
 */
static int
import_stream(relic_t *ctx, const char *source_name, FILE *stream,
    const relic_import_plan_t *plan)
{
	importer_t importer;
	int status;

	importer_init(&importer, ctx, plan, stream);
	status = stage_table(&importer, source_name);
	if (status == 0)
		status = relic_staging_commit(&importer.staging);
	importer_reset(&importer);

	return (status);
}

static void
importer_init(importer_t *importer, relic_t *ctx,
    const relic_import_plan_t *plan, FILE *stream)
{
	bzero(importer, sizeof(*importer));
	importer->ctx = ctx;
	importer->plan = plan;
	importer->input.stream = stream;
	importer->header_pending = plan->header;
	relic_staging_init(&importer->staging, ctx);
	relic_window_init(&importer->window, (uint16_t)plan->arity);
	relic_framer_init(&importer->framing.framer, RELIC_IMPORT_WINDOW_BYTES,
	    RELIC_IMPORT_WINDOW_RECORDS, RELIC_IMPORT_PIECE_BYTES);
	importer->framing.input = &importer->input;
}

static void
importer_reset(importer_t *importer)
{
	forget_window(importer);
	relic_window_reset(&importer->window);
	relic_framer_reset(&importer->framing.framer);
	relic_table_reader_reset(&importer->reader);
	relic_staging_reset(&importer->staging);
	free(importer->input.bytes);
	free(importer->source_name);
}

/*
 * Read the table from the importer's stream and stage its records, none
 * yet committed.  Returns 0, or -1 having reported why the table cannot be
 * imported.
 *
 * The predicate is interned before the table is read, and so before any of
 * its values, as a load interns a fact's predicate when it stages the fact.
 * Like the values its fields intern, it stays interned if the import fails.
 *
 * The locations the reader reports name the source by the importer's copy
 * of the name the caller gave, or of "<input>": the stream's read function
 * is the caller's and may rewrite the caller's buffer while the table is
 * read, and a location of an import is read only within the call, so the
 * copy lives exactly as long as the import.
 */
static int
stage_table(importer_t *importer, const char *source_name)
{
	const relic_import_plan_t *plan;
	const char *name;
	size_t length;

	plan = importer->plan;
	name = source_name != NULL ? source_name : RELIC_UNNAMED_SOURCE;

	length = strlen(name);
	importer->source_name = malloc(length + 1);
	if (importer->source_name == NULL) {
		relic_diag_no_memory(importer->ctx);
		return (-1);
	}
	bcopy(name, importer->source_name, length + 1);
	name = importer->source_name;

	if (relic_pred_intern(importer->ctx, plan->predicate,
	    plan->predicate_length, (uint16_t)plan->arity, NULL,
	    &importer->pred) != 0)
		return (-1);

	relic_table_reader_init(&importer->reader, plan->format,
	    plan->column_count, name);
	importer->boundary = importer->reader.where;

	return (read_table(importer));
}

/*
 * Read the whole table and stage its records.  Returns 0 when the table
 * ended with every record staged, or -1 having reported the first mistake,
 * a failed read, or memory.
 *
 * The table is staged a window at a time when the context may batch it,
 * the stream is a regular file, its first window's horizon of bytes does
 * not hold it all (unless batching is forced), and the pool is ready to
 * share it.  A window that is not admitted whole has its records read a
 * field at a time, and windows go on after them; a table not batched is
 * read a field at a time whole.  The first window's bytes are the
 * reader's when the table is not batched; a read for them that failed is
 * the reader's to report, and one that found no room the reader makes
 * again.  Both stage exactly the same facts and intern exactly the same
 * values under the same identifiers, and a window is admitted only when
 * reading it a field at a time could not fail, so the import fails, when
 * it does, at the same record and with the same message as a field at a
 * time, with the same values interned.  Each turn of the loop stages a
 * window or reads a record, or ends the import.
 */
static int
read_table(importer_t *importer)
{
	enum import_progress progress;
	relic_t *ctx;
	size_t horizon;
	size_t most_records;
	bool batched;

	ctx = importer->ctx;
	batched = false;
	if (may_batch(ctx) && reads_a_regular_file(importer->input.stream) &&
	    fill_input(&importer->input, RELIC_IMPORT_WINDOW_BYTES) == 0 &&
	    (batching_forced(ctx) || outgrows_window(&importer->input)))
		batched = prepare_batching(ctx) == 0;

	horizon = batched ? RELIC_IMPORT_WINDOW_BYTES : SIZE_MAX;
	most_records = batched ? RELIC_IMPORT_WINDOW_RECORDS : SIZE_MAX;
	do {
		progress = batched ? stage_windows(importer) : IMPORT_READ_ON;
		if (progress == IMPORT_READ_ON)
			progress = read_fields(importer, horizon,
			    most_records);
	} while (progress == IMPORT_READ_ON);

	return (progress == IMPORT_TABLE_ENDED ? 0 : -1);
}

/*
 * Whether the context may stage a table a window at a time (internal.h):
 * never under RELIC_F_NO_BATCH_IMPORT, always under
 * RELIC_F_PARALLEL_ALWAYS, and otherwise when it has
 * RELIC_IMPORT_BATCH_WORKERS threads or more.
 */
static bool
may_batch(const relic_t *ctx)
{
	if ((ctx->config.flags & RELIC_F_NO_BATCH_IMPORT) != 0)
		return (false);
	if (batching_forced(ctx))
		return (true);

	return (ctx->config.threads >= RELIC_IMPORT_BATCH_WORKERS);
}

/*
 * Whether the stream reads a regular file, whose reads return what it
 * holds without waiting for a producer to write more.  A window is read
 * whole before any of its records is typed, so a pipe, a socket, a
 * terminal or a stream of the caller's own, whose producer may send a
 * malformed record and then wait, is read a field at a time, its mistake
 * reported once the chunk holding it has arrived.  A stream without a
 * descriptor, or one fstat(2) cannot describe, is not a regular file.
 */
static bool
reads_a_regular_file(FILE *stream)
{
	struct stat status;
	int descriptor;

#ifdef RELIC_IMPORT_REGULAR_FILE
	/*
	 * The small-window import suite's stand-in.  No regular file a test
	 * can make fails to read, so that build names a function here that
	 * takes a failing stream of the suite's own for one, and the read
	 * errors a batch import meets are reached.  No other build defines it.
	 */
	if (RELIC_IMPORT_REGULAR_FILE(stream))
		return (true);
#endif

	descriptor = fileno(stream);

	return (descriptor >= 0 && fstat(descriptor, &status) == 0 &&
	    S_ISREG(status.st_mode));
}

/*
 * Whether the context stages every table a window at a time, whatever its
 * size and however few workers its pool runs, unless
 * RELIC_F_NO_BATCH_IMPORT forbids it.
 */
static bool
batching_forced(const relic_t *ctx)
{
	return ((ctx->config.flags & RELIC_F_PARALLEL_ALWAYS) != 0);
}

/*
 * Whether the table goes on past the window's worth fill_input() has read
 * of it: its end has not been met, and no read has failed.
 */
static bool
outgrows_window(const import_input_t *input)
{
	return (!input->ended && !input->failed);
}

/*
 * Make the context's pool ready to share a table's windows: made the first
 * time, as a round makes it, and made the calling process's by
 * relic_pool_workers(), which leaves a forked child's with one worker.
 * Returns 0, or -1 when the pool cannot be made or, unless batching is
 * forced, runs fewer than RELIC_IMPORT_BATCH_WORKERS workers, as the pool of
 * a forked child, or one whose threads did not all start, may; the table is
 * then read a field at a time.  Reports nothing.
 */
static int
prepare_batching(relic_t *ctx)
{
	if (ctx->pool == NULL) {
		if (relic_pool_create(&ctx->pool, ctx->config.threads,
		    ctx->hash_seed) != 0)
			return (-1);
	}
	if (relic_pool_workers(ctx->pool) < RELIC_IMPORT_BATCH_WORKERS &&
	    !batching_forced(ctx))
		return (-1);

	return (0);
}

/*
 * Stage windows of the table from `boundary` for as long as each is
 * admitted whole.  Returns IMPORT_TABLE_ENDED when the last window of the
 * table was, IMPORT_FAILED having reported why room for one could not be
 * made, or IMPORT_READ_ON when one was declined, which leaves its bytes in
 * the input for the reader to read from `boundary`, or the input holds
 * nothing for the reader to read but the table's end or a failed read.
 * The last window framed is forgotten before the reader begins, and its
 * storage kept for the windows after it.  A window there is no room to
 * read the horizon of is declined.
 */
static enum import_progress
stage_windows(importer_t *importer)
{
	enum window_outcome outcome;

	if (fill_input(&importer->input, RELIC_IMPORT_WINDOW_BYTES) != 0) {
		importer->ctx->counters.windows_declined++;
		return (IMPORT_READ_ON);
	}

	importer->framing.start = 0;
	relic_framer_begin(&importer->framing.framer, importer->plan,
	    importer->source_name, &importer->boundary,
	    importer->header_pending, importer->input.length,
	    importer->input.ended);
	do
		outcome = stage_window(importer);
	while (outcome == WINDOW_STAGED);
	forget_window(importer);
	importer->ctx->counters.frames_ahead += importer->framing.frames_ahead;
	importer->framing.frames_ahead = 0;

	switch (outcome) {
	case WINDOW_ENDED:
		return (IMPORT_TABLE_ENDED);
	case WINDOW_FAILED:
		return (IMPORT_FAILED);
	case WINDOW_STAGED:
	case WINDOW_DECLINED:
	case WINDOW_NONE:
		break;
	}

	return (IMPORT_READ_ON);
}

/*
 * Read the stream in chunks until the input holds `target` bytes, or the
 * stream has ended or failed.  Returns 0, or -1 when there was no room to
 * read into, having reported nothing.
 */
static int
fill_input(import_input_t *input, size_t target)
{
	while (!input->ended && !input->failed && input->length < target) {
		if (read_chunk(input) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Stage the window the input begins with, which the framer is framing.
 *
 * The framing is finished first, on the calling thread; the input holds
 * the window's horizon, or the rest of the table, so nothing need be read
 * for it.  The window then takes the framer's manifest, and the framer
 * begins on the next window, at this one's end, once the input holds the
 * next window's horizon too.  This window's analysis jobs carry the next
 * one's framing as their filler, and whatever it leaves is finished when
 * the next window is staged.
 *
 * A window that cannot be framed, decoded or proved admissible is
 * declined, with nothing of it interned or staged and the next window's
 * framing dropped, and so is one whose next horizon there is no room to
 * read.  An admitted window's values and new tuples are added once room
 * is made for them, which cannot fail after; a failure making it is the
 * import's.  An input that holds nothing has ended or failed, and holds no
 * window.
 */
static enum window_outcome
stage_window(importer_t *importer)
{
	relic_pool_filler_t filler;
	const relic_pool_filler_t *ahead;
	import_input_t *input;
	relic_window_t *window;
	size_t value_bytes;

	input = &importer->input;
	window = &importer->window;
	forget_window(importer);
	if (input->length == 0)
		return (WINDOW_NONE);

	assert(importer->framing.framer.state != RELIC_FRAMING_NONE);
	frame_rest(&importer->framing);
	if (importer->framing.framer.state != RELIC_FRAMED)
		return (decline_window(importer));
	relic_window_take_frame(window, &importer->framing.framer);

	ahead = NULL;
	if (!window->ends_input) {
		if (fill_input(input, window->length +
		    RELIC_IMPORT_WINDOW_BYTES) != 0)
			return (decline_window(importer));
		importer->framing.start = window->length;
		relic_framer_begin(&importer->framing.framer, importer->plan,
		    importer->source_name, &window->end, false,
		    input->length - window->length, input->ended);
		filler.step = frame_ahead;
		filler.argument = &importer->framing;
		ahead = &filler;
	}
	window->input = input->bytes;

	if (relic_window_size(window) != 0)
		return (decline_window(importer));
	if (analyze_window(importer, ahead, &value_bytes) != 0)
		return (decline_window(importer));
	if (!window_admissible(importer, value_bytes))
		return (decline_window(importer));

	if (publish_window(importer) != 0)
		return (WINDOW_FAILED);

	importer->ctx->counters.windows_staged++;
	consume_input(input, window->length);
	importer->framing.start = 0;
	importer->boundary = window->end;
	importer->header_pending = false;

	return (window->ends_input ? WINDOW_ENDED : WINDOW_STAGED);
}

/*
 * Decline the window: the reader reads its records, from `boundary`, and
 * windows go on after them.
 */
static enum window_outcome
decline_window(importer_t *importer)
{
	importer->ctx->counters.windows_declined++;

	return (WINDOW_DECLINED);
}

/*
 * One step of the framing over the window it frames, which begins
 * `start` bytes into the input.  The base is found anew at each step,
 * since the input's bytes move when a window is consumed.
 */
static bool
frame_step(import_framing_t *framing)
{
	assert(framing->start + framing->framer.buffered <=
	    framing->input->length);

	return (relic_framer_step(&framing->framer,
	    framing->input->bytes + framing->start, RELIC_IMPORT_READ_CHUNK));
}

/*
 * A step of the framing as the filler of a window's analysis jobs, run on
 * the calling thread while the parts run on the others.  The stream is
 * read, and the input's bytes moved, only between jobs, and the parts read
 * no byte after the window's, where the framer reads; so the filler and
 * the parts share nothing either writes, and the framing's alignment
 * keeps its state off the parts' cache lines.
 */
static bool
frame_ahead(void *argument)
{
	import_framing_t *framing;

	framing = argument;
	if (framing->framer.state == RELIC_FRAMING)
		framing->frames_ahead++;

	return (frame_step(framing));
}

/* Finish, on the calling thread, whatever the framing has left. */
static void
frame_rest(import_framing_t *framing)
{
	while (frame_step(framing))
		continue;
}

/*
 * Forget the window the importer last framed, and the map of its values.
 */
static void
forget_window(importer_t *importer)
{
	relic_window_clear(&importer->window);
	if (importer->mapped) {
		relic_pending_map_close(&importer->map);
		importer->mapped = false;
	}
}

/*
 * Decode the window's pieces, number their values, and find the tuples
 * new to the predicate's facts, into the window and the map alone: the
 * steps lib/window.h lists, each job on the pool.  *value_bytesp is what
 * the value table and the pool would hold with the window's values
 * interned.  Returns 0, or -1 when a field holds no value its column
 * accepts, a value would be given an identifier no identifier can name,
 * or memory ran out.
 *
 * The map is open only when the window holds a value; a window of tuples
 * of no columns has none, and its values take no more room than they do.
 *
 * The jobs whose parts only read what the window's framing and earlier
 * steps wrote carry `ahead`, the framing of the next window, as their
 * filler, when it is not NULL.
 */
static int
analyze_window(importer_t *importer, const relic_pool_filler_t *ahead,
    size_t *value_bytesp)
{
	relic_window_t *window;
	relic_pending_batch_t batch;
	relic_window_job_t job;
	relic_t *ctx;

	ctx = importer->ctx;
	window = &importer->window;
	job.ctx = ctx;
	job.plan = importer->plan;
	job.source_name = importer->source_name;
	job.window = window;
	job.builder = relic_staging_builder(&importer->staging, importer->pred);
	job.store = relic_base_store(ctx, importer->pred);
	job.ids = NULL;
	job.tuple_seed = ctx->hash_seed;

	relic_pool_for_filled(ctx->pool, window->piece_count,
	    relic_window_decode_part, &job, ahead);
	if (!relic_window_decoded(window))
		return (-1);
	if (relic_window_gather(window) != 0)
		return (-1);

	*value_bytesp = relic_value_storage_bytes(ctx);
	if (window->value_count > 0) {
		if (relic_pending_map_open(&importer->map,
		    window->value_count) != 0)
			return (-1);
		importer->mapped = true;
		job.ids = importer->map.ids;
		relic_pool_for_filled(ctx->pool, RELIC_WINDOW_VALUE_PARTS,
		    relic_window_find_part, &job, ahead);

		batch.values = window->values;
		batch.hashes = window->value_hashes;
		batch.bytes = window->decoded;
		batch.count = window->value_count;
		*value_bytesp = relic_pending_number(ctx, &batch,
		    &importer->map);
		if (!relic_pending_fits(ctx, &importer->map))
			return (-1);
	}

	relic_pool_for_filled(ctx->pool, window->piece_count,
	    relic_window_resolve_part, &job, ahead);
	if (relic_window_list(window) != 0)
		return (-1);
	relic_pool_for_filled(ctx->pool, RELIC_STRIPES,
	    relic_window_classify_part, &job, ahead);
	relic_window_rank(window);

	return (0);
}

/*
 * Whether the window can be staged with no check failing that reading it a
 * field at a time would make, its identifiers having been found to fit.
 * The value table and the pool, grown to what interning the window's
 * values makes them, `value_bytes`, must fit the memory limit, which asks
 * about each growth: each grows to no more than that, so if the last fits
 * every one does.  Interning asks the limit only about a growth, so the
 * question is put only when the values take more room than they do now.
 * And the new tuples, with the values in, must pass staging's checks,
 * which grow with each tuple too.  It reports nothing and changes nothing.
 */
static bool
window_admissible(const importer_t *importer, size_t value_bytes)
{
	relic_tuple_growth_t growth;
	const relic_window_t *window;
	const relic_t *ctx;
	size_t current;

	ctx = importer->ctx;
	window = &importer->window;
	current = relic_value_storage_bytes(ctx);
	if (value_bytes == SIZE_MAX || (value_bytes > current &&
	    relic_account_exceeds(&ctx->account, current, value_bytes)))
		return (false);

	window_growth(window, &growth);

	return (relic_staging_admits(&importer->staging, importer->pred,
	    &growth, value_bytes));
}

/* The window's new tuples, counted by the canonical stripe each falls in. */
static void
window_growth(const relic_window_t *window, relic_tuple_growth_t *growth)
{
	size_t s;

	for (s = 0; s < RELIC_STRIPES; s++)
		growth->by_stripe[s] = window->fresh_by_stripe[s];
}

/*
 * Intern the window's values and stage its new tuples, in input order,
 * the tuples a stripe of the builder to a part on the pool.  Room for both
 * is made first, so that nothing is added unless everything can be.
 * Returns 0, or -1 having reported that memory ran out, with the values
 * and the facts staged as they were.
 *
 * Staging is reserved whenever the window holds a tuple, fresh or not,
 * since the first tuple a field-at-a-time import stages begins the
 * predicate's staged set, and with it the floor its store's rows add,
 * whether or not the tuple is new.
 */
static int
publish_window(importer_t *importer)
{
	relic_window_t *window;
	relic_tuple_growth_t growth;
	relic_stripe_fill_t fills[RELIC_STRIPES];
	relic_builder_batch_t batch;
	window_fill_t fill;
	relic_t *ctx;

	ctx = importer->ctx;
	window = &importer->window;
	window_growth(window, &growth);

	if (importer->mapped &&
	    relic_pending_reserve(ctx, &importer->map) != 0) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	if (window->tuple_count > 0 && relic_staging_reserve(&importer->staging,
	    importer->pred, &growth) != 0) {
		if (importer->mapped)
			relic_pending_unreserve(ctx, &importer->map);
		relic_diag_no_memory(ctx);
		return (-1);
	}

	if (importer->mapped)
		relic_pending_append(ctx, &importer->map);
	if (window->fresh_count == 0)
		return (0);

	batch.tuples = window->tuples;
	batch.hashes = window->hashes;
	batch.ranks = window->ranks;
	batch.stripe_list = window->stripe_list;
	batch.stripe_start = window->stripe_start;
	batch.count = window->fresh_count;
	relic_staging_stripe_fills(&importer->staging, importer->pred, fills);
	fill.fills = fills;
	fill.batch = &batch;
	relic_pool_for(ctx->pool, RELIC_STRIPES, fill_stripe, &fill);
	relic_staging_admit(&importer->staging, importer->pred, &batch);

	return (0);
}

/* A pool part: the builder's stripe filled with the tuples ranked in it. */
static void
fill_stripe(void *argument, size_t stripe, size_t worker_index RELIC_UNUSED)
{
	const window_fill_t *fill;

	fill = argument;
	relic_stripe_fill(&fill->fills[stripe], fill->batch);
}

/*
 * Read the table from `boundary` one field at a time, feeding the reader
 * the input and handing its events to the importer in order, until the
 * table ends, the import fails, or a record ends that is the
 * `most_records`th read, a header counted, or ends `horizon` bytes or more
 * past `boundary`.  In the last case returns IMPORT_READ_ON with
 * `boundary` at that record's end and the input beginning there, its bytes
 * after the record unread.  A window's bounds read the records of a window
 * declined there, and of one that would have held no record, the first;
 * SIZE_MAX for both reads the rest of the table.
 *
 * The input holds the bytes last fed the reader, and nothing else, from
 * the first feed on; `dropped` counts, saturating, the bytes read past
 * `boundary` that feed_reader() has dropped from it.  The reader is fed
 * nothing before the bytes it has not read move to the input's start.
 */
static enum import_progress
read_fields(importer_t *importer, size_t horizon, size_t most_records)
{
	relic_table_event_t event;
	enum import_progress progress;
	relic_table_reader_t *reader;
	import_input_t *input;
	size_t dropped;
	size_t records;
	size_t taken;

	input = &importer->input;
	reader = &importer->reader;
	relic_table_reader_start_at(reader, &importer->boundary);
	relic_table_feed(reader, input->bytes, input->length, input->ended);
	dropped = 0;
	records = 0;
	do {
		if (relic_table_next(reader, &event) == RELIC_TABLE_MORE) {
			dropped = relic_saturating_sum(dropped, input->length);
			progress = feed_reader(importer);
			continue;
		}
		progress = take_event(importer, &event);
		if (progress != IMPORT_READ_ON ||
		    event.kind != RELIC_TABLE_RECORD)
			continue;

		records++;
		taken = input->length - relic_table_unread(reader);
		if (records == most_records ||
		    relic_saturating_sum(dropped, taken) >= horizon) {
			relic_table_feed(reader, NULL, 0, false);
			consume_input(input, taken);
			importer->boundary = reader->where;
			return (IMPORT_READ_ON);
		}
	} while (progress == IMPORT_READ_ON);

	return (progress);
}

/*
 * Drop the bytes the input holds, which the reader has read all of, and
 * feed it the next chunk of the stream.  Reads on, or fails having
 * reported a failed read or memory.  The reader borrows the bytes, which
 * stay where they are until it asks for more.  A failed read is reported
 * once every byte read before it has been fed, and so once every record
 * before it has been staged.  A stream that fails without saying why, such
 * as one whose read function sets no errno, is reported as an input error
 * rather than as no error at all.
 */
static enum import_progress
feed_reader(importer_t *importer)
{
	import_input_t *input;

	input = &importer->input;
	consume_input(input, input->length);
	if (!input->ended && !input->failed && read_chunk(input) != 0) {
		relic_diag_no_memory(importer->ctx);
		return (IMPORT_FAILED);
	}

	if (input->length == 0 && input->failed) {
		relic_diag_error(importer->ctx, RELIC_ERROR_IO, NULL,
		    "cannot read %s: %s", importer->source_name,
		    strerror(input->read_errno != 0 ? input->read_errno :
		    EIO));
		return (IMPORT_FAILED);
	}

	relic_table_feed(&importer->reader, input->bytes, input->length,
	    input->ended);

	return (IMPORT_READ_ON);
}

/*
 * Read the next chunk of the stream after the bytes the input holds.
 * Returns 0, or -1 when there was no room for it, having read nothing.
 *
 * A short read with no error on the stream is its end.  A short read with
 * an error has failed, and whatever bytes it returned are dropped rather
 * than taken for what the stream holds.  errno is cleared before the read,
 * because a stream whose error indicator was already set can return short
 * without touching it, and the message would then name whatever an
 * earlier call left behind.
 */
static int
read_chunk(import_input_t *input)
{
	char *bytes;
	size_t got;
	int read_errno;

	assert(!input->ended && !input->failed);

	bytes = relic_grow_array(input->bytes, &input->capacity,
	    input->length + RELIC_IMPORT_READ_CHUNK, sizeof(*bytes));
	if (bytes == NULL)
		return (-1);
	input->bytes = bytes;

	errno = 0;
	got = fread(input->bytes + input->length, 1, RELIC_IMPORT_READ_CHUNK,
	    input->stream);
	read_errno = errno;

	if (got < RELIC_IMPORT_READ_CHUNK && ferror(input->stream)) {
		input->failed = true;
		input->read_errno = read_errno;
		return (0);
	}

	input->length += got;
	input->ended = got < RELIC_IMPORT_READ_CHUNK;

	return (0);
}

/*
 * Drop the first `length` bytes the input holds, which have been read.
 * The bytes after them move to the input's start, so a reader that
 * borrowed them reads nothing more before it is fed again.
 */
static void
consume_input(import_input_t *input, size_t length)
{
	assert(length <= input->length);

	if (length < input->length)
		bcopy(input->bytes + length, input->bytes,
		    input->length - length);
	input->length -= length;
}

/*
 * Act on one event of the reader's other than a request for more input,
 * reporting why the import fails when it does.
 */
static enum import_progress
take_event(importer_t *importer, const relic_table_event_t *event)
{
	switch (event->kind) {
	case RELIC_TABLE_FIELD:
		return (take_field(importer, event));
	case RELIC_TABLE_RECORD:
		return (take_record(importer));
	case RELIC_TABLE_END:
		return (take_end(importer, event));
	case RELIC_TABLE_MISTAKE:
		report_mistake(importer->ctx, importer->plan, event);
		return (IMPORT_FAILED);
	case RELIC_TABLE_NO_MEMORY:
		relic_diag_no_memory(importer->ctx);
		return (IMPORT_FAILED);
	case RELIC_TABLE_MORE:
		break;
	}

	/* Unreachable: read_table() feeds the reader instead. */
	assert(!"a request for more input taken as an event");
	return (IMPORT_FAILED);
}

/*
 * Type a field by its column: intern the value it holds into the record's
 * tuple, or drop it when its column is skipped.  Fails having reported a
 * field its column refuses, or memory.
 *
 * The reader emits a field only at a position below the plan's column
 * count, and the plan keeps `arity` columns, so a record's kept fields fill
 * the tuple and never pass it.
 */
static enum import_progress
take_field(importer_t *importer, const relic_table_event_t *event)
{
	enum relic_column column;
	relic_dom_t value;

	if (importer->header_pending)
		return (IMPORT_READ_ON);

	column = importer->plan->columns[event->position];
	if (column == RELIC_COLUMN_SKIP)
		return (IMPORT_READ_ON);

	if (!field_value(importer->ctx, column, event, &value))
		return (IMPORT_FAILED);

	assert(importer->kept < importer->plan->arity);
	importer->tuple[importer->kept] = value;
	importer->kept++;

	return (IMPORT_READ_ON);
}

/*
 * The value a field holds under its kept column's type, interned as a
 * program's constant is once relic_judge_field() admits it, the verdict
 * a batch's decoding parts ask too.  Returns false having reported a field
 * the verdict refuses, or a refusal of interning.
 *
 * A field is located at its first byte, which for a quoted csv field is the
 * opening quote, and is quoted in a message as it was decoded, the value it
 * would have been.
 */
static bool
field_value(relic_t *ctx, enum relic_column column,
    const relic_table_event_t *event, relic_dom_t *valuep)
{
	char described[IMPORT_FIELD_TEXT_MAX];
	int64_t integer;

	switch (relic_judge_field(column, event->bytes, event->length,
	    &integer)) {
	case RELIC_FIELD_VALID:
		if (column == RELIC_COLUMN_INTEGER)
			return (relic_value_intern_integer(ctx, integer,
			    valuep) == 0);
		return (relic_value_intern_bytes(ctx,
		    column == RELIC_COLUMN_SYMBOL ? RELIC_SYMBOL :
		    RELIC_STRING, event->bytes, event->length, valuep) == 0);
	case RELIC_FIELD_TOO_LONG:
		relic_symbol_refuse_length(ctx, &event->where, event->length);
		return (false);
	case RELIC_FIELD_NOT_SYMBOL:
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, &event->where,
		    "expected a symbol, found %s", describe_field(event->bytes,
		    event->length, described, sizeof(described)));
		return (false);
	case RELIC_FIELD_NOT_INTEGER:
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, &event->where,
		    "expected an integer, found %s",
		    describe_field(event->bytes, event->length, described,
		    sizeof(described)));
		return (false);
	case RELIC_FIELD_OUT_OF_RANGE:
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, &event->where,
		    "integer out of range: %s", describe_field(event->bytes,
		    event->length, described, sizeof(described)));
		return (false);
	}

	/* Unreachable: every verdict is answered above. */
	assert(!"a field verdict answered by no case");
	return (false);
}

/*
 * A record has ended with a field per column, so its tuple is whole: stage
 * it, unless the record was the header.  Fails having reported why the
 * tuple could not be staged.
 */
static enum import_progress
take_record(importer_t *importer)
{
	if (importer->header_pending) {
		importer->header_pending = false;
		return (IMPORT_READ_ON);
	}

	importer->kept = 0;

	if (relic_staging_add(&importer->staging, importer->pred,
	    importer->tuple) != 0)
		return (IMPORT_FAILED);

	return (IMPORT_READ_ON);
}

/*
 * The table has ended.  A header the import was told of must have been
 * there: a table with no records has none.
 */
static enum import_progress
take_end(importer_t *importer, const relic_table_event_t *event)
{
	if (!importer->header_pending)
		return (IMPORT_TABLE_ENDED);

	relic_diag_error(importer->ctx, RELIC_ERROR_REJECTED, &event->where,
	    "expected a header, found the end of the input");

	return (IMPORT_FAILED);
}

/*
 * Report a mistake the reader found in the table's structure.
 */
static void
report_mistake(relic_t *ctx, const relic_import_plan_t *plan,
    const relic_table_event_t *event)
{
	char described[RELIC_BYTE_TEXT_MAX];
	const char *plural;

	plural = plan->column_count == 1 ? "" : "s";

	switch (event->mistake) {
	case RELIC_TABLE_TOO_MANY_FIELDS:
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, &event->where,
		    "expected %zu field%s, found more than %zu",
		    plan->column_count, plural, plan->column_count);
		break;
	case RELIC_TABLE_TOO_FEW_FIELDS:
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, &event->where,
		    "expected %zu field%s, found %zu", plan->column_count,
		    plural, event->fields_found);
		break;
	case RELIC_TABLE_UNKNOWN_ESCAPE:
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, &event->where,
		    "unknown escape: a backslash followed by %s",
		    relic_diag_byte(event->byte, described, sizeof(described)));
		break;
	case RELIC_TABLE_ESCAPE_AT_END:
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, &event->where,
		    "unknown escape: a backslash at the end of the input");
		break;
	case RELIC_TABLE_STRAY_RETURN:
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, &event->where,
		    "carriage return inside a field");
		break;
	case RELIC_TABLE_QUOTE_IN_UNQUOTED:
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, &event->where,
		    "quote inside an unquoted field");
		break;
	case RELIC_TABLE_AFTER_CLOSING_QUOTE:
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, &event->where,
		    "expected ',' or the end of the line after a closing "
		    "quote, found %s",
		    relic_diag_byte(event->byte, described, sizeof(described)));
		break;
	case RELIC_TABLE_UNTERMINATED_QUOTE:
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, &event->where,
		    "unterminated quoted field");
		break;
	}
}

/*
 * Describe a field for a message, into a buffer the caller owns, and return
 * the buffer.
 *
 * A message must be printable ASCII with no backslash.  It goes to whatever
 * diagnostic callback is installed, which may show it without escaping it,
 * and the tool's formatter doubles a backslash so that its own escapes
 * cannot be mistaken for the text's, which would misstate the field.  So a
 * field is quoted in full, in double quotes, only when every byte is
 * printable ASCII other than a quote or a backslash, and cut past
 * IMPORT_QUOTED_FIELD_MAX bytes with "..." after the closing quote.  Any
 * other field is named by the first byte that stops it being quoted, which
 * tells the reader what to look for in it; the location still points at the
 * field.
 */
static const char *
describe_field(const char *bytes, size_t length, char *buffer, size_t size)
{
	char described[RELIC_BYTE_TEXT_MAX];
	size_t quoted;
	size_t i;

	if (length == 0) {
		snprintf(buffer, size, "an empty field");
		return (buffer);
	}

	for (i = 0; i < length; i++) {
		unsigned char byte;

		byte = (unsigned char)bytes[i];
		if (byte < ' ' || byte > '~' || byte == '"' || byte == '\\') {
			snprintf(buffer, size, "a field holding %s",
			    relic_diag_byte(byte, described,
			    sizeof(described)));
			return (buffer);
		}
	}

	quoted = length;
	if (quoted > IMPORT_QUOTED_FIELD_MAX)
		quoted = IMPORT_QUOTED_FIELD_MAX;

	snprintf(buffer, size, "\"%.*s\"%s", (int)quoted, bytes,
	    length > quoted ? "..." : "");

	return (buffer);
}
