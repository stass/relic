/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <relic.h>

#include "../random/answer.h"
#include "../unit/harness.h"
#include "fuzz.h"

/*
 * The evaluation target: an input is a program, and every query it holds is
 * answered twice and the answers compared.
 *
 * The optimized context answers as a caller would by default -- semi-naive,
 * indexed, reordered, through the Magic rewrite -- and explains its plans to
 * a callback, so the explain renderer runs over whatever the fuzzer builds.
 * The reference context answers with the naive fixed point over the whole
 * program, the oracle every optimization is held to.  Where both answer a
 * query, the answers must be the same set of rows; a difference aborts,
 * which is how a fuzzer records a finding.
 *
 * Both contexts share limits on tuples and memory, so an input deriving a
 * large relation is refused rather than timing out.  The two strategies
 * derive different amounts, so a limit may refuse a query in one context
 * and not the other; only queries both answer are compared.  A query that
 * fails for any other reason is a finding.  Loading and
 * checking do not depend on the strategy, so the two contexts must accept
 * or refuse the program alike.
 */

#define	EVAL_MAX_TUPLES		20000
#define	EVAL_MAX_MEMORY		((size_t)64 * 1024 * 1024)

/*
 * The errors a context reported during one call: how many, and whether the
 * last was a limit's refusal.  Reset before each call, so that a call that
 * fails without reporting anything is seen as the finding it is rather than
 * excused by an error some earlier call left behind.
 */
typedef struct error_capture error_capture_t;

struct error_capture {
	size_t		count;
	bool		limit;
};

static void	 capture_error(void *capture_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static bool	 collect_both(relic_t *optimized,
		    error_capture_t *optimized_errors, relic_t *reference,
		    error_capture_t *reference_errors, size_t index,
		    answer_t *optimized_answer, answer_t *reference_answer);
static bool	 collect_unless_limited(relic_t *ctx, error_capture_t *errors,
		    size_t index, answer_t *answer, const char *strategy);
static void	 compare_queries(relic_t *optimized,
		    error_capture_t *optimized_errors, relic_t *reference,
		    error_capture_t *reference_errors);
static void	 discard_explain(void *callback_ctx, const char *line);
static relic_t	*open_context(const uint8_t *data, size_t size,
		    unsigned int flags, bool explain, error_capture_t *errors,
		    bool *accepted);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	error_capture_t optimized_errors, reference_errors;
	relic_t *optimized, *reference;
	bool optimized_accepted, reference_accepted;

	optimized = open_context(data, size, 0, true, &optimized_errors,
	    &optimized_accepted);
	reference = open_context(data, size, RELIC_F_NAIVE | RELIC_F_NO_MAGIC,
	    false, &reference_errors, &reference_accepted);

	if (optimized_accepted != reference_accepted) {
		fprintf(stderr, "the program was %s by default and %s under "
		    "naive+no-magic\n",
		    optimized_accepted ? "accepted" : "refused",
		    reference_accepted ? "accepted" : "refused");
		abort();
	}
	if (optimized_accepted)
		compare_queries(optimized, &optimized_errors, reference,
		    &reference_errors);

	relic_destroy(optimized);
	relic_destroy(reference);
	return (0);
}

/*
 * A context with the target's limits and the given strategy, holding the
 * input as its program, reporting its errors to *errors, and with explain,
 * rendering every plan it makes.  *accepted says whether the program loaded
 * and checked.
 */
static relic_t *
open_context(const uint8_t *data, size_t size, unsigned int flags,
    bool explain, error_capture_t *errors, bool *accepted)
{
	relic_config_t config;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	config.max_tuples = EVAL_MAX_TUPLES;
	config.max_memory = EVAL_MAX_MEMORY;
	config.flags = flags;

	ctx = relic_create_config(&config);
	if (ctx == NULL)
		abort();
	errors->count = 0;
	errors->limit = false;
	relic_diag_set(ctx, capture_error, errors);
	if (explain)
		relic_explain_set(ctx, discard_explain, NULL);

	*accepted = false;
	if (fuzz_load(ctx, data, size) != 0)
		return (ctx);
	if (relic_check(ctx) != 0)
		return (ctx);
	*accepted = true;
	return (ctx);
}

static void
compare_queries(relic_t *optimized, error_capture_t *optimized_errors,
    relic_t *reference, error_capture_t *reference_errors)
{
	size_t query_count;

	query_count = relic_query_count(optimized);
	if (query_count != relic_query_count(reference)) {
		fprintf(stderr, "the contexts hold %zu and %zu queries\n",
		    query_count, relic_query_count(reference));
		abort();
	}

	for (size_t q = 0; q < query_count; q++) {
		answer_t optimized_answer = { NULL, 0 };
		answer_t reference_answer = { NULL, 0 };

		if (!collect_both(optimized, optimized_errors, reference,
		    reference_errors, q, &optimized_answer, &reference_answer))
			continue;

		if (!answer_equal(&reference_answer, &optimized_answer)) {
			fprintf(stderr, "query %zu: the default answers %zu "
			    "rows, naive+no-magic %zu\n", q + 1,
			    optimized_answer.row_count,
			    reference_answer.row_count);
			answer_report_difference(stderr, &reference_answer,
			    &optimized_answer);
			abort();
		}

		answer_free(&optimized_answer);
		answer_free(&reference_answer);
	}
}

/*
 * Answer query `index` in both contexts.  Returns true when both answered,
 * and otherwise false with both answers left empty: a query a limit refused
 * in either context has nothing to compare.
 */
static bool
collect_both(relic_t *optimized, error_capture_t *optimized_errors,
    relic_t *reference, error_capture_t *reference_errors, size_t index,
    answer_t *optimized_answer, answer_t *reference_answer)
{
	if (!collect_unless_limited(optimized, optimized_errors, index,
	    optimized_answer, "default"))
		return (false);
	if (!collect_unless_limited(reference, reference_errors, index,
	    reference_answer, "naive+no-magic")) {
		answer_free(optimized_answer);
		return (false);
	}
	return (true);
}

/*
 * Answer query `index` in ctx.  Returns false when a limit refused it, and
 * aborts when it failed any other way, or without saying why: with nothing
 * but limits to stop it, a query over a program that checked must be
 * answered.
 */
static bool
collect_unless_limited(relic_t *ctx, error_capture_t *errors, size_t index,
    answer_t *answer, const char *strategy)
{
	relic_query_t *query;
	bool collected = false;

	errors->count = 0;
	errors->limit = false;
	query = relic_query_loaded(ctx, index);
	if (query != NULL) {
		collected = answer_collect(ctx, query, answer);
		relic_query_destroy(query);
	}
	if (collected)
		return (true);

	if (errors->count == 0) {
		fprintf(stderr, "query %zu failed under %s with no error\n",
		    index + 1, strategy);
		abort();
	}
	if (errors->limit)
		return (false);
	fprintf(stderr, "query %zu failed under %s: %s\n", index + 1, strategy,
	    relic_error(ctx));
	abort();
}

/*
 * Count an error and note whether it is a limit's refusal, which every
 * limit words the same way.  Warnings and notes are discarded.
 */
static void
capture_error(void *capture_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_TEST_UNUSED, const char *message)
{
	static const char limit_refusal[] = "limit exceeded:";
	error_capture_t *errors = capture_ctx;

	if (level != RELIC_ERROR)
		return;
	errors->count++;
	errors->limit = strncmp(message, limit_refusal,
	    sizeof(limit_refusal) - 1) == 0;
}

static void
discard_explain(void *callback_ctx RELIC_TEST_UNUSED,
    const char *line RELIC_TEST_UNUSED)
{
}
