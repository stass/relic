/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <strings.h>

#include "account.h"

static bool	 total_exceeds(const relic_account_t *account,
		    size_t round_bytes, size_t current_bytes,
		    size_t projected_bytes);

void
relic_account_init(relic_account_t *account, size_t limit)
{
	bzero(account, sizeof(*account));
	account->limit = limit;
}

bool
relic_limit_set(size_t limit)
{
	return (limit != 0 && limit != SIZE_MAX);
}

/*
 * Whether something holding `current_bytes` of the total -- a relation, an
 * index being built, or nothing yet for an arena's next chunk -- would take
 * the total past the limit by growing to `projected_bytes`.
 *
 * The projection replaces that holder's contribution with what it would
 * become, so the answer covers every holder without revisiting any of them.
 * The subtraction is safe because the total is by construction the sum of
 * what every holder holds, so the current bytes of any one are a part of it.
 */
bool
relic_account_exceeds(const relic_account_t *account, size_t current_bytes,
    size_t projected_bytes)
{
	return (total_exceeds(account,
	    relic_atomic_size_load(&account->round_bytes), current_bytes,
	    projected_bytes));
}

/*
 * relic_account_exceeds() with the round's buffers holding `round_bytes`.
 */
static bool
total_exceeds(const relic_account_t *account, size_t round_bytes,
    size_t current_bytes, size_t projected_bytes)
{
	size_t total;
	size_t elsewhere_bytes;

	if (!relic_limit_set(account->limit))
		return (false);

	total = account->store_bytes + account->arena_bytes + round_bytes;
	elsewhere_bytes = total - current_bytes;

	/* Two sizes that fit in a size_t need not have a sum that does. */
	if (projected_bytes > SIZE_MAX - elsewhere_bytes)
		return (true);

	return (elsewhere_bytes + projected_bytes > account->limit);
}

/*
 * The check comes before the allocation it admits, so a limit that fires
 * has prevented the chunk rather than reported it.
 */
bool
relic_account_charge(relic_account_t *account, size_t bytes)
{
	if (relic_account_exceeds(account, 0, bytes)) {
		account->refused = true;
		return (false);
	}

	account->arena_bytes += bytes;

	return (true);
}

/*
 * An arena gives back only what it was charged, so the charged total always
 * covers it.
 */
void
relic_account_release(relic_account_t *account, size_t bytes)
{
	assert(bytes <= account->arena_bytes);
	account->arena_bytes -= bytes;
}

/*
 * The workers of a round charge at once.  Each attempt checks the total
 * with the round's bytes as it read them and adds its own only if they are
 * still what it read, so the bytes admitted together never pass the limit
 * whichever worker's growth comes first.
 */
bool
relic_account_charge_round(relic_account_t *account, size_t bytes)
{
	size_t held;

	held = relic_atomic_size_load(&account->round_bytes);
	do {
		if (total_exceeds(account, held, 0, bytes))
			return (false);
	} while (!relic_atomic_size_swap_if(&account->round_bytes, &held,
	    held + bytes));

	return (true);
}

void
relic_account_release_round(relic_account_t *account, size_t bytes)
{
	assert(bytes <= relic_atomic_size_load(&account->round_bytes));
	relic_atomic_size_subtract(&account->round_bytes, bytes);
}

/*
 * Whether the limit refused an arena since the record was last taken, and
 * forget that it did, so each refusal is reported once.
 */
bool
relic_account_take_refusal(relic_account_t *account)
{
	bool refused;

	refused = account->refused;
	account->refused = false;

	return (refused);
}
