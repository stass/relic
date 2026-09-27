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
 * calls and their entropy source renamed, and this file supplies the
 * replacements.  Undefining the names here, before anything is included,
 * lets this one translation unit reach the real interfaces to implement
 * them.
 */
#undef calloc
#undef getentropy
#undef malloc
#undef mmap
#undef realloc
#undef reallocarray

#include "config.h"

#include <sys/types.h>
#include <sys/mman.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "compat.h"
#include "harness.h"
#include "internal.h"
#include "programs.h"

#define	SCENARIO_CHECK		"a check that ran out of memory can be retried"
#define	SCENARIO_CREATE		"context creation reports a failed allocation"
#define	SCENARIO_ENTROPY	"context creation fails without entropy"
#define	SCENARIO_EVALUATE	"a query that ran out of memory can be retried"
#define	SCENARIO_FALLBACK	"an abandoned rewrite out of memory reports once"
#define	SCENARIO_IMPORT		"an import that ran out of memory adds nothing"
#define	SCENARIO_INDEX		"an index survives a failed allocation"
#define	SCENARIO_INSERT		"a relation survives a failed allocation"
#define	SCENARIO_INTERFACE	"an added fact or collected answer reports once"
#define	SCENARIO_LOAD		"a load that ran out of memory adds nothing"
#define	SCENARIO_LOOKAHEAD	"a refusal in the lookahead reports once"
#define	SCENARIO_MATERIALIZE	"a materialization out of memory reports once"
#define	SCENARIO_SHIM		"the test allocator is the one being used"
#define	SCENARIO_SCHEDULE	"a schedule that could not be rebuilt is kept"
#define	SCENARIO_STRATIFY	"an analysis out of memory says nothing"
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
 * The bytes of the symbol the lookahead sweep's programs place after a fact
 * and after a term.  Interning a symbol this long cannot fit in the slack of
 * an arena chunk already obtained, so it always allocates, and the sweep
 * goes far enough that one of its refusals lands there.
 */
#define	LOOKAHEAD_SYMBOL_BYTES	300000
#define	LOOKAHEAD_SWEEP		200

/*
 * The check sweep's program, and how far the sweep goes.
 *
 * The program has to be large enough that compiling it allocates many times
 * over, because a sweep only tests the statements its refusals actually
 * reach.  A four-rule program makes three requests in total -- the predicate
 * table, its index, the array of rules -- and all three fall while the first
 * statement is being compiled, so every refusal would land in the same
 * place and the cursor would never be asked to resume anywhere else.  A
 * chain of a couple of hundred rules doubles each of those arrays several
 * times and needs fresh arena chunks, so the refusals are spread across the
 * whole program.
 */
#define	CHECK_RULES	200
#define	CHECK_SWEEP	60

/*
 * The stratification sweep's program: a cycle of this many predicates
 * closed by one negative literal, which the analysis must reject and name in
 * full, one note per edge.  The sweep runs from refusals in compilation,
 * through the graph and the search space for the cycle, to a check that
 * completes.  The program is small, so the arrays of the later stages fit in
 * arena chunks already obtained and no refusal can land on them; the
 * schedule sweep below is the one that reaches those.
 */
#define	STRATIFY_CYCLE	40
#define	STRATIFY_SWEEP	60

/*
 * The schedule sweep's program: a chain of this many predicates, each
 * negating the one before it, so each is a component of its own in a stratum
 * of its own.  Every array the analysis allocates is proportional to the
 * number of predicates, components or strata.  At this size the larger ones
 * -- the edges, the rejection's scan, the rule counts and each of the new
 * schedule's own arrays -- are requests of their own, so a sweep over the
 * check refuses them in turn.  An array small enough to fit in a chunk the
 * arena already holds makes no request and cannot be refused on its own; it
 * shares the failure handling of the stage that allocates it.
 */
#define	SCHEDULE_CHAIN	1000
#define	SCHEDULE_SWEEP	80

/*
 * The insertion sweep's shape: how many tuples the relation holds before the
 * sweep begins, how many are inserted with the allocator refusing, and how
 * far the sweep goes.
 *
 * The prelude is what makes a refusal land on a relation with something to
 * lose rather than on an empty one.  The batch is what makes the swept region
 * allocate at all: an insertion into a relation that already has room asks
 * for nothing, so a sweep over a single insertion would refuse nothing and
 * would report success without having tested anything.  Forty insertions from
 * a standing start of twenty cross two index doublings and a column doubling,
 * so a refusal has three different growths to land on, and the sweep is bound
 * well past that so a request added later is swept too.
 */
#define	INSERT_PRELUDE	20
#define	INSERT_BATCH	40
#define	INSERT_SWEEP	10

/*
 * The index sweep builds an index over a relation of INDEX_PRELUDE tuples
 * and then inserts INDEX_BATCH more, each step with one more allocation
 * served.  The build makes room one tuple at a time, so its three arrays
 * each grow several times, and the batch grows the relation and the index
 * together; the sweep is bound past every one of those requests, so a
 * refusal lands on each and some run completes.
 */
#define	INDEX_PRELUDE	40
#define	INDEX_BATCH	40
#define	INDEX_SWEEP	60

/*
 * The evaluation sweep's program is the transitive closure of a chain of
 * this many edges, which derives EVAL_CHAIN * (EVAL_CHAIN + 1) / 2 paths,
 * and a stratum above it that finds the chain's one sink by negation.
 * That is enough to grow the path relation's columns and index several times
 * over, so a refusal can land in preparing the query, in planning a positive
 * or a negated literal, in any relation's growth, or in creating the answer
 * relation, and the sweep is bound well past the last of them.
 */
#define	EVAL_CHAIN	40
#define	EVAL_SWEEP	240

/*
 * The materialization sweep's program is a chain of this many predicates,
 * each copying the one before it.  An evaluation allocates its per-component
 * plans and then a delta range and a membership mark for every predicate.
 * With this many components the first of those arrays is too large for the
 * arena's first chunk, so the delta ranges need a chunk of their own and a
 * refusal can land on them, which is the second request the sweep refuses.
 * The arena's failure is sticky, so the membership marks that follow are
 * refused as well without asking the allocator, and exactly one message must
 * say so.
 *
 * Components are planned as they start, so a budget can also run out part
 * way along the chain, after the components before it have derived their
 * tuples.  A refused run must not be taken for a finished one, and a retry
 * with memory to spare must then discard what it left and derive the whole
 * chain.
 */
#define	WIDE_PREDICATES	600
#define	WIDE_SWEEP	48

/*
 * The fallback sweep answers the widening program of arity 6, whose query
 * reaches 2^5 adornments of p, which outgrow the rewrite's budget, so the
 * query is answered from the whole program after a warning.  An undisturbed
 * run makes some 680 allocations; the sweep stops at the first run that
 * needs no refusal, and the bound only keeps a regression that fails every
 * run from sweeping forever.
 */
#define	WIDENING_ARITY	6
#define	FALLBACK_SWEEP	1024

/*
 * The interface sweep builds a value, adds it as a fact, prepares a query
 * and collects its answer, which makes a few dozen allocations undisturbed;
 * like the fallback sweep it stops at the first run that needs no refusal.
 */
#define	INTERFACE_SWEEP	512

typedef struct eval_sweep eval_sweep_t;

/*
 * One way of answering the evaluation sweep's query: how the strategy is
 * named in a failure, the query, the relations that hold the closure and
 * the sink once it is answered, the flags that select the strategy, and how
 * many rows the answer has.
 */
struct eval_sweep {
	const char	*strategy;
	const char	*query;
	const char	*paths;
	const char	*sinks;
	unsigned int	 flags;
	size_t		 rows;
};

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

/* How many RELIC_WARNING diagnostics the context has reported. */
static size_t	 warnings_reported;

/* How many RELIC_NOTE diagnostics the context has reported. */
static size_t	 notes_reported;

/* Whether relic_test_getentropy() refuses every request. */
static bool	 entropy_unavailable;

/* How many of the next requests relic_test_getentropy() fills with zeros. */
static unsigned int zero_draws_remaining;

void		*relic_test_calloc(size_t count, size_t size);
int		 relic_test_getentropy(void *buffer, size_t length);
void		*relic_test_malloc(size_t size);
void		*relic_test_mmap(void *address, size_t length, int protection,
		    int flags, int descriptor, off_t offset);
void		*relic_test_realloc(void *block, size_t size);
void		*relic_test_reallocarray(void *items, size_t count,
		    size_t size);

static bool	 refuse_this_allocation(void);
static void	 serve_every_allocation(void);
static void	 refuse_allocations_after(long served);
static void	 context_creation_reports_a_failed_allocation(void);
static void	 context_creation_fails_without_entropy(void);
static void	 interning_a_symbol_survives_a_failed_allocation(void);
static void	 interning_a_value_survives_a_failed_allocation(void);
static void	 a_load_that_ran_out_of_memory_adds_nothing(void);
static void	 an_import_that_ran_out_of_memory_adds_nothing(void);
static void	 a_refusal_in_the_lookahead_reports_once(void);
static void	 sweep_lookahead(const char *program, bool well_formed);
static void	 a_check_that_ran_out_of_memory_can_be_retried(void);
static void	 an_analysis_that_ran_out_of_memory_says_nothing(void);
static void	 a_schedule_that_could_not_be_rebuilt_is_kept(void);
static void	 a_relation_survives_a_failed_allocation(void);
static void	 an_index_survives_a_failed_allocation(void);
static bool	 index_finds_every_tuple(const relic_relation_t *rel,
		    uint32_t position);
static size_t	 storage_by_hand(const relic_t *ctx);
static void	 a_query_that_ran_out_of_memory_can_be_retried(void);
static void	 sweep_query_retry(const eval_sweep_t *sweep,
		    const char *program);
static void	 a_materialization_out_of_memory_reports_once(void);
static void	 an_abandoned_rewrite_out_of_memory_reports_once(void);
static void	 an_added_fact_or_collected_answer_reports_once(void);
static int	 add_and_collect(relic_t *ctx, size_t *rowsp);
static char	*wide_sweep_program(void);
static int	 count_answer(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static char	*evaluation_sweep_program(void);
static bool	 answer_is_first(const relic_t *ctx,
		    const relic_query_t *query);
static const relic_relation_t *relation_named(relic_t *ctx, const char *name,
		    uint16_t arity);
static relic_relation_t *sweep_relation(relic_t *ctx, relic_dom_t *values,
		    size_t count);
static char	*check_sweep_program(void);
static char	*stratify_sweep_program(void);
static char	*schedule_sweep_program(void);
static uint64_t	 schedule_digest(const relic_schedule_t *schedule);
static bool	 schedule_then_extend(relic_t *ctx, const char *program);
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

/*
 * The real entropy source, or one of two broken ones.  With
 * `entropy_unavailable` set every request fails, as getentropy(2) does on a
 * system with no random device to offer.  While `zero_draws_remaining` is
 * positive a request succeeds and hands out zeros, as a random device
 * replaced by /dev/zero would.
 */
int
relic_test_getentropy(void *buffer, size_t length)
{
	if (entropy_unavailable) {
		errno = EIO;
		return (-1);
	}

	if (zero_draws_remaining > 0) {
		zero_draws_remaining--;
		bzero(buffer, length);
		return (0);
	}

	return (getentropy(buffer, length));
}

void *
relic_test_malloc(size_t size)
{
	if (refuse_this_allocation())
		return (NULL);

	return (malloc(size));
}

/*
 * A refused mapping fails as mmap(2) does when the system has no memory to
 * give, with MAP_FAILED and ENOMEM.
 */
void *
relic_test_mmap(void *address, size_t length, int protection, int flags,
    int descriptor, off_t offset)
{
	if (refuse_this_allocation()) {
		errno = ENOMEM;
		return (MAP_FAILED);
	}

	return (mmap(address, length, protection, flags, descriptor, offset));
}

void *
relic_test_realloc(void *block, size_t size)
{
	if (refuse_this_allocation())
		return (NULL);

	return (realloc(block, size));
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
	else if (level == RELIC_WARNING)
		warnings_reported++;
	else if (level == RELIC_NOTE)
		notes_reported++;
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
 * A context whose tables could not be keyed by a secret seed is not created
 * at all, with or without a configuration, rather than falling back to a
 * seed an input could be crafted against.  Once entropy is available again
 * creation succeeds, so the failure left nothing behind.
 */
static void
context_creation_fails_without_entropy(void)
{
	relic_config_t config;
	relic_t *ctx;
	relic_t *configured;

	bzero(&config, sizeof(config));

	entropy_unavailable = true;
	ctx = relic_create();
	configured = relic_create_config(&config);
	entropy_unavailable = false;

	check(ctx == NULL, SCENARIO_ENTROPY,
	    "creating a context should fail without entropy");
	check(configured == NULL, SCENARIO_ENTROPY,
	    "creating a configured context should fail without entropy");
	relic_destroy(ctx);
	relic_destroy(configured);

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_ENTROPY,
	    "creating a context should succeed once entropy is back");
	relic_destroy(ctx);

	/*
	 * Zero is the one seed a context may not hold.  A single zero is a
	 * draw like any other and is drawn again; a source that gives nothing
	 * but zeros is broken, and must fail creation rather than be asked
	 * forever.
	 */
	zero_draws_remaining = 1;
	ctx = relic_create();
	check(ctx != NULL && ctx->hash_seed != 0, SCENARIO_ENTROPY,
	    "a zero draw should be drawn again");
	relic_destroy(ctx);

	zero_draws_remaining = UINT_MAX;
	ctx = relic_create();
	zero_draws_remaining = 0;
	check(ctx == NULL, SCENARIO_ENTROPY,
	    "a source of nothing but zeros should fail creation");
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
 * Two properties are at stake and neither is visible in a return value.  A load
 * is all or nothing, so a load that failed must have added no statements and no
 * facts at all -- half of a program is worse than none, because a caller cannot
 * tell which half.  A load that succeeded holds the rule and the query as
 * statements and the fact in edge's base store.  And the program arena's
 * failure flag is sticky, so a load that did not clear it after reporting would
 * leave a context that can never load anything again; the second load is what
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
			checkf(ctx->ast.count == 0 &&
			    relic_base_total(ctx) == 0, SCENARIO_LOAD,
			    "a failed load should add no statements and no "
			    "facts, not %zu and %zu, with allocation %ld "
			    "refused", ctx->ast.count,
			    relic_base_total(ctx), budget);
		} else {
			completed++;
			checkf(ctx->ast.count == 2 &&
			    relic_base_total(ctx) == 1, SCENARIO_LOAD,
			    "a load that reported success should have added "
			    "two statements and one fact, not %zu and %zu, "
			    "with allocation %ld refused", ctx->ast.count,
			    relic_base_total(ctx), budget);
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

/*
 * An import out of memory adds nothing and says so once, wherever the
 * refusal strikes: the plan copied from its description, the chunk it reads
 * into, a field growing, a value interned, a tuple staged, or the commit.
 * The table is held in a temporary file made before any allocation is
 * refused, since the C library's own allocations are not the import's.
 */
static void
an_import_that_ran_out_of_memory_adds_nothing(void)
{
	static const enum relic_column columns[] = {
		RELIC_COLUMN_SYMBOL, RELIC_COLUMN_SKIP, RELIC_COLUMN_STRING
	};
	static const char table[] =
	    "ann,x,\"a quoted field long enough to make its buffer grow\"\n"
	    "bob,y,b\n"
	    "ann,z,\"a quoted field long enough to make its buffer grow\"\n"
	    "cy,w,\"c\"\n";
	relic_import_t import;
	size_t refused;
	size_t completed;
	long budget;

	bzero(&import, sizeof(import));
	import.predicate = "person";
	import.columns = columns;
	import.column_count = 3;
	import.format = RELIC_FORMAT_CSV;

	refused = 0;
	completed = 0;

	for (budget = 0; budget < LOAD_SWEEP; budget++) {
		relic_t *ctx;
		FILE *stream;
		int status;

		ctx = relic_create();
		stream = tmpfile();
		if (ctx == NULL || stream == NULL ||
		    fwrite(table, 1, sizeof(table) - 1, stream) !=
		    sizeof(table) - 1) {
			check(false, SCENARIO_IMPORT,
			    "a context and a table should be made");
			relic_destroy(ctx);
			if (stream != NULL)
				fclose(stream);
			return;
		}

		relic_diag_set(ctx, count_error, NULL);
		errors_reported = 0;

		rewind(stream);
		refuse_allocations_after(budget);
		status = relic_import_file(ctx, "people.csv", stream, &import);
		serve_every_allocation();

		checkf(errors_reported == (status != 0 ? 1u : 0u),
		    SCENARIO_IMPORT,
		    "an import that %s should report %d message(s), not %zu, "
		    "with allocation %ld refused",
		    status != 0 ? "failed" : "succeeded", status != 0 ? 1 : 0,
		    errors_reported, budget);

		if (status != 0) {
			refused++;
			checkf(relic_base_total(ctx) == 0, SCENARIO_IMPORT,
			    "a failed import should add no facts, not %zu, "
			    "with allocation %ld refused",
			    relic_base_total(ctx), budget);
		} else {
			completed++;
			checkf(relic_base_total(ctx) == 3, SCENARIO_IMPORT,
			    "an import that reported success should have added "
			    "three facts, not %zu, with allocation %ld refused",
			    relic_base_total(ctx), budget);
		}

		rewind(stream);
		checkf(relic_import_file(ctx, "people.csv", stream,
		    &import) == 0 && relic_base_total(ctx) == 3,
		    SCENARIO_IMPORT, "importing should succeed once the "
		    "allocator recovers, with allocation %ld refused", budget);

		fclose(stream);
		relic_destroy(ctx);
	}

	checkf(refused > 0, SCENARIO_IMPORT,
	    "the sweep should reach an import that runs out of memory, in %d "
	    "attempts", LOAD_SWEEP);
	checkf(completed > 0, SCENARIO_IMPORT,
	    "the sweep should reach an import that completes, in %d attempts",
	    LOAD_SWEEP);
}

/*
 * One exhaustion is one message even when it strikes the token of
 * lookahead.  The parser reads the token after a fact or a term before it is
 * done with the fact or the term, and a refusal the lexer reports while
 * interning that token stops the parse: staging the fact, or interning the
 * term's value, must not report the same exhaustion again.  A long symbol
 * follows a fact in a well-formed program and a term in a malformed one.
 */
static void
a_refusal_in_the_lookahead_reports_once(void)
{
	char *symbol;
	char *after_fact;
	char *after_term;
	size_t size;

	symbol = malloc(LOOKAHEAD_SYMBOL_BYTES + 1);
	size = LOOKAHEAD_SYMBOL_BYTES + 32;
	after_fact = malloc(size);
	after_term = malloc(size);
	if (symbol == NULL || after_fact == NULL || after_term == NULL) {
		check(false, SCENARIO_LOOKAHEAD, "the programs are built");
		free(symbol);
		free(after_fact);
		free(after_term);
		return;
	}

	memset(symbol, 'l', LOOKAHEAD_SYMBOL_BYTES);
	symbol[LOOKAHEAD_SYMBOL_BYTES] = '\0';
	snprintf(after_fact, size, "e(a).\n%s(b).\n", symbol);
	snprintf(after_term, size, "e(a %s).\n", symbol);

	sweep_lookahead(after_fact, true);
	sweep_lookahead(after_term, false);

	free(symbol);
	free(after_fact);
	free(after_term);
}

/*
 * Load `program` with the nth allocation refused, for every n the sweep
 * reaches, and require one message from each load that fails and none from
 * one that succeeds.  A malformed program fails every time, with either its
 * syntax error or its exhaustion, and a well-formed one succeeds once no
 * refusal strikes it.  Either way the sweep must reach a load that made
 * every allocation it asked for, so that it is known to have run past them
 * all.
 */
static void
sweep_lookahead(const char *program, bool well_formed)
{
	bool unrefused;
	bool reached_unrefused;
	unsigned long seen_before;
	long budget;

	reached_unrefused = false;

	for (budget = 0; budget < LOOKAHEAD_SWEEP; budget++) {
		relic_t *ctx;
		int status;

		ctx = relic_create();
		if (ctx == NULL) {
			check(false, SCENARIO_LOOKAHEAD,
			    "a context should be created");
			return;
		}

		relic_diag_set(ctx, count_error, NULL);
		errors_reported = 0;

		seen_before = allocations_seen;
		refuse_allocations_after(budget);
		status = relic_load_string(ctx, "lookahead.dl", program);
		serve_every_allocation();
		unrefused =
		    allocations_seen - seen_before <= (unsigned long)budget;

		checkf(errors_reported == (status != 0 ? 1u : 0u),
		    SCENARIO_LOOKAHEAD,
		    "a %s load that %s should report %d message(s), not %zu, "
		    "with allocation %ld refused",
		    well_formed ? "well-formed" : "malformed",
		    status != 0 ? "failed" : "succeeded", status != 0 ? 1 : 0,
		    errors_reported, budget);
		checkf((status == 0) == (well_formed && unrefused),
		    SCENARIO_LOOKAHEAD, "a %s load that %s refusal struck "
		    "should %s, with allocation %ld refused",
		    well_formed ? "well-formed" : "malformed",
		    unrefused ? "no" : "a",
		    well_formed && unrefused ? "succeed" : "fail", budget);

		if (unrefused)
			reached_unrefused = true;

		relic_destroy(ctx);
	}

	checkf(reached_unrefused, SCENARIO_LOOKAHEAD, "the sweep should "
	    "reach a load that no refusal strikes, in %d attempts",
	    LOOKAHEAD_SWEEP);
}

/*
 * Build the check sweep's program: a chain of rules long enough that
 * compiling it allocates many times over.
 *
 * `excluded/1` is referenced by the first rule and defined nowhere, so it
 * sits in the predicate table long before the last statement is compiled.
 * That is what lets the sweep tell a check which skipped the
 * undefined-predicate sweep from one which ran it and found nothing to say:
 * a check that ran it after stopping early would warn about `excluded/1`,
 * because by then the table knows about it.
 *
 * The caller owns the returned text.
 */
static char *
check_sweep_program(void)
{
	char *text;
	size_t used;
	size_t size;
	unsigned int i;

	size = (CHECK_RULES + 2) * 48;
	text = malloc(size);
	if (text == NULL)
		return (NULL);

	used = (size_t)snprintf(text, size,
	    "edge(a, b).\n"
	    "p0(X, Y) :- edge(X, Y), not excluded(Y).\n");

	for (i = 1; i < CHECK_RULES; i++) {
		used += (size_t)snprintf(text + used, size - used,
		    "p%u(X, Y) :- p%u(X, Y).\n", i, i - 1);

		if (used >= size) {
			free(text);
			return (NULL);
		}
	}

	return (text);
}

/*
 * Check a well formed program with the nth allocation refused, then check it
 * again with the allocator restored.
 *
 * Exhaustion and rejection are different answers and the whole of the
 * cursor's contract is that they stay different.  A check that ran out of
 * memory has not learned anything about the program, so it must leave the
 * cursor on the statement it could not compile, must not mark the program
 * invalid, and must clear the program arena's sticky flag on its way out.
 * If any of those went wrong the failure would be silent and permanent: the
 * second check below would refuse a program that is perfectly good, or would
 * accept it while the rules it skipped were never compiled at all.
 */
static void
a_check_that_ran_out_of_memory_can_be_retried(void)
{
	char *program;
	size_t refused;
	size_t completed;
	size_t resumed;
	size_t analysed;
	long budget;

	program = check_sweep_program();
	if (program == NULL) {
		check(false, SCENARIO_CHECK, "the sweep program is built");
		return;
	}

	refused = 0;
	completed = 0;
	resumed = 0;
	analysed = 0;

	for (budget = 0; budget < CHECK_SWEEP; budget++) {
		relic_t *ctx;
		int status;

		ctx = relic_create();
		if (ctx == NULL) {
			check(false, SCENARIO_CHECK,
			    "a context should be created");
			return;
		}

		relic_diag_set(ctx, count_error, NULL);

		if (relic_load_string(ctx, "sweep.dl", program) != 0) {
			check(false, SCENARIO_CHECK,
			    "the program should load before the sweep begins");
			relic_destroy(ctx);
			free(program);
			return;
		}

		errors_reported = 0;
		warnings_reported = 0;

		refuse_allocations_after(budget);
		status = relic_check(ctx);
		serve_every_allocation();

		/*
		 * The undefined-predicate sweep asks a question about the
		 * whole program, so a check that stopped part way through
		 * must not answer it: the definitions it would need are in
		 * the statements it never reached.  `excluded/1` is genuinely
		 * undefined and is interned early, so a check that ran the
		 * sweep would warn about it whether or not it got to the end
		 * -- and the warning is due exactly once, from whichever
		 * check first does get there.
		 */
		checkf(warnings_reported == (status != 0 ? 0u : 1u),
		    SCENARIO_CHECK,
		    "a check that %s should report %d undefined-predicate "
		    "warning(s), not %zu, with allocation %ld refused",
		    status != 0 ? "stopped early" : "finished",
		    status != 0 ? 0 : 1, warnings_reported, budget);

		/*
		 * The program is well formed, so the only thing a check of it
		 * can report is that it ran out of memory, and it must say so
		 * exactly once however many allocations the refusal struck.
		 */
		checkf(errors_reported == (status != 0 ? 1u : 0u),
		    SCENARIO_CHECK,
		    "a check that %s should report %d message(s), not %zu, "
		    "with allocation %ld refused",
		    status != 0 ? "failed" : "succeeded", status != 0 ? 1 : 0,
		    errors_reported, budget);

		if (status != 0) {
			refused++;

			/*
			 * A refusal that had already compiled something is
			 * the case the cursor exists for.  One that stopped
			 * on the very first statement exercises nothing
			 * about resuming, so the two are counted apart.
			 */
			if (ctx->program.rules.count > 0)
				resumed++;

			/*
			 * A refusal after every statement was compiled struck
			 * the dependency analysis, which must fail the check
			 * without the program being condemned for it.
			 */
			if (ctx->program.rules.count == CHECK_RULES)
				analysed++;
		} else {
			completed++;
		}

		checkf(!ctx->program.invalid, SCENARIO_CHECK,
		    "running out of memory should not condemn the program, "
		    "with allocation %ld refused", budget);

		checkf(relic_check(ctx) == 0, SCENARIO_CHECK,
		    "checking should succeed once the allocator recovers, "
		    "with allocation %ld refused", budget);

		checkf(warnings_reported == 1, SCENARIO_CHECK,
		    "the undefined predicate should be reported exactly once "
		    "across both checks, not %zu times, with allocation %ld "
		    "refused", warnings_reported, budget);

		/*
		 * Whatever the first check managed, the program afterwards
		 * holds one compiled rule per rule statement; the fact went
		 * to edge's base store when it was loaded.  A cursor left too
		 * far forward would show fewer.
		 */
		checkf(ctx->program.rules.count == CHECK_RULES,
		    SCENARIO_CHECK,
		    "every statement should end up compiled exactly once, "
		    "not %zu of %d, with allocation %ld refused",
		    ctx->program.rules.count, CHECK_RULES, budget);

		relic_destroy(ctx);
	}

	free(program);

	/*
	 * The sweep has to straddle the point where the check stops needing
	 * memory, or one of the two outcomes above is never reached and is
	 * checking nothing.
	 */
	checkf(refused > 0, SCENARIO_CHECK,
	    "the sweep should reach a check that runs out of memory, in %d "
	    "attempts", CHECK_SWEEP);
	checkf(completed > 0, SCENARIO_CHECK,
	    "the sweep should reach a check that completes, in %d attempts",
	    CHECK_SWEEP);
	checkf(resumed > 0, SCENARIO_CHECK,
	    "the sweep should reach a check that stops part way through the "
	    "program, in %d attempts", CHECK_SWEEP);
	checkf(analysed > 0, SCENARIO_CHECK,
	    "the sweep should reach a check that runs out of memory in the "
	    "dependency analysis, in %d attempts", CHECK_SWEEP);
}

/*
 * A cycle of STRATIFY_CYCLE predicates, q0 to q39, each depending on the one
 * before it, with q0 depending negatively on the last: one component holding
 * a negative edge, whose shortest cycle has one edge per predicate.
 */
static char *
stratify_sweep_program(void)
{
	char *text;
	size_t used;
	size_t size;
	unsigned int i;

	size = (STRATIFY_CYCLE + 2) * 48;
	text = malloc(size);
	if (text == NULL)
		return (NULL);

	used = (size_t)snprintf(text, size,
	    "base(a).\n"
	    "q0(X) :- base(X), not q%u(X).\n", STRATIFY_CYCLE - 1);

	for (i = 1; i < STRATIFY_CYCLE; i++) {
		used += (size_t)snprintf(text + used, size - used,
		    "q%u(X) :- q%u(X).\n", i, i - 1);

		if (used >= size) {
			free(text);
			return (NULL);
		}
	}

	return (text);
}

/*
 * Check an unstratified program with the nth allocation refused, then check
 * it twice more with the allocator restored.
 *
 * Running out of memory in the analysis says nothing about the program, so
 * such a check must report that and only that: no error about the cycle, no
 * notes, and the program neither condemned nor marked unstratified.  Whether
 * or not the first check got that far, the cycle is then reported exactly
 * once across all three checks -- one error and a note per edge -- and the
 * program stays refused.  A partial report left behind by a failed analysis,
 * or a report repeated by a later check, would each show as a count that is
 * off.
 */
static void
an_analysis_that_ran_out_of_memory_says_nothing(void)
{
	char *program;
	size_t analysed;
	size_t completed;
	long budget;
	int status;

	program = stratify_sweep_program();
	if (program == NULL) {
		check(false, SCENARIO_STRATIFY, "the sweep program is built");
		return;
	}

	analysed = 0;
	completed = 0;

	for (budget = 0; budget < STRATIFY_SWEEP; budget++) {
		relic_t *ctx;

		ctx = relic_create();
		if (ctx == NULL) {
			check(false, SCENARIO_STRATIFY,
			    "a context should be created");
			break;
		}
		relic_diag_set(ctx, count_error, NULL);

		if (relic_load_string(ctx, "cycle.dl", program) != 0) {
			check(false, SCENARIO_STRATIFY,
			    "the program should load before the sweep begins");
			relic_destroy(ctx);
			break;
		}

		errors_reported = 0;
		notes_reported = 0;

		refuse_allocations_after(budget);
		status = relic_check(ctx);
		serve_every_allocation();

		checkf(status != 0, SCENARIO_STRATIFY,
		    "an unstratified program should never pass a check, with "
		    "allocation %ld refused", budget);

		if (ctx->program.unstratified) {
			completed++;
		} else {
			checkf(errors_reported == 1 && notes_reported == 0,
			    SCENARIO_STRATIFY,
			    "a check that ran out of memory should report that "
			    "alone, not %zu error(s) and %zu note(s), with "
			    "allocation %ld refused", errors_reported,
			    notes_reported, budget);
			checkf(!ctx->program.invalid, SCENARIO_STRATIFY,
			    "running out of memory should not condemn the "
			    "program, with allocation %ld refused", budget);

			if (ctx->program.rules.count == STRATIFY_CYCLE)
				analysed++;

			errors_reported = 0;
		}

		status = relic_check(ctx);
		checkf(status != 0, SCENARIO_STRATIFY,
		    "the program should be refused once the allocator "
		    "recovers, with allocation %ld refused", budget);

		status = relic_check(ctx);
		checkf(status != 0, SCENARIO_STRATIFY,
		    "the program should stay refused on a later check, with "
		    "allocation %ld refused", budget);

		checkf(ctx->program.unstratified && ctx->program.invalid,
		    SCENARIO_STRATIFY,
		    "the program should end up marked unstratified, with "
		    "allocation %ld refused", budget);

		checkf(errors_reported == 1 && notes_reported == STRATIFY_CYCLE,
		    SCENARIO_STRATIFY,
		    "the cycle should be reported once, as 1 error and %d "
		    "notes, not %zu and %zu, with allocation %ld refused",
		    STRATIFY_CYCLE, errors_reported, notes_reported, budget);

		relic_destroy(ctx);
	}

	free(program);

	checkf(analysed > 0, SCENARIO_STRATIFY,
	    "the sweep should reach a check that runs out of memory in the "
	    "analysis, in %d attempts", STRATIFY_SWEEP);
	checkf(completed > 0, SCENARIO_STRATIFY,
	    "the sweep should reach a check that completes, in %d attempts",
	    STRATIFY_SWEEP);
}

/*
 * A chain of SCHEDULE_CHAIN predicates, s0 to s999, each but the first
 * negating the one before it: a component and a stratum per predicate.
 */
static char *
schedule_sweep_program(void)
{
	char *text;
	size_t used;
	size_t size;
	unsigned int i;

	size = (SCHEDULE_CHAIN + 2) * 48;
	text = malloc(size);
	if (text == NULL)
		return (NULL);

	used = (size_t)snprintf(text, size, "base(a).\ns0(X) :- base(X).\n");

	for (i = 1; i < SCHEDULE_CHAIN; i++) {
		used += (size_t)snprintf(text + used, size - used,
		    "s%u(X) :- base(X), not s%u(X).\n", i, i - 1);

		if (used >= size) {
			free(text);
			return (NULL);
		}
	}

	return (text);
}

/*
 * A digest of everything a schedule holds, so that a test can tell whether
 * one changed without keeping a copy of it.  Each word is folded in with the
 * FNV-1a step, which is enough to make an accidental match between two
 * different schedules implausible.
 */
static uint64_t
schedule_digest(const relic_schedule_t *schedule)
{
	const relic_component_t *component;
	uint64_t digest;
	uint64_t words[6];
	size_t i;
	size_t w;

	digest = 14695981039346656037ULL;

	for (i = 0; i < schedule->component_count; i++) {
		component = &schedule->components[i];
		words[0] = component->first_member;
		words[1] = component->member_count;
		words[2] = component->first_rule;
		words[3] = component->rule_count;
		words[4] = component->stratum;
		words[5] = component->recursive;

		for (w = 0; w < 6; w++)
			digest = (digest ^ words[w]) * 1099511628211ULL;
	}

	for (i = 0; i < schedule->member_count; i++)
		digest = (digest ^ schedule->members[i]) * 1099511628211ULL;

	for (i = 0; i < schedule->rule_count; i++)
		digest = (digest ^ schedule->rule_indexes[i]) *
		    1099511628211ULL;

	return (digest);
}

/*
 * The schedule sweep's fixture: load the chain and check it, which builds a
 * schedule, then load one more rule, which leaves that schedule describing
 * fewer rules than the program has.  Returns whether all three succeeded;
 * the allocator serves everything, so a failure is a broken fixture.
 */
static bool
schedule_then_extend(relic_t *ctx, const char *program)
{
	if (relic_load_string(ctx, "chain.dl", program) != 0)
		return (false);

	if (relic_check(ctx) != 0)
		return (false);

	return (relic_load_string(ctx, "top.dl", "top(X) :- s999(X).\n") == 0);
}

/*
 * Check a stratified program, load one more rule, and check again with the
 * nth allocation refused.
 *
 * A check that runs out of memory must leave the schedule the first check
 * built exactly as it was -- the new one is built beside it and replaces it
 * only when complete -- and must leave it describing fewer rules than the
 * program has, so that nothing evaluates from it and the next check rebuilds
 * it.  That next check, with the allocator restored, must pass and schedule
 * the new rule.  A schedule torn down before its replacement was ready would
 * show as a changed digest, and one installed half-built as a check that
 * passes.
 */
static void
a_schedule_that_could_not_be_rebuilt_is_kept(void)
{
	relic_t *ctx;
	char *program;
	uint64_t digest;
	size_t analysed;
	size_t completed;
	size_t components;
	long budget;
	int status;
	bool appended;

	program = schedule_sweep_program();
	if (program == NULL) {
		check(false, SCENARIO_SCHEDULE, "the sweep program is built");
		return;
	}

	analysed = 0;
	completed = 0;

	for (budget = 0; budget < SCHEDULE_SWEEP; budget++) {
		ctx = relic_create();
		if (ctx == NULL) {
			check(false, SCENARIO_SCHEDULE,
			    "a context should be created");
			break;
		}
		relic_diag_set(ctx, count_error, NULL);

		if (!schedule_then_extend(ctx, program)) {
			check(false, SCENARIO_SCHEDULE,
			    "the program should load and pass a check before "
			    "the sweep begins");
			relic_destroy(ctx);
			break;
		}

		digest = schedule_digest(&ctx->program.schedule);
		components = ctx->program.schedule.component_count;
		errors_reported = 0;

		refuse_allocations_after(budget);
		status = relic_check(ctx);
		serve_every_allocation();

		if (status == 0) {
			completed++;
		} else {
			checkf(errors_reported == 1, SCENARIO_SCHEDULE,
			    "a check that ran out of memory should report that "
			    "alone, not %zu errors, with allocation %ld "
			    "refused", errors_reported, budget);
			checkf(schedule_digest(&ctx->program.schedule) ==
			    digest, SCENARIO_SCHEDULE,
			    "the schedule should be kept as it was, with "
			    "allocation %ld refused", budget);

			/*
			 * A refusal while compiling the new rule leaves it
			 * out of the program, and the kept schedule then
			 * describes every rule there is.  Once the rule is
			 * in, the kept schedule falls short of it.
			 */
			appended = ctx->program.rules.count ==
			    SCHEDULE_CHAIN + 1;
			checkf(relic_schedule_is_current(&ctx->program) ==
			    !appended, SCENARIO_SCHEDULE,
			    "the kept schedule should pass for current only "
			    "if the new rule was not compiled, with allocation "
			    "%ld refused", budget);

			if (appended)
				analysed++;

			status = relic_check(ctx);
			checkf(status == 0, SCENARIO_SCHEDULE,
			    "the program should pass a check once the "
			    "allocator recovers, with allocation %ld refused",
			    budget);
		}

		checkf(relic_schedule_is_current(&ctx->program) &&
		    ctx->program.schedule.component_count == components + 1,
		    SCENARIO_SCHEDULE,
		    "the new rule should end up scheduled, with allocation "
		    "%ld refused", budget);

		relic_destroy(ctx);
	}

	free(program);

	checkf(analysed > 0, SCENARIO_SCHEDULE,
	    "the sweep should reach a check that runs out of memory in the "
	    "analysis, in %d attempts", SCHEDULE_SWEEP);
	checkf(completed > 0, SCENARIO_SCHEDULE,
	    "the sweep should reach a check that completes, in %d attempts",
	    SCHEDULE_SWEEP);
}

/*
 * Prepare an empty edge/2 relation and intern the values its tuples are built
 * from, reporting NULL if anything in the preparation fails.
 *
 * All of this happens before the sweep starts refusing, so a failure here is
 * a broken fixture rather than the condition under test.
 */
static relic_relation_t *
sweep_relation(relic_t *ctx, relic_dom_t *values, size_t count)
{
	relic_location_t where;
	relic_pred_id_t pred;
	uint32_t name;
	size_t ordinal;
	char spelling[32];

	if (relic_symbol_intern(ctx, "edge", 4, &name) != 0)
		return (NULL);

	bzero(&where, sizeof(where));
	if (relic_pred_intern(ctx, name, 2, &where, &pred) != 0)
		return (NULL);

	for (ordinal = 0; ordinal < count; ordinal++) {
		snprintf(spelling, sizeof(spelling), "v%zu", ordinal);

		if (relic_value_intern_bytes(ctx, RELIC_SYMBOL, spelling,
		    strlen(spelling), &values[ordinal]) != 0)
			return (NULL);
	}

	return (relic_relation_read(ctx, pred));
}

/*
 * The storage total recomputed from every relation, which the running total
 * must equal whatever a refusal interrupted.
 */
static size_t
storage_by_hand(const relic_t *ctx)
{
	size_t bytes;
	size_t position;

	bytes = 0;
	for (position = 0; position < ctx->store.derived.count; position++) {
		if (ctx->store.derived.entries[position] != NULL)
			bytes += relic_relation_bytes(
			    ctx->store.derived.entries[position]);
	}
	for (position = 0; position < ctx->store.bases.count; position++) {
		if (ctx->store.bases.entries[position] != NULL)
			bytes += relic_relation_bytes(
			    ctx->store.bases.entries[position]);
	}

	return (bytes);
}

/*
 * Whether the index at `position` lists every tuple under its key: walking
 * from each tuple's key reaches that tuple, and the chains together hold
 * each tuple once.  A chain's length is counted only when walking it from its
 * own first tuple, so each chain is counted once.
 */
static bool
index_finds_every_tuple(const relic_relation_t *rel, uint32_t position)
{
	const relic_index_t *index;
	relic_tuple_id_t walked;
	relic_tuple_id_t first;
	relic_tuple_id_t id;
	size_t chained;
	bool reached;

	index = &rel->indexes[position];
	chained = 0;

	for (id = RELIC_TUPLE_FIRST; id <= rel->count; id++) {
		first = relic_index_first(index, rel,
		    relic_relation_tuple(rel, id));

		reached = false;
		for (walked = first; walked != RELIC_ID_NONE;
		    walked = relic_index_next(index, walked)) {
			if (walked == id)
				reached = true;
			if (first == id)
				chained++;
		}

		if (!reached)
			return (false);
	}

	return (chained == rel->count);
}

/*
 * Build an index over a relation and insert more tuples into it, with the
 * nth allocation refused, then let the allocator serve again and finish the
 * work.
 *
 * A build that could not finish must publish no index and leave the storage
 * total counting only what the relation holds; a lookup through a partial
 * index would miss the tuples it had not reached.  An insertion refused
 * while an index exists must leave the relation and every index agreeing, or
 * a lookup would miss a tuple the relation holds.  Once memory is back, the
 * build and the batch complete, and the index lists every tuple.
 */
static void
an_index_survives_a_failed_allocation(void)
{
	relic_dom_t values[INDEX_PRELUDE + INDEX_BATCH + 1];
	size_t built_then_refused;
	size_t build_refused;
	size_t completed;
	long budget;

	build_refused = 0;
	built_then_refused = 0;
	completed = 0;

	for (budget = 0; budget < INDEX_SWEEP; budget++) {
		relic_t *ctx;
		relic_relation_t *rel;
		relic_dom_t columns[2];
		uint32_t position;
		size_t ordinal;
		size_t inserted;
		int built;
		bool added;

		ctx = relic_create();
		if (ctx == NULL) {
			check(false, SCENARIO_INDEX,
			    "a context should be created");
			return;
		}
		relic_diag_set(ctx, count_error, NULL);

		rel = sweep_relation(ctx, values,
		    INDEX_PRELUDE + INDEX_BATCH + 1);
		if (rel == NULL) {
			check(false, SCENARIO_INDEX,
			    "the fixture should be prepared before the sweep");
			relic_destroy(ctx);
			return;
		}

		for (ordinal = 0; ordinal < INDEX_PRELUDE; ordinal++) {
			columns[0] = values[ordinal % 7];
			columns[1] = values[ordinal + 1];
			if (relic_relation_insert(ctx, rel, columns,
			    &added) != 0) {
				check(false, SCENARIO_INDEX,
				    "the prelude should be inserted before "
				    "the sweep");
				relic_destroy(ctx);
				return;
			}
		}

		errors_reported = 0;
		inserted = 0;

		refuse_allocations_after(budget);
		built = relic_relation_index(ctx, rel, 0x1, &position);
		for (ordinal = INDEX_PRELUDE; built == 0 &&
		    ordinal < INDEX_PRELUDE + INDEX_BATCH; ordinal++) {
			columns[0] = values[ordinal % 7];
			columns[1] = values[ordinal + 1];
			if (relic_relation_insert(ctx, rel, columns,
			    &added) != 0)
				break;
			inserted++;
		}
		serve_every_allocation();

		checkf(errors_reported == (built != 0 ||
		    inserted < INDEX_BATCH ? 1u : 0u), SCENARIO_INDEX,
		    "a refusal should be reported once, and %zu errors were, "
		    "with allocation %ld refused", errors_reported, budget);
		checkf(relic_relation_total_bytes(ctx) == storage_by_hand(ctx),
		    SCENARIO_INDEX, "the storage total should be %zu, not %zu, "
		    "with allocation %ld refused", storage_by_hand(ctx),
		    relic_relation_total_bytes(ctx), budget);

		if (built != 0) {
			build_refused++;
			checkf(rel->index_count == 0, SCENARIO_INDEX,
			    "a refused build should publish no index, with "
			    "allocation %ld refused", budget);
		} else if (inserted < INDEX_BATCH) {
			built_then_refused++;
			checkf(index_finds_every_tuple(rel, position),
			    SCENARIO_INDEX, "after a refused insertion the "
			    "index should list every tuple, with allocation "
			    "%ld refused", budget);
		} else {
			completed++;
		}

		/* With memory back, finish the build and the batch. */
		built = relic_relation_index(ctx, rel, 0x1, &position);
		for (ordinal = INDEX_PRELUDE; built == 0 &&
		    ordinal < INDEX_PRELUDE + INDEX_BATCH; ordinal++) {
			columns[0] = values[ordinal % 7];
			columns[1] = values[ordinal + 1];
			if (relic_relation_insert(ctx, rel, columns,
			    &added) != 0)
				built = -1;
		}

		checkf(built == 0 && rel->index_count == 1 &&
		    relic_relation_count(rel) == INDEX_PRELUDE + INDEX_BATCH &&
		    index_finds_every_tuple(rel, position), SCENARIO_INDEX,
		    "once memory is back the index should list all %d tuples, "
		    "with allocation %ld refused", INDEX_PRELUDE + INDEX_BATCH,
		    budget);

		relic_destroy(ctx);
	}

	checkf(build_refused > 0 && built_then_refused > 0 && completed > 0,
	    SCENARIO_INDEX, "the sweep should refuse a build (%zu), refuse an "
	    "insertion after one (%zu), and complete (%zu)", build_refused,
	    built_then_refused, completed);
}

/*
 * Insert a batch of tuples into a relation that already holds several, with
 * the nth allocation refused, then restore the allocator and insert the whole
 * batch again.
 *
 * What is at stake is the same invariant interning has, in a sharper form.  A
 * relation is a set, and a tuple whose columns were stored while its index
 * entry was not is a tuple that membership cannot find: the next insertion of
 * it would append a second copy, and every count, join and fixed-point
 * decision afterwards would be wrong with nothing having reported an error.
 * However far the interrupted batch got, the relation must afterwards hold
 * exactly the prelude and the batch, every one of those tuples must be found,
 * and inserting the batch a third time must add nothing at all.
 */
static void
a_relation_survives_a_failed_allocation(void)
{
	relic_dom_t values[INSERT_PRELUDE + INSERT_BATCH + 1];
	size_t refused;
	size_t completed;
	long budget;

	refused = 0;
	completed = 0;

	for (budget = 0; budget < INSERT_SWEEP; budget++) {
		relic_t *ctx;
		relic_relation_t *rel;
		relic_dom_t columns[2];
		size_t held;
		size_t ordinal;
		size_t inserted;
		size_t found;
		size_t added_again;
		bool added;

		ctx = relic_create();
		if (ctx == NULL) {
			check(false, SCENARIO_INSERT,
			    "a context should be created");
			return;
		}

		rel = sweep_relation(ctx, values,
		    INSERT_PRELUDE + INSERT_BATCH + 1);
		if (rel == NULL) {
			check(false, SCENARIO_INSERT,
			    "the fixture should be prepared before the sweep");
			relic_destroy(ctx);
			return;
		}

		for (ordinal = 0; ordinal < INSERT_PRELUDE; ordinal++) {
			columns[0] = values[0];
			columns[1] = values[ordinal + 1];

			if (relic_relation_insert(ctx, rel, columns,
			    &added) != 0) {
				check(false, SCENARIO_INSERT,
				    "the prelude should be inserted before "
				    "the sweep");
				relic_destroy(ctx);
				return;
			}
		}

		held = INSERT_PRELUDE + INSERT_BATCH;
		inserted = 0;

		refuse_allocations_after(budget);
		for (ordinal = INSERT_PRELUDE; ordinal < held; ordinal++) {
			columns[0] = values[0];
			columns[1] = values[ordinal + 1];

			if (relic_relation_insert(ctx, rel, columns,
			    &added) != 0)
				break;

			inserted++;
		}
		serve_every_allocation();

		if (inserted == INSERT_BATCH)
			completed++;
		else
			refused++;

		/*
		 * The batch is inserted again in full, so that whatever the
		 * refusal interrupted is completed.  Every one of these must
		 * succeed: the allocator is serving again, and an insertion
		 * of a tuple already present needs nothing.
		 */
		for (ordinal = INSERT_PRELUDE; ordinal < held; ordinal++) {
			columns[0] = values[0];
			columns[1] = values[ordinal + 1];

			checkf(relic_relation_insert(ctx, rel, columns,
			    &added) == 0, SCENARIO_INSERT,
			    "inserting should succeed once the allocator "
			    "recovers, with allocation %ld refused", budget);
		}

		checkf(relic_relation_count(rel) == held, SCENARIO_INSERT,
		    "the relation should hold %zu tuples, not %zu, with "
		    "allocation %ld refused", held,
		    relic_relation_count(rel), budget);

		/*
		 * Every tuple the relation is supposed to hold must be found,
		 * and inserting the batch once more must add nothing.  The
		 * second is the sharp check: a refusal that stored columns
		 * without indexing them would leave a tuple that membership
		 * misses, so this pass would append it a second time and the
		 * relation would quietly stop being a set.
		 */
		found = 0;
		added_again = 0;

		for (ordinal = 0; ordinal < held; ordinal++) {
			columns[0] = values[0];
			columns[1] = values[ordinal + 1];

			if (relic_relation_contains(rel, columns))
				found++;

			if (relic_relation_insert(ctx, rel, columns,
			    &added) == 0 && added)
				added_again++;
		}

		checkf(found == held, SCENARIO_INSERT,
		    "every one of %zu tuples should be found, not %zu, with "
		    "allocation %ld refused", held, found, budget);
		checkf(added_again == 0, SCENARIO_INSERT,
		    "no tuple should be added a second time, and %zu were, "
		    "with allocation %ld refused", added_again, budget);
		checkf(relic_relation_count(rel) == held, SCENARIO_INSERT,
		    "the relation should still hold %zu tuples, not %zu, with "
		    "allocation %ld refused", held,
		    relic_relation_count(rel), budget);

		/*
		 * The total the memory limit is enforced against must still
		 * describe what the relation holds.  A refusal that left the
		 * total counting an allocation that did not happen would make
		 * the limit fire on a program well inside it.
		 */
		checkf(relic_relation_total_bytes(ctx) ==
		    relic_relation_bytes(rel), SCENARIO_INSERT,
		    "the storage total (%zu) should be the relation's size "
		    "(%zu), with allocation %ld refused",
		    relic_relation_total_bytes(ctx),
		    relic_relation_bytes(rel), budget);

		relic_destroy(ctx);
	}

	/*
	 * The sweep has to straddle the point where the batch stops needing
	 * memory, or one of the two outcomes above is never reached and is
	 * checking nothing.
	 */
	checkf(refused > 0, SCENARIO_INSERT,
	    "the sweep should reach a batch that runs out of memory, in %d "
	    "attempts", INSERT_SWEEP);
	checkf(completed > 0, SCENARIO_INSERT,
	    "the sweep should reach a batch that completes, in %d attempts",
	    INSERT_SWEEP);
}

/*
 * The transitive closure of a chain n0 -> n1 -> ... of EVAL_CHAIN edges, and
 * its sink: the one node with an edge in and none out.
 */
static char *
evaluation_sweep_program(void)
{
	char *text;
	size_t used;
	size_t size;
	unsigned int i;

	size = (EVAL_CHAIN + 4) * 48;
	text = malloc(size);
	if (text == NULL)
		return (NULL);

	used = (size_t)snprintf(text, size,
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n"
	    "source(X) :- edge(X, _).\n"
	    "sink(Y) :- edge(_, Y), not source(Y).\n");

	for (i = 0; i < EVAL_CHAIN; i++) {
		used += (size_t)snprintf(text + used, size - used,
		    "edge(n%u, n%u).\n", i, i + 1);

		if (used >= size) {
			free(text);
			return (NULL);
		}
	}

	return (text);
}

/*
 * Whether a query's answer is $q0, the first a context names.
 */
static bool
answer_is_first(const relic_t *ctx, const relic_query_t *query)
{
	char name[RELIC_NAME_TEXT_MAX];

	return (strcmp(relic_relation_name(ctx, query->answer, name,
	    sizeof(name)), "$q0") == 0);
}

/*
 * The relation of a predicate given by name, or NULL when it cannot be had.
 */
static const relic_relation_t *
relation_named(relic_t *ctx, const char *name, uint16_t arity)
{
	relic_pred_id_t pred;
	uint32_t symbol;

	if (relic_symbol_intern(ctx, name, strlen(name), &symbol) != 0)
		return (NULL);

	if (relic_pred_intern(ctx, symbol, arity, NULL, &pred) != 0)
		return (NULL);

	return (relic_relation_read(ctx, pred));
}

static int
count_answer(void *callback_ctx, const relic_value_t *values RELIC_UNUSED,
    size_t count RELIC_UNUSED)
{
	size_t *rows;

	rows = callback_ctx;
	(*rows)++;

	return (0);
}

/*
 * Prepare and run a query with the nth allocation refused, then run it again
 * with the allocator restored, for each way a query can be answered.
 *
 * A query that runs out of memory must say so once and deliver no rows,
 * since a partial answer would look like a complete one.  The retry must
 * reach exactly the answer an undisturbed run does: the failed evaluation
 * left its relations stale, part of a fixed point, and the retry discards
 * them and starts again.  A relation holding more paths than the closure has
 * pairs, or a sink relation holding other than the chain's one sink, would
 * mean something of the failed run survived into the retry.
 *
 * Answered from the whole fixed point, the closure is the path relation.
 * Answered through the Magic rewrite, it is path$bf, which holds the whole
 * closure too, because the query's binding reaches every node of the chain;
 * the negated sink is read completely, so the sweep also lands in the part
 * of the program evaluated before the rewrite.  A query that reaches path
 * free reads it completely instead of rewriting, into the path relation.
 * And a query that calls sink bound before calling it free finds its
 * adornments in a second search, which reads sink through sink$f whatever
 * the call binds.
 */
static void
a_query_that_ran_out_of_memory_can_be_retried(void)
{
	static const eval_sweep_t sweeps[] = {
		{ "from the whole fixed point", "path(n0, X)?", "path", "sink",
		    RELIC_F_NO_MAGIC, EVAL_CHAIN },
		{ "through the Magic rewrite", "path(n0, X), not sink(X)?",
		    "path$bf", "sink", 0, EVAL_CHAIN - 1 },
		{ "reading what it reaches completely",
		    "path(X, Y), not sink(Y)?", "path", "sink", 0,
		    EVAL_CHAIN * (EVAL_CHAIN + 1) / 2 - EVAL_CHAIN },
		{ "through a second search", "path(n0, X), sink(X), sink(Y)?",
		    "path$bf", "sink$f", 0, 1 }
	};
	char *program;
	size_t i;

	program = evaluation_sweep_program();
	if (program == NULL) {
		check(false, SCENARIO_EVALUATE, "the sweep program is built");
		return;
	}

	for (i = 0; i < sizeof(sweeps) / sizeof(sweeps[0]); i++)
		sweep_query_retry(&sweeps[i], program);

	free(program);
}

/*
 * One strategy's sweep of a_query_that_ran_out_of_memory_can_be_retried().
 */
static void
sweep_query_retry(const eval_sweep_t *sweep, const char *program)
{
	relic_config_t config;
	relic_query_t *query;
	size_t completed;
	size_t evaluating;
	size_t expected_paths;
	size_t refused;
	size_t rows;
	long budget;

	bzero(&config, sizeof(config));
	config.flags = sweep->flags;

	expected_paths = (size_t)EVAL_CHAIN * (EVAL_CHAIN + 1) / 2;
	completed = 0;
	evaluating = 0;
	refused = 0;

	for (budget = 0; budget < EVAL_SWEEP; budget++) {
		const relic_relation_t *paths;
		const relic_relation_t *sinks;
		relic_t *ctx;
		int status;

		ctx = relic_create_config(&config);
		if (ctx == NULL) {
			check(false, SCENARIO_EVALUATE,
			    "a context should be created");
			break;
		}
		relic_diag_set(ctx, count_error, NULL);

		if (relic_load_string(ctx, "sweep.dl", program) != 0 ||
		    relic_check(ctx) != 0) {
			check(false, SCENARIO_EVALUATE,
			    "the program should load and check before the "
			    "sweep begins");
			relic_destroy(ctx);
			break;
		}

		errors_reported = 0;
		rows = 0;

		refuse_allocations_after(budget);
		query = relic_query_prepare(ctx, NULL, sweep->query);
		status = -1;
		if (query != NULL)
			status = relic_query_run(ctx, query, count_answer,
			    &rows);
		serve_every_allocation();

		if (status == 0) {
			completed++;
		} else {
			refused++;
			if (query != NULL)
				evaluating++;
		}

		checkf(errors_reported == (status != 0 ? 1u : 0u),
		    SCENARIO_EVALUATE,
		    "a query answered %s that %s should report %d message(s), "
		    "not %zu, with allocation %ld refused", sweep->strategy,
		    status != 0 ? "failed" : "succeeded", status != 0 ? 1 : 0,
		    errors_reported, budget);
		checkf(rows == (status != 0 ? 0u : sweep->rows),
		    SCENARIO_EVALUATE,
		    "a query answered %s that %s should deliver %zu rows, not "
		    "%zu, with allocation %ld refused", sweep->strategy,
		    status != 0 ? "failed" : "succeeded",
		    status != 0 ? 0 : sweep->rows, rows, budget);
		checkf(ctx->account.arena_bytes == 0 &&
		    ctx->account.store_bytes == storage_by_hand(ctx),
		    SCENARIO_EVALUATE,
		    "a query answered %s should leave the account holding the "
		    "relations' %zu bytes and no arena's, not %zu and %zu, with "
		    "allocation %ld refused", sweep->strategy,
		    storage_by_hand(ctx), ctx->account.store_bytes,
		    ctx->account.arena_bytes, budget);

		if (query == NULL)
			query = relic_query_prepare(ctx, NULL, sweep->query);

		/*
		 * A query that ran out of memory was never published, so the
		 * one that eventually is takes the number it would have had:
		 * the program holds no query of its own, so this is $q0.
		 */
		checkf(query != NULL && answer_is_first(ctx, query),
		    SCENARIO_EVALUATE,
		    "the retried query answered %s should answer into $q0, "
		    "with allocation %ld refused", sweep->strategy, budget);

		rows = 0;
		checkf(query != NULL &&
		    relic_query_run(ctx, query, count_answer, &rows) == 0 &&
		    rows == sweep->rows, SCENARIO_EVALUATE,
		    "the retry answered %s should answer %zu rows, not %zu, "
		    "with allocation %ld refused", sweep->strategy, sweep->rows,
		    rows, budget);

		paths = relation_named(ctx, sweep->paths, 2);
		checkf(paths != NULL &&
		    relic_relation_count(paths) == expected_paths,
		    SCENARIO_EVALUATE,
		    "answered %s, the %s relation should hold %zu tuples, not "
		    "%zu, with allocation %ld refused", sweep->strategy,
		    sweep->paths, expected_paths,
		    paths != NULL ? relic_relation_count(paths) : 0, budget);

		sinks = relation_named(ctx, sweep->sinks, 1);
		checkf(sinks != NULL && relic_relation_count(sinks) == 1,
		    SCENARIO_EVALUATE,
		    "answered %s, the %s relation should hold 1 tuple, not "
		    "%zu, with allocation %ld refused", sweep->strategy,
		    sweep->sinks,
		    sinks != NULL ? relic_relation_count(sinks) : 0, budget);

		relic_query_destroy(query);
		relic_destroy(ctx);
	}

	/*
	 * The sweep has to reach a refusal inside evaluation itself, not
	 * only in preparing the query, and a run that completes; otherwise
	 * one of the outcomes above is never checked.
	 */
	checkf(evaluating > 0, SCENARIO_EVALUATE,
	    "answered %s, the sweep should reach a query that runs out of "
	    "memory while evaluating, in %d attempts", sweep->strategy,
	    EVAL_SWEEP);
	checkf(refused > evaluating, SCENARIO_EVALUATE,
	    "answered %s, the sweep should reach a query that runs out of "
	    "memory while being prepared, in %d attempts", sweep->strategy,
	    EVAL_SWEEP);
	checkf(completed > 0, SCENARIO_EVALUATE,
	    "answered %s, the sweep should reach a query that completes, in "
	    "%d attempts", sweep->strategy, EVAL_SWEEP);
}

/*
 * Answer a query whose Magic rewrite outgrows its budget with the nth
 * allocation refused, for every allocation an undisturbed run makes, then
 * answer it again with the allocator restored.
 *
 * A refusal can land in the rewrite before it is abandoned, or in answering
 * from the whole program after it; either way the query must fail with one
 * error and no rows, not fall back a second time or deliver a partial
 * answer.  The warning that the rewrite was abandoned is reported at most
 * once, and only by a run that got as far as abandoning it.  The retry must
 * then answer exactly as an undisturbed run does.
 */
static void
an_abandoned_rewrite_out_of_memory_reports_once(void)
{
	static const char query_text[] = "p(c, Y2, Y3, Y4, Y5, Y6)?";
	relic_query_t *query;
	char *program;
	unsigned long undisturbed;
	size_t abandoned_then_refused;
	size_t completed;
	size_t rows;
	long budget;

	program = widening_program(WIDENING_ARITY);
	if (program == NULL) {
		check(false, SCENARIO_FALLBACK, "the sweep program is built");
		return;
	}

	undisturbed = 0;
	abandoned_then_refused = 0;
	completed = 0;

	for (budget = 0; budget < FALLBACK_SWEEP &&
	    (undisturbed == 0 || budget <= (long)undisturbed); budget++) {
		relic_t *ctx;
		unsigned long before;
		int status;

		ctx = relic_create();
		if (ctx == NULL) {
			check(false, SCENARIO_FALLBACK,
			    "a context should be created");
			break;
		}
		relic_diag_set(ctx, count_error, NULL);

		if (relic_load_string(ctx, "widening.dl", program) != 0 ||
		    relic_check(ctx) != 0) {
			check(false, SCENARIO_FALLBACK,
			    "the program should load and check before the "
			    "sweep begins");
			relic_destroy(ctx);
			break;
		}

		errors_reported = 0;
		warnings_reported = 0;
		rows = 0;

		before = allocations_seen;
		refuse_allocations_after(budget);
		query = relic_query_prepare(ctx, NULL, query_text);
		status = -1;
		if (query != NULL)
			status = relic_query_run(ctx, query, count_answer,
			    &rows);
		serve_every_allocation();

		/*
		 * The first run to succeed was refused nothing, so what it
		 * allocated bounds the sweep.
		 */
		if (status == 0) {
			completed++;
			if (undisturbed == 0)
				undisturbed = allocations_seen - before;
		} else if (warnings_reported > 0) {
			abandoned_then_refused++;
		}

		checkf(errors_reported == (status != 0 ? 1u : 0u),
		    SCENARIO_FALLBACK,
		    "an abandoned rewrite that %s should report %d error(s), "
		    "not %zu, with allocation %ld refused",
		    status != 0 ? "failed" : "succeeded", status != 0 ? 1 : 0,
		    errors_reported, budget);
		checkf(warnings_reported <= 1 &&
		    (status != 0 || warnings_reported == 1), SCENARIO_FALLBACK,
		    "an abandoned rewrite that %s should warn %s, not %zu "
		    "times, with allocation %ld refused",
		    status != 0 ? "failed" : "succeeded",
		    status != 0 ? "at most once" : "once", warnings_reported,
		    budget);
		checkf(rows == (status != 0 ? 0u : 1u), SCENARIO_FALLBACK,
		    "an abandoned rewrite that %s should deliver %d rows, not "
		    "%zu, with allocation %ld refused",
		    status != 0 ? "failed" : "succeeded", status != 0 ? 0 : 1,
		    rows, budget);

		if (query == NULL)
			query = relic_query_prepare(ctx, NULL, query_text);

		errors_reported = 0;
		rows = 0;
		checkf(query != NULL &&
		    relic_query_run(ctx, query, count_answer, &rows) == 0 &&
		    rows == 1 && errors_reported == 0, SCENARIO_FALLBACK,
		    "the retry should answer 1 row and report no error, not %zu "
		    "rows and %zu errors, with allocation %ld refused", rows,
		    errors_reported, budget);

		relic_query_destroy(query);
		relic_destroy(ctx);
	}

	/*
	 * The sweep has to reach a refusal after the rewrite was abandoned,
	 * in answering from the whole program; otherwise only the rewrite's
	 * own failures were checked.
	 */
	checkf(abandoned_then_refused > 0, SCENARIO_FALLBACK,
	    "the sweep should reach a query that runs out of memory after "
	    "abandoning its rewrite, in %ld attempts", budget);
	checkf(completed > 0, SCENARIO_FALLBACK,
	    "the sweep should reach a query that completes, in %ld attempts",
	    budget);

	free(program);
}

/*
 * The chain w0(a), w1(X) :- w0(X), and so on to WIDE_PREDICATES - 1.
 */
static char *
wide_sweep_program(void)
{
	char *text;
	size_t used;
	size_t size;
	unsigned int i;

	size = (WIDE_PREDICATES + 1) * 32;
	text = malloc(size);
	if (text == NULL)
		return (NULL);

	used = (size_t)snprintf(text, size, "w0(a).\n");

	for (i = 1; i < WIDE_PREDICATES; i++) {
		used += (size_t)snprintf(text + used, size - used,
		    "w%u(X) :- w%u(X).\n", i, i - 1);

		if (used >= size) {
			free(text);
			return (NULL);
		}
	}

	return (text);
}

/*
 * Materializing a program with many predicates runs out of memory while the
 * evaluation is being set up or a component planned, and says so exactly
 * once however many of its requests the exhaustion refused.  The relations it
 * leaves are stale, and a retry with memory available discards them and
 * reaches the end of the chain.
 */
static void
a_materialization_out_of_memory_reports_once(void)
{
	char *program;
	size_t held;
	size_t refused;
	long budget;

	program = wide_sweep_program();
	if (program == NULL) {
		check(false, SCENARIO_MATERIALIZE, "the sweep program is built");
		return;
	}

	refused = 0;

	for (budget = 0; budget < WIDE_SWEEP; budget++) {
		relic_t *ctx;
		int status;

		ctx = relic_create();
		if (ctx == NULL) {
			check(false, SCENARIO_MATERIALIZE,
			    "a context should be created");
			break;
		}
		relic_diag_set(ctx, count_error, NULL);

		if (relic_load_string(ctx, "wide.dl", program) != 0 ||
		    relic_check(ctx) != 0) {
			check(false, SCENARIO_MATERIALIZE,
			    "the program should load and check before the "
			    "sweep begins");
			relic_destroy(ctx);
			break;
		}

		errors_reported = 0;

		refuse_allocations_after(budget);
		status = relic_materialize(ctx);
		serve_every_allocation();
		held = relic_relation_total_tuples(ctx);

		if (status != 0)
			refused++;

		checkf(errors_reported == (status != 0 ? 1u : 0u),
		    SCENARIO_MATERIALIZE,
		    "a materialization that %s should report %d message(s), "
		    "not %zu, with allocation %ld refused",
		    status != 0 ? "failed" : "succeeded", status != 0 ? 1 : 0,
		    errors_reported, budget);
		checkf(status == 0 || (held < WIDE_PREDICATES &&
		    !relic_relations_current(ctx)), SCENARIO_MATERIALIZE,
		    "a refused materialization should be left stale, holding "
		    "%zu tuples, with allocation %ld refused", held, budget);

		status = relic_materialize(ctx);
		held = relic_relation_total_tuples(ctx);
		checkf(status == 0 && held == WIDE_PREDICATES,
		    SCENARIO_MATERIALIZE,
		    "the retry should hold %d tuples, not %zu, with allocation "
		    "%ld refused", WIDE_PREDICATES, held, budget);

		relic_destroy(ctx);
	}

	free(program);

	checkf(refused == WIDE_SWEEP, SCENARIO_MATERIALIZE,
	    "every step of the sweep should refuse the materialization, "
	    "%zu of %d did", refused, WIDE_SWEEP);
}

/*
 * Build the value `alpha`, add f(alpha) and collect f(X)? into a result,
 * storing its row count in *rowsp.  Returns 0, or -1 at the first call that
 * failed.
 */
static int
add_and_collect(relic_t *ctx, size_t *rowsp)
{
	relic_value_t value;
	relic_query_t *query;
	relic_result_t *result;

	if (relic_value_symbol(ctx, "alpha", 5, &value) != 0)
		return (-1);
	if (relic_fact_add(ctx, "f", 1, &value) != 0)
		return (-1);

	query = relic_query_prepare(ctx, NULL, "f(X)?");
	if (query == NULL)
		return (-1);
	result = relic_query_collect(ctx, query);
	relic_query_destroy(query);
	if (result == NULL)
		return (-1);

	*rowsp = relic_result_rows(result);
	relic_result_destroy(result);

	return (0);
}

/*
 * Build a value, add it as a fact and collect a query's answer with the nth
 * allocation refused, for every allocation an undisturbed run makes, then do
 * it all again with the allocator restored.
 *
 * The context starts empty, so that building the fact's statement makes the
 * program arena's first allocation rather than finding room left by a load.
 * A refusal can land in interning the value, in building the statement, in
 * preparing or answering the query, or in storing a row of the result.
 * Wherever it lands, the call it lands in fails with exactly one error, and
 * the retry answers the one row an undisturbed run does: the fact added
 * twice is one tuple, and nothing half-added is left behind.
 */
static void
an_added_fact_or_collected_answer_reports_once(void)
{
	unsigned long undisturbed;
	size_t refused;
	size_t completed;
	size_t rows;
	long budget;

	undisturbed = 0;
	refused = 0;
	completed = 0;

	for (budget = 0; budget < INTERFACE_SWEEP &&
	    (undisturbed == 0 || budget <= (long)undisturbed); budget++) {
		relic_t *ctx;
		unsigned long before;
		int status;

		ctx = relic_create();
		if (ctx == NULL) {
			check(false, SCENARIO_INTERFACE,
			    "a context should be created");
			break;
		}
		relic_diag_set(ctx, count_error, NULL);

		errors_reported = 0;
		rows = 0;

		before = allocations_seen;
		refuse_allocations_after(budget);
		status = add_and_collect(ctx, &rows);
		serve_every_allocation();

		if (status == 0) {
			completed++;
			if (undisturbed == 0)
				undisturbed = allocations_seen - before;
		} else
			refused++;

		checkf(errors_reported == (status != 0 ? 1u : 0u),
		    SCENARIO_INTERFACE,
		    "a run that %s should report %d error(s), not %zu, with "
		    "allocation %ld refused", status != 0 ? "failed" :
		    "succeeded", status != 0 ? 1 : 0, errors_reported, budget);

		rows = 0;
		checkf(add_and_collect(ctx, &rows) == 0 && rows == 1,
		    SCENARIO_INTERFACE,
		    "the retry should collect 1 row, not %zu, with allocation "
		    "%ld refused", rows, budget);

		relic_destroy(ctx);
	}

	checkf(refused > 0 && completed > 0, SCENARIO_INTERFACE,
	    "the sweep should reach both a refused run and a completed one, "
	    "not %zu and %zu", refused, completed);
}

int
main(void)
{
	context_creation_reports_a_failed_allocation();
	context_creation_fails_without_entropy();
	interning_a_symbol_survives_a_failed_allocation();
	interning_a_value_survives_a_failed_allocation();
	a_load_that_ran_out_of_memory_adds_nothing();
	an_import_that_ran_out_of_memory_adds_nothing();
	a_refusal_in_the_lookahead_reports_once();
	a_check_that_ran_out_of_memory_can_be_retried();
	an_analysis_that_ran_out_of_memory_says_nothing();
	a_schedule_that_could_not_be_rebuilt_is_kept();
	a_relation_survives_a_failed_allocation();
	an_index_survives_a_failed_allocation();
	a_query_that_ran_out_of_memory_can_be_retried();
	a_materialization_out_of_memory_reports_once();
	an_abandoned_rewrite_out_of_memory_reports_once();
	an_added_fact_or_collected_answer_reports_once();

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
