/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_ACCOUNT_H
#define RELIC_ACCOUNT_H

/*
 * This header defines what code compiled against frozen.h alone must not
 * see, and such code defines RELIC_FROZEN_ONLY to have it refused.
 */
#ifdef RELIC_FROZEN_ONLY
#error "code that only reads the store must see it only through frozen.h"
#endif

#include <stdbool.h>
#include <stddef.h>

#include "atomic.h"
#include "frozen.h"

/*
 * The memory account: the bytes a context holds on the user's behalf while
 * it works, which the configured memory limit is enforced against.
 *
 * Five kinds of holder contribute, each keeping its own part of the total
 * exact.  The relation store counts in `store_bytes` the capacity of every
 * relation's columns and indexes, an index being built included: the base
 * stores holding the program's facts and the relations evaluation derives
 * alike.  The value table counts in `value_bytes` the capacity of its
 * integer table and of its record of which symbols are string values, and
 * the symbol pool counts in `pool_bytes` its entries, its index and its
 * bytes, whose integers and strings evaluation computes as well as the
 * program writes.  An arena initialized charged counts in `arena_bytes`
 * every chunk it holds from the allocator, and gives them back when it is
 * reset.  Arenas holding what the caller wrote -- the program's rules and
 * queries, the schedule, a query -- are not charged, and neither is the
 * text being loaded nor the staging a load holds its facts in until it
 * commits, which are proportional to that text and released when the load
 * returns.  A round of evaluation counts in `round_bytes` the buffers it
 * holds for its own length -- the strings its plans compute, and, when its
 * insertions wait for its end, what it has derived -- from the moment they
 * grow until the round ends, when they are released.  So the account is a
 * bound on what the program's stores hold and on what running the program
 * does, and not on the program as it was written.
 *
 * Every holder asks relic_account_exceeds() before it grows, against the
 * whole total, so the limit bounds the sum and no holder can spend what
 * another was admitted.  A round's buffers grow on every worker of the
 * round at once, so `round_bytes` is an atomic word, charged by a
 * compare-and-swap that admits the bytes only if the total with them stays
 * within the limit; the store and the arenas change only on the calling
 * thread, while no round's workers run.
 *
 * `refused` records that the limit refused an arena a chunk, or the value
 * table or the symbol pool a growth.  A refused round is not recorded,
 * because nothing reports it: the round is run again inserting as it
 * derives, which holds no buffers, and whatever refuses that run reports
 * itself.  The arena or the table fails as it would had the allocator
 * refused, and its caller reports the failure through relic_diag_no_memory()
 * as every allocation failure is reported; that function takes the record
 * with relic_account_take_refusal() and reports the limit instead of
 * exhaustion.  Nothing else reads it.  The
 * beginning of an operation, and of each activity within one, clears a
 * record nobody reported, so that no later failure is blamed on it.
 */

struct relic_account {
	size_t	limit;		/* bytes; see relic_limit_set() */
	size_t	store_bytes;	/* relation and index storage */
	size_t	value_bytes;	/* the integer table and the string bits */
	size_t	pool_bytes;	/* the symbol pool */
	size_t	arena_bytes;	/* chunks of the charged arenas */
	relic_atomic_size_t round_bytes; /* buffers of a deferred round */
	bool	refused;
};

/*
 * relic_limit_set() says whether a configured limit, of tuples or of bytes,
 * can refuse anything, and is what every check of a limit asks first, so
 * that the checks and relic_limits_bound_relations() agree on what sets no
 * limit.  Zero sets none, and SIZE_MAX sets none in effect: no count of
 * tuples reaches it, and a total of bytes passing it is one no allocation
 * could have produced.
 *
 * relic_account_admit() asks relic_account_exceeds() for a holder that
 * counts itself, such as the value table or the symbol pool, and records a
 * refusal as relic_account_charge() does, so that the failure the holder
 * then reports through relic_diag_no_memory() is reported as the limit.
 *
 * relic_account_charge() admits `bytes` more for an arena and counts them,
 * or records the refusal and returns false having counted nothing.
 * relic_account_release() gives back bytes an arena counted.  A round's
 * buffers are charged and released through the two functions frozen.h
 * declares, which the code running a round's tasks can reach.
 */
void	 relic_account_init(relic_account_t *account, size_t limit);
bool	 relic_limit_set(size_t limit);
bool	 relic_account_exceeds(const relic_account_t *account,
	    size_t current_bytes, size_t projected_bytes);
bool	 relic_account_admit(relic_account_t *account, size_t current_bytes,
	    size_t projected_bytes);
bool	 relic_account_charge(relic_account_t *account, size_t bytes);
void	 relic_account_release(relic_account_t *account, size_t bytes);
bool	 relic_account_take_refusal(relic_account_t *account);

#endif /* RELIC_ACCOUNT_H */
