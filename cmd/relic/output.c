/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

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
 * query's being a JSON document of its own in json.  An interactive session
 * at a terminal writes a prompt before each query, which tells them apart
 * already, so there the blank line is left out.  What a query writes
 * first -- that separator, or the opening bracket -- is written only with
 * the query's first row or once the query has succeeded, and the library
 * delivers no row until everything that can fail has succeeded.  A query
 * that fails therefore writes nothing at all.
 *
 * Everything is written into a buffer of the output's own and reaches the
 * stream a block of OUTPUT_BUFFER_SIZE bytes at a time, since a stdio call
 * per value takes and releases the stream's lock per value and costs more
 * than deriving the rows.  A block can end at any byte, inside a value or an
 * escape, and the stream receives the same bytes wherever it ends.  The
 * buffer is emptied when it fills and when a query's results end, so the
 * buffer is empty between queries: whatever is written after a query -- the
 * next query's plan, a diagnostic, -v's count, a prompt -- follows its
 * results, whoever writes it.
 *
 * A write that fails is reported once.  Nothing reaches the stream after it,
 * the rows of the query being written stop coming, and the query fails.
 *
 * An output with no stream attached writes nothing: its queries run and their
 * rows are counted and dropped, which is what -n asks for, and what .plan
 * does with the rows of the query it shows.
 */

/* The bytes gathered before they are handed to the stream. */
#define	OUTPUT_BUFFER_SIZE	(64 * 1024)

/* Enough for any int64_t in decimal, the longest being INT64_MIN. */
#define	INTEGER_TEXT_MAX	(sizeof("-9223372036854775808") - 1)

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

typedef struct row_order row_order_t;

/*
 * What -s sorts rows by: the context whose values they hold, which reads a
 * string's bytes, and the number of values in each.
 */
struct row_order {
	const relic_result_t	*result;
	size_t			 width;
};

static void	 begin_query(output_t *out);
static int	 compare_rows(const row_order_t *order,
		    const relic_value_t *left, const relic_value_t *right);
static int	 count_row(void *context, const relic_value_t *values,
		    size_t count);
static bool	 csv_needs_quotes(const char *bytes, size_t length);
static void	 end_query(output_t *out);
static char	 field_separator(enum output_format format);
static void	 flush_buffer(output_t *out);
static size_t	 format_decimal(int64_t integer,
		    char text[INTEGER_TEXT_MAX]);
static void	 merge_sort(const row_order_t *order,
		    const relic_value_t **rows, const relic_value_t **scratch,
		    size_t count);
static size_t	 literal_piece(const unsigned char *bytes, size_t available,
		    char *piece, size_t size);
static void	 put_byte(output_t *out, char byte);
static void	 put_bytes(output_t *out, const char *bytes, size_t length);
static void	 put_literal(output_t *out, const char *bytes, size_t length,
		    enum literal_context context);
static void	 put_literal_text(output_t *out, const char *text,
		    enum literal_context context);
static void	 put_text(output_t *out, const char *text);
static int	 receive_row(void *context, const relic_value_t *values,
		    size_t count);
static void	 report_write_failure(output_t *out, int error);
static const char *tsv_escape(char byte);
static size_t	 utf8_sequence_length(const unsigned char *bytes,
		    size_t available);
static const char *value_bytes(const output_t *out,
		    const relic_value_t *value, size_t *lengthp);
static int	 write_block(FILE *stream, const char *bytes, size_t length);
static int	 write_collected(output_t *out, relic_query_t *query);
static void	 write_csv_value(output_t *out, const relic_value_t *value);
static void	 write_integer(output_t *out, int64_t integer);
static void	 write_json_control(output_t *out, unsigned char byte);
static void	 write_json_string(output_t *out, const char *bytes,
		    size_t length);
static void	 write_json_value(output_t *out, const relic_value_t *value);
static void	 write_row(output_t *out, const relic_value_t *values,
		    size_t count);
static int	 write_sorted_rows(output_t *out,
		    const relic_result_t *result);
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
output_init(output_t *out, const char *program_name, relic_t *ctx)
{
	bzero(out, sizeof(*out));
	out->ctx = ctx;
	out->program_name = program_name;
	out->format = OUTPUT_TEXT;
	out->separated = true;
}

/*
 * The stream's own buffer is turned off, so that what other writers send
 * through stdio reaches the descriptor at once, in order with the blocks
 * flush_buffer() writes to it directly.  A stream that refuses keeps its
 * buffer, and flush_buffer() empties it before each block.
 */
int
output_attach(output_t *out, FILE *stream, const char *name,
    enum output_format format, bool sorted)
{
	char *buffer;

	buffer = malloc(OUTPUT_BUFFER_SIZE);
	if (buffer == NULL) {
		report_error(out->program_name, "out of memory");
		return (-1);
	}

	setvbuf(stream, NULL, _IONBF, 0);

	out->stream = stream;
	out->buffer = buffer;
	out->filled = 0;
	out->name = name;
	out->format = format;
	out->sorted = sorted;

	return (0);
}

/*
 * Report that a write to the stream failed, the first time one does, with
 * the reason `error` gives, an errno value, or none when it is zero.
 */
static void
report_write_failure(output_t *out, int error)
{
	if (out->write_failed)
		return;

	if (error != 0)
		report_error(out->program_name, "cannot write %s: %s",
		    out->name, strerror(error));
	else
		report_error(out->program_name, "cannot write %s", out->name);
	out->write_failed = true;
}

/*
 * Write the `length` bytes at `bytes` to the stream's descriptor, continuing
 * after a short write and retrying one interrupted before it wrote anything.
 * Returns 0, or the errno value of the write that failed.  A write that
 * accepts no bytes would be retried forever, and a flush may fail without
 * setting errno, so each fails as an I/O error.
 *
 * The bytes go to write(2) rather than fwrite(), which on an unbuffered
 * stream may split them into writes of BUFSIZ bytes.  Whatever stdio holds
 * for the stream is pushed first, so the bytes follow what other writers
 * sent before them; the stream is unbuffered, so that is nothing, unless
 * setvbuf() refused.
 */
static int
write_block(FILE *stream, const char *bytes, size_t length)
{
	ssize_t written;

	errno = 0;
	if (fflush(stream) != 0)
		return (errno != 0 ? errno : EIO);

	while (length > 0) {
		written = write(fileno(stream), bytes, length);
		if (written < 0 && errno == EINTR)
			continue;
		if (written < 0)
			return (errno);
		if (written == 0)
			return (EIO);

		bytes += written;
		length -= (size_t)written;
	}

	return (0);
}

/*
 * Hand the bytes the buffer holds to the stream, and empty the buffer
 * whether or not they were written: put_byte() and put_bytes() rely on a
 * flush leaving room.
 *
 * Once a write has failed nothing more reaches the stream: what was
 * gathered since is dropped here, and receive_row() or write_sorted_rows()
 * stops the rows of the query being written once the row in hand is
 * formatted, so a closed pipe whose SIGPIPE is ignored costs one failed
 * write.
 */
static void
flush_buffer(output_t *out)
{
	int error;

	if (out->filled > 0 && !out->write_failed) {
		error = write_block(out->stream, out->buffer, out->filled);
		if (error != 0)
			report_write_failure(out, error);
	}

	out->filled = 0;
}

static void
put_byte(output_t *out, char byte)
{
	if (out->filled == OUTPUT_BUFFER_SIZE)
		flush_buffer(out);

	out->buffer[out->filled] = byte;
	out->filled++;
}

/*
 * Append bytes of any length, flushing each time the buffer fills.
 */
static void
put_bytes(output_t *out, const char *bytes, size_t length)
{
	size_t room;
	size_t piece;

	while (length > 0) {
		if (out->filled == OUTPUT_BUFFER_SIZE)
			flush_buffer(out);

		room = OUTPUT_BUFFER_SIZE - out->filled;
		piece = length < room ? length : room;
		bcopy(bytes, out->buffer + out->filled, piece);
		out->filled += piece;

		bytes += piece;
		length -= piece;
	}
}

static void
put_text(output_t *out, const char *text)
{
	put_bytes(out, text, strlen(text));
}

/*
 * The bytes a symbol or string value names, read from the collected answer
 * being written when there is one and from the context otherwise.  An
 * identifier neither knows -- which no row holds -- reads as no bytes
 * rather than as a fault.
 */
static const char *
value_bytes(const output_t *out, const relic_value_t *value, size_t *lengthp)
{
	const char *bytes;

	if (out->collected != NULL)
		bytes = relic_result_string(out->collected, value->v.id,
		    lengthp);
	else
		bytes = relic_string(out->ctx, value->v.id, lengthp);
	if (bytes == NULL) {
		*lengthp = 0;
		return ("");
	}

	return (bytes);
}

/*
 * Write `integer` in decimal at the end of `text` and return the index its
 * first byte is at.  No terminator is written.
 *
 * The digits come least significant first, so they are written from the end
 * backwards.  The magnitude is taken in uint64_t, where negating INT64_MIN
 * is exact, since no int64_t holds its magnitude.
 */
static size_t
format_decimal(int64_t integer, char text[INTEGER_TEXT_MAX])
{
	uint64_t magnitude;
	size_t start;

	if (integer < 0)
		magnitude = 0 - (uint64_t)integer;
	else
		magnitude = (uint64_t)integer;

	start = INTEGER_TEXT_MAX;
	do {
		start--;
		text[start] = (char)('0' + magnitude % 10);
		magnitude /= 10;
	} while (magnitude > 0);

	if (integer < 0) {
		start--;
		text[start] = '-';
	}

	return (start);
}

static void
write_integer(output_t *out, int64_t integer)
{
	char text[INTEGER_TEXT_MAX];
	size_t start;

	start = format_decimal(integer, text);
	put_bytes(out, &text[start], INTEGER_TEXT_MAX - start);
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
			put_byte(out, '\\');
		put_byte(out, text[i]);
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
		put_bytes(out, bytes, length);
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
		put_bytes(out, bytes, length);
		return;
	}

	put_byte(out, '"');
	for (i = 0; i < length; i++) {
		if (bytes[i] == '"')
			put_byte(out, '"');
		put_byte(out, bytes[i]);
	}
	put_byte(out, '"');
}

/*
 * The escape a byte takes inside a tsv field, or NULL for a byte written as
 * it is: a backslash, tab, newline and carriage return are escaped, so that
 * neither separator can appear inside a field.
 */
static const char *
tsv_escape(char byte)
{
	switch (byte) {
	case '\\':
		return ("\\\\");
	case '\t':
		return ("\\t");
	case '\n':
		return ("\\n");
	case '\r':
		return ("\\r");
	default:
		return (NULL);
	}
}

/*
 * A tsv field: a value's bytes, each run of bytes that needs no escape
 * copied whole, and each byte that does replaced by its escape.
 */
static void
write_tsv_value(output_t *out, const relic_value_t *value)
{
	const char *bytes;
	const char *escape;
	size_t length;
	size_t run_start;
	size_t i;

	if (value->type == RELIC_INTEGER) {
		write_integer(out, value->v.integer);
		return;
	}

	bytes = value_bytes(out, value, &length);

	run_start = 0;
	for (i = 0; i < length; i++) {
		escape = tsv_escape(bytes[i]);
		if (escape == NULL)
			continue;

		put_bytes(out, &bytes[run_start], i - run_start);
		put_text(out, escape);
		run_start = i + 1;
	}
	put_bytes(out, &bytes[run_start], length - run_start);
}

/*
 * A control byte inside a JSON string, as the \u escape of its code point.
 */
static void
write_json_control(output_t *out, unsigned char byte)
{
	static const char hex_digits[] = "0123456789abcdef";

	put_text(out, "\\u00");
	put_byte(out, hex_digits[byte >> 4]);
	put_byte(out, hex_digits[byte & 0x0f]);
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

	put_byte(out, '"');

	i = 0;
	while (i < length) {
		if (bytes[i] == '"' || bytes[i] == '\\') {
			put_byte(out, '\\');
			put_byte(out, bytes[i]);
			i++;
			continue;
		}

		if (unsigned_bytes[i] < 0x20) {
			write_json_control(out, unsigned_bytes[i]);
			i++;
			continue;
		}

		sequence = utf8_sequence_length(&unsigned_bytes[i], length - i);
		if (sequence == 0) {
			put_text(out, "\\ufffd");
			i++;
			continue;
		}

		put_bytes(out, &bytes[i], sequence);
		i += sequence;
	}

	put_byte(out, '"');
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
		put_byte(out, '"');
		put_literal(out, bytes, length, LITERAL_IN_JSON);
		put_byte(out, '"');
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
		put_byte(out, '[');
	else if (out->separated && out->queries_written > 0)
		put_byte(out, '\n');
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
		put_text(out, "\n]\n");
	else
		put_text(out, "]\n");
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
		put_text(out, out->rows_written == 0 ? "\n  [" : ",\n  [");

	for (i = 0; i < count; i++) {
		if (i > 0)
			put_byte(out, field_separator(out->format));
		write_value(out, &values[i]);
	}

	if (out->format == OUTPUT_JSON)
		put_byte(out, ']');
	else
		put_byte(out, '\n');

	out->rows_written++;
}

/*
 * The order -s sorts by: column by column, each in the library's total order
 * over values, of the collected result the rows belong to.
 */
static int
compare_rows(const row_order_t *order, const relic_value_t *left,
    const relic_value_t *right)
{
	size_t column;
	int sign;

	for (column = 0; column < order->width; column++) {
		sign = relic_result_compare(order->result, &left[column],
		    &right[column]);
		if (sign != 0)
			return (sign);
	}

	return (0);
}

/*
 * Sort an array of rows, using `scratch`, which has room for as many.
 *
 * A merge sort rather than qsort(3), because the order needs the result to
 * read a string's bytes and qsort's comparator receives no argument to carry
 * it; reaching the result through a global would make the sort the one
 * piece of the tool that is not reentrant.  The recursion is as deep as the
 * logarithm of the row count.
 */
static void
merge_sort(const row_order_t *order, const relic_value_t **rows,
    const relic_value_t **scratch, size_t count)
{
	size_t half;
	size_t left;
	size_t right;
	size_t merged;

	if (count < 2)
		return;

	half = count / 2;
	merge_sort(order, rows, scratch, half);
	merge_sort(order, rows + half, scratch, count - half);

	left = 0;
	right = half;
	merged = 0;
	while (left < half && right < count) {
		if (compare_rows(order, rows[right], rows[left]) < 0)
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
 * Sort a collected answer and write it, stopping once a write has failed.
 * Returns 0, or -1 having reported that memory ran out or that the session
 * was interrupted while the rows were written, which is looked for before
 * each row, as the library looks before each row it delivers.
 *
 * The sort moves pointers to rows rather than the rows, so a row of any
 * width costs the same to move.
 */
static int
write_sorted_rows(output_t *out, const relic_result_t *result)
{
	row_order_t order;
	const relic_value_t **sorted;
	const relic_value_t **scratch;
	size_t rows;
	size_t i;
	int status;

	rows = relic_result_rows(result);
	if (rows == 0)
		return (0);

	sorted = reallocarray(NULL, rows, sizeof(*sorted));
	scratch = reallocarray(NULL, rows, sizeof(*scratch));
	if (sorted == NULL || scratch == NULL) {
		report_error(out->program_name, "out of memory");
		free(sorted);
		free(scratch);
		return (-1);
	}

	for (i = 0; i < rows; i++)
		sorted[i] = relic_result_row(result, i);

	order.result = result;
	order.width = relic_result_columns(result);
	merge_sort(&order, sorted, scratch, rows);

	out->collected = result;
	status = 0;
	for (i = 0; i < rows && !out->write_failed; i++) {
		if (repl_interrupted) {
			flush_buffer(out);
			report_error(out->program_name,
			    "interrupted while writing the rows");
			status = -1;
			break;
		}
		write_row(out, sorted[i], order.width);
	}
	out->collected = NULL;

	free(sorted);
	free(scratch);

	return (status);
}

/*
 * The library's row callback.  Returns nonzero to stop the rows coming once
 * a write has failed, since nothing more can usefully be done with them.
 */
static int
receive_row(void *context, const relic_value_t *values, size_t count)
{
	output_t *out;

	out = context;
	write_row(out, values, count);

	return (out->write_failed ? 1 : 0);
}

/*
 * Answer a query and write its rows sorted.  Returns 0, or -1 with the
 * failure reported, by the library when the query or the collection failed.
 */
static int
write_collected(output_t *out, relic_query_t *query)
{
	relic_result_t *result;
	int status;

	result = relic_query_collect(out->ctx, query);
	if (result == NULL)
		return (-1);

	status = write_sorted_rows(out, result);
	relic_result_destroy(result);

	return (status);
}

/*
 * The row callback of a query whose rows are counted and dropped.  Its
 * context is the count.
 */
static int
count_row(void *context, const relic_value_t *values RELIC_CMD_UNUSED,
    size_t count RELIC_CMD_UNUSED)
{
	size_t *rows;

	rows = context;
	(*rows)++;

	return (0);
}

int
output_discard(output_t *out, relic_query_t *query, size_t *rowsp)
{
	size_t rows;

	rows = 0;
	if (relic_query_run(out->ctx, query, count_row, &rows) != 0)
		return (-1);

	*rowsp = rows;

	return (0);
}

void
output_write_buffered(output_t *out)
{
	if (out->stream != NULL)
		flush_buffer(out);
}

bool
output_is_stdout(const output_t *out)
{
	return (out->stream == stdout);
}

/*
 * Results written to a file or a pipe keep the blank line even in a
 * prompting session: whoever reads them later has no prompt to go by.
 */
void
output_prompted(output_t *out)
{
	if (out->stream != NULL && isatty(fileno(out->stream)) == 1)
		out->separated = false;
}

/*
 * Run a query and write its results, handing them to the stream before
 * returning.  Returns 0, or -1 with the failure reported: the query's,
 * the library's, or a write's.
 *
 * Nothing is written for a query that fails before its first row, and the
 * library delivers no row until everything that can fail has succeeded,
 * what comes before the first row being written only with it.  The one
 * failure that can follow a row is an interrupt.  The rows delivered before
 * it are handed to the stream, so that the output ends where a row does,
 * and nothing more: the format's closing bracket would make a truncated
 * answer look complete, and the count of -v would claim one.  The query
 * still counts as written, so that the next query's results are separated
 * from its rows.  When sorting, the answer is collected whole and written
 * once it has been sorted, and an interrupt while it is written stops it
 * between two rows in the same way.
 */
int
output_query(output_t *out, relic_query_t *query, size_t *rowsp)
{
	int status;

	if (out->stream == NULL)
		return (output_discard(out, query, rowsp));

	out->begun = false;
	out->rows_written = 0;

	if (out->sorted)
		status = write_collected(out, query);
	else
		status = relic_query_run(out->ctx, query, receive_row, out);

	if (status != 0) {
		if (out->begun)
			out->queries_written++;
		flush_buffer(out);
		return (-1);
	}

	end_query(out);
	out->queries_written++;
	*rowsp = out->rows_written;

	flush_buffer(out);

	return (out->write_failed ? -1 : 0);
}

/*
 * The stream's error indicator is checked as well as the flush, because an
 * interactive session writes its replies to standard output around the
 * output's own writes, and a reply that failed is reported here like a row.
 * The reply's write failed when it was made, unbuffered, and errno no longer
 * says why, so that failure is reported without a reason.
 */
int
output_flush(output_t *out)
{
	if (out->stream == NULL)
		return (0);

	flush_buffer(out);
	if (out->write_failed)
		return (-1);

	errno = 0;
	if (fflush(out->stream) != 0) {
		report_write_failure(out, errno);
		return (-1);
	}

	if (ferror(out->stream)) {
		report_write_failure(out, 0);
		return (-1);
	}

	return (0);
}

int
output_close(output_t *out)
{
	FILE *stream;
	bool failed;

	if (out->stream == NULL)
		return (0);

	failed = output_flush(out) != 0;
	free(out->buffer);
	out->buffer = NULL;
	stream = out->stream;
	out->stream = NULL;

	if (stream == stdout)
		return (failed ? -1 : 0);

	if (fclose(stream) != 0 && !failed) {
		report_error(out->program_name, "cannot close %s: %s",
		    out->name, strerror(errno));
		failed = true;
	}

	return (failed ? -1 : 0);
}
