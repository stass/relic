/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/types.h>

#include <relic.h>

#include "../unit/harness.h"
#include "fuzz.h"

/*
 * The import target: an input is a table, and the byte before it says how
 * to read it.
 *
 * The first byte chooses a format, whether the table has a header, and one
 * of a few column lists, each chosen to reach a different part of the
 * importer: every field a string, which any bytes are; a symbol and an
 * integer, which refuse most; a skipped column between two kept ones; a
 * single column, whose records an empty line satisfies; and the widest
 * import a predicate allows.  The rest of the input is the table, empty
 * included.
 *
 * The table is handed over as a stream whose read function is the target's
 * own, as an embedder's can be, where the platform can make one.  Beyond
 * the sanitizers' checks, the target holds the import to one promise: an
 * import that fails says why, so a failure with no error reported aborts.
 */

#define	IMPORT_WIDEST		32

typedef struct column_list column_list_t;
typedef struct error_count error_count_t;
typedef struct table_source table_source_t;

/*
 * One column list the first byte can choose.
 */
struct column_list {
	const enum relic_column	*columns;
	size_t			 count;
};

/* The context an import runs in, and how many errors it reported. */
struct error_count {
	relic_t		*ctx;
	size_t		 errors;
};

/*
 * The table a stream serves, and how much of it has been served.  `copy`
 * is a copy of the bytes the stream reads instead, where it needs one, freed
 * once the stream is closed.
 */
struct table_source {
	const uint8_t	*bytes;
	char		*copy;
	size_t		 length;
	size_t		 offset;
};

static void	 count_error(void *callback_ctx, enum relic_diag_level level,
		    const relic_location_t *where, const char *message);
static FILE	*open_table(table_source_t *source);
static ssize_t	 serve_table(table_source_t *source, char *buffer,
		    size_t size);

static const enum relic_column all_strings[] = {
	RELIC_COLUMN_STRING, RELIC_COLUMN_STRING, RELIC_COLUMN_STRING
};
static const enum relic_column symbol_and_integer[] = {
	RELIC_COLUMN_SYMBOL, RELIC_COLUMN_INTEGER
};
static const enum relic_column skip_between[] = {
	RELIC_COLUMN_STRING, RELIC_COLUMN_SKIP, RELIC_COLUMN_STRING
};
static const enum relic_column one_column[] = {
	RELIC_COLUMN_SYMBOL
};
static const enum relic_column widest[IMPORT_WIDEST] = {
	RELIC_COLUMN_STRING, RELIC_COLUMN_STRING, RELIC_COLUMN_STRING,
	RELIC_COLUMN_STRING, RELIC_COLUMN_STRING, RELIC_COLUMN_STRING,
	RELIC_COLUMN_STRING, RELIC_COLUMN_STRING, RELIC_COLUMN_STRING,
	RELIC_COLUMN_STRING, RELIC_COLUMN_STRING, RELIC_COLUMN_STRING,
	RELIC_COLUMN_STRING, RELIC_COLUMN_STRING, RELIC_COLUMN_STRING,
	RELIC_COLUMN_STRING, RELIC_COLUMN_STRING, RELIC_COLUMN_STRING,
	RELIC_COLUMN_STRING, RELIC_COLUMN_STRING, RELIC_COLUMN_STRING,
	RELIC_COLUMN_STRING, RELIC_COLUMN_STRING, RELIC_COLUMN_STRING,
	RELIC_COLUMN_STRING, RELIC_COLUMN_STRING, RELIC_COLUMN_STRING,
	RELIC_COLUMN_STRING, RELIC_COLUMN_STRING, RELIC_COLUMN_STRING,
	RELIC_COLUMN_STRING, RELIC_COLUMN_STRING
};

static const column_list_t column_lists[] = {
	{ all_strings, sizeof(all_strings) / sizeof(all_strings[0]) },
	{ symbol_and_integer,
	    sizeof(symbol_and_integer) / sizeof(symbol_and_integer[0]) },
	{ skip_between, sizeof(skip_between) / sizeof(skip_between[0]) },
	{ one_column, sizeof(one_column) / sizeof(one_column[0]) },
	{ widest, sizeof(widest) / sizeof(widest[0]) }
};

#define	COLUMN_LIST_COUNT \
	(sizeof(column_lists) / sizeof(column_lists[0]))

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const column_list_t *list;
	table_source_t source;
	relic_import_t import;
	error_count_t count;
	relic_t *ctx;
	FILE *stream;
	unsigned int choice;
	int status;

	if (size == 0)
		return (0);

	/*
	 * The choice's lowest bit is the format, the next the header, and
	 * the rest the column list.
	 */
	choice = data[0];
	list = &column_lists[(choice >> 2) % COLUMN_LIST_COUNT];

	bzero(&import, sizeof(import));
	import.predicate = "t";
	import.columns = list->columns;
	import.column_count = list->count;
	if ((choice & 1u) != 0)
		import.format = RELIC_FORMAT_TSV;
	else
		import.format = RELIC_FORMAT_CSV;
	if ((choice & 2u) != 0)
		import.flags = RELIC_IMPORT_HEADER;

	bzero(&source, sizeof(source));
	source.bytes = data + 1;
	source.length = size - 1;
	stream = open_table(&source);
	if (stream == NULL)
		abort();

	ctx = relic_create();
	if (ctx == NULL)
		abort();
	count.ctx = ctx;
	count.errors = 0;
	relic_diag_set(ctx, count_error, &count);

	status = relic_import_file(ctx, "fuzz.csv", stream, &import);
	if (status != 0 && count.errors == 0)
		abort();

	relic_destroy(ctx);
	fclose(stream);
	free(source.copy);

	return (0);
}

/* Count each error a context reports, and abort on an internal one. */
static void
count_error(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_TEST_UNUSED,
    const char *message RELIC_TEST_UNUSED)
{
	error_count_t *count;

	count = callback_ctx;
	fuzz_refuse_internal(count->ctx, level);
	if (level == RELIC_ERROR)
		count->errors++;
}

/* Serve the next part of the table, at most `size` bytes of it. */
static ssize_t
serve_table(table_source_t *source, char *buffer, size_t size)
{
	size_t remaining;

	remaining = source->length - source->offset;
	if (size > remaining)
		size = remaining;
	bcopy(source->bytes + source->offset, buffer, size);
	source->offset += size;

	return ((ssize_t)size);
}

#if defined(HAVE_FUNOPEN)
static int
funopen_read(void *cookie, char *buffer, int size)
{
	return ((int)serve_table(cookie, buffer, (size_t)size));
}

/* A stream reading the table through serve_table(). */
static FILE *
open_table(table_source_t *source)
{
	return (funopen(source, funopen_read, NULL, NULL, NULL));
}
#elif defined(HAVE_FOPENCOOKIE)
static ssize_t
cookie_read(void *cookie, char *buffer, size_t size)
{
	return (serve_table(cookie, buffer, size));
}

/* A stream reading the table through serve_table(). */
static FILE *
open_table(table_source_t *source)
{
	cookie_io_functions_t functions = { cookie_read, NULL, NULL, NULL };

	return (fopencookie(source, "r", functions));
}
#else
/*
 * No way to give a stream a read function of the target's own here, so the
 * table is read from a stream over a copy of its bytes instead.
 */
static FILE *
open_table(table_source_t *source)
{
	if (source->length == 0)
		return (fopen("/dev/null", "r"));

	source->copy = malloc(source->length);
	if (source->copy == NULL)
		return (NULL);
	bcopy(source->bytes, source->copy, source->length);

	return (fmemopen(source->copy, source->length, "r"));
}
#endif
