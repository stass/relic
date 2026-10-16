/*
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO	"dense collection preserves serial publication"
#define	NODES	32u
#define	TASKS	16u
#define	OLD_ROWS	(NODES * NODES / 2)
#define	NEW_ROWS	132u

typedef struct dense_fixture dense_fixture_t;
struct dense_fixture {
	relic_t			*ctx;
	relic_pool_t		*pool;
	relic_relation_t	*target;
	relic_exec_env_t	 env;
	relic_round_t		 round;
	relic_round_item_t	 item;
	relic_round_job_t	 job;
	relic_dense_t		 dense;
	relic_plan_t		 plan;
	relic_op_t		 emit;
	relic_plan_term_t	 terms[2];
	relic_dom_t		 values[NODES + 2];
};

static void	 derive_part(void *argument, size_t task, size_t worker);
static void	 expected_tuple(const dense_fixture_t *fixture, size_t position,
		    relic_dom_t *tuple);
static void	 fixture_close(dense_fixture_t *fixture);
static bool	 fixture_open(dense_fixture_t *fixture, size_t workers);
static void	 malformed_and_ineligible(void);
static void	 an_old_only_round_needs_no_order_array(void);
static void	 staging_refusal_releases_room(void);
static void	 several_tasks_win(void);
static void	 offer_distinct_task(dense_fixture_t *fixture, size_t task,
		    size_t worker);
static void	 ordered_publication(bool parallel);
static void	 refusal_releases_room(bool interrupt, bool during_order);

/* The first half of a square is old; mixed typed values share pool bytes. */
static bool
fixture_open(dense_fixture_t *fixture, size_t workers)
{
	char text[16384];
	relic_pred_id_t predicate;
	size_t length = 0;
	size_t i, j;
	uint32_t index;

	bzero(fixture, sizeof(*fixture));
	fixture->ctx = relic_create();
	if (fixture->ctx == NULL)
		return (false);
	for (i = 0; i < NODES / 2; i++) {
		for (j = 0; j < NODES; j++)
			length += (size_t)snprintf(text + length, sizeof(text) - length,
			    "p(%zu,%zu).\n", i, j);
	}
	snprintf(text + length, sizeof(text) - length, "v(shared). v(\"shared\").");
	if (relic_load_string(fixture->ctx, "dense.dl", text) != 0 ||
	    relic_pred_intern(fixture->ctx, "p", 1, 2, NULL, &predicate) != 0)
		return (false);
	fixture->target = relic_relation_read(fixture->ctx, predicate);
	if (fixture->target == NULL || relic_relation_index(fixture->ctx,
	    fixture->target, 1, &index) != 0 || relic_pool_create(&fixture->pool,
	    workers, fixture->ctx->hash_seed) != 0)
		return (false);
	fixture->pool->tasks = calloc(TASKS, sizeof(*fixture->pool->tasks));
	if (fixture->pool->tasks == NULL)
		return (false);
	fixture->pool->task_count = TASKS;
	fixture->pool->task_capacity = TASKS;
	for (i = 0; i < NODES; i++)
		fixture->values[i] = relic_value_find_integer(fixture->ctx, (int64_t)i);
	fixture->values[NODES] = relic_value_find_string(fixture->ctx, "shared", 6);
	fixture->values[NODES + 1] = RELIC_DOM_SYMBOL |
	    (fixture->values[NODES] & RELIC_DOM_INDEX_MASK);
	fixture->env.ctx = fixture->ctx;
	fixture->terms[0].action = RELIC_PLAN_BOUND;
	fixture->terms[1].action = RELIC_PLAN_BOUND;
	fixture->terms[1].u.slot = 1;
	fixture->emit.kind = RELIC_OP_EMIT;
	fixture->emit.u.emit.arity = 2;
	fixture->emit.u.emit.terms = fixture->terms;
	fixture->plan.ops = &fixture->emit;
	fixture->plan.op_count = 1;
	fixture->item.plan = &fixture->plan;
	fixture->round.items = &fixture->item;
	fixture->round.item_count = 1;
	fixture->round.targets = &fixture->target;
	fixture->round.target_count = 1;
	fixture->round.env = &fixture->env;
	fixture->job.round = &fixture->round;
	fixture->job.tasks = fixture->pool->tasks;
	fixture->job.workers = fixture->pool->workers;
	fixture->job.account = &fixture->ctx->account;
	fixture->job.interrupted = &fixture->ctx->interrupted;
	fixture->job.admissible = SIZE_MAX;
	relic_atomic_bool_init(&fixture->job.abandoned, false);
	return (true);
}

static void
fixture_close(dense_fixture_t *fixture)
{
	relic_dense_close(&fixture->dense);
	if (fixture->ctx != NULL)
		check(relic_atomic_size_load(&fixture->ctx->account.round_bytes) == 0,
		    SCENARIO, "every private charge is released");
	relic_pool_free(fixture->pool);
	relic_destroy(fixture->ctx);
}

static void
expected_tuple(const dense_fixture_t *fixture, size_t position, relic_dom_t *tuple)
{
	if (position < 128) {
		tuple[0] = fixture->values[16 + position / NODES];
		tuple[1] = fixture->values[position % NODES];
	} else {
		position -= 128;
		tuple[0] = fixture->values[NODES + position / 2];
		tuple[1] = fixture->values[position % 2 == 0 ? 0 : NODES];
	}
}

/* Each task offers the same new set, in a different order, with duplicates. */
static void
derive_part(void *argument, size_t task, size_t worker)
{
	dense_fixture_t *fixture = argument;
	relic_dom_t tuple[2];
	uint32_t *sequence = &fixture->pool->workers[worker].dense_sequence;
	size_t i;

	fixture->pool->tasks[task].worker = worker;
	for (i = 0; i < NEW_ROWS * 4; i++) {
		expected_tuple(fixture, (i + task * 17) % NEW_ROWS, tuple);
		if (!relic_dense_take(&fixture->dense, task, sequence, tuple)) {
			relic_atomic_bool_store_relaxed(&fixture->job.abandoned, true);
			return;
		}
	}
	tuple[0] = fixture->values[0];
	tuple[1] = fixture->values[1];
	if (!relic_dense_take(&fixture->dense, task, sequence, tuple))
		relic_atomic_bool_store_relaxed(&fixture->job.abandoned, true);
}

static void
ordered_publication(bool parallel)
{
	dense_fixture_t fixture;
	relic_dom_t expected[2];
	const relic_dom_t *actual;
	size_t i;
	bool merged;

	if (!fixture_open(&fixture, 4)) {
		check(false, SCENARIO, "the fixture is built");
		fixture_close(&fixture);
		return;
	}
	if (!parallel)
		fixture.pool->task_count = fixture.pool->worker_count;
	check(relic_dense_open(&fixture.dense, fixture.pool, &fixture.job) == 1,
	    SCENARIO, "the real byte choice selects dense collection");
	if (fixture.dense.ranks == NULL) {
		fixture_close(&fixture);
		return;
	}
	if (parallel)
		relic_pool_for(fixture.pool, TASKS, derive_part, &fixture);
	else {
		/* One task per worker, later tasks finish before the earliest. */
		for (i = fixture.pool->task_count; i > 0; i--)
			derive_part(&fixture, i - 1, i - 1);
	}
	check(!relic_atomic_bool_load_relaxed(&fixture.job.abandoned),
	    SCENARIO, "collection succeeds");
	merged = relic_round_merge(fixture.ctx, fixture.pool, &fixture.round,
	    0, &fixture.dense);
	check(merged, SCENARIO, "the actual merge publishes without serial retry");
	check(relic_relation_count(fixture.target) == OLD_ROWS + NEW_ROWS,
	    SCENARIO, "only distinct new tuples are published");
	for (i = 0; i < NEW_ROWS && merged; i++) {
		expected_tuple(&fixture, i, expected);
		actual = relic_relation_tuple(fixture.target,
		    (relic_tuple_id_t)(OLD_ROWS + i + 1));
		check(actual != NULL && actual[0] == expected[0] &&
		    actual[1] == expected[1], SCENARIO,
		    "tuple identifiers follow the earliest task's order");
		check(relic_relation_find(fixture.target, expected) == OLD_ROWS + i + 1,
		    SCENARIO, "canonical membership uses the published identifier");
	}
	check(fixture.pool->tasks[0].inserted == NEW_ROWS, SCENARIO,
	    "the earliest task owns every insertion");
	for (i = 1; i < fixture.pool->task_count; i++)
		check(fixture.pool->tasks[i].inserted == 0, SCENARIO,
		    "later tasks own no insertion");
	fixture_close(&fixture);
}

/* Later tasks own unique rows too; task two contributes nothing. */
static void
offer_distinct_task(dense_fixture_t *fixture, size_t task, size_t worker)
{
	static const unsigned int rows[5][3] = {
		{ 0, 1, 0 }, { 2, 1, 2 }, { 0 }, { 3, 0 }, { 4, 3 }
	};
	static const size_t counts[5] = { 3, 3, 0, 2, 2 };
	relic_dom_t tuple[2];
	size_t i;

	fixture->pool->tasks[task].worker = worker;
	for (i = 0; i < counts[task]; i++) {
		expected_tuple(fixture, rows[task][i], tuple);
		check(relic_dense_take(&fixture->dense, task,
		    &fixture->pool->workers[worker].dense_sequence, tuple),
		    SCENARIO, "a distinct task contributes its rows");
	}
}

static void
several_tasks_win(void)
{
	static const size_t inserted[5] = { 2, 1, 0, 1, 1 };
	dense_fixture_t fixture;
	const relic_index_t *index;
	relic_dom_t expected[2];
	relic_tuple_id_t id;
	size_t i;
	bool merged;

	if (!fixture_open(&fixture, 4)) {
		check(false, SCENARIO, "the fixture is built");
		fixture_close(&fixture);
		return;
	}
	fixture.pool->task_count = 5;
	check(relic_dense_open(&fixture.dense, fixture.pool, &fixture.job) == 1,
	    SCENARIO, "the multi-owner round selects dense collection");
	if (fixture.dense.ranks == NULL) {
		fixture_close(&fixture);
		return;
	}
	if (fixture.pool->worker_count == 1) {
		for (i = 0; i < 5; i++)
			offer_distinct_task(&fixture, i, 0);
	} else {
		/* Each worker's tasks increase, while their completions interleave. */
		offer_distinct_task(&fixture, 1, 1);
		offer_distinct_task(&fixture, 3, 1);
		offer_distinct_task(&fixture, 4, 1);
		offer_distinct_task(&fixture, 0, 0);
		offer_distinct_task(&fixture, 2, 0);
	}
	merged = relic_round_merge(fixture.ctx, fixture.pool, &fixture.round,
	    0, &fixture.dense);
	check(merged, SCENARIO, "the multi-owner merge publishes directly");
	check(relic_relation_count(fixture.target) == OLD_ROWS + 5, SCENARIO,
	    "each winning task contributes exactly its new rows");
	index = relic_relation_index_at(fixture.target, 0);
	expected_tuple(&fixture, 0, expected);
	id = relic_index_first(index, fixture.target, expected);
	for (i = 0; i < 5 && merged; i++) {
		expected_tuple(&fixture, i, expected);
		check(relic_relation_find(fixture.target, expected) == OLD_ROWS + i + 1,
		    SCENARIO, "multiple winners retain task then derivation order");
		check(fixture.pool->tasks[i].inserted == inserted[i], SCENARIO,
		    "task counts include empty and partly overwritten tasks");
		check(id == OLD_ROWS + i + 1, SCENARIO,
		    "the secondary index chains the new rows in identifier order");
		if (id != RELIC_ID_NONE)
			id = relic_index_next(index, id);
	}
	check(id == RELIC_ID_NONE, SCENARIO, "the new secondary chain ends");
	fixture_close(&fixture);
}

static void
malformed_and_ineligible(void)
{
	dense_fixture_t fixture;
	relic_dom_t tuple[2];
	uint32_t sequence = 0;
	size_t saved, integers, symbols;

	if (!fixture_open(&fixture, 1)) {
		check(false, SCENARIO, "the fixture is built");
		fixture_close(&fixture);
		return;
	}
	fixture.terms[0].action = RELIC_PLAN_TEXT;
	check(relic_dense_open(&fixture.dense, fixture.pool, &fixture.job) == 0,
	    SCENARIO, "a computed head keeps the general collector");
	fixture.terms[0].action = RELIC_PLAN_BOUND;
	saved = fixture.pool->task_count;
	fixture.pool->task_count = UINT32_MAX;
	check(relic_dense_open(&fixture.dense, fixture.pool, &fixture.job) == 0,
	    SCENARIO, "task rank overflow is declined before allocation");
	fixture.pool->task_count = saved;
	integers = fixture.ctx->values.integer_count;
	symbols = fixture.ctx->symbols.count;
	fixture.ctx->values.integer_count = 65536;
	fixture.ctx->symbols.count = 0;
	check(relic_dense_open(&fixture.dense, fixture.pool, &fixture.job) == 0,
	    SCENARIO, "a rectangle beyond 32-bit positions is declined");
	fixture.ctx->values.integer_count = 0;
	check(relic_dense_open(&fixture.dense, fixture.pool, &fixture.job) == 0,
	    SCENARIO, "an empty domain is declined before allocation");
	fixture.ctx->values.integer_count = integers;
	fixture.ctx->symbols.count = symbols;
	check(relic_dense_open(&fixture.dense, fixture.pool, &fixture.job) == 1,
	    SCENARIO, "valid dimensions are admitted");
	if (fixture.dense.ranks != NULL) {
		expected_tuple(&fixture, 0, tuple);
		tuple[1] = 1;
		check(!relic_dense_take(&fixture.dense, 0, &sequence, tuple),
		    SCENARIO, "a pending marker is not an assigned coordinate");
		tuple[1] = RELIC_DOM_INTEGER | 100000;
		check(!relic_dense_take(&fixture.dense, 0, &sequence, tuple),
		    SCENARIO, "an out-of-span identifier is rejected before access");
		expected_tuple(&fixture, 0, tuple);
		check(!relic_dense_take(&fixture.dense, TASKS, &sequence, tuple),
		    SCENARIO, "an out-of-range task is rejected");
		sequence = UINT32_MAX;
		check(!relic_dense_take(&fixture.dense, 0, &sequence, tuple),
		    SCENARIO, "an invalid sequence cannot wrap into a sentinel");
	}
	fixture_close(&fixture);
}

static void
refusal_releases_room(bool interrupt, bool during_order)
{
	dense_fixture_t fixture;
	relic_account_t *account;
	size_t headroom;
	int status;

	if (!fixture_open(&fixture, 1)) {
		check(false, SCENARIO, "the fixture is built");
		fixture_close(&fixture);
		return;
	}
	if (during_order) {
		status = relic_dense_open(&fixture.dense, fixture.pool, &fixture.job);
		check(status == 1, SCENARIO, "collection is admitted before refusal");
		if (status != 1) {
			fixture_close(&fixture);
			return;
		}
		derive_part(&fixture, 0, 0);
	}
	account = &fixture.ctx->account;
	if (interrupt)
		relic_signal_flag_raise(&fixture.ctx->interrupted);
	else {
		headroom = during_order ? 256 * sizeof(size_t) +
		    2 * NEW_ROWS * sizeof(uint32_t) - 1 : 0;
		account->limit = account->store_bytes + account->value_bytes +
		    account->pool_bytes + account->predtab_bytes + account->arena_bytes +
		    relic_atomic_size_load(&account->round_bytes) + headroom;
	}
	status = during_order ? relic_dense_order(&fixture.dense) :
	    relic_dense_open(&fixture.dense, fixture.pool, &fixture.job);
	check(status == -1, SCENARIO, "private refusal is returned to the caller");
	check(relic_relation_count(fixture.target) == OLD_ROWS, SCENARIO,
	    "refusal publishes no tuple");
	relic_signal_flag_lower(&fixture.ctx->interrupted);
	fixture_close(&fixture);
}

static void
an_old_only_round_needs_no_order_array(void)
{
	dense_fixture_t fixture;
	relic_dom_t tuple[2];
	uint32_t sequence = 0;
	bool merged;

	if (!fixture_open(&fixture, 1)) {
		check(false, SCENARIO, "the fixture is built");
		fixture_close(&fixture);
		return;
	}
	fixture.job.admissible = 0;
	check(relic_dense_open(&fixture.dense, fixture.pool, &fixture.job) == 1,
	    SCENARIO, "an old-only round selects dense collection");
	if (fixture.dense.ranks != NULL) {
		tuple[0] = fixture.values[0];
		tuple[1] = fixture.values[1];
		check(relic_dense_take(&fixture.dense, 0, &sequence, tuple),
		    SCENARIO, "an old tuple consumes no insertion room");
		merged = relic_round_merge(fixture.ctx, fixture.pool, &fixture.round,
		    0, &fixture.dense);
		check(merged && relic_relation_count(fixture.target) == OLD_ROWS,
		    SCENARIO, "an empty append succeeds without adding tuples");
		check(fixture.dense.ranks == NULL && fixture.dense.order == NULL,
		    SCENARIO, "an old-only round keeps no ordering arrays");
	}
	fixture_close(&fixture);
}

/* Ordering fits, but the wider common append cannot coexist with its positions. */
static void
staging_refusal_releases_room(void)
{
	dense_fixture_t fixture;
	relic_account_t *account;
	relic_dom_t tuple[2];
	size_t i;
	uint32_t sequence = 0;
	bool merged;

	if (!fixture_open(&fixture, 1)) {
		check(false, SCENARIO, "the fixture is built");
		fixture_close(&fixture);
		return;
	}
	check(relic_dense_open(&fixture.dense, fixture.pool, &fixture.job) == 1,
	    SCENARIO, "staging refusal starts with an admitted collector");
	if (fixture.dense.ranks == NULL) {
		fixture_close(&fixture);
		return;
	}
	for (i = 0; i < OLD_ROWS; i++) {
		tuple[0] = fixture.values[NODES / 2 + i / NODES];
		tuple[1] = fixture.values[i % NODES];
		check(relic_dense_take(&fixture.dense, 0, &sequence, tuple),
		    SCENARIO, "the other half of the square is gathered");
	}
	account = &fixture.ctx->account;
	account->limit = account->store_bytes + account->value_bytes +
	    account->pool_bytes + account->predtab_bytes + account->arena_bytes +
	    relic_atomic_size_load(&account->round_bytes) + 256 * sizeof(size_t) +
	    2 * OLD_ROWS * sizeof(uint32_t);
	merged = relic_round_merge(fixture.ctx, fixture.pool, &fixture.round,
	    0, &fixture.dense);
	check(!merged && fixture.dense.ranks == NULL &&
	    fixture.dense.order != NULL && fixture.dense.count == OLD_ROWS,
	    SCENARIO, "ordering succeeds and append staging declines privately");
	check(relic_relation_count(fixture.target) == OLD_ROWS, SCENARIO,
	    "failed append preparation publishes no tuple");
	fixture_close(&fixture);
}

int
main(void)
{
	if (RELIC_DENSE_ATOMICS && sizeof(relic_atomic_u64_t) == 8) {
		ordered_publication(false);
		ordered_publication(true);
		several_tasks_win();
		an_old_only_round_needs_no_order_array();
		staging_refusal_releases_room();
		malformed_and_ineligible();
		refusal_releases_room(false, false);
		refusal_releases_room(false, true);
		refusal_releases_room(true, false);
		refusal_releases_room(true, true);
	}
	return (check_summary("test_dense"));
}
