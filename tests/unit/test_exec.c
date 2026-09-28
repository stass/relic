/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the executor, run directly rather than through an
 * evaluation.
 *
 * The evaluator always empties a full batch before it runs a plan again,
 * never plans an EMIT alone, and inserts only what a plan's views exclude,
 * so what the executor does for a caller that does otherwise, and at the
 * exact boundaries of its batch, is not reached through it.  Each scenario
 * builds by hand the plan that scans one relation and emits each tuple it
 * reads, runs it a call at a time, and checks every batch it is handed
 * against the relation it scanned: every tuple once, in identifier order,
 * a full batch exactly full, and nothing written past the batch.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_BATCHES	"a plan's batches hold each derivation once"
#define	SCENARIO_EMIT_ONLY	"a plan of its EMIT alone derives its head once"
#define	SCENARIO_GROWTH		"what is appended between calls is not read"
#define	SCENARIO_NEGLECT	"a batch left full is not written past"
#define	SCENARIO_UNSTARTED	"a scratch no plan has started derives nothing"

/*
 * Words past the end of the batch, set to GUARD_VALUE, which an EMIT that
 * wrote past the batch would overwrite.  No tuple holds GUARD_VALUE: the
 * scanned relation holds small integers, whose identifiers are small.
 */
#define	GUARD_WORDS	8u
#define	GUARD_VALUE	UINT32_C(0xdeadbeef)

typedef struct fixture fixture_t;

/*
 * A context with a relation `scanned` of one column and the plan that
 * scans it and emits each tuple into `emitted`, ready to run in `scratch`.
 * `batch` has room for one batch of one column and the guard words after.
 */
struct fixture {
	relic_t			*ctx;
	relic_relation_t	*scanned;
	relic_relation_t	*emitted;
	relic_plan_term_t	 bind;
	relic_plan_term_t	 read;
	relic_op_t		 ops[2];
	relic_plan_t		 plan;
	relic_exec_env_t	 env;
	relic_op_state_t	 states[2];
	relic_dom_t		 batch[RELIC_INSERT_BATCH + GUARD_WORDS];
	relic_scratch_t		 scratch;
};

static void	 a_batch_left_full_is_not_written_past(void);
static void	 a_plans_batches_hold_each_derivation_once(void);
static void	 a_plan_of_its_emit_alone_derives_its_head_once(void);
static void	 a_scratch_no_plan_has_started_derives_nothing(void);
static bool	 fill(fixture_t *fixture, const char *scenario, int64_t first,
		    size_t count);
static void	 fixture_close(fixture_t *fixture);
static bool	 fixture_open(fixture_t *fixture, const char *scenario);
static bool	 guards_hold(const fixture_t *fixture);
static relic_relation_t *relation(relic_t *ctx, const char *scenario,
		    const char *name);
static bool	 run_to_end(fixture_t *fixture, const char *scenario,
		    size_t expected, bool grow);
static void	 what_is_appended_between_calls_is_not_read(void);

/*
 * The derived relation of a predicate of one column with this name, or NULL
 * having complained.  The predicate is made derived as evaluation's are, by
 * a rule deriving it joining a rule table, here a scratch one.
 */
static relic_relation_t *
relation(relic_t *ctx, const char *scenario, const char *name)
{
	relic_location_t where;
	relic_relation_t *rel;
	relic_ruletab_t rules;
	relic_rule_t rule;
	relic_pred_id_t pred;
	uint32_t symbol;
	int appended;

	bzero(&where, sizeof(where));
	where.file = "test_exec.c";
	where.line = 1;
	where.column = 1;

	if (relic_symbol_intern(ctx, name, strlen(name), &symbol) != 0 ||
	    relic_pred_intern(ctx, symbol, 1, &where, &pred) != 0) {
		checkf(false, scenario, "%s/1 should intern", name);
		return (NULL);
	}

	bzero(&rules, sizeof(rules));
	bzero(&rule, sizeof(rule));
	rule.head = pred;
	rule.head_arity = 1;
	appended = relic_ruletab_append(ctx, &rules, &rule);
	relic_free_array(rules.entries);

	rel = appended == 0 ? relic_relation_written(ctx, pred) : NULL;
	if (rel == NULL)
		checkf(false, scenario, "%s/1 should have a relation", name);

	return (rel);
}

/*
 * Set up a fixture whose scanned relation is empty.  Returns false having
 * complained, with nothing left to close.
 */
static bool
fixture_open(fixture_t *fixture, const char *scenario)
{
	size_t i;

	bzero(fixture, sizeof(*fixture));

	fixture->ctx = relic_create_config(NULL);
	if (fixture->ctx == NULL) {
		check(false, scenario, "a context should be created");
		return (false);
	}

	fixture->scanned = relation(fixture->ctx, scenario, "r");
	fixture->emitted = relation(fixture->ctx, scenario, "q");
	if (fixture->scanned == NULL || fixture->emitted == NULL) {
		relic_destroy(fixture->ctx);
		return (false);
	}

	fixture->bind.action = RELIC_PLAN_BIND;
	fixture->bind.u.slot = 0;
	fixture->read.action = RELIC_PLAN_BOUND;
	fixture->read.u.slot = 0;

	fixture->ops[0].kind = RELIC_OP_SCAN;
	fixture->ops[0].u.atom.relation = fixture->scanned;
	fixture->ops[0].u.atom.terms = &fixture->bind;
	fixture->ops[0].u.atom.arity = 1;
	fixture->ops[0].u.atom.view = RELIC_VIEW_ALL;
	fixture->ops[1].kind = RELIC_OP_EMIT;
	fixture->ops[1].u.atom.relation = fixture->emitted;
	fixture->ops[1].u.atom.terms = &fixture->read;
	fixture->ops[1].u.atom.arity = 1;
	fixture->plan.ops = fixture->ops;
	fixture->plan.op_count = 2;

	fixture->env.ctx = fixture->ctx;
	fixture->env.deltas = NULL;
	fixture->env.note_mixed = false;

	for (i = 0; i < GUARD_WORDS; i++)
		fixture->batch[RELIC_INSERT_BATCH + i] = GUARD_VALUE;
	relic_scratch_init(&fixture->scratch, fixture->states, 2,
	    fixture->batch, 1);

	return (true);
}

static void
fixture_close(fixture_t *fixture)
{
	relic_destroy(fixture->ctx);
}

/*
 * Append the integers first to first + count - 1 to the scanned relation,
 * in that order.
 */
static bool
fill(fixture_t *fixture, const char *scenario, int64_t first, size_t count)
{
	relic_dom_t value;
	size_t i;
	bool added;

	for (i = 0; i < count; i++) {
		if (relic_value_intern_integer(fixture->ctx, first + (int64_t)i,
		    &value) != 0 ||
		    relic_relation_insert(fixture->ctx, fixture->scanned,
		    &value, &added) != 0 || !added) {
			check(false, scenario, "the scanned relation fills");
			return (false);
		}
	}

	return (true);
}

/*
 * Whether every word past the batch still holds GUARD_VALUE.
 */
static bool
guards_hold(const fixture_t *fixture)
{
	size_t i;

	for (i = 0; i < GUARD_WORDS; i++) {
		if (fixture->batch[RELIC_INSERT_BATCH + i] != GUARD_VALUE)
			return (false);
	}

	return (true);
}

/*
 * Start the fixture's plan and run it a call at a time to its end, taking
 * each batch as it comes, and check that it derived the scanned relation's
 * `expected` tuples, each once and in identifier order: every batch the
 * executor calls full holds RELIC_INSERT_BATCH tuples, the last holds the
 * rest, and nothing is written past the batch.  With `grow`, a tuple is
 * appended to the scanned relation after every call, which the plan's view,
 * resolved when it started, excludes.
 */
static bool
run_to_end(fixture_t *fixture, const char *scenario, size_t expected,
    bool grow)
{
	enum relic_exec_status status;
	relic_scratch_t *scratch;
	const relic_dom_t *tuple;
	size_t derived;
	size_t i;

	scratch = &fixture->scratch;
	relic_exec_start(scratch, &fixture->env, &fixture->plan);

	derived = 0;
	do {
		status = relic_exec_run(scratch);

		if (status == RELIC_EXEC_BATCH_FULL &&
		    scratch->batch_count != RELIC_INSERT_BATCH) {
			checkf(false, scenario, "a batch called full holds "
			    "%zu tuples", scratch->batch_count);
			return (false);
		}
		if (derived + scratch->batch_count > expected) {
			checkf(false, scenario, "%zu tuples should be derived, "
			    "and more were", expected);
			return (false);
		}

		for (i = 0; i < scratch->batch_count; i++) {
			tuple = relic_relation_tuple(fixture->scanned,
			    (relic_tuple_id_t)(RELIC_TUPLE_FIRST + derived));
			if (tuple == NULL || scratch->batch[i] != tuple[0]) {
				checkf(false, scenario, "derivation %zu should "
				    "be tuple %zu", derived + 1, derived + 1);
				return (false);
			}
			derived++;
		}
		scratch->batch_count = 0;

		if (!guards_hold(fixture)) {
			check(false, scenario, "nothing is written past the "
			    "batch");
			return (false);
		}
		if (grow && !fill(fixture, scenario,
		    (int64_t)(1000000 + relic_relation_count(fixture->scanned)),
		    1))
			return (false);
	} while (status == RELIC_EXEC_BATCH_FULL);

	checkf(derived == expected, scenario, "%zu tuples should be "
	    "derived, not %zu", expected, derived);
	checkf(scratch->counters.tuples_scanned == expected, scenario,
	    "%zu tuples should be scanned, not %llu", expected,
	    (unsigned long long)scratch->counters.tuples_scanned);
	check(relic_exec_run(scratch) == RELIC_EXEC_FINISHED &&
	    scratch->batch_count == 0, scenario,
	    "a finished plan derives nothing more");

	bzero(&scratch->counters, sizeof(scratch->counters));

	return (derived == expected);
}

/*
 * A plan's batches hold every tuple it derives once, whether the count
 * falls short of a batch, fills one exactly, or passes one by a tuple, and
 * the same at two batches.
 */
static void
a_plans_batches_hold_each_derivation_once(void)
{
	static const size_t counts[] = {
		0, 1, RELIC_INSERT_BATCH - 1, RELIC_INSERT_BATCH,
		RELIC_INSERT_BATCH + 1, 2 * RELIC_INSERT_BATCH - 1,
		2 * RELIC_INSERT_BATCH, 2 * RELIC_INSERT_BATCH + 1
	};
	fixture_t fixture;
	size_t held;
	size_t i;

	if (!fixture_open(&fixture, SCENARIO_BATCHES))
		return;

	held = 0;
	for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		if (!fill(&fixture, SCENARIO_BATCHES, (int64_t)held + 1,
		    counts[i] - held))
			break;
		held = counts[i];

		if (!run_to_end(&fixture, SCENARIO_BATCHES, held, false)) {
			checkf(false, SCENARIO_BATCHES, "over %zu tuples",
			    held);
			break;
		}
	}

	fixture_close(&fixture);
}

/*
 * A caller that runs a plan again without taking a full batch is told the
 * batch is full again, and the batch is neither appended to nor changed;
 * taking it then lets the plan go on as though it had been taken at once.
 */
static void
a_batch_left_full_is_not_written_past(void)
{
	relic_dom_t first[RELIC_INSERT_BATCH];
	fixture_t fixture;
	relic_scratch_t *scratch;

	if (!fixture_open(&fixture, SCENARIO_NEGLECT))
		return;
	if (!fill(&fixture, SCENARIO_NEGLECT, 1, 2 * RELIC_INSERT_BATCH + 1)) {
		fixture_close(&fixture);
		return;
	}

	scratch = &fixture.scratch;
	relic_exec_start(scratch, &fixture.env, &fixture.plan);
	check(relic_exec_run(scratch) == RELIC_EXEC_BATCH_FULL &&
	    scratch->batch_count == RELIC_INSERT_BATCH, SCENARIO_NEGLECT,
	    "the first call fills the batch");
	bcopy(fixture.batch, first, sizeof(first));

	check(relic_exec_run(scratch) == RELIC_EXEC_BATCH_FULL &&
	    scratch->batch_count == RELIC_INSERT_BATCH, SCENARIO_NEGLECT,
	    "a second call over a full batch reports it full");
	check(bcmp(fixture.batch, first, sizeof(first)) == 0 &&
	    guards_hold(&fixture), SCENARIO_NEGLECT,
	    "the neglected batch is unchanged and nothing is written past it");
	check(scratch->counters.tuples_scanned == RELIC_INSERT_BATCH,
	    SCENARIO_NEGLECT, "the second call reads nothing");

	scratch->batch_count = 0;
	bzero(&scratch->counters, sizeof(scratch->counters));
	check(relic_exec_run(scratch) == RELIC_EXEC_BATCH_FULL &&
	    scratch->batch_count == RELIC_INSERT_BATCH &&
	    fixture.batch[0] != first[0], SCENARIO_NEGLECT,
	    "once taken, the batch fills with the next tuples");
	scratch->batch_count = 0;
	check(relic_exec_run(scratch) == RELIC_EXEC_FINISHED &&
	    scratch->batch_count == 1, SCENARIO_NEGLECT,
	    "the plan ends with the one tuple left");

	fixture_close(&fixture);
}

/*
 * Tuples appended to the scanned relation between two calls lie past the
 * view the plan resolved when it started, and none is derived, however many
 * batches the plan takes.
 */
static void
what_is_appended_between_calls_is_not_read(void)
{
	fixture_t fixture;
	size_t held;

	if (!fixture_open(&fixture, SCENARIO_GROWTH))
		return;

	held = 3 * RELIC_INSERT_BATCH + 5;
	if (fill(&fixture, SCENARIO_GROWTH, 1, held))
		(void)run_to_end(&fixture, SCENARIO_GROWTH, held, true);

	fixture_close(&fixture);
}

/*
 * A plan whose first operation is its EMIT derives its head once and is
 * finished.  No rule is planned that way, and the executor needs no rule to
 * say so.
 */
static void
a_plan_of_its_emit_alone_derives_its_head_once(void)
{
	relic_plan_term_t constant;
	relic_scratch_t *scratch;
	relic_plan_t emit_only;
	fixture_t fixture;
	relic_dom_t value;

	if (!fixture_open(&fixture, SCENARIO_EMIT_ONLY))
		return;
	if (relic_value_intern_integer(fixture.ctx, 7, &value) != 0) {
		check(false, SCENARIO_EMIT_ONLY, "a value is interned");
		fixture_close(&fixture);
		return;
	}

	constant.action = RELIC_PLAN_CONSTANT;
	constant.u.constant = value;
	fixture.ops[1].u.atom.terms = &constant;
	emit_only.ops = &fixture.ops[1];
	emit_only.op_count = 1;

	scratch = &fixture.scratch;
	relic_exec_start(scratch, &fixture.env, &emit_only);
	check(relic_exec_run(scratch) == RELIC_EXEC_FINISHED &&
	    scratch->batch_count == 1 && fixture.batch[0] == value,
	    SCENARIO_EMIT_ONLY, "the head is derived once and the plan ends");

	scratch->batch_count = 0;
	check(relic_exec_run(scratch) == RELIC_EXEC_FINISHED &&
	    scratch->batch_count == 0, SCENARIO_EMIT_ONLY,
	    "nothing more is derived");

	fixture_close(&fixture);
}

/*
 * A scratch fresh from relic_scratch_init() is finished, so running it
 * derives nothing and reads nothing.
 */
static void
a_scratch_no_plan_has_started_derives_nothing(void)
{
	fixture_t fixture;

	if (!fixture_open(&fixture, SCENARIO_UNSTARTED))
		return;

	check(relic_exec_run(&fixture.scratch) == RELIC_EXEC_FINISHED &&
	    fixture.scratch.batch_count == 0 &&
	    fixture.scratch.counters.tuples_scanned == 0, SCENARIO_UNSTARTED,
	    "an unstarted scratch is finished and empty");

	fixture_close(&fixture);
}

int
main(void)
{
	a_scratch_no_plan_has_started_derives_nothing();
	a_plans_batches_hold_each_derivation_once();
	a_batch_left_full_is_not_written_past();
	what_is_appended_between_calls_is_not_read();
	a_plan_of_its_emit_alone_derives_its_head_once();

	return (check_summary("test_exec"));
}
