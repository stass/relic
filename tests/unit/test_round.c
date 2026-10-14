/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
 * compared too, and so is the integer table, integer by integer: the
 * integers a round computes are interned by its merge, and must take the
 * identifiers the serial evaluator gives them.
 *
 * A deferred context whose rounds were all declined would pass every
 * comparison, being the serial evaluator, so each scenario also requires
 * that rounds were merged, and the limits' sweeps that rounds were
 * abandoned and run again serially.
 *
 * Every scenario runs twice: with the deferred context's rounds run by the
 * calling thread alone, and shared among DEFERRED_THREADS threads, whose
 * tasks are claimed in whatever order the threads reach them.  Neither may
 * differ from the serial context in anything but the rounds counted as
 * shared, which a build with threads must count at the larger size.
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
#define	SCENARIO_GROWTH		"the memory limit refuses a merge as serially"
#define	SCENARIO_NOTES		"notes come as the serial evaluator's do"
#define	SCENARIO_ERRORS		"an undefined result fails as serially"
#define	SCENARIO_COMPUTING	"a round computing integers is merged"
#define	SCENARIO_SUM		"an overflowing sum fails as serially"
#define	SCENARIO_STRINGS	"a round computing strings is merged"
#define	SCENARIO_PATTERN	"an invalid pattern fails as serially"
#define	SCENARIO_TEXT_TUPLES	"the tuple limit refuses strings as serially"
#define	SCENARIO_TEXT_MEMORY	"the memory limit refuses strings as serially"
#define	SCENARIO_HISTORY	"a round reserves what its items inserted"
#define	SCENARIO_COMPACT	"a merge resolves each local value once"
#define	SCENARIO_DRIVER		"a lookup drives a shared round"
#define	SCENARIO_DELTA_DRIVER	"a delta lookup drives its rounds"

/* Room for a generated program and for a context's diagnostics. */
#define	PROGRAM_MAX		(1024 * 1024)
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

/*
 * The links of the wide program's chain, the columns of the tuple each of
 * its rounds derives, and the steps its memory limit is swept in.
 */
#define	WIDE_LINKS		100
#define	WIDE_ARITY		16
#define	WIDE_LIMIT_STEPS	400

/*
 * The most a program of these tests holds beside its storage while it is
 * evaluated: a round computing strings holds a megabyte of pending bytes.
 */
#define	WORKING_MAX		(4u * 1024u * 1024u)

/* The buffers each worker of a round computing strings holds from its start. */
#define	WORKER_TEXTS		(RELIC_TEXT_STACK_INITIAL + RELIC_PENDING_BYTES)

/* The threads the deferred context shares its rounds among, the second time. */
#define	DEFERRED_THREADS	4
#define	MANY_THREADS		16

/*
 * The links of the chain whose closure gains one tuple a round; the links
 * of a broom's handle, along which the walk's rounds are too small to
 * share; and the leaves its end fans out to, every number of
 * BROOM_DIGITS decimal digits, more than any round's work below the
 * threshold of sharing.
 */
#define	HISTORY_CHAIN		50u
#define	BROOM_HANDLE		10u
#define	BROOM_DIGITS		5u
#define	BROOM_LEAVES		100000u

/*
 * The facts of the program whose one round holds few pending values in
 * many columns; the new directory and extension strings and the new
 * integers its tuples hold, each twice in a tuple; and the pending columns
 * of each tuple it derives.
 */
#define	COMPACT_FILES		10000u
#define	COMPACT_DIRS		7u
#define	COMPACT_EXTS		3u
#define	COMPACT_NUMBERS		5u
#define	COMPACT_NUMBER_BASE	7000000
#define	COMPACT_PENDING		6u

/* The bytes of each constant the string program's long strings repeat. */
#define	LONG_STRING_BYTES	3000u

/*
 * The facts row(A, V, 7) of the join a lookup drives, each of its own V,
 * and the facts row(B, V, 13) it joins for each V.
 */
#define	DRIVER_MATCHES		3000u
#define	DRIVER_FAN		8u

/* The layers of the graph the delta lookups walk, and its nodes a layer. */
#define	DELTA_LAYERS		12u
#define	DELTA_WIDTH		8u

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

static void	 a_deferred_round_inserts_in_serial_order(size_t threads);
static void	 a_delta_lookup_drives_its_rounds(size_t threads);
static void	 a_lookup_drives_a_shared_round(size_t threads);
static void	 a_querys_rewrite_runs_deferred_rounds(size_t threads);
static uint64_t	 expected_candidates(const relic_t *ctx);
static uint64_t	 reservation_declines(const relic_t *ctx);
static void	 rounds_reserve_what_deferred_rounds_inserted(size_t threads);
static void	 rounds_reserve_what_serial_rounds_inserted(void);
static void	 appendf(text_t *text, const char *format, ...)
		    RELIC_TEST_PRINTFLIKE(2, 3);
static relic_t	*context_for(const char *program, unsigned int flags,
		    size_t threads, size_t max_tuples, size_t max_memory,
		    diagnostics_t *diagnostics);
static uint64_t	 abandoned_rounds(const relic_t *ctx);
static uint64_t	 limit_declines(const relic_t *ctx);
static bool	 contexts_agree(const char *scenario, const char *what,
		    relic_t *serial, relic_t *deferred);
static void	 graph_program(text_t *text);
static size_t	 lines_in(const diagnostics_t *diagnostics);
static void	 limits_refuse_as_serially(const char *scenario,
		    const char *program, bool memory, size_t steps,
		    size_t threads);
static size_t	 least_memory_limit(const char *program, bool merging,
		    size_t low, size_t high);
static bool	 materializes_under(const char *program, bool merging,
		    size_t limit);
static void	 compact_program(text_t *text);
static size_t	 compact_values_held(const relic_t *ctx);
static void	 compacted_rounds_agree(size_t threads, size_t max_memory);
static void	 computing_rounds_merge(size_t threads);
static void	 pending_values_resolve_once(void);
static void	 errors_fail_as_serially(size_t threads);
static void	 sums_overflow_as_serially(size_t threads);
static bool	 values_agree(const relic_t *serial, const relic_t *deferred);
static void	 byte_string_rules(text_t *text);
static void	 string_program(text_t *text);
static void	 strings_rounds_merge(size_t threads);
static void	 patterns_fail_as_serially(size_t threads);
static void	 notes_come_as_serially(size_t threads);
static bool	 shared_as_expected(relic_t *deferred, size_t threads);
static void	 wide_program(text_t *text);
static uint64_t	 thread_tasks(const relic_t *ctx);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static void	 record_explained(void *callback_ctx, const char *line);
static int	 record_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static uint64_t	 merged_rounds(const relic_t *ctx);
static bool	 relations_agree(const relic_t *serial,
		    const relic_t *deferred);
static int	 run_query(relic_t *ctx, const char *query, text_t *rows);

/*
 * The program every scenario but the notes', the errors' and the computing
 * rounds' runs, and the program whose merges the memory limit refuses,
 * written once by main(); the program an undefined result fails, or whose
 * every round computes; and the rows of the two runs of a query.
 */
static text_t graph;
static text_t wide;
static text_t strings;
static text_t failing;
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
 *	reach/1 from a constant, whose plan is driven by a lookup and is cut
 *	along its chain; pair/2 over two derivations of one relation filtered
 *	by a comparison, with many duplicates across the items of one round;
 *	lonely/1 behind a negation, and done/0 of no columns;
 *	span/1, total/1, both/2, near/2 and dist/3 over idx/2, a number for
 *	each node, computing integers: span/1 one the value table lacks for
 *	each edge, total/1 few of them each derived many times over, so that
 *	several workers compute one integer in one round, both/2 two in one
 *	tuple, near/2 integers the table holds, and dist/3 new integers round
 *	by round of a recursion;
 *	outdeg/2, reached/2, weight/2, closest/2 and widest/1 folding groups:
 *	a count keyed on each node, a count and a sum over the closure, whose
 *	groups a round derives in many pieces, a min over a recursion's
 *	integers, and a max with no key over another fold's results.
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
		appendf(text, "idx(n%u, %u).\n", i, i);
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
	    "done :- lonely(X).\n"
	    "span(S) :- idx(X, I), edge(X, Y), idx(Y, J), S = I * 1000 + J.\n"
	    "total(S) :- idx(X, I), idx(Y, J), S = I + J + 1000.\n"
	    "both(A, B) :- edge(X, Y), idx(X, I), idx(Y, J), A = I + 2000,\n"
	    "    B = J + 2000.\n"
	    "near(X, K) :- idx(X, I), K = I + 1.\n"
	    "dist(X, Y, 1000) :- edge(X, Y).\n"
	    "dist(X, Z, D) :- dist(X, Y, E), edge(Y, Z), E < 1008,\n"
	    "    D = E + 1.\n"
	    "outdeg(X, N) :- node(X), N = count { edge(X, Y) }.\n"
	    "reached(X, N) :- node(X), N = count { path(X, Y) }.\n"
	    "weight(X, S) :- node(X), S = sum(I) { path(X, Y), idx(Y, I) }.\n"
	    "closest(X, D) :- idx(X, _), D = min(E) { dist(X, n0, E) }.\n"
	    "widest(M) :- M = max(N) { reached(X, N) }.\n");
}

/*
 * A program each of whose rounds derives one tuple of WIDE_ARITY columns,
 * the next along a chain of WIDE_LINKS links.  A round's buffers then hold
 * one candidate, while the relation's columns double as they fill, so at a
 * doubling the growth of the round's merge outweighs everything the round
 * has charged, and a memory limit between the two refuses the growth that
 * the serial evaluator is refused too.  The graph program's rounds, whose
 * buffers outweigh their growth, have their merges declined where the
 * buffers are what the limit refuses, where the serial evaluator, which
 * has none, is not refused.
 */
static void
wide_program(text_t *text)
{
	unsigned int i;

	text->length = 0;
	text->bytes[0] = '\0';

	for (i = 0; i < WIDE_LINKS; i++)
		appendf(text, "link(n%u, n%u).\n", i, i + 1);

	appendf(text, "wide(n0");
	for (i = 1; i < WIDE_ARITY; i++)
		appendf(text, ", c%u", i);
	appendf(text, ").\n");

	appendf(text, "wide(Y");
	for (i = 1; i < WIDE_ARITY; i++)
		appendf(text, ", C%u", i);
	appendf(text, ") :- wide(X");
	for (i = 1; i < WIDE_ARITY; i++)
		appendf(text, ", C%u", i);
	appendf(text, "), link(X, Y).\n");
}

/*
 * A program whose every round computes strings: one new string per fact
 * (label/2), a few new ones derived many times over in one round (pair/1),
 * strings the pool holds as a name or a symbol's spelling (spelled/2), the
 * empty string, which has no bytes to hold (blank/2), new ones round by
 * round of a recursion (walk/2), and a pattern computed per binding
 * (hit/1).
 */
static void
string_program(text_t *text)
{
	unsigned int i;

	text->length = 0;
	text->bytes[0] = '\0';

	for (i = 0; i < GRAPH_NODES; i++) {
		appendf(text, "idx(%u).\n", i);
		appendf(text, "step(%u, %u).\n", i, (i * 7 + 3) % GRAPH_NODES);
	}
	appendf(text,
	    "label(I, S) :- idx(I), S = cat(\"n\", str(I)).\n"
	    "pair(S) :- idx(I), idx(J), "
	    "S = cat(str(I mod 3), cat(\"-\", str(J mod 5))).\n"
	    "spelled(I, S) :- idx(I), I < 5, S = str(label).\n"
	    "blank(I, S) :- idx(I), I < 3, S = substr(str(I), 5).\n"
	    "walk(X, P) :- idx(X), X < 3, P = str(X).\n"
	    "walk(Y, Q) :- walk(X, P), step(X, Y), length(P) < 24, "
	    "Q = cat(P, cat(\"/\", str(Y))).\n"
	    "hit(I) :- label(I, S), match(S, cat(\"^n\", str(I mod 10))) = 0.\n");
}

/*
 * Add to the string program rules computing strings whose bytes only
 * byte-for-byte equality tells apart: cut/2 strings holding a NUL byte,
 * each the prefix of the next -- "", "z", "z\0" and "z\0" with a digit --
 * and long/2 two strings of twice LONG_STRING_BYTES bytes and more, each
 * derived several times.  The limits' sweeps run the string program
 * without them, the band of limits they search for being narrow.
 */
static void
byte_string_rules(text_t *text)
{
	unsigned int i;
	unsigned int j;

	appendf(text,
	    "cut(I, S) :- idx(I), S = substr(cat(\"z\\0\", str(I)), 0, "
	    "I mod 4).\n");

	appendf(text, "long(I, S) :- idx(I), I < 6, S = ");
	for (j = 0; j < 2; j++) {
		appendf(text, "cat(\"");
		for (i = 0; i < LONG_STRING_BYTES; i++)
			appendf(text, "%c", j == 0 ? 'x' : 'y');
		appendf(text, "\", ");
	}
	appendf(text, "str(I mod 2))).\n");
}

/*
 * A round whose plans compute strings the context lacks is merged, not
 * abandoned, and the merge interns them as the serial run does: in the
 * same order, under the same identifiers, into a pool of the same size.
 */
static void
strings_rounds_merge(size_t threads)
{
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_stats_t loaded;
	relic_stats_t computed;
	relic_t *serial;
	relic_t *deferred;
	int serial_status;
	int deferred_status;
	char what[64];

	string_program(&failing);
	byte_string_rules(&failing);
	snprintf(what, sizeof(what), "strings on %zu thread(s)", threads);
	serial = context_for(failing.bytes, 0, 0, 0, 0, &serial_diagnostics);
	deferred = context_for(failing.bytes, RELIC_F_PARALLEL_ALWAYS, threads,
	    0, 0, &deferred_diagnostics);

	if (serial != NULL && deferred != NULL) {
		relic_stats(deferred, &loaded);
		serial_status = relic_materialize(serial);
		deferred_status = relic_materialize(deferred);
		relic_stats(deferred, &computed);
		checkf(serial_status == 0 && deferred_status == 0,
		    SCENARIO_STRINGS, "%s: both materialize", what);
		checkf(merged_rounds(deferred) > 0 &&
		    abandoned_rounds(deferred) == 0, SCENARIO_STRINGS,
		    "%s: every round is merged, %" PRIu64 " merged and %"
		    PRIu64 " abandoned", what, merged_rounds(deferred),
		    abandoned_rounds(deferred));
		checkf(computed.string_values > loaded.string_values + 60,
		    SCENARIO_STRINGS, "%s: the merges interned strings, "
		    "%zu before and %zu after", what, loaded.string_values,
		    computed.string_values);
		checkf(shared_as_expected(deferred, threads),
		    SCENARIO_STRINGS, "%s: the rounds are shared when the "
		    "pool has threads", what);
		contexts_agree(SCENARIO_STRINGS, what, serial, deferred);
		checkf(deferred->account.round_bytes == 0, SCENARIO_STRINGS,
		    "%s: the rounds gave back their buffers", what);
	}

	relic_destroy(serial);
	relic_destroy(deferred);
}

/*
 * A deferred round expects to gather what its items inserted the last
 * time each ran, as its merge counted them.  The closure of a chain from
 * one node gains one tuple a round, the round of its last link none: the
 * recursive rule's first round has run nothing before and expects
 * nothing, and each round after it expects the one tuple the round before
 * inserted, HISTORY_CHAIN rounds in all.  The seed's round has no plan to
 * keep a history for.
 */
static void
rounds_reserve_what_deferred_rounds_inserted(size_t threads)
{
	static text_t program;
	diagnostics_t diagnostics;
	relic_t *deferred;
	char what[64];
	unsigned int i;

	program.length = 0;
	program.bytes[0] = '\0';
	for (i = 0; i < HISTORY_CHAIN; i++)
		appendf(&program, "succ(%u, %u).\n", i, i + 1);
	appendf(&program,
	    "reach(0).\n"
	    "reach(Y) :- reach(X), succ(X, Y).\n");

	snprintf(what, sizeof(what), "a chain on %zu thread(s)", threads);
	deferred = context_for(program.bytes, RELIC_F_PARALLEL_ALWAYS, threads,
	    0, 0, &diagnostics);
	if (deferred != NULL) {
		checkf(relic_materialize(deferred) == 0, SCENARIO_HISTORY,
		    "%s: it materializes", what);
		checkf(merged_rounds(deferred) > HISTORY_CHAIN &&
		    abandoned_rounds(deferred) == 0, SCENARIO_HISTORY,
		    "%s: every round is merged, %" PRIu64 " merged and %"
		    PRIu64 " abandoned", what, merged_rounds(deferred),
		    abandoned_rounds(deferred));
		checkf(expected_candidates(deferred) == HISTORY_CHAIN,
		    SCENARIO_HISTORY, "%s: the rounds expected %u candidates, "
		    "not %" PRIu64, what, HISTORY_CHAIN,
		    expected_candidates(deferred));
	}

	relic_destroy(deferred);
}

/*
 * A round run serially records what it inserted as a deferred round does.
 * A broom -- a handle of BROOM_HANDLE links whose end fans out to
 * BROOM_LEAVES leaves -- is walked on a context with threads and without
 * RELIC_F_PARALLEL_ALWAYS: the walk's rounds along the handle are too small
 * to share and run serially, the round that reaches the fan inserts every
 * leaf serially, being predicted as small as the rounds before it, and the
 * round after it, which reads the leaves, is shared, and expects the
 * leaves its item inserted serially.  The rule copying the leaves into the
 * broom, whose first view is every leaf, is shared too, and having no
 * history expects nothing; so the rounds' expectations sum to the leaves
 * only if the round reading them took its expectation from the serial
 * round before it.  A build without threads shares no round.
 */
static void
rounds_reserve_what_serial_rounds_inserted(void)
{
	static text_t program;
	diagnostics_t diagnostics;
	relic_t *ctx;
	unsigned int i;

	_Static_assert(BROOM_LEAVES == 10u * 10u * 10u * 10u * 10u &&
	    BROOM_DIGITS == 5u, "the leaves are every number of five digits");

	program.length = 0;
	program.bytes[0] = '\0';
	for (i = 0; i < 10; i++)
		appendf(&program, "digit(%u).\n", i);
	for (i = 0; i < BROOM_HANDLE; i++)
		appendf(&program, "succ(%u, %u).\n", i, i + 1);
	appendf(&program,
	    "leaf(X) :- digit(A), digit(B), digit(C), digit(D), digit(E), "
	    "X = 1000000 + A * 10000 + B * 1000 + C * 100 + D * 10 + E.\n"
	    "succ(%u, X) :- leaf(X).\n"
	    "reach(0).\n"
	    "reach(Y) :- reach(X), succ(X, Y).\n", BROOM_HANDLE);

	ctx = context_for(program.bytes, 0, DEFERRED_THREADS, 0, 0,
	    &diagnostics);
	if (ctx != NULL) {
		checkf(relic_materialize(ctx) == 0, SCENARIO_HISTORY,
		    "a broom: it materializes");
		if (relic_pool_size(DEFERRED_THREADS) > 1) {
			checkf(merged_rounds(ctx) > 0, SCENARIO_HISTORY,
			    "a broom: rounds are shared");
			checkf(expected_candidates(ctx) == BROOM_LEAVES,
			    SCENARIO_HISTORY, "a broom: the shared rounds "
			    "expected %u candidates, not %" PRIu64,
			    BROOM_LEAVES, expected_candidates(ctx));
		} else {
			checkf(merged_rounds(ctx) == 0, SCENARIO_HISTORY,
			    "a broom: no round is shared without threads");
		}
	}

	relic_destroy(ctx);
}

/*
 * A round whose plan starts with a lookup is shared on its work, without
 * RELIC_F_PARALLEL_ALWAYS:
 *
 *	pair(A, B) :- row(A, V, 7), row(B, V, 13).
 *
 * over DRIVER_MATCHES facts row(A, V, 7), each of its own V, and
 * DRIVER_FAN facts row(B, V, 13) for each V.  The plan looks both literals
 * up, the first on its constant and the second on V and its constant, so
 * the round has no scan to cut; it is cut along the first lookup's
 * matches into several tasks, and derives what the serial evaluator
 * derives, tuple for tuple, counting the same reads.  A pool of one worker
 * shares nothing.
 */
static void
a_lookup_drives_a_shared_round(size_t threads)
{
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	diagnostics_t explained;
	relic_stats_t stats;
	relic_t *serial;
	relic_t *deferred;
	const char *at;
	size_t lookups;
	char what[64];
	unsigned int i;
	unsigned int k;

	failing.length = 0;
	failing.bytes[0] = '\0';
	for (i = 0; i < DRIVER_MATCHES; i++) {
		appendf(&failing, "row(%u,%u,7).\n", i, i);
		for (k = 0; k < DRIVER_FAN; k++)
			appendf(&failing, "row(%u,%u,13).\n",
			    i * DRIVER_FAN + k, i);
	}
	appendf(&failing, "pair(A, B) :- row(A, V, 7), row(B, V, 13).\n");

	snprintf(what, sizeof(what), "a join on %zu thread(s)", threads);
	serial = context_for(failing.bytes, 0, 0, 0, 0, &serial_diagnostics);
	deferred = context_for(failing.bytes, 0, threads, 0, 0,
	    &deferred_diagnostics);
	if (serial != NULL && deferred != NULL) {
		bzero(&explained, sizeof(explained));
		relic_explain_set(deferred, record_explained, &explained);
		checkf(relic_materialize(serial) == 0 &&
		    relic_materialize(deferred) == 0, SCENARIO_DRIVER,
		    "%s: both materialize", what);

		lookups = 0;
		for (at = explained.text; (at = strstr(at, "lookup row(")) !=
		    NULL; at++)
			lookups++;
		checkf(lookups == 2 && strstr(explained.text, "scan ") == NULL,
		    SCENARIO_DRIVER, "%s: both literals are looked up: \"%s\"",
		    what, explained.text);

		if (relic_pool_size(threads) > 1) {
			checkf(merged_rounds(deferred) > 0 &&
			    abandoned_rounds(deferred) == 0, SCENARIO_DRIVER,
			    "%s: the join is merged, %" PRIu64 " merged and %"
			    PRIu64 " abandoned", what, merged_rounds(deferred),
			    abandoned_rounds(deferred));
			checkf(deferred->pool->lookup_slices > 1,
			    SCENARIO_DRIVER, "%s: the lookup is cut into "
			    "slices, %" PRIu64 " of them", what,
			    deferred->pool->lookup_slices);
		} else {
			checkf(merged_rounds(deferred) == 0, SCENARIO_DRIVER,
			    "%s: no round is shared on one worker", what);
		}
		checkf(shared_as_expected(deferred, threads), SCENARIO_DRIVER,
		    "%s: the rounds are shared when the pool has threads",
		    what);
		relic_stats(deferred, &stats);
		checkf(stats.tuples == DRIVER_MATCHES * (1 + DRIVER_FAN) +
		    DRIVER_MATCHES * DRIVER_FAN, SCENARIO_DRIVER,
		    "%s: the join derives %u pairs, not %zu", what,
		    DRIVER_MATCHES * DRIVER_FAN,
		    stats.tuples - DRIVER_MATCHES * (1 + DRIVER_FAN));
		contexts_agree(SCENARIO_DRIVER, what, serial, deferred);
		checkf(deferred->account.round_bytes == 0, SCENARIO_DRIVER,
		    "%s: the rounds gave back their buffers", what);
	}

	relic_destroy(serial);
	relic_destroy(deferred);
}

/*
 * The rounds of a recursion whose plans are driven by a lookup of a delta,
 * and rounds whose plans test a fact before their driver, are shared under
 * RELIC_F_PARALLEL_ALWAYS and derive what the serial evaluator does:
 *
 *	s(7, Y) :- flag(1), s(7, X), next(X, Y).
 *	s(8, Y) :- flag(1), s(8, X), next(X, Y).
 *	some(Y) :- flag(1), s(8, Y).
 *	none(Y) :- flag(2), s(7, Y).
 *
 * over a layered graph next/2 of DELTA_LAYERS layers of DELTA_WIDTH nodes,
 * every node joined to every node of the next layer, both walks seeded at
 * the first layer.  Each round of the recursion looks its delta up on 7 or
 * on 8, whose chains are interleaved, so each chain's visits begin at its
 * first tuple at or past the delta's start, found from the key's frontier.
 * some/1's plan tests flag(1), which holds, before its lookup, and none/1's
 * flag(2), which does not, so its round has no visit to divide and derives
 * nothing.  A pool of one worker runs the same tasks on the calling thread.
 */
static void
a_delta_lookup_drives_its_rounds(size_t threads)
{
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_t *serial;
	relic_t *deferred;
	char what[64];
	unsigned int layer;
	unsigned int from;
	unsigned int to;

	failing.length = 0;
	failing.bytes[0] = '\0';
	for (layer = 0; layer + 1 < DELTA_LAYERS; layer++) {
		for (from = 0; from < DELTA_WIDTH; from++) {
			for (to = 0; to < DELTA_WIDTH; to++)
				appendf(&failing, "next(l%u_%u, l%u_%u).\n",
				    layer, from, layer + 1, to);
		}
	}
	for (from = 0; from < DELTA_WIDTH; from++)
		appendf(&failing, "s(7, l0_%u).\ns(8, l0_%u).\n", from, from);
	appendf(&failing,
	    "flag(1).\n"
	    "s(7, Y) :- flag(1), s(7, X), next(X, Y).\n"
	    "s(8, Y) :- flag(1), s(8, X), next(X, Y).\n"
	    "some(Y) :- flag(1), s(8, Y).\n"
	    "none(Y) :- flag(2), s(7, Y).\n");

	snprintf(what, sizeof(what), "delta lookups on %zu thread(s)",
	    threads);
	serial = context_for(failing.bytes, 0, 0, 0, 0, &serial_diagnostics);
	deferred = context_for(failing.bytes, RELIC_F_PARALLEL_ALWAYS,
	    threads, 0, 0, &deferred_diagnostics);
	if (serial != NULL && deferred != NULL) {
		checkf(relic_materialize(serial) == 0 &&
		    relic_materialize(deferred) == 0, SCENARIO_DELTA_DRIVER,
		    "%s: both materialize", what);
		checkf(merged_rounds(deferred) > 0 &&
		    abandoned_rounds(deferred) == 0 &&
		    deferred->pool->lookup_slices > 1, SCENARIO_DELTA_DRIVER,
		    "%s: the rounds are merged and their lookups cut, %" PRIu64
		    " merged, %" PRIu64 " abandoned and %" PRIu64 " slices",
		    what, merged_rounds(deferred), abandoned_rounds(deferred),
		    deferred->pool->lookup_slices);
		checkf(shared_as_expected(deferred, threads),
		    SCENARIO_DELTA_DRIVER, "%s: the rounds are shared when the "
		    "pool has threads", what);
		contexts_agree(SCENARIO_DELTA_DRIVER, what, serial, deferred);
		checkf(deferred->account.round_bytes == 0,
		    SCENARIO_DELTA_DRIVER, "%s: the rounds gave back their "
		    "buffers", what);
	}

	relic_destroy(serial);
	relic_destroy(deferred);
}

/*
 * A pattern that is not a regular expression, met by a task, abandons its
 * round, which runs again serially and reports it as the serial run does;
 * the rounds before it are merged, and the strings the failing round's
 * earlier batches interned are interned alike.  A query that matches is
 * then answered alike on what the failure left.
 */
static void
patterns_fail_as_serially(size_t threads)
{
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_t *serial;
	relic_t *deferred;
	int serial_status;
	int deferred_status;
	char what[64];
	unsigned int i;

	failing.length = 0;
	failing.bytes[0] = '\0';
	for (i = 0; i < 600; i++)
		appendf(&failing, "num(%u).\n", i);
	appendf(&failing,
	    "name(I, S) :- num(I), S = cat(\"x\", str(I)).\n"
	    "found(I, N) :- name(I, S), "
	    "N = match(S, cat(\"x\", substr(\"(\", 0, I - 450))).\n"
	    "fine(I) :- name(I, S), match(S, \"7$\") >= 0.\n");

	snprintf(what, sizeof(what), "patterns on %zu thread(s)", threads);
	serial = context_for(failing.bytes, 0, 0, 0, 0, &serial_diagnostics);
	deferred = context_for(failing.bytes, RELIC_F_PARALLEL_ALWAYS, threads,
	    0, 0, &deferred_diagnostics);

	if (serial != NULL && deferred != NULL) {
		serial_status = relic_materialize(serial);
		deferred_status = relic_materialize(deferred);
		checkf(serial_status != 0 && deferred_status != 0,
		    SCENARIO_PATTERN, "%s: both fail", what);
		checkf(strstr(serial_diagnostics.text,
		    "invalid regular expression in rule found/2: unmatched (") !=
		    NULL, SCENARIO_PATTERN, "%s: the serial run names the "
		    "rule and the reason: %s", what, serial_diagnostics.text);
		checkf(strcmp(serial_diagnostics.text,
		    deferred_diagnostics.text) == 0, SCENARIO_PATTERN,
		    "%s: the diagnostics agree: \"%s\" and \"%s\"", what,
		    serial_diagnostics.text, deferred_diagnostics.text);
		checkf(abandoned_rounds(deferred) > 0, SCENARIO_PATTERN,
		    "%s: the failing round is abandoned", what);
		contexts_agree(SCENARIO_PATTERN, what, serial, deferred);
		checkf(deferred->account.round_bytes == 0, SCENARIO_PATTERN,
		    "%s: the round gave back its buffers", what);
	}

	relic_destroy(serial);
	relic_destroy(deferred);
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
 * Keep a line of a plan explained, one per line, as long as it fits.
 */
static void
record_explained(void *callback_ctx, const char *line)
{
	diagnostics_t *explained;
	int written;

	explained = callback_ctx;
	written = snprintf(explained->text + explained->length,
	    sizeof(explained->text) - explained->length, "%s\n", line);
	if (written > 0 && (size_t)written <
	    sizeof(explained->text) - explained->length)
		explained->length += (size_t)written;
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
 * A context with the given flags, threads and limits holding a loaded
 * program, or NULL having recorded a failure.  Its diagnostics go to
 * `diagnostics`.
 */
static relic_t *
context_for(const char *program, unsigned int flags, size_t threads,
    size_t max_tuples, size_t max_memory, diagnostics_t *diagnostics)
{
	relic_config_t config;
	relic_t *ctx;

	bzero(diagnostics, sizeof(*diagnostics));
	bzero(&config, sizeof(config));
	config.flags = flags;
	config.threads = threads;
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
		    relic_relation_count(left) != relic_relation_count(right) ||
		    relic_relation_bytes(left) != relic_relation_bytes(right))
			return (false);
		if (relic_relation_count(left) > 0 && left->arity > 0 &&
		    bcmp(left->payload.columns, right->payload.columns,
		    relic_relation_count(left) * left->arity *
		    sizeof(*left->payload.columns)) != 0)
			return (false);
	}

	return (true);
}

/*
 * Whether two contexts hold the same integers and the same pool, byte
 * string by byte string under the same identifiers, in a table and a pool
 * of the same size.
 */
static bool
values_agree(const relic_t *serial, const relic_t *deferred)
{
	const relic_valtab_t *left;
	const relic_valtab_t *right;
	const char *left_bytes;
	const char *right_bytes;
	size_t left_length;
	size_t right_length;
	uint32_t id;

	left = &serial->values;
	right = &deferred->values;
	if (left->integer_count != right->integer_count ||
	    serial->symbols.count != deferred->symbols.count ||
	    relic_value_storage_bytes(serial) !=
	    relic_value_storage_bytes(deferred))
		return (false);

	for (id = 1; id <= serial->symbols.count; id++) {
		left_bytes = relic_symbol_bytes(serial, id, &left_length);
		right_bytes = relic_symbol_bytes(deferred, id, &right_length);
		if (left_length != right_length || (left_length > 0 &&
		    bcmp(left_bytes, right_bytes, left_length) != 0))
			return (false);
	}

	return (left->integer_count == 0 || bcmp(left->integers,
	    right->integers, left->integer_count * sizeof(*left->integers)) ==
	    0);
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
 * The merges a context's pool declined because a limit would have refused
 * what they insert.
 */
static uint64_t
limit_declines(const relic_t *ctx)
{
	return (ctx->pool != NULL ? ctx->pool->limit_declines : 0);
}

/*
 * The candidates a context's pool expected its rounds to gather, from what
 * their items inserted the last time each ran.
 */
static uint64_t
expected_candidates(const relic_t *ctx)
{
	return (ctx->pool != NULL ? ctx->pool->expected_candidates : 0);
}

/*
 * The rounds a context's pool declined because the room for their
 * candidates was refused.
 */
static uint64_t
reservation_declines(const relic_t *ctx)
{
	return (ctx->pool != NULL ? ctx->pool->reservation_declines : 0);
}

/*
 * Whether a deferred context counted its merged rounds as shared exactly
 * when it had threads to share them with: every one of them on a pool of
 * more than one worker, and none on the calling thread alone, where no
 * task can have run on a thread of the pool.  A build without threads has
 * one worker whatever it is configured with.
 */
static bool
shared_as_expected(relic_t *deferred, size_t threads)
{
	relic_stats_t stats;

	relic_stats(deferred, &stats);
	if (relic_pool_size(threads) > 1)
		return (stats.parallel_rounds == merged_rounds(deferred));

	return (stats.parallel_rounds == 0 && thread_tasks(deferred) == 0);
}

/*
 * The tasks of a context's merged rounds that a thread of its pool ran
 * rather than the calling thread.
 */
static uint64_t
thread_tasks(const relic_t *ctx)
{
	return (ctx->pool != NULL ? ctx->pool->thread_tasks : 0);
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

	if (!values_agree(serial, deferred)) {
		checkf(false, scenario, "%s: the integer tables and the pools "
		    "should agree", what);
		return (false);
	}

	relic_stats(serial, &left);
	relic_stats(deferred, &right);
	if (left.integer_values != right.integer_values ||
	    left.string_values != right.string_values) {
		checkf(false, scenario, "%s: the values counted should "
		    "agree, were %zu and %zu integers and %zu and %zu strings",
		    what, left.integer_values, right.integer_values,
		    left.string_values, right.string_values);
		return (false);
	}
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
 *
 * That the pool's threads ran tasks is asked of the three strategies
 * together.  The naive fixed point defers few rounds, and on a loaded
 * machine the calling thread can claim every task of them before a parked
 * thread wakes; over every round of the three, only threads that never run
 * leave the count at zero.
 */
static void
a_deferred_round_inserts_in_serial_order(size_t threads)
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
	char what[96];
	uint64_t threaded;
	size_t i;

	threaded = 0;
	for (i = 0; i < sizeof(strategies) / sizeof(strategies[0]); i++) {
		snprintf(what, sizeof(what), "%s on %zu thread(s)",
		    strategies[i].name, threads);
		serial = context_for(graph.bytes, strategies[i].flags, 0, 0, 0,
		    &serial_diagnostics);
		deferred = context_for(graph.bytes, strategies[i].flags |
		    RELIC_F_PARALLEL_ALWAYS, threads, 0, 0,
		    &deferred_diagnostics);

		if (serial != NULL && deferred != NULL) {
			serial_status = relic_materialize(serial);
			deferred_status = relic_materialize(deferred);
			checkf(serial_status == 0 && deferred_status == 0,
			    SCENARIO_ORDER, "%s: both materialize", what);
			checkf(merged_rounds(deferred) > 0, SCENARIO_ORDER,
			    "%s: rounds are merged", what);
			checkf(shared_as_expected(deferred, threads),
			    SCENARIO_ORDER, "%s: the rounds are shared when "
			    "the pool has threads", what);
			threaded += thread_tasks(deferred);
			contexts_agree(SCENARIO_ORDER, what, serial, deferred);
			checkf(deferred->account.round_bytes == 0,
			    SCENARIO_ORDER, "%s: the round gave back its "
			    "buffers", what);
		}

		relic_destroy(serial);
		relic_destroy(deferred);
	}

	checkf(relic_pool_size(threads) < 2 || threaded > 0,
	    SCENARIO_ORDER, "on %zu thread(s) the pool's threads run some of "
	    "the tasks", threads);
}

/*
 * Answer queries through their Magic rewrites, whose rounds run the seeds
 * the rewrite generates, and compare the rows in the order they came, the
 * relations and the counters.  A query that gathers its answer runs a
 * round of its own, and so does one holding an aggregate, after the round
 * that derives its group.
 */
static void
a_querys_rewrite_runs_deferred_rounds(size_t threads)
{
	static const char *const queries[] = {
		"path(n3, Y)?", "hop(n5, Y)?", "odd(Y)?", "pair(X, n4)?",
		"lonely(X)?", "done?", "path(X, Y), path(Y, X), X < Y?",
		"dist(n3, Y, D)?", "total(S), S > 1070?",
		"weight(n2, S)?", "closest(n4, D)?",
		"node(X), N = count { path(X, Y), edge(Y, X) }?",
	};
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_t *serial;
	relic_t *deferred;
	int serial_status;
	int deferred_status;
	char what[96];
	size_t i;

	for (i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
		snprintf(what, sizeof(what), "%s on %zu thread(s)", queries[i],
		    threads);
		serial = context_for(graph.bytes, 0, 0, 0, 0,
		    &serial_diagnostics);
		deferred = context_for(graph.bytes, RELIC_F_PARALLEL_ALWAYS,
		    threads, 0, 0, &deferred_diagnostics);

		if (serial != NULL && deferred != NULL) {
			serial_status = run_query(serial, queries[i],
			    &serial_rows);
			deferred_status = run_query(deferred, queries[i],
			    &deferred_rows);
			checkf(serial_status == 0 && deferred_status == 0,
			    SCENARIO_REWRITE, "%s is answered", what);
			checkf(serial_rows.length > 0 &&
			    strcmp(serial_rows.bytes, deferred_rows.bytes) == 0,
			    SCENARIO_REWRITE, "%s: the rows come in one order",
			    what);
			checkf(merged_rounds(deferred) > 0, SCENARIO_REWRITE,
			    "%s: rounds are merged", what);
			checkf(shared_as_expected(deferred, threads),
			    SCENARIO_REWRITE, "%s: the rounds are shared when "
			    "the pool has threads", what);
			contexts_agree(SCENARIO_REWRITE, what, serial,
			    deferred);
		}

		relic_destroy(serial);
		relic_destroy(deferred);
	}
}

/*
 * Materialize a program under limits from what its facts need to more than
 * its evaluation does, in about `steps` steps, and require the deferred
 * context to succeed or fail as the serial one does, with the same
 * diagnostic, leaving the same relations.  A limit that refuses the room a
 * deferred round reserves for its candidates declines the round before it
 * runs, and it runs serially; one that refuses a round's buffers as they
 * grow abandons the round, which runs again serially; so does one that
 * would refuse its merge, before the merge changes any relation.
 * The memory limit also counts the evaluation's plans, which relic_stats_t
 * does not report, so its sweep runs to twice the storage.
 *
 * A program whose evaluation holds more beside the storage than the
 * storage itself -- the buffers of a round computing strings -- has the
 * memory limit's sweep moved up by the difference between the least limit
 * its serial run succeeds under and the storage it needs, so that the
 * sweep again runs from limits that refuse the evaluation early to limits
 * that admit it.  Every worker holds those buffers of its own, so with more
 * than one worker the sweep is moved up by the other workers' too.  The
 * workers are those the pool is sized to, which in a build without threads
 * is one however many threads are asked for.
 * Such a sweep need not see a merge the limit declines: the workers'
 * buffers are released before the merge asks, and are more than the merge
 * asks for.
 *
 * Otherwise the sweep must reach both ways a deferred round ends under a
 * limit: a round abandoned and run again serially, and a round merged.
 * Under the memory limit it must also reach a round declined at its
 * reservation, which the rounds after the first of a recursive component
 * make.
 * Under the memory limit it must also reach an abandoned round whose
 * serial run then succeeds, the case where the buffers' charge alone was
 * in the way, and a merge the limit itself declines, once every task has
 * run, rather than a buffer or a scratch table it refused; under the tuple
 * limit neither need exist, since a worker abandons the round once its own
 * candidates outnumber what the limit admits, which on one thread comes
 * before any merge could be declined for it.
 */
static void
limits_refuse_as_serially(const char *scenario, const char *program,
    bool memory, size_t steps, size_t threads)
{
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_stats_t loaded;
	relic_stats_t needed;
	relic_t *serial;
	relic_t *deferred;
	size_t workers;
	size_t working;
	size_t least;
	size_t first;
	size_t last;
	size_t limit;
	size_t step;
	int serial_status;
	int deferred_status;
	bool abandoned_seen;
	bool recovered_seen;
	bool declined_seen;
	bool reservation_seen;
	bool merged_seen;
	char what[64];

	serial = context_for(program, 0, 0, 0, 0, &serial_diagnostics);
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

	workers = relic_pool_size(threads);
	working = 0;
	if (memory) {
		least = least_memory_limit(program, false, loaded.storage_bytes,
		    needed.storage_bytes * 2 + WORKING_MAX);
		if (least > needed.storage_bytes * 2)
			working = least - needed.storage_bytes +
			    (workers - 1) * WORKER_TEXTS;
	}

	first = memory ? loaded.storage_bytes + working : loaded.tuples;
	last = memory ? needed.storage_bytes * 2 + working : needed.tuples;
	step = (last - first) / steps;
	if (step == 0)
		step = 1;

	abandoned_seen = false;
	recovered_seen = false;
	declined_seen = false;
	reservation_seen = false;
	merged_seen = false;

	for (limit = first; limit <= last; limit += step) {
		serial = context_for(program, 0, 0, memory ? 0 : limit,
		    memory ? limit : 0, &serial_diagnostics);
		deferred = context_for(program, RELIC_F_PARALLEL_ALWAYS,
		    threads, memory ? 0 : limit, memory ? limit : 0,
		    &deferred_diagnostics);
		if (serial == NULL || deferred == NULL) {
			relic_destroy(serial);
			relic_destroy(deferred);
			return;
		}

		serial_status = relic_materialize(serial);
		deferred_status = relic_materialize(deferred);

		snprintf(what, sizeof(what), "a limit of %zu on %zu thread(s)",
		    limit, threads);
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
		if (limit_declines(deferred) > 0)
			declined_seen = true;
		if (reservation_declines(deferred) > 0)
			reservation_seen = true;
		if (merged_rounds(deferred) > 0)
			merged_seen = true;

		relic_destroy(serial);
		relic_destroy(deferred);
	}

	checkf(abandoned_seen, scenario, "on %zu thread(s), the sweep abandons "
	    "a round", threads);
	checkf(merged_seen, scenario, "on %zu thread(s), the sweep merges a "
	    "round", threads);
	if (memory) {
		checkf(recovered_seen, scenario, "on %zu thread(s), the sweep "
		    "abandons a round whose serial run succeeds", threads);
		checkf(declined_seen || (working > 0 && workers > 1), scenario,
		    "on %zu thread(s), the sweep has the limit decline a "
		    "merge", threads);
		checkf(reservation_seen, scenario, "on %zu thread(s), the "
		    "sweep has the limit refuse a round's reservation",
		    threads);
	}
}

/*
 * The least memory limit in [low, high] under which `program`
 * materializes, as materializes_under() asks it, found by bisection, or
 * `high` when none below it does.  A context that succeeds under a limit
 * succeeds under any larger one, which is what the bisection relies on.
 */
static size_t
least_memory_limit(const char *program, bool merging, size_t low,
    size_t high)
{
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (materializes_under(program, merging, middle))
			high = middle;
		else
			low = middle + 1;
	}

	return (high);
}

/*
 * Whether a context under a memory limit of `limit` bytes materializes
 * `program`: a serial one, or when `merging` a deferred one on the calling
 * thread alone that also merges its every round, declining none at its
 * reservation and abandoning none.
 */
static bool
materializes_under(const char *program, bool merging, size_t limit)
{
	diagnostics_t diagnostics;
	relic_t *ctx;
	bool succeeded;

	ctx = context_for(program, merging ? RELIC_F_PARALLEL_ALWAYS : 0,
	    merging ? 1 : 0, 0, limit, &diagnostics);
	if (ctx == NULL)
		return (false);

	succeeded = relic_materialize(ctx) == 0;
	if (merging)
		succeeded = succeeded && merged_rounds(ctx) > 0 &&
		    abandoned_rounds(ctx) == 0 &&
		    reservation_declines(ctx) == 0;
	relic_destroy(ctx);

	return (succeeded);
}

/*
 * RELIC_F_NOTE_MIXED's notes come from the merge in task order, once per
 * rule, as the serial evaluator reports them.  The two rules of `small`
 * each compare values of two types and are one component's pass, so one
 * round holds both and must note both, in program order; `large` is a
 * round of its own, noted after them.
 */
static void
notes_come_as_serially(size_t threads)
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

	serial = context_for(program, RELIC_F_NOTE_MIXED, 0, 0, 0,
	    &serial_diagnostics);
	deferred = context_for(program, RELIC_F_NOTE_MIXED |
	    RELIC_F_PARALLEL_ALWAYS, threads, 0, 0, &deferred_diagnostics);

	if (serial != NULL && deferred != NULL) {
		serial_status = relic_materialize(serial);
		deferred_status = relic_materialize(deferred);
		checkf(serial_status == 0 && deferred_status == 0,
		    SCENARIO_NOTES, "on %zu thread(s), both materialize",
		    threads);
		checkf(lines_in(&serial_diagnostics) == 3, SCENARIO_NOTES,
		    "each of three rules is noted: \"%s\"",
		    serial_diagnostics.text);
		checkf(strcmp(serial_diagnostics.text,
		    deferred_diagnostics.text) == 0, SCENARIO_NOTES,
		    "on %zu thread(s), the notes agree: \"%s\" and \"%s\"",
		    threads, serial_diagnostics.text,
		    deferred_diagnostics.text);
		checkf(merged_rounds(deferred) > 0, SCENARIO_NOTES,
		    "on %zu thread(s), rounds are merged", threads);
		contexts_agree(SCENARIO_NOTES, relic_pool_size(threads) > 1 ?
		    "the notes' program shared" : "the notes' program",
		    serial, deferred);
	}

	relic_destroy(serial);
	relic_destroy(deferred);
}

/*
 * A round whose plans compute integers the value table lacks is merged,
 * not abandoned: every round of `program` computes, so a run that merged
 * rounds and abandoned none merged computing rounds.  total/1 derives each
 * of its few new integers many times over, on as many workers as take its
 * pieces; dist/2 derives new integers round by round.
 */
static void
computing_rounds_merge(size_t threads)
{
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_stats_t loaded;
	relic_stats_t computed;
	relic_t *serial;
	relic_t *deferred;
	int serial_status;
	int deferred_status;
	char what[64];
	unsigned int i;

	failing.length = 0;
	failing.bytes[0] = '\0';
	for (i = 0; i < GRAPH_NODES; i++) {
		appendf(&failing, "idx(%u).\n", i);
		appendf(&failing, "step(%u, %u).\n", i, (i * 7 + 3) %
		    GRAPH_NODES);
	}
	appendf(&failing,
	    "total(S) :- idx(I), idx(J), S = I + J + 1000.\n"
	    "dist(X, D) :- idx(X), D = X + 5000.\n"
	    "dist(Y, E) :- dist(X, D), step(X, Y), D < 5040, E = D + 1.\n");

	snprintf(what, sizeof(what), "computing on %zu thread(s)", threads);
	serial = context_for(failing.bytes, 0, 0, 0, 0, &serial_diagnostics);
	deferred = context_for(failing.bytes, RELIC_F_PARALLEL_ALWAYS, threads,
	    0, 0, &deferred_diagnostics);

	if (serial != NULL && deferred != NULL) {
		relic_stats(deferred, &loaded);
		serial_status = relic_materialize(serial);
		deferred_status = relic_materialize(deferred);
		relic_stats(deferred, &computed);
		checkf(serial_status == 0 && deferred_status == 0,
		    SCENARIO_COMPUTING, "%s: both materialize", what);
		checkf(merged_rounds(deferred) > 0 &&
		    abandoned_rounds(deferred) == 0, SCENARIO_COMPUTING,
		    "%s: every round is merged, %" PRIu64 " merged and %"
		    PRIu64 " abandoned", what, merged_rounds(deferred),
		    abandoned_rounds(deferred));
		checkf(computed.integer_values > loaded.integer_values + 100,
		    SCENARIO_COMPUTING, "%s: the merges interned integers, "
		    "%zu before and %zu after", what, loaded.integer_values,
		    computed.integer_values);
		checkf(shared_as_expected(deferred, threads),
		    SCENARIO_COMPUTING, "%s: the rounds are shared when the "
		    "pool has threads", what);
		contexts_agree(SCENARIO_COMPUTING, what, serial, deferred);
	}

	relic_destroy(serial);
	relic_destroy(deferred);
}

/*
 * A merge resolves each value a worker holds pending once, however many of
 * its candidates' columns hold it.  kind/7 derives one tuple for each of
 * COMPACT_FILES files, in one round on one worker, over many tasks and
 * many batches.  Each tuple holds COMPACT_PENDING pending columns over
 * fifteen new values: one of seven strings, one of three strings and one
 * of five integers, each held twice in the tuple.
 *
 * The test bounds memory, not time.  It finds the least memory limit under
 * which the serial context materializes the program, and the least under
 * which the deferred one does while merging its every round.  What lies
 * between the two is what the deferred round holds beside the serial run:
 * its candidates, its workers' text buffers, its merge's scratch and its
 * value resolution.  A resolution sized by occurrences would by itself
 * take a pending descriptor and a value map entry for every pending column,
 * more than that difference; one sized by the worker's fifteen values
 * takes a few kilobytes.
 *
 * The deferred context under its least limit must then hold what the
 * serial context holds, word for word, and the round must have interned
 * exactly the fifteen values, which the loaded program lacked.
 */
static void
pending_values_resolve_once(void)
{
	size_t occurrence_bytes;
	size_t occurrences;
	size_t serial_least;
	size_t merging_least;
	size_t high;

	compact_program(&failing);
	occurrences = COMPACT_FILES * COMPACT_PENDING;
	occurrence_bytes = occurrences * sizeof(relic_pending_t) +
	    relic_pending_map_bytes(occurrences) +
	    relic_pending_probe_bytes(occurrences);
	high = WORKING_MAX + 4 * occurrence_bytes;

	serial_least = least_memory_limit(failing.bytes, false, 0, high);
	merging_least = least_memory_limit(failing.bytes, true, serial_least,
	    high);
	checkf(serial_least < high && merging_least < high, SCENARIO_COMPACT,
	    "both contexts materialize under %zu bytes, the serial under "
	    "%zu and the deferred under %zu", high, serial_least,
	    merging_least);
	checkf(merging_least - serial_least < occurrence_bytes,
	    SCENARIO_COMPACT, "the deferred round holds %zu bytes beside the "
	    "serial run, less than the %zu its %zu pending columns would "
	    "take resolved by occurrence", merging_least - serial_least,
	    occurrence_bytes, occurrences);

	compacted_rounds_agree(1, merging_least);
}

/*
 * The program of pending_values_resolve_once(): COMPACT_FILES files, each
 * deriving a kind/7 tuple whose directory, extension and number are held
 * twice each.
 */
static void
compact_program(text_t *text)
{
	unsigned int i;

	text->length = 0;
	text->bytes[0] = '\0';
	for (i = 0; i < COMPACT_FILES; i++)
		appendf(text, "file(%u).\n", i);
	appendf(text,
	    "kind(I, D, E, N, D, E, N) :- file(I),\n"
	    "    D = cat(\"dir\", str(I mod %u)),\n"
	    "    E = cat(\"ext\", str(I mod %u)),\n"
	    "    N = I mod %u + %d.\n",
	    COMPACT_DIRS, COMPACT_EXTS, COMPACT_NUMBERS, COMPACT_NUMBER_BASE);
}

/*
 * Materialize the compact program serially and deferred on `threads`
 * threads under a memory limit of `max_memory`, zero for none, and require
 * the deferred context to merge every round, to hold what the serial one
 * holds word for word, to have interned exactly the values kind/7
 * computes, which the loaded program lacked, and to have given back every
 * byte its rounds and merges charged.  On several workers, equal values of
 * different workers are resolved to one identifier.
 */
static void
compacted_rounds_agree(size_t threads, size_t max_memory)
{
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_t *serial;
	relic_t *deferred;
	size_t loaded_values;
	size_t computed_values;
	int serial_status;
	int deferred_status;
	char what[96];

	compact_program(&failing);
	snprintf(what, sizeof(what), "few values in many columns on %zu "
	    "thread(s), under %zu bytes", threads, max_memory);
	serial = context_for(failing.bytes, 0, 0, 0, 0, &serial_diagnostics);
	deferred = context_for(failing.bytes, RELIC_F_PARALLEL_ALWAYS, threads,
	    0, max_memory, &deferred_diagnostics);
	if (serial != NULL && deferred != NULL) {
		loaded_values = compact_values_held(deferred);
		serial_status = relic_materialize(serial);
		deferred_status = relic_materialize(deferred);
		checkf(serial_status == 0 && deferred_status == 0,
		    SCENARIO_COMPACT, "%s: both materialize", what);
		computed_values = compact_values_held(deferred);
		checkf(merged_rounds(deferred) > 0 &&
		    abandoned_rounds(deferred) == 0 &&
		    reservation_declines(deferred) == 0, SCENARIO_COMPACT,
		    "%s: every round is merged, %" PRIu64 " merged, %" PRIu64
		    " abandoned and %" PRIu64 " declined", what,
		    merged_rounds(deferred), abandoned_rounds(deferred),
		    reservation_declines(deferred));
		checkf(loaded_values == 0 && computed_values ==
		    COMPACT_DIRS + COMPACT_EXTS + COMPACT_NUMBERS,
		    SCENARIO_COMPACT, "%s: the merge interned the %u values, "
		    "held %zu before and %zu after", what,
		    COMPACT_DIRS + COMPACT_EXTS + COMPACT_NUMBERS,
		    loaded_values, computed_values);
		checkf(shared_as_expected(deferred, threads),
		    SCENARIO_COMPACT, "%s: the rounds are shared when the "
		    "pool has threads", what);
		contexts_agree(SCENARIO_COMPACT, what, serial, deferred);
		checkf(deferred->account.round_bytes == 0, SCENARIO_COMPACT,
		    "%s: the rounds gave back their buffers", what);
	}

	relic_destroy(serial);
	relic_destroy(deferred);
}

/*
 * How many of the values kind/7 computes a context has assigned, asked
 * without interning any: the strings "dir0" up to COMPACT_DIRS and "ext0"
 * up to COMPACT_EXTS, and COMPACT_NUMBERS integers from
 * COMPACT_NUMBER_BASE.
 */
static size_t
compact_values_held(const relic_t *ctx)
{
	char bytes[16];
	size_t held;
	int length;
	unsigned int i;

	held = 0;
	for (i = 0; i < COMPACT_DIRS; i++) {
		length = snprintf(bytes, sizeof(bytes), "dir%u", i);
		if (relic_value_find_string(ctx, bytes, (size_t)length) != 0)
			held++;
	}
	for (i = 0; i < COMPACT_EXTS; i++) {
		length = snprintf(bytes, sizeof(bytes), "ext%u", i);
		if (relic_value_find_string(ctx, bytes, (size_t)length) != 0)
			held++;
	}
	for (i = 0; i < COMPACT_NUMBERS; i++) {
		if (relic_value_find_integer(ctx,
		    COMPACT_NUMBER_BASE + (int64_t)i) != 0)
			held++;
	}

	return (held);
}

/*
 * An evaluation an undefined result fails is failed by the serial
 * evaluator's code, at the derivation the serial evaluator meets it, and
 * leaves what it leaves.  A task that meets one abandons its round, which
 * runs again serially and reports it; the rounds before it are merged, and
 * the integers the failing round's earlier batches interned are interned
 * alike.
 *
 * `overflow` triples forty numbers round by round until each passes the
 * 64-bit integers, a round of forty tuples each time; `division` divides
 * by each of six hundred numbers in one round, reaching zero half way,
 * after batches whose integers the serial run has interned, and first by a
 * symbol, which is noted.  Each context is then asked a query that
 * computes and succeeds, on the pool the failure left, and the two must
 * still agree.
 */
static void
errors_fail_as_serially(size_t threads)
{
	static const char *const messages[] = {
		"integer overflow in rule grow/2",
		"division by zero in rule ratio/2",
	};
	static const char *const queries[] = {
		"small(K, Y)?",
		"fine(N, Q)?",
	};
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_stats_t loaded;
	relic_stats_t failed;
	text_t *program;
	relic_t *serial;
	relic_t *deferred;
	int serial_status;
	int deferred_status;
	char what[96];
	unsigned int i;
	size_t k;

	program = &failing;
	for (k = 0; k < sizeof(messages) / sizeof(messages[0]); k++) {
		program->length = 0;
		program->bytes[0] = '\0';
		if (k == 0) {
			for (i = 0; i < 40; i++)
				appendf(program, "start(%u, %u).\n", i, i + 1);
			appendf(program,
			    "grow(K, X) :- start(K, X).\n"
			    "grow(K, Y) :- grow(K, X), Y = X * 3 + K.\n"
			    "small(K, Y) :- start(K, X), Y = X * 7 + 5000.\n");
		} else {
			appendf(program, "num(x, y).\n");
			for (i = 0; i < 600; i++)
				appendf(program, "num(%u, %d).\n", i,
				    (int)i - 300);
			appendf(program,
			    "ratio(N, Q) :- num(N, D), Q = 1000000007 / D.\n"
			    "fine(N, Q) :- num(N, D), D > 0,\n"
			    "    Q = 1000000009 / D.\n");
		}

		snprintf(what, sizeof(what), "%s on %zu thread(s)",
		    messages[k], threads);
		serial = context_for(program->bytes, RELIC_F_NOTE_MIXED, 0, 0, 0,
		    &serial_diagnostics);
		deferred = context_for(program->bytes, RELIC_F_NOTE_MIXED |
		    RELIC_F_PARALLEL_ALWAYS, threads, 0, 0,
		    &deferred_diagnostics);

		if (serial != NULL && deferred != NULL) {
			relic_stats(serial, &loaded);
			serial_status = relic_materialize(serial);
			deferred_status = relic_materialize(deferred);
			checkf(serial_status != 0 && deferred_status != 0,
			    SCENARIO_ERRORS, "%s: both fail", what);
			checkf(strstr(serial_diagnostics.text,
			    messages[k]) != NULL, SCENARIO_ERRORS,
			    "%s: the serial run reports it: \"%s\"", what,
			    serial_diagnostics.text);
			checkf(k == 0 || lines_in(&serial_diagnostics) == 2,
			    SCENARIO_ERRORS, "%s: the symbol divisor is noted "
			    "before it: \"%s\"", what, serial_diagnostics.text);
			checkf(strcmp(serial_diagnostics.text,
			    deferred_diagnostics.text) == 0, SCENARIO_ERRORS,
			    "%s: the diagnostics agree: \"%s\" and \"%s\"",
			    what, serial_diagnostics.text,
			    deferred_diagnostics.text);
			checkf(abandoned_rounds(deferred) > 0, SCENARIO_ERRORS,
			    "%s: the failing round is abandoned", what);
			checkf(k == 1 || merged_rounds(deferred) > 0,
			    SCENARIO_ERRORS, "%s: the rounds before it are "
			    "merged", what);
			relic_stats(serial, &failed);
			checkf(failed.integer_values > loaded.integer_values,
			    SCENARIO_ERRORS, "%s: the failing run interned "
			    "integers", what);
			contexts_agree(SCENARIO_ERRORS, what, serial, deferred);
			checkf(deferred->account.round_bytes == 0,
			    SCENARIO_ERRORS, "%s: the round gave back its "
			    "buffers", what);

			serial_status = run_query(serial, queries[k],
			    &serial_rows);
			deferred_status = run_query(deferred, queries[k],
			    &deferred_rows);
			checkf(serial_status == 0 && deferred_status == 0 &&
			    serial_rows.length > 0 &&
			    strcmp(serial_rows.bytes, deferred_rows.bytes) == 0,
			    SCENARIO_ERRORS, "%s: %s is then answered alike",
			    what, queries[k]);
			contexts_agree(SCENARIO_ERRORS, queries[k], serial,
			    deferred);
		}

		relic_destroy(serial);
		relic_destroy(deferred);
	}
}

/*
 * A sum whose total does not fit fails the evaluation where the serial
 * evaluator fails it, whatever order a deferred round's tasks derived its
 * group in, and leaves what the serial run leaves.  The group of big/1 is
 * four hundred values near 2^62, derived by a round of their own, cut into
 * many pieces; small/1 folds the same group less its large values and
 * fits.  Each context is then asked a query the failure left answerable.
 */
static void
sums_overflow_as_serially(size_t threads)
{
	static const char message[] = "integer overflow in rule big/1";
	diagnostics_t serial_diagnostics;
	diagnostics_t deferred_diagnostics;
	relic_t *serial;
	relic_t *deferred;
	int serial_status;
	int deferred_status;
	char what[96];
	unsigned int i;

	failing.length = 0;
	failing.bytes[0] = '\0';
	for (i = 0; i < 400; i++)
		appendf(&failing, "num(%u, %" PRIu64 ").\n", i,
		    (UINT64_C(1) << 62) + i);
	appendf(&failing,
	    "val(K, V) :- num(K, V).\n"
	    "big(S) :- S = sum(V) { val(K, V) }.\n"
	    "small(S) :- S = sum(K) { val(K, V) }.\n");

	snprintf(what, sizeof(what), "%s on %zu thread(s)", message, threads);
	serial = context_for(failing.bytes, 0, 0, 0, 0, &serial_diagnostics);
	deferred = context_for(failing.bytes, RELIC_F_PARALLEL_ALWAYS, threads,
	    0, 0, &deferred_diagnostics);

	if (serial != NULL && deferred != NULL) {
		serial_status = relic_materialize(serial);
		deferred_status = relic_materialize(deferred);
		checkf(serial_status != 0 && deferred_status != 0,
		    SCENARIO_SUM, "%s: both fail", what);
		checkf(strstr(serial_diagnostics.text, message) != NULL &&
		    lines_in(&serial_diagnostics) == 1, SCENARIO_SUM,
		    "%s: the serial run reports it alone: \"%s\"", what,
		    serial_diagnostics.text);
		checkf(strcmp(serial_diagnostics.text,
		    deferred_diagnostics.text) == 0, SCENARIO_SUM,
		    "%s: the diagnostics agree: \"%s\" and \"%s\"", what,
		    serial_diagnostics.text, deferred_diagnostics.text);
		checkf(merged_rounds(deferred) > 0, SCENARIO_SUM,
		    "%s: the rounds before it are merged", what);
		contexts_agree(SCENARIO_SUM, what, serial, deferred);
		checkf(deferred->account.round_bytes == 0, SCENARIO_SUM,
		    "%s: the round gave back its buffers", what);

		serial_status = run_query(serial, "small(S)?", &serial_rows);
		deferred_status = run_query(deferred, "small(S)?",
		    &deferred_rows);
		checkf(serial_status == 0 && deferred_status == 0 &&
		    strstr(serial_rows.bytes, ":79800 \n") != NULL &&
		    strcmp(serial_rows.bytes, deferred_rows.bytes) == 0,
		    SCENARIO_SUM, "%s: small(S)? is then answered alike: "
		    "\"%s\"", what, serial_rows.bytes);
		contexts_agree(SCENARIO_SUM, "small(S)?", serial, deferred);
	}

	relic_destroy(serial);
	relic_destroy(deferred);
}

int
main(void)
{
	static const size_t thread_counts[] = { 1, DEFERRED_THREADS };
	size_t threads;
	size_t i;

	graph_program(&graph);
	wide_program(&wide);
	string_program(&strings);

	for (i = 0; i < sizeof(thread_counts) / sizeof(thread_counts[0]);
	    i++) {
		threads = thread_counts[i];
		a_deferred_round_inserts_in_serial_order(threads);
		a_querys_rewrite_runs_deferred_rounds(threads);
		limits_refuse_as_serially(SCENARIO_TUPLES, graph.bytes, false,
		    LIMIT_STEPS, threads);
		limits_refuse_as_serially(SCENARIO_MEMORY, graph.bytes, true,
		    LIMIT_STEPS, threads);
		limits_refuse_as_serially(SCENARIO_GROWTH, wide.bytes, true,
		    WIDE_LIMIT_STEPS, threads);
		notes_come_as_serially(threads);
		computing_rounds_merge(threads);
		errors_fail_as_serially(threads);
		sums_overflow_as_serially(threads);
		strings_rounds_merge(threads);
		patterns_fail_as_serially(threads);
		limits_refuse_as_serially(SCENARIO_TEXT_TUPLES, strings.bytes,
		    false, LIMIT_STEPS, threads);
		limits_refuse_as_serially(SCENARIO_TEXT_MEMORY, strings.bytes,
		    true, LIMIT_STEPS, threads);
		rounds_reserve_what_deferred_rounds_inserted(threads);
	}
	rounds_reserve_what_serial_rounds_inserted();
	a_lookup_drives_a_shared_round(1);
	a_lookup_drives_a_shared_round(DEFERRED_THREADS);
	a_lookup_drives_a_shared_round(MANY_THREADS);
	a_delta_lookup_drives_its_rounds(1);
	a_delta_lookup_drives_its_rounds(DEFERRED_THREADS);
	a_delta_lookup_drives_its_rounds(MANY_THREADS);
	pending_values_resolve_once();
	compacted_rounds_agree(2, 0);
	compacted_rounds_agree(DEFERRED_THREADS, 0);
	compacted_rounds_agree(MANY_THREADS, 0);
	computing_rounds_merge(MANY_THREADS);
	strings_rounds_merge(MANY_THREADS);

	return (check_summary("test_round"));
}
