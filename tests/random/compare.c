/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <err.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <relic.h>

#include "answer.h"
#include "random.h"

/*
 * Running a program under every evaluation strategy and comparing them.
 *
 * Each strategy loads the program's text into a context of its own, created
 * with the strategy's flags, and collects every query the program holds.
 * Everything goes through <relic.h>: the equivalences are reachable through
 * the public configuration, so the suite needs no private entry point and
 * tests what an embedder gets.
 *
 * Two things are compared.  A strategy's answer to every query must equal
 * the default's.  And where a strategy evaluates the same rules as a
 * reference -- the naive fixed point beside the semi-naive one, say -- it
 * must hold as many tuples after each query, which catches a relation that
 * is wrong but that no query reads.  Equal totals stand for equal relations
 * because the naive fixed point is complete, so its total is the size of the
 * least model of its rules, and every strategy inserts only tuples those
 * rules justify.  It must hold as many string values too: equal relations
 * hold the same computed strings, and a string evaluation computes is
 * interned only when a tuple holding it is stored.  A strategy whose rules
 * differ from every other's, because its planner's order decides what the
 * Magic rewrite generates, has no reference.
 *
 * And the semi-naive fixed point must make every derivation exactly once: to
 * materialize a program whose rules hold no `_`, one per fact and, for each
 * other rule, one per distinct binding of its body's variables over the
 * fixed point.  An aggregate's braces are compiled to a rule of their own,
 * the group rule, which derives once per distinct binding of the group
 * body's variables.  The bindings are counted in a context of their own
 * under the naive fixed point, as the rows of one query per rule body and
 * one per group body.  No answer shows a derivation made twice, since a
 * duplicate changes none, so this is the only comparison that can.
 */

typedef struct strategy strategy_t;

struct strategy {
	const char	*name;
	unsigned int	 flags;
	/* Evaluate the whole program before the queries run over it. */
	bool		 materialize;
	/* The strategy whose tuple totals this one must match. */
	random_strategy_t total_reference;
	/* The threads its rounds are shared among; zero is one. */
	size_t		 threads;
};

/*
 * The threads the deferred strategies share every round among, so that
 * the merge of several workers' candidates is held to the serial answers.
 */
#define	PARALLEL_THREADS	4

static const strategy_t strategies[STRATEGY_COUNT] = {
	[STRATEGY_DEFAULT] = {
		"default", 0, false, STRATEGY_NONE, 0 },
	[STRATEGY_NAIVE] = {
		"naive", RELIC_F_NAIVE, false, STRATEGY_DEFAULT, 0 },
	[STRATEGY_NO_INDEX] = {
		"no-index", RELIC_F_NO_INDEX, false, STRATEGY_DEFAULT, 0 },
	[STRATEGY_NO_STREAM] = {
		"no-stream", RELIC_F_NO_STREAM, false, STRATEGY_DEFAULT, 0 },
	[STRATEGY_PARALLEL] = {
		"parallel", RELIC_F_PARALLEL_ALWAYS, false, STRATEGY_DEFAULT,
		PARALLEL_THREADS },
	[STRATEGY_NO_REORDER] = {
		"no-reorder", RELIC_F_NO_REORDER, false, STRATEGY_NONE, 0 },
	[STRATEGY_NO_MAGIC] = {
		"no-magic", RELIC_F_NO_MAGIC, false, STRATEGY_NONE, 0 },
	[STRATEGY_NAIVE_NO_MAGIC] = {
		"naive+no-magic", RELIC_F_NAIVE | RELIC_F_NO_MAGIC, false,
		STRATEGY_NO_MAGIC, 0 },
	[STRATEGY_NO_INDEX_NO_MAGIC] = {
		"no-index+no-magic", RELIC_F_NO_INDEX | RELIC_F_NO_MAGIC, false,
		STRATEGY_NO_MAGIC, 0 },
	[STRATEGY_NO_REORDER_NO_MAGIC] = {
		"no-reorder+no-magic", RELIC_F_NO_REORDER | RELIC_F_NO_MAGIC,
		false, STRATEGY_NO_MAGIC, 0 },
	[STRATEGY_PARALLEL_NO_MAGIC] = {
		"parallel+no-magic", RELIC_F_PARALLEL_ALWAYS | RELIC_F_NO_MAGIC,
		false, STRATEGY_NO_MAGIC, PARALLEL_THREADS },
	[STRATEGY_MATERIALIZED] = {
		"materialized", 0, true, STRATEGY_NO_MAGIC, 0 },
};

/*
 * What one strategy produced: an answer, a tuple total and a count of
 * string values per query, and for a strategy that materializes, the
 * derivations that took.
 */
typedef struct run run_t;

struct run {
	answer_t	answers[RANDOM_MAX_QUERIES];
	size_t		totals[RANDOM_MAX_QUERIES];
	size_t		strings[RANDOM_MAX_QUERIES];
	uint64_t	derivations;
};

/*
 * An evaluation error: the words the library's message about it opens
 * with, and the kind of failure it is.
 */
typedef struct evaluation_error evaluation_error_t;

struct evaluation_error {
	const char		*opening;
	random_failure_kind_t	 kind;
};

/* The diagnostic callback's state while a strategy loads the program. */
typedef struct diag_capture diag_capture_t;

struct diag_capture {
	FILE		*report;
	const char	*strategy;
	size_t		 errors;
};

static void	 capture_diagnostic(void *capture_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static void	 drop_last_row(answer_t *answer);
static random_failure_kind_t count_bindings(relic_t *ctx, size_t query,
		    size_t rule, FILE *report, uint64_t *expectedp);
static random_failure_kind_t count_derivations(
		    const random_program_t *program, FILE *report,
		    uint64_t *expectedp);
static bool	 constants_equal(random_term_t left, random_term_t right);
static bool	 fact_repeats_earlier(const random_program_t *program,
		    size_t r);
static bool	 fact_seeds(const random_program_t *program, size_t r);
static const char *error_text(relic_t *ctx);
static bool	 load_program(relic_t *ctx, const char *text,
		    const diag_capture_t *capture);
static random_failure_kind_t evaluation_failure(relic_t *ctx);
static random_failure_kind_t run_strategy(const random_program_t *program,
		    const char *text, random_strategy_t id,
		    const random_options_t *options, run_t *run, FILE *report);

const char *
random_strategy_name(random_strategy_t strategy)
{
	if (strategy >= STRATEGY_COUNT)
		return ("?");
	return (strategies[strategy].name);
}

/*
 * Run the program under every strategy and compare the runs.  Each run is
 * announced as it begins, so that a caller watching from outside knows
 * which strategy was running if this process dies; STRATEGY_NONE is
 * announced once the runs are over, however they ended.  What disagreed
 * is described on report; the first failure found is returned, answers
 * before totals, since a wrong answer is the more direct report.
 */
random_failure_t
random_compare(const random_program_t *program,
    const random_options_t *options, FILE *report,
    random_announce_fn *announce, void *announce_ctx)
{
	random_failure_t failure = { FAILURE_NONE, STRATEGY_NONE, 0 };
	const run_t *expected;
	run_t *runs;
	char *text;

	runs = calloc(STRATEGY_COUNT, sizeof(*runs));
	if (runs == NULL)
		err(1, "calloc");
	text = random_program_text(program);
	expected = &runs[STRATEGY_DEFAULT];

	for (random_strategy_t s = 0; s < STRATEGY_COUNT; s++) {
		announce(announce_ctx, s);
		failure.kind = run_strategy(program, text, s, options, &runs[s],
		    report);
		if (failure.kind != FAILURE_NONE) {
			failure.strategy = s;
			break;
		}
	}
	announce(announce_ctx, STRATEGY_NONE);
	if (failure.kind != FAILURE_NONE)
		goto done;

	for (random_strategy_t s = STRATEGY_DEFAULT + 1; s < STRATEGY_COUNT;
	    s++) {
		for (size_t q = 0; q < program->query_count; q++) {
			const answer_t *want = &expected->answers[q];
			const answer_t *got = &runs[s].answers[q];

			if (answer_equal(want, got))
				continue;
			fprintf(report, "query %zu: %s answers %zu rows, "
			    "default %zu\n", q + 1, strategies[s].name,
			    got->row_count, want->row_count);
			answer_report_difference(report, want, got);
			failure.kind = FAILURE_ANSWERS;
			failure.strategy = s;
			goto done;
		}
	}

	for (random_strategy_t s = STRATEGY_DEFAULT + 1; s < STRATEGY_COUNT;
	    s++) {
		random_strategy_t reference = strategies[s].total_reference;

		if (reference == STRATEGY_NONE)
			continue;
		for (size_t q = 0; q < program->query_count; q++) {
			if (runs[s].totals[q] == runs[reference].totals[q])
				continue;
			fprintf(report, "query %zu: %s holds %zu tuples, "
			    "%s holds %zu\n", q + 1, strategies[s].name,
			    runs[s].totals[q], strategies[reference].name,
			    runs[reference].totals[q]);
			failure.kind = FAILURE_TOTALS;
			failure.strategy = s;
			goto done;
		}
		for (size_t q = 0; q < program->query_count; q++) {
			if (runs[s].strings[q] == runs[reference].strings[q])
				continue;
			fprintf(report, "query %zu: %s holds %zu string "
			    "values, %s holds %zu\n", q + 1,
			    strategies[s].name, runs[s].strings[q],
			    strategies[reference].name,
			    runs[reference].strings[q]);
			failure.kind = FAILURE_TOTALS;
			failure.strategy = s;
			goto done;
		}
	}

	if (!random_program_has_anonymous(program)) {
		uint64_t expected_derivations;
		uint64_t made;

		failure.kind = count_derivations(program, report,
		    &expected_derivations);
		if (failure.kind != FAILURE_NONE)
			goto done;

		made = runs[STRATEGY_MATERIALIZED].derivations;
		if (made != expected_derivations) {
			fprintf(report, "%s makes %" PRIu64 " derivations, the "
			    "rules have %" PRIu64 " ways to derive\n",
			    strategies[STRATEGY_MATERIALIZED].name, made,
			    expected_derivations);
			failure.kind = FAILURE_DERIVATIONS;
			failure.strategy = STRATEGY_MATERIALIZED;
		}
	}

done:
	for (size_t s = 0; s < STRATEGY_COUNT; s++) {
		for (size_t q = 0; q < RANDOM_MAX_QUERIES; q++)
			answer_free(&runs[s].answers[q]);
	}
	free(runs);
	free(text);
	return (failure);
}

/*
 * Load, check and answer the program under one strategy, recording each
 * query's answer and the tuples held once it has run.  A refusal of the
 * program, or a query failing with no limit configured, is a failure of its
 * own: nothing a generated program does should cause either.
 */
static random_failure_kind_t
run_strategy(const random_program_t *program, const char *text,
    random_strategy_t id, const random_options_t *options, run_t *run,
    FILE *report)
{
	const strategy_t *strategy = &strategies[id];
	random_failure_kind_t kind = FAILURE_NONE;
	diag_capture_t capture;
	relic_config_t config;
	relic_t *ctx;
	size_t query_count;

	bzero(&config, sizeof(config));
	config.flags = strategy->flags;
	config.threads = strategy->threads;
	ctx = relic_create_config(&config);
	if (ctx == NULL)
		errx(1, "relic_create_config failed");

	capture.report = report;
	capture.strategy = strategy->name;
	capture.errors = 0;
	relic_diag_set(ctx, capture_diagnostic, &capture);

	if (!load_program(ctx, text, &capture)) {
		fprintf(report, "%s: the program was refused\n",
		    strategy->name);
		kind = FAILURE_REJECTED;
		goto done;
	}

	query_count = relic_query_count(ctx);
	if (query_count != program->query_count) {
		fprintf(report, "%s: %zu queries loaded, %zu written\n",
		    strategy->name, query_count, program->query_count);
		kind = FAILURE_REJECTED;
		goto done;
	}

	if (strategy->materialize) {
		relic_stats_t stats;

		if (relic_materialize(ctx) != 0) {
			fprintf(report, "%s: materializing failed: %s\n",
			    strategy->name, error_text(ctx));
			kind = evaluation_failure(ctx);
			goto done;
		}
		relic_stats(ctx, &stats);
		run->derivations = stats.derivations;
	}

	for (size_t q = 0; q < query_count; q++) {
		answer_t *answer = &run->answers[q];
		relic_query_t *query;
		relic_stats_t stats;
		bool collected;

		query = relic_query_loaded(ctx, q);
		if (query == NULL)
			errx(1, "relic_query_loaded failed");
		collected = answer_collect(ctx, query, answer);
		relic_query_destroy(query);
		if (!collected) {
			fprintf(report, "%s: query %zu failed: %s\n",
			    strategy->name, q + 1, error_text(ctx));
			kind = evaluation_failure(ctx);
			goto done;
		}

		relic_stats(ctx, &stats);
		run->totals[q] = stats.tuples;
		run->strings[q] = stats.string_values;

		if (options->inject_fault && id == STRATEGY_NAIVE)
			drop_last_row(answer);
	}

done:
	relic_destroy(ctx);
	return (kind);
}

/*
 * The derivations that materializing the program must make: one per distinct
 * fact of a predicate some rule with a body also derives, which seeds that
 * predicate's derived relation, and for every other rule the number of distinct
 * bindings of its body's variables over the fixed point, and of each of its
 * group bodies', counted as the rows of a query asking for that body under the
 * naive fixed point without the Magic rewrite.  The queries come in the order
 * random_bindings_text() writes them.  A refusal of the program or a failed
 * query is reported as it would be for a strategy.
 */
static random_failure_kind_t
count_derivations(const random_program_t *program, FILE *report,
    uint64_t *expectedp)
{
	random_failure_kind_t kind = FAILURE_NONE;
	diag_capture_t capture;
	relic_config_t config;
	relic_t *ctx;
	char *text;
	size_t query = 0;

	bzero(&config, sizeof(config));
	config.flags = RELIC_F_NAIVE | RELIC_F_NO_MAGIC;
	ctx = relic_create_config(&config);
	if (ctx == NULL)
		errx(1, "relic_create_config failed");

	capture.report = report;
	capture.strategy = "bindings";
	capture.errors = 0;
	relic_diag_set(ctx, capture_diagnostic, &capture);

	*expectedp = 0;
	text = random_bindings_text(program);
	if (!load_program(ctx, text, &capture)) {
		fprintf(report, "bindings: the program was refused\n");
		kind = FAILURE_REJECTED;
		goto done;
	}

	for (size_t r = 0; r < program->rule_count; r++) {
		const random_body_t *body = &program->rules[r].body;
		size_t body_count;

		if (body->length == 0) {
			if (fact_seeds(program, r))
				(*expectedp)++;
			continue;
		}

		body_count = 1 + random_body_aggregates(body);
		for (size_t b = 0; b < body_count; b++) {
			kind = count_bindings(ctx, query, r, report, expectedp);
			if (kind != FAILURE_NONE)
				goto done;
			query++;
		}
	}

done:
	free(text);
	relic_destroy(ctx);
	return (kind);
}

/*
 * Add the rows of loaded query `query`, the bindings of a body of rule
 * `rule`, to *expectedp.
 */
static random_failure_kind_t
count_bindings(relic_t *ctx, size_t query, size_t rule, FILE *report,
    uint64_t *expectedp)
{
	relic_query_t *body;
	relic_result_t *result;

	body = relic_query_loaded(ctx, query);
	if (body == NULL)
		errx(1, "relic_query_loaded failed");
	result = relic_query_collect(ctx, body);
	relic_query_destroy(body);
	if (result == NULL) {
		fprintf(report, "bindings: a body of rule %zu failed: %s\n",
		    rule + 1, error_text(ctx));
		return (evaluation_failure(ctx));
	}
	*expectedp += relic_result_rows(result);
	relic_result_destroy(result);
	return (FAILURE_NONE);
}

/*
 * Whether fact `r` seeds a derived relation: a rule with a body derives its
 * predicate, and no earlier fact states the same tuple.  A predicate no
 * such rule derives is read from its base store, and nothing evaluates its
 * facts.
 */
static bool
fact_seeds(const random_program_t *program, size_t r)
{
	uint8_t predicate = program->rules[r].head_predicate;
	bool derived = false;

	for (size_t other = 0; other < program->rule_count; other++) {
		if (program->rules[other].head_predicate == predicate &&
		    program->rules[other].body.length > 0)
			derived = true;
	}

	return (derived && !fact_repeats_earlier(program, r));
}

/*
 * Whether a fact before fact `r` states the same tuple: the same predicate,
 * and in every column the same value.
 */
static bool
fact_repeats_earlier(const random_program_t *program, size_t r)
{
	const random_rule_t *fact = &program->rules[r];
	uint8_t arity = program->predicates[fact->head_predicate].arity;

	for (size_t other = 0; other < r; other++) {
		const random_rule_t *earlier = &program->rules[other];
		bool same = earlier->body.length == 0 &&
		    earlier->head_predicate == fact->head_predicate;

		for (uint8_t column = 0; same && column < arity; column++)
			same = constants_equal(earlier->head[column],
			    fact->head[column]);
		if (same)
			return (true);
	}

	return (false);
}

/*
 * Whether two constants of a fact are one value.  Every constant of the
 * pool is a distinct value, and so is every number, but a number and an
 * integer of the pool are one value when they spell one integer.
 */
static bool
constants_equal(random_term_t left, random_term_t right)
{
	const random_constant_t *constant;
	long long number;

	if (left.kind == right.kind)
		return (left.index == right.index);
	if (left.kind == TERM_NUMBER) {
		random_term_t swap = left;

		left = right;
		right = swap;
	}

	constant = &random_constants[left.index];
	number = (long long)right.index - RANDOM_NUMBER_MAX;
	return (constant->type == CONSTANT_INTEGER &&
	    strtoll(constant->spelling, NULL, 10) == number);
}

/*
 * The self-test's fault: an answer loses its last row, as a join that
 * stopped one tuple early would lose it.
 */
static void
drop_last_row(answer_t *answer)
{
	if (answer->row_count == 0)
		return;
	answer->row_count--;
	free(answer->rows[answer->row_count]);
}

/*
 * The kind of failure the context's last error is: one of the evaluation
 * errors, told by the message the library opens it with, or else a query
 * that failed.
 */
static random_failure_kind_t
evaluation_failure(relic_t *ctx)
{
	static const evaluation_error_t errors[] = {
		{ "integer overflow in ", FAILURE_UNDEFINED },
		{ "division by zero in ", FAILURE_UNDEFINED },
		{ "string too long in ", FAILURE_TOO_LONG },
		{ "invalid regular expression in ", FAILURE_PATTERN },
	};
	const char *error = relic_error(ctx);

	for (size_t i = 0; error != NULL && i < sizeof(errors) /
	    sizeof(errors[0]); i++) {
		if (strncmp(error, errors[i].opening,
		    strlen(errors[i].opening)) == 0)
			return (errors[i].kind);
	}
	return (FAILURE_QUERY);
}

/* The library's account of the last error, for a report. */
static const char *
error_text(relic_t *ctx)
{
	const char *error = relic_error(ctx);

	return (error != NULL ? error : "no error reported");
}

/*
 * Load and check the program.  An error diagnostic is a refusal even when
 * both calls succeed, since a generated program should draw none.
 */
static bool
load_program(relic_t *ctx, const char *text, const diag_capture_t *capture)
{
	if (relic_load_string(ctx, "random.dl", text) != 0)
		return (false);
	if (relic_check(ctx) != 0)
		return (false);
	return (capture->errors == 0);
}

/*
 * Report an error the library gives while loading, and count it.  Warnings
 * are expected from a generated program -- a singleton variable, a
 * predicate nothing defines -- and are not a finding.
 */
static void
capture_diagnostic(void *capture_ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	diag_capture_t *capture = capture_ctx;

	if (level != RELIC_ERROR)
		return;
	capture->errors++;
	if (where != NULL && where->file != NULL)
		fprintf(capture->report, "%s: %s:%" PRIu32 ":%" PRIu32
		    ": error: %s\n", capture->strategy, where->file,
		    where->line, where->column, message);
	else
		fprintf(capture->report, "%s: error: %s\n", capture->strategy,
		    message);
}
