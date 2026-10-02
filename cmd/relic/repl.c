/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <relic.h>

#include "cmd.h"
#include "compat.h"

/*
 * The interactive session of -i.
 *
 * A session reads standard input a line at a time and answers each line
 * before reading the next.  A line is blank, a comment, a command beginning
 * with a dot, or one query.  A query is prepared and run exactly as a -q
 * query is, into the same output, so what a session answers looks exactly
 * like what a run answers.
 *
 * Only queries are accepted.  Preparing a query adds nothing to the program,
 * so a mistyped query costs its diagnostic and nothing more, where a rule
 * that failed its check would leave the program rejected for the rest of the
 * session.
 *
 * A line that fails is reported and the session goes on to the next; the
 * session fails, and with it the run, when any line failed.  It ends at the
 * end of input, at .quit, when a read fails, or when a write fails, since
 * nothing answered after that could be seen.
 *
 * A command's reply goes to standard output and never to the -o file, which
 * holds query results and nothing else.
 */

/*
 * The bytes that separate words on a line: those the language treats as
 * whitespace, short of the newline that ends the line.
 */
#define	BLANKS			" \t\r\v\f"

typedef struct repl repl_t;
typedef struct repl_command repl_command_t;

/*
 * An interactive session in progress, within the invocation that holds the
 * context it answers from.  `next_number` is the number the next query is
 * given, continuing the numbering of the queries that ran before the
 * session.  `done` is set once no further line is to be read: by .quit, at
 * the end of input, and when a read fails.
 */
struct repl {
	invocation_t	*invocation;
	output_t	*output;
	size_t		 next_number;
	bool		 done;
};

/*
 * One command: its name without the dot, how .help shows it and what it
 * says it does, and whether the rest of the line is a query it takes or must
 * be empty.  `run` is given the line with the command's name blanked out,
 * which for a command taking a query is that query where it was typed.
 */
struct repl_command {
	const char	*name;
	const char	*synopsis;
	const char	*description;
	int		(*run)(repl_t *repl, const char *argument);
	bool		 takes_query;
};

static int	 answer_line(repl_t *repl, char *line, size_t length,
		    size_t line_number);
static int	 answer_next_line(repl_t *repl, line_reader_t *reader);
static int	 answer_query(repl_t *repl, const char *text);
static int	 command_help(repl_t *repl, const char *argument);
static const repl_command_t *command_named(const char *name, size_t length);
static int	 command_plan(repl_t *repl, const char *argument);
static int	 command_quit(repl_t *repl, const char *argument);
static int	 command_stats(repl_t *repl, const char *argument);
static int	 flush_answers(const repl_t *repl);
static relic_query_t *prepare_numbered(repl_t *repl, const char *text,
		    size_t *numberp);
static int	 run_command(repl_t *repl, char *line, char *dot);

/* The commands, in the order .help lists them. */
static const repl_command_t commands[] = {
	{ "help", ".help", "list the commands", command_help, false },
	{ "plan", ".plan QUERY", "run QUERY and show its plan instead of "
	    "its rows", command_plan, true },
	{ "quit", ".quit", "end the session", command_quit, false },
	{ "stats", ".stats", "show the evaluation counters", command_stats,
	    false }
};

/*
 * Prepare a query from text, naming it by the next number whether or not it
 * can be prepared, so that the diagnostics about two lines never share a
 * name.
 */
static relic_query_t *
prepare_numbered(repl_t *repl, const char *text, size_t *numberp)
{
	char name[CMD_QUERY_NAME_MAX];

	*numberp = repl->next_number;
	repl->next_number++;

	snprintf(name, sizeof(name), "<query %zu>", *numberp);

	return (relic_query_prepare(repl->invocation->ctx, name, text));
}

/*
 * Answer one query into the output, and count its rows under -v.
 */
static int
answer_query(repl_t *repl, const char *text)
{
	relic_query_t *query;
	size_t number;
	size_t rows;
	int status;

	query = prepare_numbered(repl, text, &number);
	if (query == NULL)
		return (-1);

	status = output_query(repl->output, query, &rows);
	relic_query_destroy(query);
	if (status != 0)
		return (-1);

	report_rows(repl->invocation, number, rows);

	return (0);
}

static int
command_help(repl_t *repl RELIC_CMD_UNUSED,
    const char *argument RELIC_CMD_UNUSED)
{
	size_t i;

	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
		printf("%-14s%s\n", commands[i].synopsis,
		    commands[i].description);

	return (0);
}

/*
 * Run a query with its plan written to standard output and its rows
 * discarded.  The plan is the one evaluation chooses and runs, which is why
 * the query is run rather than only planned: over a program not yet
 * evaluated it includes the Magic rewrite and every component the query
 * evaluates, and after -a only the query's own plan.  Wherever -e sends
 * plans, this one goes to standard output like any reply, and -e's
 * destination is restored afterwards.
 */
static int
command_plan(repl_t *repl, const char *argument)
{
	relic_query_t *query;
	relic_t *ctx;
	size_t number;
	size_t rows;
	int status;

	ctx = repl->invocation->ctx;

	query = prepare_numbered(repl, argument, &number);
	if (query == NULL)
		return (-1);

	explain_into(ctx, stdout);
	status = output_discard(repl->output, query, &rows);
	explain_into(ctx, repl->invocation->explain_stream);

	relic_query_destroy(query);
	if (status != 0)
		return (-1);

	report_rows(repl->invocation, number, rows);

	return (0);
}

static int
command_quit(repl_t *repl, const char *argument RELIC_CMD_UNUSED)
{
	repl->done = true;

	return (0);
}

static int
command_stats(repl_t *repl, const char *argument RELIC_CMD_UNUSED)
{
	write_stats(repl->invocation->ctx, stdout);

	return (0);
}

/*
 * The command whose name is the `length` bytes at `name`, or NULL.
 */
static const repl_command_t *
command_named(const char *name, size_t length)
{
	size_t i;

	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (strlen(commands[i].name) == length &&
		    bcmp(commands[i].name, name, length) == 0)
			return (&commands[i]);
	}

	return (NULL);
}

/*
 * Run the command a line names, `dot` being where it begins.  The name runs
 * from the dot to the first blank, and what follows it, blanks skipped, is
 * the command's query or must be nothing.
 *
 * Once the command is known to fit the line, the dot and the name are
 * overwritten with blanks and the whole line becomes the command's argument.
 * A query given to .plan is then at the column it was typed at, so its
 * diagnostics point where they would for the same query typed alone.
 */
static int
run_command(repl_t *repl, char *line, char *dot)
{
	const repl_command_t *command;
	const char *name;
	const char *rest;
	size_t name_length;
	cmd_quoted_t quoted_name;

	name = dot + 1;
	name_length = strcspn(name, BLANKS);
	rest = name + name_length + strspn(name + name_length, BLANKS);

	command = command_named(name, name_length);
	if (command == NULL) {
		report_error(repl->invocation->program_name,
		    "unknown command .%s; .help lists the commands",
		    cmd_quote(name, name_length, &quoted_name));
		return (-1);
	}

	if (command->takes_query && *rest == '\0') {
		report_error(repl->invocation->program_name,
		    "usage: %s", command->synopsis);
		return (-1);
	}
	if (!command->takes_query && *rest != '\0') {
		report_error(repl->invocation->program_name,
		    ".%s takes no argument", command->name);
		return (-1);
	}

	memset(dot, ' ', 1 + name_length);

	return (command->run(repl, line));
}

/*
 * Answer one line.  A query is given the whole line rather than the line
 * with its leading blanks skipped, so that a diagnostic's column is the
 * column the reader typed it at.
 *
 * A line holding a NUL byte is refused, since a query is text and whatever
 * followed the NUL would be silently dropped.
 */
static int
answer_line(repl_t *repl, char *line, size_t length, size_t line_number)
{
	char *first;

	if (strlen(line) != length) {
		report_error(repl->invocation->program_name,
		    "line %zu of standard input holds a NUL byte",
		    line_number);
		return (-1);
	}

	first = line + strspn(line, BLANKS);
	if (*first == '\0' || *first == '%')
		return (0);

	if (*first == '.')
		return (run_command(repl, line, first));

	return (answer_query(repl, line));
}

/*
 * Read the next line and answer it.  A line the reader refused fails as a
 * line answered would, and the session goes on.  The end of input ends the
 * session, and so does a read that failed.
 */
static int
answer_next_line(repl_t *repl, line_reader_t *reader)
{
	char *line;
	size_t length;

	switch (line_reader_next(reader, &line, &length)) {
	case LINE_READ:
		return (answer_line(repl, line, length,
		    line_reader_number(reader)));
	case LINE_REFUSED:
		return (-1);
	case LINE_AT_END:
		repl->done = true;
		return (0);
	case LINE_FAILED:
		break;
	}

	repl->done = true;

	return (-1);
}

/*
 * Push everything answered so far to whoever reads it: the rows in the
 * output and, when they go elsewhere, the replies to commands on standard
 * output.  Returns -1 when a write has failed, reported.
 */
static int
flush_answers(const repl_t *repl)
{
	if (output_flush(repl->output) != 0)
		return (-1);

	if (output_is_stdout(repl->output))
		return (0);

	if (fflush(stdout) == 0 && !ferror(stdout))
		return (0);

	report_error(repl->invocation->program_name,
	    "cannot write standard output");

	return (-1);
}

/*
 * Everything answered is pushed out before each line is read, so that
 * whoever types the next line has seen the answer to the last, the answers
 * of the queries run before the session included; and once more when the
 * session ends.  A write that failed ends the session there, so its failure
 * is reported once.
 */
int
repl_run(invocation_t *invocation, output_t *output, size_t first_number)
{
	line_reader_t *reader;
	repl_t repl;
	int status;

	reader = line_reader_open(invocation->program_name);
	if (reader == NULL)
		return (-1);
	if (line_reader_prompts(reader))
		output_prompted(output);

	bzero(&repl, sizeof(repl));
	repl.invocation = invocation;
	repl.output = output;
	repl.next_number = first_number;

	status = 0;
	for (;;) {
		if (flush_answers(&repl) != 0) {
			status = -1;
			break;
		}
		if (repl.done)
			break;

		if (answer_next_line(&repl, reader) != 0)
			status = -1;
	}

	line_reader_close(reader);

	return (status);
}
