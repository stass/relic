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
#define	SCENARIO_LOAD		"a load that ran out of memory adds nothing"
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
 * A load makes many more requests than an intern: the text buffer, an arena
 * chunk per few statements, the body scratch array, and an intern for every
 * distinct name, value and source name.  The bound covers the small program
 * below several times over.
 */
#define	LOAD_SWEEP	80

/*
 * Requests still to be served before the allocator starts refusing.  A
 * negative budget means refuse nothing, which is the state every allocation
 * outside a swept region runs in.  Once the budget reaches zero every later
 * request is refused as well, so a sweep step models an allocator that has
 * run out rather than one that hiccupped.
 */
static long	 allocations_remaining = -1;
static unsigned long allocations_seen;

/* How many RELIC_ERROR diagnostics the context has reported. */
static size_t	 errors_reported;

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
static void	 a_load_that_ran_out_of_memory_adds_nothing(void);
static void	 count_error(void *callback_ctx, enum relic_diag_level level,
		    const relic_location_t *where, const char *message);

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
count_error(void *callback_ctx RELIC_UNUSED, enum relic_diag_level level,
    const relic_location_t *where RELIC_UNUSED,
    const char *message RELIC_UNUSED)
{
	if (level == RELIC_ERROR)
		errors_reported++;
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

/*
 * Load a program with the nth allocation refused, then load the same text
 * again with the allocator restored.
 *
 * Two properties are at stake and neither is visible in a return value.  A
 * load is all or nothing, so a load that failed must have added no
 * statements at all -- half of a program is worse than none, because a
 * caller cannot tell which half.  And the program arena's failure flag is
 * sticky, so a load that did not clear it after reporting would leave a
 * context that can never load anything again; the second load is what
 * proves it was cleared.
 */
static void
a_load_that_ran_out_of_memory_adds_nothing(void)
{
	static const char program[] =
	    "edge(a, b).\n"
	    "path(X, Y) :- edge(X, Y), not blocked(X), Y != \"stop\".\n"
	    "path(a, Z)?\n";

	size_t refused;
	size_t completed;
	long budget;

	refused = 0;
	completed = 0;

	for (budget = 0; budget < LOAD_SWEEP; budget++) {
		relic_t *ctx;
		int status;

		ctx = relic_create();
		if (ctx == NULL) {
			check(false, SCENARIO_LOAD,
			    "a context should be created");
			return;
		}

		relic_diag_set(ctx, count_error, NULL);
		errors_reported = 0;

		refuse_allocations_after(budget);
		status = relic_load_string(ctx, "sweep.dl", program);
		serve_every_allocation();

		/*
		 * One exhaustion is one message.  The program is well formed,
		 * so nothing else has anything to report, and a load that
		 * failed must say why exactly once however many allocations
		 * the refusal went on to strike.  Two messages would mean a
		 * site reported for itself as well as leaving the boundary to
		 * report; none would mean a load failed in silence.
		 */
		checkf(errors_reported == (status != 0 ? 1u : 0u),
		    SCENARIO_LOAD,
		    "a load that %s should report %d message(s), not %zu, "
		    "with allocation %ld refused",
		    status != 0 ? "failed" : "succeeded", status != 0 ? 1 : 0,
		    errors_reported, budget);

		if (status != 0) {
			refused++;
			checkf(ctx->ast.count == 0, SCENARIO_LOAD,
			    "a failed load should add no statements, not %zu, "
			    "with allocation %ld refused", ctx->ast.count,
			    budget);
		} else {
			completed++;
			checkf(ctx->ast.count == 3, SCENARIO_LOAD,
			    "a load that reported success should have added "
			    "all three statements, not %zu, with allocation "
			    "%ld refused", ctx->ast.count, budget);
		}

		checkf(relic_load_string(ctx, "sweep.dl", program) == 0,
		    SCENARIO_LOAD,
		    "loading should succeed once the allocator recovers, with "
		    "allocation %ld refused", budget);

		relic_destroy(ctx);
	}

	/*
	 * The sweep has to straddle the point where the program stops
	 * needing memory, or one of the two branches above is never taken
	 * and is checking nothing.  A bound set too low would make every
	 * load fail, and one set too high would only waste time; both are
	 * reported here rather than left to be noticed.
	 */
	checkf(refused > 0, SCENARIO_LOAD,
	    "the sweep should reach a load that runs out of memory, in %d "
	    "attempts", LOAD_SWEEP);
	checkf(completed > 0, SCENARIO_LOAD,
	    "the sweep should reach a load that completes, in %d attempts",
	    LOAD_SWEEP);
}

int
main(void)
{
	context_creation_reports_a_failed_allocation();
	interning_a_symbol_survives_a_failed_allocation();
	interning_a_value_survives_a_failed_allocation();
	a_load_that_ran_out_of_memory_adds_nothing();

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
