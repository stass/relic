/*
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define	RELIC_FROZEN_ONLY

#include "config.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <strings.h>

#include "cdefs.h"
#include "round.h"

/*
 * The dense collector records the first occurrence of each new tuple of a
 * binary round as one rank per cell of the context's pair grid, so that
 * workers copy no tuples (specs/design-dense-rounds.md).
 */

/*
 * A cell's rank is DENSE_RANK_HELD for a tuple the target already holds,
 * DENSE_RANK_UNCLAIMED while no task has claimed it, and otherwise the
 * claiming task above DENSE_TASK_SHIFT and the claim's sequence below it
 * (relic_dense_take()).
 */
#define	DENSE_RANK_HELD		UINT64_C(0)
#define	DENSE_RANK_UNCLAIMED	UINT64_MAX
#define	DENSE_TASK_SHIFT	32

/*
 * Ordering sorts the ranks one digit at a time, low digit first.  A digit
 * is a byte, which radix_part() reads as a uint8_t.
 */
#define	DENSE_RANK_BITS		64
#define	DENSE_DIGIT_BITS	8
#define	DENSE_DIGITS		(1u << DENSE_DIGIT_BITS)

_Static_assert(DENSE_RANK_BITS == sizeof(uint64_t) * CHAR_BIT,
    "a rank is one 64-bit word");
_Static_assert(DENSE_DIGIT_BITS == sizeof(uint8_t) * CHAR_BIT,
    "a radix digit is one byte");
_Static_assert(DENSE_TASK_SHIFT == sizeof(uint32_t) * CHAR_BIT,
    "a rank's sequence is its low 32-bit word");

static void	*allocate(relic_dense_t *dense, size_t bytes);
static size_t	 boundary(const relic_dense_t *dense, size_t count,
		    size_t part);
static bool	 count_task_insertions(relic_dense_t *dense);
static size_t	 dense_bytes(const relic_dense_t *dense, size_t room);
static bool	 dense_is_smaller(const relic_dense_t *dense, size_t held,
		    uint64_t derivations);
static void	 initialize_part(void *argument, size_t part, size_t worker);
static void	 list_part(void *argument, size_t part, size_t worker);
static size_t	 ordinary_bytes(const relic_dense_t *dense, size_t share,
		    size_t candidates);
static size_t	 ordinary_candidate_bytes(void);
static void	 radix_part(void *argument, size_t part, size_t worker);
static bool	 radix_sort(relic_dense_t *dense);
static void	 release_ranking(relic_dense_t *dense);
static bool	 round_is_dense_shaped(const relic_round_t *round);
static void	 seed_part(void *argument, size_t part, size_t worker);
static bool	 stopped(relic_dense_t *dense);
static const relic_relation_t *target_relation(const relic_dense_t *dense);

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

static size_t
boundary(const relic_dense_t *dense, size_t count, size_t part)
{
	return ((size_t)((uint64_t)count * part / dense->parts));
}

static const relic_relation_t *
target_relation(const relic_dense_t *dense)
{
	return (relic_destination_relation(dense->job->round->targets[0]));
}

/*
 * Whether the round has the one shape the collector serves: a single set
 * target, binary, that every item writes, and every plan ending in an emit
 * of two values its operations assigned, so a derived tuple is always a
 * pair of identifiers the grid can number.
 */
static bool
round_is_dense_shaped(const relic_round_t *round)
{
	const relic_relation_t *target;
	const relic_op_emit_t *emit;
	const relic_plan_t *plan;
	size_t c, i;

	if (round->target_count != 1 ||
	    relic_destination_set_relation(round->targets[0]) == NULL)
		return (false);
	target = relic_destination_relation(round->targets[0]);
	if (relic_relation_arity(target) != RELIC_PAIR_GRID_ARITY)
		return (false);
	for (i = 0; i < round->item_count; i++) {
		if (round->items[i].target != 0)
			return (false);
		plan = round->items[i].plan;
		if (plan == NULL)
			continue;
		if (plan->op_count == 0 ||
		    plan->ops[plan->op_count - 1].kind != RELIC_OP_EMIT)
			return (false);
		emit = &plan->ops[plan->op_count - 1].u.emit;
		if (emit->arity != RELIC_PAIR_GRID_ARITY)
			return (false);
		for (c = 0; c < RELIC_PAIR_GRID_ARITY; c++) {
			if (!relic_emit_term_assigned(emit->terms[c].action))
				return (false);
		}
	}
	return (true);
}

/*
 * Bytes the ordinary collector holds for one binary candidate: in its
 * worker's candidates, its entry, its columns in `words` and its stripe;
 * in the merge, its reference among its stripe's firsts (a
 * relic_merge_ref_t, a worker and a position) and its position in its
 * worker's stripe list (relic_merge_list_t).  Screen tables are counted
 * apart, by relic_idtab_bytes_for().
 */
static size_t
ordinary_candidate_bytes(void)
{
	size_t bytes;

	/* The worker's entry, its columns in `words` and its stripe. */
	bytes = sizeof(relic_candidate_t);
	bytes += RELIC_PAIR_GRID_ARITY * sizeof(relic_dom_t);
	bytes += sizeof(uint8_t);
	/* The merge's reference, a worker and a position, and list position. */
	bytes += sizeof(uint32_t) + sizeof(uint32_t);
	bytes += sizeof(uint32_t);
	return (bytes);
}

/*
 * The ordinary collector's bytes when each worker gathers `share`
 * candidates, `candidates` in all: the candidates themselves, each worker's
 * screen over its own, and one table over all of them standing for the
 * merge's stripe tables, whose actual distribution is not known yet.
 */
static size_t
ordinary_bytes(const relic_dense_t *dense, size_t share, size_t candidates)
{
	size_t bytes;

	bytes = relic_saturating_product(candidates,
	    ordinary_candidate_bytes());
	bytes = relic_saturating_sum(bytes, relic_saturating_product(
	    dense->parts, relic_idtab_bytes_for(true, share)));
	bytes = relic_saturating_sum(bytes,
	    relic_idtab_bytes_for(true, candidates));
	return (bytes);
}

/*
 * The dense collector's bytes when it finds `room` new tuples: a rank per
 * cell, the order and scratch position arrays the radix sort alternates
 * between, and each part's histogram.
 */
static size_t
dense_bytes(const relic_dense_t *dense, size_t room)
{
	size_t bytes;

	bytes = relic_saturating_product(dense->grid.cells,
	    sizeof(*dense->ranks));
	bytes = relic_saturating_sum(bytes, relic_saturating_product(room,
	    sizeof(*dense->order) + sizeof(*dense->scratch)));
	bytes = relic_saturating_sum(bytes,
	    dense->parts * DENSE_DIGITS * sizeof(*dense->histograms));
	return (bytes);
}

/*
 * Whether the dense layout is expected to be the smaller for a round
 * expected to make `derivations` over a target holding `held` tuples: the
 * workers gather one candidate per derivation, an even share each, and the
 * new tuples among them are at most the grid's free cells.  The dense
 * arrays are filled and scanned every round, so the comparison bounds time
 * as well as memory (specs/design-dense-rounds.md).  This is a heuristic:
 * the arrays are charged at their actual sizes either way, and a poor guess
 * costs time or memory, never a different result.  A dense estimate that
 * saturates declines the dense layout; an ordinary one that saturates
 * leaves the dense layout the smaller.
 */
static bool
dense_is_smaller(const relic_dense_t *dense, size_t held,
    uint64_t derivations)
{
	size_t candidates, dense_estimate, free_cells, ordinary_estimate;
	size_t room, share;

	candidates = derivations > SIZE_MAX ? SIZE_MAX : (size_t)derivations;
	share = candidates / dense->parts + (candidates % dense->parts != 0);
	free_cells = dense->grid.cells - held;
	room = free_cells < candidates ? free_cells : candidates;
	dense_estimate = dense_bytes(dense, room);
	ordinary_estimate = ordinary_bytes(dense, share, candidates);
	if (dense_estimate == SIZE_MAX)
		return (false);
	return (dense_estimate < ordinary_estimate);
}

/*
 * Select the dense collector for a round of the right shape whose grid a
 * rank array can cover and which the byte model favours for the round's
 * expected `derivations`, then initialize every rank and seed the cells of
 * the tuples the target already holds.
 */
int
relic_dense_open(relic_dense_t *dense, relic_pool_t *pool,
    relic_round_job_t *job, uint64_t derivations)
{
	size_t held, i;

	bzero(dense, sizeof(*dense));
	if (!RELIC_DENSE_ATOMICS || sizeof(*dense->ranks) != sizeof(uint64_t) ||
	    pool->task_count >= UINT32_MAX)
		return (0);
	if (!round_is_dense_shaped(job->round))
		return (0);
	dense->job = job;
	dense->pool = pool;
	dense->parts = relic_pool_workers(pool);
	relic_pair_grid_take(job->round->env->ctx, &dense->grid);
	if (dense->grid.cells == 0 || dense->grid.cells > UINT32_MAX)
		return (0);
	held = relic_relation_count(target_relation(dense));
	if (held > dense->grid.cells || !dense_is_smaller(dense, held,
	    derivations))
		return (0);

	dense->ranks = allocate(dense, relic_saturating_product(
	    dense->grid.cells, sizeof(*dense->ranks)));
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

	end = boundary(dense, dense->grid.cells, part + 1);
	for (i = boundary(dense, dense->grid.cells, part); i < end; i++) {
		if ((i & RELIC_DENSE_POLL_MASK) == 0 && stopped(dense))
			return;
		relic_atomic_u64_init(&dense->ranks[i], DENSE_RANK_UNCLAIMED);
	}
}

/*
 * Initialization has its own barrier: no seed can race a cell's
 * initialization.
 */
static void
seed_part(void *argument, size_t part, size_t worker RELIC_UNUSED)
{
	relic_dense_t *dense = argument;
	const relic_relation_t *target;
	const relic_dom_t *tuple;
	size_t cell, count, end, i;

	target = target_relation(dense);
	count = relic_relation_count(target);
	end = boundary(dense, count, part + 1);
	for (i = boundary(dense, count, part); i < end; i++) {
		if ((i & RELIC_DENSE_POLL_MASK) == 0 && stopped(dense))
			return;
		tuple = relic_relation_tuple(target, (relic_tuple_id_t)(i + 1));
		if (!relic_pair_grid_cell(&dense->grid, tuple, &cell)) {
			relic_atomic_bool_store_relaxed(&dense->job->abandoned,
			    true);
			return;
		}
		relic_atomic_u64_store_release(&dense->ranks[cell],
		    DENSE_RANK_HELD);
	}
}

/*
 * A cell holds DENSE_RANK_HELD for a tuple the target already has,
 * DENSE_RANK_UNCLAIMED while no task has claimed it, or
 * task << DENSE_TASK_SHIFT | sequence for the claim that holds it;
 * DENSE_RANK_UNCLAIMED names a task above every real one.  A task claims a
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
	size_t cell_index;

	if (task >= dense->pool->task_count || task >= UINT32_MAX ||
	    !relic_pair_grid_cell(&dense->grid, tuple, &cell_index))
		return (false);
	cell = &dense->ranks[cell_index];
	before = relic_atomic_u64_load_relaxed(cell);
	while (before != DENSE_RANK_HELD &&
	    (before >> DENSE_TASK_SHIFT) > task) {
		if (*sequence == UINT32_MAX)
			return (false);
		rank = ((uint64_t)task << DENSE_TASK_SHIFT) |
		    ((uint64_t)*sequence + 1);
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

/*
 * Without order this counts claimed cells; with it, each part fills its
 * prefixed range.
 */
static void
list_part(void *argument, size_t part, size_t worker RELIC_UNUSED)
{
	relic_dense_t *dense = argument;
	size_t end, i, next;
	uint64_t rank;

	next = dense->order == NULL ? 0 :
	    dense->histograms[part * DENSE_DIGITS];
	end = boundary(dense, dense->grid.cells, part + 1);
	for (i = boundary(dense, dense->grid.cells, part); i < end; i++) {
		if ((i & RELIC_DENSE_POLL_MASK) == 0 && stopped(dense))
			return;
		rank = relic_atomic_u64_load_relaxed(&dense->ranks[i]);
		if (rank == DENSE_RANK_HELD || rank == DENSE_RANK_UNCLAIMED)
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
		if ((i & RELIC_DENSE_POLL_MASK) == 0 && stopped(dense))
			return;
		digit = (uint8_t)(relic_atomic_u64_load_relaxed(
		    &dense->ranks[dense->order[i]]) >> dense->shift);
		if (dense->scattering)
			dense->scratch[histogram[digit]++] = dense->order[i];
		else
			histogram[digit]++;
	}
}

/*
 * Order the claimed cells by rank, which is the order a serial run first
 * derives their tuples, and count each task's.  The claimed cells are
 * counted before the two position arrays are allocated, so both are exact.
 */
int
relic_dense_order(relic_dense_t *dense)
{
	size_t bytes, held, i, p;

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
	    RELIC_TUPLE_MAX - relic_relation_count(target_relation(dense))) {
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

	if (!radix_sort(dense))
		return (-1);
	if (!count_task_insertions(dense))
		return (-1);
	release_ranking(dense);
	return (0);
}

/*
 * Sort `order` by rank, one digit per pass, low digit first.  Each pass is
 * stable: it keeps input order within each digit and, across parts, the
 * parts' order, so the last pass leaves the positions sorted by the whole
 * rank.
 */
static bool
radix_sort(relic_dense_t *dense)
{
	size_t digit, held, i, next, p;
	uint32_t *swap;

	for (dense->shift = 0; dense->shift < DENSE_RANK_BITS;
	    dense->shift += DENSE_DIGIT_BITS) {
		if (stopped(dense))
			return (false);
		dense->scattering = false;
		relic_pool_for(dense->pool, dense->parts, radix_part, dense);
		if (stopped(dense))
			return (false);
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
	return (true);
}

/*
 * Count each task's new tuples from the ordered ranks, on the calling
 * thread alone, so that staging can place each task's tuples at its
 * prefix position.  A rank whose task is out of range, or whose sequence
 * is zero, which no claim writes, cannot have come from gathering, and
 * fails the order.
 */
static bool
count_task_insertions(relic_dense_t *dense)
{
	uint64_t rank;
	size_t i, task;

	for (i = 0; i < dense->count; i++) {
		if ((i & RELIC_DENSE_POLL_MASK) == 0 && stopped(dense))
			return (false);
		rank = relic_atomic_u64_load_relaxed(
		    &dense->ranks[dense->order[i]]);
		task = (size_t)(rank >> DENSE_TASK_SHIFT);
		if (task >= dense->pool->task_count || (uint32_t)rank == 0)
			return (false);
		dense->pool->tasks[task].inserted++;
	}
	return (true);
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
		relic_account_release_round(dense->job->account,
		    dense->charged);
	dense->charged = 0;
}

void
relic_dense_tuple(const relic_dense_t *dense, size_t position,
    relic_dom_t *tuple)
{
	assert(position < dense->count);
	relic_pair_grid_tuple(&dense->grid, dense->order[position], tuple);
}

void
relic_dense_close(relic_dense_t *dense)
{
	free(dense->ranks);
	free(dense->order);
	free(dense->scratch);
	free(dense->histograms);
	if (dense->charged != 0)
		relic_account_release_round(dense->job->account,
		    dense->charged);
	bzero(dense, sizeof(*dense));
}
