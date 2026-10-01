/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>

#include <relic.h>

static int
print_row(void *ctx, const relic_value_t *values, size_t count)
{
	if (count > 0 && values[0].type == RELIC_SYMBOL)
		puts(relic_string(ctx, values[0].v.id, NULL));

	return (0);
}

int
main(void)
{
	static char name[] = "query";
	relic_query_t *query;
	relic_t *ctx;
	int status;

	ctx = relic_create();
	if (ctx == NULL)
		return (EXIT_FAILURE);
	relic_diag_set(ctx, relic_diag_stderr, name);

	query = NULL;
	status = relic_load_string(ctx, "graph",
	    "edge(a, b). edge(b, c).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n");
	if (status == 0)
		query = relic_query_prepare(ctx, NULL, "path(a, X)?");
	if (query == NULL || relic_query_run(ctx, query, print_row, ctx) != 0)
		status = -1;

	relic_query_destroy(query);
	relic_destroy(ctx);

	if (fflush(stdout) == EOF || ferror(stdout))
		status = -1;

	return (status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
