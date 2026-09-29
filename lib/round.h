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

#include "atomic.h"
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
 * side (lib/round.c) decides whether a round is worth sharing, prepares the
 * pool, cuts the tasks and merges.  The pool's threads, and how a job is
 * handed to them and waited for, are lib/pool.c's.
 */

typedef struct relic_candidate relic_candidate_t;
typedef struct relic_candidates relic_candidates_t;
typedef struct relic_plan_history relic_plan_history_t;
typedef struct relic_pool relic_pool_t;
typedef struct relic_pool_threads relic_pool_threads_t;
typedef struct relic_round relic_round_t;
typedef struct relic_round_item relic_round_item_t;
typedef struct relic_round_job relic_round_job_t;
typedef struct relic_round_task relic_round_task_t;
typedef struct relic_worker relic_worker_t;

/*
 * What a plan did the last time it ran, from which the next run's work is
 * estimated: `work`, the tuples it read and derived, and `range`, the
 * tuples its first scan's view held then, one for a plan that is not cut.
 * `ran` is false until it has run.
 */
struct relic_plan_history {
	uint64_t	work;
	uint64_t	range;
	bool		ran;
};

/*
 * One item of a round, what the serial evaluator runs in one turn: a seed,
 * whose `plan` is NULL and whose head is all constants, or a plan.  `rule`
 * is what the item was built from.  `history` is the plan's, where one is
 * kept, and NULL otherwise.  `target` is the position among the round's
 * targets of the relation the item's tuples go into, which for a plan is
 * the relation its EMIT names.  `gained` is set once the item's turn has
 * inserted a tuple its target did not hold, whichever way the round ran.
 */
struct relic_round_item {
	const relic_plan_t	*plan;
	const relic_rule_t	*rule;
	relic_plan_history_t	*history;
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
 * lie among that worker's, the first mixed-type comparison it met, and the
 * work it did, the tuples it read and derived.
 */
struct relic_round_task {
	size_t			 item;
	relic_tuple_range_t	 piece;
	bool			 cut;

	size_t			 worker;
	size_t			 first_candidate;
	size_t			 candidate_count;
	relic_mixed_note_t	 note;
	uint64_t		 work;
};

/*
 * A tuple a worker derived that the round's frozen store does not hold and
 * that none of the worker's earlier candidates is: where its columns begin
 * in the worker's `words`, its wide hash, which the merge probes by without
 * hashing again, and the target it is for.
 */
struct relic_candidate {
	size_t		offset;
	uint64_t	hash;
	uint32_t	target;
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
 * A context's pool: its `worker_count` workers, the first of which is the
 * calling thread and each other of which is a thread of `threads`, NULL
 * when there is none; the tasks of the round being run; and for each
 * target of the round the number of candidates its tasks gathered.  The
 * arrays are kept from one round to the next, grown to the largest round
 * the pool has run; they are sized by the program and the thread count and
 * not by the data, and are not charged.
 *
 * `merged_rounds` and `abandoned_rounds` count the rounds the pool merged
 * and those it ran and then left for the caller to run serially, over the
 * life of the context, so that a test can tell that the deferred way was
 * taken and did not merely fall back.  `thread_tasks` counts the tasks of
 * merged rounds that a thread of the pool ran rather than the calling
 * thread, so that a test can tell the threads did work.
 */
struct relic_pool {
	relic_worker_t		*workers;
	relic_pool_threads_t	*threads;
	relic_round_task_t	*tasks;
	size_t			*target_candidates;
	size_t			 worker_count;
	size_t			 task_count;
	size_t			 task_capacity;
	size_t			 target_capacity;
	uint64_t		 merged_rounds;
	uint64_t		 abandoned_rounds;
	uint64_t		 thread_tasks;
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
 * Three words are shared by every worker of the job and are atomic.
 * `next_task` is the first task not yet claimed: a worker claims a task by
 * taking it and moving it past, so each task is claimed once and each
 * worker claims its tasks in increasing order.  `abandoned` is set by a
 * worker whose task cannot go on, after which no worker claims another.
 * `finished` counts the workers that have left their claim loop, for
 * whatever reason; worker 0 waits for it to reach the pool's workers, and
 * what every worker wrote -- its tasks' records, its candidates, its
 * counters -- is then visible to it.
 */
struct relic_round_job {
	const relic_round_t	*round;
	relic_round_task_t	*tasks;
	relic_account_t		*account;
	size_t			 task_count;
	size_t			 admissible;
	relic_atomic_size_t	 next_task;
	relic_atomic_size_t	 finished;
	relic_atomic_bool_t	 abandoned;
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
 * the pool the first time, and says how it ended.  It declines a round
 * that reads a relation it writes, and, unless RELIC_F_PARALLEL_ALWAYS
 * asks for every round, one too small to repay sharing it among the
 * pool's threads.  An item that inserted a tuple its target lacked is
 * marked as having gained when the round merged.
 *
 * relic_round_record() records in an item's history, when it has one,
 * that its turn in a round run serially did `work`, the tuples it read and
 * derived.
 */
enum relic_round_outcome relic_round_run(relic_t *ctx, relic_round_t *round);
void	 relic_round_record(const relic_round_t *round,
	    relic_round_item_t *item, uint64_t work);

/*
 * lib/pool.c
 *
 * relic_pool_size() is how many workers a pool asked for `threads` can
 * have in this build: that many, zero counting as one, or one alone in a
 * build without threads.
 *
 * relic_pool_create() gives a pool `workers` workers, at least one, the
 * calling thread among them, starting a thread for each other it can; a
 * thread that cannot be started is not an error, and the pool has fewer
 * workers.  Returns 0, or -1 when the pool could not be allocated.
 *
 * relic_pool_adopt() makes the pool the calling process's before a job:
 * a pool carried into a child of fork(2) has none of its threads there,
 * and becomes the calling thread alone.
 *
 * relic_pool_run() runs a job on every worker of the pool, the calling
 * thread as worker 0, and returns once every worker has left the job.
 *
 * relic_pool_free() stops and joins the pool's threads and releases the
 * pool and everything it kept.
 */
size_t	 relic_pool_size(size_t threads);
int	 relic_pool_create(relic_pool_t **poolp, size_t workers);
void	 relic_pool_adopt(relic_pool_t *pool);
void	 relic_pool_run(relic_pool_t *pool, relic_round_job_t *job);
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
