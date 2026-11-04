/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <emscripten.h>
#include <relic.h>

/* The adapter does not include the engine's private attribute header. */
#define RELIC_UNUSED __attribute__((__unused__))

/*
 * C owns the context and reads C layouts; JavaScript owns bounded copies.
 * Every bridge callback uses the current heap view, since an engine call
 * can grow linear memory. A sticky copy failure stops subsequent stages.
 */
EM_JS(int, bridge_failed, (void), {
	return Module['bridge'].failed;
});

EM_JS(int, bridge_query, (void), {
	return Module['bridge'].query();
});

EM_JS(int, bridge_row, (size_t count), {
	return Module['bridge'].row(count);
});

EM_JS(void, bridge_integer, (int64_t value), {
	Module['bridge'].integer(value);
});

EM_JS(void, bridge_bytes, (int type, const char *bytes, size_t length), {
	Module['bridge'].bytes(type, bytes, length, HEAPU8);
});

EM_JS(void, bridge_diagnostic, (int level, const char *message,
    size_t length, const char *file, size_t file_length,
    unsigned int line, unsigned int column), {
	Module['bridge'].diagnostic(level, message, length, file, file_length,
	    line, column, HEAPU8);
});

static void	 diagnostic(void *arg, enum relic_diag_level level,
		    const relic_location_t *where, const char *message);
static int	 row(void *arg, const relic_value_t *values, size_t count);

int	 relic_wasm_evaluate(const char *program, size_t max_input,
	    size_t max_memory, size_t max_tuples, size_t max_magic);

static void
diagnostic(void *arg RELIC_UNUSED, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	const char *file;
	unsigned int column, line;

	file = where != NULL ? where->file : NULL;
	line = where != NULL ? where->line : 0;
	column = where != NULL ? where->column : 0;
	bridge_diagnostic(level, message, strlen(message), file,
	    file != NULL ? strlen(file) : 0, line, column);
}

static int
row(void *arg, const relic_value_t *values, size_t count)
{
	relic_t *ctx = arg;
	const char *bytes;
	size_t i, length;

	if (bridge_row(count) != 0)
		return (1);
	for (i = 0; i < count; i++) {
		if (values[i].type == RELIC_INTEGER) {
			bridge_integer(values[i].v.integer);
		} else {
			bytes = relic_string(ctx, values[i].v.id, &length);
			bridge_bytes(values[i].type, bytes, length);
		}
		if (bridge_failed() != 0)
			return (1);
	}
	return (0);
}

/* Return a C error class, captured before cleanup can change error state. */
int
relic_wasm_evaluate(const char *program, size_t max_input, size_t max_memory,
    size_t max_tuples, size_t max_magic)
{
	relic_config_t config = { 0 };
	relic_query_t *query = NULL;
	relic_t *ctx;
	size_t count, i;
	int error = RELIC_ERROR_NONE;

	config.max_input = max_input;
	config.max_memory = max_memory;
	config.max_tuples = max_tuples;
	config.max_magic = max_magic;
	config.threads = 1;
	ctx = relic_create_config(&config, sizeof(config));
	if (ctx == NULL)
		return (errno == ENOMEM ? RELIC_ERROR_MEMORY : RELIC_ERROR_INTERNAL);
	if (relic_diag_set(ctx, diagnostic, NULL) != 0)
		goto failed;
	if (relic_load_string(ctx, "program", program) != 0)
		goto failed;
	if (bridge_failed() != 0)
		goto done;
	if (relic_check(ctx) != 0)
		goto failed;
	if (bridge_failed() != 0)
		goto done;

	count = relic_query_count(ctx);
	for (i = 0; i < count; i++) {
		if (bridge_query() != 0)
			goto done;
		query = relic_query_loaded(ctx, i);
		if (query == NULL)
			goto failed;
		if (bridge_failed() != 0)
			goto done;
		if (relic_query_run(ctx, query, row, ctx) != 0)
			goto failed;
		if (bridge_failed() != 0)
			goto done;
		relic_query_destroy(query);
		query = NULL;
	}
	goto done;

failed:
	error = relic_error_class(ctx);
	if (error == RELIC_ERROR_NONE)
		error = RELIC_ERROR_INTERNAL;
done:
	relic_query_destroy(query);
	relic_destroy(ctx);
	return (error);
}
