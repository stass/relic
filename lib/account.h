/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_ACCOUNT_H
#define RELIC_ACCOUNT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The memory account: the bytes a context holds on the user's behalf while
 * it works, which the configured memory limit is enforced against.
 *
 * Two kinds of holder contribute, each keeping its own part of the total
 * exact.  The relation store counts in `store_bytes` the capacity of every
 * relation's columns and indexes, an index being built included: the base
 * stores holding the program's facts and the relations evaluation derives
 * alike.  An arena initialized charged counts in `arena_bytes` every chunk
 * it holds from the allocator, and gives them back when it is reset.  Arenas
 * holding what the caller wrote -- the program's rules and queries, the
 * schedule, a query -- are not charged, and neither is the text being
 * loaded nor the staging a load holds its facts in until it commits, which
 * are proportional to that text and released when the load returns.  So
 * the account is a bound on what the program's stores hold and on what
 * running the program does, and not on the program as it was written.
 *
 * Every holder asks relic_account_exceeds() before it grows, against the
 * whole total, so the limit bounds the sum and no holder can spend what
 * another was admitted.
 *
 * `refused` records that the limit refused an arena a chunk.  The arena
 * fails as it would had the allocator refused, and its caller reports the
 * failure through relic_diag_no_memory() as every allocation failure is
 * reported; that function takes the record with relic_account_take_refusal()
 * and reports the limit instead of exhaustion.  Nothing else reads it.  The
 * beginning of an operation, and of each activity within one, clears a
 * record nobody reported, so that no later failure is blamed on it.
 */

typedef struct relic_account relic_account_t;

struct relic_account {
	size_t	limit;		/* bytes; zero for none */
	size_t	store_bytes;	/* relation and index storage */
	size_t	arena_bytes;	/* chunks of the charged arenas */
	bool	refused;
};

/*
 * relic_account_charge() admits `bytes` more for an arena and counts them,
 * or records the refusal and returns false having counted nothing.
 * relic_account_release() gives back bytes an arena counted.
 */
void	 relic_account_init(relic_account_t *account, size_t limit);
bool	 relic_account_exceeds(const relic_account_t *account,
	    size_t current_bytes, size_t projected_bytes);
bool	 relic_account_charge(relic_account_t *account, size_t bytes);
void	 relic_account_release(relic_account_t *account, size_t bytes);
bool	 relic_account_take_refusal(relic_account_t *account);

#endif /* RELIC_ACCOUNT_H */
