/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_ARENA_H
#define RELIC_ARENA_H

#include <stdbool.h>
#include <stddef.h>

#include "account.h"
#include "queue.h"

/*
 * The region allocator, in a header of its own so that a structure owning an
 * arena can embed one wherever that structure is declared.
 */

typedef struct relic_arena relic_arena_t;
typedef struct relic_arena_chunk relic_arena_chunk_t;
typedef struct relic_arena_chunk_list relic_arena_chunk_list_t;

/*
 * One allocation holding this header and, after it, the payload that
 * relic_arena_alloc() hands out in pieces.
 */
struct relic_arena_chunk {
	SLIST_ENTRY(relic_arena_chunk)	 link;
	size_t				 size;	/* payload bytes */
	size_t				 used;	/* handed out, at most size */
};

SLIST_HEAD(relic_arena_chunk_list, relic_arena_chunk);

/*
 * A region whose allocations share a lifetime and are released together.
 *
 * An arena may be moved by assigning the structure, which moves ownership of
 * every chunk with it: the chunks are reached only through the list head and
 * none refers back to the structure holding it.  The source of a move must
 * then be reinitialized rather than reset, since resetting it would free the
 * chunks the destination now owns.  A representation that gave a chunk a
 * pointer to its list head would have to replace moving with a function.
 *
 * An arena is charged or not, which its creator decides by which of the two
 * initializations it calls.  A charged arena counts every chunk it holds in
 * `account`, asks the account before it obtains one, and is refused a chunk
 * the account's limit does not admit exactly as it would be refused one the
 * allocator could not supply.  A reset gives its chunks back to the account
 * and keeps it charged.  An arena holding what the caller wrote is not
 * charged, and its `account` is NULL.  A moved arena takes its account with
 * it.
 *
 * Once an allocation fails, `failed` stays set and every later request is
 * refused without reaching the allocator.  Callers check the returned
 * pointer as usual; the flag lets a helper that is already several
 * allocations into a structure stop without threading a status back through
 * every level, and it lets the phase that owns the arena confirm at its
 * boundary that nothing was quietly truncated.
 */
struct relic_arena {
	relic_arena_chunk_list_t	 chunks;	/* newest first */
	relic_account_t			*account;	/* NULL: not charged */
	size_t				 next_chunk_size;
	size_t				 total_bytes;	/* from the allocator */
	bool				 failed;
};

/*
 * relic_arena_reset() returns an arena to its initialized state and releases
 * all of its memory, so it serves both as a reset between phases and as the
 * teardown operation; a charged arena stays charged to the same account.
 * There is no per-allocation free: the point of an arena is that a phase's
 * allocations are forgotten together.
 */
void	*relic_arena_alloc(relic_arena_t *arena, size_t size);
void	*relic_arena_allocarray(relic_arena_t *arena, size_t count,
	    size_t size);
void	 relic_arena_clear_failure(relic_arena_t *arena);
void	 relic_arena_fail(relic_arena_t *arena);
bool	 relic_arena_failed(const relic_arena_t *arena);
void	 relic_arena_init(relic_arena_t *arena);
void	 relic_arena_init_charged(relic_arena_t *arena,
	    relic_account_t *account);
void	 relic_arena_reset(relic_arena_t *arena);
size_t	 relic_arena_total_bytes(const relic_arena_t *arena);

#endif /* RELIC_ARENA_H */
