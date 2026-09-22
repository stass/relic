/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cmd.h"
#include "compat.h"

/*
 * Writing query results.
 *
 * Every format writes one row per answer and each value according to its
 * type.  The text format writes a value the way a program would: a symbol as
 * its bytes, an integer in decimal, and a string as a quoted literal with its
 * control characters escaped, C1 included, so a symbol and a string never
 * look alike and a program cannot send a terminal escape sequence to whoever
 * reads its results.  csv and tsv are machine formats and write a value's
 * bytes, with only the quoting or escaping their own separators need.  json
 * writes integers as numbers and everything else as JSON strings, a string
 * value keeping its quoted literal inside the JSON string, so that a leading
 * quote marks a string and any bytes survive in a document that must be
 * UTF-8.
 *
 * The results of successive queries are told apart by a blank line before
 * every query's results but the first in text, csv and tsv, and by each
 * query's being a JSON document of its own in json.  What a query writes
 * first -- that separator, or the opening bracket -- is written only once the
 * query has succeeded, which the library guarantees by delivering no row
 * before the whole answer is known.  A query that fails therefore writes
 * nothing at all.
 *
 * A write that fails is found by the stream's error indicator after each
 * row, and reported once.
 */

/* Enough for any int64_t in decimal, its sign and a terminator. */
#define	INTEGER_TEXT_MAX	24

/*
 * Enough for one piece of a string literal and a terminator: the longest is
 * a C1 control, two \xHH escapes.
 */
#define	LITERAL_PIECE_MAX	16

/*
 * Where a string literal is being written: on its own, as the text format
 * writes it, or inside a JSON string, where the literal's own quotes and
 * backslashes must be escaped once more.
 */
enum literal_context {
	LITERAL_BARE,
	LITERAL_IN_JSON
};

static void	 begin_query(output_t *out);
static int	 compare_rows(const output_t *out, const relic_value_t *left,
		    const relic_value_t *right);
static bool	 csv_needs_quotes(const char *bytes, size_t length);
static void	 end_query(output_t *out);
static char	 field_separator(enum output_format format);
static int	 hold_row(output_t *out, const relic_value_t *values,
		    size_t count);
static void	 merge_sort(const output_t *out, const relic_value_t **rows,
		    const relic_value_t **scratch, size_t count);
static size_t	 literal_piece(const unsigned char *bytes, size_t available,
		    char *piece, size_t size);
static void	 put_literal(output_t *out, const char *bytes, size_t length,
		    enum literal_context context);
static void	 put_literal_text(output_t *out, const char *text,
		    enum literal_context context);
static int	 receive_row(void *context, const relic_value_t *values,
		    size_t count);
static void	 release_held_rows(output_t *out);
static bool	 stream_failed(output_t *out);
static size_t	 utf8_sequence_length(const unsigned char *bytes,
		    size_t available);
static const char *value_bytes(const output_t *out,
		    const relic_value_t *value, size_t *lengthp);
static void	 write_csv_value(output_t *out, const relic_value_t *value);
static int	 write_sorted_rows(output_t *out);
static void	 write_integer(output_t *out, int64_t integer);
static void	 write_json_string(output_t *out, const char *bytes,
		    size_t length);
static void	 write_json_value(output_t *out, const relic_value_t *value);
static void	 write_row(output_t *out, const relic_value_t *values,
		    size_t count);
static void	 write_text_value(output_t *out, const relic_value_t *value);
static void	 write_tsv_value(output_t *out, const relic_value_t *value);
static void	 write_value(output_t *out, const relic_value_t *value);

int
output_format_named(const char *name, enum output_format *formatp)
{
	static const struct {
		const char		*name;
		enum output_format	 format;
	} formats[] = {
		{ "csv", OUTPUT_CSV },
		{ "json", OUTPUT_JSON },
		{ "text", OUTPUT_TEXT },
		{ "tsv", OUTPUT_TSV }
	};
	size_t i;

	for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (strcmp(name, formats[i].name) == 0) {
			*formatp = formats[i].format;
			return (0);
		}
	}

	return (-1);
}

void
output_init(output_t *out, const char *program_name, relic_t *ctx,
    FILE *stream, const char *name, enum output_format format, bool sorted)
{
	bzero(out, sizeof(*out));
	out->ctx = ctx;
	out->stream = stream;
	out->name = name;
	out->program_name = program_name;
	out->format = format;
	out->sorted = sorted;
}

/*
 * Whether a write to the output has failed, reporting it the first time.
 *
 * The stream's error indicator is sticky, so checking it after a row covers
 * every write the row made, and errno still holds what the failing write
 * left there.
 */
static bool
stream_failed(output_t *out)
{
	if (out->write_failed)
		return (true);

	if (!ferror(out->stream))
		return (false);

	report_error(out->program_name, "cannot write %s: %s", out->name,
	    strerror(errno));
	out->write_failed = true;

	return (true);
}

/*
 * The bytes a symbol or string value names.  An identifier the context does
 * not know -- which a query result never holds -- reads as no bytes rather
 * than as a fault.
 */
static const char *
value_bytes(const output_t *out, const relic_value_t *value, size_t *lengthp)
{
	const char *bytes;

	bytes = relic_string(out->ctx, value->v.id, lengthp);
	if (bytes == NULL) {
		*lengthp = 0;
		return ("");
	}

	return (bytes);
}

static void
write_integer(output_t *out, int64_t integer)
{
	char text[INTEGER_TEXT_MAX];

	snprintf(text, sizeof(text), "%" PRId64, integer);
	fputs(text, out->stream);
}

/*
 * The length of the valid UTF-8 sequence that begins at `bytes`, or zero when
 * none does.
 *
 * Valid means what RFC 3629 means: the lead byte announces a length the
 * continuation bytes fill, the code point is written in the fewest bytes
 * that can hold it, and it is neither a surrogate nor beyond U+10FFFF.
 */
static size_t
utf8_sequence_length(const unsigned char *bytes, size_t available)
{
	uint32_t code_point;
	uint32_t smallest;
	size_t length;
	size_t i;

	if (bytes[0] < 0x80)
		return (1);

	if ((bytes[0] & 0xe0) == 0xc0) {
		length = 2;
		code_point = bytes[0] & 0x1f;
		smallest = 0x80;
	} else if ((bytes[0] & 0xf0) == 0xe0) {
		length = 3;
		code_point = bytes[0] & 0x0f;
		smallest = 0x800;
	} else if ((bytes[0] & 0xf8) == 0xf0) {
		length = 4;
		code_point = bytes[0] & 0x07;
		smallest = 0x10000;
	} else {
		return (0);
	}

	if (length > available)
		return (0);

	for (i = 1; i < length; i++) {
		if ((bytes[i] & 0xc0) != 0x80)
			return (0);
		code_point = (code_point << 6) | (bytes[i] & 0x3f);
	}

	if (code_point < smallest || code_point > 0x10ffff ||
	    (code_point >= 0xd800 && code_point <= 0xdfff))
		return (0);

	return (length);
}

/*
 * The next piece of a string literal: how the character that begins at
 * `bytes` is written, into `piece`, and how many bytes it took.
 *
 * A backslash, a double quote, newline, tab, carriage return and NUL have
 * escapes of their own.  Every other byte that is not part of a printable
 * character is written as \xHH: the C0 controls and DEL; a byte that does
 * not begin a valid UTF-8 sequence; and both bytes of a C1 control, U+0080 to
 * U+009F, which some terminals obey as the start of an escape sequence just
 * as they do ESC.  Everything else -- printable ASCII, or a whole UTF-8
 * character above the C1 range -- is copied.
 *
 * \xHH is an escape the language already has, so the literal still spells
 * exactly the bytes the string holds, and it is always valid UTF-8 holding no
 * control character, which is what makes it safe both to print and to embed
 * in a JSON document.
 *
 * No piece that is copied holds a backslash or a double quote, since those
 * two always take their escapes; put_literal_text() relies on that.
 */
static size_t
literal_piece(const unsigned char *bytes, size_t available, char *piece,
    size_t size)
{
	size_t length;

	switch (bytes[0]) {
	case '\\':
		strlcpy(piece, "\\\\", size);
		return (1);
	case '"':
		strlcpy(piece, "\\\"", size);
		return (1);
	case '\n':
		strlcpy(piece, "\\n", size);
		return (1);
	case '\t':
		strlcpy(piece, "\\t", size);
		return (1);
	case '\r':
		strlcpy(piece, "\\r", size);
		return (1);
	case '\0':
		strlcpy(piece, "\\0", size);
		return (1);
	default:
		break;
	}

	length = utf8_sequence_length(bytes, available);

	if (length == 2 && bytes[0] == 0xc2 && bytes[1] < 0xa0) {
		snprintf(piece, size, "\\x%02x\\x%02x",
		    (unsigned int)bytes[0], (unsigned int)bytes[1]);
		return (2);
	}

	if (length == 0 || bytes[0] < 0x20 || bytes[0] == 0x7f) {
		snprintf(piece, size, "\\x%02x", (unsigned int)bytes[0]);
		return (1);
	}

	bcopy(bytes, piece, length);
	piece[length] = '\0';

	return (length);
}

/*
 * Write text that is part of a string literal.  Inside a JSON string, the
 * literal's quotes and backslashes are escaped once more; nothing else needs
 * to be, because a literal holds no control character and only valid UTF-8.
 */
static void
put_literal_text(output_t *out, const char *text,
    enum literal_context context)
{
	size_t i;

	for (i = 0; text[i] != '\0'; i++) {
		if (context == LITERAL_IN_JSON &&
		    (text[i] == '"' || text[i] == '\\'))
			fputc('\\', out->stream);
		fputc(text[i], out->stream);
	}
}

/*
 * Write a string value as a quoted literal, the way a program would write it,
 * piece by piece.
 */
static void
put_literal(output_t *out, const char *bytes, size_t length,
    enum literal_context context)
{
	const unsigned char *unsigned_bytes;
	char piece[LITERAL_PIECE_MAX];
	size_t i;

	unsigned_bytes = (const unsigned char *)bytes;

	put_literal_text(out, "\"", context);

	i = 0;
	while (i < length) {
		i += literal_piece(&unsigned_bytes[i], length - i, piece,
		    sizeof(piece));
		put_literal_text(out, piece, context);
	}

	put_literal_text(out, "\"", context);
}

static void
write_text_value(output_t *out, const relic_value_t *value)
{
	const char *bytes;
	size_t length;

	if (value->type == RELIC_INTEGER) {
		write_integer(out, value->v.integer);
		return;
	}

	bytes = value_bytes(out, value, &length);

	if (value->type == RELIC_STRING)
		put_literal(out, bytes, length, LITERAL_BARE);
	else
		fwrite(bytes, 1, length, out->stream);
}

/*
 * Whether a csv field must be quoted: when it holds a comma, a double quote,
 * a carriage return or a newline, or is empty.  An empty field is quoted so
 * that a record of one empty string is not an empty line, which some readers
 * skip.
 */
static bool
csv_needs_quotes(const char *bytes, size_t length)
{
	size_t i;

	if (length == 0)
		return (true);

	for (i = 0; i < length; i++) {
		switch (bytes[i]) {
		case ',':
		case '"':
		case '\r':
		case '\n':
			return (true);
		default:
			break;
		}
	}

	return (false);
}

/*
 * A csv field: a value's bytes, quoted when they must be, with a double
 * quote inside a quoted field doubled.
 */
static void
write_csv_value(output_t *out, const relic_value_t *value)
{
	const char *bytes;
	size_t length;
	size_t i;

	if (value->type == RELIC_INTEGER) {
		write_integer(out, value->v.integer);
		return;
	}

	bytes = value_bytes(out, value, &length);

	if (!csv_needs_quotes(bytes, length)) {
		fwrite(bytes, 1, length, out->stream);
		return;
	}

	fputc('"', out->stream);
	for (i = 0; i < length; i++) {
		if (bytes[i] == '"')
			fputc('"', out->stream);
		fputc(bytes[i], out->stream);
	}
	fputc('"', out->stream);
}

/*
 * A tsv field: a value's bytes, with a backslash, tab, newline and carriage
 * return escaped so that neither separator can appear inside a field.
 */
static void
write_tsv_value(output_t *out, const relic_value_t *value)
{
	const char *bytes;
	size_t length;
	size_t i;

	if (value->type == RELIC_INTEGER) {
		write_integer(out, value->v.integer);
		return;
	}

	bytes = value_bytes(out, value, &length);

	for (i = 0; i < length; i++) {
		switch (bytes[i]) {
		case '\\':
			fputs("\\\\", out->stream);
			break;
		case '\t':
			fputs("\\t", out->stream);
			break;
		case '\n':
			fputs("\\n", out->stream);
			break;
		case '\r':
			fputs("\\r", out->stream);
			break;
		default:
			fputc(bytes[i], out->stream);
			break;
		}
	}
}

/*
 * A JSON string holding a symbol's bytes.
 *
 * A symbol is an identifier, so its bytes need no escaping, but the document
 * must be valid JSON whatever a symbol holds: a quote, a backslash and a
 * control byte are escaped, and a byte that does not begin a valid UTF-8
 * sequence becomes U+FFFD, since a JSON string can carry only characters.
 */
static void
write_json_string(output_t *out, const char *bytes, size_t length)
{
	const unsigned char *unsigned_bytes;
	size_t sequence;
	size_t i;

	unsigned_bytes = (const unsigned char *)bytes;

	fputc('"', out->stream);

	i = 0;
	while (i < length) {
		if (bytes[i] == '"' || bytes[i] == '\\') {
			fputc('\\', out->stream);
			fputc(bytes[i], out->stream);
			i++;
			continue;
		}

		if (unsigned_bytes[i] < 0x20) {
			fprintf(out->stream, "\\u%04x",
			    (unsigned int)unsigned_bytes[i]);
			i++;
			continue;
		}

		sequence = utf8_sequence_length(&unsigned_bytes[i], length - i);
		if (sequence == 0) {
			fputs("\\ufffd", out->stream);
			i++;
			continue;
		}

		fwrite(&bytes[i], 1, sequence, out->stream);
		i += sequence;
	}

	fputc('"', out->stream);
}

static void
write_json_value(output_t *out, const relic_value_t *value)
{
	const char *bytes;
	size_t length;

	if (value->type == RELIC_INTEGER) {
		write_integer(out, value->v.integer);
		return;
	}

	bytes = value_bytes(out, value, &length);

	if (value->type == RELIC_STRING) {
		fputc('"', out->stream);
		put_literal(out, bytes, length, LITERAL_IN_JSON);
		fputc('"', out->stream);
		return;
	}

	write_json_string(out, bytes, length);
}

static void
write_value(output_t *out, const relic_value_t *value)
{
	switch (out->format) {
	case OUTPUT_TEXT:
		write_text_value(out, value);
		break;
	case OUTPUT_CSV:
		write_csv_value(out, value);
		break;
	case OUTPUT_TSV:
		write_tsv_value(out, value);
		break;
	case OUTPUT_JSON:
		write_json_value(out, value);
		break;
	}
}

/*
 * Write what comes before a query's first row: a blank line after an earlier
 * query's results, or the bracket that opens a JSON document.  Called when
 * the first row is written, or when the query ends with none, so that a
 * query that fails writes nothing.
 */
static void
begin_query(output_t *out)
{
	if (out->begun)
		return;
	out->begun = true;

	if (out->format == OUTPUT_JSON)
		fputc('[', out->stream);
	else if (out->queries_written > 0)
		fputc('\n', out->stream);
}

/*
 * Write what comes after a query's last row: the bracket that closes a JSON
 * document, on a line of its own when there were rows.
 */
static void
end_query(output_t *out)
{
	begin_query(out);

	if (out->format != OUTPUT_JSON)
		return;

	if (out->rows_written > 0)
		fputs("\n]\n", out->stream);
	else
		fputs("]\n", out->stream);
}

static char
field_separator(enum output_format format)
{
	switch (format) {
	case OUTPUT_CSV:
	case OUTPUT_JSON:
		return (',');
	case OUTPUT_TSV:
		return ('\t');
	case OUTPUT_TEXT:
		break;
	}

	return (' ');
}

/*
 * Write one row.  A JSON row goes on a line of its own, indented, and every
 * row but the first ends the line of the row before it with a comma.
 */
static void
write_row(output_t *out, const relic_value_t *values, size_t count)
{
	size_t i;

	begin_query(out);

	if (out->format == OUTPUT_JSON)
		fputs(out->rows_written == 0 ? "\n  [" : ",\n  [",
		    out->stream);

	for (i = 0; i < count; i++) {
		if (i > 0)
			fputc(field_separator(out->format), out->stream);
		write_value(out, &values[i]);
	}

	if (out->format == OUTPUT_JSON)
		fputc(']', out->stream);
	else
		fputc('\n', out->stream);

	out->rows_written++;
}

/*
 * Keep a copy of a row for sorting.  Returns 0, or -1 having reported that
 * memory ran out.
 *
 * The values are copied because the library lends a row only for the
 * duration of the call.  A symbol's or string's bytes are not copied: a value
 * refers to them by identifier, and they belong to the context, which
 * outlives the output.
 */
static int
hold_row(output_t *out, const relic_value_t *values, size_t count)
{
	relic_value_t *held;
	size_t capacity;
	size_t needed;

	if (out->held_count == 0)
		out->width = count;

	/*
	 * Room for one more row, in values.  Both the count and the doubling
	 * are checked before they are formed, since a wrapped size would be
	 * answered with an array smaller than what is copied into it.
	 */
	if (count != 0 && out->held_count + 1 > SIZE_MAX / count)
		goto exhausted;
	needed = (out->held_count + 1) * count;

	/*
	 * The array is allocated even for rows of no values, so that every
	 * held row can be pointed at: a pointer formed from a null array, even
	 * at offset zero, is not one C defines.
	 */
	if (needed > out->held_capacity || out->held == NULL) {
		capacity = out->held_capacity == 0 ? 64 : out->held_capacity;
		while (capacity < needed) {
			if (capacity > SIZE_MAX / 2)
				goto exhausted;
			capacity *= 2;
		}

		held = reallocarray(out->held, capacity, sizeof(*held));
		if (held == NULL)
			goto exhausted;
		out->held = held;
		out->held_capacity = capacity;
	}

	if (count > 0)
		memcpy(&out->held[out->held_count * count], values,
		    count * sizeof(*values));
	out->held_count++;

	return (0);

exhausted:
	report_error(out->program_name, "out of memory");
	return (-1);
}

/*
 * The order -s sorts by: column by column, each in the library's total order
 * over values.
 */
static int
compare_rows(const output_t *out, const relic_value_t *left,
    const relic_value_t *right)
{
	size_t column;
	int order;

	for (column = 0; column < out->width; column++) {
		order = relic_value_compare(out->ctx, &left[column],
		    &right[column]);
		if (order != 0)
			return (order);
	}

	return (0);
}

/*
 * Sort an array of rows, using `scratch`, which has room for as many.
 *
 * A merge sort rather than qsort(3), because the order needs the context to
 * read a string's bytes and qsort's comparator receives no argument to carry
 * it; reaching the context through a global would make the sort the one
 * piece of the tool that is not reentrant.  The recursion is as deep as the
 * logarithm of the row count.
 */
static void
merge_sort(const output_t *out, const relic_value_t **rows,
    const relic_value_t **scratch, size_t count)
{
	size_t half;
	size_t left;
	size_t right;
	size_t merged;

	if (count < 2)
		return;

	half = count / 2;
	merge_sort(out, rows, scratch, half);
	merge_sort(out, rows + half, scratch, count - half);

	left = 0;
	right = half;
	merged = 0;
	while (left < half && right < count) {
		if (compare_rows(out, rows[right], rows[left]) < 0)
			scratch[merged++] = rows[right++];
		else
			scratch[merged++] = rows[left++];
	}
	while (left < half)
		scratch[merged++] = rows[left++];
	while (right < count)
		scratch[merged++] = rows[right++];

	memcpy(rows, scratch, count * sizeof(*rows));
}

/*
 * Sort the held rows and write them.  Returns 0, or -1 having reported that
 * memory ran out.
 *
 * The sort moves pointers to rows rather than the rows, so a row of any
 * width costs the same to move.
 */
static int
write_sorted_rows(output_t *out)
{
	const relic_value_t **order;
	const relic_value_t **scratch;
	size_t i;

	if (out->held_count == 0)
		return (0);

	order = reallocarray(NULL, out->held_count, sizeof(*order));
	scratch = reallocarray(NULL, out->held_count, sizeof(*scratch));
	if (order == NULL || scratch == NULL) {
		report_error(out->program_name, "out of memory");
		free(order);
		free(scratch);
		return (-1);
	}

	for (i = 0; i < out->held_count; i++)
		order[i] = &out->held[i * out->width];

	merge_sort(out, order, scratch, out->held_count);

	for (i = 0; i < out->held_count; i++)
		write_row(out, order[i], out->width);

	free(order);
	free(scratch);

	return (0);
}

static void
release_held_rows(output_t *out)
{
	free(out->held);
	out->held = NULL;
	out->held_count = 0;
	out->held_capacity = 0;
}

/*
 * The library's row callback.  Returns nonzero to stop the rows coming once
 * something has failed, since nothing more can usefully be done with them.
 */
static int
receive_row(void *context, const relic_value_t *values, size_t count)
{
	output_t *out;

	out = context;

	if (out->sorted) {
		if (hold_row(out, values, count) != 0) {
			out->hold_failed = true;
			return (1);
		}
		return (0);
	}

	write_row(out, values, count);

	return (stream_failed(out) ? 1 : 0);
}

/*
 * Run a query and write its results.
 *
 * Nothing is written for a query that fails, because the library delivers no
 * row until the whole answer is known and what comes before the first row is
 * written only with it.  When sorting, the rows are held as they arrive and
 * written once the last one has.
 */
int
output_query(output_t *out, relic_query_t *query)
{
	int status;

	out->begun = false;
	out->rows_written = 0;
	out->hold_failed = false;

	status = relic_query_run(out->ctx, query, receive_row, out);

	if (status == 0 && out->hold_failed)
		status = -1;

	if (status == 0 && out->sorted)
		status = write_sorted_rows(out);

	release_held_rows(out);

	if (status != 0)
		return (-1);

	end_query(out);
	out->queries_written++;

	return (stream_failed(out) ? -1 : 0);
}

int
output_close(output_t *out)
{
	bool failed;

	/*
	 * A failed flush sets the stream's error indicator, so the check that
	 * follows reports it, along with any earlier failure not yet reported.
	 */
	fflush(out->stream);
	failed = stream_failed(out);

	if (out->stream == stdout)
		return (failed ? -1 : 0);

	if (fclose(out->stream) != 0 && !failed) {
		report_error(out->program_name, "cannot close %s: %s",
		    out->name, strerror(errno));
		failed = true;
	}

	return (failed ? -1 : 0);
}
