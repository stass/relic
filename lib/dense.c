/*
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define	RELIC_FROZEN_ONLY

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <strings.h>

#include "cdefs.h"
#include "round.h"

/* A fixed pair rectangle collects first occurrences without worker tuple copies. */
#define	DENSE_POLL_MASK	4095u
#define	DENSE_DIGITS	256u

static void	*allocate(relic_dense_t *dense, size_t bytes);
static size_t	 boundary(const relic_dense_t *dense, size_t count, size_t part);
static bool	 coordinate(const relic_dense_t *dense, relic_dom_t value,
		    size_t *position);
static relic_dom_t decode(const relic_dense_t *dense, size_t position);
static void	 initialize_part(void *argument, size_t part, size_t worker);
static void	 list_part(void *argument, size_t part, size_t worker);
static void	 radix_part(void *argument, size_t part, size_t worker);
static void	 release_ranking(relic_dense_t *dense);
static void	 seed_part(void *argument, size_t part, size_t worker);
static bool	 stopped(relic_dense_t *dense);

static bool
stopped(relic_dense_t *dense)
{
	if (relic_signal_flag_raised(dense->job->interrupted))
		relic_atomic_bool_store_relaxed(&dense->job->abandoned, true);
	return (relic_atomic_bool_load_relaxed(&dense->job->abandoned));
}

static void *
allocate(relic_dense_t *dense, size_t bytes)
{
	void *memory;

	if (bytes == SIZE_MAX ||
	    !relic_account_charge_round(dense->job->account, bytes))
		return (NULL);
	memory = malloc(bytes);
	if (memory == NULL)
		relic_account_release_round(dense->job->account, bytes);
	else
		dense->charged += bytes;
	return (memory);
}

/* Typed pool positions are disjoint, even when symbol and string share bytes. */
static bool
coordinate(const relic_dense_t *dense, relic_dom_t value, size_t *position)
{
	size_t index;

	index = value & RELIC_DOM_INDEX_MASK;
	if (index == 0)
		return (false);
	switch (value & RELIC_DOM_TYPE_MASK) {
	case RELIC_DOM_INTEGER:
		*position = index - 1;
		return (index <= dense->integers);
	case RELIC_DOM_SYMBOL:
		*position = dense->integers + index - 1;
		return (index <= dense->symbols);
	case RELIC_DOM_STRING:
		*position = dense->integers + dense->symbols + index - 1;
		return (index <= dense->symbols);
	}
	return (false);
}

static relic_dom_t
decode(const relic_dense_t *dense, size_t position)
{
	if (position < dense->integers)
		return (RELIC_DOM_INTEGER | (relic_dom_t)(position + 1));
	position -= dense->integers;
	if (position < dense->symbols)
		return (RELIC_DOM_SYMBOL | (relic_dom_t)(position + 1));
	return (RELIC_DOM_STRING |
	    (relic_dom_t)(position - dense->symbols + 1));
}

static size_t
boundary(const relic_dense_t *dense, size_t count, size_t part)
{
	return ((size_t)((uint64_t)count * part / dense->parts));
}

int
relic_dense_open(relic_dense_t *dense, relic_pool_t *pool,
    relic_round_job_t *job)
{
	const relic_round_t *round;
	const relic_op_emit_t *emit;
	const relic_plan_t *plan;
	size_t c, copies, held, i, ordinary, projected, room;

	bzero(dense, sizeof(*dense));
	round = job->round;
	if (!RELIC_DENSE_ATOMICS || sizeof(*dense->ranks) != 8 ||
	    round->target_count != 1 || pool->task_count >= UINT32_MAX ||
	    relic_destination_set_relation(round->targets[0]) == NULL ||
	    relic_relation_arity(relic_destination_relation(round->targets[0])) != 2)
		return (0);
	for (i = 0; i < round->item_count; i++) {
		if (round->items[i].target != 0)
			return (0);
		plan = round->items[i].plan;
		if (plan == NULL)
			continue;
		if (plan->op_count == 0 ||
		    plan->ops[plan->op_count - 1].kind != RELIC_OP_EMIT)
			return (0);
		emit = &plan->ops[plan->op_count - 1].u.emit;
		if (emit->arity != 2)
			return (0);
		for (c = 0; c < 2; c++) {
			if (!relic_emit_term_assigned(emit->terms[c].action))
				return (0);
		}
	}
	dense->job = job;
	dense->pool = pool;
	dense->parts = relic_pool_workers(pool);
	relic_value_spans(round->env->ctx, &dense->integers, &dense->symbols);
	dense->width = relic_saturating_sum(dense->integers,
	    relic_saturating_product(2, dense->symbols));
	dense->cells = relic_saturating_product(dense->width, dense->width);
	if (dense->cells == 0 || dense->cells > UINT32_MAX)
		return (0);
	held = relic_relation_count(relic_destination_relation(round->targets[0]));
	if (held > dense->cells)
		return (0);
	copies = relic_saturating_product(dense->parts, held);
	room = dense->cells - held < copies ? dense->cells - held : copies;
	projected = relic_saturating_sum(relic_saturating_product(8,
	    relic_saturating_sum(dense->cells, room)),
	    dense->parts * DENSE_DIGITS * sizeof(size_t));
	ordinary = relic_saturating_product(copies,
	    sizeof(relic_candidate_t) + 3 * sizeof(uint32_t) + 1 + 8);
	ordinary = relic_saturating_sum(ordinary, relic_saturating_product(
	    dense->parts, relic_idtab_bytes_for(true, held)));
	ordinary = relic_saturating_sum(ordinary,
	    relic_idtab_bytes_for(true, copies));
	if (projected == SIZE_MAX || ordinary == SIZE_MAX || projected >= ordinary)
		return (0);
	dense->ranks = allocate(dense,
	    relic_saturating_product(dense->cells, sizeof(*dense->ranks)));
	if (dense->ranks == NULL)
		return (-1);
	for (i = 0; i < dense->parts; i++)
		pool->workers[i].dense_sequence = 0;
	relic_pool_for(pool, dense->parts, initialize_part, dense);
	if (!stopped(dense))
		relic_pool_for(pool, dense->parts, seed_part, dense);
	return (stopped(dense) ? -1 : 1);
}

static void
initialize_part(void *argument, size_t part, size_t worker RELIC_UNUSED)
{
	relic_dense_t *dense = argument;
	size_t end, i;

	end = boundary(dense, dense->cells, part + 1);
	for (i = boundary(dense, dense->cells, part); i < end; i++) {
		if ((i & DENSE_POLL_MASK) == 0 && stopped(dense))
			return;
		relic_atomic_u64_init(&dense->ranks[i], UINT64_MAX);
	}
}

/* Initialization has its own barrier: no seed can race a cell's initialization. */
static void
seed_part(void *argument, size_t part, size_t worker RELIC_UNUSED)
{
	relic_dense_t *dense = argument;
	const relic_relation_t *target = relic_destination_relation(dense->job->round->targets[0]);
	const relic_dom_t *tuple;
	size_t a, b, count, end, i;

	count = relic_relation_count(target);
	end = boundary(dense, count, part + 1);
	for (i = boundary(dense, count, part); i < end; i++) {
		if ((i & DENSE_POLL_MASK) == 0 && stopped(dense))
			return;
		tuple = relic_relation_tuple(target, (relic_tuple_id_t)(i + 1));
		if (!coordinate(dense, tuple[0], &a) ||
		    !coordinate(dense, tuple[1], &b)) {
			relic_atomic_bool_store_relaxed(&dense->job->abandoned, true);
			return;
		}
		relic_atomic_u64_store_release(&dense->ranks[a * dense->width + b], 0);
	}
}

/*
 * A cell holds zero for a tuple the target already has, UINT64_MAX while
 * no task has claimed it, or task << 32 | sequence for the claim that
 * holds it; UINT64_MAX names a task above every real one.  A task claims a
 * cell only from a higher-numbered task, so a cell's task only decreases,
 * and once every task has run it names the lowest task that derives the
 * tuple, whatever the schedule.  That task claims the cell at its first
 * derivation of the tuple, since no lower task can hold it, and never
 * again, since its own later derivations find the cell already its own;
 * only a lower-numbered task could take the cell away.
 *
 * `sequence` is the worker's dense_sequence, which numbers its successful
 * claims in the order it makes them.  A task runs on one worker from start
 * to end, so within a task the numbers follow its derivations, and the
 * cells a task keeps sort in the order of their first derivation there.
 * The sequence values themselves depend on the schedule, but that order
 * does not.  Tasks are cut from the round in its serial order, so ordering
 * the ranks, by task and then by sequence, gives the order in which a
 * serial run first derives each tuple.
 *
 * relic_pool_for() hands each worker its tasks in increasing order, so a
 * worker that claimed a cell for one task never claims it again for a
 * later one: it claims each cell at most once, which bounds its sequence
 * by cells.  Any false return, such as for a tuple outside the rectangle or
 * past the room the limit admits, abandons the round, which then runs
 * serially; the ranks of an abandoned round are never ordered.
 *
 * The word publishes no other data, so relaxed CAS and the pool barrier
 * suffice.
 */
bool
relic_dense_take(relic_dense_t *dense RELIC_UNUSED, size_t task RELIC_UNUSED,
    uint32_t *sequence RELIC_UNUSED, const relic_dom_t *tuple RELIC_UNUSED)
{
#if RELIC_DENSE_ATOMICS
	relic_atomic_u64_t *cell;
	uint64_t before, rank;
	size_t a, b;

	if (task >= dense->pool->task_count || task >= UINT32_MAX ||
	    !coordinate(dense, tuple[0], &a) || !coordinate(dense, tuple[1], &b))
		return (false);
	cell = &dense->ranks[a * dense->width + b];
	before = relic_atomic_u64_load_relaxed(cell);
	while (before != 0 && (before >> 32) > task) {
		if (*sequence == UINT32_MAX)
			return (false);
		rank = ((uint64_t)task << 32) | ((uint64_t)*sequence + 1);
		if (relic_atomic_u64_swap_if_relaxed(cell, &before, rank)) {
			(*sequence)++;
			return (*sequence <= dense->job->admissible);
		}
	}
	return (true);
#else
	return (false);
#endif
}

/* Without order this counts cells; with it, each part fills its prefixed range. */
static void
list_part(void *argument, size_t part, size_t worker RELIC_UNUSED)
{
	relic_dense_t *dense = argument;
	size_t end, i, next;
	uint64_t rank;

	next = dense->order == NULL ? 0 : dense->histograms[part * DENSE_DIGITS];
	end = boundary(dense, dense->cells, part + 1);
	for (i = boundary(dense, dense->cells, part); i < end; i++) {
		if ((i & DENSE_POLL_MASK) == 0 && stopped(dense))
			return;
		rank = relic_atomic_u64_load_relaxed(&dense->ranks[i]);
		if (rank == 0 || rank == UINT64_MAX)
			continue;
		if (dense->order != NULL)
			dense->order[next] = (uint32_t)i;
		next++;
	}
	dense->histograms[part * DENSE_DIGITS] = next;
}

static void
radix_part(void *argument, size_t part, size_t worker RELIC_UNUSED)
{
	relic_dense_t *dense = argument;
	size_t *histogram = &dense->histograms[part * DENSE_DIGITS];
	size_t end, i;
	uint8_t digit;

	if (!dense->scattering)
		bzero(histogram, DENSE_DIGITS * sizeof(*histogram));
	end = boundary(dense, dense->count, part + 1);
	for (i = boundary(dense, dense->count, part); i < end; i++) {
		if ((i & DENSE_POLL_MASK) == 0 && stopped(dense))
			return;
		digit = (uint8_t)(relic_atomic_u64_load_relaxed(
		    &dense->ranks[dense->order[i]]) >> dense->shift);
		if (dense->scattering)
			dense->scratch[histogram[digit]++] = dense->order[i];
		else
			histogram[digit]++;
	}
}

int
relic_dense_order(relic_dense_t *dense)
{
	size_t bytes, digit, held, i, next, p, task;
	uint32_t *swap;
	uint64_t rank;

	/* Count winners before allocating the two exact position arrays. */
	for (i = 0; i < dense->pool->task_count; i++)
		dense->pool->tasks[i].inserted = 0;
	dense->histograms = allocate(dense,
	    dense->parts * DENSE_DIGITS * sizeof(*dense->histograms));
	if (dense->histograms == NULL)
		return (-1);
	relic_pool_for(dense->pool, dense->parts, list_part, dense);
	if (stopped(dense))
		return (-1);
	for (p = 0; p < dense->parts; p++) {
		held = dense->histograms[p * DENSE_DIGITS];
		dense->histograms[p * DENSE_DIGITS] = dense->count;
		dense->count += held;
	}
	if (dense->count > dense->job->admissible || dense->count >
	    RELIC_TUPLE_MAX - relic_relation_count(relic_destination_relation(dense->job->round->targets[0]))) {
		dense->pool->limit_declines++;
		return (-1);
	}
	if (dense->count == 0) {
		release_ranking(dense);
		return (0);
	}
	bytes = relic_saturating_product(dense->count, sizeof(*dense->order));
	dense->order = allocate(dense, bytes);
	if (dense->order == NULL)
		return (-1);
	dense->scratch = allocate(dense, bytes);
	if (dense->scratch == NULL)
		return (-1);
	relic_pool_for(dense->pool, dense->parts, list_part, dense);

	/* Stable passes preserve input order within each digit and ordered part. */
	for (dense->shift = 0; dense->shift < 64; dense->shift += 8) {
		if (stopped(dense))
			return (-1);
		dense->scattering = false;
		relic_pool_for(dense->pool, dense->parts, radix_part, dense);
		if (stopped(dense))
			return (-1);
		next = 0;
		for (digit = 0; digit < DENSE_DIGITS; digit++) {
			for (p = 0; p < dense->parts; p++) {
				i = p * DENSE_DIGITS + digit;
				held = dense->histograms[i];
				dense->histograms[i] = next;
				next += held;
			}
		}
		dense->scattering = true;
		relic_pool_for(dense->pool, dense->parts, radix_part, dense);
		swap = dense->order;
		dense->order = dense->scratch;
		dense->scratch = swap;
	}
	/* One caller owns task counts; staging later uses their prefix positions. */
	for (i = 0; i < dense->count; i++) {
		if ((i & DENSE_POLL_MASK) == 0 && stopped(dense))
			return (-1);
		rank = relic_atomic_u64_load_relaxed(&dense->ranks[dense->order[i]]);
		task = (size_t)(rank >> 32);
		if (task >= dense->pool->task_count || (uint32_t)rank == 0)
			return (-1);
		dense->pool->tasks[task].inserted++;
	}
	release_ranking(dense);
	return (0);
}

/*
 * Free what only ordering needs, keeping the `count` ordered positions and
 * their charge: staging reads nothing else.  With no positions, order was
 * never allocated and nothing stays charged.
 */
static void
release_ranking(relic_dense_t *dense)
{
	size_t kept;

	kept = dense->count * sizeof(*dense->order);
	assert(kept <= dense->charged && (dense->order != NULL || kept == 0));
	free(dense->ranks);
	free(dense->scratch);
	free(dense->histograms);
	dense->ranks = NULL;
	dense->scratch = NULL;
	dense->histograms = NULL;
	relic_account_release_round(dense->job->account, dense->charged - kept);
	dense->charged = kept;
}

void
relic_dense_release_order(relic_dense_t *dense)
{
	assert(dense->ranks == NULL && dense->scratch == NULL &&
	    dense->histograms == NULL);
	free(dense->order);
	dense->order = NULL;
	dense->count = 0;
	if (dense->charged != 0)
		relic_account_release_round(dense->job->account, dense->charged);
	dense->charged = 0;
}

void
relic_dense_tuple(const relic_dense_t *dense, size_t position, relic_dom_t *tuple)
{
	size_t cell;

	assert(position < dense->count);
	cell = dense->order[position];

	tuple[0] = decode(dense, cell / dense->width);
	tuple[1] = decode(dense, cell % dense->width);
}

void
relic_dense_close(relic_dense_t *dense)
{
	free(dense->ranks);
	free(dense->order);
	free(dense->scratch);
	free(dense->histograms);
	if (dense->charged != 0)
		relic_account_release_round(dense->job->account, dense->charged);
	bzero(dense, sizeof(*dense));
}
