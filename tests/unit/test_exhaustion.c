/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * The library's behaviour when an allocation fails.
 *
 * Every other suite runs with an allocator that always succeeds, so the
 * failure paths that the arena contract of specs/design-memory.md is built
 * around are never taken.  This one compiles the library against a
 * replacement allocator that can be told to refuse the nth request, and
 * sweeps n across the whole of an operation.
 *
 * What it checks is not that a failure is reported -- that much a return
 * value would show -- but that a context which reported one is still
 * internally consistent.  Interning is where this matters most: the design
 * rests on equal bytes always producing the same identifier, and a failure
 * that stored a symbol without indexing it would break that invariant for
 * the life of the context, so the very next intern of the same bytes would
 * quietly assign a second identifier.  Two identical symbols with different
 * identifiers compare unequal, which is a wrong answer rather than a
 * reported failure.
 */

/*
 * The library sources in this program are compiled with their allocator
 * calls renamed, and this file supplies the replacements.  Undefining the
 * names here, before anything is included, lets this one translation unit
 * reach the real allocator to implement them.
 */
#undef calloc
#undef malloc
#undef reallocarray

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "compat.h"
#include "harness.h"
#include "internal.h"

#define	SCENARIO_CREATE		"context creation reports a failed allocation"
#define	SCENARIO_SHIM		"the test allocator is the one being used"
#define	SCENARIO_SYMBOL		"interning a symbol survives a failed allocation"
#define	SCENARIO_VALUE		"interning a value survives a failed allocation"

/*
 * How far into an operation the sweep goes.  An intern makes a handful of
 * requests -- an arena chunk, the entry array, the index slots -- and the
 * bound is set well past that so a request added later is swept too.
 */
#define	SYMBOL_SWEEP	12
#define	VALUE_SWEEP	20

/*
 * Requests still to be served before the allocator starts refusing.  A
 * negative budget means refuse nothing, which is the state every allocation
 * outside a swept region runs in.
 */
static long	 allocations_remaining = -1;
static unsigned long allocations_seen;

void		*relic_test_calloc(size_t count, size_t size);
void		*relic_test_malloc(size_t size);
void		*relic_test_reallocarray(void *items, size_t count,
		    size_t size);

static bool	 refuse_this_allocation(void);
static void	 serve_every_allocation(void);
static void	 refuse_allocations_after(long served);
static void	 context_creation_reports_a_failed_allocation(void);
static void	 interning_a_symbol_survives_a_failed_allocation(void);
static void	 interning_a_value_survives_a_failed_allocation(void);

static bool
refuse_this_allocation(void)
{
	allocations_seen++;

	if (allocations_remaining < 0)
		return (false);

	if (allocations_remaining == 0)
		return (true);

	allocations_remaining--;

	return (false);
}

static void
serve_every_allocation(void)
{
	allocations_remaining = -1;
}

static void
refuse_allocations_after(long served)
{
	allocations_remaining = served;
}

void *
relic_test_calloc(size_t count, size_t size)
{
	if (refuse_this_allocation())
		return (NULL);

	return (calloc(count, size));
}

void *
relic_test_malloc(size_t size)
{
	if (refuse_this_allocation())
		return (NULL);

	return (malloc(size));
}

void *
relic_test_reallocarray(void *items, size_t count, size_t size)
{
	if (refuse_this_allocation())
		return (NULL);

	return (reallocarray(items, count, size));
}

static void
context_creation_reports_a_failed_allocation(void)
{
	relic_t *ctx;

	refuse_allocations_after(0);
	ctx = relic_create();
	serve_every_allocation();

	check(ctx == NULL, SCENARIO_CREATE,
	    "creating a context should fail when nothing can be allocated");

	relic_destroy(ctx);
}

/*
 * Intern one symbol with the nth allocation refused, then restore the
 * allocator and intern the same bytes twice more.  However the first attempt
 * ended, the context must afterwards hold exactly one symbol, and both later
 * attempts must return the identifier that names those bytes.
 */
static void
interning_a_symbol_survives_a_failed_allocation(void)
{
	static const char text[] = "alpha";

	long budget;

	for (budget = 0; budget < SYMBOL_SWEEP; budget++) {
		relic_t *ctx;
		const char *bytes;
		size_t length;
		uint32_t interrupted;
		uint32_t first;
		uint32_t again;
		int status;

		ctx = relic_create();
		if (ctx == NULL) {
			check(false, SCENARIO_SYMBOL,
			    "a context should be created");
			return;
		}

		refuse_allocations_after(budget);
		status = relic_symbol_intern(ctx, text, strlen(text),
		    &interrupted);
		serve_every_allocation();

		checkf(relic_symbol_intern(ctx, text, strlen(text),
		    &first) == 0, SCENARIO_SYMBOL,
		    "interning should succeed once the allocator recovers, "
		    "with allocation %ld refused", budget);
		checkf(relic_symbol_intern(ctx, text, strlen(text),
		    &again) == 0, SCENARIO_SYMBOL,
		    "interning should succeed again, with allocation %ld "
		    "refused", budget);

		checkf(first == again, SCENARIO_SYMBOL,
		    "equal bytes should give one identifier (%u then %u), "
		    "with allocation %ld refused", first, again, budget);
		checkf(relic_symbol_count(ctx) == 1, SCENARIO_SYMBOL,
		    "the context should hold one symbol, not %zu, with "
		    "allocation %ld refused", relic_symbol_count(ctx),
		    budget);

		if (status == 0) {
			checkf(interrupted == first, SCENARIO_SYMBOL,
			    "a reported success should stand, with allocation "
			    "%ld refused", budget);
		}

		bytes = relic_symbol_bytes(ctx, first, &length);
		checkf(bytes != NULL && length == strlen(text) &&
		    bcmp(bytes, text, length) == 0, SCENARIO_SYMBOL,
		    "the identifier should name the bytes interned, with "
		    "allocation %ld refused", budget);

		relic_destroy(ctx);
	}
}

/*
 * The same sweep for the value table, which interns bytes in the symbol pool
 * and then the value that refers to them, so it has both tables' failure
 * paths in one operation.
 */
static void
interning_a_value_survives_a_failed_allocation(void)
{
	static const char text[] = "beta";

	long budget;

	for (budget = 0; budget < VALUE_SWEEP; budget++) {
		relic_t *ctx;
		relic_dom_t interrupted;
		relic_dom_t first;
		relic_dom_t again;
		int status;

		ctx = relic_create();
		if (ctx == NULL) {
			check(false, SCENARIO_VALUE,
			    "a context should be created");
			return;
		}

		refuse_allocations_after(budget);
		status = relic_value_intern_bytes(ctx, RELIC_SYMBOL, text,
		    strlen(text), &interrupted);
		serve_every_allocation();

		checkf(relic_value_intern_bytes(ctx, RELIC_SYMBOL, text,
		    strlen(text), &first) == 0, SCENARIO_VALUE,
		    "interning should succeed once the allocator recovers, "
		    "with allocation %ld refused", budget);
		checkf(relic_value_intern_bytes(ctx, RELIC_SYMBOL, text,
		    strlen(text), &again) == 0, SCENARIO_VALUE,
		    "interning should succeed again, with allocation %ld "
		    "refused", budget);

		checkf(first == again, SCENARIO_VALUE,
		    "equal values should give one identifier (%u then %u), "
		    "with allocation %ld refused", first, again, budget);
		checkf(relic_value_count(ctx) == 1, SCENARIO_VALUE,
		    "the context should hold one value, not %zu, with "
		    "allocation %ld refused", relic_value_count(ctx), budget);
		checkf(relic_symbol_count(ctx) == 1, SCENARIO_VALUE,
		    "and one symbol, not %zu, with allocation %ld refused",
		    relic_symbol_count(ctx), budget);

		if (status == 0) {
			checkf(interrupted == first, SCENARIO_VALUE,
			    "a reported success should stand, with allocation "
			    "%ld refused", budget);
		}

		relic_destroy(ctx);
	}
}

int
main(void)
{
	context_creation_reports_a_failed_allocation();
	interning_a_symbol_survives_a_failed_allocation();
	interning_a_value_survives_a_failed_allocation();

	/*
	 * If the renaming that puts this file's allocator in front of the
	 * library's ever stopped taking effect, every check above would pass
	 * without a single allocation having been refused.  The suite would
	 * go on reporting success while testing nothing, so it says outright
	 * that the replacement was reached.
	 */
	check(allocations_seen > 0, SCENARIO_SHIM,
	    "the library should have allocated through this file");

	return (check_summary("exhaustion"));
}
