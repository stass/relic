/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <errno.h>
#include <limits.h>
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
#include "compat.h"

/*
 * The relic tool: load a program, check it, and answer its queries.
 *
 * An invocation runs in phases, and each runs only if the one before it
 * succeeded: parse the command line, load every operand and then every -I
 * import, check the program, prepare every query that will run, and only then
 * evaluate.  Every mistake in the input is therefore reported before anything
 * is evaluated, and a run that evaluates does so over a program and queries
 * that are all known to be valid.  With -c the run stops after preparing;
 * with -a the whole program is materialized before the first query; each
 * -O writes a relation to its file after the last; with -i an interactive
 * session follows, answering into the same output as the queries.
 *
 * The queries that run are the -q ones when there are any, and otherwise the
 * ones the program contains.  The program's own queries are still checked
 * when -q replaces them, because they are part of the program.
 *
 * -n runs every query and writes none of its rows, and each -v adds to what
 * standard error is told: a count of each query's rows, then notes about
 * comparisons between values of different types.
 *
 * -l sets a resource limit, as key=value, and -X selects internal behaviour
 * for testing and debugging: an alternative evaluation strategy, whose
 * answers must equal the default's, or counters printed to standard error
 * when the run ends.  Both only fill in the configuration the context is
 * created with.
 */

/* A diagnostic of the tool's own is formatted into this many bytes. */
#define	REPORT_TEXT_MAX	512

/*
 * The largest value -l accepts: a limit is a size_t, and strtonum() returns a
 * long long, so whichever of the two is smaller.
 */
#define	LIMIT_VALUE_MAX	((uintmax_t)SIZE_MAX < (uintmax_t)LLONG_MAX ? \
	(long long)SIZE_MAX : LLONG_MAX)

typedef struct limit_key limit_key_t;

/*
 * One -l key and the field of the configuration it sets.
 */
struct limit_key {
	const char	*name;
	size_t		*field;
};

typedef struct options options_t;

/*
 * Who reads standard input.  It has one reader at most: the program when no
 * file is named, the interactive session, or one import of `-`.
 */
enum stdin_reader {
	STDIN_UNREAD,
	STDIN_PROGRAM,
	STDIN_SESSION,
	STDIN_IMPORT
};

/*
 * What the command line asked for.  `queries` holds the -q arguments in the
 * order given; they point into argv and are not copied.  `imports` holds the
 * -I operands, parsed, in the order given, and `types` the -T operands
 * their columns may come from.  `exports` holds the -O operands, parsed, in
 * the order given.  `config` is what the context is created with: -j sets
 * its thread count, -l its limits, -X naive, no-index, no-reorder,
 * no-magic, no-factor, no-stream, no-exists, no-batch-import, parallel and
 * group-rules its flags, and a second -v the flag that asks for notes about
 * mixed-type comparisons.
 * `verbosity` counts the -v options.
 *
 * `stdin_reader` is who reads standard input, which parse_options() decides
 * once every option and operand is known, and `stdin_import` the import that
 * does when one does.
 */
struct options {
	const char		**queries;
	import_operand_t	 *imports;
	column_types_t		 *types;
	export_operand_t	 *exports;
	const import_operand_t	 *stdin_import;
	const char		 *output_path;
	size_t			  query_count;
	size_t			  import_count;
	size_t			  type_count;
	size_t			  export_count;
	relic_config_t		  config;
	enum output_format	  format;
	enum stdin_reader	  stdin_reader;
	unsigned int		  verbosity;
	bool			  check_only;
	bool			  discard_rows;
	bool			  explain;
	bool			  interactive;
	bool			  materialize;
	bool			  print_stats;
	bool			  print_version;
	bool			  sorted;
	bool			  version_alone;
};

static int		 attach_output(const options_t *options,
			    output_t *output);
static int		 claim_stdin(const char *program_name,
			    options_t *options, int file_count);
static void		 command_files_of(const options_t *options,
			    char *const *programs, int program_count,
			    command_files_t *files);
static void		*diag_context(const char *program_name);
static int		 evaluate(invocation_t *invocation,
			    const options_t *options);
static void		 explain_to_stream(void *callback_ctx,
			    const char *line);
static void		 invocation_close(invocation_t *invocation);
static int		 invocation_open(invocation_t *invocation,
			    const char *program_name, const options_t *options);
static const char	*invoked_name(const char *argv0);
static bool		 is_decimal_digit(char byte);
static bool		 is_printable_ascii(const char *text);
static size_t		*limit_named(relic_config_t *config, const char *key,
			    size_t key_length);
static int		 load_operands(relic_t *ctx, const options_t *options,
			    int argc, char *argv[]);
static void		 options_free(options_t *options);
static int		 parse_debug(const char *name, options_t *options);
static int		 parse_limit(const char *program_name,
			    const char *argument, relic_config_t *config);
static int		 parse_options(const char *program_name, int argc,
			    char *argv[], options_t *options);
static int		 parse_threads(const char *program_name,
			    const char *argument, relic_config_t *config);
static int		 prepare_queries(invocation_t *invocation,
			    const options_t *options);
static bool		 refuse_terminal_input(const char *program_name,
			    const options_t *options);
static void		 report_diagnostic(void *callback_ctx,
			    enum relic_diag_level level,
			    const relic_location_t *where, const char *message);
static void		 report_two_readers(const char *program_name,
			    const options_t *options,
			    const import_operand_t *second);
static void		 report_unknown_option(const char *program_name,
			    int option);
static int		 run(const char *program_name,
			    const options_t *options, int argc, char *argv[]);
static int		 run_queries(invocation_t *invocation,
			    output_t *output);
static void		 usage(const char *program_name);
static int		 validate(invocation_t *invocation,
			    const options_t *options, int argc, char *argv[]);

/*
 * Whether a byte is an ASCII decimal digit, whatever the locale says.
 */
static bool
is_decimal_digit(char byte)
{
	return (byte >= '0' && byte <= '9');
}

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
 * Report an option the tool does not accept, as an error like any other.
 *
 * optopt holds a byte taken straight from the command line and may be any
 * value at all.  report_error() writes a byte outside printable ASCII by its
 * value rather than as itself, so the diagnostic cannot carry a terminal
 * escape sequence out of the argument vector and into the reader's
 * terminal.  getopt(3) never reports a NUL, which ends an argument.
 */
static void
report_unknown_option(const char *program_name, int option)
{
	report_error(program_name, "unknown option -- %c", option & 0xff);
}

static void
usage(const char *program_name)
{
	fprintf(stderr, "usage: %s [-aceinsv] [-F format] [-I import] "
	    "[-j threads]\n"
	    "       %*s [-l limit=value] [-O export] [-o file] [-q query]\n"
	    "       %*s [-T types] [-X debug] [file ...]\n", program_name,
	    (int)strlen(program_name), "", (int)strlen(program_name), "");
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

/*
 * The context's diagnostic callback: hand the rows the output has gathered
 * to its stream, then write the diagnostic as relic_diag_stderr() does.  A
 * diagnostic reported while a query delivers rows -- an interrupt, a note
 * -vv asks for -- then follows every row delivered before it on a terminal,
 * as it would were the output not buffered.  The library calls it between
 * rows, never inside one, so the rows handed over end where a row does.
 */
static void
report_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	const invocation_t *invocation;

	invocation = callback_ctx;
	if (invocation->output != NULL)
		output_write_buffered(invocation->output);

	relic_diag_stderr(diag_context(invocation->program_name), level, where,
	    message);
}

void
report_error(const char *program_name, const char *format, ...)
{
	va_list arguments;
	char message[REPORT_TEXT_MAX];

	va_start(arguments, format);
	if (vsnprintf(message, sizeof(message), format, arguments) < 0)
		message[0] = '\0';
	va_end(arguments);

	relic_diag_stderr(diag_context(program_name), RELIC_ERROR, NULL,
	    message);
}

const char *
cmd_quote(const char *bytes, size_t length, cmd_quoted_t *quoted)
{
	size_t kept;

	kept = length < CMD_QUOTED_MAX ? length : CMD_QUOTED_MAX;
	bcopy(bytes, quoted->text, kept);
	quoted->text[kept] = '\0';
	if (kept < length)
		strlcat(quoted->text, "...", sizeof(quoted->text));

	return (quoted->text);
}

/*
 * The line -v adds after each query or export that succeeded.  Like the
 * counters of -X stats it is data about the run rather than a complaint, so
 * it is written without the program name and reads the same however the
 * tool was invoked.
 *
 * Standard output is flushed first.  The query's rows have reached their
 * stream already, but the plan .plan wrote may wait in standard output's
 * buffer when the rows go elsewhere, and flushing it is what makes the count
 * follow it where a terminal or a pipe shows both streams, rather than
 * overtake it from standard error, which is not buffered.
 */
void
report_rows(const invocation_t *invocation, const char *what, size_t number,
    size_t rows)
{
	if (invocation->verbosity < 1)
		return;

	fflush(stdout);
	fprintf(stderr, "%s %zu: %zu %s\n", what, number, rows,
	    rows == 1 ? "row" : "rows");
}

/*
 * Load the program -- every operand, or standard input when there are none
 * and nothing else reads it -- and then every import in the order given.
 * Standard input goes to its one reader, as claim_stdin() decided, and to
 * no other.
 *
 * The program comes first so that its diagnostics precede the data's.  Every
 * file and every import is attempted even after one of them fails, so that a
 * run reports the mistakes in all of the input rather than in whichever
 * happened to be named first.
 */
static int
load_operands(relic_t *ctx, const options_t *options, int argc, char *argv[])
{
	const import_operand_t *import;
	size_t j;
	int loaded;
	int status;
	int i;

	status = 0;
	if (options->stdin_reader == STDIN_PROGRAM) {
		if (relic_load_file(ctx, "<stdin>", stdin) != 0)
			status = -1;
	}

	for (i = 0; i < argc; i++) {
		if (relic_load_path(ctx, argv[i]) != 0)
			status = -1;
	}

	for (j = 0; j < options->import_count; j++) {
		import = &options->imports[j];
		if (import == options->stdin_import)
			loaded = relic_import_file(ctx, "<stdin>", stdin,
			    &import->import);
		else
			loaded = relic_import_path(ctx, import->path,
			    &import->import);
		if (loaded != 0)
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
prepare_queries(invocation_t *invocation, const options_t *options)
{
	relic_query_t *query;
	char name[CMD_QUERY_NAME_MAX];
	size_t count;
	size_t i;
	int status;

	if (options->query_count > 0)
		count = options->query_count;
	else
		count = relic_query_count(invocation->ctx);

	if (count == 0)
		return (0);

	invocation->queries = calloc(count, sizeof(*invocation->queries));
	if (invocation->queries == NULL) {
		report_error(invocation->program_name, "out of memory");
		return (-1);
	}
	invocation->query_count = count;

	status = 0;
	for (i = 0; i < count; i++) {
		if (options->query_count > 0) {
			snprintf(name, sizeof(name), "<query %zu>", i + 1);
			query = relic_query_prepare(invocation->ctx, name,
			    options->queries[i]);
		} else {
			query = relic_query_loaded(invocation->ctx, i);
		}

		if (query == NULL)
			status = -1;
		invocation->queries[i] = query;
	}

	return (status);
}

/*
 * The phases that decide whether there is anything valid to evaluate: load
 * the input, check the program, and prepare the queries and the query of
 * each export.  The exports are prepared even when a query could not be, so
 * that one run reports the mistakes in both.
 *
 * The program is checked only when every operand loaded.  A load is all or
 * nothing, so a file with a syntax error contributes no statements at all,
 * and checking what is left would report the predicates that file defined as
 * referenced and never defined -- complaints about the file that is missing
 * rather than about the program the user wrote.
 */
static int
validate(invocation_t *invocation, const options_t *options, int argc,
    char *argv[])
{
	int status;

	if (load_operands(invocation->ctx, options, argc, argv) != 0)
		return (-1);

	if (relic_check(invocation->ctx) != 0)
		return (-1);

	status = prepare_queries(invocation, options);
	if (exports_prepare(invocation, options->exports,
	    options->export_count) != 0)
		status = -1;

	return (status);
}

/*
 * Attach the output results are written to: the -o file or standard output.
 * The -o file is created or truncated only once a query has been answered,
 * or once a run with no query succeeds, so a run that fails before then --
 * in evaluating its first query, at a limit included -- leaves an existing
 * file as it was.  Under -n nothing is attached, so the output discards
 * every row and no file is created.
 */
static int
attach_output(const options_t *options, output_t *output)
{
	if (options->discard_rows)
		return (0);

	if (options->output_path == NULL)
		return (output_attach(output, stdout, "standard output",
		    options->format, options->sorted));

	return (output_attach_file(output, options->output_path,
	    options->format, NULL, options->sorted));
}

/*
 * Run each prepared query in turn, the -q queries or else the program's own,
 * writing its results.  A query's number is its position among them.
 *
 * The first query that fails ends the run.  A failure in evaluation is most
 * often an exceeded limit, and it would recur in every query after it.  The
 * results of the queries before it have been written and stay written.
 */
static int
run_queries(invocation_t *invocation, output_t *output)
{
	size_t rows;
	size_t i;

	for (i = 0; i < invocation->query_count; i++) {
		if (output_query(output, invocation->queries[i], &rows) != 0)
			return (-1);
		report_rows(invocation, "query", i + 1, rows);
	}

	return (0);
}

/*
 * Evaluate: materialize the program when asked to, run the queries, write
 * the exports, and then run the interactive session when asked for one.
 * The exports follow the queries, whose answers someone may be waiting
 * for, and precede the session, so that what a file holds does not depend
 * on how the session went.  The session answers into the same output as
 * the queries, so its results follow theirs under the same format and
 * separators, and its first query is numbered after theirs.
 */
static int
evaluate(invocation_t *invocation, const options_t *options)
{
	output_t output;
	int status;

	if (options->materialize) {
		if (relic_materialize(invocation->ctx) != 0)
			return (-1);
	}

	output_init(&output, invocation->program_name, invocation->ctx);
	if (attach_output(options, &output) != 0)
		return (-1);
	invocation->output = &output;

	status = run_queries(invocation, &output);

	if (status == 0)
		status = exports_write(invocation, options->sorted);

	if (status == 0 && options->interactive)
		status = repl_run(invocation, &output,
		    invocation->query_count + 1);

	invocation->output = NULL;
	if (output_close(&output, status == 0) != 0)
		status = -1;

	return (status);
}

/*
 * Write one line of a plan to the stream the callback was installed with.
 * The library promises the line is printable ASCII, so it is written as it
 * is.
 */
static void
explain_to_stream(void *callback_ctx, const char *line)
{
	FILE *stream;

	stream = callback_ctx;
	fprintf(stream, "%s\n", line);
}

void
explain_into(relic_t *ctx, FILE *stream)
{
	if (stream == NULL)
		relic_explain_set(ctx, NULL, NULL);
	else
		relic_explain_set(ctx, explain_to_stream, stream);
}

static int
invocation_open(invocation_t *invocation, const char *program_name,
    const options_t *options)
{
	invocation->queries = NULL;
	invocation->query_count = 0;
	invocation->exports = NULL;
	invocation->export_count = 0;
	invocation->output = NULL;
	invocation->program_name = program_name;
	invocation->verbosity = options->verbosity;
	invocation->explain_stream = options->explain ? stderr : NULL;

	/*
	 * Creation fails for one of two reasons and does not say which, so
	 * the message names both.
	 */
	invocation->ctx = relic_create_config(&options->config);
	if (invocation->ctx == NULL) {
		report_error(program_name,
		    "cannot create a context: out of memory or no random device");
		return (-1);
	}
	relic_diag_set(invocation->ctx, report_diagnostic, invocation);

	explain_into(invocation->ctx, invocation->explain_stream);

	return (0);
}

/*
 * Write the context's counters to `stream`, one per line.
 *
 * The lines are not prefixed with the program name, unlike the tool's own
 * messages: they are data about the run rather than a complaint about it,
 * and their text is the same however the tool was invoked, which is what
 * lets a golden test compare them.
 */
void
write_stats(relic_t *ctx, FILE *stream)
{
	relic_stats_t stats;

	relic_stats(ctx, &stats);

	fprintf(stream, "stats: iterations %ju\n",
	    (uintmax_t)stats.iterations);
	fprintf(stream, "stats: derivations %ju\n",
	    (uintmax_t)stats.derivations);
	fprintf(stream, "stats: tuples inserted %ju\n",
	    (uintmax_t)stats.tuples_inserted);
	fprintf(stream, "stats: tuples scanned %ju\n",
	    (uintmax_t)stats.tuples_scanned);
	fprintf(stream, "stats: tuples looked up %ju\n",
	    (uintmax_t)stats.tuples_looked_up);
	fprintf(stream, "stats: parallel rounds %ju\n",
	    (uintmax_t)stats.parallel_rounds);
	fprintf(stream, "stats: tuples held %zu\n", stats.tuples);
	fprintf(stream, "stats: storage bytes %zu\n", stats.storage_bytes);
	fprintf(stream, "stats: symbol values %zu\n", stats.symbol_values);
	fprintf(stream, "stats: string values %zu\n", stats.string_values);
	fprintf(stream, "stats: integer values %zu\n", stats.integer_values);
}

static void
invocation_close(invocation_t *invocation)
{
	size_t i;

	for (i = 0; i < invocation->query_count; i++)
		relic_query_destroy(invocation->queries[i]);
	free(invocation->queries);
	for (i = 0; i < invocation->export_count; i++)
		relic_query_destroy(invocation->exports[i].query);
	free(invocation->exports);
	relic_destroy(invocation->ctx);
}

/*
 * Standard input is a terminal: refuse the run when the program or a table
 * would be read from it, as the usage error it is, and return whether it was
 * refused.  The tool would otherwise wait for input with no prompt and
 * nothing said.  Interactive use is an option of its own, and a terminal is
 * the session's to read.
 */
static bool
refuse_terminal_input(const char *program_name, const options_t *options)
{
	cmd_quoted_t operand;

	switch (options->stdin_reader) {
	case STDIN_PROGRAM:
		usage(program_name);
		return (true);
	case STDIN_IMPORT:
		report_error(program_name, "standard input is a terminal, and "
		    "-I %s would read a table from it",
		    cmd_quote(options->stdin_import->operand,
		    strlen(options->stdin_import->operand), &operand));
		usage(program_name);
		return (true);
	case STDIN_SESSION:
	case STDIN_UNREAD:
		break;
	}

	return (false);
}

static int
run(const char *program_name, const options_t *options, int argc,
    char *argv[])
{
	invocation_t invocation;
	int status;

	if (isatty(STDIN_FILENO) == 1 &&
	    refuse_terminal_input(program_name, options))
		return (RELIC_EXIT_USAGE);

	if (invocation_open(&invocation, program_name, options) != 0)
		return (RELIC_EXIT_ERROR);

	status = validate(&invocation, options, argc, argv);
	if (status == 0 && !options->check_only)
		status = evaluate(&invocation, options);

	/*
	 * The counters are printed whether or not the run succeeded: the work
	 * an evaluation did before a limit stopped it is often what the reader
	 * wants to see.
	 */
	if (options->print_stats)
		write_stats(invocation.ctx, stderr);

	invocation_close(&invocation);

	return (status == 0 ? RELIC_EXIT_OK : RELIC_EXIT_ERROR);
}

/*
 * Apply one -X argument.  Returns 0, or -1 when it names no behaviour.
 *
 * Only the behaviours that exist are accepted.  A name for one that does not
 * would be accepted and do nothing, and a comparison run with it would pass
 * while comparing a strategy with itself.
 */
static int
parse_debug(const char *name, options_t *options)
{
	if (strcmp(name, "naive") == 0) {
		options->config.flags |= RELIC_F_NAIVE;
		return (0);
	}

	if (strcmp(name, "no-index") == 0) {
		options->config.flags |= RELIC_F_NO_INDEX;
		return (0);
	}

	if (strcmp(name, "no-reorder") == 0) {
		options->config.flags |= RELIC_F_NO_REORDER;
		return (0);
	}

	if (strcmp(name, "no-magic") == 0) {
		options->config.flags |= RELIC_F_NO_MAGIC;
		return (0);
	}

	if (strcmp(name, "no-stream") == 0) {
		options->config.flags |= RELIC_F_NO_STREAM;
		return (0);
	}

	if (strcmp(name, "no-exists") == 0) {
		options->config.flags |= RELIC_F_NO_EXISTS;
		return (0);
	}

	if (strcmp(name, "no-factor") == 0) {
		options->config.flags |= RELIC_F_NO_FACTOR;
		return (0);
	}

	if (strcmp(name, "no-batch-import") == 0) {
		options->config.flags |= RELIC_F_NO_BATCH_IMPORT;
		return (0);
	}

	if (strcmp(name, "parallel") == 0) {
		options->config.flags |= RELIC_F_PARALLEL_ALWAYS;
		return (0);
	}

	if (strcmp(name, "group-rules") == 0) {
		options->config.flags |= RELIC_F_GROUP_RULES;
		return (0);
	}

	if (strcmp(name, "stats") == 0) {
		options->print_stats = true;
		return (0);
	}

	return (-1);
}

/*
 * The field of the configuration a -l key names, or NULL for a key that names
 * no limit.  The key is the `key_length` bytes at `key`, which the caller has
 * cut from a key=value argument, so it is not terminated where it ends.
 */
static size_t *
limit_named(relic_config_t *config, const char *key, size_t key_length)
{
	const limit_key_t limits[] = {
		{ "tuples", &config->max_tuples },
		{ "memory", &config->max_memory },
		{ "input", &config->max_input },
		{ "magic", &config->max_magic }
	};
	size_t i;

	for (i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
		if (strlen(limits[i].name) == key_length &&
		    bcmp(limits[i].name, key, key_length) == 0)
			return (limits[i].field);
	}

	return (NULL);
}

/*
 * Apply one -l argument, key=value, to the configuration.  Returns 0, or -1
 * having reported why the argument is not a limit.
 *
 * The value is decimal digits and nothing else, from zero, which asks for
 * the library's default, to the largest a limit can hold.  strtonum()
 * rejects trailing bytes, an empty value and a value out of range, and says
 * which it was; it would also skip leading space and accept a sign, so the
 * value must begin with a digit before it is asked.  A key given twice takes
 * the later value, as a repeated option does.
 */
static int
parse_limit(const char *program_name, const char *argument,
    relic_config_t *config)
{
	const char *equals;
	const char *error;
	size_t *field;
	long long value;

	equals = strchr(argument, '=');
	if (equals == NULL) {
		report_error(program_name,
		    "a limit is written key=value: %s", argument);
		return (-1);
	}

	field = limit_named(config, argument, (size_t)(equals - argument));
	if (field == NULL) {
		report_error(program_name, "unknown limit: %s", argument);
		return (-1);
	}

	value = strtonum(equals + 1, 0, LIMIT_VALUE_MAX, &error);
	if (error == NULL && !is_decimal_digit(equals[1]))
		error = "invalid";
	if (error != NULL) {
		report_error(program_name, "limit value is %s: %s", error,
		    argument);
		return (-1);
	}

	*field = (size_t)value;

	return (0);
}

/*
 * Parse -j's argument into the thread count evaluation may use: a decimal
 * number from 0 to RELIC_THREADS_MAX, zero asking for as many as the
 * system has processors online, within the same bound.  The number is
 * validated as a limit's value is.
 */
static int
parse_threads(const char *program_name, const char *argument,
    relic_config_t *config)
{
	const char *error;
	long long value;
	long online;

	value = strtonum(argument, 0, RELIC_THREADS_MAX, &error);
	if (error == NULL && !is_decimal_digit(argument[0]))
		error = "invalid";
	if (error != NULL) {
		report_error(program_name, "thread count is %s: %s", error,
		    argument);
		return (-1);
	}

	if (value == 0) {
		online = sysconf(_SC_NPROCESSORS_ONLN);
		value = online < 1 ? 1 : online > RELIC_THREADS_MAX ?
		    RELIC_THREADS_MAX : online;
	}

	config->threads = (size_t)value;

	return (0);
}

/*
 * Decide who reads standard input: the session under -i, an import of `-`,
 * or, when neither does, the program when no file is named.  Returns
 * RELIC_EXIT_OK, or RELIC_EXIT_USAGE having reported that two claim it,
 * since each would take what the other expects.
 *
 * The claim is on the spelling `-`: a path such as /dev/stdin is a file like
 * any other.
 */
static int
claim_stdin(const char *program_name, options_t *options, int file_count)
{
	size_t i;

	options->stdin_reader = STDIN_UNREAD;
	if (options->interactive)
		options->stdin_reader = STDIN_SESSION;

	for (i = 0; i < options->import_count; i++) {
		const import_operand_t *import;

		import = &options->imports[i];
		if (!import_operand_reads_stdin(import))
			continue;

		if (options->stdin_reader != STDIN_UNREAD) {
			report_two_readers(program_name, options, import);
			return (RELIC_EXIT_USAGE);
		}
		options->stdin_reader = STDIN_IMPORT;
		options->stdin_import = import;
	}

	if (options->stdin_reader == STDIN_UNREAD && file_count == 0)
		options->stdin_reader = STDIN_PROGRAM;

	return (RELIC_EXIT_OK);
}

/*
 * Report that an import of `-` claims standard input after the session or
 * an earlier import already has, as a usage error naming both.
 */
static void
report_two_readers(const char *program_name, const options_t *options,
    const import_operand_t *second)
{
	cmd_quoted_t first_operand;
	cmd_quoted_t second_operand;

	cmd_quote(second->operand, strlen(second->operand), &second_operand);
	if (options->stdin_reader == STDIN_SESSION) {
		report_error(program_name, "standard input has two readers: "
		    "-i and -I %s", second_operand.text);
	} else {
		cmd_quote(options->stdin_import->operand,
		    strlen(options->stdin_import->operand), &first_operand);
		report_error(program_name, "standard input has two readers: "
		    "-I %s and -I %s", first_operand.text, second_operand.text);
	}
	usage(program_name);
}

/*
 * Collect every file the command line reads and writes: the
 * `program_count` program operands at `programs`, the imports' tables, the
 * -o file and the exports', and standard input and output when the run
 * reads or writes them.  Standard input's reader must be decided already.
 * Standard output is written when the results go to it, or the replies of
 * an interactive session; -c writes neither.
 */
static void
command_files_of(const options_t *options, char *const *programs,
    int program_count, command_files_t *files)
{
	bzero(files, sizeof(*files));
	files->programs = programs;
	files->program_count = program_count > 0 ? (size_t)program_count : 0;
	files->imports = options->imports;
	files->import_count = options->import_count;
	files->reads_stdin = options->stdin_reader != STDIN_UNREAD;
	files->writes_stdout = !options->check_only && (options->interactive ||
	    (options->output_path == NULL && !options->discard_rows));
	files->output_path = options->output_path;
	files->exports = options->exports;
	files->export_count = options->export_count;
}

/*
 * Parse the options into *options, and decide who reads standard input
 * once every option and operand is known.  Returns RELIC_EXIT_OK, or the
 * status to exit with having reported why the command line is malformed.
 *
 * `queries`, `imports`, `types` and `exports` must each have room for every
 * argument, which is the most -q, -I, -T or -O options a command line can
 * hold.
 */
static int
parse_options(const char *program_name, int argc, char *argv[],
    options_t *options)
{
	command_files_t files;
	int ch;
	int status;

	/*
	 * getopt(3) reports an unknown option by printing argv[0], which is
	 * whatever path the program was invoked by.  Silencing it and
	 * reporting the option here keeps diagnostics identical whether the
	 * tool runs from an absolute path, from the build tree through a
	 * libtool wrapper, or from its installed location, which is what the
	 * golden output tests compare.
	 */
	opterr = 0;

	while ((ch = getopt(argc, argv, ":aceF:I:ij:l:nO:o:q:sT:vVX:")) != -1) {
		if (ch != 'V')
			options->version_alone = false;

		switch (ch) {
		case 'a':
			options->materialize = true;
			break;
		case 'c':
			options->check_only = true;
			break;
		case 'e':
			options->explain = true;
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
		case 'I':
			switch (import_operand_parse(program_name, optarg,
			    &options->imports[options->import_count])) {
			case OPERAND_PARSED:
				options->import_count++;
				break;
			case OPERAND_MALFORMED:
				usage(program_name);
				return (RELIC_EXIT_USAGE);
			case OPERAND_NO_MEMORY:
				return (RELIC_EXIT_ERROR);
			}
			break;
		case 'i':
			options->interactive = true;
			break;
		case 'j':
			if (parse_threads(program_name, optarg,
			    &options->config) != 0) {
				usage(program_name);
				return (RELIC_EXIT_USAGE);
			}
			break;
		case 'l':
			if (parse_limit(program_name, optarg,
			    &options->config) != 0) {
				usage(program_name);
				return (RELIC_EXIT_USAGE);
			}
			break;
		case 'n':
			options->discard_rows = true;
			break;
		case 'O':
			switch (export_operand_parse(program_name, optarg,
			    &options->exports[options->export_count])) {
			case OPERAND_PARSED:
				options->export_count++;
				break;
			case OPERAND_MALFORMED:
				usage(program_name);
				return (RELIC_EXIT_USAGE);
			case OPERAND_NO_MEMORY:
				return (RELIC_EXIT_ERROR);
			}
			break;
		case 'o':
			if (optarg[0] == '\0') {
				report_error(program_name,
				    "invalid -o file: empty path");
				usage(program_name);
				return (RELIC_EXIT_USAGE);
			}
			options->output_path = optarg;
			break;
		case 'q':
			options->queries[options->query_count] = optarg;
			options->query_count++;
			break;
		case 's':
			options->sorted = true;
			break;
		case 'T':
			switch (column_types_parse(program_name, optarg,
			    &options->types[options->type_count])) {
			case OPERAND_PARSED:
				options->type_count++;
				break;
			case OPERAND_MALFORMED:
				usage(program_name);
				return (RELIC_EXIT_USAGE);
			case OPERAND_NO_MEMORY:
				return (RELIC_EXIT_ERROR);
			}
			break;
		case 'v':
			if (options->verbosity < UINT_MAX)
				options->verbosity++;
			break;
		case 'V':
			options->print_version = true;
			break;
		case 'X':
			if (parse_debug(optarg, options) != 0) {
				report_error(program_name,
				    "unknown debug behaviour: %s", optarg);
				usage(program_name);
				return (RELIC_EXIT_USAGE);
			}
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

	/* An import's columns may come from a -T given after it. */
	if (import_operands_resolve(program_name, options->imports,
	    options->import_count, options->types, options->type_count) != 0) {
		usage(program_name);
		return (RELIC_EXIT_USAGE);
	}

	/*
	 * -c forbids evaluation, and -a and -i demand it, so asking for either
	 * with -c is a contradiction rather than a combination.
	 */
	if (options->check_only && options->materialize) {
		report_error(program_name, "-a and -c cannot be combined");
		usage(program_name);
		return (RELIC_EXIT_USAGE);
	}
	if (options->check_only && options->interactive) {
		report_error(program_name, "-c and -i cannot be combined");
		usage(program_name);
		return (RELIC_EXIT_USAGE);
	}

	/*
	 * The second -v asks the library for the notes it gives only on
	 * request.
	 */
	if (options->verbosity >= 2)
		options->config.flags |= RELIC_F_NOTE_MIXED;

	/* -V reads nothing, and main() refuses it with anything else. */
	if (options->print_version)
		return (RELIC_EXIT_OK);

	status = claim_stdin(program_name, options, argc - optind);
	if (status != RELIC_EXIT_OK)
		return (status);

	/*
	 * Which files the run reads and writes is known only once the whole
	 * command line is, standard input's reader included.
	 */
	command_files_of(options, argv + optind, argc - optind, &files);
	status = command_files_check(program_name, &files);
	if (status == RELIC_EXIT_USAGE)
		usage(program_name);

	return (status);
}

static void
options_free(options_t *options)
{
	size_t i;

	for (i = 0; i < options->import_count; i++)
		import_operand_free(&options->imports[i]);
	free(options->imports);
	for (i = 0; i < options->type_count; i++)
		column_types_free(&options->types[i]);
	free(options->types);
	for (i = 0; i < options->export_count; i++)
		export_operand_free(&options->exports[i]);
	free(options->exports);
	free(options->queries);
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
	options.imports = calloc(argc > 0 ? (size_t)argc : 1,
	    sizeof(*options.imports));
	options.types = calloc(argc > 0 ? (size_t)argc : 1,
	    sizeof(*options.types));
	options.exports = calloc(argc > 0 ? (size_t)argc : 1,
	    sizeof(*options.exports));
	if (options.queries == NULL || options.imports == NULL ||
	    options.types == NULL || options.exports == NULL) {
		report_error(program_name, "out of memory");
		options_free(&options);
		return (RELIC_EXIT_ERROR);
	}

	status = parse_options(program_name, argc, argv, &options);
	if (status != RELIC_EXIT_OK) {
		options_free(&options);
		return (status);
	}
	argc -= optind;
	argv += optind;

	if (options.print_version) {
		options_free(&options);

		if (!options.version_alone || argc != 0) {
			report_error(program_name,
			    "-V takes no other option or operand");
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
		errno = 0;
		(void)printf("%s %s\n", PACKAGE_NAME, relic_version());
		if (fflush(stdout) != 0 || ferror(stdout)) {
			if (errno != 0)
				report_error(program_name,
				    "cannot write standard output: %s",
				    strerror(errno));
			else
				report_error(program_name,
				    "cannot write standard output");
			return (RELIC_EXIT_ERROR);
		}

		return (RELIC_EXIT_OK);
	}

	status = run(program_name, &options, argc, argv);
	options_free(&options);

	return (status);
}
