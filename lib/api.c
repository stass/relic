/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * Context lifecycle.
 *
 * A context owns an arena for everything whose lifetime is the context's own,
 * a symbol pool, a value table and the diagnostic state.  Creating one is the
 * only operation in the library that cannot report through a diagnostic
 * callback, because there is no context yet to have one installed.
 *
 * The public entry points belonging to a subsystem are defined with that
 * subsystem rather than forwarded from here: relic_string() lives beside the
 * symbol pool it reads, and relic_error() beside the buffer it returns.  A
 * forwarding layer would add a place to look without adding anything to see.
 */

void *
relic_context_alloc(relic_t *ctx, size_t size)
{
	void *block;

	block = relic_arena_alloc(&ctx->arena, size);
	if (block != NULL)
		return (block);

	relic_arena_clear_failure(&ctx->arena);
	relic_diag_no_memory(ctx);

	return (NULL);
}

void
relic_destroy(relic_t *ctx)
{
	if (ctx == NULL)
		return;

	relic_valtab_reset(&ctx->values);
	relic_symtab_reset(&ctx->symbols);
	relic_arena_reset(&ctx->program_arena);
	relic_arena_reset(&ctx->arena);
	free(ctx);
}

relic_t *
relic_create_config(const relic_config_t *config)
{
	relic_t *ctx;

	ctx = malloc(sizeof(*ctx));
	if (ctx == NULL)
		return (NULL);

	bzero(ctx, sizeof(*ctx));
	relic_arena_init(&ctx->arena);
	relic_arena_init(&ctx->program_arena);
	relic_symtab_init(&ctx->symbols);
	relic_valtab_init(&ctx->values);
	relic_ast_program_init(&ctx->ast);
	relic_diag_init(&ctx->diag);

	/*
	 * A zeroed configuration means every limit unlimited, so a caller
	 * that passes none gets the same context as one that passes a zeroed
	 * structure.
	 */
	if (config != NULL)
		ctx->config = *config;

	return (ctx);
}

relic_t *
relic_create(void)
{
	return (relic_create_config(NULL));
}

const char *
relic_version(void)
{
	return (RELIC_VERSION_STRING);
}
