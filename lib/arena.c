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
 * A region allocator for structures that share a lifetime.
 *
 * An arena is a list of chunks.  A chunk is one allocation holding a header
 * and, after it, a payload that allocation hands out by bumping an offset.
 * Nothing is freed individually; relic_arena_reset() releases every chunk
 * together and returns the arena to its initial state.  A charged arena's
 * account counts exactly the bytes `total_bytes` counts: a chunk is charged
 * before it is obtained, the charge is given back if the allocator refuses
 * it, and a reset gives back the whole total.
 *
 * The strictest alignment any object needs is used for every allocation.
 * Rounding each request up to it means the running offset is always aligned,
 * so no allocation has to inspect or repair alignment, and the payload itself
 * starts at the first correctly aligned offset past the header.
 */

#define	RELIC_ARENA_ALIGNMENT	 _Alignof(max_align_t)

/*
 * The payload of the first chunk, and the ceiling that doubling stops at.
 * The first is small because a context that interns a handful of symbols
 * should not reserve a page it will not use; the ceiling exists because past
 * a point a larger chunk only rounds the arena's footprint up.
 */
#define	RELIC_ARENA_FIRST_CHUNK	 4096u
#define	RELIC_ARENA_MAX_CHUNK	 (256u * 1024u)

static relic_arena_chunk_t	*arena_add_chunk(relic_arena_t *arena,
				    size_t size, bool oversize);
static bool			 charge(relic_arena_t *arena, size_t bytes);
static void			 uncharge(relic_arena_t *arena, size_t bytes);
static unsigned char		*chunk_payload(relic_arena_chunk_t *chunk);
static size_t			 round_up(size_t size, size_t alignment);

/*
 * Round size up to a multiple of alignment, which must be a power of two.
 *
 * The caller must already have established that the addition cannot
 * overflow.  Returning some in-band failure value instead would be worse
 * than the check it saves: every plausible one is also a legitimate rounded
 * size, so a caller could not tell a refusal from a result.
 */
static size_t
round_up(size_t size, size_t alignment)
{
	return ((size + alignment - 1) & ~(alignment - 1));
}

/*
 * The first byte of a chunk's payload.  The header is rounded up to the
 * strictest alignment so that what follows it is aligned for any object.
 */
static unsigned char *
chunk_payload(relic_arena_chunk_t *chunk)
{
	return ((unsigned char *)chunk +
	    round_up(sizeof(*chunk), RELIC_ARENA_ALIGNMENT));
}

/*
 * Count `bytes` more against a charged arena's account, or return false when
 * its limit refuses them.  An arena that is not charged is always admitted.
 */
static bool
charge(relic_arena_t *arena, size_t bytes)
{
	return (arena->account == NULL ||
	    relic_account_charge(arena->account, bytes));
}

static void
uncharge(relic_arena_t *arena, size_t bytes)
{
	if (arena->account != NULL)
		relic_account_release(arena->account, bytes);
}

/*
 * Obtain a chunk able to hold an aligned allocation of `size` bytes and link
 * it into the arena, or return NULL and mark the arena failed.  A chunk the
 * account does not admit is never asked of the allocator.
 *
 * A chunk that had to be made oversize for one large request is linked after
 * the head rather than in front of it.  Pushing it to the head would abandon
 * whatever space the current head still has, and since the oversize chunk is
 * full the moment it is created, every small allocation that followed would
 * need a chunk of its own.  Linking it behind leaves the partially used chunk
 * in front, where the next small allocation finds its remaining space.
 */
static relic_arena_chunk_t *
arena_add_chunk(relic_arena_t *arena, size_t size, bool oversize)
{
	relic_arena_chunk_t *chunk;
	relic_arena_chunk_t *head;
	size_t header_size;
	size_t payload_size;
	size_t total;

	header_size = round_up(sizeof(*chunk), RELIC_ARENA_ALIGNMENT);
	payload_size = oversize ? size : arena->next_chunk_size;

	if (payload_size > SIZE_MAX - header_size) {
		arena->failed = true;
		return (NULL);
	}
	total = header_size + payload_size;

	if (!charge(arena, total)) {
		arena->failed = true;
		return (NULL);
	}

	chunk = malloc(total);
	if (chunk == NULL) {
		uncharge(arena, total);
		arena->failed = true;
		return (NULL);
	}

	chunk->size = payload_size;
	chunk->used = 0;

	head = SLIST_FIRST(&arena->chunks);
	if (head != NULL && oversize)
		SLIST_INSERT_AFTER(head, chunk, link);
	else
		SLIST_INSERT_HEAD(&arena->chunks, chunk, link);

	arena->total_bytes += total;

	/*
	 * The progression doubles per chunk, to a ceiling, and it tracks how
	 * fast the arena is filling rather than how large any one request
	 * was: an oversize chunk advances it by the same step as an ordinary
	 * one, and never to the oversize chunk's own size.
	 */
	if (arena->next_chunk_size < RELIC_ARENA_MAX_CHUNK)
		arena->next_chunk_size *= 2;

	return (chunk);
}

void
relic_arena_init(relic_arena_t *arena)
{
	bzero(arena, sizeof(*arena));
	SLIST_INIT(&arena->chunks);
	arena->next_chunk_size = RELIC_ARENA_FIRST_CHUNK;
}

void
relic_arena_init_charged(relic_arena_t *arena, relic_account_t *account)
{
	relic_arena_init(arena);
	arena->account = account;
}

void
relic_arena_reset(relic_arena_t *arena)
{
	relic_arena_chunk_t *chunk;
	relic_arena_chunk_t *next;
	relic_account_t *account;

	SLIST_FOREACH_SAFE(chunk, &arena->chunks, link, next)
		free(chunk);
	uncharge(arena, arena->total_bytes);

	account = arena->account;
	relic_arena_init_charged(arena, account);
}

/*
 * Allocate `size` bytes that live until the arena is reset, or return NULL.
 *
 * A request of zero bytes is rounded up like any other and yields a distinct
 * usable pointer, so a null return means the allocation failed and nothing
 * else.
 *
 * A failed arena refuses every later request without reaching the allocator.
 * That keeps the failure flag from being contradicted by a small allocation
 * that happens to succeed afterwards, and stops a construction that is
 * already unwinding from asking an allocator that has said no once.
 */
void *
relic_arena_alloc(relic_arena_t *arena, size_t size)
{
	relic_arena_chunk_t *chunk;
	unsigned char *block;
	size_t needed;

	if (arena->failed)
		return (NULL);

	if (size > SIZE_MAX - (RELIC_ARENA_ALIGNMENT - 1)) {
		arena->failed = true;
		return (NULL);
	}

	needed = round_up(size, RELIC_ARENA_ALIGNMENT);
	if (needed == 0)
		needed = RELIC_ARENA_ALIGNMENT;

	chunk = SLIST_FIRST(&arena->chunks);
	if (chunk == NULL || needed > chunk->size - chunk->used) {
		chunk = arena_add_chunk(arena, needed,
		    needed > arena->next_chunk_size);
		if (chunk == NULL)
			return (NULL);
	}

	block = chunk_payload(chunk) + chunk->used;
	chunk->used += needed;

	return (block);
}

/*
 * Allocate an array of `count` elements of `size` bytes each.
 *
 * The product is checked rather than assumed, because a wrapped product would
 * be answered with a block smaller than what the caller goes on to write into
 * it.  A product that would wrap marks the arena failed, exactly as an
 * exhausted allocator does, so that a null return has one meaning and the
 * phase boundary that reports a failed arena reports this too.
 */
void *
relic_arena_allocarray(relic_arena_t *arena, size_t count, size_t size)
{
	if (size != 0 && count > SIZE_MAX / size) {
		arena->failed = true;
		return (NULL);
	}

	return (relic_arena_alloc(arena, count * size));
}

bool
relic_arena_failed(const relic_arena_t *arena)
{
	return (arena->failed);
}

/*
 * Record that an allocation for this arena could not be satisfied, although
 * the arena itself was never asked for it.
 *
 * An owner sometimes makes an allocation elsewhere on the arena's behalf --
 * a growable array beside the structure the arena holds is the case that
 * exists -- and a failure there would otherwise mean something different
 * from a null return out of relic_arena_alloc(): the first would leave the
 * flag clear and the phase boundary would see nothing wrong.  Marking the
 * arena instead gives a refused allocation one meaning wherever it was
 * decided, so the boundary check stays the single place that turns any of
 * them into a reported failure.
 */
void
relic_arena_fail(relic_arena_t *arena)
{
	arena->failed = true;
}

/*
 * Forget that an allocation failed, so the arena serves requests again.
 *
 * This belongs to an owner that has observed the failure and reported it,
 * and whose arena is not about to be reset.  An arena that is reset between
 * phases needs none of this: the reset clears the flag along with the
 * memory.  An arena that lives as long as its context does, and so is never
 * reset, would otherwise turn one transient exhaustion into a context that
 * can never allocate again.
 */
void
relic_arena_clear_failure(relic_arena_t *arena)
{
	arena->failed = false;
}

/*
 * Bytes this arena has obtained from the allocator, chunk headers included.
 * This is its footprint rather than the total it has handed out: the
 * difference is the space lost to alignment and to the tail of a chunk that
 * an allocation did not fit into.
 */
size_t
relic_arena_total_bytes(const relic_arena_t *arena)
{
	return (arena->total_bytes);
}
