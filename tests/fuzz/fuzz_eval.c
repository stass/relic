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
 * answered three times and the answers compared.
 *
 * The optimized context answers as a caller would by default -- semi-naive,
 * indexed, reordered, through the Magic rewrite -- and explains its plans to
 * a callback, so the explain renderer runs over whatever the fuzzer builds.
 * The deferred context answers the same way with every round's insertions
 * deferred to its end, as a round run on several threads makes them.  The
 * reference context answers with the naive fixed point over the whole
 * program, the oracle every optimization is held to.  Where two answer a
 * query, the answers must be the same set of rows; a difference aborts,
 * which is how a fuzzer records a finding.
 *
 * All three contexts share limits on tuples and memory, so an input deriving
 * a large relation is refused rather than timing out.  The reference derives
 * different amounts, so a limit may refuse a query in the reference and not
 * in the optimized context, or the other way round; and it evaluates
 * expressions over other bindings, so an evaluation error -- an arithmetic
 * result that does not fit, a zero divisor, a string too long or a pattern
 * that is not one -- may stop one and not the other.  Only queries both
 * answer are compared.  Deferral changes nothing but the cost, so the
 * deferred context must answer exactly the queries the optimized one does,
 * and a limit or an evaluation error must stop the rest in both.  A query
 * that fails for any other reason is a finding.  Loading and checking do
 * not depend on the strategy, so the contexts must accept or refuse the
 * program alike.
 */

#define	EVAL_MAX_TUPLES		20000
#define	EVAL_MAX_MEMORY		((size_t)64 * 1024 * 1024)

/*
 * The errors a context reported during one call: how many, and whether the
 * last was one that may stop a query without being a finding: a limit's
 * refusal or an evaluation error.  Reset before each call, so
 * that a call that fails without reporting anything is seen as the finding
 * it is rather than excused by an error some earlier call left behind.
 */
typedef struct error_capture error_capture_t;

struct error_capture {
	size_t		count;
	bool		stopped;
};

static void	 capture_error(void *capture_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static bool	 collect_unless_stopped(relic_t *ctx, error_capture_t *errors,
		    size_t index, answer_t *answer, const char *strategy);
static void	 compare_answers(size_t index, const char *strategy,
		    const answer_t *answer, const char *oracle,
		    const answer_t *expected);
static void	 compare_queries(relic_t *optimized,
		    error_capture_t *optimized_errors, relic_t *deferred,
		    error_capture_t *deferred_errors, relic_t *reference,
		    error_capture_t *reference_errors);
static void	 discard_explain(void *callback_ctx, const char *line);
static relic_t	*open_context(const uint8_t *data, size_t size,
		    unsigned int flags, bool explain, error_capture_t *errors,
		    bool *accepted);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	error_capture_t optimized_errors, deferred_errors, reference_errors;
	relic_t *optimized, *deferred, *reference;
	bool optimized_accepted, deferred_accepted, reference_accepted;

	optimized = open_context(data, size, 0, true, &optimized_errors,
	    &optimized_accepted);
	deferred = open_context(data, size, RELIC_F_PARALLEL_ALWAYS, false,
	    &deferred_errors, &deferred_accepted);
	reference = open_context(data, size, RELIC_F_NAIVE | RELIC_F_NO_MAGIC,
	    false, &reference_errors, &reference_accepted);

	if (optimized_accepted != deferred_accepted ||
	    optimized_accepted != reference_accepted) {
		fprintf(stderr, "the program was %s by default, %s under "
		    "parallel and %s under naive+no-magic\n",
		    optimized_accepted ? "accepted" : "refused",
		    deferred_accepted ? "accepted" : "refused",
		    reference_accepted ? "accepted" : "refused");
		abort();
	}
	if (optimized_accepted)
		compare_queries(optimized, &optimized_errors, deferred,
		    &deferred_errors, reference, &reference_errors);

	relic_destroy(optimized);
	relic_destroy(deferred);
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
	errors->stopped = false;
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

/*
 * Answer every query in the three contexts and compare what they answer.
 */
static void
compare_queries(relic_t *optimized, error_capture_t *optimized_errors,
    relic_t *deferred, error_capture_t *deferred_errors, relic_t *reference,
    error_capture_t *reference_errors)
{
	size_t query_count;

	query_count = relic_query_count(optimized);
	if (query_count != relic_query_count(deferred) ||
	    query_count != relic_query_count(reference)) {
		fprintf(stderr, "the contexts hold %zu, %zu and %zu queries\n",
		    query_count, relic_query_count(deferred),
		    relic_query_count(reference));
		abort();
	}

	for (size_t q = 0; q < query_count; q++) {
		answer_t optimized_answer = { NULL, 0 };
		answer_t deferred_answer = { NULL, 0 };
		answer_t reference_answer = { NULL, 0 };
		bool optimized_answered, deferred_answered;

		optimized_answered = collect_unless_stopped(optimized,
		    optimized_errors, q, &optimized_answer, "default");
		deferred_answered = collect_unless_stopped(deferred,
		    deferred_errors, q, &deferred_answer, "parallel");
		if (optimized_answered != deferred_answered) {
			fprintf(stderr, "query %zu: it was stopped %s\n",
			    q + 1, optimized_answered ? "under parallel only" :
			    "by default only");
			abort();
		}

		if (optimized_answered) {
			compare_answers(q, "parallel", &deferred_answer,
			    "the default", &optimized_answer);
			if (collect_unless_stopped(reference, reference_errors,
			    q, &reference_answer, "naive+no-magic"))
				compare_answers(q, "the default",
				    &optimized_answer, "naive+no-magic",
				    &reference_answer);
		}

		answer_free(&optimized_answer);
		answer_free(&deferred_answer);
		answer_free(&reference_answer);
	}
}

/*
 * Abort, reporting the difference, unless query `index` answered under
 * `strategy` what it answered under `oracle`.
 */
static void
compare_answers(size_t index, const char *strategy, const answer_t *answer,
    const char *oracle, const answer_t *expected)
{
	if (answer_equal(expected, answer))
		return;

	fprintf(stderr, "query %zu: %s answers %zu rows, %s %zu\n", index + 1,
	    strategy, answer->row_count, oracle, expected->row_count);
	answer_report_difference(stderr, expected, answer);
	abort();
}

/*
 * Answer query `index` in ctx.  Returns false when a limit refused it or
 * it met an evaluation error, and aborts when it failed any other
 * way, or without saying why: with nothing but those to stop it, a query
 * over a program that checked must be answered.
 */
static bool
collect_unless_stopped(relic_t *ctx, error_capture_t *errors, size_t index,
    answer_t *answer, const char *strategy)
{
	relic_query_t *query;
	bool collected = false;

	errors->count = 0;
	errors->stopped = false;
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
	if (errors->stopped)
		return (false);
	fprintf(stderr, "query %zu failed under %s: %s\n", index + 1, strategy,
	    relic_error(ctx));
	abort();
}

/*
 * Count an error and note whether it may stop a query: a limit's refusal,
 * which every limit words the same way, or an evaluation error -- an
 * undefined arithmetic result, a string too long to be a value, or a
 * pattern arriving in a variable that is not a regular expression -- which
 * names its failure first.  A constant pattern that is not one is refused
 * at load, alike in every context, and is worded without the " in ".
 * Warnings and notes are discarded.
 */
static void
capture_error(void *capture_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_TEST_UNUSED, const char *message)
{
	static const char *const stops[] = {
		"limit exceeded:",
		"integer overflow in ",
		"division by zero in ",
		"string too long in ",
		"invalid regular expression in "
	};
	error_capture_t *errors = capture_ctx;

	if (level != RELIC_ERROR)
		return;
	errors->count++;
	errors->stopped = false;
	for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
		if (strncmp(message, stops[i], strlen(stops[i])) == 0)
			errors->stopped = true;
	}
}

static void
discard_explain(void *callback_ctx RELIC_TEST_UNUSED,
    const char *line RELIC_TEST_UNUSED)
{
}
