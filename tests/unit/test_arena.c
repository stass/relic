/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the region allocator.
 *
 * Four properties matter to everything built on it: an allocation is
 * correctly aligned and does not overlap its neighbours, a reset returns the
 * arena to a reusable empty state, a failure is both reported to the caller
 * and remembered on the arena, and a charged arena's account holds exactly
 * what the arena holds and refuses a chunk its limit does not admit.  A
 * projection of allocations not yet made must give the bytes making them
 * does, since the owner asks the limit with it before it allocates.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_ALIGNMENT	"every allocation is aligned"
#define	SCENARIO_CHARGED	"a charged arena's chunks are in its account"
#define	SCENARIO_LIMITED	"the account's limit refuses a chunk"
#define	SCENARIO_DISTINCT	"allocations do not overlap"
#define	SCENARIO_FAILURE	"an impossible request fails stickily"
#define	SCENARIO_MARKED		"an owner can record a failure of its own"
#define	SCENARIO_PACKED		"an arena aligns to its own alignment"
#define	SCENARIO_PROJECTED	"a projection gives what the allocations take"
#define	SCENARIO_REWOUND	"a rewind returns the arena to its mark"
#define	SCENARIO_RESET		"reset returns the arena to an empty state"
#define	SCENARIO_ZERO		"a zero-byte request yields a usable pointer"

static bool	 is_aligned(const void *block);
static size_t	 first_chunk_bytes(void);
static void	 a_charged_arena_is_counted_in_its_account(void);
static void	 the_limit_refuses_a_chunk_before_it_is_obtained(void);
static void	 a_reset_arena_allocates_again(void);
static void	 a_zero_byte_request_returns_a_distinct_pointer(void);
static void	 an_allocation_is_aligned_whatever_its_size(void);
static void	 an_impossible_request_fails_and_stays_failed(void);
static void	 an_arena_aligns_to_its_own_alignment(void);
static void	 an_owner_can_record_a_refusal_it_made_itself(void);
static void	 separate_allocations_do_not_overlap(void);
static void	 a_projection_gives_what_the_allocations_take(void);
static void	 a_rewind_returns_the_arena_to_its_mark(void);
static void	 a_refused_sequence_is_rewound_and_retried(void);
static unsigned char *allocate_before_mark(relic_arena_t *arena);
static bool	 pattern_intact(const unsigned char *patterned);
static size_t	 linked_payload_bytes(const relic_arena_t *arena);
static void	 allocate_sizes(relic_arena_t *arena, const size_t *sizes,
		    size_t count);

static bool
is_aligned(const void *block)
{
	return (((uintptr_t)block % _Alignof(max_align_t)) == 0);
}

/*
 * Every size, not only the convenient ones: an odd size must still leave the
 * next allocation aligned, which is the property the whole arena rests on.
 */
static void
an_allocation_is_aligned_whatever_its_size(void)
{
	static const size_t sizes[] = { 1, 3, 7, 8, 17, 64, 4095, 4096, 8193 };

	relic_arena_t arena;
	size_t i;

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		void *block;

		block = relic_arena_alloc(&arena, sizes[i]);
		checkf(block != NULL, SCENARIO_ALIGNMENT,
		    "a request of %zu bytes should succeed", sizes[i]);
		checkf(block != NULL && is_aligned(block), SCENARIO_ALIGNMENT,
		    "a block of %zu bytes should be aligned for any object",
		    sizes[i]);
	}

	check(!relic_arena_failed(&arena), SCENARIO_ALIGNMENT,
	    "no allocation should have failed");
	check(relic_arena_total_bytes(&arena) > 0, SCENARIO_ALIGNMENT,
	    "the arena should report the memory it obtained");

	relic_arena_reset(&arena);
}

/*
 * An arena created with a smaller alignment rounds each request to it and no
 * further: one of alignment one hands out each block where the last ended,
 * which is what lets a pool of short strings waste nothing, and one of
 * alignment eight rounds a three-byte request up to eight.
 */
static void
an_arena_aligns_to_its_own_alignment(void)
{
	static const size_t sizes[] = { 1, 3, 7, 2, 9 };

	enum { SIZE_COUNT = sizeof(sizes) / sizeof(sizes[0]) };

	unsigned char *blocks[SIZE_COUNT];
	unsigned char *first;
	unsigned char *second;
	relic_arena_t arena;
	size_t i;

	relic_arena_init(&arena, 1);
	for (i = 0; i < SIZE_COUNT; i++)
		blocks[i] = relic_arena_alloc(&arena, sizes[i]);
	for (i = 1; i < SIZE_COUNT; i++) {
		checkf(blocks[i - 1] != NULL && blocks[i] == blocks[i - 1] +
		    sizes[i - 1], SCENARIO_PACKED,
		    "the block of %zu bytes should begin where the block of "
		    "%zu ended", sizes[i], sizes[i - 1]);
	}
	relic_arena_reset(&arena);

	relic_arena_init(&arena, 8);
	first = relic_arena_alloc(&arena, 3);
	second = relic_arena_alloc(&arena, 3);
	check(first != NULL && second == first + 8 &&
	    (uintptr_t)second % 8 == 0, SCENARIO_PACKED,
	    "an arena of alignment eight should round three bytes to eight");
	relic_arena_reset(&arena);
}

/*
 * Fill each block with a distinct byte and read them all back afterwards.
 * Overlapping blocks, or a chunk that hands out the same offset twice, would
 * show up as a byte that changed after it was written.  The count crosses
 * several chunk boundaries so that the transition from one chunk to the next
 * is part of what is checked.
 */
static void
separate_allocations_do_not_overlap(void)
{
	enum { BLOCK_COUNT = 400, BLOCK_SIZE = 48 };

	relic_arena_t arena;
	unsigned char *blocks[BLOCK_COUNT];
	size_t first_unallocated;
	size_t first_corrupted;
	size_t i;

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);

	first_unallocated = BLOCK_COUNT;
	for (i = 0; i < BLOCK_COUNT; i++) {
		blocks[i] = relic_arena_alloc(&arena, BLOCK_SIZE);
		if (blocks[i] == NULL) {
			first_unallocated = i;
			break;
		}
		memset(blocks[i], (int)(i % 251), BLOCK_SIZE);
	}

	checkf(first_unallocated == BLOCK_COUNT, SCENARIO_DISTINCT,
	    "allocation %zu of %d should have succeeded", first_unallocated,
	    BLOCK_COUNT);

	if (first_unallocated != BLOCK_COUNT) {
		relic_arena_reset(&arena);
		return;
	}

	first_corrupted = BLOCK_COUNT;
	for (i = 0; i < BLOCK_COUNT && first_corrupted == BLOCK_COUNT; i++) {
		unsigned char expected[BLOCK_SIZE];

		memset(expected, (int)(i % 251), BLOCK_SIZE);
		if (bcmp(blocks[i], expected, BLOCK_SIZE) != 0)
			first_corrupted = i;
	}

	checkf(first_corrupted == BLOCK_COUNT, SCENARIO_DISTINCT,
	    "block %zu should still hold what was written to it",
	    first_corrupted);

	relic_arena_reset(&arena);
}

/*
 * A request of nothing is rounded up like any other, so it returns a usable
 * pointer.  If it returned NULL instead, a caller could not tell an arena
 * that refused a zero-byte request from one that had run out of memory.
 */
static void
a_zero_byte_request_returns_a_distinct_pointer(void)
{
	relic_arena_t arena;
	void *first;
	void *second;

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);

	first = relic_arena_alloc(&arena, 0);
	second = relic_arena_alloc(&arena, 0);

	check(first != NULL && second != NULL, SCENARIO_ZERO,
	    "a zero-byte request should not report failure");
	check(first != second, SCENARIO_ZERO,
	    "two requests should not hand back the same address");
	check(!relic_arena_failed(&arena), SCENARIO_ZERO,
	    "the arena should not be marked failed");

	relic_arena_reset(&arena);
}

/*
 * A size that cannot be rounded up without overflowing a size_t is refused,
 * and so is an array whose count and element size multiply past one.  No
 * allocator could satisfy either, so this exercises the failure path without
 * depending on how much memory the machine running the test has.
 *
 * The flag is what the test is really about.  After a refusal the arena
 * refuses everything, so a construction already several allocations deep
 * stops rather than continuing against a structure it can no longer finish.
 */
static void
an_impossible_request_fails_and_stays_failed(void)
{
	relic_arena_t arena;
	void *refused;
	void *afterwards;

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);

	refused = relic_arena_alloc(&arena, SIZE_MAX);
	check(refused == NULL, SCENARIO_FAILURE,
	    "a request that cannot be represented should fail");
	check(relic_arena_failed(&arena), SCENARIO_FAILURE,
	    "the failure should be recorded on the arena");

	afterwards = relic_arena_alloc(&arena, 8);
	check(afterwards == NULL, SCENARIO_FAILURE,
	    "a later ordinary request should be refused too");

	relic_arena_reset(&arena);
	check(!relic_arena_failed(&arena), SCENARIO_FAILURE,
	    "a reset should clear the failure");

	refused = relic_arena_allocarray(&arena, SIZE_MAX / 2 + 1, 2);
	check(refused == NULL && relic_arena_failed(&arena), SCENARIO_FAILURE,
	    "an array whose bytes a size_t cannot hold should fail the "
	    "arena");

	relic_arena_reset(&arena);
}

/*
 * An owner sometimes refuses a request before the arena sees it -- a count
 * and an element size whose product would wrap is the case that exists --
 * and marking the arena is how that refusal reaches the phase boundary.
 *
 * Without it such a refusal would be a second thing a null return could
 * mean: the caller stops, the flag stays clear, and the boundary check that
 * is supposed to catch a truncated result sees nothing wrong.  The
 * indistinguishability from an exhausted arena is the property under test.
 */
static void
an_owner_can_record_a_refusal_it_made_itself(void)
{
	relic_arena_t arena;
	void *afterwards;

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);

	check(!relic_arena_failed(&arena), SCENARIO_MARKED,
	    "a fresh arena should not be marked");

	relic_arena_fail(&arena);
	check(relic_arena_failed(&arena), SCENARIO_MARKED,
	    "marking the arena should record the failure");

	afterwards = relic_arena_alloc(&arena, 8);
	check(afterwards == NULL, SCENARIO_MARKED,
	    "a marked arena should refuse an ordinary request, as an "
	    "exhausted one does");

	relic_arena_clear_failure(&arena);
	check(!relic_arena_failed(&arena), SCENARIO_MARKED,
	    "clearing should let the arena serve requests again");

	afterwards = relic_arena_alloc(&arena, 8);
	check(afterwards != NULL, SCENARIO_MARKED,
	    "and the request should then succeed");

	relic_arena_reset(&arena);
}

/*
 * A reset arena is indistinguishable from a freshly initialized one: it
 * allocates again, and it has given back everything it held.
 */
static void
a_reset_arena_allocates_again(void)
{
	relic_arena_t arena;
	void *block;

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);

	block = relic_arena_alloc(&arena, 1000);
	check(block != NULL, SCENARIO_RESET, "the first allocation succeeds");

	relic_arena_reset(&arena);
	check(relic_arena_total_bytes(&arena) == 0, SCENARIO_RESET,
	    "a reset arena should hold no memory");

	block = relic_arena_alloc(&arena, 1000);
	check(block != NULL, SCENARIO_RESET,
	    "the arena should allocate again after a reset");
	check(relic_arena_total_bytes(&arena) > 0, SCENARIO_RESET,
	    "and should report the memory it obtained again");

	relic_arena_reset(&arena);
}

/*
 * What an arena obtains for its first small allocation, chunk header
 * included, measured rather than assumed so that the scenarios below hold
 * whatever the header's size on the platform.
 */
static size_t
first_chunk_bytes(void)
{
	relic_arena_t arena;
	size_t bytes;

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);
	bytes = relic_arena_alloc(&arena, 8) != NULL ?
	    relic_arena_total_bytes(&arena) : 0;
	relic_arena_reset(&arena);

	return (bytes);
}

/*
 * A charged arena's account counts exactly what the arena holds from the
 * allocator, an oversize chunk included; a reset gives it all back and
 * leaves the arena charged; an arena that is not charged changes nothing,
 * and neither arena touches the store's part of the account.
 */
static void
a_charged_arena_is_counted_in_its_account(void)
{
	relic_account_t account;
	relic_arena_t charged;
	relic_arena_t uncharged;

	relic_account_init(&account, 0);
	account.store_bytes = 1234;
	relic_arena_init_charged(&charged, &account);
	relic_arena_init(&uncharged, RELIC_ARENA_ANY_OBJECT);

	check(relic_arena_alloc(&charged, 100) != NULL &&
	    relic_arena_alloc(&charged, 100000) != NULL, SCENARIO_CHARGED,
	    "a charged arena without a limit allocates");
	checkf(account.arena_bytes == relic_arena_total_bytes(&charged) &&
	    account.arena_bytes > 100000, SCENARIO_CHARGED,
	    "the account should hold the arena's %zu bytes, and holds %zu",
	    relic_arena_total_bytes(&charged), account.arena_bytes);

	check(relic_arena_alloc(&uncharged, 100000) != NULL &&
	    account.arena_bytes == relic_arena_total_bytes(&charged),
	    SCENARIO_CHARGED, "an arena that is not charged is not counted");

	relic_arena_reset(&charged);
	checkf(account.arena_bytes == 0, SCENARIO_CHARGED,
	    "a reset should give back every byte, and %zu remain",
	    account.arena_bytes);

	check(relic_arena_alloc(&charged, 100) != NULL &&
	    account.arena_bytes == relic_arena_total_bytes(&charged) &&
	    account.arena_bytes > 0, SCENARIO_CHARGED,
	    "a reset arena stays charged");
	check(account.store_bytes == 1234 && !account.refused,
	    SCENARIO_CHARGED, "the store's part is untouched");

	relic_arena_reset(&charged);
	relic_arena_reset(&uncharged);
}

/*
 * The limit applies to the whole account: a chunk that the store's part
 * leaves no room for is refused, before the allocator is asked, and the
 * arena fails as an exhausted one does, stickily.  The account records the
 * refusal once.  A limit that admits the first chunk refuses the next when
 * that one would cross it, and the account still holds exactly what the
 * arena does.
 */
static void
the_limit_refuses_a_chunk_before_it_is_obtained(void)
{
	relic_account_t account;
	relic_arena_t arena;
	size_t first;
	int i;

	first = first_chunk_bytes();
	check(first > 0, SCENARIO_LIMITED, "a first chunk is measured");

	relic_account_init(&account, first + 100);
	account.store_bytes = 101;
	relic_arena_init_charged(&arena, &account);

	check(relic_arena_alloc(&arena, 8) == NULL &&
	    relic_arena_failed(&arena), SCENARIO_LIMITED,
	    "a chunk the store leaves no room for is refused");
	check(relic_arena_total_bytes(&arena) == 0 && account.arena_bytes == 0,
	    SCENARIO_LIMITED, "nothing was obtained or counted");
	check(relic_account_take_refusal(&account) &&
	    !relic_account_take_refusal(&account), SCENARIO_LIMITED,
	    "the refusal is recorded once");

	account.store_bytes = 100;
	check(relic_arena_alloc(&arena, 8) == NULL, SCENARIO_LIMITED,
	    "a failed arena stays failed when the limit would admit it");
	check(!relic_account_take_refusal(&account), SCENARIO_LIMITED,
	    "and asks the account nothing");

	relic_arena_reset(&arena);
	check(relic_arena_alloc(&arena, 8) != NULL &&
	    account.arena_bytes == first, SCENARIO_LIMITED,
	    "a first chunk that exactly reaches the limit is admitted");

	for (i = 0; i < 1000 && relic_arena_alloc(&arena, 64) != NULL; i++)
		continue;
	check(relic_arena_failed(&arena) &&
	    relic_account_take_refusal(&account), SCENARIO_LIMITED,
	    "the next chunk crosses the limit and is refused by it");
	check(account.arena_bytes == relic_arena_total_bytes(&arena) &&
	    account.arena_bytes == first, SCENARIO_LIMITED,
	    "the account holds what the arena holds");

	relic_arena_reset(&arena);
	check(account.arena_bytes == 0, SCENARIO_LIMITED,
	    "the reset gives it back");

	check(relic_account_exceeds(&account, 0, SIZE_MAX), SCENARIO_LIMITED,
	    "a projection past the size of memory exceeds any limit");
}

/*
 * Allocations of sizes from nothing to many times a chunk, in an order that
 * leaves chunks part full and asks for more than the next chunk holds, are
 * projected before they are made: each window of WINDOW allocations as a
 * whole, and each allocation alone.  Every projection gives the total the
 * arena holds once the allocations are made, on an arena of either
 * alignment.  A size no chunk could be counted for leaves the projection at
 * SIZE_MAX, where every allocation after it leaves it.
 */
static void
a_projection_gives_what_the_allocations_take(void)
{
	static const size_t sizes[] = {
		0, 1, 7, 64, 1000, 4096, 5000, 70000, 300000, 3, 130000
	};
	static const size_t alignments[] = { 1, RELIC_ARENA_ANY_OBJECT };
	enum { SIZES = sizeof(sizes) / sizeof(sizes[0]), STEPS = 200,
	    WINDOW = 5 };
	relic_arena_projection_t window;
	relic_arena_projection_t one;
	relic_arena_t arena;
	uint32_t state;
	size_t size[STEPS];
	size_t first;
	size_t end;
	size_t a;
	size_t i;
	bool agreed;

	state = 12345;
	for (i = 0; i < STEPS; i++) {
		state = state * 1103515245u + 12345u;
		size[i] = sizes[(state >> 16) % SIZES];
	}

	for (a = 0; a < sizeof(alignments) / sizeof(alignments[0]); a++) {
		relic_arena_init(&arena, alignments[a]);
		agreed = true;
		for (first = 0; first < STEPS && agreed; first += WINDOW) {
			end = first + WINDOW < STEPS ? first + WINDOW : STEPS;
			relic_arena_project(&arena, &window);
			for (i = first; i < end; i++)
				relic_arena_project_alloc(&window, size[i]);

			for (i = first; i < end && agreed; i++) {
				relic_arena_project(&arena, &one);
				relic_arena_project_alloc(&one, size[i]);
				agreed = relic_arena_alloc(&arena,
				    size[i]) != NULL && one.total_bytes ==
				    relic_arena_total_bytes(&arena);
				checkf(agreed, SCENARIO_PROJECTED, "at "
				    "alignment %zu, allocation %zu of %zu "
				    "bytes should take the arena to %zu bytes, "
				    "not %zu", alignments[a], i, size[i],
				    one.total_bytes,
				    relic_arena_total_bytes(&arena));
			}

			agreed = agreed && window.total_bytes ==
			    relic_arena_total_bytes(&arena);
			checkf(agreed, SCENARIO_PROJECTED, "at alignment %zu, "
			    "allocations %zu to %zu should take the arena to "
			    "%zu bytes, not %zu", alignments[a], first, end - 1,
			    window.total_bytes,
			    relic_arena_total_bytes(&arena));
		}

		relic_arena_project(&arena, &window);
		relic_arena_project_alloc(&window, SIZE_MAX);
		relic_arena_project_alloc(&window, 1);
		checkf(window.total_bytes == SIZE_MAX, SCENARIO_PROJECTED,
		    "at alignment %zu, a projection past SIZE_MAX should stay "
		    "at SIZE_MAX, not %zu", alignments[a], window.total_bytes);
		relic_arena_reset(&arena);
	}
}

/*
 * Allocate each of `count` sizes from an arena, failing the check when one
 * is refused.
 */
static void
allocate_sizes(relic_arena_t *arena, const size_t *sizes, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (relic_arena_alloc(arena, sizes[i]) == NULL) {
			checkf(false, SCENARIO_REWOUND, "allocation %zu of %zu "
			    "bytes should succeed", i, sizes[i]);
			return;
		}
	}
}

/*
 * The allocations made before a mark, the first of them filled with a
 * pattern a rewind must not disturb, and the pattern's first byte.
 */
#define	REWIND_PATTERN_BYTES	100

static unsigned char *
allocate_before_mark(relic_arena_t *arena)
{
	static const size_t before[] = { 5000, 70000, 30 };
	unsigned char *patterned;
	size_t i;

	patterned = relic_arena_alloc(arena, REWIND_PATTERN_BYTES);
	if (patterned == NULL) {
		check(false, SCENARIO_REWOUND, "the patterned allocation "
		    "should succeed");
		return (NULL);
	}
	for (i = 0; i < REWIND_PATTERN_BYTES; i++)
		patterned[i] = (unsigned char)(i * 7 + 1);
	allocate_sizes(arena, before, sizeof(before) / sizeof(before[0]));

	return (patterned);
}

/*
 * The payload bytes of the chunks the arena links, which a rewind that
 * restored its totals but left a chunk linked would not give back.
 */
static size_t
linked_payload_bytes(const relic_arena_t *arena)
{
	const relic_arena_chunk_t *chunk;
	size_t bytes;

	bytes = 0;
	SLIST_FOREACH(chunk, &arena->chunks, link)
		bytes += chunk->size;

	return (bytes);
}

static bool
pattern_intact(const unsigned char *patterned)
{
	size_t i;

	for (i = 0; i < REWIND_PATTERN_BYTES; i++) {
		if (patterned[i] != (unsigned char)(i * 7 + 1))
			return (false);
	}

	return (true);
}

/*
 * Allocations made after a mark are undone by a rewind: the arena and its
 * account hold what they held at the mark, bytes allocated before it keep
 * their contents, the head chunk's room is where it was, and the same
 * allocations made again take the arena where they took it the first time.
 *
 * The allocations after the mark cover both ways a chunk is obtained.  The
 * first takes the recorded head's room; the next two are oversize, and so
 * are linked directly behind the recorded head; the rest push new heads in
 * front of it, and fill them.  From an empty arena the first allocation
 * obtains the head instead, and a rewind returns the arena to empty.
 */
static void
a_rewind_returns_the_arena_to_its_mark(void)
{
	static const size_t after[] = { 10, 300000, 600000, 4000, 130000,
	    9000, 20, 7 };
	enum { AFTER = sizeof(after) / sizeof(after[0]) };
	relic_account_t account;
	relic_arena_mark_t mark;
	relic_arena_t arena;
	unsigned char *patterned;
	void *first_in_room;
	size_t marked_total;
	size_t marked_charge;
	size_t marked_payload;
	size_t first_total;
	int start;

	for (start = 0; start < 2; start++) {
		relic_account_init(&account, 0);
		relic_arena_init_charged(&arena, &account);
		patterned = NULL;
		if (start == 1)
			patterned = allocate_before_mark(&arena);

		relic_arena_mark(&arena, &mark);
		marked_total = relic_arena_total_bytes(&arena);
		marked_charge = account.arena_bytes;
		marked_payload = linked_payload_bytes(&arena);

		first_in_room = relic_arena_alloc(&arena, after[0]);
		allocate_sizes(&arena, after + 1, AFTER - 1);
		first_total = relic_arena_total_bytes(&arena);
		relic_arena_rewind(&arena, &mark);
		checkf(relic_arena_total_bytes(&arena) == marked_total &&
		    account.arena_bytes == marked_charge, SCENARIO_REWOUND,
		    "from %s arena, the rewind should return the arena to %zu "
		    "bytes and its account to %zu, not %zu and %zu",
		    start == 0 ? "an empty" : "a used", marked_total,
		    marked_charge, relic_arena_total_bytes(&arena),
		    account.arena_bytes);
		checkf(linked_payload_bytes(&arena) == marked_payload,
		    SCENARIO_REWOUND, "from %s arena, the rewind should leave "
		    "chunks of %zu payload bytes linked, not %zu",
		    start == 0 ? "an empty" : "a used", marked_payload,
		    linked_payload_bytes(&arena));

		/*
		 * Only a used arena's first allocation lands in a chunk that
		 * survives the rewind, so only there is its address the same.
		 */
		if (start == 1) {
			check(pattern_intact(patterned), SCENARIO_REWOUND,
			    "bytes allocated before the mark should keep "
			    "their contents");
			check(relic_arena_alloc(&arena, after[0]) ==
			    first_in_room, SCENARIO_REWOUND, "the first "
			    "allocation after the rewind should take the "
			    "head's room where the first did");
		} else {
			(void)relic_arena_alloc(&arena, after[0]);
		}
		allocate_sizes(&arena, after + 1, AFTER - 1);
		checkf(relic_arena_total_bytes(&arena) == first_total,
		    SCENARIO_REWOUND, "from %s arena, the same allocations "
		    "should take it to %zu bytes again, not %zu",
		    start == 0 ? "an empty" : "a used", first_total,
		    relic_arena_total_bytes(&arena));

		relic_arena_reset(&arena);
		check(account.arena_bytes == 0, SCENARIO_REWOUND,
		    "the reset gives back every byte");
	}
}

/*
 * A sequence the limit refuses part way is rewound to its mark and made
 * again once the limit allows it: the refusal leaves the arena failed, the
 * rewind leaves the failure for the owner to clear, and after both the
 * sequence takes the arena where an arena that was never refused goes.
 */
static void
a_refused_sequence_is_rewound_and_retried(void)
{
	static const size_t after[] = { 10, 300000, 600000, 4000 };
	enum { AFTER = sizeof(after) / sizeof(after[0]) };
	relic_account_t account;
	relic_account_t unrefused_account;
	relic_arena_mark_t mark;
	relic_arena_t arena;
	relic_arena_t unrefused;
	size_t marked_charge;
	size_t i;
	bool refused;

	relic_account_init(&unrefused_account, 0);
	relic_arena_init_charged(&unrefused, &unrefused_account);
	(void)allocate_before_mark(&unrefused);
	allocate_sizes(&unrefused, after, AFTER);

	relic_account_init(&account, 0);
	relic_arena_init_charged(&arena, &account);
	(void)allocate_before_mark(&arena);
	relic_arena_mark(&arena, &mark);
	marked_charge = account.arena_bytes;

	account.limit = marked_charge + 400000;
	refused = false;
	for (i = 0; i < AFTER && !refused; i++)
		refused = relic_arena_alloc(&arena, after[i]) == NULL;
	check(refused && i == 3, SCENARIO_REWOUND,
	    "the limit should refuse the third allocation");

	relic_arena_rewind(&arena, &mark);
	check(relic_arena_failed(&arena), SCENARIO_REWOUND,
	    "the rewind should leave the failure for the owner");
	check(account.arena_bytes == marked_charge, SCENARIO_REWOUND,
	    "the rewind should give back what the refused sequence took");

	relic_arena_clear_failure(&arena);
	account.limit = 0;
	allocate_sizes(&arena, after, AFTER);
	checkf(relic_arena_total_bytes(&arena) ==
	    relic_arena_total_bytes(&unrefused), SCENARIO_REWOUND,
	    "the retry should take the arena to %zu bytes, not %zu",
	    relic_arena_total_bytes(&unrefused),
	    relic_arena_total_bytes(&arena));

	relic_arena_reset(&arena);
	relic_arena_reset(&unrefused);
}

int
main(void)
{
	a_projection_gives_what_the_allocations_take();
	a_rewind_returns_the_arena_to_its_mark();
	a_refused_sequence_is_rewound_and_retried();
	a_reset_arena_allocates_again();
	a_charged_arena_is_counted_in_its_account();
	the_limit_refuses_a_chunk_before_it_is_obtained();
	a_zero_byte_request_returns_a_distinct_pointer();
	an_allocation_is_aligned_whatever_its_size();
	an_arena_aligns_to_its_own_alignment();
	an_impossible_request_fails_and_stays_failed();
	an_owner_can_record_a_refusal_it_made_itself();
	separate_allocations_do_not_overlap();

	return (check_summary("arena"));
}
