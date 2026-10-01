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
 * Every request is rounded up to the arena's alignment.  That keeps the
 * running offset aligned, so no allocation has to inspect or repair
 * alignment, and the payload itself starts at the first offset past the
 * header aligned for any object, which serves every alignment an arena may
 * have.
 */

/*
 * The payload of the first chunk, and the ceiling that doubling stops at.
 * The first is small because a context that interns a handful of symbols
 * should not reserve a page it will not use; the ceiling exists because past
 * a point a larger chunk only rounds the arena's footprint up.
 */
#define	RELIC_ARENA_FIRST_CHUNK	 4096u
#define	RELIC_ARENA_MAX_CHUNK	 (256u * 1024u)

typedef struct arena_placement arena_placement_t;

/*
 * Where an allocation goes: `needed` bytes, its size rounded to the
 * arena's alignment, taken from the head chunk when `chunk_payload` is
 * zero, and otherwise from a new chunk of that payload, which becomes the
 * head when `becomes_head` says so and is linked behind it when not.
 */
struct arena_placement {
	size_t	needed;
	size_t	chunk_payload;
	bool	becomes_head;
};

static relic_arena_chunk_t	*arena_add_chunk(relic_arena_t *arena,
				    const arena_placement_t *placement);
static bool			 arena_place(size_t alignment, bool has_head,
				    size_t head_room, size_t next_chunk_size,
				    size_t size, arena_placement_t *placement);
static size_t			 chunk_header_size(void);
static size_t			 next_chunk_size_after(size_t size);
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
 * The bytes of a chunk's header, rounded up to the strictest alignment so
 * that the payload after it is aligned for any object.
 */
static size_t
chunk_header_size(void)
{
	return (round_up(sizeof(relic_arena_chunk_t), RELIC_ARENA_ANY_OBJECT));
}

/*
 * The first byte of a chunk's payload.
 */
static unsigned char *
chunk_payload(relic_arena_chunk_t *chunk)
{
	return ((unsigned char *)chunk + chunk_header_size());
}

/*
 * The payload of the chunk after one of `size`: double, to a ceiling.  The
 * progression tracks how fast the arena is filling rather than how large
 * any one request was: an oversize chunk advances it by the same step as an
 * ordinary one, and never to the oversize chunk's own size.
 */
static size_t
next_chunk_size_after(size_t size)
{
	return (size < RELIC_ARENA_MAX_CHUNK ? size * 2 : size);
}

/*
 * Decide where an allocation of `size` bytes goes, from the arena's state
 * alone: whether it has a head chunk, the room the head has, and the
 * payload the next chunk would have.  relic_arena_alloc() and a projection
 * both decide here, so that a projection makes the choices the allocations
 * will.  Returns false when the size, rounded or with a chunk's header,
 * would not fit a size_t.
 *
 * A request of zero bytes is rounded up to one unit of alignment, so that
 * it yields a distinct usable pointer.  A request that fits the head's room
 * takes it from there.  One that does not takes a new chunk of the next
 * size, or of its own size when it is larger than that.  A chunk made
 * oversize for one large request is linked after the head rather than in
 * front of it: pushing it to the head would abandon whatever space the
 * current head still has, and since the oversize chunk is full the moment
 * it is created, every small allocation that followed would need a chunk of
 * its own.  Linking it behind leaves the partially used chunk in front,
 * where the next small allocation finds its remaining space.
 */
static bool
arena_place(size_t alignment, bool has_head, size_t head_room,
    size_t next_chunk_size, size_t size, arena_placement_t *placement)
{
	bool oversize;

	if (size > SIZE_MAX - (alignment - 1))
		return (false);

	placement->needed = round_up(size, alignment);
	if (placement->needed == 0)
		placement->needed = alignment;

	placement->chunk_payload = 0;
	placement->becomes_head = false;
	if (has_head && placement->needed <= head_room)
		return (true);

	oversize = placement->needed > next_chunk_size;
	placement->chunk_payload = oversize ? placement->needed :
	    next_chunk_size;
	placement->becomes_head = !has_head || !oversize;

	return (placement->chunk_payload <= SIZE_MAX - chunk_header_size());
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
 * Obtain the chunk a placement asks for and link it where the placement
 * says, or return NULL and mark the arena failed.  A chunk the account does
 * not admit is never asked of the allocator.
 */
static relic_arena_chunk_t *
arena_add_chunk(relic_arena_t *arena, const arena_placement_t *placement)
{
	relic_arena_chunk_t *chunk;
	relic_arena_chunk_t *head;
	size_t total;

	total = chunk_header_size() + placement->chunk_payload;
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

	chunk->size = placement->chunk_payload;
	chunk->used = 0;

	head = SLIST_FIRST(&arena->chunks);
	if (placement->becomes_head)
		SLIST_INSERT_HEAD(&arena->chunks, chunk, link);
	else
		SLIST_INSERT_AFTER(head, chunk, link);

	arena->total_bytes += total;
	arena->next_chunk_size = next_chunk_size_after(arena->next_chunk_size);

	return (chunk);
}

void
relic_arena_init(relic_arena_t *arena, size_t alignment)
{
	assert(alignment > 0 && (alignment & (alignment - 1)) == 0 &&
	    alignment <= RELIC_ARENA_ANY_OBJECT);

	bzero(arena, sizeof(*arena));
	SLIST_INIT(&arena->chunks);
	arena->alignment = alignment;
	arena->next_chunk_size = RELIC_ARENA_FIRST_CHUNK;
}

void
relic_arena_init_charged(relic_arena_t *arena, relic_account_t *account)
{
	relic_arena_init(arena, RELIC_ARENA_ANY_OBJECT);
	arena->account = account;
}

void
relic_arena_reset(relic_arena_t *arena)
{
	relic_arena_chunk_t *chunk;
	relic_arena_chunk_t *next;
	relic_account_t *account;
	size_t alignment;

	SLIST_FOREACH_SAFE(chunk, &arena->chunks, link, next)
		free(chunk);
	uncharge(arena, arena->total_bytes);

	account = arena->account;
	alignment = arena->alignment;
	relic_arena_init(arena, alignment);
	arena->account = account;
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
	arena_placement_t placement;
	relic_arena_chunk_t *chunk;
	unsigned char *block;

	if (arena->failed)
		return (NULL);

	chunk = SLIST_FIRST(&arena->chunks);
	if (!arena_place(arena->alignment, chunk != NULL,
	    chunk != NULL ? chunk->size - chunk->used : 0,
	    arena->next_chunk_size, size, &placement)) {
		arena->failed = true;
		return (NULL);
	}

	if (placement.chunk_payload > 0) {
		chunk = arena_add_chunk(arena, &placement);
		if (chunk == NULL)
			return (NULL);
	}

	block = chunk_payload(chunk) + chunk->used;
	chunk->used += placement.needed;

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

void
relic_arena_mark(const relic_arena_t *arena, relic_arena_mark_t *mark)
{
	mark->head = SLIST_FIRST(&arena->chunks);
	mark->after_head = mark->head != NULL ?
	    SLIST_NEXT(mark->head, link) : NULL;
	mark->head_used = mark->head != NULL ? mark->head->used : 0;
	mark->next_chunk_size = arena->next_chunk_size;
	mark->total_bytes = arena->total_bytes;
}

void
relic_arena_rewind(relic_arena_t *arena, const relic_arena_mark_t *mark)
{
	relic_arena_chunk_t *chunk;

	while ((chunk = SLIST_FIRST(&arena->chunks)) != mark->head) {
		assert(chunk != NULL);
		SLIST_REMOVE_HEAD(&arena->chunks, link);
		free(chunk);
	}

	if (mark->head != NULL) {
		while ((chunk = SLIST_NEXT(mark->head, link)) !=
		    mark->after_head) {
			assert(chunk != NULL);
			SLIST_REMOVE_AFTER(mark->head, link);
			free(chunk);
		}
		mark->head->used = mark->head_used;
	}

	assert(arena->total_bytes >= mark->total_bytes);
	uncharge(arena, arena->total_bytes - mark->total_bytes);
	arena->total_bytes = mark->total_bytes;
	arena->next_chunk_size = mark->next_chunk_size;
}

void
relic_arena_project(const relic_arena_t *arena,
    relic_arena_projection_t *projection)
{
	const relic_arena_chunk_t *head;

	assert(!arena->failed);

	head = SLIST_FIRST(&arena->chunks);
	projection->alignment = arena->alignment;
	projection->has_head = head != NULL;
	projection->head_room = head != NULL ? head->size - head->used : 0;
	projection->next_chunk_size = arena->next_chunk_size;
	projection->total_bytes = arena->total_bytes;
}

/*
 * One allocation's placement, decided as relic_arena_alloc() decides it,
 * applied to the projection's state in place of the chunks.
 */
void
relic_arena_project_alloc(relic_arena_projection_t *projection, size_t size)
{
	arena_placement_t placement;

	if (projection->total_bytes == SIZE_MAX)
		return;

	if (!arena_place(projection->alignment, projection->has_head,
	    projection->head_room, projection->next_chunk_size, size,
	    &placement)) {
		projection->total_bytes = SIZE_MAX;
		return;
	}

	if (placement.chunk_payload == 0) {
		projection->head_room -= placement.needed;
		return;
	}

	projection->total_bytes = relic_saturating_sum(projection->total_bytes,
	    chunk_header_size() + placement.chunk_payload);
	if (placement.becomes_head) {
		projection->head_room = placement.chunk_payload -
		    placement.needed;
		projection->has_head = true;
	}
	projection->next_chunk_size = next_chunk_size_after(
	    projection->next_chunk_size);
}
