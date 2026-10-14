/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Ordinary aggregate accumulators, compared with independent exact sums
 * and bytewise extrema. Every group is folded in several row orders;
 * invalidity, overflow, empty groups and equal extrema keep their meaning.
 */

#include "config.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "fold.h"
#include "harness.h"

#define	SCENARIO_GROUP	"a fold agrees with its independent outcome"
#define	SCENARIO_STEP	"a fold reports its state changes once"
#define	MAX_ROWS	12
#define	TRIALS		20000

typedef struct group group_t;

/* The accumulator and the borrowed extreme its caller retains. */
struct group {
	relic_fold_state_t	state;
	relic_value_view_t	extreme;
	uint64_t		rows;
	unsigned int		invalidations;
};

static void	 group_init(group_t *group);
static void	 group_add(enum relic_aggregate_fn fn, group_t *group,
		    const relic_value_view_t *value);
static uint64_t	 next_random(uint64_t *state);
static void	 shuffle(uint64_t *seed, size_t *order, size_t count);
static relic_value_view_t integer_view(int64_t integer);
static relic_value_view_t text_view(enum relic_type type, const char *bytes);
static int	 model_order(const relic_value_view_t *left,
		    const relic_value_view_t *right);
static void	 check_rows(enum relic_aggregate_fn fn,
		    const relic_value_view_t *rows, size_t count,
		    enum relic_fold_outcome expected, int64_t number,
		    const relic_value_view_t *extreme);
static void	 empty_and_mixed_groups_keep_their_outcomes(void);
#ifdef __SIZEOF_INT128__
static void	 random_sums_agree_with_the_model(void);
#endif
static void	 random_extremes_agree_with_the_model(void);
static void	 a_step_reports_only_a_change(void);

static void
group_init(group_t *group)
{
	bzero(group, sizeof(*group));
	relic_fold_init(&group->state);
}

static void
group_add(enum relic_aggregate_fn fn, group_t *group,
    const relic_value_view_t *value)
{
	switch (relic_fold_add(fn, &group->state, group->rows, &group->extreme,
	    value)) {
	case RELIC_FOLD_REPLACED:
		group->extreme = *value;
		break;
	case RELIC_FOLD_INVALIDATED:
		group->invalidations++;
		break;
	case RELIC_FOLD_KEPT:
		break;
	}
	group->rows++;
}

static uint64_t
next_random(uint64_t *state)
{
	*state ^= *state >> 12;
	*state ^= *state << 25;
	*state ^= *state >> 27;

	return (*state * UINT64_C(2685821657736338717));
}

static void
shuffle(uint64_t *seed, size_t *order, size_t count)
{
	size_t swap;
	size_t i;
	size_t j;

	for (i = 0; i < count; i++)
		order[i] = i;
	for (i = count; i > 1; i--) {
		j = next_random(seed) % i;
		swap = order[i - 1];
		order[i - 1] = order[j];
		order[j] = swap;
	}
}

static relic_value_view_t
integer_view(int64_t integer)
{
	relic_value_view_t view;

	bzero(&view, sizeof(view));
	view.type = RELIC_INTEGER;
	view.integer = integer;
	view.bytes = "";

	return (view);
}

static relic_value_view_t
text_view(enum relic_type type, const char *bytes)
{
	relic_value_view_t view;

	bzero(&view, sizeof(view));
	view.type = type;
	view.bytes = bytes;
	view.length = strlen(bytes);

	return (view);
}

static int
model_order(const relic_value_view_t *left, const relic_value_view_t *right)
{
	if (left->type != RELIC_INTEGER)
		return (strcmp(left->bytes, right->bytes));

	if (left->integer < right->integer)
		return (-1);

	return (left->integer > right->integer ? 1 : 0);
}

/*
 * Expected values come from the test case or its independent model.
 * Four orders include the source order and three reproducible shuffles.
 */
static void
check_rows(enum relic_aggregate_fn fn, const relic_value_view_t *rows,
    size_t count, enum relic_fold_outcome expected, int64_t number,
    const relic_value_view_t *extreme)
{
	size_t order[MAX_ROWS];
	group_t group;
	uint64_t seed;
	int64_t integer;
	enum relic_fold_outcome outcome;
	size_t pass;
	size_t i;
	bool correct;

	seed = UINT64_C(0x9e3779b97f4a7c15);
	for (i = 0; i < count; i++)
		order[i] = i;
	for (pass = 0; pass < 4; pass++) {
		group_init(&group);
		for (i = 0; i < count; i++)
			group_add(fn, &group, &rows[order[i]]);
		integer = 0;
		outcome = relic_fold_outcome(fn, &group.state, group.rows,
		    &integer);
		correct = outcome == expected && group.rows == count &&
		    group.invalidations ==
		    (expected == RELIC_FOLD_MIXED ? 1u : 0u);
		if (expected == RELIC_FOLD_VALUE) {
			if (fn == RELIC_AGG_COUNT || fn == RELIC_AGG_SUM)
				correct = correct && integer == number;
			else
				correct = correct &&
				    group.extreme.type == extreme->type &&
				    model_order(&group.extreme, extreme) == 0;
		}
		checkf(correct, SCENARIO_GROUP,
		    "function %d, %zu rows, order %zu: expected outcome %d, "
		    "got %d, integer %" PRId64, (int)fn, count, pass,
		    (int)expected, (int)outcome, integer);
		shuffle(&seed, order, count);
	}
}

static void
empty_and_mixed_groups_keep_their_outcomes(void)
{
	relic_value_view_t rows[3];
	size_t at;
	size_t i;

	check_rows(RELIC_AGG_COUNT, NULL, 0, RELIC_FOLD_VALUE, 0, NULL);
	check_rows(RELIC_AGG_SUM, NULL, 0, RELIC_FOLD_VALUE, 0, NULL);
	check_rows(RELIC_AGG_MIN, NULL, 0, RELIC_FOLD_EMPTY, 0, NULL);
	check_rows(RELIC_AGG_MAX, NULL, 0, RELIC_FOLD_EMPTY, 0, NULL);

	rows[0] = integer_view(INT64_MAX);
	rows[1] = integer_view(1);
	rows[2] = integer_view(-1);
	check_rows(RELIC_AGG_SUM, rows, 3, RELIC_FOLD_VALUE, INT64_MAX, NULL);
	rows[1] = integer_view(INT64_MAX);
	check_rows(RELIC_AGG_SUM, rows, 2, RELIC_FOLD_OVERFLOW, 0, NULL);

	for (at = 0; at < 3; at++) {
		for (i = 0; i < 3; i++)
			rows[i] = i == at ? text_view(RELIC_SYMBOL, "x") :
			    integer_view(INT64_MAX);
		check_rows(RELIC_AGG_SUM, rows, 3, RELIC_FOLD_MIXED, 0, NULL);
		check_rows(RELIC_AGG_COUNT, rows, 3, RELIC_FOLD_VALUE, 3, NULL);
		for (i = 0; i < 3; i++)
			rows[i] = text_view(i == at ? RELIC_STRING :
			    RELIC_SYMBOL, "x");
		check_rows(RELIC_AGG_MIN, rows, 3, RELIC_FOLD_MIXED, 0, NULL);
		check_rows(RELIC_AGG_MAX, rows, 3, RELIC_FOLD_MIXED, 0, NULL);
	}
}

#ifdef __SIZEOF_INT128__
/* The compiler's wide integer is independent of relic's two-word sum. */
static void
random_sums_agree_with_the_model(void)
{
	static const int64_t edges[] = {
		INT64_MAX, INT64_MIN, INT64_MAX - 1, INT64_MIN + 1, 1, -1, 0
	};
	relic_value_view_t rows[MAX_ROWS];
	__int128 total;
	uint64_t seed;
	uint64_t draw;
	enum relic_fold_outcome expected;
	size_t trial;
	size_t count;
	size_t i;
	bool invalid;

	seed = UINT64_C(0x9e3779b97f4a7c15);
	for (trial = 0; trial < TRIALS; trial++) {
		count = next_random(&seed) % (MAX_ROWS + 1);
		total = 0;
		invalid = false;
		for (i = 0; i < count; i++) {
			draw = next_random(&seed);
			if (draw % 41 == 0) {
				rows[i] = text_view(RELIC_STRING, "s");
				invalid = true;
			} else if (draw % 3 == 0) {
				rows[i] = integer_view((int64_t)next_random(&seed));
			} else {
				rows[i] = integer_view(edges[(draw >> 8) %
				    (sizeof(edges) / sizeof(edges[0]))]);
			}
			if (rows[i].type == RELIC_INTEGER)
				total += rows[i].integer;
		}
		if (invalid)
			expected = RELIC_FOLD_MIXED;
		else if (total < INT64_MIN || total > INT64_MAX)
			expected = RELIC_FOLD_OVERFLOW;
		else
			expected = RELIC_FOLD_VALUE;
		check_rows(RELIC_AGG_SUM, rows, count, expected,
		    expected == RELIC_FOLD_VALUE ? (int64_t)total : 0, NULL);
	}
}
#endif

/* The model orders integers and unsigned text bytes without value helpers. */
static void
random_extremes_agree_with_the_model(void)
{
	static const char *const spellings[] = {
		"", "a", "ab", "b", "ba", "z", "\x80", "\xff"
	};
	static const enum relic_aggregate_fn fns[] = {
		RELIC_AGG_MIN, RELIC_AGG_MAX
	};
	relic_value_view_t rows[MAX_ROWS];
	relic_value_view_t best;
	uint64_t seed;
	size_t trial;
	size_t count;
	size_t i;
	size_t f;
	int order;
	bool text;

	seed = UINT64_C(0x2545f4914f6cdd1d);
	for (trial = 0; trial < TRIALS; trial++) {
		count = next_random(&seed) % (MAX_ROWS + 1);
		text = next_random(&seed) % 2 == 0;
		for (i = 0; i < count; i++) {
			if (text)
				rows[i] = text_view(RELIC_SYMBOL,
				    spellings[next_random(&seed) %
				    (sizeof(spellings) / sizeof(spellings[0]))]);
			else
				rows[i] = integer_view((int64_t)
				    (next_random(&seed) % 9) - 4);
		}
		for (f = 0; f < 2; f++) {
			bzero(&best, sizeof(best));
			for (i = 0; i < count; i++) {
				order = i == 0 ? 0 : model_order(&rows[i], &best);
				if (i == 0 ||
				    (fns[f] == RELIC_AGG_MIN && order < 0) ||
				    (fns[f] == RELIC_AGG_MAX && order > 0))
					best = rows[i];
			}
			check_rows(fns[f], rows, count, count == 0 ?
			    RELIC_FOLD_EMPTY : RELIC_FOLD_VALUE, 0, &best);
		}
	}
}

static void
a_step_reports_only_a_change(void)
{
	relic_value_view_t value;
	group_t group;

	group_init(&group);
	check(relic_fold_add(RELIC_AGG_COUNT, &group.state, 0, NULL, NULL) ==
	    RELIC_FOLD_KEPT, SCENARIO_STEP, "a count should read no value");
	value = integer_view(3);
	group_add(RELIC_AGG_MIN, &group, &value);
	check(relic_fold_add(RELIC_AGG_MIN, &group.state, group.rows,
	    &group.extreme, &value) == RELIC_FOLD_KEPT, SCENARIO_STEP,
	    "an equal extreme should keep the existing value");
	value = text_view(RELIC_STRING, "x");
	group_add(RELIC_AGG_MIN, &group, &value);
	group_add(RELIC_AGG_MIN, &group, &value);
	check(group.invalidations == 1, SCENARIO_STEP,
	    "a group should report invalidity once");
}

int
main(void)
{
	empty_and_mixed_groups_keep_their_outcomes();
#ifdef __SIZEOF_INT128__
	random_sums_agree_with_the_model();
#endif
	random_extremes_agree_with_the_model();
	a_step_reports_only_a_change();

	return (check_summary("fold"));
}
