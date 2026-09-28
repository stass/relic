/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_ROUND_H
#define RELIC_ROUND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <relic.h>

#include "exec.h"
#include "frozen.h"
#include "plan.h"
#include "program.h"
#include "util.h"

/*
 * Deferred rounds: a round of evaluation whose insertions all wait for its
 * end.
 *
 * A round's items are cut into tasks, and each task runs over a store that
 * nothing writes while the round runs, gathering what it derives as
 * candidates.  Worker 0 -- the calling thread -- then merges the candidates
 * into the relations in the order the serial evaluator would have inserted
 * them.  The task side (lib/task.c) sees the store only through frozen.h,
 * as the executor does, and this header is written for it: it names a
 * relation, a context and an index and never looks inside one.  Worker 0's
 * side (lib/round.c) prepares the pool, cuts the tasks and merges.
 */

typedef struct relic_candidate relic_candidate_t;
typedef struct relic_candidates relic_candidates_t;
typedef struct relic_pool relic_pool_t;
typedef struct relic_round relic_round_t;
typedef struct relic_round_item relic_round_item_t;
typedef struct relic_round_job relic_round_job_t;
typedef struct relic_round_task relic_round_task_t;
typedef struct relic_worker relic_worker_t;

/*
 * One item of a round, what the serial evaluator runs in one turn: a seed,
 * whose `plan` is NULL and whose head is all constants, or a plan.  `rule`
 * is what the item was built from.  `target` is the position among the
 * round's targets of the relation the item's tuples go into, which for a
 * plan is the relation its EMIT names.  `gained` is set once the item's
 * turn has inserted a tuple its target did not hold, whichever way the
 * round ran.
 */
struct relic_round_item {
	const relic_plan_t	*plan;
	const relic_rule_t	*rule;
	size_t			 target;
	bool			 gained;
};

/*
 * A round: its `item_count` items, in the order the serial evaluator runs
 * them, in room for `item_capacity`; the relations they write; and what
 * their plans read besides the relations.
 *
 * A round may wait for its end to insert only when no item reads a target
 * except through a view fixed before the round began -- a semi-naive
 * delta, or the `old` or `current` view of one -- so that nothing the round
 * inserts is read by the round.  relic_round_run() asks before it runs one.
 */
struct relic_round {
	relic_round_item_t	 *items;
	relic_relation_t	**targets;
	const relic_exec_env_t	 *env;
	size_t			  item_count;
	size_t			  item_capacity;
	size_t			  target_count;
};

/*
 * One task: an item run over its first operation's whole view or, when
 * `cut`, over the contiguous piece `piece` of it.  A seed and a plan whose
 * first operation is not a SCAN are never cut.
 *
 * The rest is written by the worker that runs the task and read by the
 * merge once every task has run: which worker ran it, where its candidates
 * lie among that worker's, and the first mixed-type comparison it met.
 */
struct relic_round_task {
	size_t			 item;
	relic_tuple_range_t	 piece;
	bool			 cut;

	size_t			 worker;
	size_t			 first_candidate;
	size_t			 candidate_count;
	relic_mixed_note_t	 note;
};

/*
 * A tuple a worker derived that the round's frozen store does not hold and
 * that none of the worker's earlier candidates is: the target it is for,
 * where its columns begin in the worker's `words`, and its hash, which the
 * merge probes by without hashing again.
 */
struct relic_candidate {
	size_t		offset;
	uint32_t	target;
	uint32_t	hash;
};

/*
 * A worker's candidates for the round being run, in the order it derived
 * them: `entries` and, end to end, their columns in `words`.
 *
 * `screen` finds a candidate by its target and tuple, holding candidate
 * positions plus one, so that a tuple the worker has already gathered is
 * not gathered again; one table serves every target of the round.
 *
 * `charged` is what the three arrays have been charged to the memory
 * account, which is always what they hold at their capacities: a growth is
 * charged at its projection before it is made, and whatever it did not
 * reach is given back at once.  They hold data and count against the limit
 * while they exist, and they exist only while a round runs: they are
 * released, memory and charge alike, before it ends.
 */
struct relic_candidates {
	relic_candidate_t	*entries;
	relic_dom_t		*words;
	relic_idtab_t		 screen;
	size_t			 count;
	size_t			 capacity;
	size_t			 word_count;
	size_t			 word_capacity;
	size_t			 charged;
};

/*
 * What one worker holds: the scratch its tasks run in, with the storage for
 * its operation states and its batch, which is kept from one round to the
 * next and never charged; its candidates, which exist only while a round
 * runs; and the derivations its tasks made, which the scratch does not
 * count.  The scratch's counters and the derivations are taken by worker 0
 * after the round, or forgotten when it is abandoned.
 */
struct relic_worker {
	relic_scratch_t		 scratch;
	relic_op_state_t	*states;
	relic_dom_t		*batch;
	size_t			 state_capacity;
	size_t			 batch_capacity;
	relic_candidates_t	 candidates;
	uint64_t		 derivations;
};

/*
 * A context's pool: its workers, the first of which is the calling thread,
 * the tasks of the round being run, and for each target of the round the
 * number of candidates its tasks gathered.  The arrays are kept from one
 * round to the next, grown to the largest round the pool has run; they are
 * sized by the program and not by the data, and are not charged.
 *
 * `merged_rounds` and `abandoned_rounds` count the rounds the pool merged
 * and those it ran and then left for the caller to run serially, over the
 * life of the context, so that a test can tell that the deferred way was
 * taken and did not merely fall back.
 */
struct relic_pool {
	relic_worker_t		*workers;
	relic_round_task_t	*tasks;
	size_t			*target_candidates;
	size_t			 worker_count;
	size_t			 task_count;
	size_t			 task_capacity;
	size_t			 target_capacity;
	uint64_t		 merged_rounds;
	uint64_t		 abandoned_rounds;
};

/*
 * A round as its tasks see it while they run.
 *
 * The round and everything it reaches are read-only, and so is the store.
 * The account is the one thing outside a worker's own a task writes, when a
 * candidate buffer grows, and it does so only through the round charge
 * frozen.h declares.  `admissible` is how many more tuples the tuple
 * limit can admit, SIZE_MAX when none is set: a worker gathering more
 * candidates than that holds a round the merge would refuse part way, and
 * abandons it rather than fill memory first.
 *
 * `next_task` is the first task not yet claimed, and `abandoned` is set by a
 * task that cannot go on, after which no task is claimed.
 */
struct relic_round_job {
	const relic_round_t	*round;
	relic_round_task_t	*tasks;
	relic_account_t		*account;
	size_t			 task_count;
	size_t			 admissible;
	size_t			 next_task;
	bool			 abandoned;
};

/*
 * How a round offered for deferral ended: merged, its candidates inserted;
 * declined, every relation, counter and account total as it was, for the
 * caller to run the round serially -- because it reads what it writes, the
 * pool could not be had, or a task could not go on or the merge would not
 * fit; or failed in its merge, the refusal reported as the serial
 * evaluator reports it.
 */
enum relic_round_outcome {
	RELIC_ROUND_MERGED,
	RELIC_ROUND_DECLINED,
	RELIC_ROUND_FAILED
};

/*
 * lib/round.c
 *
 * relic_round_run() runs a round deferred on the context's pool, creating
 * the pool the first time, unless the round reads a relation it writes,
 * and says how it ended; an item that inserted a tuple its target lacked
 * is marked as having gained when the round merged.  relic_pool_free()
 * releases a pool and everything it kept.
 */
enum relic_round_outcome relic_round_run(relic_t *ctx, relic_round_t *round);
void	 relic_pool_free(relic_pool_t *pool);

/*
 * lib/task.c
 *
 * relic_round_work() claims the job's tasks one after another and runs each
 * on the worker, until none is left or one could not go on, which it
 * records in the job.  relic_candidates_init() readies a worker's empty
 * candidates, and relic_candidates_release() releases them, giving the
 * account back what they were charged.
 */
void	 relic_round_work(relic_round_job_t *job, relic_worker_t *worker,
	    size_t worker_index);
void	 relic_candidates_init(relic_candidates_t *candidates);
void	 relic_candidates_release(relic_candidates_t *candidates,
	    relic_account_t *account);

#endif /* RELIC_ROUND_H */
