/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * A context's worker pool: when its threads start and stop, and what a
 * thread that cannot be started costs.
 *
 * The library sources in this program are compiled with pthread_create()
 * and pthread_join() renamed, and this file supplies the replacements,
 * which count every thread started and joined and can be told to refuse a
 * start.  So the test sees the threads the library starts without asking
 * the operating system how many a process has, which no portable interface
 * says.  Undefining the names here, before anything is included, lets this
 * one translation unit reach the real interfaces to implement them.
 *
 * A build without threads starts none, and the pool is the calling thread
 * alone: the test then checks only that a context configured with threads
 * evaluates as one without, and counts no round as shared.
 */
#undef pthread_create
#undef pthread_join

#include "config.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifdef RELIC_THREADS
#include <pthread.h>
#include <signal.h>
#endif

#include "harness.h"
#include "internal.h"

#define	SCENARIO_LAZY		"a context starts its threads when it shares"
#define	SCENARIO_JOIN		"destroying a context joins its threads"
#define	SCENARIO_REFUSED	"a thread that cannot start is not an error"
#define	SCENARIO_SIGNALS \
	"the pool's threads block every signal but a fault's"
#define	SCENARIO_FORK		"a pool carried into a child of fork"
#define	SCENARIO_NO_THREADS	"a build without threads evaluates alone"
#define	SCENARIO_SPIN \
	"a waiter spins only when every worker has a processor"

/*
 * The threads the test's contexts are configured with, and the length of
 * the chain whose closure they derive, which has CHAIN_LENGTH *
 * (CHAIN_LENGTH + 1) / 2 paths.  A round of the closure is far below the
 * work a round must hold to be shared, so only a context with
 * RELIC_F_PARALLEL_ALWAYS shares one.
 */
#define	POOL_THREADS	4
#define	CHAIN_LENGTH	30
#define	CHAIN_PATHS	(CHAIN_LENGTH * (CHAIN_LENGTH + 1) / 2)

/*
 * A graph of LAYERS layers of LAYER_WIDTH nodes, every node of a layer
 * joined to every node of the next, whose middle rounds of paths hold
 * enough work to be shared without RELIC_F_PARALLEL_ALWAYS.
 */
#define	LAYERS		6
#define	LAYER_WIDTH	32
#define	LAYERED_EDGES	(LAYER_WIDTH * LAYER_WIDTH * (LAYERS - 1))
#define	LAYERED_PATHS	\
	(LAYER_WIDTH * LAYER_WIDTH * LAYERS * (LAYERS - 1) / 2)
#define	LAYERED_MAX	(128 + LAYERED_EDGES * 32)

/* How long a forked child may take before it is taken to have hung. */
#define	FORK_SECONDS	20

/* Room for the chain program. */
#define	PROGRAM_MAX	4096

#ifdef RELIC_THREADS

int		 relic_test_pthread_create(pthread_t *thread,
		    const pthread_attr_t *attributes,
		    void *(*start)(void *), void *argument);
int		 relic_test_pthread_join(pthread_t thread, void **result);

/*
 * The threads the library has started and joined since the counts were
 * last cleared, and how many more it may start before a start is refused;
 * STARTS_UNLIMITED refuses none.  Whether every thread the library started
 * began with the signals a caller handles blocked and those a fault raises
 * not.  Only the thread that calls into the library starts and joins, so
 * none of these is shared.
 */
#define	STARTS_UNLIMITED	SIZE_MAX

static size_t	 threads_started;
static size_t	 threads_joined;
static size_t	 starts_remaining = STARTS_UNLIMITED;
static bool	 started_blocked = true;
static bool	 started_faults_open = true;

static bool	 caller_signals_blocked(void);
static bool	 fault_signals_open(void);
static void	 threads_start_and_stop_with_the_pool(void);
static void	 a_refused_start_is_not_an_error(void);
static void	 a_pool_carried_into_a_child_of_fork(void);

#endif /* RELIC_THREADS */

static relic_t	*chain_context(unsigned int flags, size_t threads);
static relic_t	*context_holding(const char *program, unsigned int flags,
		    size_t threads);
static relic_t	*layered_context(size_t threads);
static bool	 closure_is_complete(relic_t *ctx);
static void	 count_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static uint64_t	 parallel_rounds(relic_t *ctx);
static size_t	 tuples_held(relic_t *ctx);
static void	 waiters_spin_only_on_processors_of_their_own(void);
static void	 without_threads_rounds_run_alone(void);

/* How many diagnostics every context has reported. */
static size_t	 diagnostics_reported;

#ifdef RELIC_THREADS

int
relic_test_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
    void *(*start)(void *), void *argument)
{
	int error;

	if (starts_remaining == 0)
		return (EAGAIN);

	if (!caller_signals_blocked())
		started_blocked = false;
	if (!fault_signals_open())
		started_faults_open = false;

	error = pthread_create(thread, attributes, start, argument);
	if (error != 0)
		return (error);

	threads_started++;
	if (starts_remaining != STARTS_UNLIMITED)
		starts_remaining--;

	return (0);
}

int
relic_test_pthread_join(pthread_t thread, void **result)
{
	threads_joined++;

	return (pthread_join(thread, result));
}

/*
 * Whether the calling thread blocks the signals a caller is most likely to
 * handle, which a thread it starts inherits.
 */
static bool
caller_signals_blocked(void)
{
	static const int signals[] = {
		SIGINT, SIGTERM, SIGHUP, SIGALRM, SIGUSR1, SIGCHLD, SIGPIPE,
	};
	sigset_t mask;
	size_t i;

	if (pthread_sigmask(SIG_BLOCK, NULL, &mask) != 0)
		return (false);

	for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		if (sigismember(&mask, signals[i]) != 1)
			return (false);
	}

	return (true);
}

/*
 * Whether the calling thread leaves open every signal a fault raises, which
 * a thread it starts inherits: a fault in a thread that blocks its signal
 * repeats forever rather than ending the process.
 */
static bool
fault_signals_open(void)
{
	static const int signals[] = {
		SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGSYS,
	};
	sigset_t mask;
	size_t i;

	if (pthread_sigmask(SIG_BLOCK, NULL, &mask) != 0)
		return (false);

	for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		if (sigismember(&mask, signals[i]) != 0)
			return (false);
	}

	return (true);
}

#endif /* RELIC_THREADS */

static void
count_diagnostic(void *callback_ctx RELIC_UNUSED,
    enum relic_diag_level level RELIC_UNUSED,
    const relic_location_t *where RELIC_UNUSED,
    const char *message RELIC_UNUSED)
{
	diagnostics_reported++;
}

/*
 * A context with the given flags and threads holding the chain program, or
 * NULL having recorded a failure.
 */
static relic_t *
chain_context(unsigned int flags, size_t threads)
{
	char program[PROGRAM_MAX];
	size_t used;
	unsigned int i;

	used = (size_t)snprintf(program, sizeof(program),
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- path(X, Y), edge(Y, Z).\n");
	for (i = 0; i < CHAIN_LENGTH && used < sizeof(program); i++)
		used += (size_t)snprintf(program + used, sizeof(program) - used,
		    "edge(n%u, n%u).\n", i, i + 1);
	if (used >= sizeof(program)) {
		check(false, "building the chain", "the program fits");
		return (NULL);
	}

	return (context_holding(program, flags, threads));
}

/*
 * A context of `threads` threads holding the paths of the layered graph,
 * or NULL having recorded a failure.
 */
static relic_t *
layered_context(size_t threads)
{
	relic_t *ctx;
	char *program;
	size_t used;
	unsigned int layer;
	unsigned int from;
	unsigned int to;

	program = malloc(LAYERED_MAX);
	if (program == NULL) {
		check(false, "building the layered graph", "memory suffices");
		return (NULL);
	}

	used = (size_t)snprintf(program, LAYERED_MAX,
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- path(X, Y), edge(Y, Z).\n");
	for (layer = 0; layer + 1 < LAYERS; layer++) {
		for (from = 0; from < LAYER_WIDTH; from++) {
			for (to = 0; to < LAYER_WIDTH && used < LAYERED_MAX;
			    to++)
				used += (size_t)snprintf(program + used,
				    LAYERED_MAX - used,
				    "edge(l%u_%u, l%u_%u).\n", layer, from,
				    layer + 1, to);
		}
	}

	ctx = NULL;
	if (used < LAYERED_MAX)
		ctx = context_holding(program, 0, threads);
	else
		check(false, "building the layered graph", "the program fits");
	free(program);

	return (ctx);
}

/*
 * A context with the given flags and threads holding `program`, loaded and
 * checked, or NULL having recorded a failure.
 */
static relic_t *
context_holding(const char *program, unsigned int flags, size_t threads)
{
	relic_config_t config;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	config.flags = flags;
	config.threads = threads;

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, "creating a context", "a context is created");
		return (NULL);
	}
	relic_diag_set(ctx, count_diagnostic, NULL);

	if (relic_load_string(ctx, "test.dl", program) != 0 ||
	    relic_check(ctx) != 0) {
		check(false, "loading a test program", "the program loads");
		relic_destroy(ctx);
		return (NULL);
	}

	return (ctx);
}

/*
 * Whether a materialized context holds the chain's whole closure and
 * nothing more.
 */
static bool
closure_is_complete(relic_t *ctx)
{
	relic_stats_t stats;

	relic_stats(ctx, &stats);

	return (stats.tuples == CHAIN_LENGTH + CHAIN_PATHS);
}

static size_t
tuples_held(relic_t *ctx)
{
	relic_stats_t stats;

	relic_stats(ctx, &stats);

	return (stats.tuples);
}

static uint64_t
parallel_rounds(relic_t *ctx)
{
	relic_stats_t stats;

	relic_stats(ctx, &stats);

	return (stats.parallel_rounds);
}

#ifdef RELIC_THREADS

/*
 * A context starts no thread until an evaluation shares a round: not when
 * it is created, loaded or checked, and not for an evaluation whose rounds
 * are all too small to share.  The first round it shares starts one thread
 * for each worker but the calling thread, each with every signal blocked
 * but those a fault raises, and leaves the caller's own mask as it was; a
 * later evaluation reuses them.  Destroying the context joins every one.
 */
static void
threads_start_and_stop_with_the_pool(void)
{
	relic_t *ctx;

	threads_started = 0;
	threads_joined = 0;

	ctx = chain_context(0, POOL_THREADS);
	if (ctx != NULL) {
		check(threads_started == 0, SCENARIO_LAZY,
		    "creating, loading and checking start no thread");
		check(relic_materialize(ctx) == 0 && closure_is_complete(ctx),
		    SCENARIO_LAZY, "the chain materializes");
		check(threads_started == 0 && parallel_rounds(ctx) == 0,
		    SCENARIO_LAZY,
		    "rounds too small to share start no thread");
	}
	relic_destroy(ctx);
	check(threads_joined == 0, SCENARIO_JOIN,
	    "a context that started no thread joins none");

	started_blocked = true;
	started_faults_open = true;
	ctx = chain_context(RELIC_F_PARALLEL_ALWAYS, POOL_THREADS);
	if (ctx != NULL) {
		check(threads_started == 0, SCENARIO_LAZY,
		    "creating, loading and checking start no thread");
		check(relic_materialize(ctx) == 0 && closure_is_complete(ctx),
		    SCENARIO_LAZY, "the chain materializes shared");
		checkf(threads_started == POOL_THREADS - 1, SCENARIO_LAZY,
		    "the first shared round starts %d threads, not %zu",
		    POOL_THREADS - 1, threads_started);
		check(parallel_rounds(ctx) > 0, SCENARIO_LAZY,
		    "the rounds are counted as shared");
		check(started_blocked, SCENARIO_SIGNALS,
		    "every thread starts with the caller's signals blocked");
		check(started_faults_open, SCENARIO_SIGNALS,
		    "every thread starts with a fault's signals open");
		check(!caller_signals_blocked(), SCENARIO_SIGNALS,
		    "the caller's mask is restored");

		check(relic_materialize(ctx) == 0 &&
		    threads_started == POOL_THREADS - 1, SCENARIO_LAZY,
		    "a second evaluation reuses the pool's threads");
	}
	relic_destroy(ctx);
	checkf(threads_joined == POOL_THREADS - 1, SCENARIO_JOIN,
	    "destroying the context joins %d threads, not %zu",
	    POOL_THREADS - 1, threads_joined);
}

/*
 * A pool whose threads could not all be started evaluates with those that
 * could, and reports nothing: with none started it is the calling thread
 * alone, and shares no round, though under RELIC_F_PARALLEL_ALWAYS it
 * still defers them.  Without the switch it declines the rounds of the
 * layered graph that a pool with threads shares, and they run serially.
 * Destroying the context joins exactly the threads that started.
 */
static void
a_refused_start_is_not_an_error(void)
{
	static const size_t layered_starts[] = { 0, POOL_THREADS - 1 };
	relic_t *ctx;
	size_t allowed;
	size_t i;

	for (allowed = 0; allowed < POOL_THREADS - 1; allowed++) {
		threads_started = 0;
		threads_joined = 0;
		diagnostics_reported = 0;

		ctx = chain_context(RELIC_F_PARALLEL_ALWAYS, POOL_THREADS);
		if (ctx == NULL)
			continue;

		starts_remaining = allowed;
		checkf(relic_materialize(ctx) == 0 && closure_is_complete(ctx),
		    SCENARIO_REFUSED, "with %zu thread(s) started, the chain "
		    "materializes", allowed);
		starts_remaining = STARTS_UNLIMITED;

		checkf(threads_started == allowed, SCENARIO_REFUSED,
		    "%zu thread(s) start, not %zu", allowed, threads_started);
		checkf((parallel_rounds(ctx) > 0) == (allowed > 0),
		    SCENARIO_REFUSED, "with %zu thread(s) started, rounds are "
		    "counted as shared only with more than one worker",
		    allowed);
		checkf(ctx->pool != NULL && ctx->pool->merged_rounds > 0,
		    SCENARIO_REFUSED, "with %zu thread(s) started, rounds are "
		    "still deferred", allowed);
		checkf(diagnostics_reported == 0, SCENARIO_REFUSED,
		    "with %zu thread(s) started, nothing is reported", allowed);

		relic_destroy(ctx);
		checkf(threads_joined == allowed, SCENARIO_JOIN,
		    "destroying the context joins the %zu thread(s) that "
		    "started, not %zu", allowed, threads_joined);
	}

	for (i = 0; i < sizeof(layered_starts) / sizeof(layered_starts[0]);
	    i++) {
		allowed = layered_starts[i];
		threads_started = 0;
		diagnostics_reported = 0;

		ctx = layered_context(POOL_THREADS);
		if (ctx == NULL)
			continue;

		starts_remaining = allowed;
		checkf(relic_materialize(ctx) == 0 &&
		    tuples_held(ctx) == LAYERED_EDGES + LAYERED_PATHS,
		    SCENARIO_REFUSED, "with %zu thread(s) started, the layered "
		    "graph materializes", allowed);
		starts_remaining = STARTS_UNLIMITED;

		checkf(threads_started == allowed, SCENARIO_REFUSED,
		    "%zu thread(s) start, not %zu", allowed, threads_started);
		checkf((parallel_rounds(ctx) > 0) == (allowed > 0) &&
		    (ctx->pool != NULL && ctx->pool->merged_rounds > 0) ==
		    (allowed > 0), SCENARIO_REFUSED, "with %zu thread(s) "
		    "started, the layered graph's rounds are shared only with "
		    "threads", allowed);
		checkf(diagnostics_reported == 0, SCENARIO_REFUSED,
		    "with %zu thread(s) started, nothing is reported", allowed);

		relic_destroy(ctx);
	}
}

/*
 * A child of fork(2) has none of its parent's threads.  A pool it destroys
 * releases their memory without joining any, and a pool it evaluates on
 * becomes the calling thread alone rather than waiting for threads that do
 * not exist, which a child that hangs past FORK_SECONDS would be.  The
 * parent's pool is untouched and joins its own.
 */
static void
a_pool_carried_into_a_child_of_fork(void)
{
	relic_t *ctx;
	pid_t child;
	int status;

	threads_started = 0;
	threads_joined = 0;

	ctx = chain_context(RELIC_F_PARALLEL_ALWAYS, POOL_THREADS);
	if (ctx == NULL)
		return;
	if (relic_materialize(ctx) != 0 ||
	    threads_started != POOL_THREADS - 1) {
		check(false, SCENARIO_FORK, "the pool's threads start");
		relic_destroy(ctx);
		return;
	}

	child = fork();
	if (child == 0) {
		relic_destroy(ctx);
		_exit(threads_joined == 0 ? 0 : 1);
	}
	check(child > 0, SCENARIO_FORK, "a child is forked");

	if (child > 0) {
		status = -1;
		while (waitpid(child, &status, 0) < 0 && errno == EINTR)
			continue;
		check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		    SCENARIO_FORK, "the child's destroy joins no thread");
	}

	child = fork();
	if (child == 0) {
		alarm(FORK_SECONDS);
		_exit(relic_load_string(ctx, "more.dl", "edge(z0, z1).\n") ==
		    0 && relic_materialize(ctx) == 0 &&
		    tuples_held(ctx) == CHAIN_LENGTH + CHAIN_PATHS + 2 ? 0 : 1);
	}
	check(child > 0, SCENARIO_FORK, "a second child is forked");

	if (child > 0) {
		status = -1;
		while (waitpid(child, &status, 0) < 0 && errno == EINTR)
			continue;
		check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		    SCENARIO_FORK, "the child evaluates without the parent's "
		    "threads");
	}

	check(relic_materialize(ctx) == 0 && closure_is_complete(ctx),
	    SCENARIO_FORK, "the parent's pool still evaluates");
	relic_destroy(ctx);
	checkf(threads_joined == POOL_THREADS - 1, SCENARIO_FORK,
	    "the parent joins its %d threads, not %zu", POOL_THREADS - 1,
	    threads_joined);
}

#endif /* RELIC_THREADS */

/*
 * Without threads a context configured with several evaluates on the
 * calling thread alone: its rounds are not offered to a pool, however
 * large, and under RELIC_F_PARALLEL_ALWAYS they are deferred and merged
 * and none is counted as shared.  With threads the same contexts share
 * their rounds, which the scenarios above check.
 */
static void
without_threads_rounds_run_alone(void)
{
#ifndef RELIC_THREADS
	relic_t *ctx;

	ctx = layered_context(POOL_THREADS);
	if (ctx != NULL)
		check(relic_materialize(ctx) == 0 &&
		    tuples_held(ctx) == LAYERED_EDGES + LAYERED_PATHS &&
		    parallel_rounds(ctx) == 0 && ctx->pool == NULL,
		    SCENARIO_NO_THREADS, "the layered graph materializes "
		    "without a pool");
	relic_destroy(ctx);

	ctx = chain_context(RELIC_F_PARALLEL_ALWAYS, POOL_THREADS);
	if (ctx != NULL)
		check(relic_materialize(ctx) == 0 && closure_is_complete(ctx) &&
		    ctx->pool != NULL && ctx->pool->merged_rounds > 0 &&
		    parallel_rounds(ctx) == 0, SCENARIO_NO_THREADS,
		    "deferred rounds are merged and none is counted as shared");
	relic_destroy(ctx);
#endif
}

/*
 * A waiter spinning on the processor the worker it waits for needs delays
 * that worker, so a pool whose workers outnumber the processors, or that
 * cannot tell how many there are, parks at once.  Otherwise it spins.
 */
static void
waiters_spin_only_on_processors_of_their_own(void)
{
	check(relic_pool_spin_nanoseconds(2, 2) > 0, SCENARIO_SPIN,
	    "two workers on two processors spin");
	check(relic_pool_spin_nanoseconds(4, 16) > 0, SCENARIO_SPIN,
	    "four workers on sixteen processors spin");
	check(relic_pool_spin_nanoseconds(4, 4) ==
	    relic_pool_spin_nanoseconds(4, 16), SCENARIO_SPIN,
	    "a worker per processor spins as long as spare processors do");
	check(relic_pool_spin_nanoseconds(3, 2) == 0, SCENARIO_SPIN,
	    "three workers on two processors park at once");
	check(relic_pool_spin_nanoseconds(256, 16) == 0, SCENARIO_SPIN,
	    "256 workers on sixteen processors park at once");
	check(relic_pool_spin_nanoseconds(2, 0) == 0, SCENARIO_SPIN,
	    "workers on no processor reported park at once");
	check(relic_pool_spin_nanoseconds(2, -1) == 0, SCENARIO_SPIN,
	    "workers on an unknown count of processors park at once");
}

int
main(void)
{
#ifdef RELIC_THREADS
	threads_start_and_stop_with_the_pool();
	a_refused_start_is_not_an_error();
	a_pool_carried_into_a_child_of_fork();
#endif
	waiters_spin_only_on_processors_of_their_own();
	without_threads_rounds_run_alone();

	return (check_summary("test_pool"));
}
