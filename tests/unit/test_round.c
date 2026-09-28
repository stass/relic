/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Deferred rounds: a round whose insertions wait for its end inserts what
 * the serial round inserts, in the order it inserts it, and fails only
 * where and as the serial round fails.
 *
 * Every scenario evaluates one program in two contexts: one as the serial
 * evaluator runs it, and one with RELIC_F_PARALLEL_ALWAYS, which defers
 * every round that can be.  The contexts load the same text, so they
 * intern the same values under the same identifiers and number the same
 * predicates alike, and their relations are compared word for word: the
 * same tuples under the same tuple identifiers, which is what the merge's
 * order promises and what comparing answers as sets would not see.  The
 * counters of work, the storage the relations hold and every diagnostic are
 * compared too.
 *
 * A deferred context whose rounds were all declined would pass every
 * comparison, being the serial evaluator, so each scenario also requires
 * that rounds were merged, and the limits' sweeps that rounds were
 * abandoned and run again serially.
 */

#include "config.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_ORDER		"a deferred round inserts in serial order"
#define	SCENARIO_REWRITE	"a query's rewrite runs deferred rounds"
#define	SCENARIO_TUPLES		"the tuple limit refuses the serial tuple"
#define	SCENARIO_MEMORY		"the memory limit refuses as serially"
#define	SCENARIO_NOTES		"notes come as the serial evaluator's do"

/* Room for a generated program and for a context's diagnostics. */
#define	PROGRAM_MAX		(256 * 1024)
#define	DIAGNOSTICS_MAX		4096

/*
 * The nodes of the generated graph, and the length of its chain: enough
 * that a round's scans are cut into many pieces and its tasks fill several
 * batches, and few enough that the limits can be swept over many values.
 * The limits are swept in about LIMIT_STEPS steps.
 */
#define	GRAPH_NODES		40
#define	CHAIN_LENGTH		150
#define	LIMIT_STEPS		60

typedef struct diagnostics diagnostics_t;
typedef struct text text_t;

/*
 * Every diagnostic a context reported, one per line as level, line, column
 * and message.
 */
struct diagnostics {
	char	text[DIAGNOSTICS_MAX];
	size_t	length;
};

/*
 * A program being written, or the rows a query delivered, one per line in
 * the order they came.
 */
struct text {
	char	bytes[PROGRAM_MAX];
	size_t	length;
};

static void	 a_deferred_round_inserts_in_serial_order(void);
static void	 a_querys_rewrite_runs_deferred_rounds(void);
static void	 appendf(text_t *text, const char *format, ...)
		    RELIC_TEST_PRINTFLIKE(2, 3);
static relic_t	*context_for(const char *program, unsigned int flags,
		    size_t max_tuples, size_t max_memory,
		    diagnostics_t *diagnostics);
static uint64_t	 abandoned_rounds(const relic_t *ctx);
static bool	 contexts_agree(const char *scenario, const char *what,
		    relic_t *serial, relic_t *deferred);
static void	 graph_program(text_t *text);
static size_t	 lines_in(const diagnostics_t *diagnostics);
static void	 limits_refuse_as_serially(const char *scenario,
		    const char *program, bool memory);
static void	 notes_come_as_serially(void);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static int	 record_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static uint64_t	 merged_rounds(const relic_t *ctx);
static bool	 relations_agree(const relic_t *serial,
		    const relic_t *deferred);
static int	 run_query(relic_t *ctx, const char *query, text_t *rows);

/*
 * The program every scenario but the notes' runs, written once by main(),
 * and the rows of the two runs of a query.
 */
static text_t graph;
static text_t serial_rows;
static text_t deferred_rows;

static void
appendf(text_t *text, const char *format, ...)
{
	va_list arguments;
	int written;

	va_start(arguments, format);
	written = vsnprintf(text->bytes + text->length,
	    sizeof(text->bytes) - text->length, format, arguments);
	va_end(arguments);

	if (written < 0 ||
	    (size_t)written >= sizeof(text->bytes) - text->length) {
		fprintf(stderr, "test_round: a program outgrew its buffer\n");
		exit(1);
	}
	text->length += (size_t)written;
}

/*
 * A program whose rounds are large enough to be cut into many pieces and
 * to fill many batches, and which has a rule of every kind a round runs:
 *
 *	edge/2 over GRAPH_NODES nodes, two edges out of each, so that the
 *	closure is every pair and takes several rounds;
 *	path/2 its closure, linear, and hop/2 the same closure written with
 *	two member literals, so that a round runs two variants of one rule;
 *	even/1 and odd/1, mutually recursive over a chain, a component of two
 *	members whose rounds write both;
 *	reach/1 from a constant, whose plan starts with a lookup and is not
 *	cut; pair/2 over two derivations of one relation filtered by a
 *	comparison, with many duplicates across the items of one round;
 *	lonely/1 behind a negation, and done/0 of no columns.
 */
static void
graph_program(text_t *text)
{
	unsigned int i;

	text->length = 0;
	text->bytes[0] = '\0';

	for (i = 0; i < GRAPH_NODES; i++) {
		appendf(text, "edge(n%u, n%u).\n", i, (i + 1) % GRAPH_NODES);
		appendf(text, "edge(n%u, n%u).\n", i,
		    (i * 7 + 3) % GRAPH_NODES);
		appendf(text, "node(n%u).\n", i);
	}
	appendf(text, "node(island).\n");
	for (i = 0; i < CHAIN_LENGTH; i++)
		appendf(text, "succ(%u, %u).\n", i, i + 1);
	appendf(text, "zero(0).\n");

	appendf(text,
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- path(X, Y), edge(Y, Z).\n"
	    "hop(X, Y) :- edge(X, Y).\n"
	    "hop(X, Z) :- hop(X, Y), hop(Y, Z).\n"
	    "even(X) :- zero(X).\n"
	    "even(Y) :- odd(X), succ(X, Y).\n"
	    "odd(Y) :- even(X), succ(X, Y).\n"
	    "reach(Y) :- path(n0, Y).\n"
	    "end(X) :- edge(X, _).\n"
	    "end(Y) :- edge(_, Y).\n"
	    "pair(X, Y) :- end(X), end(Y), X < Y, edge(X, Y).\n"
	    "lonely(X) :- node(X), not reach(X).\n"
	    "done :- lonely(X).\n");
}

static void
record_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	diagnostics_t *diagnostics;
	int written;

	diagnostics = callback_ctx;
	written = snprintf(diagnostics->text + diagnostics->length,
	    sizeof(diagnostics->text) - diagnostics->length, "%d %" PRIu32
	    ":%" PRIu32 " %s\n", (int)level, where == NULL ? 0 : where->line,
	    where == NULL ? 0 : where->column, message);
	if (written > 0 && (size_t)written <
	    sizeof(diagnostics->text) - diagnostics->length)
		diagnostics->length += (size_t)written;
}

/*
 * How many diagnostics were recorded, one per line.
 */
static size_t
lines_in(const diagnostics_t *diagnostics)
{
	size_t lines;
	size_t i;

	lines = 0;
	for (i = 0; i < diagnostics->length; i++) {
		if (diagnostics->text[i] == '\n')
			lines++;
	}

	return (lines);
}

/*
 * A context with the given flags and limits holding a loaded program, or
 * NULL having recorded a failure.  Its diagnostics go to `diagnostics`.
 */
static relic_t *
context_for(const char *program, unsigned int flags, size_t max_tuples,
    size_t max_memory, diagnostics_t *diagnostics)
{
	relic_config_t config;
	relic_t *ctx;

	bzero(diagnostics, sizeof(*diagnostics));
	bzero(&config, sizeof(config));
	config.flags = flags;
	config.max_tuples = max_tuples;
	config.max_memory = max_memory;

	ctx = relic_create_config(&config);
	if (ctx == NULL) {
		check(false, "creating a context", "a context is created");
		return (NULL);
	}
	relic_diag_set(ctx, record_diagnostic, diagnostics);

	if (relic_load_string(ctx, "test.dl", program) != 0) {
		check(false, "loading a test program", "the program loads");
		relic_destroy(ctx);
		return (NULL);
	}

	return (ctx);
}

/*
 * Whether two contexts hold the same derived relations: each the same
 * tuples under the same identifiers, in storage of the same size.
 */
static bool
relations_agree(const relic_t *serial, const relic_t *deferred)
{
	const relic_relation_t *left;
	const relic_relation_t *right;
	size_t count;
	size_t i;

	count = serial->store.derived.count;
	if (deferred->store.derived.count > count)
		count = deferred->store.derived.count;

	for (i = 0; i < count; i++) {
		left = i < serial->store.derived.count ?
		    serial->store.derived.entries[i] : NULL;
		right = i < deferred->store.derived.count ?
		    deferred->store.derived.entries[i] : NULL;

		if (left == NULL || right == NULL) {
			if (left != right && relic_relation_count(left == NULL ?
			    right : left) > 0)
				return (false);
			continue;
		}

		if (left->arity != right->arity ||
		    left->count != right->count ||
		    relic_relation_bytes(left) != relic_relation_bytes(right))
			return (false);
		if (left->count > 0 && left->arity > 0 &&
		    bcmp(left->columns, right->columns, left->count *
		    left->arity * sizeof(*left->columns)) != 0)
			return (false);
	}

	return (true);
}

/*
 * The rounds a context's pool merged, and those it ran and left to be run
 * again serially.  A context that never deferred a round has no pool.
 */
static uint64_t
merged_rounds(const relic_t *ctx)
{
	return (ctx->pool != NULL ? ctx->pool->merged_rounds : 0);
}

static uint64_t
abandoned_rounds(const relic_t *ctx)
{
	return (ctx->pool != NULL ? ctx->pool->abandoned_rounds : 0);
}

/*
 * Whether two contexts agree on their relations and on every counter of
 * relic_stats_t, reporting the first difference found.  A failed
 * evaluation is held to the counters too: a merge is begun only once it is
 * known that no limit can refuse it, so a limit refuses only in a round run
 * serially, after the same work the serial evaluator did.
 */
static bool
contexts_agree(const char *scenario, const char *what, relic_t *serial,
    relic_t *deferred)
{
	relic_stats_t left;
	relic_stats_t right;

	if (!relations_agree(serial, deferred)) {
		checkf(false, scenario, "%s: the relations should agree",
		    what);
		return (false);
	}

	relic_stats(serial, &left);
	relic_stats(deferred, &right);
	if (left.tuples != right.tuples ||
	    left.storage_bytes != right.storage_bytes ||
	    left.iterations != right.iterations ||
	    left.tuples_inserted != right.tuples_inserted) {
		checkf(false, scenario, "%s: the relations' totals should "
		    "agree, were %zu %zu %" PRIu64 " %" PRIu64 " and %zu %zu "
		    "%" PRIu64 " %" PRIu64, what, left.tuples,
		    left.storage_bytes, left.iterations, left.tuples_inserted,
		    right.tuples, right.storage_bytes, right.iterations,
		    right.tuples_inserted);
		return (false);
	}

	if (left.derivations != right.derivations ||
	    left.tuples_scanned != right.tuples_scanned ||
	    left.tuples_looked_up != right.tuples_looked_up) {
		checkf(false, scenario, "%s: the counters of work should "
		    "agree, were %" PRIu64 " %" PRIu64 " %" PRIu64 " and "
		    "%" PRIu64 " %" PRIu64 " %" PRIu64, what,
		    left.derivations, left.tuples_scanned,
		    left.tuples_looked_up, right.derivations,
		    right.tuples_scanned, right.tuples_looked_up);
		return (false);
	}

	return (true);
}

/*
 * The row callback: append the row to `rows` as one line of values, each
 * its type and its identifier or integer.  The two contexts intern alike,
 * so a value has one rendering in both.
 */
static int
record_row(void *callback_ctx, const relic_value_t *values, size_t count)
{
	text_t *rows;
	size_t i;

	rows = callback_ctx;
	for (i = 0; i < count; i++)
		appendf(rows, "%d:%" PRId64 " ", (int)values[i].type,
		    values[i].type == RELIC_INTEGER ? values[i].v.integer :
		    (int64_t)values[i].v.id);
	appendf(rows, "\n");

	return (0);
}

/*
 * Run a query, recording its rows in the order they were delivered.
 */
static int
run_query(relic_t *ctx, const char *query, text_t *rows)
{
	relic_query_t *prepared;
	int status;

	rows->length = 0;
	rows->bytes[0] = '\0';

	prepared = relic_query_prepare(ctx, NULL, query);
	if (prepared == NULL)
		return (-1);

	status = relic_query_run(ctx, prepared, record_row, rows);
	relic_query_destroy(prepared);

	return (status);
}

/*
 * Materialize the graph program serially and with every round deferred,
 * under each planner and each fixed point, and compare the relations tuple
 * by tuple and every counter.  The naive fixed point's passes are never
 * deferred; its components that are not recursive are, and it is here for
 * those.
 */
static void
a_deferred_round_inserts_in_serial_order(void)
{
	static const struct {
		const char	*name;
		unsigned int	 flags;
	} strategies[] = {
		{ "the default planner", 0 },
		{ "the textual planner",
		    RELIC_F_NO_INDEX | RELIC_F_NO_REORDER },
		{ "the naive fixed point", RELIC_F_NAIVE },
	};
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_t *serial;
	relic_t *deferred;
	int serial_status;
	int deferred_status;
	size_t i;

	for (i = 0; i < sizeof(strategies) / sizeof(strategies[0]); i++) {
		serial = context_for(graph.bytes, strategies[i].flags, 0, 0,
		    &serial_diagnostics);
		deferred = context_for(graph.bytes, strategies[i].flags |
		    RELIC_F_PARALLEL_ALWAYS, 0, 0, &deferred_diagnostics);

		if (serial != NULL && deferred != NULL) {
			serial_status = relic_materialize(serial);
			deferred_status = relic_materialize(deferred);
			checkf(serial_status == 0 && deferred_status == 0,
			    SCENARIO_ORDER, "%s: both materialize",
			    strategies[i].name);
			checkf(merged_rounds(deferred) > 0, SCENARIO_ORDER,
			    "%s: rounds are merged", strategies[i].name);
			contexts_agree(SCENARIO_ORDER, strategies[i].name,
			    serial, deferred);
			checkf(deferred->account.round_bytes == 0,
			    SCENARIO_ORDER, "%s: the round gave back its "
			    "buffers", strategies[i].name);
		}

		relic_destroy(serial);
		relic_destroy(deferred);
	}
}

/*
 * Answer queries through their Magic rewrites, whose rounds run the seeds
 * the rewrite generates, and compare the rows in the order they came, the
 * relations and the counters.  A query that gathers its answer runs a
 * round of its own.
 */
static void
a_querys_rewrite_runs_deferred_rounds(void)
{
	static const char *const queries[] = {
		"path(n3, Y)?", "hop(n5, Y)?", "odd(Y)?", "pair(X, n4)?",
		"lonely(X)?", "done?", "path(X, Y), path(Y, X), X < Y?",
	};
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_t *serial;
	relic_t *deferred;
	int serial_status;
	int deferred_status;
	size_t i;

	for (i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
		serial = context_for(graph.bytes, 0, 0, 0,
		    &serial_diagnostics);
		deferred = context_for(graph.bytes, RELIC_F_PARALLEL_ALWAYS,
		    0, 0, &deferred_diagnostics);

		if (serial != NULL && deferred != NULL) {
			serial_status = run_query(serial, queries[i],
			    &serial_rows);
			deferred_status = run_query(deferred, queries[i],
			    &deferred_rows);
			checkf(serial_status == 0 && deferred_status == 0,
			    SCENARIO_REWRITE, "%s is answered", queries[i]);
			checkf(serial_rows.length > 0 &&
			    strcmp(serial_rows.bytes, deferred_rows.bytes) == 0,
			    SCENARIO_REWRITE, "%s: the rows come in one order",
			    queries[i]);
			checkf(merged_rounds(deferred) > 0, SCENARIO_REWRITE,
			    "%s: rounds are merged", queries[i]);
			contexts_agree(SCENARIO_REWRITE, queries[i], serial,
			    deferred);
		}

		relic_destroy(serial);
		relic_destroy(deferred);
	}
}

/*
 * Materialize a program under limits from what its facts need to more than
 * its evaluation does, in steps fine enough to land inside rounds and
 * inside merges, and require the deferred context to succeed or fail as the
 * serial one does, with the same diagnostic, leaving the same relations.  A
 * limit that refuses a deferred round's buffers abandons the round, which
 * runs again serially; one that would refuse its merge is found before the
 * merge begins.  The memory limit also counts the evaluation's plans, which
 * relic_stats_t does not report, so its sweep runs to twice the storage.
 *
 * The sweep must reach both ways a deferred round ends under a limit: a
 * round abandoned and run again serially, and a round merged.  Under the
 * memory limit it must also reach an abandoned round whose serial run then
 * succeeds, the case where the buffers' charge alone was in the way; under
 * the tuple limit none exists, since a round abandoned for its candidates
 * holds more new tuples than the limit admits.
 */
static void
limits_refuse_as_serially(const char *scenario, const char *program,
    bool memory)
{
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_stats_t loaded;
	relic_stats_t needed;
	relic_t *serial;
	relic_t *deferred;
	size_t first;
	size_t last;
	size_t limit;
	size_t step;
	int serial_status;
	int deferred_status;
	bool abandoned_seen;
	bool recovered_seen;
	bool merged_seen;
	char what[64];

	serial = context_for(program, 0, 0, 0, &serial_diagnostics);
	if (serial == NULL)
		return;
	relic_stats(serial, &loaded);
	if (relic_materialize(serial) != 0) {
		check(false, scenario, "the program materializes unlimited");
		relic_destroy(serial);
		return;
	}
	relic_stats(serial, &needed);
	relic_destroy(serial);

	first = memory ? loaded.storage_bytes : loaded.tuples;
	last = memory ? needed.storage_bytes * 2 : needed.tuples;
	step = (last - first) / LIMIT_STEPS;
	if (step == 0)
		step = 1;

	abandoned_seen = false;
	recovered_seen = false;
	merged_seen = false;

	for (limit = first; limit <= last; limit += step) {
		serial = context_for(program, 0, memory ? 0 : limit,
		    memory ? limit : 0, &serial_diagnostics);
		deferred = context_for(program, RELIC_F_PARALLEL_ALWAYS,
		    memory ? 0 : limit, memory ? limit : 0,
		    &deferred_diagnostics);
		if (serial == NULL || deferred == NULL) {
			relic_destroy(serial);
			relic_destroy(deferred);
			return;
		}

		serial_status = relic_materialize(serial);
		deferred_status = relic_materialize(deferred);

		snprintf(what, sizeof(what), "a limit of %zu", limit);
		checkf(serial_status == deferred_status, scenario,
		    "%s: both %s", what, serial_status == 0 ? "succeed" :
		    "fail");
		checkf(strcmp(serial_diagnostics.text,
		    deferred_diagnostics.text) == 0, scenario,
		    "%s: the diagnostics agree: \"%s\" and \"%s\"", what,
		    serial_diagnostics.text, deferred_diagnostics.text);
		contexts_agree(scenario, what, serial, deferred);
		checkf(deferred->account.round_bytes == 0, scenario,
		    "%s: the round gave back its buffers", what);

		if (abandoned_rounds(deferred) > 0) {
			abandoned_seen = true;
			if (deferred_status == 0)
				recovered_seen = true;
		}
		if (merged_rounds(deferred) > 0)
			merged_seen = true;

		relic_destroy(serial);
		relic_destroy(deferred);
	}

	check(abandoned_seen, scenario, "the sweep abandons a round");
	check(merged_seen, scenario, "the sweep merges a round");
	if (memory)
		check(recovered_seen, scenario, "the sweep abandons a round "
		    "whose serial run succeeds");
}

/*
 * RELIC_F_NOTE_MIXED's notes come from the merge in task order, once per
 * rule, as the serial evaluator reports them.  The two rules of `small`
 * each compare values of two types and are one component's pass, so one
 * round holds both and must note both, in program order; `large` is a
 * round of its own, noted after them.
 */
static void
notes_come_as_serially(void)
{
	static const char *const program =
	    "value(1). value(2). value(a). value(\"s\").\n"
	    "small(X) :- value(X), X < 2.\n"
	    "small(X) :- value(X), X < b.\n"
	    "large(X) :- value(X), X > 1.\n";
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_t *serial;
	relic_t *deferred;
	int serial_status;
	int deferred_status;

	serial = context_for(program, RELIC_F_NOTE_MIXED, 0, 0,
	    &serial_diagnostics);
	deferred = context_for(program, RELIC_F_NOTE_MIXED |
	    RELIC_F_PARALLEL_ALWAYS, 0, 0, &deferred_diagnostics);

	if (serial != NULL && deferred != NULL) {
		serial_status = relic_materialize(serial);
		deferred_status = relic_materialize(deferred);
		check(serial_status == 0 && deferred_status == 0,
		    SCENARIO_NOTES, "both materialize");
		checkf(lines_in(&serial_diagnostics) == 3, SCENARIO_NOTES,
		    "each of three rules is noted: \"%s\"",
		    serial_diagnostics.text);
		checkf(strcmp(serial_diagnostics.text,
		    deferred_diagnostics.text) == 0, SCENARIO_NOTES,
		    "the notes agree: \"%s\" and \"%s\"",
		    serial_diagnostics.text, deferred_diagnostics.text);
		checkf(merged_rounds(deferred) > 0, SCENARIO_NOTES,
		    "rounds are merged");
		contexts_agree(SCENARIO_NOTES, "the notes' program", serial,
		    deferred);
	}

	relic_destroy(serial);
	relic_destroy(deferred);
}

int
main(void)
{
	graph_program(&graph);

	a_deferred_round_inserts_in_serial_order();
	a_querys_rewrite_runs_deferred_rounds();
	limits_refuse_as_serially(SCENARIO_TUPLES, graph.bytes, false);
	limits_refuse_as_serially(SCENARIO_MEMORY, graph.bytes, true);
	notes_come_as_serially();

	return (check_summary("test_round"));
}
