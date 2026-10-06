/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * The collector: a query's answer copied out of the stream into memory the
 * caller owns.
 *
 * It is a row callback of the stream relic_query_run() delivers, and reads
 * nothing of the engine besides, so it answers exactly what the stream
 * delivers, in the same order.  A result holds nothing of the context: an
 * integer is copied whole, and the bytes of a symbol or a string are copied
 * into the result's own pool, once for each distinct byte string, and the
 * value given the pool's identifier for them with RELIC_RESULT_ID_BIT set.
 * So a result survives anything done to the context afterwards, destroying
 * it included, and a result's value handed back to the context is refused
 * as one the context never assigned.
 */

/*
 * `rows` rows of `columns` values each, row after row in `values`, which
 * has room for `capacity` values.  `strings` holds the bytes of the
 * result's symbols and strings, hashed under the seed of the context the
 * rows came from, so that an answer cannot be crafted to crowd it, and
 * charged to nothing, since a result is the caller's.  `failed` records
 * that a row could not be stored, which stops the stream and makes the
 * whole result a failure.
 */
struct relic_result {
	relic_value_t	*values;
	relic_symtab_t	 strings;
	size_t		 rows;
	size_t		 columns;
	size_t		 capacity;
	bool		 failed;
};

/*
 * A collection in progress: the result being filled, and the context whose
 * rows it copies, which the result itself never holds.
 *
 * The context gives equal bytes one identifier, so a symbol or a string is
 * copied once, when its context identifier is first seen, and found again
 * by that identifier alone: `copies` indexes the result's identifiers by
 * the context identifier each was copied from, which `copied_from` holds,
 * indexed by the result's identifier.  A long string repeated down a
 * column is then hashed once rather than once a row.  Both live only as
 * long as the collection.
 */
struct collector {
	relic_t		*ctx;
	relic_result_t	*result;
	relic_idtab_t	 copies;
	uint32_t	*copied_from;
	size_t		 copied_capacity;
};

typedef struct collector collector_t;

static relic_result_t *collect(relic_t *ctx, relic_query_t *query);
static uint32_t	 copy_bytes(collector_t *collector, uint32_t source,
		    uint32_t hash);
static uint32_t	 copy_rehash(uint32_t id, void *ctx);
static uint32_t	 find_copy(const collector_t *collector, uint32_t source,
		    uint32_t hash);
static bool	 own_value(collector_t *collector, relic_value_t *value);
static const char *result_bytes(const void *source, uint32_t id,
		    size_t *lengthp);
static bool	 result_holds(const relic_result_t *result,
		    const relic_value_t *value);
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
	collector_t *collector;
	relic_result_t *result;
	relic_value_t *grown;
	relic_value_t *row;
	size_t needed;
	size_t i;

	collector = callback_ctx;
	result = collector->result;

	needed = (result->rows + 1) * count;
	grown = relic_grow_array(result->values, &result->capacity,
	    needed > 0 ? needed : 1, sizeof(*result->values));
	if (grown == NULL) {
		result->failed = true;
		return (1);
	}
	result->values = grown;

	row = &result->values[result->rows * count];
	for (i = 0; i < count; i++) {
		row[i] = values[i];
		if (!own_value(collector, &row[i])) {
			result->failed = true;
			return (1);
		}
	}
	result->rows++;

	return (0);
}

/*
 * Make a value copied from the context the result's own: a symbol's or a
 * string's identifier is replaced by the result's for the same bytes,
 * copied into the result's pool the first time they are seen.  Returns
 * false when memory ran out.
 */
static bool
own_value(collector_t *collector, relic_value_t *value)
{
	uint32_t source;
	uint32_t hash;
	uint32_t id;

	if (value->type == RELIC_INTEGER)
		return (true);

	source = value->v.id;
	hash = relic_hash_word(collector->result->strings.hash_seed, source);
	id = find_copy(collector, source, hash);
	if (id == RELIC_ID_NONE)
		id = copy_bytes(collector, source, hash);
	if (id == RELIC_ID_NONE)
		return (false);

	value->v.id = RELIC_RESULT_ID_BIT | id;

	return (true);
}

/*
 * The result's identifier for the bytes of context identifier `source`,
 * whose hash is `hash`, or RELIC_ID_NONE when they have not been copied.
 */
static uint32_t
find_copy(const collector_t *collector, uint32_t source, uint32_t hash)
{
	relic_idprobe_t probe;
	uint32_t id;

	relic_idprobe_open(&probe, &collector->copies, hash);
	while (relic_idprobe_next(&probe, &id)) {
		if (collector->copied_from[id - 1] == source)
			return (id);
	}

	return (RELIC_ID_NONE);
}

/*
 * Copy the bytes of context identifier `source` into the result's pool and
 * index the copy by `source`, whose hash is `hash`.  Returns the result's
 * identifier for them, or RELIC_ID_NONE when memory ran out.  Every room is
 * made before the copy is numbered, so a failure leaves each earlier copy
 * findable.
 *
 * The bytes are new to the result, since the context holds them under one
 * identifier alone and that identifier was not found, so the pool numbers
 * them next; the pool's own lookup by bytes merely confirms it.
 */
static uint32_t
copy_bytes(collector_t *collector, uint32_t source, uint32_t hash)
{
	relic_symtab_t *strings;
	uint32_t *copied_from;
	const char *bytes;
	size_t length;
	uint32_t id;

	strings = &collector->result->strings;
	bytes = relic_string(collector->ctx, source, &length);
	assert(bytes != NULL);

	if (relic_idtab_reserve(&collector->copies, 1, copy_rehash,
	    collector) != 0)
		return (RELIC_ID_NONE);
	copied_from = relic_grow_array(collector->copied_from,
	    &collector->copied_capacity, strings->count + 1,
	    sizeof(*collector->copied_from));
	if (copied_from == NULL)
		return (RELIC_ID_NONE);
	collector->copied_from = copied_from;

	if (relic_symtab_intern(strings, bytes, length, &id) != 0)
		return (RELIC_ID_NONE);
	assert(id == strings->count);

	collector->copied_from[id - 1] = source;
	relic_idtab_place(&collector->copies, hash, id);

	return (id);
}

/*
 * The hash a copy is indexed by, for the index to use when it grows.
 */
static uint32_t
copy_rehash(uint32_t id, void *ctx)
{
	const collector_t *collector;

	collector = ctx;

	return (relic_hash_word(collector->result->strings.hash_seed,
	    collector->copied_from[id - 1]));
}

relic_result_t *
relic_query_collect(relic_t *ctx, relic_query_t *query)
{
	relic_result_t *result;

	if (relic_enter(ctx, NULL, NULL) != 0)
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
	collector_t collector;
	relic_result_t *result;
	int status;

	result = calloc(1, sizeof(*result));
	if (result == NULL) {
		relic_diag_no_memory(ctx);
		return (NULL);
	}
	relic_symtab_init(&result->strings, ctx->symbols.hash_seed);

	bzero(&collector, sizeof(collector));
	collector.ctx = ctx;
	collector.result = result;
	relic_idtab_init(&collector.copies, false);
	status = relic_query_answer(ctx, query, store_row, &collector);
	relic_idtab_reset(&collector.copies);
	free(collector.copied_from);
	if (status != 0) {
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

const char *
relic_result_string(const relic_result_t *result, uint32_t id, size_t *lenp)
{
	if ((id & RELIC_RESULT_ID_BIT) == 0)
		return (NULL);

	return (relic_symtab_bytes(&result->strings, id & ~RELIC_RESULT_ID_BIT,
	    lenp));
}

/*
 * relic_result_string() as a reader of any source's bytes, the source being
 * the result.
 */
static const char *
result_bytes(const void *source, uint32_t id, size_t *lengthp)
{
	return (relic_result_string(source, id, lengthp));
}

/*
 * Whether a value is one of the result's: an integer, or a symbol or a
 * string whose identifier the result assigned.
 */
static bool
result_holds(const relic_result_t *result, const relic_value_t *value)
{
	switch (value->type) {
	case RELIC_INTEGER:
		return (true);
	case RELIC_SYMBOL:
	case RELIC_STRING:
		return (relic_result_string(result, value->v.id, NULL) != NULL);
	}

	return (false);
}

/*
 * A result has nowhere to report, so a value it does not hold compares
 * equal to everything, as relic_value_compare() has it compare after
 * reporting it.
 */
int
relic_result_compare(const relic_result_t *result, const relic_value_t *a,
    const relic_value_t *b)
{
	if (!result_holds(result, a) || !result_holds(result, b))
		return (0);

	return (relic_value_order_totally(a, b, result_bytes, result));
}

void
relic_result_destroy(relic_result_t *result)
{
	if (result == NULL)
		return;

	relic_symtab_reset(&result->strings);
	free(result->values);
	free(result);
}
