/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat.h"
#include "random.h"

/*
 * The driver of the randomized differential suite.
 *
 * For each seed in a range it generates a program, runs it under every
 * strategy in a child process and, when the child reports a failure, shrinks
 * the program and prints the seed, the failure and the shrunk program.
 *
 * The child is what lets the suite report rather than stop.  The engine
 * under test may crash, loop, leak or trip a sanitizer; any of those in this
 * process would end the run with the seed unreported and nothing to shrink.
 * The child tells the parent over a pipe which strategy it is running, and
 * its verdict once it has one.  A child that dies is charged to the strategy
 * it last announced, a child that ends without a verdict or with a nonzero
 * status exited abnormally, and an alarm turns a child that never finishes
 * into a failure of its own kind.
 *
 * With -F the run is the suite's self-test: a fault is injected into the
 * answers of one comparison after another, and the run succeeds only if
 * each fault is found, reported as the disagreement it is, and shrunk to a
 * handful of statements.
 */

/* The largest program the self-test accepts as shrunk. */
#define	SELFTEST_MAX_STATEMENTS	4

/*
 * A record the child writes to the pipe: a strategy's run beginning, or the
 * verdict.  A record is three bytes, far below PIPE_BUF, so each write
 * reaches the parent whole.
 */
typedef enum progress_event {
	PROGRESS_RUNNING,	/* strategy has begun */
	PROGRESS_VERDICT	/* kind and strategy are the child's verdict */
} progress_event_t;

typedef struct progress_record progress_record_t;

struct progress_record {
	uint8_t		event;
	uint8_t		kind;
	uint8_t		strategy;
};

/* What the parent learned from the pipe once the child has ended. */
typedef struct progress progress_t;

struct progress {
	random_strategy_t	running;	/* last announced */
	bool			has_verdict;
	random_failure_t	verdict;
};

typedef struct driver driver_t;

struct driver {
	random_options_t	 options;
	const char		*output_directory;	/* NULL: none */
	uint64_t		 first_seed;
	uint64_t		 seed_count;
	bool			 print_only;
	bool			 selftest;
	bool			 verbose;
};

/*
 * A fault the self-test injects, and the failure it must be reported as:
 * the comparison's own kind, charged to the strategy whose answers lost a
 * row, or to none for the helper rendering, which is no strategy.
 */
typedef struct selftest_case selftest_case_t;

struct selftest_case {
	const char		*name;
	random_fault_t		 fault;
	random_failure_kind_t	 kind;
	random_strategy_t	 strategy;
};

/* One fault for each comparison of answers that can disagree. */
static const selftest_case_t selftest_cases[] = {
	{ "a row dropped from the naive strategy's answers", FAULT_NAIVE,
	    FAILURE_ANSWERS, STRATEGY_NAIVE },
	{ "a row dropped from the helper rendering's answers", FAULT_HELPER,
	    FAILURE_HELPER, STRATEGY_NONE },
};

static void	 announce_strategy(void *announce_ctx,
		    random_strategy_t strategy);
static random_failure_t failure_of_child(int status,
		    const progress_t *progress);
static bool	 failure_names_strategy(random_failure_t failure);
static void	 parse_options(int argc, char **argv, driver_t *driver);
static progress_t read_progress(int fd);
static void	 redirect_stderr_to_null(void);
static random_program_t *report_failure(const driver_t *driver,
		    const random_options_t *options, uint64_t seed,
		    const random_program_t *program, random_failure_t failure);
static void	 run_child(const random_program_t *program,
		    const random_options_t *options, bool show_report,
		    int progress_fd);
static void	 generate_checked(random_program_t *program, uint64_t seed);
static int	 print_programs(const driver_t *driver);
static int	 run_seeds(const driver_t *driver);
static int	 run_selftest(const driver_t *driver);
static bool	 run_selftest_case(const driver_t *driver,
		    const selftest_case_t *test);
static bool	 selftest_passed(const selftest_case_t *test,
		    random_failure_t failure, const random_program_t *shrunk);
static void	 usage(void);
static void	 write_progress(int fd, progress_record_t record);
static void	 write_program_file(const char *directory, uint64_t seed,
		    const char *suffix, const random_program_t *program);

int
main(int argc, char **argv)
{
	driver_t driver;

	parse_options(argc, argv, &driver);
	if (driver.print_only)
		return (print_programs(&driver));
	if (driver.selftest)
		return (run_selftest(&driver));
	return (run_seeds(&driver));
}

static void
parse_options(int argc, char **argv, driver_t *driver)
{
	const char *error;
	int ch;

	driver->options.fault = FAULT_NONE;
	driver->options.timeout_seconds = 60;
	driver->output_directory = NULL;
	driver->first_seed = 1;
	driver->seed_count = 50;
	driver->print_only = false;
	driver->selftest = false;
	driver->verbose = false;

	while ((ch = getopt(argc, argv, "Fn:o:ps:t:v")) != -1) {
		switch (ch) {
		case 'F':
			driver->selftest = true;
			break;
		case 'n':
			driver->seed_count = (uint64_t)strtonum(optarg, 1,
			    LLONG_MAX, &error);
			if (error != NULL)
				errx(1, "seed count %s: %s", optarg, error);
			break;
		case 'o':
			driver->output_directory = optarg;
			break;
		case 'p':
			driver->print_only = true;
			break;
		case 's':
			driver->first_seed = (uint64_t)strtonum(optarg, 0,
			    LLONG_MAX, &error);
			if (error != NULL)
				errx(1, "seed %s: %s", optarg, error);
			break;
		case 't':
			driver->options.timeout_seconds =
			    (unsigned int)strtonum(optarg, 1, 86400, &error);
			if (error != NULL)
				errx(1, "timeout %s: %s", optarg, error);
			break;
		case 'v':
			driver->verbose = true;
			break;
		default:
			usage();
		}
	}
	if (optind != argc)
		usage();
}

static void
usage(void)
{
	fprintf(stderr, "usage: random [-Fpv] [-n count] [-o directory] "
	    "[-s seed] [-t seconds]\n");
	exit(2);
}

/* Print the program each seed of the range generates. */
static int
print_programs(const driver_t *driver)
{
	random_program_t *program;

	program = malloc(sizeof(*program));
	if (program == NULL)
		err(1, "malloc");

	for (uint64_t i = 0; i < driver->seed_count; i++) {
		uint64_t seed = driver->first_seed + i;

		generate_checked(program, seed);
		printf("%% seed %" PRIu64 "\n", seed);
		random_program_write(program, stdout);
	}
	free(program);
	return (0);
}

/*
 * Run every seed of the range and report each failure.  Returns the exit
 * status: zero when every seed passed.
 */
static int
run_seeds(const driver_t *driver)
{
	random_program_t *program;
	uint64_t failures = 0;

	program = malloc(sizeof(*program));
	if (program == NULL)
		err(1, "malloc");

	for (uint64_t i = 0; i < driver->seed_count; i++) {
		uint64_t seed = driver->first_seed + i;
		random_failure_t failure;

		generate_checked(program, seed);
		if (driver->verbose)
			printf("seed %" PRIu64 "\n", seed);
		failure = random_run_isolated(program, &driver->options, false);
		if (failure.kind == FAILURE_NONE)
			continue;

		failures++;
		free(report_failure(driver, &driver->options, seed, program,
		    failure));
	}
	free(program);

	printf("%" PRIu64 " seeds from %" PRIu64 ": %" PRIu64 " failed\n",
	    driver->seed_count, driver->first_seed, failures);
	return (failures == 0 ? 0 : 1);
}

/*
 * The self-test: run every case of selftest_cases[], and succeed only if
 * each passes.  Returns the exit status.
 */
static int
run_selftest(const driver_t *driver)
{
	int status = 0;

	for (size_t c = 0; c < sizeof(selftest_cases) /
	    sizeof(selftest_cases[0]); c++) {
		if (!run_selftest_case(driver, &selftest_cases[c]))
			status = 1;
	}
	return (status);
}

/*
 * Run seeds with the case's fault injected until one fails, and say
 * whether that failure is the fault, reported and shrunk as it should be.
 */
static bool
run_selftest_case(const driver_t *driver, const selftest_case_t *test)
{
	random_options_t options = driver->options;
	random_program_t *program, *shrunk;
	bool passed;

	options.fault = test->fault;
	program = malloc(sizeof(*program));
	if (program == NULL)
		err(1, "malloc");

	for (uint64_t i = 0; i < driver->seed_count; i++) {
		uint64_t seed = driver->first_seed + i;
		random_failure_t failure;

		generate_checked(program, seed);
		failure = random_run_isolated(program, &options, false);
		if (failure.kind == FAILURE_NONE)
			continue;

		shrunk = report_failure(driver, &options, seed, program,
		    failure);
		passed = selftest_passed(test, failure, shrunk);
		printf("self-test: %s: %s\n", test->name, passed ?
		    "detected and shrunk" : "not reported as expected");
		free(shrunk);
		free(program);
		return (passed);
	}
	free(program);

	printf("self-test: %s: not detected in %" PRIu64 " seeds\n",
	    test->name, driver->seed_count);
	return (false);
}

/*
 * Generate the program of a seed.  The generator is safe and bounded by
 * construction; a program the model's own checks call unsafe or unbounded
 * is the generator's bug.
 */
static void
generate_checked(random_program_t *program, uint64_t seed)
{
	random_generate(program, seed);
	if (!random_program_is_safe(program))
		errx(1, "seed %" PRIu64 ": the generator produced an unsafe "
		    "program", seed);
	if (!random_program_is_bounded(program))
		errx(1, "seed %" PRIu64 ": the generator produced a program "
		    "whose arithmetic is not bounded", seed);
}

/*
 * Shrink a failing program and print what failed, the shrunk program, and
 * the comparison's own account of it.  Returns the shrunk program, which
 * the caller frees.
 */
static random_program_t *
report_failure(const driver_t *driver, const random_options_t *options,
    uint64_t seed, const random_program_t *program, random_failure_t failure)
{
	random_program_t *shrunk;

	printf("seed %" PRIu64 ": %s", seed, random_failure_name(failure.kind));
	if (failure.kind == FAILURE_CRASH || failure.kind == FAILURE_ABNORMAL)
		printf(" (%s %d)", failure.kind == FAILURE_CRASH ?
		    "signal" : "status", failure.code);
	if (failure_names_strategy(failure))
		printf(" under %s", random_strategy_name(failure.strategy));
	printf("\n");

	shrunk = malloc(sizeof(*shrunk));
	if (shrunk == NULL)
		err(1, "malloc");
	*shrunk = *program;
	random_shrink(shrunk, failure, options);

	printf("%% shrunk from %zu statements to %zu:\n",
	    random_program_statements(program),
	    random_program_statements(shrunk));
	random_program_write(shrunk, stdout);
	fflush(stdout);
	random_run_isolated(shrunk, options, true);

	if (driver->output_directory != NULL) {
		write_program_file(driver->output_directory, seed, "", program);
		write_program_file(driver->output_directory, seed, "-shrunk",
		    shrunk);
	}
	return (shrunk);
}

/*
 * Whether a self-test case's failure is its injected fault, reported as
 * the case says, and shrunk far enough to show the shrinker works.
 */
static bool
selftest_passed(const selftest_case_t *test, random_failure_t failure,
    const random_program_t *shrunk)
{
	return (failure.kind == test->kind &&
	    failure.strategy == test->strategy &&
	    random_program_statements(shrunk) <= SELFTEST_MAX_STATEMENTS);
}

static void
write_program_file(const char *directory, uint64_t seed, const char *suffix,
    const random_program_t *program)
{
	char path[PATH_MAX];
	FILE *out;
	int length;

	length = snprintf(path, sizeof(path), "%s/seed-%" PRIu64 "%s.dl",
	    directory, seed, suffix);
	if (length < 0 || (size_t)length >= sizeof(path))
		errx(1, "%s: path too long", directory);

	out = fopen(path, "w");
	if (out == NULL)
		err(1, "%s", path);
	fprintf(out, "%% relic randomized suite, seed %" PRIu64 "\n", seed);
	random_program_write(program, out);
	if (fclose(out) != 0)
		err(1, "%s", path);
}

/*
 * Run the comparison in a child and return its verdict.  With show_report
 * the child's account of what disagreed reaches this process's standard
 * error; without it, which is how the shrinker runs hundreds of candidates,
 * the account is discarded.
 */
random_failure_t
random_run_isolated(const random_program_t *program,
    const random_options_t *options, bool show_report)
{
	progress_t progress;
	int pipe_fds[2];
	pid_t child;
	int status;

	/*
	 * Close-on-exec keeps the write end from a process a sanitizer
	 * starts, such as its symbolizer, which could otherwise hold the pipe
	 * open after the child has gone.
	 */
	if (pipe(pipe_fds) == -1)
		err(1, "pipe");
	if (fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC) == -1 ||
	    fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) == -1)
		err(1, "fcntl");

	fflush(stdout);
	fflush(stderr);
	child = fork();
	if (child == -1)
		err(1, "fork");
	if (child == 0) {
		close(pipe_fds[0]);
		run_child(program, options, show_report, pipe_fds[1]);
	}

	close(pipe_fds[1]);
	progress = read_progress(pipe_fds[0]);
	close(pipe_fds[0]);
	if (waitpid(child, &status, 0) == -1)
		err(1, "waitpid");
	return (failure_of_child(status, &progress));
}

/*
 * The child's side: run the comparison under the alarm, announcing each
 * strategy on progress_fd, then send the verdict and exit.
 */
static void
run_child(const random_program_t *program, const random_options_t *options,
    bool show_report, int progress_fd)
{
	progress_record_t verdict;
	random_failure_t failure;

	if (!show_report)
		redirect_stderr_to_null();

	/* An ignored SIGALRM is inherited; the alarm must kill. */
	signal(SIGALRM, SIG_DFL);
	alarm(options->timeout_seconds);
	failure = random_compare(program, options, stderr, announce_strategy,
	    &progress_fd);

	verdict.event = PROGRESS_VERDICT;
	verdict.kind = (uint8_t)failure.kind;
	verdict.strategy = (uint8_t)failure.strategy;
	write_progress(progress_fd, verdict);

	/*
	 * exit(3) rather than _exit(2): a sanitizer's leak check runs at
	 * exit, and a leak must change the status the parent sees.
	 */
	exit(0);
}

static void
redirect_stderr_to_null(void)
{
	int null;

	null = open("/dev/null", O_WRONLY);
	if (null == -1)
		_exit(1);
	if (dup2(null, STDERR_FILENO) == -1)
		_exit(1);
	close(null);
}

static void
announce_strategy(void *announce_ctx, random_strategy_t strategy)
{
	const int *progress_fd = announce_ctx;
	progress_record_t record;

	record.event = PROGRESS_RUNNING;
	record.kind = FAILURE_NONE;
	record.strategy = (uint8_t)strategy;
	write_progress(*progress_fd, record);
}

/* A failed write leaves the parent without a verdict: abnormal. */
static void
write_progress(int fd, progress_record_t record)
{
	if (write(fd, &record, sizeof(record)) != (ssize_t)sizeof(record))
		_exit(1);
}

/*
 * Read the child's records until it closes the pipe.  The last strategy
 * announced is the one that was running when the child ended; a record
 * naming a strategy or a kind out of range is the harness's own bug.
 */
static progress_t
read_progress(int fd)
{
	progress_t progress = { STRATEGY_NONE, false,
	    { FAILURE_NONE, STRATEGY_NONE, 0 } };
	progress_record_t record;
	ssize_t length;

	while ((length = read(fd, &record, sizeof(record))) != 0) {
		if (length == -1)
			err(1, "reading the child's progress");
		if (length != (ssize_t)sizeof(record) ||
		    record.strategy > STRATEGY_NONE ||
		    record.kind > FAILURE_LAST_VERDICT)
			errx(1, "the child sent a malformed progress record");

		if (record.event == PROGRESS_RUNNING) {
			progress.running = (random_strategy_t)record.strategy;
			continue;
		}
		progress.has_verdict = true;
		progress.verdict.kind = (random_failure_kind_t)record.kind;
		progress.verdict.strategy =
		    (random_strategy_t)record.strategy;
	}
	return (progress);
}

/*
 * The failure a child ended in.  A child killed by the alarm hung and one
 * killed by any other signal crashed, both under the strategy it was
 * running.  A child that exited cleanly after its verdict reports that
 * verdict.  Anything else -- a nonzero status from a sanitizer's report or a
 * leak check, err(3) in the harness, an exit with no verdict -- is
 * abnormal.
 */
static random_failure_t
failure_of_child(int status, const progress_t *progress)
{
	random_failure_t failure = { FAILURE_ABNORMAL, progress->running, 0 };

	if (WIFSIGNALED(status)) {
		failure.code = WTERMSIG(status);
		failure.kind = failure.code == SIGALRM ?
		    FAILURE_HANG : FAILURE_CRASH;
		return (failure);
	}
	if (!WIFEXITED(status))
		return (failure);

	failure.code = WEXITSTATUS(status);
	if (failure.code == 0 && progress->has_verdict)
		return (progress->verdict);
	return (failure);
}

/* Whether the failure can be charged to a strategy, for the report. */
static bool
failure_names_strategy(random_failure_t failure)
{
	return (failure.kind != FAILURE_NONE &&
	    failure.strategy != STRATEGY_NONE);
}

const char *
random_failure_name(random_failure_kind_t kind)
{
	switch (kind) {
	case FAILURE_NONE:
		return ("passed");
	case FAILURE_REJECTED:
		return ("program refused");
	case FAILURE_QUERY:
		return ("query failed");
	case FAILURE_ANSWERS:
		return ("answers differ");
	case FAILURE_TOTALS:
		return ("tuple totals differ");
	case FAILURE_DERIVATIONS:
		return ("derivations miscounted");
	case FAILURE_HELPER:
		return ("helper rendering differs");
	case FAILURE_EVALUATION:
		return ("evaluation error");
	case FAILURE_CRASH:
		return ("crashed");
	case FAILURE_HANG:
		return ("timed out");
	case FAILURE_ABNORMAL:
		return ("exited abnormally");
	}
	return ("?");
}

/*
 * Whether two failures are the same for the shrinker's purposes: the same
 * kind, the same strategy, and for a crash or an abnormal exit the same
 * signal or status.
 */
bool
random_failure_equal(random_failure_t a, random_failure_t b)
{
	return (a.kind == b.kind && a.strategy == b.strategy &&
	    a.code == b.code);
}
