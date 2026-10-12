/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Preparing a round sees the store only as frozen.h presents it, as the
 * round's tasks do, so that nothing prepared can change what the tasks
 * then read.  The headers that define a context, a relation and an index
 * refuse to be included where this is defined.
 */
#define	RELIC_FROZEN_ONLY

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <strings.h>

#include "round.h"

/*
 * Dividing a plan's driver among a round's tasks, as worker 0 prepares it
 * before the round's tasks are published.
 *
 * The plan's prefix, the tests before its driver, binds nothing, so it
 * either rejects the plan or reaches the driver once, with an empty frame.
 * It is run here once, on worker 0's scratch, through the executor, and
 * what it read and noted is the item's rather than any task's.
 *
 * The driver's visits are the tuples it reads before matching any term:
 * for a SCAN every identifier of its view, and for a LOOKUP the chain of
 * its key inside its view, the key being constants alone.  A lookup's
 * chain begins at its head for every view but a delta, and for a delta at
 * the first chain tuple at or past the delta's start, found and published
 * as the key's frontier as the executor's own lookup there would
 * (relic_index_seek_begin()).  The visits are counted by following the
 * chain's links, and the starts of the pieces they are cut into are found
 * by following them again, as far as the last piece's start.  Neither
 * walk reads a tuple or keeps one identifier per visit, and each stops to
 * look for an interrupt every RELIC_SLICE_LINKS links, so preparing an
 * item follows at most F + 2 * M links, F of them toward a delta's start
 * and M the visits, and can always be interrupted.
 *
 * A piece's boundaries are ordinals of the visits, b[j] = floor(M * j /
 * P) for P pieces, so pieces differ by at most one visit however the
 * chain's identifiers are spread; a piece of a lookup is the chain tuple
 * at its first ordinal and the next piece's first tuple, before which it
 * stops, the last piece stopping at the view's end.
 */

/*
 * How many links a walk follows between looks for an interrupt: as many
 * as the executor reads between its returns.
 */
#define	RELIC_SLICE_LINKS	RELIC_EXEC_QUANTUM

_Static_assert(RELIC_TUPLE_MAX <= UINT32_MAX,
    "a visit count times a piece ordinal fits 64 bits");

static uint64_t	 boundary(uint64_t visits, size_t piece, size_t pieces);
static int	 count_chain(relic_prepared_item_t *prepared,
		    const relic_index_t *index,
		    const relic_signal_flag_t *interrupted);
static void	 driver_probe(const relic_op_atom_t *atom, relic_dom_t *probe);
static const relic_index_t *driver_index(const relic_op_atom_t *atom);
static int	 find_chain(relic_prepared_item_t *prepared,
		    const relic_op_atom_t *atom, relic_tuple_range_t view,
		    const relic_signal_flag_t *interrupted);
static int	 run_prefix(relic_prepared_item_t *prepared,
		    relic_scratch_t *scratch, const relic_exec_env_t *env,
		    const relic_plan_t *plan,
		    const relic_signal_flag_t *interrupted, bool *reachedp);

int
relic_slice_prepare(relic_prepared_item_t *prepared, relic_scratch_t *scratch,
    const relic_exec_env_t *env, const relic_plan_t *plan,
    const relic_signal_flag_t *interrupted)
{
	const relic_op_atom_t *atom;
	relic_tuple_range_t view;
	size_t driver;
	bool reached;

	driver = relic_plan_driver(plan);
	assert(driver != RELIC_PLAN_NO_DRIVER);
	atom = &plan->ops[driver].u.atom;

	bzero(prepared, sizeof(*prepared));
	prepared->form = RELIC_TASK_EMPTY;
	prepared->pieces = 1;

	if (run_prefix(prepared, scratch, env, plan, interrupted,
	    &reached) != 0)
		return (-1);
	if (!reached)
		return (0);

	view = relic_exec_view(env, atom);
	if (plan->ops[driver].kind == RELIC_OP_SCAN) {
		prepared->driving = view;
		if (view.end > view.first)
			prepared->visits = view.end - view.first;
	} else {
		assert(plan->ops[driver].kind == RELIC_OP_LOOKUP);
		if (find_chain(prepared, atom, view, interrupted) != 0)
			return (-1);
	}

	if (prepared->visits > 0)
		prepared->form = RELIC_TASK_SLICE;

	return (0);
}

/*
 * Run a plan's prefix to its end on the scratch, and move what it read and
 * noted into the item's record, leaving the scratch with nothing counted,
 * noted or met.  *reachedp says whether every test held.  Returns 0, or -1
 * when a test met a failure or the caller was interrupted.
 */
static int
run_prefix(relic_prepared_item_t *prepared, relic_scratch_t *scratch,
    const relic_exec_env_t *env, const relic_plan_t *plan,
    const relic_signal_flag_t *interrupted, bool *reachedp)
{
	enum relic_prefix_status status;
	bool stopped;

	relic_exec_start_prefix(scratch, env, plan);
	do {
		status = relic_exec_run_prefix(scratch);
		stopped = relic_signal_flag_raised(interrupted);
	} while (status == RELIC_PREFIX_PAUSED && !stopped);

	prepared->prefix_reads = scratch->counters;
	prepared->reads = scratch->driver_reads;
	prepared->note = scratch->note;
	bzero(&scratch->counters, sizeof(scratch->counters));
	scratch->note.where = NULL;
	bzero(&scratch->error, sizeof(scratch->error));

	*reachedp = status == RELIC_PREFIX_REACHED;

	if (status == RELIC_PREFIX_FAILED || stopped)
		return (-1);

	return (0);
}

/*
 * Find where a LOOKUP driver's visits begin inside its view, and count
 * them.  Over a delta, the walk to the first visit publishes it as the
 * key's frontier once it stops, as the executor's lookup there would.
 * Returns 0, or -1 when the caller was interrupted: during that walk,
 * which leaves the frontier as it was, or during the count, which leaves
 * it published, either being a frontier the executor's lookups accept.
 */
static int
find_chain(relic_prepared_item_t *prepared, const relic_op_atom_t *atom,
    relic_tuple_range_t view, const relic_signal_flag_t *interrupted)
{
	relic_dom_t probe[RELIC_MAX_ARITY];
	relic_index_seek_t seek;
	const relic_index_t *index;

	index = driver_index(atom);
	driver_probe(atom, probe);

	if (atom->view == RELIC_VIEW_DELTA) {
		relic_index_seek_begin(index, atom->relation, probe,
		    view.first, &seek);
		while (!relic_index_seek_walk(index, &seek, RELIC_SLICE_LINKS,
		    &prepared->links)) {
			if (relic_signal_flag_raised(interrupted))
				return (-1);
		}
		prepared->driving.first = relic_index_seek_found(&seek);
	} else {
		prepared->driving.first = relic_index_first(index,
		    atom->relation, probe);
	}
	prepared->driving.end = view.end;

	return (count_chain(prepared, index, interrupted));
}

/*
 * Count the chain tuples from the first visit until the chain ends or
 * reaches the view's end.  Returns 0, or -1 when the caller was
 * interrupted.
 */
static int
count_chain(relic_prepared_item_t *prepared, const relic_index_t *index,
    const relic_signal_flag_t *interrupted)
{
	relic_tuple_id_t id;
	uint64_t budget;

	budget = RELIC_SLICE_LINKS;
	id = prepared->driving.first;
	while (id != RELIC_ID_NONE && id < prepared->driving.end) {
		if (budget == 0) {
			if (relic_signal_flag_raised(interrupted))
				return (-1);
			budget = RELIC_SLICE_LINKS;
		}
		budget--;
		prepared->visits++;
		prepared->links++;
		id = relic_index_next(index, id);
	}

	return (0);
}

int
relic_slice_cut(const relic_prepared_item_t *prepared,
    const relic_plan_t *plan, relic_round_task_t *tasks,
    const relic_signal_flag_t *interrupted, uint64_t *linksp)
{
	const relic_index_t *index;
	const relic_op_t *driver;
	relic_tuple_range_t driving;
	relic_tuple_id_t id;
	uint64_t ordinal;
	uint64_t target;
	uint64_t budget;
	size_t pieces;
	size_t j;

	pieces = prepared->pieces;
	driving = prepared->driving;
	assert(prepared->form == RELIC_TASK_SLICE);
	assert(pieces >= 1 && pieces <= prepared->visits);
	assert(relic_plan_driver(plan) != RELIC_PLAN_NO_DRIVER);
	driver = &plan->ops[relic_plan_driver(plan)];

	if (driver->kind == RELIC_OP_SCAN) {
		for (j = 0; j < pieces; j++) {
			tasks[j].slice.first = driving.first +
			    (relic_tuple_id_t)boundary(prepared->visits, j,
			    pieces);
			tasks[j].slice.end = driving.first + (relic_tuple_id_t)
			    boundary(prepared->visits, j + 1, pieces);
		}
		return (0);
	}

	/*
	 * `id` is the visit at `ordinal`, counting from the first at zero.
	 * The store is frozen, so the chain holds the visits the count found;
	 * that is checked before each link is followed all the same, since
	 * following one from past the chain's end would read outside it.
	 */
	index = driver_index(&driver->u.atom);
	id = driving.first;
	ordinal = 0;
	budget = RELIC_SLICE_LINKS;
	tasks[0].slice.first = id;
	for (j = 1; j < pieces; j++) {
		target = boundary(prepared->visits, j, pieces);
		while (ordinal < target) {
			if (budget == 0) {
				if (relic_signal_flag_raised(interrupted))
					return (-1);
				budget = RELIC_SLICE_LINKS;
			}
			budget--;
			if (id == RELIC_ID_NONE || id >= driving.end) {
				assert(!"a chain shorter than its count");
				return (-1);
			}
			id = relic_index_next(index, id);
			ordinal++;
			(*linksp)++;
		}
		if (id == RELIC_ID_NONE || id >= driving.end) {
			assert(!"a chain shorter than its count");
			return (-1);
		}
		tasks[j - 1].slice.end = id;
		tasks[j].slice.first = id;
	}
	tasks[pieces - 1].slice.end = driving.end;

	return (0);
}

uint64_t
relic_slice_cut_links(const relic_prepared_item_t *prepared,
    const relic_plan_t *plan)
{
	size_t driver;

	if (prepared->form != RELIC_TASK_SLICE || prepared->pieces < 2)
		return (0);
	driver = relic_plan_driver(plan);
	assert(driver != RELIC_PLAN_NO_DRIVER);
	if (plan->ops[driver].kind != RELIC_OP_LOOKUP)
		return (0);

	return (boundary(prepared->visits, prepared->pieces - 1,
	    prepared->pieces));
}

/*
 * The ordinal of the first visit of piece `piece` of `pieces`, and of the
 * visit past the last when `piece` is `pieces`: floor(visits * piece /
 * pieces).  The visits are below 2^32 and the piece at most the pieces,
 * which are at most the visits, so the product fits.
 */
static uint64_t
boundary(uint64_t visits, size_t piece, size_t pieces)
{
	assert(visits <= RELIC_TUPLE_MAX && piece <= pieces &&
	    pieces <= visits);

	return (visits * (uint64_t)piece / (uint64_t)pieces);
}

/*
 * The index a LOOKUP driver reads, found by position as the executor finds
 * it.
 */
static const relic_index_t *
driver_index(const relic_op_atom_t *atom)
{
	const relic_index_t *index;

	index = relic_relation_index_at(atom->relation, atom->index_position);
	assert(relic_index_key_mask(index) == atom->key_mask);

	return (index);
}

/*
 * The probe a LOOKUP driver's key is found by: each key column's constant
 * in that column.  Nothing is bound before a driver, so every key column
 * is a constant (plan.h); the other columns are not read.
 */
static void
driver_probe(const relic_op_atom_t *atom, relic_dom_t *probe)
{
	uint16_t column;

	for (column = 0; column < atom->arity; column++) {
		if ((atom->key_mask & (UINT32_C(1) << column)) == 0)
			continue;
		assert(atom->terms[column].action == RELIC_PLAN_CONSTANT);
		probe[column] = atom->terms[column].u.constant;
	}
}
