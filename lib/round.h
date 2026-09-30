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
 * candidates.  The candidates are then merged into the relations as the
 * serial evaluator would have inserted them.  The task side (lib/task.c)
 * sees the store only through frozen.h, as the executor does, and this
 * header is written for it: it names a relation, a context and an index
 * and never looks inside one.  Worker 0 -- the calling thread -- decides
 * whether a round is worth sharing, prepares the pool and cuts the tasks
 * (lib/round.c), and leads the merge, whose steps the pool's workers share
 * (lib/merge.c).  The pool's threads, and how a job is handed to them and
 * waited for, are lib/pool.c's.
 */

typedef struct relic_candidate relic_candidate_t;
typedef struct relic_candidates relic_candidates_t;
typedef struct relic_plan_history relic_plan_history_t;
typedef struct relic_pool relic_pool_t;
typedef struct relic_pool_job relic_pool_job_t;
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
 * hashing again, and the target it is for.  The hash of a tuple holding a
 * pending integer is over its markers until the merge rewrites them to
 * identifiers and hashes it again.  `repeat` is the merge's: set
 * when another worker's earlier candidate is the same tuple for the same
 * target, so that this one is not new after all.
 */
struct relic_candidate {
	size_t		offset;
	uint64_t	hash;
	uint32_t	target;
	bool		repeat;
};

/*
 * The hash a candidate is screened under, by its worker and by the merge:
 * the low half of its tuple's wide hash moved by its target, so that one
 * tuple gathered for two targets does not fill one probe sequence.  For
 * each target the move is an exclusive or with a constant, a permutation
 * of the hashes, so hashes that spread still spread.
 */
static inline uint32_t
relic_candidate_screen_hash(size_t target, uint64_t hash)
{
	return (relic_hash_low(hash) ^ ((uint32_t)target *
	    UINT32_C(0x9e3779b9)));
}

/*
 * A worker's candidates for the round being run, in the order it derived
 * them: `entries` and, end to end, their columns in `words`.  `stripes`
 * holds each candidate's stripe of its target's canonical index, a byte a
 * candidate, so that the merge's worker for a stripe finds the stripe's
 * candidates by reading a byte of each rather than the whole entry, and
 * `by_stripe` counts them.
 *
 * `screen` finds a candidate by its target and tuple, holding candidate
 * positions plus one, so that a tuple the worker has already gathered is
 * not gathered again; one table serves every target of the round.
 *
 * `values` holds the distinct values the worker's candidates hold
 * pending -- integers the value table lacked and strings the context had
 * not assigned when the round began -- in the order the worker first
 * derived them, a string's bytes in `value_bytes`, and `value_screen`
 * finds one by value, holding its position plus one, hashed under the
 * context's `hash_seed`: an integer by its value, a string by its bytes.
 * A candidate's column holding such a value holds that position plus one,
 * which has no type bits and so names no value: equal values hold equal
 * markers, so a candidate is screened by value as by identifier, and a
 * marker is never found in the frozen store, where no value the context
 * lacked can be.  The merge replaces every marker with the identifier the
 * value will take.
 *
 * `charged` is what the arrays have been charged to the memory account,
 * which is always what they hold at their capacities: a growth is charged
 * at its projection before it is made, and whatever it did not reach is
 * given back at once.  They hold data and count against the limit while
 * they exist, and they exist only while a round runs: they are released,
 * memory and charge alike, before it ends.
 */
struct relic_candidates {
	relic_candidate_t	*entries;
	uint8_t			*stripes;
	relic_dom_t		*words;
	relic_idtab_t		 screen;
	relic_pending_t		*values;
	char			*value_bytes;
	relic_idtab_t		 value_screen;
	uint64_t		 hash_seed;
	size_t			 by_stripe[RELIC_STRIPES];
	size_t			 count;
	size_t			 capacity;
	size_t			 stripe_capacity;
	size_t			 word_count;
	size_t			 word_capacity;
	size_t			 value_count;
	size_t			 value_capacity;
	size_t			 value_byte_count;
	size_t			 value_byte_capacity;
	size_t			 charged;
};

/*
 * What one worker holds: the scratch its tasks run in, with the storage for
 * its operation states, its batch and the batch's pending values, which
 * is kept from one round to the next and never charged; its candidates,
 * which exist only while a round runs; and the derivations its tasks made,
 * which the scratch does not count.  The scratch's counters and the
 * derivations are taken by worker 0 after the round, or forgotten when it
 * is abandoned.
 */
struct relic_worker {
	relic_scratch_t		 scratch;
	relic_op_state_t	*states;
	relic_dom_t		*batch;
	relic_pending_t		*pending;
	size_t			 state_capacity;
	size_t			 batch_capacity;
	size_t			 pending_capacity;
	relic_candidates_t	 candidates;
	uint64_t		 derivations;
};

/*
 * A context's pool: its `worker_count` workers, the first of which is the
 * calling thread and each other of which is a thread of `threads`, NULL
 * when there is none; and the tasks of the round being run.  The arrays are
 * kept from one round to the next, grown to the largest round the pool has
 * run; they are sized by the program and the thread count and not by the
 * data, and are not charged.
 *
 * `merged_rounds` and `abandoned_rounds` count the rounds the pool merged
 * and those it ran and then left for the caller to run serially, over the
 * life of the context, so that a test can tell that the deferred way was
 * taken and did not merely fall back; `limit_declines` counts the merges
 * declined because the tuple or the memory limit would have refused what
 * they insert, so that a test can tell a limit reached the merge itself
 * rather than a round's buffers.  `thread_tasks`
 * counts the tasks of merged rounds that a thread of the pool ran rather
 * than the calling thread, so that a test can tell the threads did work.
 */
struct relic_pool {
	relic_worker_t		*workers;
	relic_pool_threads_t	*threads;
	relic_round_task_t	*tasks;
	size_t			 worker_count;
	size_t			 task_count;
	size_t			 task_capacity;
	uint64_t		 merged_rounds;
	uint64_t		 abandoned_rounds;
	uint64_t		 limit_declines;
	uint64_t		 thread_tasks;
};

/*
 * A round's tasks as the pool's workers run them, each worker the tasks it
 * claims (relic_pool_for()) in increasing order.
 *
 * The round and everything it reaches are read-only, and so is the store.
 * The account is the one thing outside a worker's own a task writes, when a
 * candidate buffer grows, and it does so only through the round charge
 * frozen.h declares.  `admissible` is how many more tuples the tuple
 * limit can admit, SIZE_MAX when none is set: a worker gathering more
 * candidates than that holds a round the merge would refuse part way, and
 * abandons it rather than fill memory first.  `abandoned` is set by a
 * worker whose task cannot go on, after which a task claimed is not run.
 */
struct relic_round_job {
	const relic_round_t	*round;
	relic_round_task_t	*tasks;
	relic_worker_t		*workers;
	relic_account_t		*account;
	size_t			 admissible;
	relic_atomic_bool_t	 abandoned;
};

/*
 * A part of a job the pool shares: what relic_pool_for() runs for part
 * `part` on the worker at `worker_index`, with the job's argument.
 */
typedef void	(*relic_pool_part_fn)(void *argument, size_t part,
		    size_t worker_index);

/*
 * lib/round.c
 *
 * relic_round_run() runs a round deferred on the context's pool, creating
 * the pool the first time, and returns whether it did.  When it did, the
 * round's candidates are inserted and every item that inserted a tuple its
 * target lacked is marked as having gained.  When it did not, every
 * relation holds the tuples it held, every counter is as it was and the
 * round has given back its buffers, for the caller to run the round
 * serially; only a reservation the allocator refused part way leaves some
 * of a relation's arrays, or the integer table's, larger, counted in the
 * storage total, for the serial run to grow into.  A round is not run
 * deferred when it reads a relation it writes, when it is too small to
 * repay sharing it among the pool's threads -- unless
 * RELIC_F_PARALLEL_ALWAYS asks for every round -- or when the pool, or the
 * workers' buffers for the strings a calling round computes, could not be
 * had, and is abandoned when a task could not go on or its merge declined.
 * A deferred round's merge cannot fail: whatever could refuse it is asked
 * before anything is inserted, and a refusal declines the round.
 *
 * relic_round_record() records in an item's history, when it has one,
 * that its turn in a round run serially did `work`, the tuples it read and
 * derived.  relic_round_first_call() gives the first item of a round whose
 * plan calls a function, or NULL when none does.
 */
bool	 relic_round_run(relic_t *ctx, relic_round_t *round);
void	 relic_round_record(const relic_round_t *round,
	    relic_round_item_t *item, uint64_t work);
const relic_round_item_t *relic_round_first_call(const relic_round_t *round);

/*
 * lib/merge.c
 *
 * relic_round_merge() inserts the candidates a round's tasks gathered into
 * the round's targets, as the serial evaluator would have inserted them, on
 * the pool's workers.  Returns true once it has, or false having inserted
 * nothing, when a limit or the allocator would refuse the merge, for the
 * round to be run serially.  `texts_bytes` is at least what the serial
 * run's text buffers would hold by the round's end, which the memory limit
 * is asked of beside the growth.  The candidates are the caller's to release.
 */
bool	 relic_round_merge(relic_t *ctx, relic_pool_t *pool,
	    relic_round_t *round, size_t texts_bytes);

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
 * workers.  `hash_seed` is the seed of the context the pool serves.
 * Returns 0, or -1 when the pool could not be allocated.
 *
 * relic_pool_adopt() makes the pool the calling process's before a job:
 * a pool carried into a child of fork(2) has none of its threads there,
 * and becomes the calling thread alone.
 *
 * relic_pool_for() runs `run_part` for each of `part_count` parts, on every
 * worker of the pool, the calling thread as worker 0, and returns once
 * every part has run and every worker has left the job, when everything
 * the parts wrote is visible to the caller.  A worker claims the first part
 * not yet claimed, one at a time, so each part runs once and each worker
 * runs its parts in increasing order.  A job of one part, or none, runs on
 * the calling thread alone, as it would have anyway, without waking the
 * pool.
 *
 * relic_pool_free() stops and joins the pool's threads and releases the
 * pool and everything it kept.
 */
size_t	 relic_pool_size(size_t threads);
int	 relic_pool_create(relic_pool_t **poolp, size_t workers,
	    uint64_t hash_seed);
void	 relic_pool_adopt(relic_pool_t *pool);
void	 relic_pool_for(relic_pool_t *pool, size_t part_count,
	    relic_pool_part_fn run_part, void *argument);
void	 relic_pool_free(relic_pool_t *pool);

/*
 * lib/task.c
 *
 * relic_round_run_task() is the part a round job's tasks are shared by: it
 * runs task `task` on the worker at `worker_index`, unless the job has been
 * abandoned, and abandons the job when the task cannot go on.
 * relic_candidates_init() readies a worker's empty candidates, and
 * relic_candidates_release() releases them, giving the account back what
 * they were charged.
 */
void	 relic_round_run_task(void *job, size_t task, size_t worker_index);
void	 relic_candidates_init(relic_candidates_t *candidates,
	    uint64_t hash_seed);
void	 relic_candidates_release(relic_candidates_t *candidates,
	    relic_account_t *account);

#endif /* RELIC_ROUND_H */
