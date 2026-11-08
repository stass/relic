/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
#include "destination.h"
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
typedef struct relic_dense relic_dense_t;
typedef struct relic_insert_history relic_insert_history_t;
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
 * What a plan did the last time it ran, from which the next run's work and
 * derivations are estimated: `work`, the tuples it read and derived,
 * `derived`, the tuples it derived alone, each one candidate offered to the
 * round's collector, `range`, the tuples its first scan's view held then,
 * one for a plan that is not cut, and `prediction`, the tuples the work
 * estimator expected it to derive then, zero for a plan it does not
 * predict.  `ran` is false until it has run.
 */
struct relic_plan_history {
	uint64_t	work;
	uint64_t	derived;
	uint64_t	range;
	uint64_t	prediction;
	bool		ran;
};

/*
 * What an item inserted the last time it ran, from which the candidates
 * its next run gathers are expected: `inserted`, the tuples it inserted
 * that its target lacked.  `ran` is false until it has run.  The plans
 * that run one delta variant of a rule derive the same tuples, whichever
 * a round chooses, and share one such record; their work differs, and
 * each keeps its own plan history.
 */
struct relic_insert_history {
	uint64_t	inserted;
	bool		ran;
};

/*
 * One item of a round, what the serial evaluator runs in one turn: a seed,
 * whose `plan` is NULL and whose head is all constants, or a plan.  `rule`
 * is what the item was built from.  `history` is the plan's and
 * `insertions` the record of what the item inserted, where they are kept,
 * and NULL otherwise.  `target` is the position among the round's
 * targets of the relation the item's tuples go into, which for a plan is
 * the relation the caller names for those tuples.  `prediction` is the
 * tuples the work estimator expects the plan to derive in this round
 * (relic_cost_derivations()), taken before the round runs, when `predicted`
 * is set: for the plan of a delta variant whose history is kept.  `gained`
 * is set once the item's turn has inserted a tuple its target did not
 * hold, whichever way the round ran.
 */
struct relic_round_item {
	const relic_plan_t	*plan;
	const relic_rule_t	*rule;
	relic_plan_history_t	*history;
	relic_insert_history_t	*insertions;
	size_t			 target;
	uint64_t		 prediction;
	bool			 predicted;
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
	relic_destination_t	*const *targets;
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
 * The worker that runs the task then writes, for the merge to read once
 * every task has run, which worker ran it, where its candidates lie among
 * that worker's, the first mixed-type comparison it met, the work it did,
 * the tuples it read and derived, and the tuples it derived alone.
 * `inserted` is the merge's: how many of the task's candidates are tuples
 * new to its target.
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
	uint64_t		 derived;

	size_t			 inserted;
};

/*
 * A tuple a worker derived that the round's frozen store does not hold and
 * that none of the worker's earlier candidates is: where its columns begin
 * in the worker's `words`, its wide hash, which the merge probes by without
 * hashing again, and the target it is for.  The hash of a tuple holding a
 * pending value is over its markers until the merge rewrites them to
 * identifiers and hashes it again. `contribution` is one for a set's first
 * occurrence, or the multiplicity of a counted key. Worker-local repeats
 * combine into their first candidate. The merge combines equal keys into
 * their earliest candidate and zeros later contributions, so zero means the
 * candidate is discarded. The word occupies the set candidate's flags space.
 */
struct relic_candidate {
	size_t		offset;
	uint64_t	hash;
	uint32_t	target;
	uint32_t	contribution;
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
	uint32_t		 dense_sequence;
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
 * `expected_candidates` sums, over the rounds whose candidates the pool
 * reserved room for, the candidates each was expected to gather, so that
 * a test can tell the reservations follow what the rounds before inserted;
 * `reservation_declines` counts the rounds declined because the limit or
 * the allocator refused that room, so that a test can tell a limit
 * reached the reservation.
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
	uint64_t		 expected_candidates;
	uint64_t		 reservation_declines;
	uint64_t		 dense_rounds;	/* dense rounds merged */
};

/*
 * A round's tasks as the pool's workers run them, each worker the tasks it
 * claims (relic_pool_for()) in increasing order.
 *
 * The round and everything it reaches are read-only, and so is the store.
 * Tasks charge candidate growth through the account's frozen interface.
 * The dense collector instead lowers fixed shared ranks. `admissible` is
 * how many more tuples the limit can admit, SIZE_MAX when none is set:
 * a worker gathering more candidates than that holds a round the merge
 * would refuse part way, and abandons it rather than fill memory first.
 * `interrupted` is the context's interrupt flag, which a worker reads after
 * each return of the executor and abandons the job on finding raised, so
 * that the calling thread meets the interrupt in the round's serial run and
 * reports it.
 * `abandoned` is set by a worker whose task cannot go on, after which a
 * task claimed is not run.
 */
struct relic_round_job {
	const relic_round_t		*round;
	relic_round_task_t		*tasks;
	relic_worker_t			*workers;
	relic_account_t			*account;
	const relic_signal_flag_t	*interrupted;
	size_t				 admissible;
	relic_atomic_bool_t		 abandoned;
	relic_dense_t			*dense;
};

/*
 * Dense gathering, ordering and staging look for an interrupt once every
 * RELIC_DENSE_POLL_MASK + 1 cells, tuples or positions.
 */
#define	RELIC_DENSE_POLL_MASK	4095u

/*
 * A rank per cell of the pair grid for one frozen round, snapshotted when
 * the collector opens (relic_pair_grid_take()). Gathering only lowers ranks;
 * after the barrier, order and scratch alternate as radix input and output.
 * Only the final order array is fully sorted by rank.
 * Histograms belong to ordered parts, not the workers claiming those parts.
 * charged covers these arrays alone.
 *
 * The round that opens a collector closes it, after the merge returns; the
 * merge only reads it and steps it forward.  relic_dense_order() succeeds
 * leaving `count` ordered positions in `order` and nothing else, NULL when
 * there are none; when it fails the collector keeps whatever it held, for
 * the close to release.  relic_dense_release_order() gives the positions
 * back once staging has copied them, leaving none.  Every reader asks for
 * positions below `count`, so an empty result needs no case of its own.
 */
struct relic_dense {
	relic_round_job_t	*job;
	relic_pool_t		*pool;
	relic_atomic_u64_t	*ranks;
	uint32_t		*order;
	uint32_t		*scratch;
	size_t			*histograms;
	relic_pair_grid_t	 grid;
	size_t			 count;
	size_t			 parts;
	size_t			 charged;
	unsigned int		 shift;
	bool			 scattering;
};

/*
 * Open returns 1 if selected, 0 if ineligible or not chosen, -1 on private
 * refusal.  `derivations` is the round's expected derivations, the
 * candidates it is expected to offer its collector.
 */
int	 relic_dense_open(relic_dense_t *dense, relic_pool_t *pool,
	    relic_round_job_t *job, uint64_t derivations);
void	 relic_dense_close(relic_dense_t *dense);
bool	 relic_dense_take(relic_dense_t *dense, size_t task,
	    uint32_t *sequence, const relic_dom_t *tuple);
int	 relic_dense_order(relic_dense_t *dense);
void	 relic_dense_release_order(relic_dense_t *dense);
void	 relic_dense_tuple(const relic_dense_t *dense, size_t position,
	    relic_dom_t *tuple);

/*
 * A part of a job the pool shares: what relic_pool_for() runs for part
 * `part` on the worker at `worker_index`, with the job's argument.
 */
typedef void	(*relic_pool_part_fn)(void *argument, size_t part,
		    size_t worker_index);

/*
 * The span at which two processors' accesses to memory interfere: a 64-byte
 * line together with the one Intel's adjacent-line prefetcher fetches with
 * it, or a 128-byte line on Apple's arm64.
 */
#define	RELIC_CACHE_LINE	128

/*
 * A job's filler: serial work of the caller's that worker 0 runs in steps
 * while the pool's other workers run the job's parts.  `step` runs one
 * step with `argument` and returns whether another remains; called when
 * none does, it does nothing and returns false.  A step should be short,
 * since the job can end no sooner than the step in progress does.  What a
 * step writes must lie on cache lines no part reads, or every write takes
 * the line from the processors running the parts; the state a step keeps
 * is kept off theirs by aligning it to RELIC_CACHE_LINE.
 */
typedef bool	(*relic_pool_step_fn)(void *argument);

typedef struct relic_pool_filler relic_pool_filler_t;

struct relic_pool_filler {
	relic_pool_step_fn	 step;
	void			*argument;
};

/*
 * lib/round.c
 *
 * relic_round_run() runs a round deferred on the context's pool, creating
 * the pool the first time.  RELIC_ROUND_MERGED means the round's candidates
 * are inserted and every item that inserted a tuple its target lacked is
 * marked as having gained.  RELIC_ROUND_DECLINED means every relation
 * holds the tuples it held, every counter is as it was and the
 * round has given back its buffers, for the caller to run the round
 * serially; only a reservation the allocator refused part way leaves some
 * of a relation's arrays, or the value table's or the pool's, larger,
 * counted in the storage total, for the serial run to grow into.  A round
 * is not run deferred when it reads a relation it writes, when it is too
 * small to repay sharing it among the pool's threads -- unless
 * RELIC_UNSTABLE_PARALLEL_ALWAYS asks for every round -- or when the pool, the
 * workers' buffers for the strings a calling round computes, or the room
 * reserved for the candidates the round is expected to gather could not be
 * had, and is abandoned when a task could not go on or its merge declined.
 * A deferred round's merge cannot fail: whatever could refuse it is asked
 * before anything is inserted, and a refusal declines the round.
 * RELIC_ROUND_FAILED means reporting a merged round's notes met an
 * interrupt.  Its tuples are already published, so the evaluation fails
 * without replaying the round.  Derived relations remain noncurrent and
 * are discarded before reevaluation.
 *
 * relic_round_record() records in an item's history, when it has one,
 * that its turn in a round run serially did `work`, the tuples it read and
 * derived, derived `derived` tuples, and inserted `inserted` tuples its
 * target lacked, and what the item's prediction was.
 * relic_round_first_call() gives the first item of a round whose
 * plan calls a function, or NULL when none does.
 */
enum relic_round_outcome {
	RELIC_ROUND_DECLINED,
	RELIC_ROUND_MERGED,
	RELIC_ROUND_FAILED
};

enum relic_round_outcome relic_round_run(relic_t *ctx, relic_round_t *round);
void	 relic_round_record(const relic_round_t *round,
	    relic_round_item_t *item, uint64_t work, uint64_t derived,
	    uint64_t inserted);
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
 * is asked of beside the growth. Dense collection supplies ordered positions
 * instead of candidates; the merge orders and stages them, and releases
 * them once staged, but never closes the collector. The caller closes
 * either collector after the merge.
 */
bool	 relic_round_merge(relic_t *ctx, relic_pool_t *pool,
	    relic_round_t *round, size_t texts_bytes, relic_dense_t *dense);

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
 * relic_pool_workers() is the count of workers an operation decides by
 * whether, and how finely, to share its work.  It and relic_pool_for()
 * first make the pool the calling process's: a pool carried into a child
 * of fork(2) has none of its threads there, and becomes the calling thread
 * alone.  An operation reads the count this way before it sizes anything by
 * it, and the count does not change while the operation runs.
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
 * relic_pool_for_filled() is relic_pool_for() with a filler, which may be
 * NULL.  When the job is shared, the calling thread runs the filler's
 * steps before it claims a part, until the filler has none left or every
 * other worker has left the job, and then claims what parts remain; a job
 * the calling thread runs alone runs none.  Whatever the filler has not
 * done when the call returns is the caller's.
 *
 * relic_pool_free() stops and joins the pool's threads and releases the
 * pool and everything it kept.
 *
 * relic_pool_spin_nanoseconds() is how long a waiting worker of a pool of
 * `workers` looks for what it waits for before it parks.  `processors` is
 * the count of processors online, or zero or less when it is unknown.
 * Spinning pays only while the worker waited for runs on another
 * processor, so a pool whose workers outnumber the processors, or that
 * cannot tell, does not spin.
 */
size_t	 relic_pool_size(size_t threads);
int	 relic_pool_create(relic_pool_t **poolp, size_t workers,
	    uint64_t hash_seed);
size_t	 relic_pool_workers(relic_pool_t *pool);
void	 relic_pool_for(relic_pool_t *pool, size_t part_count,
	    relic_pool_part_fn run_part, void *argument);
void	 relic_pool_for_filled(relic_pool_t *pool, size_t part_count,
	    relic_pool_part_fn run_part, void *argument,
	    const relic_pool_filler_t *filler);
void	 relic_pool_free(relic_pool_t *pool);
uint64_t relic_pool_spin_nanoseconds(size_t workers, long processors);

/*
 * lib/task.c
 *
 * relic_round_run_task() is the part a round job's tasks are shared by: it
 * runs task `task` on the worker at `worker_index`, unless the job has been
 * abandoned, and abandons the job when the task cannot go on.
 * relic_candidates_init() readies a worker's empty candidates, and
 * relic_candidates_release() releases them, giving the account back what
 * they were charged.  relic_candidates_reserve() gives a worker's empty
 * candidates room for `tuples` candidates of `words` columns in all before
 * a round's tasks run, charged as a growth is, and returns 0, or -1 when
 * the limit or the allocator refused it, whatever was obtained kept and
 * charged for relic_candidates_release() to give back.
 */
void	 relic_round_run_task(void *job, size_t task, size_t worker_index);
void	 relic_candidates_init(relic_candidates_t *candidates,
	    uint64_t hash_seed);
int	 relic_candidates_reserve(relic_candidates_t *candidates,
	    relic_account_t *account, size_t tuples, size_t words);
void	 relic_candidates_release(relic_candidates_t *candidates,
	    relic_account_t *account);

#endif /* RELIC_ROUND_H */
