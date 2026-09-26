/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"

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
 * How much of the stream is read per call.  Only the number of read calls
 * depends on it: the reader holds one field, not the chunk.
 */
#define	IMPORT_READ_CHUNK	(64u * 1024u)

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

typedef struct import_plan import_plan_t;
typedef struct importer importer_t;

/*
 * What an import reads, copied from the caller's description once it has
 * been checked.  The description is borrowed, and caller code runs while a
 * table is read -- the stream's read function, the diagnostic callback --
 * so nothing is read from it after the copy, and a description changed
 * behind the import's back cannot make it keep more columns than it
 * checked.  `columns` is owned; `arity` is the number not skipped;
 * `predicate_name` is the predicate's name, interned.
 */
struct import_plan {
	enum relic_column	*columns;
	uint32_t		 predicate_name;
	size_t			 column_count;
	size_t			 arity;
	enum relic_format	 format;
	bool			 header;
};

/*
 * An import in progress.  `tuple` holds the values of the current record's
 * kept columns, `kept` of them so far.  `header_pending` is set while the
 * first record is still to be read as a header: its fields are neither typed
 * nor interned, and its end stages nothing.
 */
struct importer {
	relic_t			*ctx;
	const import_plan_t	*plan;
	relic_staging_t		 staging;
	relic_table_reader_t	 reader;
	relic_dom_t		 tuple[RELIC_MAX_ARITY];
	relic_pred_id_t		 pred;
	uint16_t		 kept;
	bool			 header_pending;
};

static void	 advance_location(relic_location_t *where, unsigned char byte);
static enum relic_table_event_kind append_field_byte(
		    relic_table_reader_t *reader, unsigned char byte,
		    relic_table_event_t *event);
static const char *describe_field(const char *bytes, size_t length,
		    char *buffer, size_t size);
static enum relic_table_event_kind end_field(relic_table_reader_t *reader,
		    enum relic_table_state next, relic_table_event_t *event);
static enum relic_table_event_kind end_of_input(relic_table_reader_t *reader,
		    relic_table_event_t *event);
static enum relic_table_event_kind end_record(relic_table_reader_t *reader,
		    relic_table_event_t *event);
static bool	 extra_field_begun(const relic_table_reader_t *reader);
static enum import_progress feed_chunk(importer_t *importer,
		    const char *source_name, FILE *stream, char *chunk);
static bool	 field_value(relic_t *ctx, enum relic_column column,
		    const relic_table_event_t *event, relic_dom_t *valuep);
static int	 import_named_file(relic_t *ctx, const char *path,
		    const import_plan_t *plan);
static bool	 import_plan_init(relic_t *ctx, const relic_import_t *import,
		    import_plan_t *plan);
static void	 import_plan_reset(import_plan_t *plan);
static int	 import_stream(relic_t *ctx, const char *source_name,
		    FILE *stream, const import_plan_t *plan);
static bool	 is_delimiter(const relic_table_reader_t *reader,
		    unsigned char byte);
static size_t	 kept_columns(const enum relic_column *columns,
		    size_t column_count);
static enum relic_table_event_kind mistake(relic_table_reader_t *reader,
		    enum relic_table_mistake which,
		    const relic_location_t *where, unsigned char byte,
		    relic_table_event_t *event);
static int	 read_table(importer_t *importer, const char *source_name,
		    FILE *stream);
static void	 report_mistake(relic_t *ctx, const import_plan_t *plan,
		    const relic_table_event_t *event);
static unsigned char separator_of(enum relic_format format);
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

enum relic_table_event_kind
relic_table_next(relic_table_reader_t *reader, relic_table_event_t *event)
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
	if (is_delimiter(reader, byte))
		return (step_delimiter(reader, byte, here, event));

	if (reader->format == RELIC_FORMAT_CSV && byte == '"')
		return (mistake(reader, RELIC_TABLE_QUOTE_IN_UNQUOTED, here, 0,
		    event));

	if (reader->format == RELIC_FORMAT_TSV && byte == '\\') {
		reader->state = RELIC_TABLE_AFTER_BACKSLASH;
		return (RELIC_TABLE_MORE);
	}

	reader->state = RELIC_TABLE_IN_FIELD;

	return (append_field_byte(reader, byte, event));
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
 * End the field being read, emitting its bytes, and go on in `next`.  The
 * field's length is taken into the event before it is cleared, so the bytes
 * the event points at stay as they are until the next byte is appended,
 * which cannot happen before the caller has seen the event.  Before the
 * first byte of any field the buffer does not exist, and an empty field's
 * bytes are then an empty string rather than NULL.
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
 * rarely, so every other byte costs a comparison and a store.
 */
static enum relic_table_event_kind
append_field_byte(relic_table_reader_t *reader, unsigned char byte,
    relic_table_event_t *event)
{
	if (reader->field_length == reader->field_capacity) {
		char *field;

		field = relic_grow_array(reader->field,
		    &reader->field_capacity, reader->field_length + 1,
		    sizeof(*reader->field));
		if (field == NULL) {
			bzero(event, sizeof(*event));
			event->kind = RELIC_TABLE_NO_MEMORY;
			event->where = reader->where;
			return (stop(reader, event));
		}
		reader->field = field;
	}

	reader->field[reader->field_length] = (char)byte;
	reader->field_length++;

	return (RELIC_TABLE_MORE);
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
	import_plan_t plan;
	int status;

	if (relic_enter(ctx) != 0)
		return (-1);

	status = -1;
	if (stream == NULL) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
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
	import_plan_t plan;
	int status;

	if (relic_enter(ctx) != 0)
		return (-1);

	status = -1;
	if (path == NULL) {
		relic_diagf(ctx, RELIC_ERROR, NULL, "no path was given");
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
 * exactly what was checked.  The predicate's name is interned here rather
 * than copied: that is all an import needs of it, and like the values a
 * table's fields intern, it stays interned if the import fails.
 */
static bool
import_plan_init(relic_t *ctx, const relic_import_t *import,
    import_plan_t *plan)
{
	bzero(plan, sizeof(*plan));

	if (!validate_import(ctx, import))
		return (false);

	if (relic_symbol_intern(ctx, import->predicate,
	    strlen(import->predicate), &plan->predicate_name) != 0)
		return (false);

	plan->columns = reallocarray(NULL, import->column_count,
	    sizeof(*plan->columns));
	if (plan->columns == NULL) {
		relic_diag_no_memory(ctx);
		return (false);
	}
	bcopy(import->columns, plan->columns,
	    import->column_count * sizeof(*plan->columns));

	plan->column_count = import->column_count;
	plan->arity = kept_columns(plan->columns, plan->column_count);
	plan->format = import->format;
	plan->header = (import->flags & RELIC_IMPORT_HEADER) != 0;

	return (true);
}

static void
import_plan_reset(import_plan_t *plan)
{
	free(plan->columns);
	bzero(plan, sizeof(*plan));
}

/*
 * Open, import and close the file at `path`, naming it by its path.
 */
static int
import_named_file(relic_t *ctx, const char *path, const import_plan_t *plan)
{
	FILE *stream;
	int status;

	stream = fopen(path, "r");
	if (stream == NULL) {
		relic_diagf(ctx, RELIC_ERROR, NULL, "cannot open %s: %s", path,
		    strerror(errno));
		return (-1);
	}

	status = import_stream(ctx, path, stream, plan);

	if (fclose(stream) != 0 && status == 0) {
		relic_diagf(ctx, RELIC_ERROR, NULL, "cannot close %s: %s", path,
		    strerror(errno));
		status = -1;
	}

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
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "no import description was given");
		return (false);
	}

	if (import->predicate == NULL ||
	    !relic_lex_is_predicate_name(import->predicate,
	    strlen(import->predicate))) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "a predicate name is a lowercase letter followed by "
		    "letters, digits and underscores, and is not `not`");
		return (false);
	}

	if (import->columns == NULL || import->column_count == 0) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "an import needs at least one column");
		return (false);
	}

	for (i = 0; i < import->column_count; i++) {
		if (import->columns[i] < RELIC_COLUMN_SYMBOL ||
		    import->columns[i] > RELIC_COLUMN_SKIP) {
			relic_diagf(ctx, RELIC_ERROR, NULL,
			    "column %zu of the import has no type", i + 1);
			return (false);
		}
	}

	if (kept_columns(import->columns, import->column_count) >
	    RELIC_MAX_ARITY) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "an import may keep at most %d columns", RELIC_MAX_ARITY);
		return (false);
	}

	if (import->format != RELIC_FORMAT_CSV &&
	    import->format != RELIC_FORMAT_TSV) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "the import's format is neither csv nor tsv");
		return (false);
	}

	if ((import->flags & ~RELIC_IMPORT_HEADER) != 0) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "the import's flags hold bits this library does not "
		    "define: 0x%x", import->flags & ~RELIC_IMPORT_HEADER);
		return (false);
	}

	return (true);
}

/*
 * Import a table from a stream by `plan`.
 *
 * The predicate is interned before the table is read, and so before any of
 * its values, as a load interns a fact's predicate when it stages the fact.
 * Like the values its fields intern, it stays interned if the import fails.
 */
static int
import_stream(relic_t *ctx, const char *source_name, FILE *stream,
    const import_plan_t *plan)
{
	importer_t importer;
	const char *name;
	int status;

	name = relic_source_name(ctx, source_name);
	if (name == NULL)
		return (-1);

	bzero(&importer, sizeof(importer));
	importer.ctx = ctx;
	importer.plan = plan;
	importer.header_pending = plan->header;

	if (relic_pred_intern(ctx, plan->predicate_name, (uint16_t)plan->arity,
	    NULL, &importer.pred) != 0)
		return (-1);

	relic_staging_init(&importer.staging, ctx);
	relic_table_reader_init(&importer.reader, plan->format,
	    plan->column_count, name);

	status = read_table(&importer, name, stream);
	if (status == 0)
		status = relic_staging_commit(&importer.staging);

	relic_table_reader_reset(&importer.reader);
	relic_staging_reset(&importer.staging);

	return (status);
}

/*
 * Read the whole table, feeding the stream to the reader a chunk at a time
 * and handing the reader's events to the importer in order.  Returns 0 when
 * the table ended with every record staged, or -1 having reported the first
 * mistake, a failed read, or memory.
 */
static int
read_table(importer_t *importer, const char *source_name, FILE *stream)
{
	relic_table_event_t event;
	enum import_progress progress;
	char *chunk;

	chunk = malloc(IMPORT_READ_CHUNK);
	if (chunk == NULL) {
		relic_diag_no_memory(importer->ctx);
		return (-1);
	}

	do {
		if (relic_table_next(&importer->reader, &event) ==
		    RELIC_TABLE_MORE)
			progress = feed_chunk(importer, source_name, stream,
			    chunk);
		else
			progress = take_event(importer, &event);
	} while (progress == IMPORT_READ_ON);

	free(chunk);

	return (progress == IMPORT_TABLE_ENDED ? 0 : -1);
}

/*
 * Read the next chunk of the stream into `chunk` and feed it to the reader.
 * Reads on, or fails having reported a failed read.
 *
 * A short read with no error on the stream is its end, and the chunk it
 * filled is the table's last.  errno is cleared before the read, because a
 * stream whose error indicator was already set can return short without
 * touching it, and the message would then name whatever an earlier call
 * left behind.  A stream that fails without saying why, such as one whose
 * read function sets no errno, is reported as an input error rather than as
 * no error at all.
 */
static enum import_progress
feed_chunk(importer_t *importer, const char *source_name, FILE *stream,
    char *chunk)
{
	size_t got;
	int read_errno;

	errno = 0;
	got = fread(chunk, 1, IMPORT_READ_CHUNK, stream);
	read_errno = errno;

	if (got < IMPORT_READ_CHUNK && ferror(stream)) {
		relic_diagf(importer->ctx, RELIC_ERROR, NULL,
		    "cannot read %s: %s", source_name,
		    strerror(read_errno != 0 ? read_errno : EIO));
		return (IMPORT_FAILED);
	}

	relic_table_feed(&importer->reader, chunk, got,
	    got < IMPORT_READ_CHUNK);

	return (IMPORT_READ_ON);
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
 * program's constant is.  Returns false having reported a field the type
 * refuses, or memory.
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

	switch (column) {
	case RELIC_COLUMN_SYMBOL:
		if (!relic_lex_is_symbol(event->bytes, event->length)) {
			relic_diagf(ctx, RELIC_ERROR, &event->where,
			    "expected a symbol, found %s",
			    describe_field(event->bytes, event->length,
			    described, sizeof(described)));
			return (false);
		}
		return (relic_value_intern_bytes(ctx, RELIC_SYMBOL,
		    event->bytes, event->length, valuep) == 0);
	case RELIC_COLUMN_STRING:
		return (relic_value_intern_bytes(ctx, RELIC_STRING,
		    event->bytes, event->length, valuep) == 0);
	case RELIC_COLUMN_INTEGER:
		switch (relic_lex_integer(event->bytes, event->length,
		    &integer)) {
		case RELIC_INTEGER_VALID:
			return (relic_value_intern_integer(ctx, integer,
			    valuep) == 0);
		case RELIC_INTEGER_MALFORMED:
			relic_diagf(ctx, RELIC_ERROR, &event->where,
			    "expected an integer, found %s",
			    describe_field(event->bytes, event->length,
			    described, sizeof(described)));
			return (false);
		case RELIC_INTEGER_OUT_OF_RANGE:
			relic_diagf(ctx, RELIC_ERROR, &event->where,
			    "integer out of range: %s",
			    describe_field(event->bytes, event->length,
			    described, sizeof(described)));
			return (false);
		}
		break;
	case RELIC_COLUMN_SKIP:
		break;
	}

	/*
	 * Unreachable: the plan holds only the four types, and the caller
	 * drops a skipped field.
	 */
	assert(!"a field typed by no kept column");
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

	relic_diagf(importer->ctx, RELIC_ERROR, &event->where,
	    "expected a header, found the end of the input");

	return (IMPORT_FAILED);
}

/*
 * Report a mistake the reader found in the table's structure.
 */
static void
report_mistake(relic_t *ctx, const import_plan_t *plan,
    const relic_table_event_t *event)
{
	char described[RELIC_BYTE_TEXT_MAX];
	const char *plural;

	plural = plan->column_count == 1 ? "" : "s";

	switch (event->mistake) {
	case RELIC_TABLE_TOO_MANY_FIELDS:
		relic_diagf(ctx, RELIC_ERROR, &event->where,
		    "expected %zu field%s, found more than %zu",
		    plan->column_count, plural, plan->column_count);
		break;
	case RELIC_TABLE_TOO_FEW_FIELDS:
		relic_diagf(ctx, RELIC_ERROR, &event->where,
		    "expected %zu field%s, found %zu", plan->column_count,
		    plural, event->fields_found);
		break;
	case RELIC_TABLE_UNKNOWN_ESCAPE:
		relic_diagf(ctx, RELIC_ERROR, &event->where,
		    "unknown escape: a backslash followed by %s",
		    relic_diag_byte(event->byte, described, sizeof(described)));
		break;
	case RELIC_TABLE_ESCAPE_AT_END:
		relic_diagf(ctx, RELIC_ERROR, &event->where,
		    "unknown escape: a backslash at the end of the input");
		break;
	case RELIC_TABLE_STRAY_RETURN:
		relic_diagf(ctx, RELIC_ERROR, &event->where,
		    "carriage return inside a field");
		break;
	case RELIC_TABLE_QUOTE_IN_UNQUOTED:
		relic_diagf(ctx, RELIC_ERROR, &event->where,
		    "quote inside an unquoted field");
		break;
	case RELIC_TABLE_AFTER_CLOSING_QUOTE:
		relic_diagf(ctx, RELIC_ERROR, &event->where,
		    "expected ',' or the end of the line after a closing "
		    "quote, found %s",
		    relic_diag_byte(event->byte, described, sizeof(described)));
		break;
	case RELIC_TABLE_UNTERMINATED_QUOTE:
		relic_diagf(ctx, RELIC_ERROR, &event->where,
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
