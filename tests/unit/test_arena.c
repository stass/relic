/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the region allocator.
 *
 * Three properties matter to everything built on it: an allocation is
 * correctly aligned and does not overlap its neighbours, a reset returns the
 * arena to a reusable empty state, and a failure is both reported to the
 * caller and remembered on the arena.
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
#define	SCENARIO_DISTINCT	"allocations do not overlap"
#define	SCENARIO_FAILURE	"an impossible request fails stickily"
#define	SCENARIO_RESET		"reset returns the arena to an empty state"
#define	SCENARIO_ZERO		"a zero-byte request yields a usable pointer"

static bool	 is_aligned(const void *block);
static void	 a_reset_arena_allocates_again(void);
static void	 a_zero_byte_request_returns_a_distinct_pointer(void);
static void	 an_allocation_is_aligned_whatever_its_size(void);
static void	 an_impossible_request_fails_and_stays_failed(void);
static void	 separate_allocations_do_not_overlap(void);

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

	relic_arena_init(&arena);

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

	relic_arena_init(&arena);

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

	relic_arena_init(&arena);

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
 * A size that cannot be rounded up without overflowing a size_t is refused.
 * No allocator could satisfy it, so this exercises the failure path without
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

	relic_arena_init(&arena);

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

	relic_arena_init(&arena);

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

int
main(void)
{
	a_reset_arena_allocates_again();
	a_zero_byte_request_returns_a_distinct_pointer();
	an_allocation_is_aligned_whatever_its_size();
	an_impossible_request_fails_and_stays_failed();
	separate_allocations_do_not_overlap();

	return (check_summary("arena"));
}
