/*
 * Copyright (c) 2026 Stanislav Sedov
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
 * what the arena holds and refuses a chunk its limit does not admit.
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
static void	 an_owner_can_record_a_refusal_it_made_itself(void);
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

	relic_arena_init(&arena);

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

	relic_arena_init(&arena);
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
	relic_arena_init(&uncharged);

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

int
main(void)
{
	a_reset_arena_allocates_again();
	a_charged_arena_is_counted_in_its_account();
	the_limit_refuses_a_chunk_before_it_is_obtained();
	a_zero_byte_request_returns_a_distinct_pointer();
	an_allocation_is_aligned_whatever_its_size();
	an_impossible_request_fails_and_stays_failed();
	an_owner_can_record_a_refusal_it_made_itself();
	separate_allocations_do_not_overlap();

	return (check_summary("arena"));
}
