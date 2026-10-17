/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * The merge of a deferred round, given rounds built by hand.
 *
 * tests/unit/test_round.c reaches the merge through rounds a program runs,
 * where the pool decides which worker runs each task and every value a
 * worker holds pending is one some candidate of it holds.  Here the test
 * fills each worker's candidates and pending values itself and calls
 * relic_round_merge() directly, so that it can fix which worker gathered
 * each task and hold values no candidate refers to, which no real round
 * produces.  The targets are two empty relations, p/2 and q/2, of a
 * context that has assigned only the integer 1 and the string "one".
 *
 * A worker's pending values are measured through compact descriptors and
 * the merge-owned resolution is opened only once some candidate is known
 * to hold one, sized by those values alone.  One worker's large string no
 * candidate holds makes that visible through the memory limit: the
 * resolution would copy its bytes, so a limit with less room than that
 * admits a merge that opens no resolution and refuses one that does.
 *
 * The merge is also given what no worker makes -- pending tables larger
 * than a marker or a size counts, markers past a worker's values -- and
 * must decline each before it allocates for it or reads past the worker's
 * table, changing nothing.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_UNUSED		"a merge opens nothing for values no " \
				"candidate holds"
#define	SCENARIO_PARTIAL	"a merge resolves only the values " \
				"candidates hold"
#define	SCENARIO_REFUSED	"a merge whose resolution is refused " \
				"changes nothing"
#define	SCENARIO_EMPTY		"a merge with no string bytes gives back " \
				"its charge"
#define	SCENARIO_ORDER		"a merge numbers values in task order"
#define	SCENARIO_SIZES		"a merge declines pending sizes it cannot " \
				"represent"
#define	SCENARIO_MARKERS	"a merge declines markers naming nothing"
#define	SCENARIO_INTERRUPT	"an interrupted merge publishes nothing"

/*
 * The round's targets, p/2 and q/2; the tasks a round holds, one item
 * each; and the candidates and pending values each worker has room for.
 */
#define	HAND_TARGETS		2
#define	HAND_ARITY		2
#define	HAND_TASKS		4
#define	HAND_TUPLES		4
#define	HAND_VALUES		8

/*
 * The bytes of the pending string no candidate holds, and the room the
 * memory limit leaves for the merge beyond what the context holds: enough
 * for everything the merge allocates but a copy of that string.
 */
#define	LARGE_STRING_BYTES	(4u * 1024u * 1024u)
#define	LIMIT_HEADROOM		(1024u * 1024u)

/* Integers the context has not assigned. */
#define	NEW_INTEGER		7000007
#define	OTHER_INTEGER		7000009

typedef struct candidates_snapshot candidates_snapshot_t;
typedef struct hand_merge hand_merge_t;

/*
 * A round built by hand over a context and a pool of its own.  Task t is
 * item t, whose target is `items[t].target`.
 */
struct hand_merge {
	relic_t			*ctx;
	relic_pool_t		*pool;
	relic_round_t		 round;
	relic_round_item_t	 items[HAND_TASKS];
	relic_relation_t	*targets[HAND_TARGETS];
	relic_destination_t destinations[HAND_TARGETS];
	relic_destination_t *destination_pointers[HAND_TARGETS];
};

/*
 * What a worker's candidates held before a merge: their columns, hashes
 * and stripes, and the stripe counts.
 */
struct candidates_snapshot {
	relic_dom_t	words[HAND_TUPLES * HAND_ARITY];
	uint64_t	hashes[HAND_TUPLES];
	uint8_t		stripes[HAND_TUPLES];
	size_t		by_stripe[RELIC_STRIPES];
	size_t		count;
};

/* The string no candidate holds: LARGE_STRING_BYTES zero bytes. */
static char	 large_string[LARGE_STRING_BYTES];

static void	 a_merge_opens_nothing_for_unheld_values(void);
static void	 a_merge_resolves_only_held_values(void);
static void	 a_merge_whose_resolution_is_refused_changes_nothing(void);
static void	 a_merge_with_no_string_bytes_gives_back_its_charge(void);
static void	 a_merge_numbers_values_in_task_order(void);
static void	 a_merge_declines_unrepresentable_sizes(void);
static void	 a_merge_declines_markers_naming_nothing(void);
static void	 an_interrupted_merge_publishes_nothing(void);
static void	 merge_declines(hand_merge_t *hand, const char *scenario,
		    const char *what);
static bool	 open_partial(hand_merge_t *hand, const char *scenario);
static bool	 partial_round(hand_merge_t *hand);
static bool	 order_round(hand_merge_t *hand);
static bool	 hand_open(hand_merge_t *hand, size_t workers);
static void	 hand_close(hand_merge_t *hand);
static relic_dom_t hand_integer(hand_merge_t *hand, size_t worker,
		    int64_t integer);
static relic_dom_t hand_string(hand_merge_t *hand, size_t worker,
		    const char *bytes, size_t length);
static relic_dom_t hand_value(hand_merge_t *hand, size_t worker,
		    const relic_pending_t *value);
static bool	 hand_task(hand_merge_t *hand, size_t worker, size_t target,
		    const relic_dom_t *tuples, size_t count);
static bool	 hand_pair(hand_merge_t *hand, size_t worker, size_t target,
		    relic_dom_t first, relic_dom_t second);
static bool	 hand_merge(hand_merge_t *hand);
static void	 limit_to(relic_t *ctx, size_t headroom);
static size_t	 round_bytes(const relic_t *ctx);
static bool	 holds(const hand_merge_t *hand, size_t target,
		    relic_dom_t first, relic_dom_t second);
static relic_tuple_id_t tuple_id(const hand_merge_t *hand, size_t target,
		    relic_dom_t first, relic_dom_t second);
static bool	 candidates_rehashed(const hand_merge_t *hand);
static void	 snapshot_candidates(const relic_candidates_t *candidates,
		    candidates_snapshot_t *snapshot);
static bool	 candidates_unchanged(const relic_candidates_t *candidates,
		    const candidates_snapshot_t *snapshot);
static relic_dom_t find_string(const relic_t *ctx, const char *bytes);

int
main(void)
{
	a_merge_opens_nothing_for_unheld_values();
	a_merge_resolves_only_held_values();
	a_merge_whose_resolution_is_refused_changes_nothing();
	a_merge_with_no_string_bytes_gives_back_its_charge();
	a_merge_numbers_values_in_task_order();
	a_merge_declines_unrepresentable_sizes();
	a_merge_declines_markers_naming_nothing();
	an_interrupted_merge_publishes_nothing();

	return (check_summary("test_merge"));
}

/*
 * Preparation may reserve capacity, but an interrupt observed at its end
 * must not publish any promised value or tuple. Private candidates may
 * have been rewritten; they are discarded when the round is declined.
 */
static void
an_interrupted_merge_publishes_nothing(void)
{
	hand_merge_t hand;
	size_t before;
	bool merged;

	if (!open_partial(&hand, SCENARIO_INTERRUPT))
		return;
	before = round_bytes(hand.ctx);
	relic_signal_flag_raise(&hand.ctx->interrupted);
	merged = hand_merge(&hand);
	check(!merged, SCENARIO_INTERRUPT, "the merge declines");
	check(relic_relation_count(hand.targets[0]) == 0 &&
	    relic_relation_count(hand.targets[1]) == 0, SCENARIO_INTERRUPT,
	    "neither target publishes a tuple identifier");
	check(find_string(hand.ctx, "amber") == 0 &&
	    find_string(hand.ctx, "blue") == 0 &&
	    relic_value_find_integer(hand.ctx, NEW_INTEGER) == 0,
	    SCENARIO_INTERRUPT, "no promised value is published");
	check(round_bytes(hand.ctx) == before, SCENARIO_INTERRUPT,
	    "preparation gives back every round charge");
	relic_signal_flag_lower(&hand.ctx->interrupted);
	hand_close(&hand);
}

/*
 * One worker holds two pending values, a new integer and the large string,
 * and its one candidate holds neither, only values the context assigned.
 * Under a limit too tight for a copy of the string the merge succeeds, so
 * it opened no resolution, interns neither value and gives back every byte
 * it charged.
 */
static void
a_merge_opens_nothing_for_unheld_values(void)
{
	hand_merge_t hand;
	relic_dom_t one;
	relic_dom_t integer_one;
	size_t before;
	bool built;
	bool merged;

	one = 0;
	integer_one = 0;
	built = hand_open(&hand, 1);
	if (built) {
		one = find_string(hand.ctx, "one");
		integer_one = relic_value_find_integer(hand.ctx, 1);
		built = hand_integer(&hand, 0, NEW_INTEGER) != 0;
	}
	if (built)
		built = hand_string(&hand, 0, large_string,
		    sizeof(large_string)) != 0;
	if (built)
		built = hand_pair(&hand, 0, 0, one, integer_one);
	if (!built) {
		check(false, SCENARIO_UNUSED, "the round is built");
		hand_close(&hand);
		return;
	}

	limit_to(hand.ctx, LIMIT_HEADROOM);
	before = round_bytes(hand.ctx);
	merged = hand_merge(&hand);
	check(merged, SCENARIO_UNUSED, "the merge is admitted under a limit "
	    "with no room for the unheld string");
	check(holds(&hand, 0, one, integer_one), SCENARIO_UNUSED,
	    "p holds the candidate");
	check(relic_value_find_integer(hand.ctx, NEW_INTEGER) == 0 &&
	    relic_value_find_string(hand.ctx, large_string,
	    sizeof(large_string)) == 0, SCENARIO_UNUSED,
	    "neither unheld value is interned");
	check(round_bytes(hand.ctx) == before, SCENARIO_UNUSED,
	    "the merge gives back every byte it charged");

	hand_close(&hand);
}

/*
 * The round partial_round() builds, with no limit.  The merge interns the
 * three values the candidates hold, numbering the strings by where task
 * order first meets them rather than by their markers, and leaves the
 * large string alone.
 */
static void
a_merge_resolves_only_held_values(void)
{
	hand_merge_t hand;
	relic_dom_t amber;
	relic_dom_t blue;
	relic_dom_t integer;
	size_t before;
	bool merged;

	if (!open_partial(&hand, SCENARIO_PARTIAL))
		return;

	before = round_bytes(hand.ctx);
	merged = hand_merge(&hand);
	check(merged, SCENARIO_PARTIAL, "the merge succeeds");
	amber = find_string(hand.ctx, "amber");
	blue = find_string(hand.ctx, "blue");
	integer = relic_value_find_integer(hand.ctx, NEW_INTEGER);
	check(amber != 0 && blue != 0 && integer != 0, SCENARIO_PARTIAL,
	    "every held value is interned");
	check(blue < amber, SCENARIO_PARTIAL, "blue, met first in task "
	    "order, is numbered before amber, marked first");
	check(relic_value_find_string(hand.ctx, large_string,
	    sizeof(large_string)) == 0, SCENARIO_PARTIAL,
	    "the unheld string is not interned");
	check(holds(&hand, 0, blue, integer) && holds(&hand, 1, amber, blue),
	    SCENARIO_PARTIAL, "p and q hold the resolved tuples");
	check(candidates_rehashed(&hand), SCENARIO_PARTIAL,
	    "every candidate is rewritten and hashed into its stripe");
	check(round_bytes(hand.ctx) == before, SCENARIO_PARTIAL,
	    "the merge gives back every byte it charged");

	hand_close(&hand);
}

/*
 * The round partial_round() builds, under the limit
 * a_merge_opens_nothing_for_unheld_values() merges under.  Its candidates
 * hold pending values, so the merge opens the resolution, whose copy of
 * the workers' strings includes the large one, and the limit refuses it:
 * the merge declines having changed nothing.  With room for that copy as
 * well the same round merges, so the tight limit refused only the copy.
 */
static void
a_merge_whose_resolution_is_refused_changes_nothing(void)
{
	hand_merge_t hand;

	if (!open_partial(&hand, SCENARIO_REFUSED))
		return;

	limit_to(hand.ctx, LIMIT_HEADROOM);
	merge_declines(&hand, SCENARIO_REFUSED, "the limit refuses the "
	    "resolution");

	limit_to(hand.ctx, LIMIT_HEADROOM + LARGE_STRING_BYTES);
	check(hand_merge(&hand), SCENARIO_REFUSED, "with room for the copy "
	    "of the strings the merge succeeds");

	hand_close(&hand);
}

/*
 * One worker holds two pending values, the empty string and the new
 * integer, which its one candidate holds, and no string bytes at all.  The
 * resolution still takes a byte for its buffer, so that the empty string
 * names a byte that exists, charged with the map.  The merge interns both
 * values and gives back every byte it charged.
 */
static void
a_merge_with_no_string_bytes_gives_back_its_charge(void)
{
	hand_merge_t hand;
	relic_dom_t empty_marker;
	relic_dom_t integer_marker;
	relic_dom_t empty;
	relic_dom_t integer;
	size_t before;
	bool built;

	empty_marker = 0;
	integer_marker = 0;
	built = hand_open(&hand, 1);
	if (built) {
		empty_marker = hand_string(&hand, 0, "", 0);
		integer_marker = hand_integer(&hand, 0, NEW_INTEGER);
		built = empty_marker != 0 && integer_marker != 0;
	}
	if (built)
		built = hand_pair(&hand, 0, 0, empty_marker, integer_marker);
	if (!built) {
		check(false, SCENARIO_EMPTY, "the round is built");
		hand_close(&hand);
		return;
	}

	before = round_bytes(hand.ctx);
	check(hand_merge(&hand), SCENARIO_EMPTY, "the merge succeeds");
	empty = relic_value_find_string(hand.ctx, "", 0);
	integer = relic_value_find_integer(hand.ctx, NEW_INTEGER);
	check(empty != 0 && integer != 0 && holds(&hand, 0, empty, integer),
	    SCENARIO_EMPTY, "p holds the tuple with both values interned");
	check(round_bytes(hand.ctx) == before, SCENARIO_EMPTY,
	    "the merge gives back every byte it charged");

	hand_close(&hand);
}

/*
 * The round order_round() builds.  Task order meets amber before blue and
 * NEW_INTEGER before OTHER_INTEGER, so the serial run numbers them so;
 * reading worker 0's values before worker 1's would number blue and
 * OTHER_INTEGER first.  Every marker must be read as its own worker's
 * value, the tuples must take their identifiers in task order, and each
 * worker's candidates must come out rewritten and hashed into their
 * stripes.
 */
static void
a_merge_numbers_values_in_task_order(void)
{
	hand_merge_t hand;
	relic_dom_t amber;
	relic_dom_t blue;
	relic_dom_t first_integer;
	relic_dom_t other_integer;
	bool built;

	/* A build without threads has a pool of one worker. */
	if (relic_pool_size(2) < 2)
		return;

	built = hand_open(&hand, 2);
	if (built)
		built = order_round(&hand);
	if (!built) {
		check(false, SCENARIO_ORDER, "the round is built");
		hand_close(&hand);
		return;
	}

	check(hand_merge(&hand), SCENARIO_ORDER, "the merge succeeds");
	amber = find_string(hand.ctx, "amber");
	blue = find_string(hand.ctx, "blue");
	first_integer = relic_value_find_integer(hand.ctx, NEW_INTEGER);
	other_integer = relic_value_find_integer(hand.ctx, OTHER_INTEGER);
	check(amber != 0 && blue != 0 && first_integer != 0 &&
	    other_integer != 0, SCENARIO_ORDER, "every value is interned");
	check(amber < blue && first_integer < other_integer, SCENARIO_ORDER,
	    "strings and integers are numbered as task order meets them");
	check(holds(&hand, 0, amber, first_integer) &&
	    holds(&hand, 0, amber, blue) &&
	    holds(&hand, 1, other_integer, blue) &&
	    holds(&hand, 1, first_integer, blue), SCENARIO_ORDER,
	    "each marker is read as its own worker's value");
	check(tuple_id(&hand, 0, amber, first_integer) <
	    tuple_id(&hand, 0, amber, blue) &&
	    tuple_id(&hand, 1, other_integer, blue) <
	    tuple_id(&hand, 1, first_integer, blue), SCENARIO_ORDER,
	    "p and q take their tuples in task order");
	check(candidates_rehashed(&hand), SCENARIO_ORDER,
	    "every candidate is rewritten and hashed into its stripe");

	hand_close(&hand);
}

/*
 * Pending tables claiming sizes the merge cannot represent, which it must
 * decline on the sizes alone, before it allocates or reads a value, so
 * the workers need not hold what they claim.  In the round partial_round()
 * builds, the worker claims more values than a marker counts, and then
 * SIZE_MAX string bytes; with the true sizes restored the round merges.
 * With two workers, each claims more than half of SIZE_MAX string bytes,
 * a sum that wraps though neither count is SIZE_MAX.
 */
static void
a_merge_declines_unrepresentable_sizes(void)
{
	hand_merge_t hand;
	relic_candidates_t *candidates;
	size_t value_count;
	size_t value_byte_count;
	size_t w;
	bool built;

	if (!open_partial(&hand, SCENARIO_SIZES))
		return;
	candidates = &hand.pool->workers[0].candidates;
	value_count = candidates->value_count;
	value_byte_count = candidates->value_byte_count;

	candidates->value_count = (size_t)RELIC_DOM_INDEX_MASK + 1;
	merge_declines(&hand, SCENARIO_SIZES, "more values than a marker "
	    "counts");
	candidates->value_count = value_count;

	candidates->value_byte_count = SIZE_MAX;
	merge_declines(&hand, SCENARIO_SIZES, "SIZE_MAX string bytes");
	candidates->value_byte_count = value_byte_count;

	check(hand_merge(&hand), SCENARIO_SIZES, "the round merges with its "
	    "true sizes");
	hand_close(&hand);

	/* A build without threads has a pool of one worker. */
	if (relic_pool_size(2) < 2)
		return;
	built = hand_open(&hand, 2);
	if (built)
		built = hand_integer(&hand, 0, NEW_INTEGER) != 0;
	if (!built) {
		check(false, SCENARIO_SIZES, "the two-worker round is built");
		hand_close(&hand);
		return;
	}
	for (w = 0; w < 2; w++)
		hand.pool->workers[w].candidates.value_byte_count =
		    SIZE_MAX / 2 + 1;
	merge_declines(&hand, SCENARIO_SIZES, "string bytes whose sum wraps");
	for (w = 0; w < 2; w++)
		hand.pool->workers[w].candidates.value_byte_count = 0;
	hand_close(&hand);
}

/*
 * The round partial_round() builds with a third task whose candidate
 * breaks its worker's table: a marker past its pending values, and then a
 * string whose bytes lie past the worker's.  No worker makes either.  A
 * build with assertions stops at the first such marker, which is what it
 * is for; an ordinary build declines the merge, having read nothing past
 * the worker's table, changed no candidate and kept no charge.
 */
static void
a_merge_declines_markers_naming_nothing(void)
{
#ifdef NDEBUG
	hand_merge_t hand;
	relic_candidates_t *candidates;
	relic_pending_t past;
	relic_dom_t past_marker;

	if (!open_partial(&hand, SCENARIO_MARKERS))
		return;
	candidates = &hand.pool->workers[0].candidates;
	past_marker = (relic_dom_t)candidates->value_count + 1;
	if (!hand_pair(&hand, 0, 0, past_marker, 3)) {
		check(false, SCENARIO_MARKERS, "the round is built");
		hand_close(&hand);
		return;
	}
	merge_declines(&hand, SCENARIO_MARKERS, "a marker past the worker's "
	    "values");
	hand_close(&hand);

	if (!open_partial(&hand, SCENARIO_MARKERS))
		return;
	candidates = &hand.pool->workers[0].candidates;
	bzero(&past, sizeof(past));
	past.kind = RELIC_PENDING_TEXT;
	past.u.text.offset = candidates->value_byte_count;
	past.u.text.length = 1;
	past_marker = hand_value(&hand, 0, &past);
	if (past_marker == 0 || !hand_pair(&hand, 0, 0, past_marker, 3)) {
		check(false, SCENARIO_MARKERS, "the round is built");
		hand_close(&hand);
		return;
	}
	merge_declines(&hand, SCENARIO_MARKERS, "a string past the worker's "
	    "bytes");
	hand_close(&hand);
#endif
}

/*
 * Merge a round whose worker 0's candidates hold markers and require that
 * the merge declines having changed nothing: those candidates keep their
 * markers, hashes and stripes, none of amber, blue and the new integer is
 * interned, neither target holds a tuple, and every byte the merge charged
 * is given back.  `what` names the case.
 */
static void
merge_declines(hand_merge_t *hand, const char *scenario, const char *what)
{
	candidates_snapshot_t snapshot;
	const relic_candidates_t *candidates;
	size_t before;
	bool merged;

	candidates = &hand->pool->workers[0].candidates;
	snapshot_candidates(candidates, &snapshot);
	before = round_bytes(hand->ctx);
	merged = hand_merge(hand);
	checkf(!merged, scenario, "%s: the merge declines", what);
	checkf(candidates_unchanged(candidates, &snapshot), scenario,
	    "%s: the candidates keep their markers, hashes and stripes", what);
	checkf(find_string(hand->ctx, "amber") == 0 &&
	    find_string(hand->ctx, "blue") == 0 &&
	    relic_value_find_integer(hand->ctx, NEW_INTEGER) == 0, scenario,
	    "%s: no value is interned", what);
	checkf(relic_relation_count(hand->targets[0]) == 0 &&
	    relic_relation_count(hand->targets[1]) == 0, scenario,
	    "%s: neither target holds a tuple", what);
	checkf(round_bytes(hand->ctx) == before, scenario,
	    "%s: every byte the merge charged is given back", what);
}

/*
 * Open a one-worker round and build partial_round()'s tasks in it.
 * Returns whether it could; when it could not, the failure is checked
 * under `scenario` and the round released.
 */
static bool
open_partial(hand_merge_t *hand, const char *scenario)
{
	bool built;

	built = hand_open(hand, 1);
	if (built)
		built = partial_round(hand);
	if (!built) {
		check(false, scenario, "the round is built");
		hand_close(hand);
	}

	return (built);
}

/*
 * Fill worker 0 of a one-worker round with four pending values -- amber,
 * the large string, the new integer and blue, marked in that order -- and
 * two tasks whose candidates hold all of them but the large string:
 *
 *	task 0:	p(blue, new integer)
 *	task 1:	q(amber, blue)
 *
 * Returns whether the round could be built.
 */
static bool
partial_round(hand_merge_t *hand)
{
	relic_dom_t amber;
	relic_dom_t blue;
	relic_dom_t integer;

	amber = hand_string(hand, 0, "amber", 5);
	if (amber == 0)
		return (false);
	if (hand_string(hand, 0, large_string, sizeof(large_string)) == 0)
		return (false);
	integer = hand_integer(hand, 0, NEW_INTEGER);
	if (integer == 0)
		return (false);
	blue = hand_string(hand, 0, "blue", 4);
	if (blue == 0)
		return (false);

	if (!hand_pair(hand, 0, 0, blue, integer))
		return (false);

	return (hand_pair(hand, 0, 1, amber, blue));
}

/*
 * Fill a two-worker round with three tasks, worker 1's, worker 0's and
 * worker 1's again, the first and last for p and the middle one for q:
 *
 *	task 0, worker 1:	p(amber, NEW_INTEGER)
 *	task 1, worker 0:	q(OTHER_INTEGER, blue), q(NEW_INTEGER, blue)
 *	task 2, worker 1:	p(amber, blue)
 *
 * Each worker marks its values as it first derives them: worker 0
 * OTHER_INTEGER, blue and NEW_INTEGER as markers 1 to 3, and worker 1
 * amber, NEW_INTEGER and blue.  So marker 1 names a different value in
 * each worker, and NEW_INTEGER and blue each have a different marker in
 * each.  Returns whether the round could be built.
 */
static bool
order_round(hand_merge_t *hand)
{
	relic_dom_t tuples[HAND_ARITY * 2];
	relic_dom_t w0_other;
	relic_dom_t w0_blue;
	relic_dom_t w0_new;
	relic_dom_t w1_amber;
	relic_dom_t w1_new;
	relic_dom_t w1_blue;

	w0_other = hand_integer(hand, 0, OTHER_INTEGER);
	w0_blue = hand_string(hand, 0, "blue", 4);
	w0_new = hand_integer(hand, 0, NEW_INTEGER);
	w1_amber = hand_string(hand, 1, "amber", 5);
	w1_new = hand_integer(hand, 1, NEW_INTEGER);
	w1_blue = hand_string(hand, 1, "blue", 4);
	if (w0_other == 0 || w0_blue == 0 || w0_new == 0 || w1_amber == 0 ||
	    w1_new == 0 || w1_blue == 0)
		return (false);

	if (!hand_pair(hand, 1, 0, w1_amber, w1_new))
		return (false);
	tuples[0] = w0_other;
	tuples[1] = w0_blue;
	tuples[2] = w0_new;
	tuples[3] = w0_blue;
	if (!hand_task(hand, 0, 1, tuples, 2))
		return (false);

	return (hand_pair(hand, 1, 0, w1_amber, w1_blue));
}

/*
 * Build an empty round over a fresh context and a pool of `workers`
 * workers, each with room for HAND_TUPLES candidates and HAND_VALUES
 * pending values.  Returns whether it could; hand_close() releases what
 * was built either way.
 */
static bool
hand_open(hand_merge_t *hand, size_t workers)
{
	static const char *const names[HAND_TARGETS] = { "p", "q" };
	relic_candidates_t *candidates;
	relic_pred_id_t pred;
	size_t i;

	bzero(hand, sizeof(*hand));
	hand->round.items = hand->items;
	hand->round.targets = hand->destination_pointers;
	hand->round.item_capacity = HAND_TASKS;
	hand->round.target_count = HAND_TARGETS;

	hand->ctx = relic_create();
	if (hand->ctx == NULL)
		return (false);
	if (relic_load_string(hand->ctx, "hand.dl", "seed(1, \"one\").") != 0)
		return (false);
	for (i = 0; i < HAND_TARGETS; i++) {
		if (relic_pred_intern(hand->ctx, names[i], strlen(names[i]),
		    HAND_ARITY, NULL, &pred) != 0)
			return (false);
		hand->targets[i] = relic_relation_read(hand->ctx, pred);
		if (hand->targets[i] == NULL)
			return (false);
		relic_destination_set(&hand->destinations[i], hand->targets[i]);
		hand->destination_pointers[i] = &hand->destinations[i];
	}

	if (relic_pool_create(&hand->pool, workers,
	    hand->ctx->hash_seed) != 0)
		return (false);
	hand->pool->tasks = calloc(HAND_TASKS, sizeof(*hand->pool->tasks));
	if (hand->pool->tasks == NULL)
		return (false);
	hand->pool->task_capacity = HAND_TASKS;
	for (i = 0; i < hand->pool->worker_count; i++) {
		candidates = &hand->pool->workers[i].candidates;
		if (relic_candidates_reserve(candidates, &hand->ctx->account,
		    HAND_TUPLES, HAND_TUPLES * HAND_ARITY) != 0)
			return (false);
		candidates->values = calloc(HAND_VALUES,
		    sizeof(*candidates->values));
		if (candidates->values == NULL)
			return (false);
		candidates->value_capacity = HAND_VALUES;
	}

	return (hand->pool->worker_count == workers);
}

/*
 * Release the round, its pool and its context, as much of them as
 * hand_open() built.
 */
static void
hand_close(hand_merge_t *hand)
{
	size_t i;

	if (hand->pool != NULL) {
		for (i = 0; i < hand->pool->worker_count; i++)
			relic_candidates_release(
			    &hand->pool->workers[i].candidates,
			    &hand->ctx->account);
		relic_pool_free(hand->pool);
	}
	relic_destroy(hand->ctx);
}

/*
 * Give worker `worker` the pending integer `integer`, and return its
 * marker, or zero when the worker has no room for it.
 */
static relic_dom_t
hand_integer(hand_merge_t *hand, size_t worker, int64_t integer)
{
	relic_pending_t value;

	bzero(&value, sizeof(value));
	value.kind = RELIC_PENDING_INTEGER;
	value.u.integer = integer;

	return (hand_value(hand, worker, &value));
}

/*
 * Give worker `worker` the pending string of `length` bytes at `bytes`,
 * copied after the worker's other strings' bytes, and return its marker,
 * or zero when the worker has no room for it.
 */
static relic_dom_t
hand_string(hand_merge_t *hand, size_t worker, const char *bytes,
    size_t length)
{
	relic_candidates_t *candidates;
	relic_pending_t value;
	char *grown;

	candidates = &hand->pool->workers[worker].candidates;
	if (length > 0) {
		grown = realloc(candidates->value_bytes,
		    candidates->value_byte_count + length);
		if (grown == NULL)
			return (0);
		candidates->value_bytes = grown;
		candidates->value_byte_capacity =
		    candidates->value_byte_count + length;
		bcopy(bytes, candidates->value_bytes +
		    candidates->value_byte_count, length);
	}

	bzero(&value, sizeof(value));
	value.kind = RELIC_PENDING_TEXT;
	value.u.text.offset = candidates->value_byte_count;
	value.u.text.length = length;
	candidates->value_byte_count += length;

	return (hand_value(hand, worker, &value));
}

/*
 * Append `value` to worker `worker`'s pending values, as a task marks a
 * value it met first, and return its marker: its position plus one.
 * Returns zero when the worker has no room for it.
 */
static relic_dom_t
hand_value(hand_merge_t *hand, size_t worker, const relic_pending_t *value)
{
	relic_candidates_t *candidates;

	candidates = &hand->pool->workers[worker].candidates;
	if (candidates->value_count == candidates->value_capacity)
		return (0);
	candidates->values[candidates->value_count] = *value;
	candidates->value_count++;

	return ((relic_dom_t)candidates->value_count);
}

/*
 * Append a task to the round: worker `worker` gathered `count` candidates
 * for target `target`, their columns HAND_ARITY at a time at `tuples`, each
 * an identifier or a marker among the worker's pending values, after its
 * earlier tasks' candidates.  Each is hashed over its columns as they
 * stand and counted in its stripe, as a worker gathers it.  Returns
 * whether the round had room for the task and the worker for the
 * candidates.
 */
static bool
hand_task(hand_merge_t *hand, size_t worker, size_t target,
    const relic_dom_t *tuples, size_t count)
{
	relic_candidates_t *candidates;
	relic_candidate_t *candidate;
	relic_round_task_t *task;
	size_t stripe;
	size_t i;

	candidates = &hand->pool->workers[worker].candidates;
	if (hand->pool->task_count == HAND_TASKS ||
	    count > HAND_TUPLES - candidates->count)
		return (false);

	task = &hand->pool->tasks[hand->pool->task_count];
	bzero(task, sizeof(*task));
	task->item = hand->pool->task_count;
	task->worker = worker;
	task->first_candidate = candidates->count;
	task->candidate_count = count;
	hand->items[task->item].target = target;
	hand->pool->task_count++;
	hand->round.item_count++;

	for (i = 0; i < count; i++) {
		candidate = &candidates->entries[candidates->count];
		candidate->offset = candidates->word_count;
		bcopy(&tuples[i * HAND_ARITY],
		    &candidates->words[candidates->word_count],
		    HAND_ARITY * sizeof(*tuples));
		candidates->word_count += HAND_ARITY;
		candidate->hash = relic_relation_hash(hand->targets[target],
		    &candidates->words[candidate->offset]);
		candidate->target = (uint32_t)target;
		candidate->contribution = 1;
		stripe = relic_hash_stripe(candidate->hash);
		candidates->stripes[candidates->count] = (uint8_t)stripe;
		candidates->by_stripe[stripe]++;
		candidates->count++;
	}

	return (true);
}

/*
 * Append a task of one candidate, (first, second), to the round, as
 * hand_task() does.
 */
static bool
hand_pair(hand_merge_t *hand, size_t worker, size_t target,
    relic_dom_t first, relic_dom_t second)
{
	relic_dom_t tuple[HAND_ARITY];

	tuple[0] = first;
	tuple[1] = second;

	return (hand_task(hand, worker, target, tuple, 1));
}

/*
 * Merge the round, as relic_round_run() does once its tasks have run, and
 * return whether the merge succeeded.
 */
static bool
hand_merge(hand_merge_t *hand)
{
	return (relic_round_merge(hand->ctx, hand->pool, &hand->round, 0, NULL));
}

/*
 * Set the context's memory limit `headroom` bytes above everything it
 * holds now: the total relic_account_exceeds() compares with the limit,
 * a round's buffers included.
 */
static void
limit_to(relic_t *ctx, size_t headroom)
{
	const relic_account_t *account;

	account = &ctx->account;
	ctx->account.limit = account->store_bytes + account->value_bytes +
	    account->pool_bytes + account->predtab_bytes +
	    account->arena_bytes + round_bytes(ctx) + headroom;
}

/* The bytes the context's round buffers are charged. */
static size_t
round_bytes(const relic_t *ctx)
{
	return (relic_atomic_size_load(&ctx->account.round_bytes));
}

/* Whether target `target` holds the tuple (first, second). */
static bool
holds(const hand_merge_t *hand, size_t target, relic_dom_t first,
    relic_dom_t second)
{
	return (tuple_id(hand, target, first, second) != RELIC_ID_NONE);
}

/*
 * The identifier of the tuple (first, second) in target `target`, or
 * RELIC_ID_NONE when it holds none.
 */
static relic_tuple_id_t
tuple_id(const hand_merge_t *hand, size_t target, relic_dom_t first,
    relic_dom_t second)
{
	relic_dom_t columns[HAND_ARITY];

	columns[0] = first;
	columns[1] = second;

	return (relic_relation_find(hand->targets[target], columns));
}

/*
 * Whether every candidate of every worker holds identifiers alone, under
 * its tuple's hash and in that hash's stripe, and each worker's stripe
 * counts count its candidates.
 */
static bool
candidates_rehashed(const hand_merge_t *hand)
{
	size_t by_stripe[RELIC_STRIPES];
	const relic_candidates_t *candidates;
	const relic_candidate_t *candidate;
	const relic_dom_t *tuple;
	size_t column;
	size_t w;
	size_t i;

	for (w = 0; w < hand->pool->worker_count; w++) {
		candidates = &hand->pool->workers[w].candidates;
		bzero(by_stripe, sizeof(by_stripe));
		for (i = 0; i < candidates->count; i++) {
			candidate = &candidates->entries[i];
			tuple = &candidates->words[candidate->offset];
			for (column = 0; column < HAND_ARITY; column++) {
				if ((tuple[column] & RELIC_DOM_TYPE_MASK) == 0)
					return (false);
			}
			if (candidate->hash != relic_relation_hash(
			    hand->targets[candidate->target], tuple) ||
			    candidates->stripes[i] !=
			    relic_hash_stripe(candidate->hash))
				return (false);
			by_stripe[candidates->stripes[i]]++;
		}
		if (memcmp(by_stripe, candidates->by_stripe,
		    sizeof(by_stripe)) != 0)
			return (false);
	}

	return (true);
}

/* Record what a worker's candidates hold, for candidates_unchanged(). */
static void
snapshot_candidates(const relic_candidates_t *candidates,
    candidates_snapshot_t *snapshot)
{
	size_t i;

	bzero(snapshot, sizeof(*snapshot));
	snapshot->count = candidates->count;
	bcopy(candidates->words, snapshot->words,
	    candidates->word_count * sizeof(*candidates->words));
	for (i = 0; i < candidates->count; i++) {
		snapshot->hashes[i] = candidates->entries[i].hash;
		snapshot->stripes[i] = candidates->stripes[i];
	}
	bcopy(candidates->by_stripe, snapshot->by_stripe,
	    sizeof(snapshot->by_stripe));
}

/*
 * Whether a worker's candidates hold what snapshot_candidates() recorded:
 * the same columns, hashes, stripes and stripe counts.
 */
static bool
candidates_unchanged(const relic_candidates_t *candidates,
    const candidates_snapshot_t *snapshot)
{
	size_t i;

	if (candidates->count != snapshot->count ||
	    memcmp(candidates->words, snapshot->words,
	    candidates->word_count * sizeof(*candidates->words)) != 0 ||
	    memcmp(candidates->by_stripe, snapshot->by_stripe,
	    sizeof(snapshot->by_stripe)) != 0)
		return (false);
	for (i = 0; i < candidates->count; i++) {
		if (candidates->entries[i].hash != snapshot->hashes[i] ||
		    candidates->stripes[i] != snapshot->stripes[i])
			return (false);
	}

	return (true);
}

/*
 * The identifier the context assigned the string `bytes`, or zero when it
 * assigned none.
 */
static relic_dom_t
find_string(const relic_t *ctx, const char *bytes)
{
	return (relic_value_find_string(ctx, bytes, strlen(bytes)));
}
