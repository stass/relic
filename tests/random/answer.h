/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_TEST_ANSWER_H
#define RELIC_TEST_ANSWER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <relic.h>

/*
 * A query's answer in a form two contexts can compare.
 *
 * A value's identifier means something only in the context that interned it,
 * so rows from two contexts cannot be compared as relic_value_t.  Each row is
 * rendered as a line of text in which no two distinct values look alike -- a
 * symbol as its spelling, a string in double quotes with every byte outside
 * printable ASCII, a quote and a backslash written as \xHH, an integer in
 * decimal -- and the lines are sorted bytewise.  Two answers are then equal
 * exactly when they hold the same set of rows, which is what a query means:
 * the order a strategy derives rows in is not part of its answer.
 *
 * The randomized differential suite and the evaluation fuzz target both
 * compare strategies this way.
 */
typedef struct answer answer_t;

struct answer {
	char		**rows;		/* sorted; each owned */
	size_t		  row_count;
};

/*
 * Collect a query into answer, which must be empty.  Returns false, leaving
 * the answer empty, when the query fails.  Failure to allocate the rendering
 * ends the process, since a harness that cannot hold an answer has nothing
 * to compare.
 */
bool	answer_collect(relic_t *ctx, relic_query_t *query, answer_t *answer);

bool	answer_equal(const answer_t *a, const answer_t *b);

/*
 * Write the rows only `expected` holds, marked `-`, and the rows only
 * `actual` holds, marked `+`.
 */
void	answer_report_difference(FILE *report, const answer_t *expected,
	    const answer_t *actual);

/* Release an answer's rows and leave it empty. */
void	answer_free(answer_t *answer);

#endif /* RELIC_TEST_ANSWER_H */
