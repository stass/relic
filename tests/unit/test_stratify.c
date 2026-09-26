/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the dependency analysis: the components, their strata,
 * the order the evaluator is handed, and the rejection of recursion through
 * negation.
 *
 * The golden language tests record what the unstratified diagnostic says.
 * What they cannot see is the schedule, so that is what this suite examines.
 * A schedule is rendered as text, one component per word in evaluation
 * order: its stratum, a colon, its members joined by commas, and a `*` when
 * it is recursive.  "0:edge/2 0:path/2*" is edge/2 and then a recursive
 * path/2, both in stratum 0.
 *
 * Two independent oracles back the hand-written renderings.  One checks the
 * property the order exists for directly from the rules: every body literal
 * reads a component no later than the head's, and a negated one reads a
 * strictly lower stratum.  The other renders the components as a sorted set,
 * which is what two textual orders of one program must agree on.  Neither
 * repeats Tarjan's algorithm.
 */

#include "config.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_CHAIN		"a long dependency chain does not overflow"
#define	SCENARIO_ORDER		"components come out dependencies first"
#define	SCENARIO_REJECTED	"recursion through negation is rejected"
#define	SCENARIO_RULES		"rules are grouped by component in order"
#define	SCENARIO_STRATA		"negation reads a strictly lower stratum"
#define	SCENARIO_TEXT		"the order depends only on the program"
#define	SCENARIO_UNUSED		"components without rules are left out"

/*
 * The long chain and the long cycle, and the stack they are analysed on.
 *
 * A recursive search over either would descend once per predicate, given the
 * order long_program() writes them in.  Even the smallest frame a call can
 * have, a return address and a saved frame pointer, is sixteen bytes, so this
 * many nested calls need more than CHAIN_STACK holds.  The analysis runs on a
 * thread given exactly that stack rather than on the main thread, whose stack
 * size varies by platform and is often large enough to hide a recursive
 * search.  The flat search needs only a few frames of it.
 */
#define	CHAIN_LENGTH	100000
#define	CHAIN_STACK	(1024 * 1024)

/* Room for any rendering below. */
#define	RENDERED_MAX	1024

typedef struct answer answer_t;
typedef struct chain_run chain_run_t;
typedef struct diag_log diag_log_t;

/* The rows a one-column query delivered: how many, and the last value. */
struct answer {
	relic_value_t	value;
	size_t		rows;
};

/*
 * A loaded long program handed to the thread that checks and queries it, and
 * what that thread found.
 */
struct chain_run {
	relic_t	*ctx;
	bool	 checked;
	bool	 answered;
};

struct diag_log {
	size_t	errors;
	size_t	warnings;
	size_t	notes;
};

static bool	 answer_is(relic_t *ctx, const char *query_text,
		    const char *expected);
static void	*check_and_query(void *argument);
static bool	 run_on_small_stack(chain_run_t *run);
static int	 compare_words(const void *left, const void *right);
static relic_t	*context_logging_to(diag_log_t *log);
static bool	 load(relic_t *ctx, const char *text);
static bool	 load_and_check(relic_t *ctx, const char *text);
static char	*long_program(bool closed);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static int	 remember_answer(void *callback_ctx,
		    const relic_value_t *values, size_t count);
static void	 render_component(const relic_t *ctx,
		    const relic_component_t *component, bool sorted,
		    char *buffer, size_t size);
static void	 render_schedule(const relic_t *ctx, bool as_set,
		    char *buffer, size_t size);
static bool	 schedule_is(const char *scenario, const char *program,
		    const char *expected);
static bool	 schedule_respects_dependencies(const relic_t *ctx);
static void	 a_long_chain_does_not_overflow(void);
static void	 components_come_out_dependencies_first(void);
static void	 components_without_rules_are_left_out(void);
static void	 negation_reads_a_strictly_lower_stratum(void);
static void	 recursion_through_negation_is_rejected(void);
static void	 rules_are_grouped_by_component_in_order(void);
static void	 the_order_depends_only_on_the_program(void);

static void
record_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_UNUSED,
    const char *message RELIC_UNUSED)
{
	diag_log_t *log;

	log = callback_ctx;

	switch (level) {
	case RELIC_ERROR:
		log->errors++;
		break;
	case RELIC_WARNING:
		log->warnings++;
		break;
	case RELIC_NOTE:
		log->notes++;
		break;
	}
}

static relic_t *
context_logging_to(diag_log_t *log)
{
	relic_t *ctx;

	bzero(log, sizeof(*log));

	ctx = relic_create();
	if (ctx == NULL) {
		check(false, "creating a context", "a context is created");
		return (NULL);
	}

	relic_diag_set(ctx, record_diagnostic, log);

	return (ctx);
}

static bool
load(relic_t *ctx, const char *text)
{
	return (relic_load_string(ctx, "test.dl", text) == 0);
}

/*
 * Whether `text` loads and the program then passes a check.  A load that
 * fails is not followed by a check.
 */
static bool
load_and_check(relic_t *ctx, const char *text)
{
	if (!load(ctx, text))
		return (false);

	return (relic_check(ctx) == 0);
}

static int
compare_words(const void *left, const void *right)
{
	return (strcmp(*(char *const *)left, *(char *const *)right));
}

/*
 * Render one component: "stratum:member,member" with a trailing `*` when it
 * is recursive.  With `sorted`, the members are listed alphabetically rather
 * than in identifier order, which is first-mention order and so differs
 * between two textual orders of one program.
 */
static void
render_component(const relic_t *ctx, const relic_component_t *component,
    bool sorted, char *buffer, size_t size)
{
	const relic_schedule_t *schedule;
	char names[16][RELIC_PRED_TEXT_MAX];
	char *ordered[16];
	size_t count;
	size_t i;

	schedule = &ctx->program.schedule;
	count = component->member_count;
	if (count > 16)
		count = 16;

	for (i = 0; i < count; i++) {
		relic_pred_describe(ctx,
		    schedule->members[component->first_member + i], names[i],
		    sizeof(names[i]));
		ordered[i] = names[i];
	}

	if (sorted)
		qsort(ordered, count, sizeof(ordered[0]), compare_words);

	snprintf(buffer, size, "%u:", (unsigned int)component->stratum);
	for (i = 0; i < count; i++) {
		if (i > 0)
			strlcat(buffer, ",", size);
		strlcat(buffer, ordered[i], size);
	}

	if (component->recursive)
		strlcat(buffer, "*", size);
}

/*
 * Render the schedule, components separated by spaces.  In evaluation order,
 * or, with `as_set`, sorted, so that only which components exist and what
 * they hold is compared.
 */
static void
render_schedule(const relic_t *ctx, bool as_set, char *buffer, size_t size)
{
	const relic_schedule_t *schedule;
	char words[64][128];
	char *ordered[64];
	size_t count;
	size_t i;

	schedule = &ctx->program.schedule;
	count = schedule->component_count;
	if (count > 64)
		count = 64;

	for (i = 0; i < count; i++) {
		render_component(ctx, &schedule->components[i], as_set,
		    words[i], sizeof(words[i]));
		ordered[i] = words[i];
	}

	if (as_set)
		qsort(ordered, count, sizeof(ordered[0]), compare_words);

	buffer[0] = '\0';
	for (i = 0; i < count; i++) {
		if (i > 0)
			strlcat(buffer, " ", size);
		strlcat(buffer, ordered[i], size);
	}
}

/*
 * The independent oracle for the order: for every rule, every relational body
 * literal reads a predicate whose component is scheduled no later than the
 * head's, and whose stratum is no higher, or strictly lower when the literal
 * is negated.  A predicate no rule defines is in no scheduled component and
 * reads as complete from the start.  It also confirms that the schedule lists
 * every rule exactly once.
 */
static bool
schedule_respects_dependencies(const relic_t *ctx)
{
	const relic_schedule_t *schedule;
	const relic_component_t *component;
	const relic_rule_t *rule;
	const relic_literal_t *literal;
	size_t *position;
	size_t *listed;
	size_t pred_count;
	size_t head_position;
	size_t body_position;
	size_t r;
	size_t l;
	size_t k;
	size_t m;
	bool holds;

	schedule = &ctx->program.schedule;
	pred_count = relic_pred_count(ctx);

	position = calloc(pred_count + 1, sizeof(*position));
	listed = calloc(ctx->program.rules.count + 1, sizeof(*listed));
	if (position == NULL || listed == NULL) {
		free(position);
		free(listed);
		return (false);
	}

	/* Position 0 is "no scheduled component"; component k is k + 1. */
	for (k = 0; k < schedule->component_count; k++) {
		component = &schedule->components[k];
		for (m = 0; m < component->member_count; m++)
			position[schedule->members[component->first_member +
			    m]] = k + 1;
		for (m = 0; m < component->rule_count; m++)
			listed[schedule->rule_indexes[component->first_rule +
			    m]]++;
	}

	holds = schedule->rule_count == ctx->program.rules.count;

	for (r = 0; r < ctx->program.rules.count; r++) {
		rule = &ctx->program.rules.entries[r];
		head_position = position[rule->head];

		if (listed[r] != 1 || head_position == 0)
			holds = false;
		if (head_position == 0)
			continue;

		for (l = 0; l < rule->body_length; l++) {
			literal = &rule->body[l];
			if (literal->kind == RELIC_LITERAL_COMPARISON)
				continue;

			body_position = position[literal->u.atom.pred];
			if (body_position == 0)
				continue;

			if (body_position > head_position)
				holds = false;

			if (literal->kind == RELIC_LITERAL_NEGATED &&
			    schedule->components[body_position - 1].stratum >=
			    schedule->components[head_position - 1].stratum)
				holds = false;

			if (schedule->components[body_position - 1].stratum >
			    schedule->components[head_position - 1].stratum)
				holds = false;
		}
	}

	free(position);
	free(listed);

	return (holds);
}

/*
 * Check a program and compare its schedule, in evaluation order, with the
 * expected rendering; confirm the oracle agrees that the order is sound.
 */
static bool
schedule_is(const char *scenario, const char *program, const char *expected)
{
	relic_t *ctx;
	diag_log_t log;
	char rendered[RENDERED_MAX];
	bool matched;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return (false);

	if (!load_and_check(ctx, program)) {
		checkf(false, scenario, "the program should pass a check: %s",
		    program);
		relic_destroy(ctx);
		return (false);
	}

	render_schedule(ctx, false, rendered, sizeof(rendered));
	matched = strcmp(rendered, expected) == 0;

	checkf(matched, scenario, "the schedule should be \"%s\", not \"%s\"",
	    expected, rendered);
	checkf(schedule_respects_dependencies(ctx), scenario,
	    "the schedule \"%s\" should put every component after what it "
	    "depends on", rendered);

	relic_destroy(ctx);

	return (matched);
}

/*
 * Each shape the plan names, written so that first-mention order and
 * dependency order disagree wherever they can: a naive "in the order
 * written" would get every one of these wrong.
 */
static void
components_come_out_dependencies_first(void)
{
	/*
	 * A fact is not a rule, so a predicate of facts alone has no component;
	 * the programs below read predicates nothing defines where they need
	 * a component at the bottom of the order.
	 */
	schedule_is(SCENARIO_ORDER, "p(a).", "");

	schedule_is(SCENARIO_ORDER,
	    "a(X) :- b(X).\n"
	    "b(X) :- c(X).\n"
	    "c(X) :- input(X).\n",
	    "0:c/1 0:b/1 0:a/1");

	schedule_is(SCENARIO_ORDER,
	    "top(X) :- left(X), right(X).\n"
	    "left(X) :- bottom(X).\n"
	    "right(X) :- bottom(X).\n"
	    "bottom(X) :- input(X).\n",
	    "0:bottom/1 0:left/1 0:right/1 0:top/1");

	schedule_is(SCENARIO_ORDER,
	    "p(X) :- p(X).\n"
	    "p(a).\n",
	    "0:p/1*");

	schedule_is(SCENARIO_ORDER,
	    "even(X) :- succ(Y, X), odd(Y).\n"
	    "odd(X) :- succ(Y, X), even(Y).\n"
	    "even(z).\n"
	    "succ(X, Y) :- pair(X, Y).\n",
	    "0:succ/2 0:even/1,odd/1*");

	schedule_is(SCENARIO_ORDER,
	    "reach(X, Y) :- link(X, Y).\n"
	    "reach(X, Z) :- link(X, Y), reach(Y, Z).\n"
	    "near(X, Y) :- road(X, Y).\n"
	    "near(X, Z) :- road(X, Y), near(Y, Z).\n"
	    "link(X, Y) :- wire(X, Y).\n"
	    "road(X, Y) :- lane(X, Y).\n",
	    "0:link/2 0:reach/2* 0:road/2 0:near/2*");
}

/*
 * Negation lifts a component one stratum above what it negates, and a
 * positive edge keeps it at least as high as what it reads.  The sort by
 * stratum then moves a component that comes early in completion order but
 * sits high behind one that comes later but sits low.
 */
static void
negation_reads_a_strictly_lower_stratum(void)
{
	schedule_is(SCENARIO_STRATA,
	    "edge(a, b).\n"
	    "todo(design).\n"
	    "todo(build).\n"
	    "depends(build, design).\n"
	    "done(design).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n"
	    "blocked(X) :- todo(X), depends(X, Y), not done(Y).\n"
	    "ready(X) :- todo(X), not blocked(X).\n",
	    "0:path/2* 1:blocked/1 2:ready/1");

	/*
	 * high/1 completes before late/1, but late/1 is in the lower stratum,
	 * so the sort puts it first; within stratum 0, completion order holds.
	 */
	schedule_is(SCENARIO_STRATA,
	    "high(X) :- base(X), not middle(X).\n"
	    "late(X) :- base(X).\n"
	    "middle(X) :- low(X), not low2(X).\n"
	    "low(X) :- base(X).\n"
	    "low2(X) :- base(X).\n"
	    "base(a).\n",
	    "0:low/1 0:low2/1 0:late/1 1:middle/1 2:high/1");

	/*
	 * A predicate nothing defines has no component in the schedule, but
	 * negating it still places the rule above stratum 0.
	 */
	schedule_is(SCENARIO_STRATA,
	    "p(X) :- q(X), not missing(X).\n"
	    "q(a).\n",
	    "1:p/1");
}

/*
 * Only components with rules to run are scheduled: a predicate that is only
 * referenced, one of facts alone, and a query's answer predicate, are not.
 */
static void
components_without_rules_are_left_out(void)
{
	relic_t *ctx;
	relic_query_t *query;
	diag_log_t log;
	char rendered[RENDERED_MAX];

	schedule_is(SCENARIO_UNUSED,
	    "p(X) :- q(X), r(X).\n"
	    "q(a).\n"
	    "p(X)?\n",
	    "0:p/1");

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load_and_check(ctx, "q(a).\n")) {
		check(false, SCENARIO_UNUSED,
		    "the program should pass a check");
		relic_destroy(ctx);
		return;
	}

	query = relic_query_prepare(ctx, NULL, "q(X)?");
	check(query != NULL, SCENARIO_UNUSED, "the query should be prepared");

	if (load(ctx, "p(X) :- q(X).\n"))
		check(relic_check(ctx) == 0, SCENARIO_UNUSED,
		    "the extended program should pass a check");

	render_schedule(ctx, false, rendered, sizeof(rendered));
	checkf(strcmp(rendered, "0:p/1") == 0, SCENARIO_UNUSED,
	    "a prepared query's answer predicate should not be scheduled, "
	    "but the schedule is \"%s\"", rendered);

	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * A component lists every rule whose head it holds, as indexes into the
 * program's rule table, in program order.
 */
static void
rules_are_grouped_by_component_in_order(void)
{
	const relic_schedule_t *schedule;
	const relic_component_t *component;
	relic_t *ctx;
	diag_log_t log;
	char rendered[RENDERED_MAX];
	char word[32];
	size_t k;
	size_t i;

	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	/*
	 * Rules 0 to 5 in load order.  path/2 holds rules 1 and 3 and
	 * edge/2 holds rules 0, 2 and 5, interleaved in the text.
	 */
	if (!load_and_check(ctx,
	    "edge(X, Y) :- wire(X, Y).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "edge(X, Y) :- cable(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n"
	    "top(X) :- path(X, _).\n"
	    "edge(X, Y) :- link(X, Y).\n")) {
		check(false, SCENARIO_RULES, "the program should pass a check");
		relic_destroy(ctx);
		return;
	}

	schedule = &ctx->program.schedule;
	rendered[0] = '\0';

	for (k = 0; k < schedule->component_count; k++) {
		component = &schedule->components[k];
		if (k > 0)
			strlcat(rendered, " |", sizeof(rendered));

		for (i = 0; i < component->rule_count; i++) {
			snprintf(word, sizeof(word), " %zu",
			    schedule->rule_indexes[component->first_rule + i]);
			strlcat(rendered, word, sizeof(rendered));
		}
	}

	checkf(strcmp(rendered, " 0 2 5 | 1 3 | 4") == 0, SCENARIO_RULES,
	    "the rules should be \" 0 2 5 | 1 3 | 4\", not \"%s\"", rendered);

	relic_destroy(ctx);
}

/*
 * The same rules in two textual orders give the same components with the
 * same strata, each schedule sound; the same text checked twice gives the
 * same schedule byte for byte.
 */
static void
the_order_depends_only_on_the_program(void)
{
	static const char *const orders[] = {
		"reach(X, Y) :- link(X, Y).\n"
		"reach(X, Z) :- link(X, Y), reach(Y, Z).\n"
		"cut(X) :- node(X), not reach(a, X).\n"
		"even(X) :- next(Y, X), odd(Y).\n"
		"odd(X) :- next(Y, X), even(Y).\n"
		"link(a, b).\n"
		"node(a).\n"
		"next(z, one).\n",

		"next(z, one).\n"
		"odd(X) :- next(Y, X), even(Y).\n"
		"node(a).\n"
		"cut(X) :- node(X), not reach(a, X).\n"
		"link(a, b).\n"
		"even(X) :- next(Y, X), odd(Y).\n"
		"reach(X, Z) :- link(X, Y), reach(Y, Z).\n"
		"reach(X, Y) :- link(X, Y).\n",
	};
	relic_t *ctx;
	diag_log_t log;
	char first_set[RENDERED_MAX];
	char set[RENDERED_MAX];
	char first_run[RENDERED_MAX];
	char run[RENDERED_MAX];
	size_t i;
	int attempt;

	first_set[0] = '\0';

	for (i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
		first_run[0] = '\0';

		for (attempt = 0; attempt < 2; attempt++) {
			ctx = context_logging_to(&log);
			if (ctx == NULL)
				return;

			if (!load_and_check(ctx, orders[i])) {
				checkf(false, SCENARIO_TEXT,
				    "order %zu should pass a check", i);
				relic_destroy(ctx);
				return;
			}

			render_schedule(ctx, true, set, sizeof(set));
			render_schedule(ctx, false, run, sizeof(run));

			checkf(schedule_respects_dependencies(ctx),
			    SCENARIO_TEXT, "order %zu should give a sound "
			    "schedule, not \"%s\"", i, run);

			if (first_set[0] == '\0')
				strlcpy(first_set, set, sizeof(first_set));
			checkf(strcmp(set, first_set) == 0, SCENARIO_TEXT,
			    "order %zu should give the components \"%s\", "
			    "not \"%s\"", i, first_set, set);

			if (attempt == 0)
				strlcpy(first_run, run, sizeof(first_run));
			checkf(strcmp(run, first_run) == 0, SCENARIO_TEXT,
			    "checking order %zu again should give \"%s\", "
			    "not \"%s\"", i, first_run, run);

			relic_destroy(ctx);
		}
	}

	checkf(strcmp(first_set,
	    "0:even/1,odd/1* 0:reach/2* 1:cut/1") ==
	    0, SCENARIO_TEXT, "the components should be the expected ones, "
	    "not \"%s\"", first_set);
}

/*
 * Programs with recursion through negation are refused, one error per
 * offending component with a note per edge of the cycle it names; the
 * refusal is sticky and not repeated; and a load that closes a cycle in a
 * program that was fine is caught by the next check.
 */
static void
recursion_through_negation_is_rejected(void)
{
	static const struct {
		const char	*program;
		size_t		 errors;
		size_t		 notes;
	} cases[] = {
		/* A negated literal over its own head: one edge. */
		{ "q(x).\np(X) :- q(X), not p(X).\n", 1, 1 },
		/* The architecture's example: two negative edges, one cycle. */
		{ "c(x).\na(X) :- c(X), not b(X).\n"
		  "b(X) :- c(X), not a(X).\n", 1, 2 },
		/* Two components, each with a cycle of its own. */
		{ "c(x).\na(X) :- c(X), not a(X).\n"
		  "b(X) :- c(X), not b(X).\n", 2, 2 },
		/*
		 * A three-edge cycle with a longer way round: the shortest
		 * path back is named, not the first one found by depth.
		 */
		{ "c(x).\np(X) :- c(X), not q(X).\n"
		  "q(X) :- r(X).\nq(X) :- s(X).\n"
		  "r(X) :- t(X).\nt(X) :- p(X).\n"
		  "s(X) :- p(X).\n", 1, 3 },
	};
	relic_t *ctx;
	diag_log_t log;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		ctx = context_logging_to(&log);
		if (ctx == NULL)
			return;

		if (!load(ctx, cases[i].program)) {
			checkf(false, SCENARIO_REJECTED, "case %zu should load",
			    i);
			relic_destroy(ctx);
			continue;
		}

		checkf(relic_check(ctx) != 0, SCENARIO_REJECTED,
		    "case %zu should be refused", i);
		checkf(log.errors == cases[i].errors &&
		    log.notes == cases[i].notes, SCENARIO_REJECTED,
		    "case %zu should report %zu error(s) and %zu note(s), not "
		    "%zu and %zu", i, cases[i].errors, cases[i].notes,
		    log.errors, log.notes);
		checkf(ctx->program.invalid && ctx->program.unstratified,
		    SCENARIO_REJECTED, "case %zu should be marked unstratified",
		    i);

		/* A later load and check: refused again, and silently. */
		bzero(&log, sizeof(log));
		if (!load(ctx, "extra(y).\n")) {
			checkf(false, SCENARIO_REJECTED,
			    "case %zu should accept a later load", i);
			relic_destroy(ctx);
			continue;
		}

		checkf(relic_check(ctx) != 0, SCENARIO_REJECTED,
		    "case %zu should stay refused after a later load", i);
		checkf(log.errors == 0 && log.notes == 0, SCENARIO_REJECTED,
		    "case %zu should not repeat itself, but said %zu error(s) "
		    "and %zu note(s)", i, log.errors, log.notes);

		relic_destroy(ctx);
	}

	/* A stratified program, then a load that closes a cycle. */
	ctx = context_logging_to(&log);
	if (ctx == NULL)
		return;

	if (!load_and_check(ctx,
	    "c(x).\na(X) :- c(X), not b(X).\nb(X) :- c(X).\n")) {
		check(false, SCENARIO_REJECTED,
		    "the stratified program should pass a check");
		relic_destroy(ctx);
		return;
	}

	if (load(ctx, "b(X) :- a(X).\n")) {
		check(relic_check(ctx) != 0, SCENARIO_REJECTED,
		    "a load that closes a cycle through negation should be "
		    "refused by the next check");
		checkf(log.errors == 1 && log.notes == 2, SCENARIO_REJECTED,
		    "the cycle it closes should be named, as 1 error and 2 "
		    "notes, not %zu and %zu", log.errors, log.notes);
	}

	relic_destroy(ctx);
}

/*
 * The source of a chain of CHAIN_LENGTH predicates, p0 depending on p1 and so
 * on to the last, which holds the one fact.  With `closed`, the last depends
 * on p0 as well, and the whole chain is one component.
 *
 * The order of the text matters to both shapes, for different reasons.
 * Predicates are numbered in the order they are first mentioned and the
 * component search starts from the first, so the open chain is written from
 * p0 down: a search from p0 has to follow the whole chain, where a search
 * starting at the fact's end would find every edge leading to a predicate
 * already finished and never go deeper than one step.  The cycle is followed
 * in full wherever the search starts, and is written from the fact up
 * instead, because it is one recursive component whose rules run in program
 * order: written this way the fact reaches p0 in one pass, and written the
 * other way it would take a pass per predicate.
 */
static char *
long_program(bool closed)
{
	char *text;
	size_t size;
	size_t used;
	unsigned int i;

	size = (size_t)CHAIN_LENGTH * 40 + 64;
	text = malloc(size);
	if (text == NULL)
		return (NULL);

	used = 0;
	if (closed) {
		used += (size_t)snprintf(text + used, size - used,
		    "p%u(a).\np%u(X) :- p0(X).\n", CHAIN_LENGTH - 1,
		    CHAIN_LENGTH - 1);
		for (i = CHAIN_LENGTH - 1; i > 0; i--)
			used += (size_t)snprintf(text + used, size - used,
			    "p%u(X) :- p%u(X).\n", i - 1, i);
	} else {
		for (i = 1; i < CHAIN_LENGTH; i++)
			used += (size_t)snprintf(text + used, size - used,
			    "p%u(X) :- p%u(X).\n", i - 1, i);
		used += (size_t)snprintf(text + used, size - used,
		    "p%u(a).\n", CHAIN_LENGTH - 1);
	}

	if (used >= size) {
		free(text);
		return (NULL);
	}

	return (text);
}

/*
 * The row callback for answer_is(): count the rows and keep a copy of the
 * last one's value.  The row itself is borrowed only for the call, so the
 * value is copied rather than pointed at.
 */
static int
remember_answer(void *callback_ctx, const relic_value_t *values, size_t count)
{
	answer_t *answer;

	answer = callback_ctx;
	answer->rows++;
	if (count == 1)
		answer->value = values[0];

	return (0);
}

/*
 * Whether a one-column query answers exactly one row, the symbol
 * `expected`.
 */
static bool
answer_is(relic_t *ctx, const char *query_text, const char *expected)
{
	relic_query_t *query;
	answer_t answer;
	const char *bytes;
	int status;

	query = relic_query_prepare(ctx, NULL, query_text);
	if (query == NULL)
		return (false);

	bzero(&answer, sizeof(answer));
	status = relic_query_run(ctx, query, remember_answer, &answer);
	relic_query_destroy(query);

	if (status != 0 || answer.rows != 1 ||
	    answer.value.type != RELIC_SYMBOL)
		return (false);

	bytes = relic_string(ctx, answer.value.v.id, NULL);

	return (bytes != NULL && strcmp(bytes, expected) == 0);
}

/*
 * The thread body for run_on_small_stack(): check the program, and when it
 * passes, ask for what reaches the far end.
 */
static void *
check_and_query(void *argument)
{
	chain_run_t *run;

	run = argument;
	run->checked = relic_check(run->ctx) == 0;
	if (run->checked)
		run->answered = answer_is(run->ctx, "p0(X)?", "a");

	return (NULL);
}

/*
 * Check and query `run->ctx` on a thread whose stack is CHAIN_STACK bytes.
 * Returns whether the thread could be started and joined; a search that
 * overflowed the stack does not return at all, it crashes the program.
 */
static bool
run_on_small_stack(chain_run_t *run)
{
	pthread_attr_t attributes;
	pthread_t thread;
	bool started;

	if (pthread_attr_init(&attributes) != 0)
		return (false);

	started = false;
	if (pthread_attr_setstacksize(&attributes, CHAIN_STACK) == 0)
		started = pthread_create(&thread, &attributes, check_and_query,
		    run) == 0;
	pthread_attr_destroy(&attributes);
	if (!started)
		return (false);

	return (pthread_join(thread, NULL) == 0);
}

/*
 * A chain and a cycle of CHAIN_LENGTH predicates are analysed and evaluated
 * on a small stack without overflowing it: the chain as that many
 * components, the cycle as one.
 */
static void
a_long_chain_does_not_overflow(void)
{
	relic_t *ctx;
	chain_run_t run;
	diag_log_t log;
	char *program;
	int closed;

	for (closed = 0; closed < 2; closed++) {
		program = long_program(closed != 0);
		if (program == NULL) {
			check(false, SCENARIO_CHAIN, "the program is built");
			return;
		}

		ctx = context_logging_to(&log);
		if (ctx == NULL) {
			free(program);
			return;
		}

		bzero(&run, sizeof(run));
		run.ctx = ctx;

		if (!load(ctx, program)) {
			checkf(false, SCENARIO_CHAIN, "the %s should load",
			    closed ? "cycle" : "chain");
		} else if (!run_on_small_stack(&run)) {
			check(false, SCENARIO_CHAIN,
			    "a thread with a small stack should run");
		} else if (!run.checked) {
			checkf(false, SCENARIO_CHAIN,
			    "the %s should pass a check",
			    closed ? "cycle" : "chain");
		} else {
			/*
			 * The open chain's last predicate holds the fact
			 * and derives nothing, so it has no component.
			 */
			checkf(ctx->program.schedule.component_count ==
			    (closed ? 1 : CHAIN_LENGTH - 1), SCENARIO_CHAIN,
			    "the %s should be %d component(s), not %zu",
			    closed ? "cycle" : "chain",
			    closed ? 1 : CHAIN_LENGTH - 1,
			    ctx->program.schedule.component_count);
			checkf(run.answered, SCENARIO_CHAIN,
			    "the fact should reach the far end of the %s",
			    closed ? "cycle" : "chain");
		}

		relic_destroy(ctx);
		free(program);
	}
}

int
main(void)
{
	components_come_out_dependencies_first();
	negation_reads_a_strictly_lower_stratum();
	components_without_rules_are_left_out();
	rules_are_grouped_by_component_in_order();
	the_order_depends_only_on_the_program();
	recursion_through_negation_is_rejected();
	a_long_chain_does_not_overflow();

	return (check_summary("test_stratify"));
}
