/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "random.h"

/*
 * The shrinker: reduce a failing program to a small one that fails the same
 * way.
 *
 * It is greedy.  A pass tries every deletion the program allows -- each
 * query, each rule or fact, each body literal, each argument position of
 * each predicate -- and keeps a deletion whenever the smaller program still
 * fails with the same kind of failure under the same strategy, and for a
 * crash or an abnormal exit with the same signal or status.  Passes repeat
 * until one keeps nothing, so the result is a program from which no single
 * deletion leaves the failure in place.
 *
 * Holding the failure fixed is what keeps a reduction on one bug.  A
 * deletion that turned a wrong answer into a crash, or made the program one
 * the library rightly refuses, is a different failure and is not kept.
 * Deletions add no dependency, so a stratified program stays stratified,
 * and add no operator, so a program whose arithmetic is bounded stays
 * bounded; a deletion that leaves a variable unbound is recognized by the
 * model's safety check and never run, and the bound is checked as well.
 *
 * Every candidate is run in a child, exactly as the driver runs a program,
 * so a crash or a hang shrinks as readily as a wrong answer.
 */

typedef struct shrink_state shrink_state_t;

struct shrink_state {
	random_program_t	*program;	/* the smallest failing so far */
	random_program_t	*candidate;	/* scratch for the next deletion */
	random_failure_t	 failure;	/* the failure to preserve */
	const random_options_t	*options;
};

static bool	 keep_if_still_failing(shrink_state_t *state);
static bool	 shrink_columns(shrink_state_t *state);
static bool	 shrink_literals(shrink_state_t *state);
static bool	 shrink_queries(shrink_state_t *state);
static bool	 shrink_rules(shrink_state_t *state);

void
random_shrink(random_program_t *program, random_failure_t failure,
    const random_options_t *options)
{
	shrink_state_t state;
	bool changed;

	state.program = program;
	state.candidate = malloc(sizeof(*state.candidate));
	if (state.candidate == NULL)
		err(1, "malloc");
	state.failure = failure;
	state.options = options;

	/*
	 * Larger deletions first: a query or a rule removes more than a
	 * literal, and each one kept makes every later pass shorter.
	 */
	do {
		changed = false;
		if (shrink_queries(&state))
			changed = true;
		if (shrink_rules(&state))
			changed = true;
		if (shrink_literals(&state))
			changed = true;
		if (shrink_columns(&state))
			changed = true;
	} while (changed);

	free(state.candidate);
}

/*
 * Each pass walks its items from the last to the first, so that a deletion
 * kept does not move the items still to be tried.
 */

static bool
shrink_queries(shrink_state_t *state)
{
	bool changed = false;

	for (size_t q = state->program->query_count; q-- > 0;) {
		if (state->program->query_count == 1)
			break;
		*state->candidate = *state->program;
		random_remove_query(state->candidate, q);
		if (keep_if_still_failing(state))
			changed = true;
	}
	return (changed);
}

static bool
shrink_rules(shrink_state_t *state)
{
	bool changed = false;

	for (size_t r = state->program->rule_count; r-- > 0;) {
		*state->candidate = *state->program;
		random_remove_rule(state->candidate, r);
		if (keep_if_still_failing(state))
			changed = true;
	}
	return (changed);
}

static bool
shrink_literals(shrink_state_t *state)
{
	bool changed = false;

	for (size_t r = state->program->rule_count; r-- > 0;) {
		size_t length = state->program->rules[r].body.length;

		for (size_t i = length; i-- > 0;) {
			*state->candidate = *state->program;
			random_remove_literal(&state->candidate->rules[r].body,
			    i);
			if (keep_if_still_failing(state))
				changed = true;
		}
	}

	for (size_t q = state->program->query_count; q-- > 0;) {
		size_t length = state->program->queries[q].length;

		for (size_t i = length; i-- > 0;) {
			*state->candidate = *state->program;
			random_remove_literal(&state->candidate->queries[q], i);
			if (keep_if_still_failing(state))
				changed = true;
		}
	}
	return (changed);
}

static bool
shrink_columns(shrink_state_t *state)
{
	bool changed = false;

	for (size_t p = 0; p < state->program->predicate_count; p++) {
		size_t arity = state->program->predicates[p].arity;

		for (size_t c = arity; c-- > 0;) {
			*state->candidate = *state->program;
			random_remove_column(state->candidate, p, c);
			if (keep_if_still_failing(state))
				changed = true;
		}
	}
	return (changed);
}

/*
 * Run the candidate, and make it the program if it is safe and fails as the
 * program does.  Returns whether it was kept.
 */
static bool
keep_if_still_failing(shrink_state_t *state)
{
	random_failure_t failure;

	if (!random_program_is_safe(state->candidate) ||
	    !random_program_is_bounded(state->candidate))
		return (false);

	failure = random_run_isolated(state->candidate, state->options, false);
	if (!random_failure_equal(failure, state->failure))
		return (false);

	*state->program = *state->candidate;
	return (true);
}
