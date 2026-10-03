/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_CMD_H
#define RELIC_CMD_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <relic.h>

/*
 * Declarations shared by the modules of the relic tool.  The tool is a client
 * of librelic and includes nothing from lib/: everything it knows about the
 * engine comes through <relic.h>.
 */

/*
 * Check a printf-style call against its format string.
 */
#if defined(__GNUC__)
#define	CMD_PRINTFLIKE(fmt, args) \
	__attribute__((__format__(__printf__, fmt, args)))
#else
#define	CMD_PRINTFLIKE(fmt, args)
#endif

/*
 * Mark a parameter a function must accept and does not read.
 */
#if defined(__GNUC__)
#define	RELIC_CMD_UNUSED	__attribute__((__unused__))
#else
#define	RELIC_CMD_UNUSED
#endif

/* Room for "<query N>" with N any size_t, and a terminator. */
#define	CMD_QUERY_NAME_MAX	32

/*
 * The most bytes of an argument a message quotes, and room for that many,
 * the "..." that marks a cut, and a terminator.
 */
#define	CMD_QUOTED_MAX		64
#define	CMD_QUOTED_TEXT_MAX	(CMD_QUOTED_MAX + sizeof("..."))

typedef struct cmd_quoted cmd_quoted_t;

/*
 * An argument quoted in a message, cut short.  The room is part of the type,
 * so no caller can hand cmd_quote() less.
 */
struct cmd_quoted {
	char	text[CMD_QUOTED_TEXT_MAX];
};

/* A macro's value as a string literal. */
#define	CMD_STRINGIFY(value)	CMD_STRINGIFY_TEXT(value)
#define	CMD_STRINGIFY_TEXT(value)	#value

/*
 * The formats a query's results can be written in.  Their exact form is part
 * of the tool's interface and is specified with the tool.
 */
enum output_format {
	OUTPUT_TEXT,
	OUTPUT_CSV,
	OUTPUT_TSV,
	OUTPUT_JSON
};

typedef struct output output_t;
typedef struct invocation invocation_t;

/*
 * Where query results go, and in what form.
 *
 * An output with no `stream` discards every row, counting it, which is what
 * -n asks for; output_attach() gives it one to write to.  The rows of one
 * query are written as they are delivered, or collected and sorted first
 * when `sorted` is set.  `queries_written` is what decides whether a query's
 * results need a separator before them, and `separated` whether the format's
 * separator is written at all; `rows_written` and `begun` belong to the query
 * being answered.
 *
 * What is written gathers in `buffer`, whose first `filled` bytes wait to be
 * handed to the stream a block at a time; an output with a stream owns one.
 * A write that fails is reported once, naming `name`, and `write_failed`
 * then keeps anything more from reaching the stream.
 */
struct output {
	relic_t			*ctx;
	FILE			*stream;
	char			*buffer;
	const char		*name;
	const char		*program_name;
	size_t			 filled;
	size_t			 queries_written;
	size_t			 rows_written;
	enum output_format	 format;
	bool			 begun;
	bool			 separated;
	bool			 sorted;
	bool			 write_failed;
};

/*
 * One invocation of the tool: its context and the queries prepared against
 * it, released together.  `queries` has one entry per query the invocation
 * runs before any interactive session, the -q queries or else the program's
 * own, NULL where one could not be prepared.  `program_name` is what the
 * tool's own messages begin with.  `verbosity` is how many -v were given,
 * and `explain_stream` is where -e sends plans, standard error, or NULL
 * without -e.  `output` is where query results go while queries run, and
 * NULL before and after; a diagnostic hands its gathered rows to the stream
 * before it is written.
 */
struct invocation {
	relic_t		 *ctx;
	relic_query_t	**queries;
	output_t	 *output;
	const char	 *program_name;
	size_t		  query_count;
	unsigned int	  verbosity;
	FILE		 *explain_stream;
};

/*
 * cmd/relic/main.c
 *
 * report_error() reports an error of the tool's own through the same
 * formatter the library's diagnostics use, so that a file name taken from
 * the command line is escaped like any other text a diagnostic quotes.
 *
 * cmd_quote() copies `length` bytes into `quoted`, cut past CMD_QUOTED_MAX
 * with "..." after them, and returns its text.  A message of the tool's own
 * is formatted into a fixed buffer, and an argument quoted whole could crowd
 * out what the message says about it.
 *
 * report_rows() writes the line -v adds after a query that succeeded, naming
 * it by its number, when the invocation is verbose enough to want it.
 *
 * explain_into() sends the plans of everything evaluated to `stream`, or
 * stops sending them when it is NULL.  write_stats() writes the context's
 * counters, one per line, as -X stats and .stats show them.
 */
void	report_error(const char *program_name, const char *format, ...)
	    CMD_PRINTFLIKE(2, 3);
const char *cmd_quote(const char *bytes, size_t length, cmd_quoted_t *quoted);
void	report_rows(const invocation_t *invocation, size_t number, size_t rows);
void	explain_into(relic_t *ctx, FILE *stream);
void	write_stats(relic_t *ctx, FILE *stream);

/*
 * cmd/relic/output.c
 *
 * output_format_named() sets *formatp from a -F argument and returns 0, or
 * returns -1 for a name that is not a format.
 *
 * output_init() prepares an output that discards rows, and output_attach()
 * gives it a stream, named `name` in messages, to write them to instead,
 * returning 0, or -1 having reported that memory ran out.
 *
 * output_query() runs a query and writes its results, handing them to the
 * stream before it returns, and output_discard() runs one and writes nothing
 * whatever the output is.  Each sets *rowsp to the number of rows and
 * returns 0, or returns -1 when the query failed or a write did, either
 * having been reported.  A query interrupted after its first row leaves the
 * rows written before the interrupt, ending at a row boundary.
 *
 * output_flush() flushes the stream and returns -1 when a write to it failed
 * at any point, the output's own or another writer's through the same
 * stream, reported once.  output_write_buffered() only hands the stream
 * what the output has gathered, a write failure reported as a row's is.  output_close() does the same, releases the buffer,
 * and closes the stream unless it is standard output.
 * output_is_stdout() is whether the stream is standard output, which other
 * writers to standard output share.
 *
 * output_prompted() tells the output that a prompt is written before every
 * query, and leaves out the blank line between queries' results when the
 * stream is a terminal, where the prompt already marks where each begins.
 */
int	output_format_named(const char *name, enum output_format *formatp);
void	output_init(output_t *out, const char *program_name, relic_t *ctx);
int	output_attach(output_t *out, FILE *stream, const char *name,
	    enum output_format format, bool sorted);
int	output_query(output_t *out, relic_query_t *query, size_t *rowsp);
int	output_discard(output_t *out, relic_query_t *query, size_t *rowsp);
int	output_flush(output_t *out);
void	output_write_buffered(output_t *out);
bool	output_is_stdout(const output_t *out);
void	output_prompted(output_t *out);
int	output_close(output_t *out);

/*
 * cmd/relic/repl.c
 *
 * Run the interactive session of -i over standard input, answering into
 * `output`.  `first_number` is the number the session's first query is
 * given.  Returns 0 when everything in the session succeeded, and -1 when
 * anything failed, each failure having been reported.
 */
int	repl_run(invocation_t *invocation, output_t *output,
	    size_t first_number);

/*
 * Set by SIGINT while a session that prompts answers a line, and cleared as
 * each line begins.  The same signal interrupts the library's evaluation;
 * this is for what a line does outside one -- preparing its query, writing
 * sorted rows -- which stops on finding it set.  It is zero in a run with
 * no such session.
 */
extern volatile sig_atomic_t repl_interrupted;

typedef struct import_operand import_operand_t;

/*
 * One -I operand, parsed: the import it describes and the storage that
 * description points into.  `operand` is the argument as given and `path`
 * the part after its first `=`; both point into argv.  `predicate` and
 * `columns` are allocated and belong to the operand.
 */
struct import_operand {
	relic_import_t		 import;
	const char		*operand;
	const char		*path;
	char			*predicate;
	enum relic_column	*columns;
};

/*
 * What parsing a -I argument came to: an import, an argument that is not
 * one, or no memory to hold it.
 */
enum import_parse {
	IMPORT_PARSED,
	IMPORT_MALFORMED,
	IMPORT_NO_MEMORY
};

/*
 * cmd/relic/import.c
 *
 * import_operand_parse() parses a -I argument into *parsed, or reports why
 * it could not, leaving nothing to free.  import_operand_free() releases
 * what a parse allocated.
 *
 * import_operand_reads_stdin() is whether the import's path is `-`, the
 * spelling that claims standard input.
 */
enum import_parse import_operand_parse(const char *program_name,
	    const char *operand, import_operand_t *parsed);
bool	import_operand_reads_stdin(const import_operand_t *parsed);
void	import_operand_free(import_operand_t *parsed);

typedef struct line_reader line_reader_t;

/*
 * What reading the next line came to: a line; a line refused, after which
 * the one following it can still be read; the end of input; or a read that
 * failed, after which nothing more can be.
 */
enum line_read {
	LINE_READ,
	LINE_REFUSED,
	LINE_AT_END,
	LINE_FAILED
};

/*
 * cmd/relic/input.c
 *
 * Standard input a line at a time, edited with libedit and prompted for when
 * it is a terminal and the build has libedit, and read plainly otherwise.
 * The structure is private to input.c; line_reader_open() allocates it.
 *
 * line_reader_next() returns LINE_READ with the next line, without its
 * newline, at *linep and its length at *lengthp.  The line belongs to the
 * reader and is valid until the next call, and the caller may change its
 * bytes in place.  A refused line and a failed read are reported before
 * line_reader_next() returns.  line_reader_number() is the number of the
 * line last read or refused, counting from one.  line_reader_prompts() is
 * whether a prompt is written before each line is read, which is when
 * someone is typing.
 */
line_reader_t	*line_reader_open(const char *program_name);
enum line_read	 line_reader_next(line_reader_t *reader, char **linep,
		    size_t *lengthp);
size_t		 line_reader_number(const line_reader_t *reader);
bool		 line_reader_prompts(const line_reader_t *reader);
void		 line_reader_close(line_reader_t *reader);

#endif /* RELIC_CMD_H */
