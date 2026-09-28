/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * Context lifecycle.
 *
 * A context owns a symbol pool, a value table, the diagnostic state, and the
 * memory account its evaluation arena and its relations are charged to.
 * Creating one is the only operation in the library that cannot report
 * through a diagnostic callback, because there is no context yet to have one
 * installed.
 *
 * The public entry points belonging to a subsystem are defined with that
 * subsystem rather than forwarded from here: relic_string() lives beside the
 * symbol pool it reads, and relic_error() beside the buffer it returns.  A
 * forwarding layer would add a place to look without adding anything to see.
 */

static int	 draw_hash_seed(uint64_t *seedp);

int
relic_enter(relic_t *ctx)
{
	if (ctx->busy) {
		relic_diag_record_refusal(ctx);
		return (-1);
	}

	ctx->busy = true;
	relic_diag_begin_operation(ctx);

	return (0);
}

/*
 * A call refused during the operation was the caller code's to learn about,
 * and the operation may yet have failed on the strength of an error reported
 * before it; once the operation returns, relic_error() describes the
 * operation again.
 */
void
relic_leave(relic_t *ctx)
{
	/*
	 * Both charged arenas are reset before the operation that filled them
	 * returns, and a round's buffers are released before the round ends,
	 * so an account still holding either has lost count.
	 */
	assert(ctx->account.arena_bytes == 0);
	assert(ctx->account.round_bytes == 0);

	ctx->busy = false;
	ctx->diag.refused = false;
}

/*
 * A context is destroyed by entering it and never leaving: nothing is left
 * to leave.
 */
void
relic_destroy(relic_t *ctx)
{
	if (ctx == NULL || relic_enter(ctx) != 0)
		return;

	relic_pool_free(ctx->pool);
	relic_store_reset(&ctx->store);
	relic_program_reset(&ctx->program);
	relic_valtab_reset(&ctx->values);
	relic_symtab_reset(&ctx->symbols);
	relic_arena_reset(&ctx->eval_arena);
	relic_arena_reset(&ctx->program_arena);
	free(ctx);
}

/*
 * Draw a context's hash seed from the operating system.  Returns 0 with a
 * nonzero seed in *seedp, or -1 when there is none to be had.
 *
 * Zero is the one value a seed may not take: reserving it is what lets the
 * hash functions tell an owner that was never given a seed from one that
 * was.  A zero draw, one in 2^64, is drawn again.  A second is not: two in a
 * row happen by chance with probability 2^-128, and mean a source that hands
 * out nothing but zeros -- a random device replaced by /dev/zero -- which
 * would otherwise be asked forever.
 */
static int
draw_hash_seed(uint64_t *seedp)
{
	uint64_t seed;
	int draw;

	for (draw = 0; draw < 2; draw++) {
		if (getentropy(&seed, sizeof(seed)) != 0)
			return (-1);

		if (seed != 0) {
			*seedp = seed;
			return (0);
		}
	}

	return (-1);
}

relic_t *
relic_create_config(const relic_config_t *config)
{
	relic_t *ctx;
	uint64_t hash_seed;

	/*
	 * A thread count past the maximum is refused rather than lowered to
	 * it: a caller who wrote one meant something else.
	 */
	if (config != NULL && config->threads > RELIC_THREADS_MAX)
		return (NULL);

	/*
	 * Without a seed nobody else can know, every table is open to keys
	 * crafted to collide, and a constant in its place would be that
	 * weakness with a false assurance attached.  So a system that cannot
	 * supply entropy gets no context, as one that cannot supply memory
	 * does not.
	 */
	if (draw_hash_seed(&hash_seed) != 0)
		return (NULL);

	ctx = malloc(sizeof(*ctx));
	if (ctx == NULL)
		return (NULL);

	bzero(ctx, sizeof(*ctx));
	ctx->hash_seed = hash_seed;

	/*
	 * A zeroed configuration means every limit unlimited, so a caller
	 * that passes none gets the same context as one that passes a zeroed
	 * structure.
	 */
	if (config != NULL)
		ctx->config = *config;
	ctx->config.threads = relic_pool_size(ctx->config.threads);

	relic_account_init(&ctx->account, ctx->config.max_memory);
	relic_arena_init(&ctx->program_arena, RELIC_ARENA_ANY_OBJECT);
	relic_arena_init_charged(&ctx->eval_arena, &ctx->account);
	relic_symtab_init(&ctx->symbols, ctx->hash_seed);
	relic_valtab_init(&ctx->values, ctx->hash_seed);
	relic_ast_program_init(&ctx->ast);
	relic_program_init(&ctx->program, ctx->hash_seed);
	relic_store_init(&ctx->store);
	relic_diag_init(&ctx->diag);

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
