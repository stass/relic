/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Direct grouped reduction: what a query's run stores, and what it
 * answers, when the rows of a source go straight into group state.
 *
 * The scenarios are built around the file pipeline: a producer `kind`
 * keeps each file with two keys computed from its path, `group` projects
 * those keys, and `grouped` counts each key's files.  `kind` keeps its
 * path, so no two files make one row; it is read only by the fold and by
 * the projection of the fold's key, so a query's run may count its rows
 * into one state per key and store none of them.
 *
 * The reference is every scenario's program under
 * RELIC_F_NO_GROUP_REDUCTION, which stores every relation and folds by
 * walking rows.  Each scenario compares the sorted answers the two give,
 * then what each stored: a reduced source holds no tuple and no index
 * after the query, and a source that something else reads, or whose
 * producer is not proved distinct, holds its rows as the reference's
 * does.  The rendering of a row writes a string in quotes, so a symbol
 * and a string of the same bytes differ.
 */

#include "config.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"
#include "relation.h"

#define	SCENARIO_REDUCED	"a proved source is reduced, not stored"
#define	SCENARIO_KEPT		"a source read otherwise keeps its rows"
#define	SCENARIO_LIFETIME	"nothing reduced outlives its query"
#define	SCENARIO_EMPTY		"empty groups keep their outcomes"
#define	SCENARIO_RETAINED	"retained values and permuted keys are kept"
#define	SCENARIO_LIMITED	"a memory limit refuses a reduction cleanly"
#define	SCENARIO_CEILING	"the row ceiling outlives the store"
#define	SCENARIO_PEAK		"the peak counts the summary"
#define	SCENARIO_CHUNKED	"many retained values are interned in chunks"
#define	SCENARIO_STORED		"a stored relation is summarized once per family"
#define	SCENARIO_CARDINALITY	"an unkeyed count reads no rows"
#define	SCENARIO_ACCUMULATORS	"stored accumulators answer as rows do"
#define	SCENARIO_BUILT_INDEX	"an index built already is no saving"
#define	SCENARIO_DELTA_INDEX	"an index a delta plan reads is no saving"
#define	SCENARIO_UNREACHED	"a fold no binding reaches is not summarized"

/*
 * The memory limits a_memory_limit_refuses_a_reduction_cleanly() tries:
 * every LIMIT_COARSE bytes from what the facts take, up to LIMIT_SPAN past
 * it, until one answers, then every LIMIT_STEP bytes of the LIMIT_FINE
 * below that one.
 */
#define	LIMIT_SPAN		(8 * 1024 * 1024)
#define	LIMIT_COARSE		(16 * 1024)
#define	LIMIT_FINE		(16 * 1024)
#define	LIMIT_STEP		64

/*
 * The files of the pipeline: FILES kept, KEYS_D directories and KEYS_E
 * extensions, chosen coprime so that every pair of keys has FILES /
 * (KEYS_D * KEYS_E) files, and TESTS more that the producer's filter
 * drops.
 */
#define	FILES			240
#define	TESTS			20
#define	KEYS_D			4
#define	KEYS_E			3
#define	GROUPS			(KEYS_D * KEYS_E)
#define	PER_GROUP		(FILES / GROUPS)

_Static_assert(FILES % GROUPS == 0, "every group has as many files");

/* Room for a program, and for the rendered answers any scenario expects. */
#define	PROGRAM_MAX		32768
#define	RENDERED_MAX		16384
#define	ROWS_MAX		512

/*
 * The pipeline's rules.  `kind` keeps the path, so its producer derives
 * each row once; `group` projects the fold's key; `grouped` folds it.
 */
#define	KIND_RULE \
	"kind(P, D, E) :- file(P), match(P, \"/test_[0-9]+[.]\") < 0,\n" \
	"    D = substr(P, 0, 2), E = substr(P, index(P, \".\") + 1).\n"
#define	GROUP_RULE	"group(D, E) :- kind(_, D, E).\n"
#define	GROUPED_RULE \
	"grouped(D, E, N) :- group(D, E), N = count { kind(P, D, E) }.\n"
#define	PIPELINE	KIND_RULE GROUP_RULE GROUPED_RULE

/*
 * A producer like kind's that also keeps each file's length, for folds
 * that read a value column, and outer keys for them, d0 and e0 held by
 * files and d7 and e8 by none.
 */
#define	STEM_RULE \
	"stem(P, D, E, L) :- file(P), match(P, \"/test_[0-9]+[.]\") < 0,\n" \
	"    D = substr(P, 0, 2), E = substr(P, index(P, \".\") + 1),\n" \
	"    L = length(P).\n"
#define	OUTER_KEYS \
	"dir(\"d0\"). dir(\"d7\"). ext(\"e0\"). ext(\"e8\").\n"

typedef struct rendering rendering_t;

/* The rows a query delivered, rendered, rows separated by `|`. */
struct rendering {
	relic_t	*ctx;
	char	 text[RENDERED_MAX];
	size_t	 length;
	size_t	 rows;
};

typedef struct outcome outcome_t;

/*
 * What one run of a scenario's query gave and left behind: the sorted
 * answer, whether it succeeded, the tuples and secondary indexes the
 * named source's relation held after it, and the reduction counters.
 */
struct outcome {
	char		answer[RENDERED_MAX];
	size_t		source_tuples;
	size_t		source_indexes;
	uint64_t	sources_reduced;
	uint64_t	reduced_rows;
	uint64_t	reduced_groups;
	uint64_t	derivations;
	bool		succeeded;
};

static void	 append_text(rendering_t *rendering, const char *text,
		    size_t length);
static int	 render_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static int	 compare_rows(const void *left, const void *right);
static void	 sort_rows(char *text);
static relic_t	*context_flagged(const char *program, unsigned int flags);
static int	 run_query(relic_t *ctx, const char *text, char *answer);
static relic_relation_t *relation_named(relic_t *ctx, const char *name,
		    uint16_t arity);
static bool	 run_outcome(const char *program, unsigned int flags,
		    const char *query, outcome_t *outcome);
static void	 write_files(char *program, size_t size, const char *rules);
static void	 expected_groups(char *expected, size_t size);
static bool	 answers_agree(const char *scenario, const char *what,
		    const char *program, const char *query,
		    outcome_t *reduced, outcome_t *reference);
static void	 the_file_pipeline_stores_no_source_rows(void);
static void	 unproved_or_shared_sources_keep_their_rows(void);
static void	 a_later_query_reads_the_whole_source(void);
static void	 empty_groups_keep_their_outcomes(void);
static void	 retained_values_and_permuted_keys_are_kept(void);
static int	 intern_file_row(relic_t *ctx, unsigned int i,
		    relic_dom_t *row);
static int	 reduce_rows(relic_t *ctx, const relic_reduced_source_t *source,
		    const relic_relation_t *relation,
		    const relic_reduce_batch_t *batch,
		    relic_reducer_t **reducerp, relic_summary_t **summaryp);
static void	 the_peak_counts_the_summary(void);
#ifndef RELIC_REDUCE_ROWS_MAX
static void	 many_retained_values_are_interned_in_chunks(void);
#endif
static bool	 answers_under_limit(const char *program, size_t limit,
		    const char *expected);
static void	 a_memory_limit_refuses_a_reduction_cleanly(void);
static void	 write_degree(char *program, size_t size);
static void	 a_stored_relation_is_summarized_once_per_family(void);
static void	 an_unkeyed_count_reads_no_rows(void);
static void	 summarized_as_reference(const char *scenario,
		    const char *program, unsigned int flags, const char *query,
		    uint64_t summaries);
static void	 stored_accumulators_answer_as_rows_do(void);
static void	 an_index_built_already_is_no_saving(void);
static void	 an_index_a_delta_plan_reads_is_no_saving(void);
static void	 a_fold_no_binding_reaches_is_not_summarized(void);

static void
append_text(rendering_t *rendering, const char *text, size_t length)
{
	if (length > sizeof(rendering->text) - 1 - rendering->length)
		length = sizeof(rendering->text) - 1 - rendering->length;

	bcopy(text, rendering->text + rendering->length, length);
	rendering->length += length;
	rendering->text[rendering->length] = '\0';
}

/*
 * The row callback: rows separated by `|`, values by a space, a string in
 * quotes.
 */
static int
render_row(void *callback_ctx, const relic_value_t *values, size_t count)
{
	rendering_t *rendering;
	const char *bytes;
	char number[32];
	size_t length;
	size_t i;

	rendering = callback_ctx;
	if (rendering->rows > 0)
		append_text(rendering, "|", 1);
	rendering->rows++;

	for (i = 0; i < count; i++) {
		if (i > 0)
			append_text(rendering, " ", 1);

		if (values[i].type == RELIC_INTEGER) {
			snprintf(number, sizeof(number), "%" PRId64,
			    values[i].v.integer);
			append_text(rendering, number, strlen(number));
			continue;
		}

		bytes = relic_string(rendering->ctx, values[i].v.id, &length);
		if (values[i].type == RELIC_STRING)
			append_text(rendering, "\"", 1);
		append_text(rendering, bytes, length);
		if (values[i].type == RELIC_STRING)
			append_text(rendering, "\"", 1);
	}

	return (0);
}

static int
compare_rows(const void *left, const void *right)
{
	return (strcmp(*(const char *const *)left,
	    *(const char *const *)right));
}

/* Sort a rendering's rows bytewise, in place. */
static void
sort_rows(char *text)
{
	static char copy[RENDERED_MAX];
	char *rows[ROWS_MAX];
	char *cursor;
	size_t count;
	size_t i;

	if (text[0] == '\0')
		return;

	strlcpy(copy, text, sizeof(copy));
	count = 0;
	cursor = copy;
	while (cursor != NULL && count < ROWS_MAX)
		rows[count++] = strsep(&cursor, "|");

	qsort(rows, count, sizeof(rows[0]), compare_rows);

	text[0] = '\0';
	for (i = 0; i < count; i++) {
		if (i > 0)
			strlcat(text, "|", RENDERED_MAX);
		strlcat(text, rows[i], RENDERED_MAX);
	}
}

/*
 * A context created with `flags` holding a loaded program, or NULL having
 * recorded the failure.
 */
static relic_t *
context_flagged(const char *program, unsigned int flags)
{
	relic_config_t config;
	relic_t *ctx;

	bzero(&config, sizeof(config));
	config.flags = flags;

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, "creating a context", "a context is created");
		return (NULL);
	}

	if (relic_load_string(ctx, "test.dl", program) != 0) {
		checkf(false, "loading a test program", "the program loads: %s",
		    relic_error(ctx));
		relic_destroy(ctx);
		return (NULL);
	}

	return (ctx);
}

/*
 * Prepare and run a query, its rows rendered sorted into `answer`, of
 * RENDERED_MAX bytes.  Returns what the run returned, or -1 when the
 * query could not be prepared.
 */
static int
run_query(relic_t *ctx, const char *text, char *answer)
{
	static rendering_t rendering;
	relic_query_t *query;
	int status;

	bzero(&rendering, sizeof(rendering));
	rendering.ctx = ctx;

	query = relic_query_prepare(ctx, NULL, text);
	if (query == NULL)
		return (-1);

	status = relic_query_run(ctx, query, render_row, &rendering);
	relic_query_destroy(query);

	sort_rows(rendering.text);
	strlcpy(answer, rendering.text, RENDERED_MAX);

	return (status);
}

/*
 * The relation the context holds for the predicate `name`/`arity`: the
 * derived relation of a predicate rules derive, created empty if the run
 * made none.
 */
static relic_relation_t *
relation_named(relic_t *ctx, const char *name, uint16_t arity)
{
	relic_pred_id_t pred;

	if (relic_pred_intern(ctx, name, strlen(name), arity, NULL,
	    &pred) != 0)
		return (NULL);

	return (relic_relation_read(ctx, pred));
}

/*
 * Run `query` once over `program` under `flags`, recording in `outcome`
 * its answer and what kind/3 held after it.
 */
static bool
run_outcome(const char *program, unsigned int flags, const char *query,
    outcome_t *outcome)
{
	relic_relation_t *source;
	relic_t *ctx;

	bzero(outcome, sizeof(*outcome));
	ctx = context_flagged(program, flags);
	if (ctx == NULL)
		return (false);

	outcome->succeeded = run_query(ctx, query, outcome->answer) == 0;
	source = relation_named(ctx, "kind", 3);
	if (source != NULL) {
		outcome->source_tuples = relic_relation_count(source);
		outcome->source_indexes = source->index_count;
	}
	outcome->sources_reduced = ctx->counters.sources_reduced;
	outcome->reduced_rows = ctx->counters.reduced_rows;
	outcome->reduced_groups = ctx->counters.reduced_groups;
	outcome->derivations = ctx->counters.derivations;

	relic_destroy(ctx);

	return (true);
}

/*
 * The pipeline's facts, FILES files of KEYS_D directories and KEYS_E
 * extensions and TESTS test files the producer drops, followed by
 * `rules`.  Each path is under 32 bytes.
 */
static void
write_files(char *program, size_t size, const char *rules)
{
	size_t used;
	unsigned int i;

	_Static_assert((FILES + TESTS) * 40 + 4096 < PROGRAM_MAX,
	    "the files fit a program");

	used = 0;
	for (i = 0; i < FILES; i++)
		used += (size_t)snprintf(program + used, size - used,
		    "file(\"d%u/f%u.e%u\").\n", i % KEYS_D, i, i % KEYS_E);
	for (i = 0; i < TESTS; i++)
		used += (size_t)snprintf(program + used, size - used,
		    "file(\"d%u/test_%u.e%u\").\n", i % KEYS_D, i, i % KEYS_E);
	(void)snprintf(program + used, size - used, "%s", rules);
}

/*
 * grouped(D, E, N)?'s answer: every pair of keys, PER_GROUP files each,
 * in the sorted order of their rendering.
 */
static void
expected_groups(char *expected, size_t size)
{
	char row[64];
	unsigned int d;
	unsigned int e;

	expected[0] = '\0';
	for (d = 0; d < KEYS_D; d++) {
		for (e = 0; e < KEYS_E; e++) {
			snprintf(row, sizeof(row), "%s\"d%u\" \"e%u\" %u",
			    expected[0] == '\0' ? "" : "|", d, e, PER_GROUP);
			strlcat(expected, row, size);
		}
	}
}

/*
 * Run `query` over `program` by default and under the reference, and
 * check that both succeed with one answer.
 */
static bool
answers_agree(const char *scenario, const char *what, const char *program,
    const char *query, outcome_t *reduced, outcome_t *reference)
{
	if (!run_outcome(program, 0, query, reduced) ||
	    !run_outcome(program, RELIC_F_NO_GROUP_REDUCTION, query,
	    reference))
		return (false);

	checkf(reduced->succeeded && reference->succeeded, scenario,
	    "%s: %s should succeed both ways", what, query);
	checkf(strcmp(reduced->answer, reference->answer) == 0, scenario,
	    "%s: %s should answer as the reference does: \"%.200s\" against "
	    "\"%.200s\"", what, query, reduced->answer, reference->answer);

	return (reduced->succeeded && reference->succeeded);
}

/*
 * The file pipeline's query counts every pair of keys.  By default `kind`
 * is reduced: its relation holds no tuple and no index, every one of its
 * rows counted into one of GROUPS groups; under the reference it holds
 * FILES tuples.  Its rows count as derivations either way, but `group`'s
 * rule derives one tuple per row of kind under the reference, and the
 * projection that replaces it one per key, so the default derives
 * FILES - GROUPS fewer.
 */
static void
the_file_pipeline_stores_no_source_rows(void)
{
	static char program[PROGRAM_MAX];
	static char expected[RENDERED_MAX];
	static outcome_t reduced;
	static outcome_t reference;

	write_files(program, sizeof(program), PIPELINE);
	expected_groups(expected, sizeof(expected));
	if (!answers_agree(SCENARIO_REDUCED, "the pipeline", program,
	    "grouped(D, E, N)?", &reduced, &reference))
		return;

	checkf(strcmp(reduced.answer, expected) == 0, SCENARIO_REDUCED,
	    "every pair of keys should count %u files: \"%.200s\"",
	    PER_GROUP, reduced.answer);
	checkf(reduced.source_tuples == 0 && reduced.source_indexes == 0,
	    SCENARIO_REDUCED, "kind should hold no tuple and no index, not "
	    "%zu tuples and %zu indexes", reduced.source_tuples,
	    reduced.source_indexes);
	checkf(reduced.sources_reduced == 1 && reduced.reduced_rows == FILES &&
	    reduced.reduced_groups == GROUPS, SCENARIO_REDUCED,
	    "one source of %u rows should be reduced into %u groups, not %"
	    PRIu64 " of %" PRIu64 " rows into %" PRIu64, FILES, GROUPS,
	    reduced.sources_reduced, reduced.reduced_rows,
	    reduced.reduced_groups);
	checkf(reference.source_tuples == FILES &&
	    reference.sources_reduced == 0, SCENARIO_REDUCED,
	    "the reference should store kind's %u rows, not %zu", FILES,
	    reference.source_tuples);
	checkf(reduced.derivations + FILES == reference.derivations + GROUPS,
	    SCENARIO_REDUCED, "the default should derive %u fewer: %" PRIu64
	    " against %" PRIu64, FILES - GROUPS, reduced.derivations,
	    reference.derivations);
}

/*
 * A source keeps its rows when its producer is not proved to derive each
 * row once, or when anything but folds of one key and projections of that
 * key read it: whatever the case, the default stores what the reference
 * stores and answers as it does.
 */
static void
unproved_or_shared_sources_keep_their_rows(void)
{
	static const struct {
		const char	*what;
		const char	*rules;
		const char	*query;
	} cases[] = {
		{ "a producer dropping the path",
		    "kind(D, E, 0) :- file(P), D = substr(P, 0, 2),\n"
		    "    E = substr(P, index(P, \".\") + 1).\n"
		    GROUP_RULE GROUPED_RULE, "grouped(D, E, N)?" },
		{ "another reader of the rows",
		    PIPELINE "paths(P) :- kind(P, _, _).\n",
		    "grouped(D, E, N)?" },
		{ "a negated reader",
		    PIPELINE "lonely(D, E) :- group(D, E), "
		    "not kind(\"x\", D, E).\n", "grouped(D, E, N)?" },
		{ "a fold of another key",
		    PIPELINE "bydir(D, N) :- group(D, _), "
		    "N = count { kind(P, D, E) }.\n", "grouped(D, E, N)?" },
		{ "a query of the source",
		    PIPELINE, "kind(P, \"d0\", E)?" },
		{ "a fact beside the producer",
		    PIPELINE "kind(\"x\", \"d0\", \"e0\").\n",
		    "grouped(D, E, N)?" },
		{ "a second producer",
		    PIPELINE "kind(\"x\", D, E) :- group(D, E), D = \"d0\".\n",
		    "grouped(D, E, N)?" },
		{ "a projection that filters",
		    KIND_RULE GROUPED_RULE
		    "group(D, E) :- kind(_, D, E), D != \"d1\".\n",
		    "grouped(D, E, N)?" },
		{ "a projection of a constant",
		    KIND_RULE GROUPED_RULE
		    "group(D, E) :- kind(_, D, E).\n"
		    "group(\"d9\", \"e9\").\n", "grouped(D, E, N)?" }
	};
	static char program[PROGRAM_MAX];
	static outcome_t reduced;
	static outcome_t reference;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		write_files(program, sizeof(program), cases[i].rules);
		if (!answers_agree(SCENARIO_KEPT, cases[i].what, program,
		    cases[i].query, &reduced, &reference))
			continue;

		checkf(reduced.sources_reduced == 0 &&
		    reduced.source_tuples == reference.source_tuples,
		    SCENARIO_KEPT, "%s: kind should be stored, %zu tuples as "
		    "the reference's %zu, %" PRIu64 " sources reduced",
		    cases[i].what, reduced.source_tuples,
		    reference.source_tuples, reduced.sources_reduced);
	}
}

/*
 * A reduced run records no fixed point, so the queries after it see whole
 * relations: kind's rows asked for next, the pipeline asked again from a
 * prepared query, and the program materialized.  Each matches the
 * reference.
 */
static void
a_later_query_reads_the_whole_source(void)
{
	static char program[PROGRAM_MAX];
	static char first[RENDERED_MAX];
	static char again[RENDERED_MAX];
	static char rows[RENDERED_MAX];
	relic_relation_t *source;
	relic_query_t *query;
	rendering_t *rendering;
	relic_t *ctx;
	size_t held;

	write_files(program, sizeof(program), PIPELINE);
	ctx = context_flagged(program, 0);
	if (ctx == NULL)
		return;

	check(run_query(ctx, "grouped(D, E, N)?", first) == 0,
	    SCENARIO_LIFETIME, "the pipeline should be answered");
	check(run_query(ctx, "kind(P, D, E)?", rows) == 0 &&
	    relic_relation_count(relation_named(ctx, "kind", 3)) == FILES,
	    SCENARIO_LIFETIME, "kind asked next should hold every file");

	rendering = calloc(1, sizeof(*rendering));
	query = relic_query_prepare(ctx, NULL, "grouped(D, E, N)?");
	check(rendering != NULL && query != NULL, SCENARIO_LIFETIME,
	    "the pipeline should be prepared");
	if (rendering != NULL && query != NULL) {
		rendering->ctx = ctx;
		check(relic_query_run(ctx, query, render_row, rendering) == 0,
		    SCENARIO_LIFETIME, "a prepared pipeline should run");
		bzero(rendering, sizeof(*rendering));
		rendering->ctx = ctx;
		check(relic_query_run(ctx, query, render_row, rendering) == 0,
		    SCENARIO_LIFETIME, "and run again");
		sort_rows(rendering->text);
		strlcpy(again, rendering->text, sizeof(again));
		checkf(strcmp(first, again) == 0, SCENARIO_LIFETIME,
		    "a second run should answer as the first: \"%.100s\"",
		    again);
		checkf(ctx->counters.sources_reduced == 3, SCENARIO_LIFETIME,
		    "each run of the pipeline should reduce kind afresh, %"
		    PRIu64 " reductions in three runs",
		    ctx->counters.sources_reduced);
	}
	relic_query_destroy(query);
	free(rendering);

	check(relic_materialize(ctx) == 0, SCENARIO_LIFETIME,
	    "the program should materialize");
	source = relation_named(ctx, "kind", 3);
	held = source == NULL ? 0 : relic_relation_count(source);
	checkf(held == FILES, SCENARIO_LIFETIME,
	    "the materialized program should hold kind's %u rows, not %zu",
	    FILES, held);

	relic_destroy(ctx);
}

/*
 * An outer key no source row holds folds an empty group: a count or a sum
 * gives zero and a min gives no value, as walking the stored rows does.
 * The outer keys are facts, two of them held by rows and two by none, so
 * each fold finds a group for some keys and none for the others, from a
 * source reduced on the fold's key, the sum and the min reading columns
 * of `stem`.  And a producer whose filter
 * drops every row leaves a summary of no group, which every key misses.
 * In d0/e0 are the files numbered by multiples of 12 below FILES: one of
 * one digit, eight of two and eleven of three, each path "d0/f.e0" and
 * the number, so their lengths sum to 20 * 7 + 1 + 16 + 33.
 */
static void
empty_groups_keep_their_outcomes(void)
{
	static const struct {
		const char	*what;
		const char	*rules;
		const char	*query;
		const char	*expected;
	} cases[] = {
		{ "a count and a sum of keys no row holds",
		    STEM_RULE OUTER_KEYS
		    "zero(D, E, N, S) :- dir(D), ext(E),\n"
		    "    N = count { stem(P1, D, E, L1) },\n"
		    "    S = sum(L2) { stem(P2, D, E, L2) }.\n",
		    "zero(D, E, N, S)?",
		    "\"d0\" \"e0\" 20 190|\"d0\" \"e8\" 0 0|"
		    "\"d7\" \"e0\" 0 0|\"d7\" \"e8\" 0 0" },
		{ "a min of keys no row holds",
		    STEM_RULE OUTER_KEYS
		    "least(D, E, M) :- dir(D), ext(E),\n"
		    "    M = min(P) { stem(P, D, E, L) }.\n",
		    "least(D, E, M)?", "\"d0\" \"e0\" \"d0/f0.e0\"" },
		{ "a producer that keeps nothing",
		    "kind(P, D, E) :- file(P), P = \"none\", "
		    "D = substr(P, 0, 2), E = P.\n"
		    OUTER_KEYS
		    "none(D, E, N) :- dir(D), ext(E), "
		    "N = count { kind(P, D, E) }.\n",
		    "none(D, E, N)?",
		    "\"d0\" \"e0\" 0|\"d0\" \"e8\" 0|"
		    "\"d7\" \"e0\" 0|\"d7\" \"e8\" 0" }
	};
	static char program[PROGRAM_MAX];
	static outcome_t reduced;
	static outcome_t reference;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		write_files(program, sizeof(program), cases[i].rules);
		if (!answers_agree(SCENARIO_EMPTY, cases[i].what, program,
		    cases[i].query, &reduced, &reference))
			continue;

		checkf(reduced.sources_reduced == 1 &&
		    reference.sources_reduced == 0, SCENARIO_EMPTY,
		    "%s: the source should be reduced by default alone, "
		    "%" PRIu64 " reductions", cases[i].what,
		    reduced.sources_reduced);
		checkf(strcmp(reduced.answer, cases[i].expected) == 0,
		    SCENARIO_EMPTY, "%s: the answer should be \"%s\", not "
		    "\"%.200s\"", cases[i].what, cases[i].expected,
		    reduced.answer);
	}
}

/*
 * What a reduction retains of values the producer computed, which the
 * context has not assigned while the producer runs, is assigned when the
 * reduction ends: each directory's key, the least and greatest of each
 * file's computed stem, a string, and of its computed length, an integer,
 * and the sum of the lengths.  The projection of the directories reads
 * the source on column 1 alone.  And a projection may write the key's
 * columns in another order: `rev` holds each pair of keys reversed, and
 * the fold over it still finds each pair's group.
 */
static void
retained_values_and_permuted_keys_are_kept(void)
{
	static const struct {
		const char	*what;
		const char	*rules;
		const char	*query;
		const char	*source;
		uint16_t	 arity;
	} cases[] = {
		{ "computed extremes",
		    "stem(P, D, B, L) :- file(P),\n"
		    "    match(P, \"/test_[0-9]+[.]\") < 0, D = substr(P, 0, 2),\n"
		    "    B = substr(P, 3, 40), L = length(P).\n"
		    "dirs(D) :- stem(_, D, _, _).\n"
		    "edge(D, LO, HI, SHORT, LONG, S) :- dirs(D),\n"
		    "    LO = min(B1) { stem(P1, D, B1, L1) },\n"
		    "    HI = max(B2) { stem(P2, D, B2, L2) },\n"
		    "    SHORT = min(L3) { stem(P3, D, B3, L3) },\n"
		    "    LONG = max(L4) { stem(P4, D, B4, L4) },\n"
		    "    S = sum(L5) { stem(P5, D, B5, L5) }.\n",
		    "edge(D, LO, HI, SHORT, LONG, S)?", "stem", 4 },
		{ "a reversed projection",
		    KIND_RULE "rev(E, D) :- kind(_, D, E).\n"
		    "byrev(E, D, N) :- rev(E, D), "
		    "N = count { kind(P, D, E) }.\n",
		    "byrev(E, D, N)?", "kind", 3 }
	};
	static char program[PROGRAM_MAX];
	static char reduced[RENDERED_MAX];
	static char reference[RENDERED_MAX];
	relic_relation_t *source;
	relic_t *ctx;
	size_t held;
	size_t i;
	int status;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		write_files(program, sizeof(program), cases[i].rules);

		ctx = context_flagged(program, 0);
		if (ctx == NULL)
			continue;
		status = run_query(ctx, cases[i].query, reduced);
		source = relation_named(ctx, cases[i].source, cases[i].arity);
		held = source == NULL ? SIZE_MAX :
		    relic_relation_count(source);
		checkf(status == 0 && ctx->counters.sources_reduced == 1 &&
		    held == 0, SCENARIO_RETAINED, "%s: %s should be reduced "
		    "and hold nothing, not %zu tuples with %" PRIu64
		    " reductions: %s", cases[i].what, cases[i].source, held,
		    ctx->counters.sources_reduced, relic_error(ctx));
		relic_destroy(ctx);

		ctx = context_flagged(program, RELIC_F_NO_GROUP_REDUCTION);
		if (ctx == NULL)
			continue;
		check(run_query(ctx, cases[i].query, reference) == 0,
		    SCENARIO_RETAINED, "the reference should answer");
		relic_destroy(ctx);

		checkf(reduced[0] != '\0' && strcmp(reduced, reference) == 0,
		    SCENARIO_RETAINED, "%s: the answer should be the "
		    "reference's: \"%.200s\" against \"%.200s\"",
		    cases[i].what, reduced, reference);
	}
}

/*
 * File `i`'s row of three integers the context assigns: its number, its
 * directory's and its extension's.
 */
static int
intern_file_row(relic_t *ctx, unsigned int i, relic_dom_t *row)
{
	if (relic_value_intern_integer(ctx, i, &row[0]) != 0)
		return (-1);
	if (relic_value_intern_integer(ctx, i % KEYS_D, &row[1]) != 0)
		return (-1);

	return (relic_value_intern_integer(ctx, i % KEYS_E, &row[2]));
}

/*
 * Open a builder for `source`, of three columns, take `batch` into it and
 * finish it into *summaryp.  *reducerp is the builder whatever happened,
 * for the caller to close, and NULL only when it could not be opened.
 */
static int
reduce_rows(relic_t *ctx, const relic_reduced_source_t *source,
    const relic_relation_t *relation, const relic_reduce_batch_t *batch,
    relic_reducer_t **reducerp, relic_summary_t **summaryp)
{
	size_t taken;

	if (relic_reducer_open(ctx, source, 3, reducerp) != 0)
		return (-1);
	if (relic_reducer_take(ctx, *reducerp, relation, batch, &taken) != 0)
		return (-1);

	return (relic_reducer_finish(ctx, *reducerp, relation, summaryp));
}

/*
 * A builder's peak is the most it and the summary it finishes held at
 * once, so it is at least what both hold as finishing returns.  The rows
 * here hold only integers the context has assigned, which leaves finishing
 * nothing to intern, so the summary is the one thing it adds to the
 * builder.  They are keyed on the directory alone, KEYS_D groups, fewer
 * than the smallest array util.c makes, so the groups' block is allocated
 * once and no growth of it held two blocks at once, which would raise the
 * peak past the summary's share.  Closing the builder and destroying the
 * summary give back everything either was charged.
 */
static void
the_peak_counts_the_summary(void)
{
	relic_dom_t rows[3 * FILES];
	relic_reduced_source_t source;
	relic_reduce_batch_t batch;
	relic_relation_t *relation;
	relic_reducer_t *reducer;
	relic_summary_t *summary;
	relic_t *ctx;
	size_t before;
	size_t held;
	unsigned int i;

	ctx = context_flagged(PIPELINE, 0);
	if (ctx == NULL)
		return;
	relation = relation_named(ctx, "kind", 3);
	if (relation == NULL) {
		check(false, SCENARIO_PEAK, "kind should have a relation");
		relic_destroy(ctx);
		return;
	}
	for (i = 0; i < FILES; i++) {
		if (intern_file_row(ctx, i, &rows[3 * i]) != 0) {
			check(false, SCENARIO_PEAK,
			    "the rows' values should be interned");
			relic_destroy(ctx);
			return;
		}
	}

	bzero(&source, sizeof(source));
	source.counts = true;
	source.key_mask = UINT32_C(1) << 1;
	bzero(&batch, sizeof(batch));
	batch.rows = rows;
	batch.count = FILES;

	before = ctx->account.arena_bytes;
	reducer = NULL;
	summary = NULL;
	if (reduce_rows(ctx, &source, relation, &batch, &reducer,
	    &summary) != 0) {
		checkf(false, SCENARIO_PEAK, "the rows should be reduced: %s",
		    relic_error(ctx));
	} else {
		held = ctx->account.arena_bytes - before;
		checkf(relic_summary_groups(summary) == KEYS_D &&
		    relic_reducer_peak(reducer) >= held, SCENARIO_PEAK,
		    "a peak of %zu bytes should cover the %zu the builder and "
		    "its summary of %zu groups hold", relic_reducer_peak(reducer),
		    held, relic_summary_groups(summary));
	}
	relic_reducer_close(ctx, reducer);
	relic_summary_destroy(ctx, summary);
	checkf(ctx->account.arena_bytes == before, SCENARIO_PEAK,
	    "everything charged should be given back, %zu bytes before and "
	    "%zu after", before, ctx->account.arena_bytes);

	relic_destroy(ctx);
}

#ifndef RELIC_REDUCE_ROWS_MAX
/*
 * More retained values than one chunk of interning holds, several times
 * over.  `big` derives CHUNKED rows whose computed key, I / 2 offset past
 * every number the facts hold, makes CHUNKED / 2 groups of two, and whose
 * computed J is the key's max: so every group retains its key and its
 * extreme, values the context has not assigned, and each odd key value
 * is first met as an extreme and then, in a later chunk once the keys pass
 * the first chunk, as a key.  The counts sum to CHUNKED, and the maxima,
 * OFFSET + 2m + 1 for each group m, to CHUNKED / 2 * OFFSET plus
 * (CHUNKED / 2)^2.
 */
static void
many_retained_values_are_interned_in_chunks(void)
{
#define	CHUNKED		10000u
#define	OFFSET		1000000u
	static const char rules[] =
	    "big(I, K, J) :- n(I), K = I / 2 + 1000000, J = I + 1000000.\n"
	    "keys(K) :- big(_, K, _).\n"
	    "cnt(K, N, M) :- keys(K), N = count { big(I, K, J) },\n"
	    "    M = max(J2) { big(I2, K, J2) }.\n"
	    "total(S, T) :- S = sum(N) { cnt(K, N, M) },\n"
	    "    T = sum(M2) { cnt(K2, N2, M2) }.\n";
	static char reduced[RENDERED_MAX];
	static char reference[RENDERED_MAX];
	char expected[64];
	relic_relation_t *source;
	relic_t *ctx;
	uint64_t groups;
	size_t size;
	size_t used;
	size_t held;
	char *program;
	unsigned int i;
	int status;

	_Static_assert(CHUNKED / 2 > 4096 && OFFSET > CHUNKED,
	    "the keys pass a chunk and no fact holds them");
	size = (size_t)CHUNKED * 16 + sizeof(rules);
	program = malloc(size);
	if (program == NULL) {
		check(false, SCENARIO_CHUNKED, "the program should fit memory");
		return;
	}
	used = 0;
	for (i = 0; i < CHUNKED; i++)
		used += (size_t)snprintf(program + used, size - used,
		    "n(%u).\n", i);
	(void)snprintf(program + used, size - used, "%s", rules);
	groups = CHUNKED / 2;
	snprintf(expected, sizeof(expected), "%u %" PRIu64, CHUNKED,
	    groups * OFFSET + groups * groups);

	ctx = context_flagged(program, 0);
	if (ctx != NULL) {
		status = run_query(ctx, "total(S, T)?", reduced);
		source = relation_named(ctx, "big", 3);
		held = source == NULL ? SIZE_MAX :
		    relic_relation_count(source);
		checkf(status == 0 && ctx->counters.sources_reduced >= 1 &&
		    held == 0, SCENARIO_CHUNKED, "big should be reduced and "
		    "hold nothing, not %zu tuples with %" PRIu64
		    " reductions: %s", held, ctx->counters.sources_reduced,
		    relic_error(ctx));
		relic_destroy(ctx);
	}

	ctx = context_flagged(program, RELIC_F_NO_GROUP_REDUCTION);
	if (ctx != NULL) {
		check(run_query(ctx, "total(S, T)?", reference) == 0,
		    SCENARIO_CHUNKED, "the reference should answer");
		relic_destroy(ctx);
	}
	free(program);

	checkf(strcmp(reduced, expected) == 0 &&
	    strcmp(reference, expected) == 0, SCENARIO_CHUNKED,
	    "both should answer \"%s\", not \"%s\" and \"%s\"", expected,
	    reduced, reference);
#undef	CHUNKED
#undef	OFFSET
}
#endif

/*
 * Run the pipeline under a memory limit of `limit` bytes, checking that it
 * either answers `expected` or fails naming the limit.  Returns whether it
 * answered; a limit the facts alone exceed answers nothing.
 */
static bool
answers_under_limit(const char *program, size_t limit, const char *expected)
{
	static char answer[RENDERED_MAX];
	relic_config_t config;
	relic_t *ctx;
	bool answered;

	bzero(&config, sizeof(config));
	config.max_memory = limit;
	ctx = relic_create_config(&config);
	if (ctx == NULL)
		return (false);
	if (relic_load_string(ctx, "test.dl", program) != 0) {
		relic_destroy(ctx);
		return (false);
	}

	answered = run_query(ctx, "grouped(D, E, N)?", answer) == 0;
	if (answered)
		checkf(strcmp(answer, expected) == 0, SCENARIO_LIMITED,
		    "under memory=%zu the answer should be whole: \"%.200s\"",
		    limit, answer);
	else
		checkf(relic_error_class(ctx) == RELIC_ERROR_LIMIT,
		    SCENARIO_LIMITED, "under memory=%zu the run should fail "
		    "on the limit: %s", limit, relic_error(ctx));
	relic_destroy(ctx);

	return (answered);
}

/*
 * A memory limit refuses a reduction as it refuses storing the rows: the
 * pipeline either answers as it does without one or fails naming the
 * memory limit, and leaves nothing behind for the sanitizers to find.  The
 * least limit that answers is found coarsely, and every limit in the
 * LIMIT_FINE bytes below it is then tried, which spans the reduction's
 * charges: the reducer holds a few kilobytes, charged as the producer
 * runs, after the evaluation's own buffers.
 */
static void
a_memory_limit_refuses_a_reduction_cleanly(void)
{
	static char program[PROGRAM_MAX];
	static char expected[RENDERED_MAX];
	relic_t *ctx;
	size_t loaded;
	size_t limit;
	size_t least;

	write_files(program, sizeof(program), PIPELINE);
	ctx = context_flagged(program, 0);
	if (ctx == NULL)
		return;
	loaded = ctx->account.store_bytes + ctx->account.value_bytes +
	    ctx->account.pool_bytes + ctx->account.predtab_bytes;
	check(run_query(ctx, "grouped(D, E, N)?", expected) == 0,
	    SCENARIO_LIMITED, "the pipeline should answer without a limit");
	relic_destroy(ctx);

	least = SIZE_MAX;
	for (limit = loaded; limit < loaded + LIMIT_SPAN;
	    limit += LIMIT_COARSE) {
		if (answers_under_limit(program, limit, expected)) {
			least = limit;
			break;
		}
	}
	checkf(least != SIZE_MAX, SCENARIO_LIMITED, "the pipeline should "
	    "answer under some limit below memory=%zu", loaded + LIMIT_SPAN);
	if (least == SIZE_MAX || least < loaded + LIMIT_FINE)
		return;

	for (limit = least - LIMIT_FINE; limit < least; limit += LIMIT_STEP)
		(void)answers_under_limit(program, limit, expected);
}

/*
 * The degree fixture: a ring of RING nodes, each with an edge to the
 * RING_STEPS nodes the steps reach; SIDE sources with edges into the ring
 * and none in, SIDE sinks with an edge from the ring and none out, two
 * self-loops, and two facts stated twice, which the base relation holds
 * once.
 */
#define	RING			40
#define	SIDE			5
#define	DEGREE_NODES		(RING + 2 * SIDE)
#define	DEGREE_RULES \
	"node(X) :- edge(X, _).\n" \
	"node(Y) :- edge(_, Y).\n" \
	"degree(X, O, I) :- node(X), O = count { edge(X, Y) },\n" \
	"    I = count { edge(Z, X) }.\n"

static void
write_degree(char *program, size_t size)
{
	static const unsigned int steps[] = { 1, 3, 7, 11 };
	size_t used;
	unsigned int i;
	unsigned int s;

	used = 0;
	for (i = 0; i < RING; i++)
		for (s = 0; s < sizeof(steps) / sizeof(steps[0]); s++)
			used += (size_t)snprintf(program + used, size - used,
			    "edge(%u, %u).\n", i, (i + steps[s]) % RING);
	for (i = 0; i < SIDE; i++)
		used += (size_t)snprintf(program + used, size - used,
		    "edge(%u, %u). edge(%u, %u). edge(%u, %u).\n",
		    100 + i, i, 100 + i, i + 1, i, 200 + i);
	(void)snprintf(program + used, size - used, "%s%s",
	    "edge(5, 5). edge(6, 6). edge(0, 1). edge(100, 0).\n",
	    DEGREE_RULES);
}

/*
 * The degrees fold the base relation edge on its first column and on its
 * second, each once for every node.  The run summarizes each of the two
 * families once, in one walk of edge, rather than once for each node, and
 * never builds the indexes the folds would read rows through: each fold
 * is one probe of a summary.  So it is in the whole program's
 * materialization, which has a scope of its own.  The reference reads
 * rows through both indexes.  A second query builds its summaries again,
 * since none outlives the run that built it.
 */
static void
a_stored_relation_is_summarized_once_per_family(void)
{
	static const struct {
		const char	*name;
		unsigned int	 flags;
	} runs[] = {
		{ "the query's run", 0 },
		{ "the materialization", RELIC_F_NO_MAGIC },
	};
	static char program[PROGRAM_MAX];
	static char reference[RENDERED_MAX];
	static char answer[RENDERED_MAX];
	const relic_relation_t *edge;
	relic_t *ctx;
	size_t r;

	write_degree(program, sizeof(program));
	ctx = context_flagged(program, RELIC_F_NO_GROUP_REDUCTION);
	if (ctx == NULL)
		return;
	check(run_query(ctx, "degree(X, O, I)?", reference) == 0,
	    SCENARIO_STORED, "the reference should answer");
	edge = relation_named(ctx, "edge", 2);
	checkf(ctx->counters.summaries_built == 0 && edge != NULL &&
	    edge->index_count == 2, SCENARIO_STORED, "the reference should "
	    "fold through two indexes and build no summary, not %" PRIu64,
	    ctx->counters.summaries_built);
	relic_destroy(ctx);

	for (r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
		ctx = context_flagged(program, runs[r].flags);
		if (ctx == NULL)
			return;
		checkf(run_query(ctx, "degree(X, O, I)?", answer) == 0 &&
		    strcmp(answer, reference) == 0, SCENARIO_STORED,
		    "%s should answer as the reference does", runs[r].name);
		edge = relation_named(ctx, "edge", 2);
		checkf(ctx->counters.summaries_built == 2 &&
		    ctx->counters.read.summary_probes == 2 * DEGREE_NODES,
		    SCENARIO_STORED, "%s should build 2 summaries and probe "
		    "them %u times, not %" PRIu64 " and %" PRIu64,
		    runs[r].name, 2 * DEGREE_NODES,
		    ctx->counters.summaries_built,
		    ctx->counters.read.summary_probes);
		checkf(edge != NULL && edge->index_count == 0 &&
		    relic_relation_count(edge) == RING * 4 + SIDE * 3 + 2,
		    SCENARIO_STORED, "%s should keep edge's rows once each "
		    "and build no index of it", runs[r].name);
		if (runs[r].flags == 0) {
			check(run_query(ctx, "degree(X, O, I)?", answer) == 0 &&
			    strcmp(answer, reference) == 0 &&
			    ctx->counters.summaries_built == 4,
			    SCENARIO_STORED, "a second query should build its "
			    "summaries again and answer the same");
		}
		relic_destroy(ctx);
	}
}

/*
 * An unkeyed count of a stored relation reads its tuple count, visiting
 * none of its rows: the reference, which walks them, scans exactly the
 * relation's tuples more, and both count the duplicate fact once.
 */
static void
an_unkeyed_count_reads_no_rows(void)
{
	static char program[PROGRAM_MAX];
	static char reference[RENDERED_MAX];
	static char answer[RENDERED_MAX];
	uint64_t scanned;
	size_t used;
	relic_t *ctx;
	unsigned int i;

	used = 0;
	for (i = 0; i < FILES; i++)
		used += (size_t)snprintf(program + used, sizeof(program) - used,
		    "e(%u, %u).\n", i, i + 1);
	(void)snprintf(program + used, sizeof(program) - used,
	    "e(0, 1).\ntotal(N) :- N = count { e(X, Y) }.\n");

	ctx = context_flagged(program, RELIC_F_NO_GROUP_REDUCTION);
	if (ctx == NULL)
		return;
	check(run_query(ctx, "total(N)?", reference) == 0 &&
	    strcmp(reference, "240") == 0, SCENARIO_CARDINALITY,
	    "the reference should count every distinct fact");
	scanned = ctx->counters.read.tuples_scanned;
	relic_destroy(ctx);

	ctx = context_flagged(program, 0);
	if (ctx == NULL)
		return;
	check(run_query(ctx, "total(N)?", answer) == 0 &&
	    strcmp(answer, reference) == 0, SCENARIO_CARDINALITY,
	    "the count should answer as the reference does");
	checkf(scanned - ctx->counters.read.tuples_scanned == FILES,
	    SCENARIO_CARDINALITY, "the count should scan %u tuples fewer "
	    "than the reference, not %" PRIu64, FILES,
	    scanned - ctx->counters.read.tuples_scanned);
	relic_destroy(ctx);
}


/*
 * Answer `query` over `program` under `flags`, and under them with the
 * reference too, checking that the answers agree and that the run builds
 * `summaries` summaries of stored relations, and the reference none.
 */
static void
summarized_as_reference(const char *scenario, const char *program,
    unsigned int flags, const char *query, uint64_t summaries)
{
	static char reference[RENDERED_MAX];
	static char answer[RENDERED_MAX];
	relic_t *ctx;

	ctx = context_flagged(program, flags | RELIC_F_NO_GROUP_REDUCTION);
	if (ctx == NULL)
		return;
	checkf(run_query(ctx, query, reference) == 0 &&
	    ctx->counters.summaries_built == 0, scenario,
	    "the reference should answer %s building no summary", query);
	relic_destroy(ctx);

	ctx = context_flagged(program, flags);
	if (ctx == NULL)
		return;
	checkf(run_query(ctx, query, answer) == 0 &&
	    strcmp(answer, reference) == 0, scenario,
	    "%s should answer as the reference does", query);
	checkf(ctx->counters.summaries_built == summaries, scenario,
	    "%s should build %" PRIu64 " summaries, not %" PRIu64, query,
	    summaries, ctx->counters.summaries_built);
	relic_destroy(ctx);
}

/*
 * A count, a sum, a min and a max of one stored relation on one key, read
 * from one summary of three accumulators: KEYS_W keys of ROWS_W rows each,
 * holding ties, a sum whose exact total passes through 2^63 and comes back
 * to zero, groups of mixed types, whose sum and extremes have no value
 * while their count does, and a key with no rows.  The family holds the
 * three accumulators stats needs whichever rule folds it, so a query of
 * counted alone, whose one count would not repay updating them, keeps
 * its row folds, and so do ones of extremes and of highest, whose min and
 * max meet a mixed group without a sum before them, and whose max meets
 * it alone; the materialization, which derives stats too, builds the
 * summary for stats, and the others read it.
 */
static void
stored_accumulators_answer_as_rows_do(void)
{
#define	KEYS_W	20
#define	ROWS_W	12
	static const struct {
		unsigned int	flags;
		uint64_t	counted;
	} runs[] = {
		{ 0, 0 },
		{ RELIC_F_NO_MAGIC, 1 },
	};
	static char program[PROGRAM_MAX];
	size_t used;
	size_t r;
	unsigned int k;
	unsigned int i;

	used = 0;
	for (k = 0; k <= KEYS_W; k++)
		used += (size_t)snprintf(program + used, sizeof(program) - used,
		    "key(%u).\n", k);
	for (k = 0; k < KEYS_W; k++) {
		for (i = 0; i < ROWS_W; i++) {
			if (k == 2 && i < 4)
				used += (size_t)snprintf(program + used,
				    sizeof(program) - used, "w(%u, %u, %s).\n",
				    k, i, i < 2 ? "4611686018427387904" :
				    "-4611686018427387904");
			else if (k % 7 == 3 && i == 5)
				used += (size_t)snprintf(program + used,
				    sizeof(program) - used,
				    "w(%u, %u, \"x\").\n", k, i);
			else
				used += (size_t)snprintf(program + used,
				    sizeof(program) - used, "w(%u, %u, %u).\n",
				    k, i, (k * 7 + i) % 5);
		}
	}
	(void)snprintf(program + used, sizeof(program) - used, "%s",
	    "stats(K, N, S, LO, HI) :- key(K), N = count { w(K, I, V) },\n"
	    "    S = sum(V1) { w(K, I1, V1) },\n"
	    "    LO = min(V2) { w(K, I2, V2) },\n"
	    "    HI = max(V3) { w(K, I3, V3) }.\n"
	    "counted(K, N) :- key(K), N = count { w(K, I, V) }.\n"
	    "extremes(K, LO, HI) :- key(K), LO = min(V2) { w(K, I2, V2) },\n"
	    "    HI = max(V3) { w(K, I3, V3) }.\n"
	    "highest(K, HI) :- key(K), HI = max(V3) { w(K, I3, V3) }.\n");

	for (r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
		summarized_as_reference(SCENARIO_ACCUMULATORS, program,
		    runs[r].flags, "stats(K, N, S, LO, HI)?", 1);
		summarized_as_reference(SCENARIO_ACCUMULATORS, program,
		    runs[r].flags, "counted(K, N)?", runs[r].counted);
		summarized_as_reference(SCENARIO_ACCUMULATORS, program,
		    runs[r].flags, "extremes(K, LO, HI)?", runs[r].counted);
		summarized_as_reference(SCENARIO_ACCUMULATORS, program,
		    runs[r].flags, "highest(K, HI)?", runs[r].counted);
	}
#undef	KEYS_W
#undef	ROWS_W
}

/*
 * A fold whose index an earlier component built is not credited with the
 * index's construction, which a summary would not save: here p's rule
 * reads e's index on its first column before r's folds are planned, so
 * the ten probes of three groups' rows remain cheaper than a summary.
 */
static void
an_index_built_already_is_no_saving(void)
{
	static const unsigned int flags[] = { 0, RELIC_F_NO_MAGIC };
	static char program[PROGRAM_MAX];
	size_t used;
	size_t f;
	unsigned int i;

	used = (size_t)snprintf(program, sizeof(program), "%s",
	    "key(1). key(4). key(7).\n"
	    "p(X) :- key(X), e(X, _).\n"
	    "r(X, N) :- p(X), N = count { e(X, Y) }.\n");
	for (i = 0; i < 100; i++)
		used += (size_t)snprintf(program + used, sizeof(program) - used,
		    "e(%u, %u).\n", i % 10, i);

	for (f = 0; f < sizeof(flags) / sizeof(flags[0]); f++)
		summarized_as_reference(SCENARIO_BUILT_INDEX, program,
		    flags[f], "r(X, N)?", 0);
}

/*
 * The index a recursive rule's delta-first plan reads is built whatever
 * the folds do, so it is no saving of a summary even when the rule's
 * ordinary plan would read e otherwise: r's second rule, run semi-naively,
 * looks e up on its first column for each new r, which is the index the
 * first rule's count would read.
 */
static void
an_index_a_delta_plan_reads_is_no_saving(void)
{
	static char program[PROGRAM_MAX];
	size_t used;
	unsigned int i;

	used = (size_t)snprintf(program, sizeof(program), "%s",
	    "key(1). key(4). key(7).\n"
	    "r(X, N) :- key(X), N = count { e(X, Y) }.\n"
	    "r(Y, N) :- e(X, Y), r(X, N).\n");
	for (i = 0; i < 100; i++)
		used += (size_t)snprintf(program + used, sizeof(program) - used,
		    "e(%u, %u).\n", i % 10, i);

	summarized_as_reference(SCENARIO_DELTA_INDEX, program,
	    RELIC_F_NO_MAGIC, "r(X, N)?", 0);
}

/*
 * A recursive rule's draft reads its own relations empty, so no binding
 * reaches a fold after such a read, and nothing shows a summary of it
 * would be read:
 * however much the index builds would cost, none is built, here with the
 * rule's order kept as written so that both folds would look rows up,
 * through indexes of e that only they read.  e's 100 rows have 20 keys
 * on its first column and 25 on its second.
 */
static void
a_fold_no_binding_reaches_is_not_summarized(void)
{
	static char program[PROGRAM_MAX];
	size_t used;
	unsigned int i;

	used = (size_t)snprintf(program, sizeof(program), "%s",
	    "reach(0).\n"
	    "reach(Y) :- reach(X), f(X, Y), O = count { e(Y, Z) },\n"
	    "    I = count { e(W, Y) }, O >= I.\n");
	for (i = 0; i < 100; i++)
		used += (size_t)snprintf(program + used, sizeof(program) - used,
		    "e(%u, %u). f(%u, %u).\n", i % 20, i % 25, i % 20, i % 25);

	summarized_as_reference(SCENARIO_UNREACHED, program,
	    RELIC_F_NO_REORDER | RELIC_F_NO_MAGIC, "reach(X)?", 0);
}

#ifdef RELIC_REDUCE_ROWS_MAX
/*
 * A reduced source has no store, but keeps the ceiling on its rows that a
 * store would have, which this build lowers to FILES, so that every other
 * scenario stays within it: one more file than the pipeline holds is
 * refused as storing kind past the ceiling would be, naming the source,
 * having counted as derived each of the FILES rows it reduced first, while
 * the reference, whose store the lowered ceiling does not bound, answers.
 */
static void
the_row_ceiling_outlives_the_store(void)
{
	static char program[PROGRAM_MAX];
	static char answer[RENDERED_MAX];
	relic_t *ctx;
	int status;

	_Static_assert(RELIC_REDUCE_ROWS_MAX == FILES,
	    "the lowered ceiling is the pipeline's rows");

	write_files(program, sizeof(program),
	    PIPELINE "file(\"d0/one_more.e0\").\n");
	ctx = context_flagged(program, 0);
	if (ctx == NULL)
		return;
	status = run_query(ctx, "grouped(D, E, N)?", answer);
	checkf(status != 0 && relic_error_class(ctx) == RELIC_ERROR_LIMIT &&
	    strcmp(relic_error(ctx),
	    "the relation of kind/3 is too large to grow") == 0,
	    SCENARIO_CEILING, "the pipeline should be refused at the "
	    "ceiling, not answer with status %d: %s", status,
	    relic_error(ctx));
	checkf(ctx->counters.derivations == FILES, SCENARIO_CEILING,
	    "every row reduced before the refusal should count as derived, "
	    "%u, not %" PRIu64, FILES, ctx->counters.derivations);
	relic_destroy(ctx);

	ctx = context_flagged(program, RELIC_F_NO_GROUP_REDUCTION);
	if (ctx == NULL)
		return;
	check(run_query(ctx, "grouped(D, E, N)?", answer) == 0,
	    SCENARIO_CEILING, "the reference should answer");
	relic_destroy(ctx);
}

#endif

int
main(void)
{
	the_file_pipeline_stores_no_source_rows();
	unproved_or_shared_sources_keep_their_rows();
	a_later_query_reads_the_whole_source();
	empty_groups_keep_their_outcomes();
	retained_values_and_permuted_keys_are_kept();
	the_peak_counts_the_summary();
#ifndef RELIC_REDUCE_ROWS_MAX
	many_retained_values_are_interned_in_chunks();
#endif
	a_memory_limit_refuses_a_reduction_cleanly();
	a_stored_relation_is_summarized_once_per_family();
	an_unkeyed_count_reads_no_rows();
	stored_accumulators_answer_as_rows_do();
	an_index_built_already_is_no_saving();
	an_index_a_delta_plan_reads_is_no_saving();
	a_fold_no_binding_reaches_is_not_summarized();
#ifdef RELIC_REDUCE_ROWS_MAX
	the_row_ceiling_outlives_the_store();
#endif

	return (check_summary("reduce"));
}
