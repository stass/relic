/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_ARENA_H
#define RELIC_ARENA_H

#include <stdbool.h>
#include <stddef.h>

#include "queue.h"

typedef struct relic_account relic_account_t;

/*
 * The region allocator, in a header of its own so that a structure owning an
 * arena can embed one wherever that structure is declared.
 */

typedef struct relic_arena relic_arena_t;
typedef struct relic_arena_chunk relic_arena_chunk_t;
typedef struct relic_arena_chunk_list relic_arena_chunk_list_t;
typedef struct relic_arena_mark relic_arena_mark_t;
typedef struct relic_arena_projection relic_arena_projection_t;

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
 *
 * Every allocation is rounded up to `alignment`, which the creator chooses:
 * RELIC_ARENA_ANY_OBJECT for an arena whose blocks hold structures, and one
 * for an arena of byte strings, whose blocks then follow one another with
 * no gap.  A reset keeps the alignment, as it keeps the account.
 */
struct relic_arena {
	relic_arena_chunk_list_t	 chunks;	/* newest first */
	relic_account_t			*account;	/* NULL: not charged */
	size_t				 alignment;	/* a power of two */
	size_t				 next_chunk_size;
	size_t				 total_bytes;	/* from the allocator */
	bool				 failed;
};

/*
 * The alignment of an arena whose blocks may hold any object, the strictest
 * any object needs.  A charged arena always has it.
 */
#define	RELIC_ARENA_ANY_OBJECT	_Alignof(max_align_t)

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
void	 relic_arena_init(relic_arena_t *arena, size_t alignment);
void	 relic_arena_init_charged(relic_arena_t *arena,
	    relic_account_t *account);
void	 relic_arena_reset(relic_arena_t *arena);
size_t	 relic_arena_total_bytes(const relic_arena_t *arena);

/*
 * What an arena would obtain from the allocator for a sequence of
 * allocations, found without making them, so that an owner can ask a limit
 * about a growth before it happens.  relic_arena_project() starts a
 * projection from the arena as it is, and relic_arena_project_alloc()
 * advances it by one allocation of `size` bytes, deciding as
 * relic_arena_alloc() would whether it takes a chunk and of what size.
 * `total_bytes` is then what relic_arena_total_bytes() would give after
 * the same allocations, or SIZE_MAX when a size would not have been
 * countable.  A failed arena obtains nothing, since it refuses every
 * request, so a projection is only asked of an arena that has not failed.
 */
struct relic_arena_projection {
	size_t	alignment;
	size_t	head_room;	/* free bytes in the chunk at the head */
	size_t	next_chunk_size;
	size_t	total_bytes;
	bool	has_head;
};

void	 relic_arena_project(const relic_arena_t *arena,
	    relic_arena_projection_t *projection);
void	 relic_arena_project_alloc(relic_arena_projection_t *projection,
	    size_t size);

/*
 * A point an arena can be returned to, for an owner that makes a sequence
 * of allocations that must take effect together or not at all.
 * relic_arena_mark() records the arena as it is; relic_arena_rewind()
 * frees every chunk obtained since, gives back its charge, and restores the
 * head chunk's room and the progression of chunk sizes, so that the arena
 * is as it was at the mark and its next allocations are placed as they
 * would have been.  Everything allocated since the mark is gone.  The
 * failure flag is not part of the mark: an owner that rewinds after a
 * refused allocation clears the failure itself.  Between
 * the two the arena may only be allocated from: a reset, or a second rewind
 * to an earlier mark, leaves the mark meaningless.
 *
 * A chunk obtained since the mark is either in front of the head chunk the
 * mark recorded, or, made oversize, linked right behind whichever chunk was
 * the head then, which is the recorded head itself or a chunk in front of
 * it.  So the chunks to free are those in front of the recorded head and
 * those between it and the chunk that followed it at the mark.
 */
struct relic_arena_mark {
	relic_arena_chunk_t	*head;		/* NULL: the arena was empty */
	relic_arena_chunk_t	*after_head;
	size_t			 head_used;
	size_t			 next_chunk_size;
	size_t			 total_bytes;
};

void	 relic_arena_mark(const relic_arena_t *arena, relic_arena_mark_t *mark);
void	 relic_arena_rewind(relic_arena_t *arena,
	    const relic_arena_mark_t *mark);

#endif /* RELIC_ARENA_H */
