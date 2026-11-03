/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <relic.h>

#include "cmd.h"
#include "compat.h"

/*
 * Exports: a whole relation written to a file, after the queries have run.
 *
 * An export is the answer to a query with a distinct variable in every
 * column of its predicate, `p(V1, ..., Vn)?`, so it reads a relation
 * exactly as a query does -- materialized under -a, through the Magic
 * rewrite otherwise -- and writes it through the same output, one per file.
 * The query is prepared with the queries, once the program is checked, which
 * is when the library can say at which arities the predicate's name is a
 * predicate: an export that does not name its arity takes the one its
 * predicate has, and is refused when it has none or several.
 */

/* Room for every arity a name can have, listed: "0, 1, ... 31 and 32". */
#define	EXPORT_ARITY_LIST_MAX	(33 * sizeof(", 32"))

static bool	 has_one_arity(uint64_t arities);
static void	 list_arities(uint64_t arities, char *text, size_t size);
static unsigned int lowest_arity(uint64_t arities);
static char	*query_text(const char *predicate, unsigned int arity);
static relic_query_t *prepare_export(invocation_t *invocation,
		    const export_operand_t *export, size_t number);
static int	 resolve_arity(invocation_t *invocation,
		    const export_operand_t *export, unsigned int *arityp);
static int	 write_export(invocation_t *invocation,
		    const prepared_export_t *export, bool sorted,
		    size_t number);

/*
 * Every export is attempted even after one fails, so that one run reports
 * the mistakes in all of them, as it does a query's.
 */
int
exports_prepare(invocation_t *invocation, const export_operand_t *operands,
    size_t count)
{
	prepared_export_t *export;
	size_t i;
	int status;

	if (count == 0)
		return (0);

	invocation->exports = calloc(count, sizeof(*invocation->exports));
	if (invocation->exports == NULL) {
		report_error(invocation->program_name, "out of memory");
		return (-1);
	}
	invocation->export_count = count;

	status = 0;
	for (i = 0; i < count; i++) {
		export = &invocation->exports[i];
		export->operand = &operands[i];
		export->query = prepare_export(invocation, export->operand,
		    i + 1);
		if (export->query == NULL)
			status = -1;
	}

	return (status);
}

/*
 * The query of every column of an export's predicate, named "<export N>"
 * by the export's position among them, or NULL having reported why there is
 * none.
 */
static relic_query_t *
prepare_export(invocation_t *invocation, const export_operand_t *export,
    size_t number)
{
	relic_query_t *query;
	char name[CMD_QUERY_NAME_MAX];
	unsigned int arity;
	char *text;

	if (resolve_arity(invocation, export, &arity) != 0)
		return (NULL);

	text = query_text(export->predicate, arity);
	if (text == NULL) {
		report_error(invocation->program_name, "out of memory");
		return (NULL);
	}

	snprintf(name, sizeof(name), "<export %zu>", number);
	query = relic_query_prepare(invocation->ctx, name, text);
	free(text);

	return (query);
}

/*
 * Settle the arity an export writes its predicate at: the one its head
 * names, when the program has the predicate at it, or else the one arity
 * the program has the name at.  A name the program does not have at all is
 * most often misspelled, and is refused rather than written as an empty
 * relation; a name it has at several is refused with the arities it has.
 */
static int
resolve_arity(invocation_t *invocation, const export_operand_t *export,
    unsigned int *arityp)
{
	cmd_quoted_t name;
	cmd_quoted_t operand;
	char listed[EXPORT_ARITY_LIST_MAX];
	uint64_t arities;

	if (relic_predicate_arities(invocation->ctx, export->predicate,
	    &arities) != 0)
		return (-1);

	if (export->has_arity &&
	    (arities & (UINT64_C(1) << export->arity)) != 0) {
		*arityp = export->arity;
		return (0);
	}
	if (!export->has_arity && has_one_arity(arities)) {
		*arityp = lowest_arity(arities);
		return (0);
	}

	cmd_quote(export->predicate, strlen(export->predicate), &name);
	cmd_quote(export->operand, strlen(export->operand), &operand);

	if (export->has_arity) {
		report_error(invocation->program_name, "no predicate '%s/%u' "
		    "to export in %s", name.text, export->arity, operand.text);
	} else if (arities == 0) {
		report_error(invocation->program_name, "no predicate '%s' to "
		    "export in %s", name.text, operand.text);
	} else {
		list_arities(arities, listed, sizeof(listed));
		report_error(invocation->program_name, "'%s' is a predicate of "
		    "arities %s; name one in a head, as %s/%u, in %s",
		    name.text, listed, name.text, lowest_arity(arities),
		    operand.text);
	}

	return (-1);
}

/* Whether a set of arities holds exactly one. */
static bool
has_one_arity(uint64_t arities)
{
	return (arities != 0 && (arities & (arities - 1)) == 0);
}

/* The lowest arity of a set that holds at least one. */
static unsigned int
lowest_arity(uint64_t arities)
{
	unsigned int arity;

	for (arity = 0; (arities & (UINT64_C(1) << arity)) == 0; arity++)
		continue;

	return (arity);
}

/*
 * List the arities of a set, in increasing order, as "2 and 3" or
 * "0, 2 and 3".
 */
static void
list_arities(uint64_t arities, char *text, size_t size)
{
	char number[sizeof("32")];
	uint64_t rest;
	unsigned int arity;

	text[0] = '\0';
	rest = arities;
	for (arity = 0; rest != 0; arity++) {
		if ((rest & (UINT64_C(1) << arity)) == 0)
			continue;
		rest &= ~(UINT64_C(1) << arity);

		if (text[0] != '\0')
			strlcat(text, rest == 0 ? " and " : ", ", size);
		snprintf(number, sizeof(number), "%u", arity);
		strlcat(text, number, size);
	}
}

/*
 * The text of the query of every column of `predicate` at `arity`,
 * allocated: `p(V1, V2)?`, or `p?` with no columns.  The name is one the
 * operand's parse accepted as a predicate name, so the text is a query
 * whatever the name is.
 */
static char *
query_text(const char *predicate, unsigned int arity)
{
	char variable[sizeof(", V32")];
	char *text;
	size_t size;
	unsigned int column;

	size = strlen(predicate) + sizeof("()?") +
	    (size_t)arity * (sizeof(variable) - 1);
	text = malloc(size);
	if (text == NULL)
		return (NULL);

	strlcpy(text, predicate, size);
	if (arity > 0) {
		strlcat(text, "(", size);
		for (column = 1; column <= arity; column++) {
			snprintf(variable, sizeof(variable), "%sV%u",
			    column > 1 ? ", " : "", column);
			strlcat(text, variable, size);
		}
		strlcat(text, ")", size);
	}
	strlcat(text, "?", size);

	return (text);
}

int
exports_write(invocation_t *invocation, bool sorted)
{
	size_t i;

	for (i = 0; i < invocation->export_count; i++) {
		if (write_export(invocation, &invocation->exports[i], sorted,
		    i + 1) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Write one export: the answer to its query, into its file.  The file is
 * created or truncated only when the first row is written, or once the
 * query has succeeded with none: the library delivers no row until
 * everything in the evaluation that can fail has succeeded, so an export
 * whose evaluation fails -- at a limit, for memory -- leaves its file as it
 * was.  A write that fails is reported naming the file.  The invocation's
 * output is this one while the query runs, so that a diagnostic follows the
 * rows written before it, and is given back after.
 */
static int
write_export(invocation_t *invocation, const prepared_export_t *export,
    bool sorted, size_t number)
{
	const export_operand_t *operand;
	output_t *previous;
	output_t output;
	size_t rows;
	int status;

	operand = export->operand;
	output_init(&output, invocation->program_name, invocation->ctx);
	if (output_attach_file(&output, operand->path, operand->format,
	    operand->format == OUTPUT_DL ? operand->predicate : NULL,
	    sorted) != 0)
		return (-1);

	previous = invocation->output;
	invocation->output = &output;
	status = output_query(&output, export->query, &rows);
	invocation->output = previous;

	if (output_close(&output, status == 0) != 0)
		status = -1;
	if (status == 0)
		report_rows(invocation, "export", number, rows);

	return (status);
}
