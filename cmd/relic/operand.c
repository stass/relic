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
 * The -I, -T and -O operands.  An import is spelled either as a path alone,
 * whose file name gives the format and the predicate,
 *
 *	path
 *
 * or with a head that says what a path cannot,
 *
 *	format[,header]:predicate[:column[,column...]]=path
 *
 * and a predicate's column types are spelled
 *
 *	predicate:column[,column...]
 *
 * An import that lists no columns takes those of its predicate's -T, given
 * anywhere on the command line, so imports are completed only once every
 * option is known (import_operands_resolve()).  The result is the
 * relic_import_t the library takes; the words are the tool's spelling, and
 * the library has none of its own.
 *
 * An export is spelled as an import is, a path alone or a head and a path,
 *
 *	format:predicate[/arity]=path
 *
 * and names a format of the tool's output rather than of the library's
 * import.  Which arity a predicate has is the program's to say, so an
 * export without one is completed once the program is loaded (export.c).
 *
 * A path with a head is everything after the first `=`, and nothing in a
 * head can hold one, so such a path may.  A malformed operand is a usage
 * error, as an unknown -F format is.
 */

/* The most columns an import may keep: the arity limit of a predicate. */
#define	IMPORT_KEPT_MAX	32

/* The largest arity an export may name, the same limit. */
#define	EXPORT_ARITY_MAX	32

typedef struct column_list column_list_t;
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
 * An import operand cut at its separators, before any part is read.  An
 * operand with a head has the format with its option, the predicate and,
 * when `has_columns`, the column list before its first `=`, and the path
 * after it; an operand without one is all path.
 */
struct operand_parts {
	word_t		 format;
	word_t		 predicate;
	word_t		 columns;
	const char	*path;
	bool		 has_head;
	bool		 has_columns;
};

/*
 * A column list, parsed: `count` types at `types`, allocated.
 */
struct column_list {
	enum relic_column	*types;
	size_t			 count;
};

/*
 * Why an operand is not what it should be: what is wrong, and the word of
 * it that is, when the problem is one word's.
 */
struct operand_problem {
	const char	*problem;
	word_t		 word;
};

static const column_types_t *columns_of(const column_types_t *types,
		    size_t type_count, const char *predicate);
static bool	 first_columns_given(const import_operand_t *imports,
		    size_t import_count, const column_types_t *declared,
		    const char *predicate, const enum relic_column **columnsp,
		    size_t *countp);
static bool	 export_format_named(word_t name, enum output_format *formatp);
static bool	 format_named(word_t name, enum relic_format *formatp);
static bool	 is_predicate_name(word_t name);
static enum operand_parse malformed(operand_problem_t *why, const char *problem,
		    const char *bytes, size_t length);
static enum operand_parse parse_arity(word_t digits, unsigned int *arityp,
		    operand_problem_t *why);
static enum operand_parse parse_columns(word_t list, column_list_t *parsed,
		    operand_problem_t *why);
static enum operand_parse parse_export_head(word_t head,
		    export_operand_t *parsed, operand_problem_t *why);
static enum operand_parse parse_export_path(const char *path,
		    export_operand_t *parsed, operand_problem_t *why);
static enum operand_parse parse_format(word_t spec, import_operand_t *parsed,
		    operand_problem_t *why);
static enum operand_parse parse_head(const operand_parts_t *parts,
		    import_operand_t *parsed, operand_problem_t *why);
static enum operand_parse parse_path_alone(const char *path,
		    import_operand_t *parsed, operand_problem_t *why);
static enum operand_parse parse_predicate(word_t name, char **predicatep,
		    operand_problem_t *why);
static bool	 kept_types_equal(const enum relic_column *first,
		    size_t first_count, const enum relic_column *second,
		    size_t second_count);
static void	 report_malformed(const char *program_name, const char *what,
		    const char *operand, const operand_problem_t *why);
static enum operand_parse split_file_name(const char *path,
		    const char *no_extension, word_t *stemp, word_t *extensionp,
		    operand_problem_t *why);
static enum operand_parse split_operand(const char *operand,
		    operand_parts_t *parts, operand_problem_t *why);
static bool	 word_is(word_t word, const char *text);
static word_t	 word_after(word_t word, const char *end);
static word_t	 word_until(const char *bytes, const char *end, char delimiter);

/*
 * Parse an -I operand: cut it at its separators, then read it as the shape
 * it has, a path alone or a head and a path.  Each shape's parser either
 * fills in `parsed` or says why it cannot, and this alone reports the
 * failure and releases what the parse allocated, so no path out of it
 * leaves anything behind.  An operand that lists no columns leaves them for
 * import_operands_resolve().
 */
enum operand_parse
import_operand_parse(const char *program_name, const char *operand,
    import_operand_t *parsed)
{
	operand_parts_t parts;
	operand_problem_t why;
	enum operand_parse status;

	bzero(parsed, sizeof(*parsed));
	bzero(&why, sizeof(why));
	parsed->operand = operand;

	status = split_operand(operand, &parts, &why);
	if (status == OPERAND_PARSED) {
		parsed->path = parts.path;
		if (parts.has_head)
			status = parse_head(&parts, parsed, &why);
		else
			status = parse_path_alone(parts.path, parsed, &why);
	}

	switch (status) {
	case OPERAND_PARSED:
		parsed->import.predicate = parsed->predicate;
		return (OPERAND_PARSED);
	case OPERAND_MALFORMED:
		report_malformed(program_name, "import", operand, &why);
		break;
	case OPERAND_NO_MEMORY:
		report_error(program_name, "out of memory");
		break;
	}
	import_operand_free(parsed);

	return (status);
}

/*
 * A head: the format and its option, the predicate and, when it lists
 * them, the columns, which the operand then owns.
 */
static enum operand_parse
parse_head(const operand_parts_t *parts, import_operand_t *parsed,
    operand_problem_t *why)
{
	column_list_t columns;
	enum operand_parse status;

	status = parse_format(parts->format, parsed, why);
	if (status == OPERAND_PARSED)
		status = parse_predicate(parts->predicate, &parsed->predicate,
		    why);
	if (status != OPERAND_PARSED || !parts->has_columns)
		return (status);

	status = parse_columns(parts->columns, &columns, why);
	parsed->own_columns = columns.types;
	parsed->import.columns = columns.types;
	parsed->import.column_count = columns.count;

	return (status);
}

/*
 * Parse a -T operand: a predicate, a colon and its column list.  Reports
 * the failure and releases what the parse allocated, as an import's parse
 * does.
 */
enum operand_parse
column_types_parse(const char *program_name, const char *operand,
    column_types_t *parsed)
{
	operand_problem_t why;
	column_list_t columns;
	enum operand_parse status;
	const char *end;
	word_t predicate;
	word_t list;

	bzero(parsed, sizeof(*parsed));
	bzero(&why, sizeof(why));
	parsed->operand = operand;

	end = operand + strlen(operand);
	predicate = word_until(operand, end, ':');
	status = OPERAND_PARSED;
	if (predicate.bytes + predicate.length == end)
		status = malformed(&why, "expected predicate:columns", NULL, 0);
	if (status == OPERAND_PARSED)
		status = parse_predicate(predicate, &parsed->predicate, &why);
	if (status == OPERAND_PARSED) {
		list = word_after(predicate, end);
		status = parse_columns(list, &columns, &why);
		parsed->columns = columns.types;
		parsed->column_count = columns.count;
	}

	switch (status) {
	case OPERAND_PARSED:
		return (OPERAND_PARSED);
	case OPERAND_MALFORMED:
		report_malformed(program_name, "column types", operand, &why);
		break;
	case OPERAND_NO_MEMORY:
		report_error(program_name, "out of memory");
		break;
	}
	column_types_free(parsed);

	return (status);
}

/*
 * Complete every import with its columns once the command line is read,
 * and hold each predicate to one list of column types.  An import that
 * lists no columns takes its predicate's -T, and is a usage error without
 * one.  Every list a predicate is given, by its -T or by an import of its
 * own, must keep columns of the same types in the same order, held to the
 * first given, its -T when it has one: which fields a table skips is that
 * table's layout, and may differ from one table to the next.  A predicate
 * has one -T at most.  Returns 0, or -1 having reported the first operand
 * at fault.  An import completed from a -T borrows its columns, which the
 * -T owns and outlives it.
 */
int
import_operands_resolve(const char *program_name, import_operand_t *imports,
    size_t import_count, const column_types_t *types, size_t type_count)
{
	const enum relic_column *given;
	const column_types_t *declared;
	import_operand_t *import;
	cmd_quoted_t quoted_predicate;
	cmd_quoted_t quoted;
	size_t given_count;
	size_t i;

	for (i = 0; i < type_count; i++) {
		/* Only the -T before this one are searched. */
		if (columns_of(types, i, types[i].predicate) == NULL)
			continue;
		report_error(program_name, "invalid column types: a second -T "
		    "of '%s' in %s", cmd_quote(types[i].predicate,
		    strlen(types[i].predicate), &quoted_predicate),
		    cmd_quote(types[i].operand, strlen(types[i].operand),
		    &quoted));
		return (-1);
	}

	for (i = 0; i < import_count; i++) {
		import = &imports[i];
		declared = columns_of(types, type_count,
		    import->import.predicate);
		cmd_quote(import->import.predicate,
		    strlen(import->import.predicate), &quoted_predicate);
		cmd_quote(import->operand, strlen(import->operand), &quoted);

		if (import->own_columns == NULL) {
			if (declared == NULL) {
				report_error(program_name, "invalid import: no "
				    "column types for '%s' in %s",
				    quoted_predicate.text, quoted.text);
				return (-1);
			}
			import->import.columns = declared->columns;
			import->import.column_count = declared->column_count;
			continue;
		}

		if (first_columns_given(imports, i, declared,
		    import->import.predicate, &given, &given_count) &&
		    !kept_types_equal(import->own_columns,
		    import->import.column_count, given, given_count)) {
			report_error(program_name, "invalid import: column types "
			    "other than those given for '%s' elsewhere in %s",
			    quoted_predicate.text, quoted.text);
			return (-1);
		}
	}

	return (0);
}

/*
 * The first of `type_count` column types that names `predicate`, or NULL.
 */
static const column_types_t *
columns_of(const column_types_t *types, size_t type_count,
    const char *predicate)
{
	size_t i;

	for (i = 0; i < type_count; i++) {
		if (strcmp(types[i].predicate, predicate) == 0)
			return (&types[i]);
	}

	return (NULL);
}

/*
 * The list of columns `predicate` was given first: those of its -T,
 * `declared`, when it has one, and otherwise those of the first of the
 * `import_count` imports that lists its own.  Sets *columnsp and *countp
 * and returns true, or returns false when nothing gave it any.
 */
static bool
first_columns_given(const import_operand_t *imports, size_t import_count,
    const column_types_t *declared, const char *predicate,
    const enum relic_column **columnsp, size_t *countp)
{
	size_t i;

	if (declared != NULL) {
		*columnsp = declared->columns;
		*countp = declared->column_count;
		return (true);
	}

	for (i = 0; i < import_count; i++) {
		if (imports[i].own_columns == NULL ||
		    strcmp(imports[i].import.predicate, predicate) != 0)
			continue;
		*columnsp = imports[i].own_columns;
		*countp = imports[i].import.column_count;
		return (true);
	}

	return (false);
}

/*
 * Whether two column lists keep columns of the same types in the same
 * order, whatever fields either skips.
 */
static bool
kept_types_equal(const enum relic_column *first, size_t first_count,
    const enum relic_column *second, size_t second_count)
{
	size_t i;
	size_t j;

	i = 0;
	j = 0;
	for (;;) {
		while (i < first_count && first[i] == RELIC_COLUMN_SKIP)
			i++;
		while (j < second_count && second[j] == RELIC_COLUMN_SKIP)
			j++;
		if (i == first_count || j == second_count)
			return (i == first_count && j == second_count);
		if (first[i] != second[j])
			return (false);
		i++;
		j++;
	}
}

/*
 * Cut an import operand into its parts.  One with no `=` is a path alone.
 * In a head, the format and the predicate cannot hold a colon, so the first
 * colon ends the format, the next one, if there is one before the `=`, ends
 * the predicate, and the column list runs from it to the `=`.
 */
static enum operand_parse
split_operand(const char *operand, operand_parts_t *parts,
    operand_problem_t *why)
{
	const char *equals;
	word_t rest;

	bzero(parts, sizeof(*parts));

	equals = strchr(operand, '=');
	if (equals == NULL) {
		parts->path = operand;
		return (OPERAND_PARSED);
	}
	if (equals[1] == '\0')
		return (malformed(why, "empty path", NULL, 0));
	parts->has_head = true;
	parts->path = equals + 1;

	parts->format = word_until(operand, equals, ':');
	if (parts->format.bytes + parts->format.length == equals)
		return (malformed(why, "expected format:predicate before '='",
		    NULL, 0));

	rest = word_after(parts->format, equals);
	parts->predicate = word_until(rest.bytes, equals, ':');
	if (parts->predicate.bytes + parts->predicate.length == equals)
		return (OPERAND_PARSED);

	parts->has_columns = true;
	parts->columns = word_after(parts->predicate, equals);

	return (OPERAND_PARSED);
}

/*
 * A path alone: its file name, the part after the last slash, is a
 * predicate name, a dot and an extension that names a format, and the
 * import reads no header.  `-` has no file name, and is named with a head.
 */
static enum operand_parse
parse_path_alone(const char *path, import_operand_t *parsed,
    operand_problem_t *why)
{
	word_t predicate;
	word_t extension;
	enum operand_parse status;

	if (strcmp(path, "-") == 0)
		return (malformed(why, "standard input needs a head", NULL,
		    0));

	status = split_file_name(path, "no .csv or .tsv extension, and no "
	    "'=' before a path", &predicate, &extension, why);
	if (status != OPERAND_PARSED)
		return (status);
	if (!format_named(extension, &parsed->import.format))
		return (malformed(why, "unknown extension", extension.bytes,
		    extension.length));

	return (parse_predicate(predicate, &parsed->predicate, why));
}

/*
 * Cut the file name of a path named alone, the part after its last slash,
 * at its last dot: the stem before the dot names a predicate, and the
 * extension after it a format.  A file name with no dot is malformed, and
 * `no_extension` is the message that says so, which the caller words to
 * list the extensions it knows.
 */
static enum operand_parse
split_file_name(const char *path, const char *no_extension, word_t *stemp,
    word_t *extensionp, operand_problem_t *why)
{
	const char *slash;
	const char *dot;
	word_t file_name;

	if (path[0] == '\0')
		return (malformed(why, "empty path", NULL, 0));

	slash = strrchr(path, '/');
	file_name.bytes = slash != NULL ? slash + 1 : path;
	file_name.length = strlen(file_name.bytes);

	dot = strrchr(file_name.bytes, '.');
	if (dot == NULL)
		return (malformed(why, no_extension, NULL, 0));

	stemp->bytes = file_name.bytes;
	stemp->length = (size_t)(dot - file_name.bytes);

	/* The extension begins after the dot that ends the stem. */
	*extensionp = word_after(*stemp, file_name.bytes + file_name.length);

	return (OPERAND_PARSED);
}

/*
 * Parse an -O operand: a path alone, or a head and a path.  Reports the
 * failure and releases what the parse allocated, as an import's parse does.
 * Standard output is not a path an export writes, in either shape: a query
 * writes it, and a second writer would interleave with the rows.
 */
enum operand_parse
export_operand_parse(const char *program_name, const char *operand,
    export_operand_t *parsed)
{
	operand_problem_t why;
	enum operand_parse status;
	const char *equals;
	word_t head;

	bzero(parsed, sizeof(*parsed));
	bzero(&why, sizeof(why));
	parsed->operand = operand;

	equals = strchr(operand, '=');
	parsed->path = equals != NULL ? equals + 1 : operand;

	if (strcmp(parsed->path, "-") == 0) {
		status = malformed(&why, "no export to standard output", NULL,
		    0);
	} else if (equals == NULL) {
		status = parse_export_path(parsed->path, parsed, &why);
	} else if (parsed->path[0] == '\0') {
		status = malformed(&why, "empty path", NULL, 0);
	} else {
		head.bytes = operand;
		head.length = (size_t)(equals - operand);
		status = parse_export_head(head, parsed, &why);
	}

	switch (status) {
	case OPERAND_PARSED:
		return (OPERAND_PARSED);
	case OPERAND_MALFORMED:
		report_malformed(program_name, "export", operand, &why);
		break;
	case OPERAND_NO_MEMORY:
		report_error(program_name, "out of memory");
		break;
	}
	export_operand_free(parsed);

	return (status);
}

/*
 * An export's path alone: its extension is the format, and its stem the
 * predicate, at whatever arity the program has it.
 */
static enum operand_parse
parse_export_path(const char *path, export_operand_t *parsed,
    operand_problem_t *why)
{
	word_t predicate;
	word_t extension;
	enum operand_parse status;

	status = split_file_name(path, "no .csv, .tsv, .json or .dl "
	    "extension, and no '=' before a path", &predicate, &extension,
	    why);
	if (status != OPERAND_PARSED)
		return (status);
	if (!export_format_named(extension, &parsed->format))
		return (malformed(why, "unknown extension", extension.bytes,
		    extension.length));

	return (parse_predicate(predicate, &parsed->predicate, why));
}

/*
 * An export's head, format:predicate[/arity].  The format cannot hold a
 * colon and the predicate cannot hold a slash, so the first of each is a
 * separator.
 */
static enum operand_parse
parse_export_head(word_t head, export_operand_t *parsed,
    operand_problem_t *why)
{
	const char *end;
	word_t format;
	word_t after_format;
	word_t predicate;
	enum operand_parse status;

	end = head.bytes + head.length;
	format = word_until(head.bytes, end, ':');
	if (format.bytes + format.length == end)
		return (malformed(why, "expected format:predicate before '='",
		    NULL, 0));
	if (format.length == 0)
		return (malformed(why, "empty format", NULL, 0));
	if (!export_format_named(format, &parsed->format))
		return (malformed(why, "unknown format", format.bytes,
		    format.length));

	after_format = word_after(format, end);
	predicate = word_until(after_format.bytes, end, '/');
	status = parse_predicate(predicate, &parsed->predicate, why);
	if (status != OPERAND_PARSED)
		return (status);

	if (predicate.bytes + predicate.length == end)
		return (OPERAND_PARSED);
	parsed->has_arity = true;

	return (parse_arity(word_after(predicate, end), &parsed->arity, why));
}

/*
 * The formats an export writes, each named by the same word in a head and
 * as a path's extension.  dl, facts in the language's own syntax, is
 * written by an export alone.
 */
static bool
export_format_named(word_t name, enum output_format *formatp)
{
	static const struct {
		const char		*name;
		enum output_format	 format;
	} formats[] = {
		{ "csv", OUTPUT_CSV },
		{ "dl", OUTPUT_DL },
		{ "json", OUTPUT_JSON },
		{ "tsv", OUTPUT_TSV }
	};
	size_t i;

	for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (word_is(name, formats[i].name)) {
			*formatp = formats[i].format;
			return (true);
		}
	}

	return (false);
}

/*
 * An arity: decimal digits, at most EXPORT_ARITY_MAX.  The value is
 * accumulated only while it is within the limit, so no run of digits can
 * overflow it.
 */
static enum operand_parse
parse_arity(word_t digits, unsigned int *arityp, operand_problem_t *why)
{
	unsigned int arity;
	size_t i;

	if (digits.length == 0)
		return (malformed(why, "empty arity", NULL, 0));

	arity = 0;
	for (i = 0; i < digits.length; i++) {
		if (digits.bytes[i] < '0' || digits.bytes[i] > '9')
			return (malformed(why, "not an arity", digits.bytes,
			    digits.length));
		arity = arity * 10 + (unsigned int)(digits.bytes[i] - '0');
		if (arity > EXPORT_ARITY_MAX)
			return (malformed(why, "arity above "
			    CMD_STRINGIFY(EXPORT_ARITY_MAX), digits.bytes,
			    digits.length));
	}
	*arityp = arity;

	return (OPERAND_PARSED);
}

/*
 * The format and its option: `csv` or `tsv`, and `,header` after it.
 */
static enum operand_parse
parse_format(word_t spec, import_operand_t *parsed, operand_problem_t *why)
{
	word_t format;
	word_t option;

	format = word_until(spec.bytes, spec.bytes + spec.length, ',');
	if (format.length == 0)
		return (malformed(why, "empty format", NULL, 0));
	if (!format_named(format, &parsed->import.format))
		return (malformed(why, "unknown format", format.bytes,
		    format.length));

	if (format.length == spec.length)
		return (OPERAND_PARSED);

	option = word_after(format, spec.bytes + spec.length);
	if (!word_is(option, "header"))
		return (malformed(why, "unknown format option", option.bytes,
		    option.length));
	parsed->import.flags |= RELIC_IMPORT_HEADER;

	return (OPERAND_PARSED);
}

/*
 * Whether a word names a format, the same words in a head and as a path's
 * extension, setting *formatp when it does.
 */
static bool
format_named(word_t name, enum relic_format *formatp)
{
	if (word_is(name, "csv")) {
		*formatp = RELIC_FORMAT_CSV;
		return (true);
	}
	if (word_is(name, "tsv")) {
		*formatp = RELIC_FORMAT_TSV;
		return (true);
	}

	return (false);
}

/*
 * The predicate, copied into *predicatep so that it is terminated, as the
 * library needs.
 */
static enum operand_parse
parse_predicate(word_t name, char **predicatep, operand_problem_t *why)
{
	if (name.length == 0)
		return (malformed(why, "empty predicate", NULL, 0));
	if (!is_predicate_name(name))
		return (malformed(why, "not a predicate name", name.bytes,
		    name.length));

	*predicatep = strndup(name.bytes, name.length);
	if (*predicatep == NULL)
		return (OPERAND_NO_MEMORY);

	return (OPERAND_PARSED);
}

/*
 * The column types, separated by commas, into a new array: one more column
 * than the list has commas.  Each type is the word from the start of the
 * list, or from just past a comma, to the next comma or the list's end.
 * The array is set in *parsed as soon as it is allocated, so that a caller
 * releases it whether or not the rest of the list parses.
 */
static enum operand_parse
parse_columns(word_t list, column_list_t *parsed, operand_problem_t *why)
{
	const char *end;
	const char *cursor;
	const char *type_start;
	size_t column_total;
	size_t column_index;
	size_t kept;

	bzero(parsed, sizeof(*parsed));
	end = list.bytes + list.length;

	column_total = 1;
	for (cursor = list.bytes; cursor < end; cursor++) {
		if (*cursor == ',')
			column_total++;
	}

	parsed->types = calloc(column_total, sizeof(*parsed->types));
	if (parsed->types == NULL)
		return (OPERAND_NO_MEMORY);
	parsed->count = column_total;

	kept = 0;
	type_start = list.bytes;
	for (column_index = 0; column_index < column_total; column_index++) {
		enum relic_column *column;
		word_t type;

		type = word_until(type_start, end, ',');
		if (type.bytes + type.length < end)
			type_start = word_after(type, end).bytes;
		column = &parsed->types[column_index];

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

	return (OPERAND_PARSED);
}

/*
 * Record why an operand is not what it should be: `problem`, and the
 * `length` bytes at `bytes` that are wrong when they are one word, or NULL.
 */
static enum operand_parse
malformed(operand_problem_t *why, const char *problem, const char *bytes,
    size_t length)
{
	why->problem = problem;
	why->word.bytes = bytes;
	why->word.length = length;

	return (OPERAND_MALFORMED);
}

/*
 * Report why an operand is not `what` it should be -- an import, column
 * types, an export.  The problem comes first and the operand is quoted cut
 * short, since an argument can be as long as the system allows, so that the
 * message says what is wrong whatever the operand is.
 */
static void
report_malformed(const char *program_name, const char *what,
    const char *operand, const operand_problem_t *why)
{
	cmd_quoted_t quoted_operand;
	cmd_quoted_t quoted_word;

	if (operand[0] == '\0') {
		report_error(program_name, "invalid %s: %s", what,
		    why->problem);
		return;
	}

	cmd_quote(operand, strlen(operand), &quoted_operand);
	if (why->word.bytes == NULL) {
		report_error(program_name, "invalid %s: %s in %s", what,
		    why->problem, quoted_operand.text);
		return;
	}

	report_error(program_name, "invalid %s: %s '%s' in %s", what,
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
 * The rest of an operand after `word` and the delimiter that ends it, up to
 * `end`; `word` must end before `end`, at a delimiter.
 */
static word_t
word_after(word_t word, const char *end)
{
	word_t rest;

	rest.bytes = word.bytes + word.length + 1;
	rest.length = (size_t)(end - rest.bytes);

	return (rest);
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
	free(parsed->own_columns);
	free(parsed->predicate);
	bzero(parsed, sizeof(*parsed));
}

void
export_operand_free(export_operand_t *parsed)
{
	free(parsed->predicate);
	bzero(parsed, sizeof(*parsed));
}

void
column_types_free(column_types_t *parsed)
{
	free(parsed->columns);
	free(parsed->predicate);
	bzero(parsed, sizeof(*parsed));
}
