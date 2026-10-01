/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cmd.h"
#include "compat.h"

/*
 * The -I operand: one import, spelled
 *
 *	format[,header]:predicate:column[,column...]=path
 *
 * and parsed into the relic_import_t the library takes.  The words are the
 * tool's spelling; the library has none of its own.  The path is everything
 * after the first `=`, and nothing before that `=` can hold one, so a path
 * may.  A malformed operand is a usage error, as an unknown -F format is.
 */

/* The most columns an import may keep: the arity limit of a predicate. */
#define	IMPORT_KEPT_MAX	32

typedef struct operand_parts operand_parts_t;
typedef struct operand_problem operand_problem_t;
typedef struct word word_t;

/*
 * A word of an operand: `length` bytes at `bytes`, which are not terminated
 * where the word ends.
 */
struct word {
	const char	*bytes;
	size_t		 length;
};

/*
 * An operand cut at its separators, before any part is read: the format
 * with its option, the predicate and the column list before the first `=`,
 * and the path after it.
 */
struct operand_parts {
	word_t		 format;
	word_t		 predicate;
	word_t		 columns;
	const char	*path;
};

/*
 * Why an operand is not an import: what is wrong, and the word of it that
 * is, when the problem is one word's.
 */
struct operand_problem {
	const char	*problem;
	word_t		 word;
};

static bool	 is_predicate_name(word_t name);
static enum import_parse malformed(operand_problem_t *why, const char *problem,
		    const char *bytes, size_t length);
static enum import_parse parse_columns(word_t list, import_operand_t *parsed,
		    operand_problem_t *why);
static enum import_parse parse_format(word_t spec, import_operand_t *parsed,
		    operand_problem_t *why);
static enum import_parse parse_predicate(word_t name,
		    import_operand_t *parsed, operand_problem_t *why);
static void	 report_malformed(const char *program_name,
		    const char *operand, const operand_problem_t *why);
static enum import_parse split_operand(const char *operand,
		    operand_parts_t *parts, operand_problem_t *why);
static bool	 word_is(word_t word, const char *text);
static word_t	 word_until(const char *bytes, const char *end, char delimiter);

/*
 * Parse an operand a part at a time.  Each part's parser either fills in
 * its part of `parsed` or says why it cannot, and this alone reports the
 * failure and releases what the parse allocated, so no path out of it
 * leaves anything behind.
 */
enum import_parse
import_operand_parse(const char *program_name, const char *operand,
    import_operand_t *parsed)
{
	operand_parts_t parts;
	operand_problem_t why;
	enum import_parse status;

	bzero(parsed, sizeof(*parsed));
	bzero(&why, sizeof(why));
	parsed->operand = operand;

	status = split_operand(operand, &parts, &why);
	if (status == IMPORT_PARSED) {
		parsed->path = parts.path;
		status = parse_format(parts.format, parsed, &why);
	}
	if (status == IMPORT_PARSED)
		status = parse_predicate(parts.predicate, parsed, &why);
	if (status == IMPORT_PARSED)
		status = parse_columns(parts.columns, parsed, &why);

	switch (status) {
	case IMPORT_PARSED:
		return (IMPORT_PARSED);
	case IMPORT_MALFORMED:
		report_malformed(program_name, operand, &why);
		break;
	case IMPORT_NO_MEMORY:
		report_error(program_name, "out of memory");
		break;
	}
	import_operand_free(parsed);

	return (status);
}

/*
 * Cut an operand into its parts.  The format and the predicate cannot hold
 * a colon, so the first two colons before the `=` are the separators, and
 * the column list runs from the second to the `=`.
 */
static enum import_parse
split_operand(const char *operand, operand_parts_t *parts,
    operand_problem_t *why)
{
	const char *equals;
	const char *after_format;
	const char *after_predicate;

	equals = strchr(operand, '=');
	if (equals == NULL)
		return (malformed(why, "no '=' before a path", NULL, 0));
	if (equals[1] == '\0')
		return (malformed(why, "empty path", NULL, 0));

	parts->format = word_until(operand, equals, ':');
	after_format = parts->format.bytes + parts->format.length;
	if (after_format == equals)
		return (malformed(why,
		    "expected format:predicate:columns before '='", NULL, 0));

	parts->predicate = word_until(after_format + 1, equals, ':');
	after_predicate = parts->predicate.bytes + parts->predicate.length;
	if (after_predicate == equals)
		return (malformed(why,
		    "expected format:predicate:columns before '='", NULL, 0));

	parts->columns.bytes = after_predicate + 1;
	parts->columns.length = (size_t)(equals - parts->columns.bytes);
	parts->path = equals + 1;

	return (IMPORT_PARSED);
}

/*
 * The format and its option: `csv` or `tsv`, and `,header` after it.
 */
static enum import_parse
parse_format(word_t spec, import_operand_t *parsed, operand_problem_t *why)
{
	word_t format;
	word_t option;

	format = word_until(spec.bytes, spec.bytes + spec.length, ',');
	if (format.length == 0)
		return (malformed(why, "empty format", NULL, 0));

	if (word_is(format, "csv"))
		parsed->import.format = RELIC_FORMAT_CSV;
	else if (word_is(format, "tsv"))
		parsed->import.format = RELIC_FORMAT_TSV;
	else
		return (malformed(why, "unknown format", format.bytes,
		    format.length));

	if (format.length == spec.length)
		return (IMPORT_PARSED);

	option.bytes = format.bytes + format.length + 1;
	option.length = spec.length - format.length - 1;
	if (!word_is(option, "header"))
		return (malformed(why, "unknown format option", option.bytes,
		    option.length));
	parsed->import.flags |= RELIC_IMPORT_HEADER;

	return (IMPORT_PARSED);
}

/*
 * The predicate, copied so that it is terminated, as the library needs.
 */
static enum import_parse
parse_predicate(word_t name, import_operand_t *parsed, operand_problem_t *why)
{
	if (name.length == 0)
		return (malformed(why, "empty predicate", NULL, 0));
	if (!is_predicate_name(name))
		return (malformed(why, "not a predicate name", name.bytes,
		    name.length));

	parsed->predicate = strndup(name.bytes, name.length);
	if (parsed->predicate == NULL)
		return (IMPORT_NO_MEMORY);
	parsed->import.predicate = parsed->predicate;

	return (IMPORT_PARSED);
}

/*
 * The column types, separated by commas, into a new array the operand owns:
 * one more column than the list has commas.  Each type is the word from the
 * start of the list, or from just past a comma, to the next comma or the
 * list's end.
 */
static enum import_parse
parse_columns(word_t list, import_operand_t *parsed, operand_problem_t *why)
{
	const char *end;
	const char *cursor;
	const char *type_start;
	size_t column_total;
	size_t column_index;
	size_t kept;

	end = list.bytes + list.length;

	column_total = 1;
	for (cursor = list.bytes; cursor < end; cursor++) {
		if (*cursor == ',')
			column_total++;
	}

	parsed->columns = calloc(column_total, sizeof(*parsed->columns));
	if (parsed->columns == NULL)
		return (IMPORT_NO_MEMORY);

	kept = 0;
	type_start = list.bytes;
	for (column_index = 0; column_index < column_total; column_index++) {
		enum relic_column *column;
		word_t type;

		type = word_until(type_start, end, ',');
		if (type.bytes + type.length < end)
			type_start = type.bytes + type.length + 1;
		column = &parsed->columns[column_index];

		if (type.length == 0)
			return (malformed(why, "empty column type", NULL, 0));
		if (word_is(type, "symbol"))
			*column = RELIC_COLUMN_SYMBOL;
		else if (word_is(type, "string"))
			*column = RELIC_COLUMN_STRING;
		else if (word_is(type, "integer"))
			*column = RELIC_COLUMN_INTEGER;
		else if (word_is(type, "_"))
			*column = RELIC_COLUMN_SKIP;
		else
			return (malformed(why, "unknown column type",
			    type.bytes, type.length));

		if (*column != RELIC_COLUMN_SKIP)
			kept++;
	}

	if (kept > IMPORT_KEPT_MAX)
		return (malformed(why, "more than "
		    CMD_STRINGIFY(IMPORT_KEPT_MAX) " columns kept", NULL, 0));

	parsed->import.columns = parsed->columns;
	parsed->import.column_count = column_total;

	return (IMPORT_PARSED);
}

/*
 * Record why an operand is not an import: `problem`, and the `length` bytes
 * at `bytes` that are wrong when they are one word, or NULL.
 */
static enum import_parse
malformed(operand_problem_t *why, const char *problem, const char *bytes,
    size_t length)
{
	why->problem = problem;
	why->word.bytes = bytes;
	why->word.length = length;

	return (IMPORT_MALFORMED);
}

/*
 * Report why an operand is not an import.  The problem comes first and the
 * operand is quoted cut short, since an argument can be as long as the
 * system allows, so that the message says what is wrong whatever the
 * operand is.
 */
static void
report_malformed(const char *program_name, const char *operand,
    const operand_problem_t *why)
{
	cmd_quoted_t quoted_operand;
	cmd_quoted_t quoted_word;

	cmd_quote(operand, strlen(operand), &quoted_operand);
	if (why->word.bytes == NULL) {
		report_error(program_name, "invalid import: %s in %s",
		    why->problem, quoted_operand.text);
		return;
	}

	report_error(program_name, "invalid import: %s '%s' in %s",
	    why->problem, cmd_quote(why->word.bytes, why->word.length,
	    &quoted_word), quoted_operand.text);
}

/* Whether a word is exactly `text`. */
static bool
word_is(word_t word, const char *text)
{
	return (strlen(text) == word.length &&
	    bcmp(word.bytes, text, word.length) == 0);
}

/*
 * The word from `bytes` up to the first `delimiter` before `end`, or up to
 * `end` when there is none.
 */
static word_t
word_until(const char *bytes, const char *end, char delimiter)
{
	word_t word;
	const char *found;

	found = memchr(bytes, delimiter, (size_t)(end - bytes));
	word.bytes = bytes;
	word.length = (size_t)((found != NULL ? found : end) - bytes);

	return (word);
}

/*
 * Whether a word may name a predicate, by the language's rule: a lowercase
 * ASCII letter followed by ASCII letters, digits and underscores, other than
 * the negation keyword.
 */
static bool
is_predicate_name(word_t name)
{
	size_t i;

	if (name.length == 0 || name.bytes[0] < 'a' || name.bytes[0] > 'z')
		return (false);

	for (i = 1; i < name.length; i++) {
		char byte;

		byte = name.bytes[i];
		if (!((byte >= 'a' && byte <= 'z') ||
		    (byte >= 'A' && byte <= 'Z') ||
		    (byte >= '0' && byte <= '9') || byte == '_'))
			return (false);
	}

	return (!word_is(name, "not"));
}

bool
import_operand_reads_stdin(const import_operand_t *parsed)
{
	return (strcmp(parsed->path, "-") == 0);
}

void
import_operand_free(import_operand_t *parsed)
{
	free(parsed->columns);
	free(parsed->predicate);
	bzero(parsed, sizeof(*parsed));
}
