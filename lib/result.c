/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * The collector: a query's answer copied out of the stream into memory the
 * caller owns.
 *
 * It is a row callback of the stream relic_query_run() delivers, and reads
 * nothing of the engine besides, so it answers exactly what the stream
 * delivers, in the same order.  A result holds no pointer into the context:
 * values are copied whole, and a symbol or string is an identifier the
 * context keeps for its whole life, so a result survives anything done to
 * the context afterwards, destroying it included.
 */

/*
 * `rows` rows of `columns` values each, row after row in `values`, which
 * has room for `capacity` values.  `failed` records that a row could not be
 * stored, which stops the stream and makes the whole result a failure.
 */
struct relic_result {
	relic_value_t	*values;
	size_t		 rows;
	size_t		 columns;
	size_t		 capacity;
	bool		 failed;
};

static relic_result_t *collect(relic_t *ctx, relic_query_t *query);
static int	 store_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);

/*
 * Append one delivered row.  Every row of an answer has the query's width,
 * so each row's count is the same stride.  Room is kept for at least one
 * value, so that a row of a query with no columns is a valid pointer like
 * any other.
 */
static int
store_row(void *callback_ctx, const relic_value_t *values, size_t count)
{
	relic_result_t *result;
	relic_value_t *grown;
	size_t needed;

	result = callback_ctx;

	needed = (result->rows + 1) * count;
	grown = relic_grow_array(result->values, &result->capacity,
	    needed > 0 ? needed : 1, sizeof(*result->values));
	if (grown == NULL) {
		result->failed = true;
		return (1);
	}
	result->values = grown;

	if (count > 0)
		bcopy(values, &result->values[result->rows * count],
		    count * sizeof(*values));
	result->rows++;

	return (0);
}

relic_result_t *
relic_query_collect(relic_t *ctx, relic_query_t *query)
{
	relic_result_t *result;

	if (relic_enter(ctx) != 0)
		return (NULL);

	result = collect(ctx, query);

	relic_leave(ctx);

	return (result);
}

/*
 * relic_query_collect() inside an operation already entered.
 */
static relic_result_t *
collect(relic_t *ctx, relic_query_t *query)
{
	relic_result_t *result;

	result = calloc(1, sizeof(*result));
	if (result == NULL) {
		relic_diag_no_memory(ctx);
		return (NULL);
	}

	if (relic_query_answer(ctx, query, store_row, result) != 0) {
		relic_result_destroy(result);
		return (NULL);
	}

	if (result->failed) {
		relic_diag_no_memory(ctx);
		relic_result_destroy(result);
		return (NULL);
	}

	/*
	 * The width is read from the query's answer relation rather than from
	 * the rows, which a query answering nothing never delivers.
	 */
	result->columns = query->answer->arity;

	return (result);
}

size_t
relic_result_rows(const relic_result_t *result)
{
	return (result->rows);
}

size_t
relic_result_columns(const relic_result_t *result)
{
	return (result->columns);
}

const relic_value_t *
relic_result_row(const relic_result_t *result, size_t row)
{
	if (row >= result->rows)
		return (NULL);

	return (&result->values[row * result->columns]);
}

void
relic_result_destroy(relic_result_t *result)
{
	if (result == NULL)
		return;

	relic_free_array(result->values);
	free(result);
}
