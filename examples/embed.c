/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Embedding relic: the smallest program that uses what an embedder needs.
 *
 * It creates a context with a tuple limit, sends diagnostics to standard
 * error, loads rules as text, adds facts through the interface, streams one
 * query and stops it early, and collects another.  It prints what it found
 * and exits 0 when that is what the program derives, so it doubles as a test
 * of the installed library: `make installcheck` builds it against the
 * installed header and library, found through pkg-config.
 *
 *	cc embed.c $(pkg-config --cflags --libs librelic)
 */

#include <stdio.h>
#include <string.h>

#include <relic.h>

/*
 * The name relic_diag_stderr() prefixes each diagnostic with.  It is passed
 * as the callback's context, a void *, so it is an array rather than a
 * string literal, which is read-only.
 */
static char program_name[] = "embed";

static const char rules[] =
    "reaches(X, Y) :- flight(X, Y).\n"
    "reaches(X, Z) :- flight(X, Y), reaches(Y, Z).\n";

/* The flights added as facts: from, to. */
static const char *const flights[][2] = {
	{ "lisbon", "paris" },
	{ "paris", "berlin" },
	{ "berlin", "warsaw" },
};

static int	 add_flights(relic_t *ctx);
static int	 first_destination(void *callback_ctx,
		    const relic_value_t *values, size_t count);
static int	 print_destinations(relic_t *ctx);

/*
 * Add each flight as the fact flight(from, to).
 */
static int
add_flights(relic_t *ctx)
{
	relic_value_t route[2];
	size_t i;
	size_t end;

	for (i = 0; i < sizeof(flights) / sizeof(flights[0]); i++) {
		for (end = 0; end < 2; end++) {
			if (relic_value_symbol(ctx, flights[i][end],
			    strlen(flights[i][end]), &route[end]) != 0)
				return (-1);
		}
		if (relic_fact_add(ctx, "flight", 2, route) != 0)
			return (-1);
	}

	return (0);
}

/*
 * A row callback that keeps the first row's value and stops the stream.
 */
static int
first_destination(void *callback_ctx, const relic_value_t *values,
    size_t count)
{
	relic_value_t *first;

	first = callback_ctx;
	if (count == 1)
		*first = values[0];

	return (1);
}

/*
 * Collect every city reachable from lisbon and print them, one per line.
 * Returns how many there were, or -1 when the query failed.
 */
static int
print_destinations(relic_t *ctx)
{
	relic_query_t *query;
	relic_result_t *result;
	const relic_value_t *row;
	size_t i;
	int rows;

	query = relic_query_prepare(ctx, "destinations", "reaches(lisbon, X)?");
	if (query == NULL)
		return (-1);

	result = relic_query_collect(ctx, query);
	relic_query_destroy(query);
	if (result == NULL)
		return (-1);

	for (i = 0; i < relic_result_rows(result); i++) {
		row = relic_result_row(result, i);
		printf("lisbon reaches %s\n",
		    relic_string(ctx, row[0].v.id, NULL));
	}
	rows = (int)relic_result_rows(result);
	relic_result_destroy(result);

	return (rows);
}

int
main(void)
{
	relic_config_t config;
	relic_query_t *query;
	relic_value_t first;
	relic_t *ctx;
	int reached;

	memset(&config, 0, sizeof(config));
	config.max_tuples = 1000;
	first.type = RELIC_INTEGER;

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		fprintf(stderr, "embed: cannot create a context\n");
		return (1);
	}
	relic_diag_set(ctx, relic_diag_stderr, program_name);

	if (relic_load_string(ctx, "rules", rules) != 0 ||
	    add_flights(ctx) != 0) {
		relic_destroy(ctx);
		return (1);
	}

	/* Stream a query, keeping only its first row. */
	query = relic_query_prepare(ctx, "direct", "flight(paris, X)?");
	if (query == NULL ||
	    relic_query_run(ctx, query, first_destination, &first) != 0) {
		relic_query_destroy(query);
		relic_destroy(ctx);
		return (1);
	}
	relic_query_destroy(query);
	if (first.type != RELIC_SYMBOL) {
		relic_destroy(ctx);
		return (1);
	}
	printf("paris flies to %s\n", relic_string(ctx, first.v.id, NULL));

	reached = print_destinations(ctx);
	relic_destroy(ctx);

	return (reached == 3 ? 0 : 1);
}
