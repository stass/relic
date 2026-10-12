/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_TABLE_H
#define RELIC_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <relic.h>

/*
 * Reading a table's fields: the lexical rules a field's value is held to
 * (lib/lex.c) and the table reader (lib/import.c).  Neither needs a
 * context, and code that must only read the store (RELIC_FROZEN_ONLY),
 * such as a batch import's workers (lib/window.c), decodes tables with
 * them.
 */

/*
 * Whether `length` bytes are exactly one symbol as the lexer scans one: a
 * lowercase letter followed by letters, digits and underscores.  Bytes that
 * reach the program by another road than text -- a value or a predicate name
 * built through the interface -- are held to it, so that everything the
 * program holds is something the language could have written.
 */
bool	 relic_lex_is_symbol(const char *bytes, size_t length);

/*
 * The language's integer rule, in two pieces that the lexer and the parser
 * each use and that table import uses together.
 *
 * relic_lex_magnitude() converts `length` bytes that must be `[0-9]+` into
 * their magnitude, which must be at most RELIC_MAGNITUDE_MAX, the magnitude
 * of INT64_MIN.  relic_integer_signed() gives a magnitude a sign, and says
 * whether the result fits int64_t: RELIC_MAGNITUDE_MAX fits only when it is
 * negative.
 *
 * relic_lex_integer() is the whole rule applied to `length` bytes: the range
 * must be `-?[0-9]+` and its value must fit int64_t.
 *
 * Each returns RELIC_INTEGER_VALID with its result stored, or the way the
 * bytes fail the rule, leaving the result untouched, since a range that is
 * no integer and one that is an integer out of range are reported
 * differently.  Nothing past the range is read.  A leading zero is
 * accepted, and `-0` is zero.
 */
#define	RELIC_MAGNITUDE_MAX	((uint64_t)INT64_MAX + 1u)

enum relic_integer_verdict {
	RELIC_INTEGER_VALID,
	RELIC_INTEGER_MALFORMED,	/* not the digits the rule asks for */
	RELIC_INTEGER_OUT_OF_RANGE	/* digits whose value does not fit */
};

enum relic_integer_verdict relic_lex_magnitude(const char *bytes,
	    size_t length, uint64_t *magnitudep);
enum relic_integer_verdict relic_integer_signed(uint64_t magnitude,
	    bool negative, int64_t *valuep);
enum relic_integer_verdict relic_lex_integer(const char *bytes, size_t length,
	    int64_t *valuep);

/*
 * The table reader: the state machine that alone decides where the fields
 * and records of a csv or tsv table end, and what bytes each field holds
 * once its format's quoting or escapes are decoded.  It is a function of its
 * state and the bytes it is fed, with no context and no input or output of
 * its own, so it reads a table the same way whether the table arrives in one
 * piece or a byte at a time.
 *
 * relic_table_reader_init() prepares a reader of a table in `format` whose
 * records have `width` fields each, at least one, and names the locations
 * it reports `source_name`, which it borrows.  relic_table_feed() hands it
 * the next `length` bytes of the table, which it borrows until it asks for
 * more, and says whether they are the last.  relic_table_next() fills in
 * the next event and returns its kind:
 *
 *	RELIC_TABLE_MORE	every byte fed has been read; feed more
 *	RELIC_TABLE_FIELD	a field ended: its decoded bytes, where its
 *				first byte is, and its position in its record,
 *				from zero.  The bytes stay valid until the next
 *				call.
 *	RELIC_TABLE_RECORD	a record ended, with a field per column
 *	RELIC_TABLE_END		the table ended, at `where`, the end of the
 *				input
 *	RELIC_TABLE_MISTAKE	the table is malformed, as `mistake` says, at
 *				`where`
 *	RELIC_TABLE_NO_MEMORY	the field could not grow
 *
 * After END, MISTAKE or NO_MEMORY the reader stops, and every later call
 * returns the same event.  A record with the wrong number of fields is a
 * mistake: the reader checks the width when a field beyond the last column
 * begins, and when a record ends short, so the first mistake a table holds
 * is the first reported, whatever it is.
 *
 * Before it reads anything, a reader can be made to frame the table
 * rather than decode it, by relic_table_reader_frame_only(): it makes the
 * same transitions, and so finds the same boundaries and the same
 * mistakes, but collects no field's bytes, and relic_table_next() returns
 * none of its FIELD events, so its events are RECORD, END, MORE and the
 * mistakes, and it never runs out of memory.  And it can be made to begin
 * at a record boundary of a table another reader has read up to, by
 * relic_table_reader_start_at(), given the location of the boundary's
 * byte: it then reports the locations a reader of the whole table would.
 * relic_table_unread() is how many of the bytes last fed the reader has
 * not read; when it has just emitted a RECORD, the bytes it has read end
 * exactly at the record's end.
 *
 * relic_table_reader_reset() releases the field buffer.
 */

enum relic_table_event_kind {
	RELIC_TABLE_MORE,
	RELIC_TABLE_FIELD,
	RELIC_TABLE_RECORD,
	RELIC_TABLE_END,
	RELIC_TABLE_MISTAKE,
	RELIC_TABLE_NO_MEMORY
};

enum relic_table_mistake {
	RELIC_TABLE_TOO_MANY_FIELDS,	/* at the first extra field */
	RELIC_TABLE_TOO_FEW_FIELDS,	/* at the end of the record */
	RELIC_TABLE_UNKNOWN_ESCAPE,	/* tsv; at `byte`, after a backslash */
	RELIC_TABLE_ESCAPE_AT_END,	/* tsv; a backslash ends the input */
	RELIC_TABLE_STRAY_RETURN,	/* a return not before a line feed */
	RELIC_TABLE_QUOTE_IN_UNQUOTED,	/* csv; at the quote */
	RELIC_TABLE_AFTER_CLOSING_QUOTE, /* csv; at `byte` */
	RELIC_TABLE_UNTERMINATED_QUOTE	/* csv; at the opening quote */
};

/*
 * Where the reader stands.  Each state also decides what the end of the
 * input means there, since the end is not a byte.
 */
enum relic_table_state {
	RELIC_TABLE_AT_RECORD,		/* at the start of a record */
	RELIC_TABLE_AT_FIELD,		/* at a field, after a separator */
	RELIC_TABLE_IN_FIELD,		/* inside an unquoted field */
	RELIC_TABLE_IN_QUOTES,		/* inside a quoted csv field */
	RELIC_TABLE_AFTER_QUOTE,	/* after a quote inside quotes */
	RELIC_TABLE_AFTER_BACKSLASH,	/* after a tsv backslash */
	RELIC_TABLE_AFTER_RETURN,	/* after a carriage return */
	RELIC_TABLE_RECORD_ENDED,	/* the record's last field has ended */
	RELIC_TABLE_STOPPED		/* `stopped` is the last event */
};

typedef struct relic_table_event relic_table_event_t;
typedef struct relic_table_reader relic_table_reader_t;

/*
 * One event.  `bytes` and `length` are a field's decoded bytes, never NULL,
 * and `position` the field's position in its record.  `byte` is the byte a
 * mistake names, and `fields_found` the number of fields a record with too
 * few has.
 */
struct relic_table_event {
	const char			*bytes;
	size_t				 length;
	size_t				 position;
	size_t				 fields_found;
	relic_location_t		 where;
	enum relic_table_event_kind	 kind;
	enum relic_table_mistake	 mistake;
	unsigned char			 byte;
};

/*
 * A reader.  `input` is the part of the bytes last fed that has not been
 * read, and `input_is_last` says whether the table ends after it.
 * `field` accumulates the field being read, unless the reader only
 * `frames`; it grows by doubling to the longest field and is reused for
 * every one.  `fields_ended` counts the fields of the current record read
 * so far.
 *
 * `where` is the location of the next byte.  The other locations are the
 * places a later event points back to: the current field's first byte, the
 * carriage return that must be followed by a line feed, and the end of the
 * record whose last field has ended.
 */
struct relic_table_reader {
	const char			*input;
	char				*field;
	size_t				 input_length;
	size_t				 field_length;
	size_t				 field_capacity;
	size_t				 width;
	size_t				 fields_ended;
	relic_location_t		 where;
	relic_location_t		 field_where;
	relic_location_t		 return_where;
	relic_location_t		 record_end_where;
	relic_table_event_t		 stopped;
	enum relic_format		 format;
	enum relic_table_state		 state;
	bool				 input_is_last;
	bool				 frames;
};

void	 relic_table_reader_init(relic_table_reader_t *reader,
	    enum relic_format format, size_t width, const char *source_name);
void	 relic_table_reader_frame_only(relic_table_reader_t *reader);
void	 relic_table_reader_start_at(relic_table_reader_t *reader,
	    const relic_location_t *where);
size_t	 relic_table_unread(const relic_table_reader_t *reader);
void	 relic_table_reader_reset(relic_table_reader_t *reader);
void	 relic_table_feed(relic_table_reader_t *reader, const char *bytes,
	    size_t length, bool last);
enum relic_table_event_kind relic_table_next(relic_table_reader_t *reader,
	    relic_table_event_t *event);

#endif /* RELIC_TABLE_H */
