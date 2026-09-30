/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <strings.h>

#ifdef RELIC_THREADS
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "internal.h"

/*
 * A context's worker pool, and how a job is handed to its threads.
 *
 * The calling thread is worker 0 and takes part in every job; the pool
 * starts a thread for each other worker when it is created, which is the
 * first time an evaluation shares a round.  Between jobs the threads wait,
 * first spinning for a bounded while, so that a component's rounds in
 * quick succession do not each pay a sleep and a wake-up, and then parked
 * on a condition variable, where they cost nothing.
 *
 * A job is a number of parts, each run once by whichever worker claims it
 * first.  It is published by worker 0 under the pool's mutex: it stores the
 * job and advances the generation, then wakes every parked thread.  A
 * thread that sees the generation move takes the job, claims and runs parts
 * until none is left, and reports that it has finished by counting itself
 * into the job's `finished`.  The mutex orders publication against parking,
 * so no wake-up is lost, and it is taken once per job and once per park,
 * never per part.
 *
 * The barrier is the same shape the other way round: worker 0, out of
 * parts, spins on `finished` and then parks until it reaches the number of
 * workers.  The worker whose report completes the count takes the mutex
 * before it signals, so its signal cannot fall between worker 0's check
 * and worker 0's wait; every other report takes no lock.  A worker touches
 * the job for the last time when it counts itself in, since worker 0 may
 * return and the job end the moment the count is complete.
 *
 * The threads run nothing but parts of jobs -- a round's tasks, the steps
 * of its merge: no planning, no diagnostic and none of the caller's
 * callbacks.  Every signal is blocked in them but those a
 * fault raises, so a signal meant for the caller is delivered to a thread
 * that is the caller's, and a fault in a task ends the process as it would
 * on the calling thread, where a blocked one would repeat forever.  The
 * calling thread cannot be cancelled while it waits for the pool, since
 * the job it waits on lives in its frame and the threads are still reading
 * it.
 *
 * A pool carried into a child of fork(2) has none of the threads it
 * started there.  Before its next job, or when it is destroyed, it releases
 * their memory without joining them, and is the calling thread alone.
 */

/*
 * A job the pool shares: `part_count` parts, each run by `run_part` with
 * `argument`.  `next_part` is the first part not yet claimed: a worker
 * claims a part by taking it and moving the word past it, so each part is
 * claimed once and each worker claims its parts in increasing order; the
 * word may be moved past the last part by workers finding nothing left.
 * `finished` counts the workers that have left the job; worker 0 waits for
 * it to reach the pool's workers, and what every worker wrote is then
 * visible to it.
 */
struct relic_pool_job {
	relic_pool_part_fn	 run_part;
	void			*argument;
	size_t			 part_count;
	relic_atomic_size_t	 next_part;
	relic_atomic_size_t	 finished;
};

static void	 job_work(relic_pool_job_t *job, size_t worker_index);

#ifdef RELIC_THREADS

/*
 * How many times a waiting worker looks at the word it waits on before it
 * parks, pausing between looks.  A constant of performance alone: long
 * enough to cover the merge between two rounds of one component, short
 * enough that a thread with no work gives its core back promptly.
 */
#define	RELIC_SPIN_LIMIT	4096u

typedef struct relic_worker_start relic_worker_start_t;

/*
 * What a thread is started with: its pool and its place among the pool's
 * workers.
 */
struct relic_worker_start {
	relic_pool_t	*pool;
	size_t		 index;
};

/*
 * The pool's threads and what they wait on.
 *
 * `job` and `stopping` are written by worker 0 under `mutex` before it
 * advances `generation`, and read by a thread after it has seen the
 * generation move, so every thread reads them as they were published.
 * `published` is the generation worker 0 last published, which only it
 * reads.  `owner` is the process that started the threads.
 */
struct relic_pool_threads {
	pthread_mutex_t		 mutex;
	pthread_cond_t		 posted;
	pthread_cond_t		 done;
	pthread_t		*handles;
	relic_worker_start_t	*starts;
	relic_pool_job_t	*job;
	relic_atomic_u64_t	 generation;
	uint64_t		 published;
	size_t			 started;
	pid_t			 owner;
	bool			 stopping;
};

static uint64_t	 await_job(relic_pool_threads_t *threads, uint64_t seen);
static void	 await_workers(relic_pool_threads_t *threads,
		    relic_pool_job_t *job, size_t workers);
static void	 finish_job(relic_pool_threads_t *threads,
		    relic_pool_job_t *job, size_t workers);
static void	 publish(relic_pool_threads_t *threads, relic_pool_job_t *job,
		    bool stopping);
static void	 spin_pause(void);
static int	 threads_start(relic_pool_t *pool, size_t workers);
static void	 threads_stop(relic_pool_threads_t *threads);
static void	 worker_signal_mask(sigset_t *mask);
static void	*worker_main(void *argument);

#endif /* RELIC_THREADS */

static void	 worker_release(relic_worker_t *worker);

size_t
relic_pool_size(size_t threads RELIC_UNUSED)
{
#ifdef RELIC_THREADS
	return (threads > 1 ? threads : 1);
#else
	return (1);
#endif
}

int
relic_pool_create(relic_pool_t **poolp, size_t workers, uint64_t hash_seed)
{
	relic_pool_t *pool;
	size_t i;

	assert(workers > 0);

	pool = calloc(1, sizeof(*pool));
	if (pool == NULL)
		return (-1);

	pool->workers = calloc(workers, sizeof(*pool->workers));
	if (pool->workers == NULL) {
		free(pool);
		return (-1);
	}
	for (i = 0; i < workers; i++)
		relic_candidates_init(&pool->workers[i].candidates,
		    hash_seed);
	pool->worker_count = 1;

#ifdef RELIC_THREADS
	if (workers > 1 && threads_start(pool, workers) != 0) {
		free(pool->workers);
		free(pool);
		return (-1);
	}
#endif

	*poolp = pool;

	return (0);
}

void
relic_pool_adopt(relic_pool_t *pool RELIC_UNUSED)
{
#ifdef RELIC_THREADS
	size_t i;

	if (pool->threads == NULL || pool->threads->owner == getpid())
		return;

	threads_stop(pool->threads);
	pool->threads = NULL;
	for (i = 1; i < pool->worker_count; i++)
		worker_release(&pool->workers[i]);
	pool->worker_count = 1;
#endif
}

void
relic_pool_for(relic_pool_t *pool RELIC_UNUSED, size_t part_count,
    relic_pool_part_fn run_part, void *argument)
{
	relic_pool_job_t job;
#ifdef RELIC_THREADS
	int cancel_state;
#endif

	job.run_part = run_part;
	job.argument = argument;
	job.part_count = part_count;
	relic_atomic_size_init(&job.next_part, 0);
	relic_atomic_size_init(&job.finished, 0);

#ifdef RELIC_THREADS
	if (pool->worker_count > 1 && part_count > 1) {
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
		publish(pool->threads, &job, false);
		job_work(&job, 0);
		finish_job(pool->threads, &job, pool->worker_count);
		await_workers(pool->threads, &job, pool->worker_count);
		pthread_setcancelstate(cancel_state, NULL);
		return;
	}
#endif

	job_work(&job, 0);
}

/*
 * Claim the job's parts one after another and run each, until none is left.
 * The claim needs no ordering of its own: what a part reads was published
 * with the job, and what it writes is published by the worker's count into
 * `finished`.
 */
static void
job_work(relic_pool_job_t *job, size_t worker_index)
{
	size_t part;

	for (;;) {
		part = relic_atomic_size_add_relaxed(&job->next_part, 1);
		if (part >= job->part_count)
			break;

		job->run_part(job->argument, part, worker_index);
	}
}

/*
 * A round releases its candidates before it returns, so only the storage
 * kept from round to round remains.
 */
void
relic_pool_free(relic_pool_t *pool)
{
	size_t i;

	if (pool == NULL)
		return;

#ifdef RELIC_THREADS
	if (pool->threads != NULL)
		threads_stop(pool->threads);
#endif

	for (i = 0; i < pool->worker_count; i++)
		worker_release(&pool->workers[i]);

	free(pool->workers);
	relic_free_array(pool->tasks);
	free(pool);
}

/*
 * Release the storage a worker keeps from one round to the next, leaving it
 * as a worker that has run no round.
 */
static void
worker_release(relic_worker_t *worker)
{
	assert(worker->candidates.count == 0);
	relic_free_array(worker->states);
	relic_free_array(worker->batch);
	relic_free_array(worker->pending);
	relic_idtab_reset(&worker->candidates.screen);
	relic_idtab_reset(&worker->candidates.value_screen);
	worker->states = NULL;
	worker->batch = NULL;
	worker->pending = NULL;
	worker->state_capacity = 0;
	worker->batch_capacity = 0;
	worker->pending_capacity = 0;
}

#ifdef RELIC_THREADS

/*
 * Start a thread for every worker but the first, each with the mask
 * worker_signal_mask() gives, and record how many started.  Returns 0,
 * even when none could be, and -1 only when the pool's own records could
 * not be allocated.
 */
static int
threads_start(relic_pool_t *pool, size_t workers)
{
	relic_pool_threads_t *threads;
	sigset_t worker_mask;
	sigset_t saved;
	size_t i;

	threads = calloc(1, sizeof(*threads));
	if (threads == NULL)
		return (-1);
	threads->handles = calloc(workers - 1, sizeof(*threads->handles));
	threads->starts = calloc(workers, sizeof(*threads->starts));
	if (threads->handles == NULL || threads->starts == NULL)
		goto fail;

	if (pthread_mutex_init(&threads->mutex, NULL) != 0)
		goto fail;
	if (pthread_cond_init(&threads->posted, NULL) != 0) {
		pthread_mutex_destroy(&threads->mutex);
		goto fail;
	}
	if (pthread_cond_init(&threads->done, NULL) != 0) {
		pthread_cond_destroy(&threads->posted);
		pthread_mutex_destroy(&threads->mutex);
		goto fail;
	}
	relic_atomic_u64_init(&threads->generation, 0);
	threads->owner = getpid();
	pool->threads = threads;

	/* A thread starts with its creator's mask, so the creator wears it. */
	worker_signal_mask(&worker_mask);
	pthread_sigmask(SIG_SETMASK, &worker_mask, &saved);
	for (i = 1; i < workers; i++) {
		threads->starts[i].pool = pool;
		threads->starts[i].index = i;
		if (pthread_create(&threads->handles[i - 1], NULL, worker_main,
		    &threads->starts[i]) != 0)
			break;
		threads->started++;
	}
	pthread_sigmask(SIG_SETMASK, &saved, NULL);

	pool->worker_count = threads->started + 1;

	return (0);

fail:
	free(threads->handles);
	free(threads->starts);
	free(threads);
	return (-1);
}

/*
 * The signals a thread of the pool blocks: every one but those a fault in
 * the thread itself raises.  A signal sent to the process is then taken
 * by a thread that is the caller's, and a fault kills the process as it
 * would on the calling thread.
 */
static void
worker_signal_mask(sigset_t *mask)
{
	sigfillset(mask);
	sigdelset(mask, SIGSEGV);
	sigdelset(mask, SIGBUS);
	sigdelset(mask, SIGFPE);
	sigdelset(mask, SIGILL);
	sigdelset(mask, SIGTRAP);
	sigdelset(mask, SIGSYS);
}

/*
 * Tell every thread to stop, join them and release what they shared.  In a
 * child of fork(2) the threads do not exist and their mutex may be held by
 * one of them, so only the memory is released.  Joining cannot be
 * cancelled, which would leave threads running on a pool being freed.
 */
static void
threads_stop(relic_pool_threads_t *threads)
{
	int cancel_state;
	size_t i;

	if (threads->owner == getpid()) {
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
		publish(threads, NULL, true);
		for (i = 0; i < threads->started; i++)
			pthread_join(threads->handles[i], NULL);
		pthread_setcancelstate(cancel_state, NULL);

		pthread_cond_destroy(&threads->done);
		pthread_cond_destroy(&threads->posted);
		pthread_mutex_destroy(&threads->mutex);
	}

	free(threads->handles);
	free(threads->starts);
	free(threads);
}

/*
 * A thread of the pool: wait for each job in turn, run its parts, and
 * report that it has finished, until the pool stops.
 */
static void *
worker_main(void *argument)
{
	const relic_worker_start_t *start;
	relic_pool_threads_t *threads;
	relic_pool_job_t *job;
	relic_pool_t *pool;
	uint64_t seen;

	start = argument;
	pool = start->pool;
	threads = pool->threads;
	seen = 0;

	for (;;) {
		seen = await_job(threads, seen);
		if (threads->stopping)
			break;

		job = threads->job;
		job_work(job, start->index);
		finish_job(threads, job, pool->worker_count);
	}

	return (NULL);
}

/*
 * Publish a job, or that the pool is stopping, to every thread.
 */
static void
publish(relic_pool_threads_t *threads, relic_pool_job_t *job, bool stopping)
{
	pthread_mutex_lock(&threads->mutex);
	threads->job = job;
	threads->stopping = stopping;
	threads->published++;
	relic_atomic_u64_store_release(&threads->generation,
	    threads->published);
	pthread_cond_broadcast(&threads->posted);
	pthread_mutex_unlock(&threads->mutex);
}

/*
 * Wait until the generation has moved past `seen`, and return it.
 */
static uint64_t
await_job(relic_pool_threads_t *threads, uint64_t seen)
{
	uint64_t generation;
	unsigned int spins;

	for (spins = 0; spins < RELIC_SPIN_LIMIT; spins++) {
		generation =
		    relic_atomic_u64_load_acquire(&threads->generation);
		if (generation != seen)
			return (generation);
		spin_pause();
	}

	pthread_mutex_lock(&threads->mutex);
	for (;;) {
		generation =
		    relic_atomic_u64_load_acquire(&threads->generation);
		if (generation != seen)
			break;
		pthread_cond_wait(&threads->posted, &threads->mutex);
	}
	pthread_mutex_unlock(&threads->mutex);

	return (generation);
}

/*
 * Count a worker that has left the job's claim loop into the job, waking
 * worker 0 when it is the last of `workers`.  The job is not touched after
 * the count, since the last count may end it.
 */
static void
finish_job(relic_pool_threads_t *threads, relic_pool_job_t *job,
    size_t workers)
{
	if (relic_atomic_size_add_acq_rel(&job->finished, 1) + 1 < workers)
		return;

	pthread_mutex_lock(&threads->mutex);
	pthread_cond_signal(&threads->done);
	pthread_mutex_unlock(&threads->mutex);
}

/*
 * Wait until all `workers` have finished the job.
 */
static void
await_workers(relic_pool_threads_t *threads, relic_pool_job_t *job,
    size_t workers)
{
	unsigned int spins;

	for (spins = 0; spins < RELIC_SPIN_LIMIT; spins++) {
		if (relic_atomic_size_load_acquire(&job->finished) == workers)
			return;
		spin_pause();
	}

	pthread_mutex_lock(&threads->mutex);
	while (relic_atomic_size_load_acquire(&job->finished) < workers)
		pthread_cond_wait(&threads->done, &threads->mutex);
	pthread_mutex_unlock(&threads->mutex);
}

/*
 * Tell the processor a thread is spinning, which on the processors that
 * have such a hint frees the core's shared resources for its sibling and
 * saves power; elsewhere the loop simply looks again.
 */
static void
spin_pause(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("yield");
#endif
}

#endif /* RELIC_THREADS */
