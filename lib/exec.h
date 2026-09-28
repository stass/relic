/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_EXEC_H
#define RELIC_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <relic.h>

#include "frozen.h"
#include "plan.h"
#include "program.h"
#include "util.h"

/*
 * The executor (lib/exec.c): one plan run, a call at a time, over relations
 * that nothing modifies during a call.
 *
 * Everything the executor changes is in one scratch, which its caller owns
 * and hands it: the frame of slot values, one state per operation of the
 * plan, and the batch of head tuples the EMIT derives.  Everything else it
 * reads -- the context, the deltas, every relation and index -- it reaches
 * as frozen.h describes, through types it cannot see inside, so a write to
 * any of them from the executor does not compile.  The one word it stores
 * to outside its scratch is a key's frontier, which relic_index_first_since()
 * moves.
 *
 * The executor never inserts.  It fills the batch and returns when the batch
 * is full, having saved where it was, and the caller decides where the
 * batch goes -- inserted into the relation the EMIT names, delivered to a
 * caller as rows, or kept aside -- and empties it before resuming.  So one
 * executor serves every way a plan's derivations are used, and nothing it
 * does can fail.
 *
 * Between two calls the caller may insert into the relations, the plan's
 * own among them, which appends tuples and may move the storage holding
 * them.  It may not remove, clear or discard anything while a plan is
 * unfinished.  A plan keeps tuple identifiers and places on index chains
 * from one call to the next, and finds each tuple and each index afresh by
 * identifier and position, so an append is invisible to it -- every tuple
 * appended lies past the views it resolved when it started -- and a removal
 * would leave it holding identifiers that name nothing.
 */

/*
 * How many tuples the executor's batch holds, and so how many tuples a plan
 * derives before its caller takes them, and how many
 * relic_relation_insert_batch() prefetches for at a time.  Measured on the
 * closure workloads: 16 overlaps most of the misses of a duplicate check, 64
 * a little more, and 256 no more than 64.
 */
#define	RELIC_INSERT_BATCH	64u

typedef struct relic_exec_counters relic_exec_counters_t;
typedef struct relic_exec_env relic_exec_env_t;
typedef struct relic_mixed_note relic_mixed_note_t;
typedef struct relic_op_state relic_op_state_t;
typedef struct relic_scratch relic_scratch_t;

/*
 * What a running plan reads besides its relations: the context, for
 * comparing values; the deltas the views of a member are resolved from,
 * indexed by predicate identifier minus one, or NULL for an evaluation that
 * runs no fixed point and whose every view is therefore `all`; and whether
 * to record a comparison that failed for ordering values of different types,
 * which RELIC_F_NOTE_MIXED asks for.
 */
struct relic_exec_env {
	const relic_t			*ctx;
	const relic_tuple_range_t	*deltas;
	bool				 note_mixed;
};

/*
 * The executor's state for one operation of the running plan.  `view` is the
 * range of identifiers the operation's view covers, resolved for every
 * operation that reads a relation when the plan starts.  `cursor` is the
 * range a SCAN has still to visit, copied from the view each time the scan
 * starts.
 *
 * A LOOKUP uses its cursor differently.  `first` is the next tuple on the
 * index chain, or RELIC_ID_NONE when the chain is exhausted, and `end` is the
 * view's end, copied when the lookup starts.  A chain is in identifier
 * order, so the lookup stops at the first tuple at or past `end`, exactly as
 * a scan stops at the end of its range.
 *
 * A test keeps no cursor, and a FILTER and the EMIT read no relation and
 * leave the view unset too.
 */
struct relic_op_state {
	relic_tuple_range_t	view;
	relic_tuple_range_t	cursor;
};

/*
 * The work the executor has counted since its caller last took the counts:
 * the tuples its scans and its lookups read.  Each is defined where
 * relic_stats_t documents the counter of the same name.
 */
struct relic_exec_counters {
	uint64_t	tuples_scanned;
	uint64_t	tuples_looked_up;
};

/*
 * The first ordering comparison between values of different types a FILTER
 * of the running plan met, when the environment asks for such comparisons,
 * since the caller last took the record: the filter, NULL when there has
 * been none, and the two values it compared.  The executor records and
 * never reports, since reporting calls the caller's diagnostic callback and
 * marks the rule as reported, both of which change the context; the caller
 * reports the record when it takes the batch, which is before any tuple
 * derived after the comparison can have been inserted or delivered.  Only
 * the first is kept, because a rule is reported on at most once and every
 * plan running here is of one rule.
 */
struct relic_mixed_note {
	const relic_op_filter_t	*filter;
	relic_dom_t		 left;
	relic_dom_t		 right;
};

/*
 * Everything the executor writes while it runs one plan.
 *
 * `plan` and `env` are the plan being run and what it reads, set when the
 * plan starts, so that every call that continues it continues the same one.
 * They are meaningful until the plan finishes, after which they may point
 * at what the caller has released, and the next start sets them afresh.
 *
 * `frame` holds one value per slot.  A slot is written by the operation that
 * binds it and read only by operations after that one, so a new binding
 * simply overwrites the old: nothing is saved on descending and nothing
 * restored on backtracking, because no operation reads a slot a later one
 * binds.
 *
 * `states` has room for the operations of a plan of `op_room` operations,
 * one state each, so that the executor's level indexes a plan's operations
 * and their states alike.
 *
 * `batch` holds `batch_count` head tuples the EMIT has derived and the
 * caller has not yet taken, packed at the arity of the relation the EMIT
 * names, with room for RELIC_INSERT_BATCH tuples of `arity_room` columns.
 *
 * `level` and `resume` are where the join was when the executor returned
 * with a full batch: the operation to ask next and whether to ask it for
 * its next match rather than a first.  `finished` is set once the first
 * operation has nothing more, after which the plan has derived everything
 * it will; a scratch no plan has started on is finished too, so that
 * running it derives nothing.
 *
 * `counters` and `note` are what the plans run on the scratch have counted
 * and met since the caller last took them.  The counters accumulate from
 * one plan to the next until they are taken; the note is taken before
 * another plan starts, since it names a filter of the plan that met it.
 *
 * The scratch belongs to one thread for as long as a plan runs on it.
 */
struct relic_scratch {
	const relic_plan_t	*plan;
	const relic_exec_env_t	*env;
	relic_dom_t		 frame[RELIC_MAX_VARIABLES];
	relic_op_state_t	*states;
	relic_dom_t		*batch;
	size_t			 op_room;
	size_t			 batch_count;
	size_t			 level;
	relic_exec_counters_t	 counters;
	relic_mixed_note_t	 note;
	uint16_t		 arity_room;
	bool			 resume;
	bool			 finished;
};

/*
 * Why relic_exec_run() returned: the batch is full and the plan has more to
 * derive, or the plan has derived everything it will.  Either way the batch
 * may hold tuples, which the caller takes before anything else.
 */
enum relic_exec_status {
	RELIC_EXEC_BATCH_FULL,
	RELIC_EXEC_FINISHED
};

/*
 * relic_scratch_init() gives a scratch the storage it runs in: room for the
 * states of a plan of `op_room` operations, and for a batch of
 * RELIC_INSERT_BATCH tuples of `arity_room` columns, both owned by the
 * caller.  The scratch starts empty and finished.
 *
 * relic_exec_start() readies the scratch to run `plan` from the beginning,
 * over what `env` gives it, resolving the view of every operation that
 * reads a relation, with an empty batch.  The plan must fit the scratch,
 * and `plan` and `env` must stay valid until the plan has finished or is
 * abandoned.  relic_exec_run() then runs it, from where it last returned,
 * until the batch is full or the plan has finished, and says which; a
 * caller that runs it again without emptying a full batch is told the batch
 * is full, and nothing is derived.  Between two calls the relations may be
 * appended to and nothing else, as the executor's description above says.
 *
 * relic_exec_view() gives the range of identifiers an operation's view
 * covers under `env`.  relic_exec_start_piece() starts a plan whose first
 * operation is a SCAN as relic_exec_start() does, except that the scan
 * visits only `piece`, which must lie within its view.  Everything below the
 * first operation reads only the binding it makes and the relations, so the
 * plan then derives exactly what the whole plan derives while its first
 * cursor is inside the piece, in the same order, and a view cut into pieces
 * run one after another derives what the whole plan does.
 */
void	 relic_scratch_init(relic_scratch_t *scratch, relic_op_state_t *states,
	    size_t op_room, relic_dom_t *batch, uint16_t arity_room);
void	 relic_exec_start(relic_scratch_t *scratch,
	    const relic_exec_env_t *env, const relic_plan_t *plan);
void	 relic_exec_start_piece(relic_scratch_t *scratch,
	    const relic_exec_env_t *env, const relic_plan_t *plan,
	    relic_tuple_range_t piece);
enum relic_exec_status relic_exec_run(relic_scratch_t *scratch);
relic_tuple_range_t relic_exec_view(const relic_exec_env_t *env,
	    const relic_op_atom_t *atom);

#endif /* RELIC_EXEC_H */
