/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <err.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <relic.h>

#include "answer.h"

static int	 compare_rows(const void *a, const void *b);
static char	*render_row(const relic_result_t *result,
		    const relic_value_t *values, size_t column_count);
static void	 render_value(const relic_result_t *result,
		    const relic_value_t *value, FILE *out);

bool
answer_collect(relic_t *ctx, relic_query_t *query, answer_t *answer)
{
	relic_result_t *result;
	size_t row_count, column_count;

	result = relic_query_collect(ctx, query);
	if (result == NULL)
		return (false);

	row_count = relic_result_rows(result);
	column_count = relic_result_columns(result);
	answer->rows = calloc(row_count > 0 ? row_count : 1,
	    sizeof(*answer->rows));
	if (answer->rows == NULL)
		err(1, "calloc");

	for (size_t r = 0; r < row_count; r++) {
		answer->rows[r] = render_row(result,
		    relic_result_row(result, r), column_count);
		answer->row_count++;
	}
	relic_result_destroy(result);

	qsort(answer->rows, answer->row_count, sizeof(*answer->rows),
	    compare_rows);
	return (true);
}

/* A row as `(v1, v2, ...)`; a row of no columns is `()`. */
static char *
render_row(const relic_result_t *result, const relic_value_t *values,
    size_t column_count)
{
	char *row = NULL;
	size_t length;
	FILE *out;

	out = open_memstream(&row, &length);
	if (out == NULL)
		err(1, "open_memstream");

	fputc('(', out);
	for (size_t c = 0; c < column_count; c++) {
		if (c > 0)
			fputs(", ", out);
		render_value(result, &values[c], out);
	}
	fputc(')', out);

	if (fclose(out) != 0)
		err(1, "rendering a row");
	return (row);
}

static void
render_value(const relic_result_t *result, const relic_value_t *value,
    FILE *out)
{
	const unsigned char *bytes;
	size_t length;

	switch (value->type) {
	case RELIC_INTEGER:
		fprintf(out, "%" PRId64, value->v.integer);
		return;
	case RELIC_SYMBOL:
		bytes = (const unsigned char *)relic_result_string(result,
		    value->v.id, &length);
		fwrite(bytes, 1, length, out);
		return;
	case RELIC_STRING:
		bytes = (const unsigned char *)relic_result_string(result,
		    value->v.id, &length);
		fputc('"', out);
		for (size_t i = 0; i < length; i++) {
			if (bytes[i] < 0x20 || bytes[i] > 0x7e ||
			    bytes[i] == '"' || bytes[i] == '\\')
				fprintf(out, "\\x%02x", bytes[i]);
			else
				fputc(bytes[i], out);
		}
		fputc('"', out);
		return;
	}
}

static int
compare_rows(const void *a, const void *b)
{
	const char *const *row_a = a;
	const char *const *row_b = b;

	return (strcmp(*row_a, *row_b));
}

bool
answer_equal(const answer_t *a, const answer_t *b)
{
	if (a->row_count != b->row_count)
		return (false);
	for (size_t r = 0; r < a->row_count; r++) {
		if (strcmp(a->rows[r], b->rows[r]) != 0)
			return (false);
	}
	return (true);
}

/*
 * Both answers are sorted, so one merge pass finds the rows each holds that
 * the other does not.
 */
void
answer_report_difference(FILE *report, const answer_t *expected,
    const answer_t *actual)
{
	size_t e = 0, a = 0;

	while (e < expected->row_count || a < actual->row_count) {
		int order;

		if (e == expected->row_count)
			order = 1;
		else if (a == actual->row_count)
			order = -1;
		else
			order = strcmp(expected->rows[e], actual->rows[a]);

		if (order < 0) {
			fprintf(report, "  - %s\n", expected->rows[e]);
			e++;
		} else if (order > 0) {
			fprintf(report, "  + %s\n", actual->rows[a]);
			a++;
		} else {
			e++;
			a++;
		}
	}
}

void
answer_free(answer_t *answer)
{
	for (size_t r = 0; r < answer->row_count; r++)
		free(answer->rows[r]);
	free(answer->rows);
	answer->rows = NULL;
	answer->row_count = 0;
}
