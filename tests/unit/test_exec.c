/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
 * a full batch exactly full, and nothing written past the batch.  The
 * rest join a second scan, lookup, aggregate fold or negated probe that
 * does a quantum of work and more for each binding, to see the executor
 * pause with its batch empty once a quantum is done, and resume where it
 * paused.
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

#define	SCENARIO_BATCHES	"a plan's batches hold each derivation once"
#define	SCENARIO_EMIT_ONLY	"a plan of its EMIT alone derives its head once"
#define	SCENARIO_GROWN_CHAIN	"a lookup driver's visits end at its view"
#define	SCENARIO_GROWTH		"what is appended between calls is not read"
#define	SCENARIO_NEGLECT	"a batch left full is not written past"
#define	SCENARIO_PIECES		"pieces of a view derive what the whole does"
#define	SCENARIO_PREFIX_PAUSE	"a prefix pausing at its driver reaches it"
#define	SCENARIO_PROBES		"a probe's scan counts toward the quantum"
#define	SCENARIO_DELTA_CHECK	"a check of a delta begins at its start"
#define	SCENARIO_FOLDS		"a fold's reads count toward the quantum"
#define	SCENARIO_FOLD_EMIT	"a fold that pauses emits its value next call"
#define	SCENARIO_LOOKUP		"a lookup rejecting its chain pauses inside it"
#define	SCENARIO_QUANTUM	"a plan deriving nothing pauses every quantum"
#define	SCENARIO_SLICES		"a lookup's slices derive what the whole does"
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
static void	 pieces_of_a_view_derive_what_the_whole_does(void);
static void	 a_plan_deriving_nothing_pauses_every_quantum(void);
static void	 a_folds_reads_count_toward_the_quantum(void);
static void	 a_fold_that_pauses_emits_its_value_next_call(void);
static void	 a_lookup_rejecting_its_chain_pauses_inside_it(void);
static void	 a_lookups_slices_derive_what_the_whole_does(void);
static void	 a_prefix_pausing_at_its_driver_reaches_it(void);
static void	 a_lookup_drivers_visits_end_at_its_view(void);
static bool	 run_collecting(relic_scratch_t *scratch, relic_dom_t *out,
		    size_t room, size_t *countp, size_t base);
static void	 a_probes_scan_counts_toward_the_quantum(void);
static void	 a_check_of_a_delta_begins_at_its_start(void);
static void	 probes_scanning_count_toward_the_quantum(
		    enum relic_op_kind kind);
static bool	 fill(fixture_t *fixture, const char *scenario, int64_t first,
		    size_t count);
static void	 fixture_close(fixture_t *fixture);
static bool	 fixture_open(fixture_t *fixture, const char *scenario);
static bool	 guards_hold(const fixture_t *fixture);
static relic_relation_t *relation(relic_t *ctx, const char *scenario,
		    const char *name, uint16_t arity);
static bool	 run_to_end(fixture_t *fixture, const char *scenario,
		    size_t expected, bool grow);
static void	 what_is_appended_between_calls_is_not_read(void);

/*
 * The derived relation of a predicate of `arity` columns with this name, or
 * NULL having complained.  The predicate is made derived as evaluation's
 * are, by a rule deriving it joining a rule table, here a scratch one.
 */
static relic_relation_t *
relation(relic_t *ctx, const char *scenario, const char *name,
    uint16_t arity)
{
	relic_location_t where;
	relic_relation_t *rel;
	relic_ruletab_t rules;
	relic_rule_t rule;
	relic_pred_id_t pred;
	int appended;

	bzero(&where, sizeof(where));
	where.file = "test_exec.c";
	where.line = 1;
	where.column = 1;

	if (relic_pred_intern(ctx, name, strlen(name), arity, &where,
	    &pred) != 0) {
		checkf(false, scenario, "%s/%u should intern", name,
		    (unsigned)arity);
		return (NULL);
	}

	bzero(&rules, sizeof(rules));
	bzero(&rule, sizeof(rule));
	rule.head = pred;
	rule.head_arity = arity;
	appended = relic_ruletab_append(ctx, &rules, &rule);
	free(rules.entries);

	rel = appended == 0 ? relic_relation_written(ctx, pred) : NULL;
	if (rel == NULL)
		checkf(false, scenario, "%s/%u should have a relation", name,
		    (unsigned)arity);

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

	fixture->scanned = relation(fixture->ctx, scenario, "r", 1);
	fixture->emitted = relation(fixture->ctx, scenario, "q", 1);
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
	fixture->ops[1].u.emit.terms = &fixture->read;
	fixture->ops[1].u.emit.arity = 1;
	fixture->ops[1].u.emit.pred = RELIC_ID_NONE;
	fixture->plan.ops = fixture->ops;
	fixture->plan.op_count = 2;

	fixture->env.ctx = fixture->ctx;
	fixture->env.deltas = NULL;
	fixture->env.note_mixed = false;

	for (i = 0; i < GUARD_WORDS; i++)
		fixture->batch[RELIC_INSERT_BATCH + i] = GUARD_VALUE;
	relic_scratch_init(&fixture->scratch, fixture->states, 2,
	    fixture->batch, NULL, 1);

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

		if (status == RELIC_EXEC_PAUSED &&
		    scratch->batch_count != RELIC_INSERT_BATCH) {
			checkf(false, scenario, "a plan deriving a tuple a "
			    "turn paused with %zu tuples, short of a full "
			    "batch", scratch->batch_count);
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
	} while (status == RELIC_EXEC_PAUSED);

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
	check(relic_exec_run(scratch) == RELIC_EXEC_PAUSED &&
	    scratch->batch_count == RELIC_INSERT_BATCH, SCENARIO_NEGLECT,
	    "the first call fills the batch");
	bcopy(fixture.batch, first, sizeof(first));

	check(relic_exec_run(scratch) == RELIC_EXEC_PAUSED &&
	    scratch->batch_count == RELIC_INSERT_BATCH, SCENARIO_NEGLECT,
	    "a second call over a full batch reports it full");
	check(bcmp(fixture.batch, first, sizeof(first)) == 0 &&
	    guards_hold(&fixture), SCENARIO_NEGLECT,
	    "the neglected batch is unchanged and nothing is written past it");
	check(scratch->counters.tuples_scanned == RELIC_INSERT_BATCH,
	    SCENARIO_NEGLECT, "the second call reads nothing");

	scratch->batch_count = 0;
	bzero(&scratch->counters, sizeof(scratch->counters));
	check(relic_exec_run(scratch) == RELIC_EXEC_PAUSED &&
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
	fixture.ops[1].u.emit.terms = &constant;
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

/*
 * A plan started on a slice of its driving scan's view derives what the
 * whole plan derives while its cursor is inside the slice, in the same
 * order, so slices that cover the view in order, run one after another,
 * derive what the whole does and scan as much.  The slices here are one
 * tuple long, a batch long, and the rest, and one runs past a batch; an
 * empty one is never started, a round running nothing for it.
 */
static void
pieces_of_a_view_derive_what_the_whole_does(void)
{
	enum { HELD = 150 };
	static const relic_tuple_id_t cuts[] = {
		RELIC_TUPLE_FIRST, RELIC_TUPLE_FIRST, RELIC_TUPLE_FIRST + 1,
		RELIC_TUPLE_FIRST + 1 + RELIC_INSERT_BATCH,
		RELIC_TUPLE_FIRST + HELD
	};
	enum relic_exec_status status;
	relic_tuple_range_t piece;
	relic_tuple_range_t view;
	relic_scratch_t *scratch;
	const relic_dom_t *tuple;
	fixture_t fixture;
	size_t derived;
	size_t i;
	size_t k;
	bool ordered;

	if (!fixture_open(&fixture, SCENARIO_PIECES))
		return;
	if (!fill(&fixture, SCENARIO_PIECES, 1, HELD)) {
		fixture_close(&fixture);
		return;
	}

	scratch = &fixture.scratch;
	view = relic_exec_view(&fixture.env, &fixture.ops[0].u.atom);
	check(view.first == cuts[0] &&
	    view.end == cuts[sizeof(cuts) / sizeof(cuts[0]) - 1],
	    SCENARIO_PIECES, "the view is the whole relation");

	derived = 0;
	ordered = true;
	for (k = 0; k + 1 < sizeof(cuts) / sizeof(cuts[0]); k++) {
		piece.first = cuts[k];
		piece.end = cuts[k + 1];
		if (piece.first == piece.end)
			continue;
		relic_exec_start_slice(scratch, &fixture.env, &fixture.plan,
		    piece);

		do {
			status = relic_exec_run(scratch);
			for (i = 0; i < scratch->batch_count; i++) {
				tuple = relic_relation_tuple(fixture.scanned,
				    (relic_tuple_id_t)(RELIC_TUPLE_FIRST +
				    derived));
				if (tuple == NULL ||
				    scratch->batch[i] != tuple[0])
					ordered = false;
				derived++;
			}
			scratch->batch_count = 0;
		} while (status == RELIC_EXEC_PAUSED);
	}

	checkf(derived == HELD && ordered, SCENARIO_PIECES, "the pieces "
	    "should derive the %u tuples in order, derived %zu%s", HELD,
	    derived, ordered ? "" : " out of order");
	checkf(scratch->counters.tuples_scanned == HELD, SCENARIO_PIECES,
	    "the pieces should scan %u tuples, not %llu", HELD,
	    (unsigned long long)scratch->counters.tuples_scanned);
	check(guards_hold(&fixture), SCENARIO_PIECES,
	    "nothing is written past the batch");

	fixture_close(&fixture);
}

/*
 * A join that derives almost nothing still returns to its caller at least
 * every RELIC_EXEC_QUANTUM of work, its batch empty, so that the caller can
 * look for an interrupt, and pausing changes nothing it derives.  The plan
 * scans r, then scans q for each tuple's value, and emits the value when q
 * holds it.  r holds a few values and q many more than a quantum, of which
 * only one is in r, so every scan of q rejects a quantum's worth of tuples
 * within one step: the executor must pause inside that scan, not only
 * between steps.
 */
static void
a_plan_deriving_nothing_pauses_every_quantum(void)
{
	relic_op_state_t states[3];
	relic_scratch_t scratch;
	relic_op_t ops[3];
	relic_plan_t plan;
	fixture_t fixture;
	relic_dom_t match;
	relic_dom_t value;
	relic_dom_t batch[RELIC_INSERT_BATCH];
	enum relic_exec_status status;
	uint64_t read_before;
	uint64_t most_read;
	size_t empty_pauses;
	size_t derived;
	size_t outer;
	size_t inner;
	size_t i;
	bool added;

	if (!fixture_open(&fixture, SCENARIO_QUANTUM))
		return;

	outer = 8;
	inner = 3 * RELIC_EXEC_QUANTUM;
	if (!fill(&fixture, SCENARIO_QUANTUM, 1, outer) ||
	    relic_value_intern_integer(fixture.ctx, (int64_t)outer / 2,
	    &match) != 0) {
		check(false, SCENARIO_QUANTUM, "r fills");
		fixture_close(&fixture);
		return;
	}
	for (i = 0; i < inner; i++) {
		if (relic_value_intern_integer(fixture.ctx,
		    (int64_t)(1000000 + i), &value) != 0 ||
		    relic_relation_insert(fixture.ctx, fixture.emitted, &value,
		    &added) != 0) {
			check(false, SCENARIO_QUANTUM, "q fills");
			fixture_close(&fixture);
			return;
		}
	}
	if (relic_relation_insert(fixture.ctx, fixture.emitted, &match,
	    &added) != 0) {
		check(false, SCENARIO_QUANTUM, "q holds the match");
		fixture_close(&fixture);
		return;
	}
	inner++;

	ops[0] = fixture.ops[0];
	ops[1] = fixture.ops[0];
	ops[1].u.atom.relation = fixture.emitted;
	ops[1].u.atom.terms = &fixture.read;
	ops[2] = fixture.ops[1];
	plan.ops = ops;
	plan.op_count = 3;

	relic_scratch_init(&scratch, states, 3, batch, NULL, 1);
	relic_exec_start(&scratch, &fixture.env, &plan);

	empty_pauses = 0;
	derived = 0;
	most_read = 0;
	do {
		read_before = scratch.counters.tuples_scanned;
		status = relic_exec_run(&scratch);
		if (scratch.counters.tuples_scanned - read_before > most_read)
			most_read = scratch.counters.tuples_scanned -
			    read_before;

		if (status == RELIC_EXEC_PAUSED && scratch.batch_count == 0)
			empty_pauses++;
		if (scratch.batch_count == 1 && batch[0] == match)
			derived++;
		else if (scratch.batch_count != 0)
			derived += 2;
		scratch.batch_count = 0;
	} while (status == RELIC_EXEC_PAUSED);

	checkf(empty_pauses >= outer * 2, SCENARIO_QUANTUM, "reading %zu "
	    "tuples should pause at least %zu times with nothing derived, "
	    "not %zu", outer * (inner + 1), outer * 2, empty_pauses);
	checkf(most_read <= 2 * RELIC_EXEC_QUANTUM, SCENARIO_QUANTUM, "a call "
	    "should read at most two quanta of %u tuples, not %llu",
	    RELIC_EXEC_QUANTUM, (unsigned long long)most_read);
	check(derived == 1, SCENARIO_QUANTUM,
	    "the one value r and q share is derived once");
	checkf(scratch.counters.tuples_scanned == outer * (inner + 1),
	    SCENARIO_QUANTUM, "the plan should read %zu tuples, not %llu",
	    outer * (inner + 1),
	    (unsigned long long)scratch.counters.tuples_scanned);

	fixture_close(&fixture);
}

/*
 * A fold cannot pause part way, so it charges the tuples it reads to the
 * call, and the call pauses as soon as a fold's step returns with them
 * spent.  The plan scans r, counts q -- a group a quantum large -- three
 * times over for each of r's tuples, and scans the empty z, which rejects
 * every binding.  A call may read one fold's walk past its quantum and no
 * more: one that paused only when its backtracks ran out would read a
 * quantum for every tuple of r, and one that paused only at a backtrack
 * would read all three walks.
 */
static void
a_folds_reads_count_toward_the_quantum(void)
{
	relic_op_state_t states[6];
	relic_scratch_t scratch;
	relic_relation_t *empty;
	relic_op_t ops[6];
	relic_plan_t plan;
	fixture_t fixture;
	relic_dom_t value;
	relic_dom_t batch[RELIC_INSERT_BATCH];
	enum relic_exec_status status;
	uint64_t read_before;
	uint64_t most_read;
	size_t outer;
	size_t group;
	size_t i;
	bool added;

	if (!fixture_open(&fixture, SCENARIO_FOLDS))
		return;

	outer = 64;
	group = RELIC_EXEC_QUANTUM;
	empty = relation(fixture.ctx, SCENARIO_FOLDS, "z", 1);
	if (empty == NULL || !fill(&fixture, SCENARIO_FOLDS, 1, outer)) {
		fixture_close(&fixture);
		return;
	}
	for (i = 0; i < group; i++) {
		if (relic_value_intern_integer(fixture.ctx, (int64_t)i,
		    &value) != 0 ||
		    relic_relation_insert(fixture.ctx, fixture.emitted, &value,
		    &added) != 0) {
			check(false, SCENARIO_FOLDS, "q fills");
			fixture_close(&fixture);
			return;
		}
	}

	bzero(ops, sizeof(ops));
	ops[0] = fixture.ops[0];
	for (i = 1; i <= 3; i++) {
		ops[i].kind = RELIC_OP_AGGREGATE;
		ops[i].u.aggregate.relation = fixture.emitted;
		ops[i].u.aggregate.access = RELIC_FOLD_SCAN;
		ops[i].u.aggregate.fn = RELIC_AGG_COUNT;
		ops[i].u.aggregate.arity = 1;
		ops[i].u.aggregate.kind = RELIC_SLOT_NUMBER;
		ops[i].u.aggregate.slot = (uint16_t)i;
		ops[i].u.aggregate.binds = true;
	}
	ops[4] = fixture.ops[0];
	ops[4].u.atom.relation = empty;
	ops[4].u.atom.terms = &fixture.read;
	ops[5] = fixture.ops[1];
	plan.ops = ops;
	plan.op_count = 6;

	relic_scratch_init(&scratch, states, 6, batch, NULL, 1);
	relic_exec_start(&scratch, &fixture.env, &plan);

	most_read = 0;
	do {
		read_before = scratch.counters.tuples_scanned;
		status = relic_exec_run(&scratch);
		if (scratch.counters.tuples_scanned - read_before > most_read)
			most_read = scratch.counters.tuples_scanned -
			    read_before;
		scratch.batch_count = 0;
	} while (status == RELIC_EXEC_PAUSED);

	checkf(most_read <= 2 * RELIC_EXEC_QUANTUM + 1, SCENARIO_FOLDS,
	    "a call should read at most a walk past its quantum, not %llu",
	    (unsigned long long)most_read);
	checkf(scratch.counters.tuples_scanned == outer * (3 * group + 1),
	    SCENARIO_FOLDS, "the plan should read %zu tuples, not %llu",
	    outer * (3 * group + 1),
	    (unsigned long long)scratch.counters.tuples_scanned);

	fixture_close(&fixture);
}

/*
 * A probe that scans has no cursor to resume from, so it charges the tuples
 * it reads to the call as a fold does, whether a negated literal asks it or
 * a positive one.
 */
static void
a_probes_scan_counts_toward_the_quantum(void)
{
	probes_scanning_count_toward_the_quantum(RELIC_OP_ANTI_MEMBER);
	probes_scanning_count_toward_the_quantum(RELIC_OP_EXISTS);
}

/*
 * The plan scans r, then three times over for each of r's tuples probes p,
 * a relation a quantum large, then scans the empty z, which rejects every
 * binding.  Each probe reads all of p: as an ANTI_MEMBER it asks whether p
 * holds a tuple whose first column is r's value and whose second is
 * anything -- the probe of `not p(X, _)` without an index -- which none
 * does; as an EXISTS whether p holds a tuple whose second column is the
 * last tuple's -- `p(_, last)` -- which only that tuple does.  A call may
 * read one probe's walk past its quantum and no more.
 */
static void
probes_scanning_count_toward_the_quantum(enum relic_op_kind kind)
{
	relic_op_state_t states[6];
	relic_plan_term_t probe_terms[2];
	relic_scratch_t scratch;
	relic_relation_t *empty;
	relic_relation_t *probed;
	relic_op_t ops[6];
	relic_plan_t plan;
	fixture_t fixture;
	relic_dom_t tuple[2];
	relic_dom_t batch[RELIC_INSERT_BATCH];
	enum relic_exec_status status;
	uint64_t read_before;
	uint64_t most_read;
	size_t outer;
	size_t walk;
	size_t i;
	bool added;

	if (!fixture_open(&fixture, SCENARIO_PROBES))
		return;
	tuple[1] = RELIC_ID_NONE;

	outer = 64;
	walk = RELIC_EXEC_QUANTUM;
	empty = relation(fixture.ctx, SCENARIO_PROBES, "z", 1);
	probed = relation(fixture.ctx, SCENARIO_PROBES, "p", 2);
	if (empty == NULL || probed == NULL ||
	    !fill(&fixture, SCENARIO_PROBES, 1, outer)) {
		fixture_close(&fixture);
		return;
	}
	for (i = 0; i < walk; i++) {
		if (relic_value_intern_integer(fixture.ctx,
		    (int64_t)(1000000 + i), &tuple[0]) != 0 ||
		    relic_value_intern_integer(fixture.ctx, (int64_t)i,
		    &tuple[1]) != 0 ||
		    relic_relation_insert(fixture.ctx, probed, tuple,
		    &added) != 0) {
			check(false, SCENARIO_PROBES, "p fills");
			fixture_close(&fixture);
			return;
		}
	}

	bzero(probe_terms, sizeof(probe_terms));
	if (kind == RELIC_OP_ANTI_MEMBER) {
		probe_terms[0] = fixture.read;
		probe_terms[1].action = RELIC_PLAN_ANY;
	} else {
		probe_terms[0].action = RELIC_PLAN_ANY;
		probe_terms[1].action = RELIC_PLAN_CONSTANT;
		probe_terms[1].u.constant = tuple[1];
	}

	bzero(ops, sizeof(ops));
	ops[0] = fixture.ops[0];
	for (i = 1; i <= 3; i++) {
		ops[i].kind = kind;
		ops[i].u.atom.relation = probed;
		ops[i].u.atom.terms = probe_terms;
		ops[i].u.atom.arity = 2;
		ops[i].u.atom.key_mask = 0;
		ops[i].u.atom.view = RELIC_VIEW_ALL;
	}
	ops[4] = fixture.ops[0];
	ops[4].u.atom.relation = empty;
	ops[4].u.atom.terms = &fixture.read;
	ops[5] = fixture.ops[1];
	plan.ops = ops;
	plan.op_count = 6;

	relic_scratch_init(&scratch, states, 6, batch, NULL, 1);
	relic_exec_start(&scratch, &fixture.env, &plan);

	most_read = 0;
	do {
		read_before = scratch.counters.tuples_scanned;
		status = relic_exec_run(&scratch);
		if (scratch.counters.tuples_scanned - read_before > most_read)
			most_read = scratch.counters.tuples_scanned -
			    read_before;
		scratch.batch_count = 0;
	} while (status == RELIC_EXEC_PAUSED);

	checkf(most_read <= 2 * RELIC_EXEC_QUANTUM + 1, SCENARIO_PROBES,
	    "a call of kind %d should read at most a walk past its quantum, "
	    "not %llu", (int)kind, (unsigned long long)most_read);
	checkf(scratch.counters.tuples_scanned == outer * (3 * walk + 1),
	    SCENARIO_PROBES, "the plan of kind %d should read %zu tuples, "
	    "not %llu", (int)kind, outer * (3 * walk + 1),
	    (unsigned long long)scratch.counters.tuples_scanned);

	fixture_close(&fixture);
}

/*
 * An EXISTS of a delta, keyed through an index, asks whether the delta
 * holds a tuple of the key, which is not whether the relation does.  p
 * holds (1, 10) and (2, 20) before its delta and (2, 21) in it, and the
 * plan scans r, which holds 1 and 2, checks `delta p(X, _)` and emits X.
 * Only 2 has a tuple in the delta, so only 2 is derived; a probe that began
 * at the head of the key's chain would find (1, 10), which lies below the
 * delta's end, and derive 1 as well.
 */
static void
a_check_of_a_delta_begins_at_its_start(void)
{
	static const int64_t rows[][2] = { { 1, 10 }, { 2, 20 }, { 2, 21 } };
	relic_tuple_range_t deltas[3];
	relic_op_state_t states[3];
	relic_plan_term_t check_terms[2];
	relic_scratch_t scratch;
	relic_relation_t *probed;
	relic_op_t ops[3];
	relic_plan_t plan;
	fixture_t fixture;
	relic_dom_t tuple[2];
	relic_dom_t batch[RELIC_INSERT_BATCH];
	relic_dom_t two;
	enum relic_exec_status status;
	uint32_t position;
	size_t derived;
	size_t i;
	bool added;

	if (!fixture_open(&fixture, SCENARIO_DELTA_CHECK))
		return;

	probed = relation(fixture.ctx, SCENARIO_DELTA_CHECK, "p", 2);
	if (probed == NULL || relic_relation_pred(probed) > 3 ||
	    !fill(&fixture, SCENARIO_DELTA_CHECK, 1, 2) ||
	    relic_relation_index(fixture.ctx, probed, UINT32_C(1),
	    &position) != 0) {
		check(false, SCENARIO_DELTA_CHECK, "p and its index exist");
		fixture_close(&fixture);
		return;
	}
	for (i = 0; i < 3; i++) {
		if (relic_value_intern_integer(fixture.ctx, rows[i][0],
		    &tuple[0]) != 0 ||
		    relic_value_intern_integer(fixture.ctx, rows[i][1],
		    &tuple[1]) != 0 ||
		    relic_relation_insert(fixture.ctx, probed, tuple,
		    &added) != 0) {
			check(false, SCENARIO_DELTA_CHECK, "p fills");
			fixture_close(&fixture);
			return;
		}
	}

	/* The delta is p's last tuple, and its index learns where it starts. */
	bzero(deltas, sizeof(deltas));
	deltas[relic_relation_pred(probed) - 1].first = RELIC_TUPLE_FIRST + 2;
	deltas[relic_relation_pred(probed) - 1].end = RELIC_TUPLE_FIRST + 3;
	relic_index_raise_since(&probed->indexes[position],
	    RELIC_TUPLE_FIRST + 2);
	fixture.env.deltas = deltas;

	bzero(check_terms, sizeof(check_terms));
	check_terms[0] = fixture.read;
	check_terms[1].action = RELIC_PLAN_ANY;

	bzero(ops, sizeof(ops));
	ops[0] = fixture.ops[0];
	ops[1].kind = RELIC_OP_EXISTS;
	ops[1].u.atom.relation = probed;
	ops[1].u.atom.terms = check_terms;
	ops[1].u.atom.arity = 2;
	ops[1].u.atom.key_mask = UINT32_C(1);
	ops[1].u.atom.index_position = position;
	ops[1].u.atom.view = RELIC_VIEW_DELTA;
	ops[2] = fixture.ops[1];
	plan.ops = ops;
	plan.op_count = 3;

	relic_scratch_init(&scratch, states, 3, batch, NULL, 1);
	relic_exec_start(&scratch, &fixture.env, &plan);

	derived = 0;
	two = RELIC_ID_NONE;
	(void)relic_value_intern_integer(fixture.ctx, 2, &two);
	do {
		status = relic_exec_run(&scratch);
		for (i = 0; i < scratch.batch_count; i++) {
			checkf(batch[i] == two, SCENARIO_DELTA_CHECK,
			    "derivation %zu should be 2", derived + 1);
			derived++;
		}
		scratch.batch_count = 0;
	} while (status == RELIC_EXEC_PAUSED);

	checkf(derived == 1, SCENARIO_DELTA_CHECK,
	    "one value has a tuple in the delta, and %zu were derived",
	    derived);

	fixture_close(&fixture);
}

/*
 * A fold that spends the call's reads and matches pauses the call at the
 * operation after it, its value bound in the frame, and the next call
 * resumes there.  The plan scans r, counts q -- a quantum and more -- and
 * emits the count, so every call that folds pauses before its EMIT.  Each
 * of r's tuples must derive the count once: a call that resumed at the
 * fold instead would ask it for a next match, which it has not, and derive
 * nothing.  The count is interned beforehand so that the EMIT writes its
 * identifier rather than a pending value.
 */
static void
a_fold_that_pauses_emits_its_value_next_call(void)
{
	relic_op_state_t states[3];
	relic_scratch_t scratch;
	relic_plan_term_t count_term;
	relic_op_t ops[3];
	relic_plan_t plan;
	fixture_t fixture;
	relic_dom_t count;
	relic_dom_t value;
	relic_dom_t batch[RELIC_INSERT_BATCH];
	enum relic_exec_status status;
	size_t paused_before_emit;
	size_t derived;
	size_t wrong;
	size_t outer;
	size_t group;
	size_t i;
	bool added;

	if (!fixture_open(&fixture, SCENARIO_FOLD_EMIT))
		return;

	outer = 16;
	group = RELIC_EXEC_QUANTUM + 5;
	if (!fill(&fixture, SCENARIO_FOLD_EMIT, 1, outer) ||
	    relic_value_intern_integer(fixture.ctx, (int64_t)group,
	    &count) != 0) {
		check(false, SCENARIO_FOLD_EMIT, "r fills");
		fixture_close(&fixture);
		return;
	}
	for (i = 0; i < group; i++) {
		if (relic_value_intern_integer(fixture.ctx,
		    (int64_t)(1000000 + i), &value) != 0 ||
		    relic_relation_insert(fixture.ctx, fixture.emitted, &value,
		    &added) != 0) {
			check(false, SCENARIO_FOLD_EMIT, "q fills");
			fixture_close(&fixture);
			return;
		}
	}

	bzero(ops, sizeof(ops));
	ops[0] = fixture.ops[0];
	ops[1].kind = RELIC_OP_AGGREGATE;
	ops[1].u.aggregate.relation = fixture.emitted;
	ops[1].u.aggregate.access = RELIC_FOLD_SCAN;
	ops[1].u.aggregate.fn = RELIC_AGG_COUNT;
	ops[1].u.aggregate.arity = 1;
	ops[1].u.aggregate.kind = RELIC_SLOT_NUMBER;
	ops[1].u.aggregate.slot = 1;
	ops[1].u.aggregate.binds = true;
	count_term.action = RELIC_PLAN_NUMBER;
	count_term.u.slot = 1;
	ops[2] = fixture.ops[1];
	ops[2].u.emit.terms = &count_term;
	plan.ops = ops;
	plan.op_count = 3;

	relic_scratch_init(&scratch, states, 3, batch, NULL, 1);
	relic_exec_start(&scratch, &fixture.env, &plan);

	paused_before_emit = 0;
	derived = 0;
	wrong = 0;
	do {
		status = relic_exec_run(&scratch);
		if (status == RELIC_EXEC_PAUSED && scratch.level == 2)
			paused_before_emit++;
		for (i = 0; i < scratch.batch_count; i++) {
			if (batch[i] == count)
				derived++;
			else
				wrong++;
		}
		scratch.batch_count = 0;
	} while (status == RELIC_EXEC_PAUSED);

	checkf(paused_before_emit == outer, SCENARIO_FOLD_EMIT, "each of %zu "
	    "folds should pause the call before its EMIT, not %zu", outer,
	    paused_before_emit);
	checkf(derived == outer && wrong == 0, SCENARIO_FOLD_EMIT, "each of "
	    "r's %zu tuples should derive the count, not %zu and %zu others",
	    outer, derived, wrong);

	fixture_close(&fixture);
}

/*
 * A lookup counts the chain tuples it rejects as a scan counts the tuples
 * it does, and one that spends the call's reads pauses inside its chain,
 * where the next call resumes it.  b holds, for each of r's values k, a
 * chain of three quanta of pairs (k, v) with v never k, and then (k, k);
 * the plan scans r, looks b up by its first column asking that the second
 * equal it too, and emits k.  Every pair but the last of each chain is
 * rejected, so each chain takes several calls that derive nothing, and
 * each value of r is derived once.
 */
static void
a_lookup_rejecting_its_chain_pauses_inside_it(void)
{
	relic_op_state_t states[3];
	relic_scratch_t scratch;
	relic_relation_t *pairs;
	relic_plan_term_t lookup_terms[2];
	relic_op_t ops[3];
	relic_plan_t plan;
	fixture_t fixture;
	relic_dom_t pair[2];
	relic_dom_t batch[RELIC_INSERT_BATCH];
	enum relic_exec_status status;
	uint64_t read_before;
	uint64_t most_read;
	uint32_t position;
	size_t empty_pauses;
	size_t derived;
	size_t outer;
	size_t chain;
	size_t k;
	size_t i;
	bool added;

	if (!fixture_open(&fixture, SCENARIO_LOOKUP))
		return;

	outer = 3;
	chain = 3 * RELIC_EXEC_QUANTUM;
	pairs = relation(fixture.ctx, SCENARIO_LOOKUP, "b", 2);
	if (pairs == NULL || !fill(&fixture, SCENARIO_LOOKUP, 1, outer)) {
		fixture_close(&fixture);
		return;
	}
	for (k = 1; k <= outer; k++) {
		for (i = 0; i <= chain; i++) {
			if (relic_value_intern_integer(fixture.ctx, (int64_t)k,
			    &pair[0]) != 0 ||
			    relic_value_intern_integer(fixture.ctx,
			    i < chain ? (int64_t)(1000000 + i) : (int64_t)k,
			    &pair[1]) != 0 ||
			    relic_relation_insert(fixture.ctx, pairs, pair,
			    &added) != 0) {
				check(false, SCENARIO_LOOKUP, "b fills");
				fixture_close(&fixture);
				return;
			}
		}
	}
	if (relic_relation_index(fixture.ctx, pairs, 1u, &position) != 0) {
		check(false, SCENARIO_LOOKUP, "b is indexed on its first column");
		fixture_close(&fixture);
		return;
	}

	lookup_terms[0] = fixture.read;
	lookup_terms[1] = fixture.read;
	ops[0] = fixture.ops[0];
	ops[1] = fixture.ops[0];
	ops[1].kind = RELIC_OP_LOOKUP;
	ops[1].u.atom.relation = pairs;
	ops[1].u.atom.terms = lookup_terms;
	ops[1].u.atom.arity = 2;
	ops[1].u.atom.key_mask = 1u;
	ops[1].u.atom.index_position = position;
	ops[2] = fixture.ops[1];
	plan.ops = ops;
	plan.op_count = 3;

	relic_scratch_init(&scratch, states, 3, batch, NULL, 1);
	relic_exec_start(&scratch, &fixture.env, &plan);

	empty_pauses = 0;
	derived = 0;
	most_read = 0;
	do {
		read_before = scratch.counters.tuples_looked_up;
		status = relic_exec_run(&scratch);
		if (scratch.counters.tuples_looked_up - read_before > most_read)
			most_read = scratch.counters.tuples_looked_up -
			    read_before;

		if (status == RELIC_EXEC_PAUSED && scratch.batch_count == 0)
			empty_pauses++;
		derived += scratch.batch_count;
		scratch.batch_count = 0;
	} while (status == RELIC_EXEC_PAUSED);

	checkf(empty_pauses >= outer * 2, SCENARIO_LOOKUP, "chains of %zu "
	    "rejected pairs should pause at least %zu times with nothing "
	    "derived, not %zu", chain, outer * 2, empty_pauses);
	checkf(most_read <= 2 * RELIC_EXEC_QUANTUM, SCENARIO_LOOKUP, "a call "
	    "should look up at most two quanta of %u tuples, not %llu",
	    RELIC_EXEC_QUANTUM, (unsigned long long)most_read);
	checkf(derived == outer, SCENARIO_LOOKUP, "each of r's %zu values "
	    "should be derived once, not %zu in all", outer, derived);
	checkf(scratch.counters.tuples_looked_up == outer * (chain + 1),
	    SCENARIO_LOOKUP, "the plan should look up %zu tuples, not %llu",
	    outer * (chain + 1),
	    (unsigned long long)scratch.counters.tuples_looked_up);

	fixture_close(&fixture);
}

/*
 * Run a started plan to its end, appending what it derives to `out`, which
 * has room for `room` values, and counting them in *countp.  Returns
 * whether every pause left the join at or above level `base`.
 */
static bool
run_collecting(relic_scratch_t *scratch, relic_dom_t *out, size_t room,
    size_t *countp, size_t base)
{
	size_t i;
	enum relic_exec_status status;
	bool above;

	above = true;
	do {
		status = relic_exec_run(scratch);
		if (status == RELIC_EXEC_PAUSED && scratch->level < base)
			above = false;
		for (i = 0; i < scratch->batch_count; i++) {
			if (*countp < room)
				out[*countp] = scratch->batch[i];
			(*countp)++;
		}
		scratch->batch_count = 0;
	} while (status == RELIC_EXEC_PAUSED);

	return (above);
}

/*
 * A plan's prefix runs alone, and its driver's visits run as slices that
 * each start at the driver, so slices that cover the visits in order
 * derive what the whole plan does, in the same order, and read the visits
 * and nothing of the prefix.  The plan checks that r holds its last value,
 * reading all of r, looks b up on the constant 7, and emits each pair's
 * second column; b's chain of 7 is interleaved with a chain of 8, so its
 * identifiers are not consecutive.  The whole run counts r's reads as the
 * prefix's and the chain's as the driver's visits; the prefix alone
 * reaches the driver, or with a value r lacks rejects the plan, reading
 * all of r either way; and slices cut at the chain's ordinals 0, 1, a
 * batch and one, and its end, each a chain tuple to the next slice's
 * first, never backtrack below the driver, though each but the shortest
 * fills a batch.
 */
static void
a_lookups_slices_derive_what_the_whole_does(void)
{
	enum { HELD = 10, CHAIN = 2 * RELIC_INSERT_BATCH + 7, SLICES = 3 };
	static relic_tuple_id_t chain[CHAIN];
	static relic_dom_t whole[CHAIN];
	static relic_dom_t sliced[CHAIN];
	static const size_t cuts[SLICES + 1] = {
		0, 1, RELIC_INSERT_BATCH + 1, CHAIN
	};
	enum relic_prefix_status prefix;
	relic_plan_term_t lookup_terms[2];
	relic_plan_term_t test_term;
	relic_tuple_range_t slice;
	relic_tuple_range_t view;
	relic_op_state_t states[3];
	relic_relation_t *pairs;
	relic_scratch_t scratch;
	relic_driver_reads_t reads;
	const relic_index_t *index;
	relic_dom_t batch[RELIC_INSERT_BATCH];
	relic_dom_t pair[2];
	relic_dom_t probe[2];
	relic_dom_t absent;
	relic_dom_t last;
	relic_op_t ops[3];
	relic_plan_t plan;
	fixture_t fixture;
	relic_tuple_id_t id;
	uint64_t looked_up;
	uint32_t position;
	size_t derived;
	size_t i;
	size_t k;
	bool above;
	bool added;

	if (!fixture_open(&fixture, SCENARIO_SLICES))
		return;
	pairs = relation(fixture.ctx, SCENARIO_SLICES, "b", 2);
	if (pairs == NULL || !fill(&fixture, SCENARIO_SLICES, 1, HELD) ||
	    relic_value_intern_integer(fixture.ctx, 99999, &absent) != 0) {
		fixture_close(&fixture);
		return;
	}
	for (i = 0; i < 2 * (size_t)CHAIN; i++) {
		if (relic_value_intern_integer(fixture.ctx,
		    7 + (int64_t)(i % 2), &pair[0]) != 0 ||
		    relic_value_intern_integer(fixture.ctx,
		    (int64_t)(1000000 + i), &pair[1]) != 0 ||
		    relic_relation_insert(fixture.ctx, pairs, pair,
		    &added) != 0) {
			check(false, SCENARIO_SLICES, "b fills");
			fixture_close(&fixture);
			return;
		}
	}
	if (relic_relation_index(fixture.ctx, pairs, 1u, &position) != 0 ||
	    relic_value_intern_integer(fixture.ctx, 7, &probe[0]) != 0 ||
	    relic_value_intern_integer(fixture.ctx, HELD, &last) != 0) {
		check(false, SCENARIO_SLICES, "b is indexed");
		fixture_close(&fixture);
		return;
	}

	test_term.action = RELIC_PLAN_CONSTANT;
	test_term.u.constant = last;
	lookup_terms[0].action = RELIC_PLAN_CONSTANT;
	lookup_terms[0].u.constant = probe[0];
	lookup_terms[1] = fixture.bind;
	bzero(ops, sizeof(ops));
	ops[0].kind = RELIC_OP_EXISTS;
	ops[0].u.atom.relation = fixture.scanned;
	ops[0].u.atom.terms = &test_term;
	ops[0].u.atom.arity = 1;
	ops[0].u.atom.view = RELIC_VIEW_ALL;
	ops[1].kind = RELIC_OP_LOOKUP;
	ops[1].u.atom.relation = pairs;
	ops[1].u.atom.terms = lookup_terms;
	ops[1].u.atom.arity = 2;
	ops[1].u.atom.key_mask = 1u;
	ops[1].u.atom.index_position = position;
	ops[1].u.atom.view = RELIC_VIEW_ALL;
	ops[2] = fixture.ops[1];
	plan.ops = ops;
	plan.op_count = 3;
	check(relic_plan_driver(&plan) == 1, SCENARIO_SLICES,
	    "the lookup after the test drives the plan");

	relic_scratch_init(&scratch, states, 3, batch, NULL, 1);
	relic_exec_start(&scratch, &fixture.env, &plan);
	derived = 0;
	(void)run_collecting(&scratch, whole, CHAIN, &derived, 0);
	checkf(derived == CHAIN && scratch.driver_reads.prefix == HELD &&
	    scratch.driver_reads.visits == CHAIN, SCENARIO_SLICES,
	    "the whole plan should derive %u pairs, reading %u in its "
	    "prefix and %u at its driver, not %zu, %llu and %llu", CHAIN,
	    HELD, CHAIN, derived,
	    (unsigned long long)scratch.driver_reads.prefix,
	    (unsigned long long)scratch.driver_reads.visits);
	bzero(&scratch.counters, sizeof(scratch.counters));

	relic_exec_start_prefix(&scratch, &fixture.env, &plan);
	while ((prefix = relic_exec_run_prefix(&scratch)) ==
	    RELIC_PREFIX_PAUSED)
		continue;
	check(prefix == RELIC_PREFIX_REACHED &&
	    scratch.counters.tuples_scanned == HELD &&
	    scratch.counters.tuples_looked_up == 0 && scratch.batch_count == 0,
	    SCENARIO_SLICES, "the prefix reaches the driver, reading r alone");
	bzero(&scratch.counters, sizeof(scratch.counters));

	test_term.u.constant = absent;
	relic_exec_start_prefix(&scratch, &fixture.env, &plan);
	while ((prefix = relic_exec_run_prefix(&scratch)) ==
	    RELIC_PREFIX_PAUSED)
		continue;
	check(prefix == RELIC_PREFIX_REJECTED &&
	    scratch.counters.tuples_scanned == HELD, SCENARIO_SLICES,
	    "a prefix whose test fails rejects the plan");
	test_term.u.constant = last;
	bzero(&scratch.counters, sizeof(scratch.counters));

	index = &pairs->indexes[position];
	id = relic_index_first(index, pairs, probe);
	for (i = 0; i < CHAIN && id != RELIC_ID_NONE; i++) {
		chain[i] = id;
		id = relic_index_next(index, id);
	}
	view = relic_exec_view(&fixture.env, &ops[1].u.atom);

	derived = 0;
	above = true;
	bzero(&reads, sizeof(reads));
	for (k = 0; k < SLICES; k++) {
		slice.first = chain[cuts[k]];
		slice.end = cuts[k + 1] < CHAIN ? chain[cuts[k + 1]] : view.end;
		relic_exec_start_slice(&scratch, &fixture.env, &plan, slice);
		if (!run_collecting(&scratch, sliced, CHAIN, &derived, 1))
			above = false;
		reads.prefix += scratch.driver_reads.prefix;
		reads.visits += scratch.driver_reads.visits;
	}
	looked_up = scratch.counters.tuples_looked_up;

	checkf(derived == CHAIN && bcmp(whole, sliced, sizeof(whole)) == 0,
	    SCENARIO_SLICES, "the slices should derive the whole's %u pairs "
	    "in order, and derived %zu", CHAIN, derived);
	check(above, SCENARIO_SLICES, "no slice pauses below its driver");
	checkf(scratch.counters.tuples_scanned == 0 && looked_up == CHAIN &&
	    reads.prefix == 0 && reads.visits == CHAIN, SCENARIO_SLICES,
	    "the slices should read the %u visits and no prefix, and read "
	    "%llu tuples of r and %llu of b", CHAIN,
	    (unsigned long long)scratch.counters.tuples_scanned,
	    (unsigned long long)looked_up);

	fixture_close(&fixture);
}

/*
 * A test of the prefix that reads past its quantum and holds pauses as the
 * join descends past it, and the join reaches the driver all the same.
 * The plan checks `p(_, last)` twice without an index, each check reading
 * all of p, a quantum large and more, whose last tuple alone holds; then
 * scans r and emits each value.  So the first check pauses on the way to
 * the second and the second on the way to the driver.  The whole run
 * counts both checks' reads as the prefix's and r's as the driver's
 * visits, though its caller takes the counters after every call, as the
 * evaluator does; and the prefix alone reaches the driver within a call of
 * each pause, deriving nothing.  Checking for a value p lacks instead, the
 * first check rejects in the call it spends, and the prefix alone is
 * finished there, having read p once.
 */
static void
a_prefix_pausing_at_its_driver_reaches_it(void)
{
	enum { HELD = 10, WALK = RELIC_EXEC_QUANTUM + 10, CALLS = 4 };
	enum relic_prefix_status prefix;
	enum relic_exec_status status;
	relic_plan_term_t probe_terms[2];
	relic_op_state_t states[4];
	relic_relation_t *probed;
	relic_scratch_t scratch;
	relic_dom_t batch[RELIC_INSERT_BATCH];
	relic_dom_t tuple[2];
	relic_dom_t absent;
	relic_op_t ops[4];
	relic_plan_t plan;
	fixture_t fixture;
	uint64_t read;
	size_t count;
	size_t calls;
	size_t i;
	bool added;

	if (!fixture_open(&fixture, SCENARIO_PREFIX_PAUSE))
		return;
	probed = relation(fixture.ctx, SCENARIO_PREFIX_PAUSE, "p", 2);
	if (probed == NULL ||
	    !fill(&fixture, SCENARIO_PREFIX_PAUSE, 1, HELD) ||
	    relic_value_intern_integer(fixture.ctx, -1, &absent) != 0) {
		fixture_close(&fixture);
		return;
	}
	for (i = 0; i < WALK; i++) {
		if (relic_value_intern_integer(fixture.ctx,
		    (int64_t)(1000000 + i), &tuple[0]) != 0 ||
		    relic_value_intern_integer(fixture.ctx, (int64_t)i,
		    &tuple[1]) != 0 ||
		    relic_relation_insert(fixture.ctx, probed, tuple,
		    &added) != 0) {
			check(false, SCENARIO_PREFIX_PAUSE, "p fills");
			fixture_close(&fixture);
			return;
		}
	}

	bzero(probe_terms, sizeof(probe_terms));
	probe_terms[0].action = RELIC_PLAN_ANY;
	probe_terms[1].action = RELIC_PLAN_CONSTANT;
	probe_terms[1].u.constant = tuple[1];
	bzero(ops, sizeof(ops));
	ops[0].kind = RELIC_OP_EXISTS;
	ops[0].u.atom.relation = probed;
	ops[0].u.atom.terms = probe_terms;
	ops[0].u.atom.arity = 2;
	ops[0].u.atom.view = RELIC_VIEW_ALL;
	ops[1] = ops[0];
	ops[2] = fixture.ops[0];
	ops[3] = fixture.ops[1];
	plan.ops = ops;
	plan.op_count = 4;
	check(relic_plan_driver(&plan) == 2, SCENARIO_PREFIX_PAUSE,
	    "the scan after the tests drives the plan");

	relic_scratch_init(&scratch, states, 4, batch, NULL, 1);
	relic_exec_start(&scratch, &fixture.env, &plan);
	count = 0;
	do {
		status = relic_exec_run(&scratch);
		count += scratch.batch_count;
		scratch.batch_count = 0;
		bzero(&scratch.counters, sizeof(scratch.counters));
	} while (status == RELIC_EXEC_PAUSED);
	checkf(count == HELD && scratch.driver_reads.prefix == 2 * WALK &&
	    scratch.driver_reads.visits == HELD, SCENARIO_PREFIX_PAUSE,
	    "the whole plan should derive %u values, reading %u in its "
	    "prefix and %u at its driver, not %zu, %llu and %llu", HELD,
	    2 * WALK, HELD, count,
	    (unsigned long long)scratch.driver_reads.prefix,
	    (unsigned long long)scratch.driver_reads.visits);

	relic_exec_start_prefix(&scratch, &fixture.env, &plan);
	calls = 0;
	read = 0;
	do {
		prefix = relic_exec_run_prefix(&scratch);
		read += scratch.counters.tuples_scanned;
		bzero(&scratch.counters, sizeof(scratch.counters));
		calls++;
	} while (prefix == RELIC_PREFIX_PAUSED && calls < CALLS);
	checkf(prefix == RELIC_PREFIX_REACHED && scratch.batch_count == 0 &&
	    read == 2 * WALK && scratch.driver_reads.prefix == read,
	    SCENARIO_PREFIX_PAUSE,
	    "the prefix should reach the driver, reading p twice and "
	    "counting it, and returned %d after %zu calls with %zu derived, "
	    "%llu read and %llu counted", (int)prefix, calls,
	    scratch.batch_count, (unsigned long long)read,
	    (unsigned long long)scratch.driver_reads.prefix);

	probe_terms[1].u.constant = absent;
	relic_exec_start_prefix(&scratch, &fixture.env, &plan);
	prefix = relic_exec_run_prefix(&scratch);
	read = scratch.counters.tuples_scanned;
	bzero(&scratch.counters, sizeof(scratch.counters));
	checkf(prefix == RELIC_PREFIX_REJECTED && read == WALK &&
	    scratch.driver_reads.prefix == WALK, SCENARIO_PREFIX_PAUSE,
	    "a check that spends the call and fails should reject in that "
	    "call, reading p once, and returned %d having read %llu and "
	    "counted %llu", (int)prefix, (unsigned long long)read,
	    (unsigned long long)scratch.driver_reads.prefix);

	fixture_close(&fixture);
}

/*
 * A LOOKUP driver's visits are the chain tuples inside its view, however
 * the chain grows while the run is paused, and are counted as of every
 * return, not only the last.  The plan looks b up on the constant 7 and
 * emits each pair's second column, so every chain tuple matches and the
 * visits at a return are the pairs derived by then.  The chain's
 * second-to-last tuple fills the second batch, so the run pauses with the
 * driver's cursor on the chain's last tuple, and the caller then appends a
 * tuple of key 7 to b, linked from that last tuple but past the view, which
 * the driver never reads, before the next call finishes the run.
 */
static void
a_lookup_drivers_visits_end_at_its_view(void)
{
	enum { CHAIN = 2 * RELIC_INSERT_BATCH + 1 };
	enum relic_exec_status status;
	relic_plan_term_t lookup_terms[2];
	relic_op_state_t states[2];
	relic_relation_t *pairs;
	relic_scratch_t scratch;
	relic_dom_t batch[RELIC_INSERT_BATCH];
	relic_dom_t pair[2];
	relic_op_t ops[2];
	relic_plan_t plan;
	fixture_t fixture;
	uint32_t position;
	size_t derived;
	size_t i;
	bool counted;
	bool added;

	if (!fixture_open(&fixture, SCENARIO_GROWN_CHAIN))
		return;
	pairs = relation(fixture.ctx, SCENARIO_GROWN_CHAIN, "b", 2);
	if (pairs == NULL ||
	    relic_value_intern_integer(fixture.ctx, 7, &pair[0]) != 0) {
		fixture_close(&fixture);
		return;
	}
	for (i = 0; i < CHAIN; i++) {
		if (relic_value_intern_integer(fixture.ctx,
		    (int64_t)(1000000 + i), &pair[1]) != 0 ||
		    relic_relation_insert(fixture.ctx, pairs, pair,
		    &added) != 0) {
			check(false, SCENARIO_GROWN_CHAIN, "b fills");
			fixture_close(&fixture);
			return;
		}
	}
	if (relic_relation_index(fixture.ctx, pairs, 1u, &position) != 0) {
		check(false, SCENARIO_GROWN_CHAIN, "b is indexed");
		fixture_close(&fixture);
		return;
	}

	lookup_terms[0].action = RELIC_PLAN_CONSTANT;
	lookup_terms[0].u.constant = pair[0];
	lookup_terms[1] = fixture.bind;
	bzero(ops, sizeof(ops));
	ops[0].kind = RELIC_OP_LOOKUP;
	ops[0].u.atom.relation = pairs;
	ops[0].u.atom.terms = lookup_terms;
	ops[0].u.atom.arity = 2;
	ops[0].u.atom.key_mask = 1u;
	ops[0].u.atom.index_position = position;
	ops[0].u.atom.view = RELIC_VIEW_ALL;
	ops[1] = fixture.ops[1];
	plan.ops = ops;
	plan.op_count = 2;

	relic_scratch_init(&scratch, states, 2, batch, NULL, 1);
	relic_exec_start(&scratch, &fixture.env, &plan);
	derived = 0;
	counted = true;
	do {
		status = relic_exec_run(&scratch);
		derived += scratch.batch_count;
		scratch.batch_count = 0;
		if (scratch.driver_reads.visits != derived)
			counted = false;
		if (derived != 2 * RELIC_INSERT_BATCH ||
		    status != RELIC_EXEC_PAUSED)
			continue;
		if (relic_value_intern_integer(fixture.ctx,
		    (int64_t)(1000000 + CHAIN), &pair[1]) != 0 ||
		    relic_relation_insert(fixture.ctx, pairs, pair,
		    &added) != 0) {
			check(false, SCENARIO_GROWN_CHAIN, "b grows");
			fixture_close(&fixture);
			return;
		}
	} while (status == RELIC_EXEC_PAUSED);

	checkf(derived == CHAIN && scratch.driver_reads.visits == CHAIN,
	    SCENARIO_GROWN_CHAIN, "the run should derive and visit the %u "
	    "pairs of its view, not %zu and %llu", CHAIN, derived,
	    (unsigned long long)scratch.driver_reads.visits);
	check(counted, SCENARIO_GROWN_CHAIN,
	    "every return counts the visits the run has made");

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
	pieces_of_a_view_derive_what_the_whole_does();
	a_plan_deriving_nothing_pauses_every_quantum();
	a_folds_reads_count_toward_the_quantum();
	a_fold_that_pauses_emits_its_value_next_call();
	a_probes_scan_counts_toward_the_quantum();
	a_check_of_a_delta_begins_at_its_start();
	a_lookup_rejecting_its_chain_pauses_inside_it();
	a_lookups_slices_derive_what_the_whole_does();
	a_prefix_pausing_at_its_driver_reaches_it();
	a_lookup_drivers_visits_end_at_its_view();

	return (check_summary("test_exec"));
}
