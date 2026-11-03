/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <errno.h>
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
relic_enter(relic_t *ctx, const char *verb, const char *subject)
{
	if (ctx->busy) {
		relic_diag_record_refusal(ctx);
		return (-1);
	}

	ctx->busy = true;
	relic_signal_flag_lower(&ctx->interrupted);
	relic_diag_begin_operation(ctx, verb, subject);

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
 * Ask the operation in progress to stop.  Raising the flag is all this
 * does, so a signal handler, another thread or caller code the operation
 * runs may call it: it neither enters the context nor reads anything else
 * of it.  An operation that begins afterwards lowers the flag, so an
 * interrupt with no operation in progress stops nothing.
 */
void
relic_interrupt(relic_t *ctx)
{
	if (ctx != NULL)
		relic_signal_flag_raise(&ctx->interrupted);
}

/*
 * A context is destroyed by entering it and never leaving: nothing is left
 * to leave.
 */
void
relic_destroy(relic_t *ctx)
{
	if (ctx == NULL || relic_enter(ctx, NULL, NULL) != 0)
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
 * nonzero seed in *seedp, or -1 with errno set when there is none to be
 * had: as getentropy(2) set it, or EIO for a source of nothing but zeros.
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

	errno = EIO;
	return (-1);
}

relic_t *
relic_create_config(const relic_config_t *config, size_t size)
{
	relic_config_t ours;
	relic_t *ctx;
	uint64_t hash_seed;
	int error;

	/*
	 * A zeroed configuration means every limit unlimited, so a caller
	 * that passes none gets the same context as one that passes a zeroed
	 * structure.
	 */
	bzero(&ours, sizeof(ours));
	if (config != NULL) {
		error = relic_struct_copy_in(config, size, &ours, sizeof(ours),
		    RELIC_CONFIG_SIZE_1_0);
		if (error != 0) {
			errno = error;
			return (NULL);
		}
	}

	/*
	 * A thread count past the maximum is refused rather than lowered to
	 * it: a caller who wrote one meant something else.
	 */
	if (ours.threads > RELIC_THREADS_MAX) {
		errno = EINVAL;
		return (NULL);
	}

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
	ctx->config = ours;
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
	relic_signal_flag_init(&ctx->interrupted);

	return (ctx);
}

relic_t *
relic_create(void)
{
	return (relic_create_config(NULL, 0));
}

int
relic_struct_copy_in(const void *theirs, size_t their_size, void *ours,
    size_t our_size, size_t oldest_size)
{
	const unsigned char *bytes;
	size_t known, i;

	if (their_size < oldest_size)
		return (EINVAL);

	bytes = theirs;
	for (i = our_size; i < their_size; i++) {
		if (bytes[i] != 0)
			return (E2BIG);
	}

	known = their_size < our_size ? their_size : our_size;
	bzero(ours, our_size);
	bcopy(theirs, ours, known);

	return (0);
}

void
relic_struct_copy_out(const void *ours, size_t our_size, void *theirs,
    size_t their_size, size_t oldest_size)
{
	size_t known;

	bzero(theirs, their_size);
	if (their_size < oldest_size)
		return;

	known = their_size < our_size ? their_size : our_size;
	bcopy(ours, theirs, known);
}

const char *
relic_version(void)
{
	return (RELIC_VERSION_STRING);
}
