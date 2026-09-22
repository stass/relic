/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_CMD_H
#define RELIC_CMD_H

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

/*
 * Where query results go, and in what form.
 *
 * The rows of one query are written as they are delivered, or held and
 * sorted first when `sorted` is set.  `queries_written` is what decides
 * whether a query's results need a separator before them; `rows_written`
 * and `begun` belong to the query being written.
 *
 * A write that fails is reported once, naming `name`, and `write_failed`
 * then keeps it from being reported again.  `hold_failed` records that a row
 * of the current query could not be held for sorting, which has been
 * reported and fails the query.
 */
struct output {
	relic_t			*ctx;
	FILE			*stream;
	const char		*name;
	const char		*program_name;
	relic_value_t		*held;		/* the values of held rows */
	size_t			 held_count;	/* rows */
	size_t			 held_capacity;	/* values */
	size_t			 width;		/* values per held row */
	size_t			 queries_written;
	size_t			 rows_written;
	enum output_format	 format;
	bool			 begun;
	bool			 hold_failed;
	bool			 sorted;
	bool			 write_failed;
};

/*
 * cmd/relic/main.c
 *
 * Report an error of the tool's own through the same formatter the library's
 * diagnostics use, so that a file name taken from the command line is escaped
 * like any other text a diagnostic quotes.
 */
void	report_error(const char *program_name, const char *format, ...)
	    CMD_PRINTFLIKE(2, 3);

/*
 * cmd/relic/output.c
 *
 * output_format_named() sets *formatp from a -F argument and returns 0, or
 * returns -1 for a name that is not a format.
 *
 * output_query() runs a query and writes its results, returning 0, or -1
 * when the query failed or a write did, either having been reported.
 *
 * output_close() flushes the output and closes it unless it is standard
 * output, returning -1 when a write failed at any point, reported once.
 */
int	output_format_named(const char *name, enum output_format *formatp);
void	output_init(output_t *out, const char *program_name, relic_t *ctx,
	    FILE *stream, const char *name, enum output_format format,
	    bool sorted);
int	output_query(output_t *out, relic_query_t *query);
int	output_close(output_t *out);

#endif /* RELIC_CMD_H */
