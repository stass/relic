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

/* Room for "<query N>" or "<export N>" with N any size_t, and a terminator. */
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
 * The formats rows can be written in.  Their exact form is part of the
 * tool's interface and is specified with the tool.  A query's results are
 * written in any but OUTPUT_DL, which writes each row as a fact of an
 * exported predicate and is written only by an export.
 */
enum output_format {
	OUTPUT_TEXT,
	OUTPUT_CSV,
	OUTPUT_TSV,
	OUTPUT_JSON,
	OUTPUT_DL
};

typedef struct output output_t;
typedef struct invocation invocation_t;
typedef struct prepared_export prepared_export_t;
typedef struct export_operand export_operand_t;

/*
 * Where query results go, and in what form.
 *
 * An output with no `buffer` discards every row, counting it, which is what
 * -n asks for.  output_attach() gives it a buffer and a stream to write to;
 * output_attach_file() gives it a buffer and `pending_path`, the file it
 * opens when the first bytes must reach it, after which `stream` is that
 * file and `pending_path` NULL.
 *
 * The rows of one query are written as they are delivered, or collected and
 * sorted first when `sorted` is set.  `queries_written` is what decides
 * whether a query's results need a separator before them, and `separated`
 * whether the format's separator is written at all; `rows_written` and
 * `begun` belong to the query being answered.
 *
 * `collected` is the collected answer whose rows are being written, whose
 * values are its own and are read through it, and NULL while rows are
 * written as they are delivered, when their values are the context's.
 *
 * What is written gathers in `buffer`, whose first `filled` bytes wait to be
 * handed to the stream a block at a time; every attached output owns one.
 * A write that fails is reported once, naming `name`, and `write_failed`
 * then keeps anything more from reaching the stream.
 *
 * `predicate` is the name each row is a fact of in OUTPUT_DL, borrowed, and
 * NULL in every other format.
 */
struct output {
	relic_t			*ctx;
	const relic_result_t	*collected;
	FILE			*stream;
	char			*buffer;
	const char		*name;
	const char		*pending_path;
	const char		*predicate;
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
 * own, NULL where one could not be prepared.  `exports` has one entry per
 * -O, in the order given.  `program_name` is what the tool's own messages
 * begin with.  `verbosity` is how many -v were given, and `explain_stream`
 * is where -e sends plans, standard error, or NULL without -e.  `output` is
 * where rows go while a query or an export runs, and NULL before and after;
 * a diagnostic hands its gathered rows to the stream before it is written.
 */
struct invocation {
	relic_t			 *ctx;
	relic_query_t		**queries;
	prepared_export_t	 *exports;
	output_t		 *output;
	const char		 *program_name;
	size_t			  query_count;
	size_t			  export_count;
	unsigned int		  verbosity;
	FILE			 *explain_stream;
};

/*
 * One -O, ready to write: its operand, borrowed, and the query of all of its
 * predicate's columns, owned, or NULL when it could not be prepared.
 */
struct prepared_export {
	const export_operand_t	*operand;
	relic_query_t		*query;
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
 * report_rows() writes the line -v adds after a query or an export that
 * succeeded, naming it by `what` it is, "query" or "export", and its
 * number, when the invocation is verbose enough to want it.
 *
 * explain_into() sends the plans of everything evaluated to `stream`, or
 * stops sending them when it is NULL.  write_stats() writes the context's
 * counters, one per line, as -X stats and .stats show them.
 */
void	report_error(const char *program_name, const char *format, ...)
	    CMD_PRINTFLIKE(2, 3);
const char *cmd_quote(const char *bytes, size_t length, cmd_quoted_t *quoted);
void	report_rows(const invocation_t *invocation, const char *what,
	    size_t number, size_t rows);
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
 * output_attach_file() gives it the file at `path` instead, named by its
 * path, which is created or truncated only when the first bytes reach it
 * or a query succeeds without writing any; a file that cannot be opened is
 * reported then, as a write that failed.  Its rows are facts of
 * `predicate` in OUTPUT_DL, which takes one and is written only to a file;
 * `predicate` is NULL in any other format.  The caller keeps `path` and
 * `predicate` for as long as the output.
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
 * what the output has gathered, a write failure reported as a row's is.
 * output_close() does what output_flush() does, releases the buffer, and
 * closes the stream unless it is standard output.
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
int	output_attach_file(output_t *out, const char *path,
	    enum output_format format, const char *predicate, bool sorted);
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

typedef struct column_types column_types_t;
typedef struct command_paths command_paths_t;
typedef struct import_operand import_operand_t;

/*
 * One -T operand, parsed: the predicate whose imports take the columns, and
 * its `column_count` columns.  `operand` is the argument as given and
 * points into argv; `predicate` and `columns` are allocated and belong to
 * the operand.
 */
struct column_types {
	const char		*operand;
	char			*predicate;
	enum relic_column	*columns;
	size_t			 column_count;
};

/*
 * One -I operand, parsed: the import it describes and the storage that
 * description points into.  `operand` is the argument as given and `path`
 * the table's path, the whole operand or the part after its first `=`; both
 * point into argv.  `predicate` is allocated and belongs to the operand.
 * `own_columns` is the column list the operand gives, allocated and the
 * operand's, or NULL when it gives none and takes them from its
 * predicate's -T, which `import.columns` then borrows from.
 */
struct import_operand {
	relic_import_t		 import;
	const char		*operand;
	const char		*path;
	char			*predicate;
	enum relic_column	*own_columns;
};

/*
 * One -O operand, parsed: the relation it writes and where.  `operand` is
 * the argument as given and `path` the file's path, the whole operand or the
 * part after its first `=`; both point into argv.  `predicate` is allocated
 * and belongs to the operand.  `arity` is the one its head names when
 * `has_arity`, and is otherwise the program's to say.
 */
struct export_operand {
	const char		*operand;
	const char		*path;
	char			*predicate;
	enum output_format	 format;
	unsigned int		 arity;
	bool			 has_arity;
};

/*
 * The paths a command line names besides its exports', which no export may
 * write: the -o file or NULL, the `import_count` imports' tables and the
 * `file_count` program operands, all borrowed.
 */
struct command_paths {
	const char		*output_path;
	const import_operand_t	*imports;
	char *const		*files;
	size_t			 import_count;
	size_t			 file_count;
};

/*
 * What parsing an -I, -T or -O argument came to: what it names, an argument
 * that is not one, or no memory to hold it.
 */
enum operand_parse {
	OPERAND_PARSED,
	OPERAND_MALFORMED,
	OPERAND_NO_MEMORY
};

/*
 * cmd/relic/operand.c
 *
 * import_operand_parse() parses a -I argument into *parsed,
 * column_types_parse() a -T argument and export_operand_parse() an -O
 * argument, or each reports why it could not, leaving nothing to free.
 * import_operand_free(), column_types_free() and export_operand_free()
 * release what a parse allocated.
 *
 * import_operands_resolve() completes every import, once the command line
 * is read, with the columns of its predicate's -T when it lists none, and
 * reports, as a usage error, an import with no columns, a predicate given
 * a second -T, and one given lists of columns whose kept types differ.
 * The imports borrow from the column types, which must outlive them.
 *
 * export_operands_resolve() reports, as a usage error, an export to a path
 * another export, -o, an import or a program operand spells alike.
 *
 * import_operand_reads_stdin() is whether the import's path is `-`, the
 * spelling that claims standard input.
 */
enum operand_parse import_operand_parse(const char *program_name,
	    const char *operand, import_operand_t *parsed);
enum operand_parse column_types_parse(const char *program_name,
	    const char *operand, column_types_t *parsed);
int	import_operands_resolve(const char *program_name,
	    import_operand_t *imports, size_t import_count,
	    const column_types_t *types, size_t type_count);
enum operand_parse export_operand_parse(const char *program_name,
	    const char *operand, export_operand_t *parsed);
int	export_operands_resolve(const char *program_name,
	    const export_operand_t *exports, size_t export_count,
	    const command_paths_t *others);
bool	import_operand_reads_stdin(const import_operand_t *parsed);
void	import_operand_free(import_operand_t *parsed);
void	column_types_free(column_types_t *parsed);
void	export_operand_free(export_operand_t *parsed);

/*
 * cmd/relic/export.c
 *
 * exports_prepare() settles the arity of each of `count` exports and
 * prepares the query of all its columns, into the invocation's `exports`,
 * reporting every export the program cannot answer before returning -1; 0
 * when every one was prepared.  The invocation borrows the operands, which
 * must outlive it.
 *
 * exports_write() writes each prepared export's relation to its file in
 * order, sorted when `sorted`, and returns 0, or -1 having reported the
 * first that failed, after which none is written.
 */
int	exports_prepare(invocation_t *invocation,
	    const export_operand_t *operands, size_t count);
int	exports_write(invocation_t *invocation, bool sorted);

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
