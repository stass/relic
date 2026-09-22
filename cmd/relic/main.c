/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <relic.h>

#include "cmd.h"

/*
 * The relic tool: load a program, check it, and answer its queries.
 *
 * An invocation runs in phases, and each runs only if the one before it
 * succeeded: parse the command line, load every operand, check the program,
 * prepare every query that will run, and only then evaluate.  Every mistake
 * in the input is therefore reported before anything is evaluated, and a run
 * that evaluates does so over a program and queries that are all known to be
 * valid.  With -c the run stops after preparing; with -a the whole program is
 * materialized before the first query.
 *
 * The queries that run are the -q ones when there are any, and otherwise the
 * ones the program contains.  The program's own queries are still checked
 * when -q replaces them, because they are part of the program.
 */

/*
 * The tool reports exactly three outcomes: the requested work succeeded, the
 * program or query was rejected, or the command line was malformed.  Every
 * option maps onto this set rather than introducing a status of its own, so
 * that a caller in a shell script can branch on it.
 */
enum {
	RELIC_EXIT_OK = 0,
	RELIC_EXIT_ERROR = 1,
	RELIC_EXIT_USAGE = 2
};

/* A diagnostic of the tool's own is formatted into this many bytes. */
#define	REPORT_TEXT_MAX	512

/* Room for "<query N>" with N any size_t, and a terminator. */
#define	QUERY_NAME_MAX	32

typedef struct options options_t;
typedef struct session session_t;

/*
 * What the command line asked for.  `queries` holds the -q arguments in the
 * order given; they point into argv and are not copied.
 */
struct options {
	const char		**queries;
	const char		 *output_path;
	size_t			  query_count;
	enum output_format	  format;
	bool			  check_only;
	bool			  materialize;
	bool			  print_version;
	bool			  sorted;
	bool			  version_alone;
};

/*
 * A context and the queries prepared against it, released together.
 * `queries` has one entry per query that will run, NULL where one could not
 * be prepared.  `program_name` is what the tool's own messages begin with.
 */
struct session {
	relic_t		 *ctx;
	relic_query_t	**queries;
	const char	 *program_name;
	size_t		  query_count;
};

static void		*diag_context(const char *program_name);
static int		 evaluate(session_t *session, const options_t *options);
static const char	*invoked_name(const char *argv0);
static bool		 is_printable_ascii(const char *text);
static int		 load_operands(relic_t *ctx, int argc, char *argv[]);
static int		 open_output(const char *program_name,
			    const options_t *options, FILE **streamp,
			    const char **namep);
static int		 parse_options(const char *program_name, int argc,
			    char *argv[], options_t *options);
static int		 prepare_queries(session_t *session,
			    const options_t *options);
static void		 report_unknown_option(const char *program_name,
			    int option);
static int		 run(const char *program_name,
			    const options_t *options, int argc, char *argv[]);
static void		 session_close(session_t *session);
static int		 session_open(session_t *session,
			    const char *program_name);
static void		 usage(const char *program_name);
static int		 validate(session_t *session, const options_t *options,
			    int argc, char *argv[]);

/*
 * Whether every byte of text is printable ASCII, space included.
 */
static bool
is_printable_ascii(const char *text)
{
	size_t i;

	for (i = 0; text[i] != '\0'; i++) {
		if (text[i] < ' ' || text[i] > '~')
			return (false);
	}

	return (true);
}

/*
 * The trailing component of the path the program was invoked by, used in
 * diagnostics so that an installation renamed with --program-prefix reports
 * the name the user typed rather than one compiled into it.
 *
 * Anything unusable falls back to the package name: an absent or empty
 * argv[0], a path ending in a slash, and a name holding a byte that is not
 * printable ASCII.  That last case is the reason this is a function rather
 * than a use of strrchr at the call site.  argv[0] is chosen by whoever
 * executes the program, and its final component is no more than a filename,
 * so it can carry newlines or terminal escape sequences; printing one
 * unexamined would let an invocation retitle the reader's terminal or forge a
 * diagnostic line of its own.  Such a name is replaced rather than escaped,
 * because unlike an unknown option byte it tells the reader nothing worth
 * showing.  Every name this returns is therefore safe to print, and no caller
 * needs to remember that.
 *
 * The result is borrowed: it points either into argv[0] or to a string
 * literal, both of which outlive main, so no caller copies or frees it.
 */
static const char *
invoked_name(const char *argv0)
{
	const char *candidate;
	const char *last_slash;

	if (argv0 == NULL || argv0[0] == '\0')
		return (PACKAGE_NAME);

	last_slash = strrchr(argv0, '/');
	if (last_slash == NULL)
		candidate = argv0;
	else
		candidate = last_slash + 1;

	if (candidate[0] == '\0' || !is_printable_ascii(candidate))
		return (PACKAGE_NAME);

	return (candidate);
}

/*
 * Report an option the tool does not accept.
 *
 * optopt holds a byte taken straight from the command line and may be any
 * value at all, so a byte outside printable ASCII is reported by its value
 * rather than echoed: a diagnostic must not carry a terminal escape sequence
 * out of the argument vector and into the reader's terminal.
 */
static void
report_unknown_option(const char *program_name, int option)
{
	unsigned int byte;

	byte = (unsigned int)option & 0xffu;

	if (byte >= '!' && byte <= '~')
		fprintf(stderr, "%s: unknown option -- %c\n", program_name,
		    (int)byte);
	else
		fprintf(stderr, "%s: unknown option -- \\x%02x\n",
		    program_name, byte);
}


static void
usage(const char *program_name)
{
	fprintf(stderr, "usage: %s [-acs] [-F format] [-o file] [-q query] "
	    "[file ...]\n", program_name);
	fprintf(stderr, "       %s -V\n", program_name);
}

/*
 * The program name as the diagnostic callback's context.
 *
 * relic_diag_stderr() takes a void * and reads it as a borrowed, read-only
 * name, so the qualifier has to come off somewhere; the round trip through
 * uintptr_t is what does it without a cast the compiler objects to.  The
 * name itself points either into argv or at a string literal, so it outlives
 * every diagnostic the context can produce.
 */
static void *
diag_context(const char *program_name)
{
	return ((void *)(uintptr_t)program_name);
}

void
report_error(const char *program_name, const char *format, ...)
{
	va_list arguments;
	char message[REPORT_TEXT_MAX];

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	relic_diag_stderr(diag_context(program_name), RELIC_ERROR, NULL,
	    message);
}

/*
 * Load every operand, or standard input when there are none.
 *
 * Every file is attempted even after one of them fails, so that a run
 * reports the mistakes in all of the input rather than in whichever file
 * happened to be named first.
 */
static int
load_operands(relic_t *ctx, int argc, char *argv[])
{
	int status;
	int i;

	if (argc == 0)
		return (relic_load_file(ctx, stdin, "<stdin>"));

	status = 0;
	for (i = 0; i < argc; i++) {
		if (relic_load_path(ctx, argv[i]) != 0)
			status = -1;
	}

	return (status);
}

/*
 * Prepare every query that will run: each -q in the order given, or else
 * each query the program contains, in load order.
 *
 * Every query is attempted even after one fails, so that one run reports the
 * mistakes in all of them.  Each -q is named by its position among them, as
 * "<query 2>", so that a message says which one it is about.  Returns 0 when
 * every one was prepared.
 */
static int
prepare_queries(session_t *session, const options_t *options)
{
	relic_query_t *query;
	char name[QUERY_NAME_MAX];
	size_t count;
	size_t i;
	int status;

	if (options->query_count > 0)
		count = options->query_count;
	else
		count = relic_query_count(session->ctx);

	if (count == 0)
		return (0);

	session->queries = calloc(count, sizeof(*session->queries));
	if (session->queries == NULL) {
		report_error(session->program_name, "out of memory");
		return (-1);
	}
	session->query_count = count;

	status = 0;
	for (i = 0; i < count; i++) {
		if (options->query_count > 0) {
			snprintf(name, sizeof(name), "<query %zu>", i + 1);
			query = relic_query_prepare(session->ctx, name,
			    options->queries[i]);
		} else {
			query = relic_query_loaded(session->ctx, i);
		}

		if (query == NULL)
			status = -1;
		session->queries[i] = query;
	}

	return (status);
}

/*
 * The phases that decide whether there is anything valid to evaluate: load
 * the input, check the program, prepare the queries.
 *
 * The program is checked only when every operand loaded.  A load is all or
 * nothing, so a file with a syntax error contributes no statements at all,
 * and checking what is left would report the predicates that file defined as
 * referenced and never defined -- complaints about the file that is missing
 * rather than about the program the user wrote.
 */
static int
validate(session_t *session, const options_t *options, int argc,
    char *argv[])
{
	if (load_operands(session->ctx, argc, argv) != 0)
		return (-1);

	if (relic_check(session->ctx) != 0)
		return (-1);

	return (prepare_queries(session, options));
}

/*
 * Where results go: the -o file, created or truncated, or standard output.
 * `*namep` is what a message about a failed write calls it.
 */
static int
open_output(const char *program_name, const options_t *options,
    FILE **streamp, const char **namep)
{
	FILE *stream;

	if (options->output_path == NULL) {
		*streamp = stdout;
		*namep = "standard output";
		return (0);
	}

	stream = fopen(options->output_path, "w");
	if (stream == NULL) {
		report_error(program_name, "cannot open %s: %s",
		    options->output_path, strerror(errno));
		return (-1);
	}

	*streamp = stream;
	*namep = options->output_path;

	return (0);
}

/*
 * Evaluate: materialize the program when asked to, then run each query in
 * turn and write its results.
 *
 * The first query that fails ends the run.  A failure in evaluation is most
 * often an exceeded limit, and it would recur in every query after it.  The
 * results of the queries before it have been written and stay written.
 */
static int
evaluate(session_t *session, const options_t *options)
{
	const char *program_name;
	output_t output;
	FILE *stream;
	const char *name;
	size_t i;
	int status;

	program_name = session->program_name;

	if (options->materialize) {
		if (relic_materialize(session->ctx) != 0)
			return (-1);
	}

	if (open_output(program_name, options, &stream, &name) != 0)
		return (-1);

	output_init(&output, program_name, session->ctx, stream, name,
	    options->format, options->sorted);

	status = 0;
	for (i = 0; i < session->query_count; i++) {
		if (output_query(&output, session->queries[i]) != 0) {
			status = -1;
			break;
		}
	}

	if (output_close(&output) != 0)
		status = -1;

	return (status);
}

static int
session_open(session_t *session, const char *program_name)
{
	session->queries = NULL;
	session->query_count = 0;
	session->program_name = program_name;

	session->ctx = relic_create();
	if (session->ctx == NULL) {
		report_error(program_name, "out of memory");
		return (-1);
	}
	relic_diag_set(session->ctx, relic_diag_stderr,
	    diag_context(program_name));

	return (0);
}

static void
session_close(session_t *session)
{
	size_t i;

	for (i = 0; i < session->query_count; i++)
		relic_query_destroy(session->queries[i]);
	free(session->queries);
	relic_destroy(session->ctx);
}

static int
run(const char *program_name, const options_t *options, int argc,
    char *argv[])
{
	session_t session;
	int status;

	/*
	 * Reading a program from a terminal would leave the tool waiting for
	 * input with no prompt and nothing said.  Interactive use is an option
	 * of its own.
	 */
	if (argc == 0 && isatty(STDIN_FILENO) == 1) {
		usage(program_name);
		return (RELIC_EXIT_USAGE);
	}

	if (session_open(&session, program_name) != 0)
		return (RELIC_EXIT_ERROR);

	status = validate(&session, options, argc, argv);
	if (status == 0 && !options->check_only)
		status = evaluate(&session, options);

	session_close(&session);

	return (status == 0 ? RELIC_EXIT_OK : RELIC_EXIT_ERROR);
}

/*
 * Parse the options into *options.  Returns RELIC_EXIT_OK, or the status to
 * exit with having reported why the command line is malformed.
 *
 * `queries` must have room for every argument, which is the most -q options
 * a command line can hold.
 */
static int
parse_options(const char *program_name, int argc, char *argv[],
    options_t *options)
{
	int ch;

	/*
	 * getopt(3) reports an unknown option by printing argv[0], which is
	 * whatever path the program was invoked by.  Silencing it and
	 * reporting the option here keeps diagnostics identical whether the
	 * tool runs from an absolute path, from the build tree through a
	 * libtool wrapper, or from its installed location, which is what the
	 * golden output tests compare.
	 */
	opterr = 0;

	while ((ch = getopt(argc, argv, ":acF:o:q:sV")) != -1) {
		if (ch != 'V')
			options->version_alone = false;

		switch (ch) {
		case 'a':
			options->materialize = true;
			break;
		case 'c':
			options->check_only = true;
			break;
		case 'F':
			if (output_format_named(optarg,
			    &options->format) != 0) {
				report_error(program_name,
				    "unknown output format: %s", optarg);
				usage(program_name);
				return (RELIC_EXIT_USAGE);
			}
			break;
		case 'o':
			options->output_path = optarg;
			break;
		case 'q':
			options->queries[options->query_count] = optarg;
			options->query_count++;
			break;
		case 's':
			options->sorted = true;
			break;
		case 'V':
			options->print_version = true;
			break;
		case ':':
			report_error(program_name,
			    "option -%c requires an argument", optopt);
			usage(program_name);
			return (RELIC_EXIT_USAGE);
		default:
			report_unknown_option(program_name, optopt);
			usage(program_name);
			return (RELIC_EXIT_USAGE);
		}
	}

	/*
	 * -c forbids evaluation and -a demands it, so asking for both is a
	 * contradiction rather than a combination.
	 */
	if (options->check_only && options->materialize) {
		report_error(program_name, "-a and -c cannot be combined");
		usage(program_name);
		return (RELIC_EXIT_USAGE);
	}

	return (RELIC_EXIT_OK);
}

int
main(int argc, char *argv[])
{
	options_t options;
	const char *program_name;
	int status;

	program_name = invoked_name(argv[0]);

	bzero(&options, sizeof(options));
	options.format = OUTPUT_TEXT;
	options.version_alone = true;

	options.queries = calloc(argc > 0 ? (size_t)argc : 1,
	    sizeof(*options.queries));
	if (options.queries == NULL) {
		report_error(program_name, "out of memory");
		return (RELIC_EXIT_ERROR);
	}

	status = parse_options(program_name, argc, argv, &options);
	if (status != RELIC_EXIT_OK) {
		free(options.queries);
		return (status);
	}
	argc -= optind;
	argv += optind;

	if (options.print_version) {
		free(options.queries);

		if (!options.version_alone || argc != 0) {
			usage(program_name);
			return (RELIC_EXIT_USAGE);
		}

		/*
		 * The version is printed under the package name rather than
		 * the invoked name: it identifies the software, which a
		 * rename does not change, and keeping it independent of
		 * argv[0] keeps the output the same whether the tool runs
		 * from the build tree or from its installed location.
		 */
		printf("%s %s\n", PACKAGE_NAME, relic_version());

		return (RELIC_EXIT_OK);
	}

	status = run(program_name, &options, argc, argv);
	free(options.queries);

	return (status);
}
