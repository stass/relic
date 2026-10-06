/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the Magic Sets rewrite of a query.
 *
 * The central property is equivalence: a query answered through its rewrite
 * answers exactly what it answers from the whole fixed point.  It is checked
 * over generated graphs, for programs that recurse on the left, on the right
 * and on both sides, that recurse mutually, that negate, and that mix facts
 * with rules, and for every way a query can bind a binary predicate's
 * columns.  A context configured with RELIC_F_NO_MAGIC is the reference.
 *
 * The rest examine the rewrite through what -e shows of it: the names of the
 * adornments and seeds, that adornments are memoized so a program calling
 * itself with alternating bindings gives a finite rewrite, that rules equal up
 * to the names of their variables are generated once, that a rule deriving its
 * head from itself is dropped, and that the limit on the rewrite's work is
 * enforced exactly at its value, charging every search and every draft.  A
 * rewrite that outgrows the limit, its default or one the caller set, is
 * abandoned and the query answered from the whole program.  Last, what the
 * rewrite derives never leaks into a later answer or a later evaluation.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"
#include "programs.h"

#define	SCENARIO_ABANDONED	"a rewrite over its limit is abandoned"
#define	SCENARIO_DECLINED	"a declined cut is charged every prefix"
#define	SCENARIO_DEDUP		"equal generated rules are kept once"
#define	SCENARIO_TREES		"rules equal but for an expression differ"
#define	SCENARIO_DROPPED	"dropped rules are charged literals"
#define	SCENARIO_EQUIVALENT	"the rewrite answers as the fixed point does"
#define	SCENARIO_FACTS		"facts behind a rule fit the rewrite's limit"
#define	SCENARIO_LIMIT		"the rewrite's limit holds at its value"
#define	SCENARIO_MEMO		"adornments are memoized"
#define	SCENARIO_LINEAR		"the rewrite grows linearly in a body's calls"
#define	SCENARIO_LONG_BODY	"a long body's rewrite fits the limit"
#define	SCENARIO_LONG_NAME	"a long name is charged per predicate named"
#define	SCENARIO_NAMES		"adornments are named by their bound columns"
#define	SCENARIO_STATE		"what the rewrite derives does not leak"
#define	SCENARIO_TAUTOLOGY	"a rule deriving its own head is dropped"
#define	SCENARIO_SEARCHED	"the first search is charged what it reads"
#define	SCENARIO_SEARCHES	"every search is charged what it reads"
#define	SCENARIO_KEPT		"a rewrite holds only what it keeps"

/*
 * The generated graphs: GRAPH_NODES nodes and GRAPH_EDGES edges drawn from a
 * fixed linear congruential sequence per seed, so every run tests the same
 * graphs.  Small enough that the whole fixed point is cheap, dense enough to
 * have cycles and nodes nothing reaches.
 */
#define	GRAPH_SEEDS	24
#define	GRAPH_NODES	10
#define	GRAPH_EDGES	14

/* The ways shape_query() binds p's two columns. */
#define	QUERY_SHAPES	5

/* Room for a generated program and for a query's text. */
#define	PROGRAM_MAX	4096
#define	QUERY_MAX	64

typedef struct diag_log diag_log_t;
typedef struct rows rows_t;
typedef struct self_calling_rewrite self_calling_rewrite_t;
typedef struct text text_t;

/* The errors and warnings a context reported, and the last of each. */
struct diag_log {
	size_t	errors;
	size_t	warnings;
	char	last[256];
	char	last_warning[256];
};

/* A growable string.  `failed` is set when memory ran out. */
struct text {
	char	*bytes;
	size_t	 length;
	size_t	 capacity;
	bool	 failed;
};

/*
 * What a rewrite of the self-calling program held and spent: the bytes of
 * its arena, of its scratch arena and of the arena of its walks once it is
 * made, the rules it kept, and the literals it was charged for the rules it
 * read and drafted.
 */
struct self_calling_rewrite {
	size_t	arena_bytes;
	size_t	scratch_bytes;
	size_t	walk_bytes;
	size_t	rules_kept;
	size_t	literals_charged;
};

/* The rows of an answer, each rendered as one line, in delivery order. */
struct rows {
	relic_t	 *ctx;
	char	**lines;
	size_t	  count;
	size_t	  capacity;
	bool	  failed;
};

static char	*answer(relic_t *ctx, const char *query_text);
static char	*calls_program(size_t calls);
static char	*named_calls_program(size_t name_length, size_t calls,
		    char **queryp);
static int	 collect_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static int	 compare_lines(const void *left, const void *right);
static relic_t	*context_configured(const char *program,
		    const relic_config_t *config, diag_log_t *log);
static relic_t	*context_with(const char *program, unsigned int flags,
		    size_t max_magic, diag_log_t *log);
static void	 count_generated(const char *explained, size_t *rulesp,
		    size_t *literalsp);
static size_t	 count_lines_starting(const char *text, const char *prefix);
static bool	 explain_answer(const char *program, const char *query_text,
		    size_t max_magic, text_t *explained, diag_log_t *log);
static void	 graph_program(unsigned int seed, const char *rules,
		    char *program, size_t size);
static bool	 has_line(const char *text, const char *line);
static bool	 shows_rewrite(const char *explained);
static void	 shape_query(size_t shape, unsigned int first,
		    unsigned int second, char *query_text, size_t size);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static void	 record_explained(void *callback_ctx, const char *line);
static void	 text_append(text_t *text, const char *bytes, size_t length);
static void	 adornments_are_memoized(void);
static void	 adornments_are_named_by_their_bound_columns(void);
static void	 equal_generated_rules_are_kept_once(void);
static void	 rules_differing_in_an_expression_are_both_kept(void);
static void	 a_rule_deriving_its_own_head_is_dropped(void);
static void	 a_rewrite_over_its_limit_is_abandoned(void);
static void	 the_first_search_is_charged_what_it_reads(void);
static void	 a_rewrite_charges_its_drafts_and_holds_what_it_keeps(void);
static int	 rewrite_self_calling(unsigned int length,
		    self_calling_rewrite_t *rewritep);
static void	 a_declined_cut_is_charged_every_prefix(void);
static char	*declined_cuts_program(size_t calls);
static void	 facts_behind_a_rule_fit_the_rewrites_limit(void);
static void	 the_rewrite_grows_linearly_in_a_bodys_calls(void);
static void	 a_long_bodys_rewrite_fits_the_limit(void);
static void	 a_long_name_is_charged_per_predicate_named(void);
static void	 a_load_after_a_rewrite_reaches_the_next_query(void);
static void	 the_limit_holds_at_its_value(void);
static void	 every_search_is_charged_what_it_reads(void);
static void	 the_rewrite_answers_as_the_fixed_point_does(void);
static void	 what_the_rewrite_derives_does_not_leak(void);

static void
record_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_UNUSED, const char *message)
{
	diag_log_t *log;

	log = callback_ctx;
	if (level == RELIC_ERROR) {
		log->errors++;
		snprintf(log->last, sizeof(log->last), "%s", message);
	} else if (level == RELIC_WARNING) {
		log->warnings++;
		snprintf(log->last_warning, sizeof(log->last_warning), "%s",
		    message);
	}
}

static void
text_append(text_t *text, const char *bytes, size_t length)
{
	char *grown;
	size_t capacity;

	if (text->failed)
		return;

	if (text->length + length + 1 > text->capacity) {
		capacity = (text->length + length + 1) * 2;
		grown = realloc(text->bytes, capacity);
		if (grown == NULL) {
			text->failed = true;
			return;
		}
		text->bytes = grown;
		text->capacity = capacity;
	}

	bcopy(bytes, text->bytes + text->length, length);
	text->length += length;
	text->bytes[text->length] = '\0';
}

static void
record_explained(void *callback_ctx, const char *line)
{
	text_t *text;

	text = callback_ctx;
	text_append(text, line, strlen(line));
	text_append(text, "\n", 1);
}

/*
 * A context holding `program`, configured with `flags` and the limit on the
 * rewrite's work, reporting its diagnostics to `log`.
 */
static relic_t *
context_with(const char *program, unsigned int flags, size_t max_magic,
    diag_log_t *log)
{
	relic_config_t config;

	bzero(&config, sizeof(config));
	config.flags = flags;
	config.max_magic = max_magic;

	return (context_configured(program, &config, log));
}

/*
 * A context holding `program`, created with `config`, reporting its
 * diagnostics to `log`.
 */
static relic_t *
context_configured(const char *program, const relic_config_t *config,
    diag_log_t *log)
{
	relic_t *ctx;

	bzero(log, sizeof(*log));

	ctx = relic_create_config(config);
	if (ctx == NULL) {
		check(false, "creating a context", "a context is created");
		return (NULL);
	}
	relic_diag_set(ctx, record_diagnostic, log);

	if (relic_load_string(ctx, "test.dl", program) != 0) {
		check(false, "loading a test program", "the program loads");
		relic_destroy(ctx);
		return (NULL);
	}

	return (ctx);
}

/*
 * Render a row as its values separated by spaces: a symbol by its name and
 * an integer in decimal.  No string appears in these programs.
 */
static int
collect_row(void *callback_ctx, const relic_value_t *values, size_t count)
{
	char number[24];
	rows_t *rows;
	text_t line;
	char **lines;
	const char *name;
	size_t i;

	rows = callback_ctx;
	bzero(&line, sizeof(line));
	text_append(&line, "", 0);

	for (i = 0; i < count; i++) {
		if (values[i].type == RELIC_INTEGER) {
			snprintf(number, sizeof(number), "%lld",
			    (long long)values[i].v.integer);
			name = number;
		} else {
			name = relic_string(rows->ctx, values[i].v.id, NULL);
		}
		if (i > 0)
			text_append(&line, " ", 1);
		text_append(&line, name, strlen(name));
	}

	if (rows->count == rows->capacity) {
		rows->capacity = rows->capacity == 0 ? 16 : rows->capacity * 2;
		lines = realloc(rows->lines,
		    rows->capacity * sizeof(*rows->lines));
		if (lines == NULL)
			line.failed = true;
		else
			rows->lines = lines;
	}

	if (line.failed) {
		free(line.bytes);
		rows->failed = true;
		return (1);
	}

	rows->lines[rows->count] = line.bytes;
	rows->count++;

	return (0);
}

static int
compare_lines(const void *left, const void *right)
{
	return (strcmp(*(char *const *)left, *(char *const *)right));
}

/*
 * The answer to a query as its rows, sorted and each ending in a newline, or
 * NULL when the query could not be prepared or run.  The caller frees it.
 */
static char *
answer(relic_t *ctx, const char *query_text)
{
	relic_query_t *query;
	rows_t rows;
	text_t sorted;
	size_t i;
	int status;

	query = relic_query_prepare(ctx, NULL, query_text);
	if (query == NULL)
		return (NULL);

	bzero(&rows, sizeof(rows));
	rows.ctx = ctx;
	status = relic_query_run(ctx, query, collect_row, &rows);
	relic_query_destroy(query);

	bzero(&sorted, sizeof(sorted));
	text_append(&sorted, "", 0);
	if (rows.count > 0)
		qsort(rows.lines, rows.count, sizeof(*rows.lines),
		    compare_lines);
	for (i = 0; i < rows.count; i++) {
		text_append(&sorted, rows.lines[i], strlen(rows.lines[i]));
		text_append(&sorted, "\n", 1);
		free(rows.lines[i]);
	}
	free(rows.lines);

	if (status != 0 || rows.failed || sorted.failed) {
		free(sorted.bytes);
		return (NULL);
	}

	return (sorted.bytes);
}

/*
 * Answer a query through the rewrite with the explain callback installed,
 * collecting what it shows.  Returns whether the query was answered.
 */
static bool
explain_answer(const char *program, const char *query_text, size_t max_magic,
    text_t *explained, diag_log_t *log)
{
	relic_t *ctx;
	char *rows;

	bzero(explained, sizeof(*explained));
	text_append(explained, "", 0);

	ctx = context_with(program, 0, max_magic, log);
	if (ctx == NULL)
		return (false);

	relic_explain_set(ctx, record_explained, explained);
	rows = answer(ctx, query_text);
	relic_destroy(ctx);

	free(rows);

	return (rows != NULL && !explained->failed);
}

/* Whether `text` holds `line` as one of its lines. */
static bool
has_line(const char *text, const char *line)
{
	const char *at;
	size_t length;

	length = strlen(line);
	for (at = text; (at = strstr(at, line)) != NULL; at++) {
		if ((at == text || at[-1] == '\n') && at[length] == '\n')
			return (true);
	}

	return (false);
}

/* How many lines of `text` begin with `prefix`. */
static size_t
count_lines_starting(const char *text, const char *prefix)
{
	const char *line;
	size_t count;

	count = 0;
	for (line = text; *line != '\0'; line = strchr(line, '\n') + 1) {
		if (strncmp(line, prefix, strlen(prefix)) == 0)
			count++;
		if (strchr(line, '\n') == NULL)
			break;
	}

	return (count);
}

/*
 * A generated graph -- its edges, a node fact for every node, and a mark on
 * a few nodes -- followed by `rules`.
 */
static void
graph_program(unsigned int seed, const char *rules, char *program,
    size_t size)
{
	uint32_t x;
	size_t used;
	unsigned int from;
	unsigned int to;
	unsigned int i;

	x = seed * 2654435761u + 1;
	used = 0;

	for (i = 0; i < GRAPH_EDGES; i++) {
		x = x * 1664525u + 1013904223u;
		from = (x >> 16) % GRAPH_NODES;
		x = x * 1664525u + 1013904223u;
		to = (x >> 16) % GRAPH_NODES;
		used += (size_t)snprintf(program + used, size - used,
		    "edge(n%u, n%u).\n", from, to);
	}

	for (i = 0; i < GRAPH_NODES; i++) {
		used += (size_t)snprintf(program + used, size - used,
		    "node(n%u).\n", i);
		if ((i + seed) % 4 == 0)
			used += (size_t)snprintf(program + used, size - used,
			    "mark(n%u).\n", i);
	}

	snprintf(program + used, size - used, "%s", rules);
}

/*
 * A query on p of the given shape: the first column bound to node `first`,
 * the second bound to node `second`, both, neither, or one variable in both.
 */
static void
shape_query(size_t shape, unsigned int first, unsigned int second,
    char *query_text, size_t size)
{
	switch (shape) {
	case 0:
		snprintf(query_text, size, "p(n%u, X)?", first);
		break;
	case 1:
		snprintf(query_text, size, "p(X, n%u)?", second);
		break;
	case 2:
		snprintf(query_text, size, "p(n%u, n%u)?", first, second);
		break;
	case 3:
		snprintf(query_text, size, "p(X, Y)?");
		break;
	default:
		snprintf(query_text, size, "p(X, X)?");
		break;
	}
}

/*
 * Over generated graphs, every program below answers every binding of its
 * binary predicate through the rewrite as it does from the whole fixed point.
 * The queries bind the first column, the second, both, neither, and one
 * variable twice; the constants are nodes, some of them nodes no edge
 * reaches.
 */
static void
the_rewrite_answers_as_the_fixed_point_does(void)
{
	static const struct {
		const char	*name;
		const char	*rules;
	} programs[] = {
		{ "right", "p(X, Y) :- edge(X, Y).\n"
		    "p(X, Z) :- edge(X, Y), p(Y, Z).\n" },
		{ "left", "p(X, Y) :- edge(X, Y).\n"
		    "p(X, Z) :- p(X, Y), edge(Y, Z).\n" },
		{ "double", "p(X, Y) :- edge(X, Y).\n"
		    "p(X, Z) :- p(X, Y), p(Y, Z).\n" },
		{ "triple", "p(X, Y) :- edge(X, Y).\n"
		    "p(X, W) :- p(X, Y), p(Y, Z), p(Z, W).\n" },
		{ "same-generation", "p(X, X) :- node(X).\n"
		    "p(X, Y) :- edge(XP, X), p(XP, YP), edge(YP, Y).\n" },
		{ "mutual", "p(X, Y) :- edge(X, Y).\n"
		    "p(X, Z) :- edge(X, Y), q(Y, Z).\n"
		    "q(X, Z) :- edge(X, Y), p(Y, Z).\n" },
		{ "negation", "r(X, Y) :- edge(X, Y).\n"
		    "r(X, Z) :- r(X, Y), edge(Y, Z).\n"
		    "open(X, Y) :- edge(X, Y), not mark(X).\n"
		    "p(X, Y) :- open(X, Y), not r(Y, X).\n"
		    "p(X, Z) :- open(X, Y), p(Y, Z).\n" },
		{ "negated-derived", "r(X, Y) :- edge(X, Y).\n"
		    "r(X, Z) :- edge(X, Y), r(Y, Z).\n"
		    "p(X, Y) :- node(X), node(Y), not r(X, Y).\n" },
		{ "mixed", "p(n0, n1).\n"
		    "p(X, Y) :- edge(X, Y), mark(Y).\n"
		    "p(X, Z) :- p(X, Y), p(Y, Z).\n" },
		{ "constants", "p(n1, Y) :- edge(n2, Y).\n"
		    "p(X, X) :- edge(X, X).\n"
		    "p(X, Y) :- edge(X, Z), p(Z, Y), mark(Z).\n" }
	};
	char program[PROGRAM_MAX];
	char query_text[QUERY_MAX];
	diag_log_t magic_log;
	diag_log_t whole_log;
	relic_t *magic;
	relic_t *whole;
	char *expected;
	char *actual;
	size_t compared;
	size_t p;
	size_t s;
	unsigned int seed;
	unsigned int node;

	compared = 0;

	for (seed = 0; seed < GRAPH_SEEDS; seed++) {
		for (p = 0; p < sizeof(programs) / sizeof(programs[0]); p++) {
			graph_program(seed, programs[p].rules, program,
			    sizeof(program));

			for (s = 0; s < QUERY_SHAPES; s++) {
				node = (seed * 7 + (unsigned int)s * 3) %
				    GRAPH_NODES;
				shape_query(s, node, (node + seed) %
				    GRAPH_NODES, query_text,
				    sizeof(query_text));

				magic = context_with(program, 0, 0,
				    &magic_log);
				whole = context_with(program, RELIC_F_NO_MAGIC,
				    0, &whole_log);
				if (magic == NULL || whole == NULL) {
					relic_destroy(magic);
					relic_destroy(whole);
					return;
				}

				expected = answer(whole, query_text);
				actual = answer(magic, query_text);

				checkf(expected != NULL && actual != NULL &&
				    strcmp(expected, actual) == 0,
				    SCENARIO_EQUIVALENT,
				    "program %s, seed %u, %s: the rewrite "
				    "answered\n%s\nthe fixed point\n%s",
				    programs[p].name, seed, query_text,
				    actual != NULL ? actual : "(failed)",
				    expected != NULL ? expected : "(failed)");
				checkf(magic_log.errors == 0 &&
				    magic_log.warnings == 0,
				    SCENARIO_EQUIVALENT,
				    "program %s, seed %u, %s: the rewrite "
				    "reported %zu errors and %zu warnings",
				    programs[p].name, seed, query_text,
				    magic_log.errors, magic_log.warnings);

				free(expected);
				free(actual);
				relic_destroy(magic);
				relic_destroy(whole);
				compared++;
			}
		}
	}

	checkf(compared > 0, SCENARIO_EQUIVALENT,
	    "some query should be compared");
}

/*
 * An adornment is named after its predicate, then `$` and a b or an f per
 * column; its magic predicate adds `magic$` in front and holds only the
 * bound columns.  A query that binds nothing has no magic predicate for its
 * own call and generates no seed, though the rules it reaches may still call
 * path with a column bound.
 */
static void
adornments_are_named_by_their_bound_columns(void)
{
	static const struct {
		const char	*query;
		const char	*adorn;
		const char	*seed;
	} cases[] = {
		{ "path(a, X)?", "    adorn path/2 bf",
		    "    seed magic$path$bf(a)." },
		{ "path(X, c)?", "    adorn path/2 fb",
		    "    seed magic$path$fb(c)." },
		{ "path(a, c)?", "    adorn path/2 bb",
		    "    seed magic$path$bb(a, c)." },
		{ "path(X, Y)?", "    adorn path/2 ff", NULL }
	};
	static const char program[] =
	    "edge(a, b).\n"
	    "edge(b, c).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n";
	text_t explained;
	diag_log_t log;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (!explain_answer(program, cases[i].query, 0, &explained,
		    &log)) {
			checkf(false, SCENARIO_NAMES, "%s should be answered",
			    cases[i].query);
			free(explained.bytes);
			continue;
		}

		checkf(has_line(explained.bytes, cases[i].adorn),
		    SCENARIO_NAMES, "%s should show \"%s\", not\n%s",
		    cases[i].query, cases[i].adorn, explained.bytes);

		if (cases[i].seed != NULL)
			checkf(has_line(explained.bytes, cases[i].seed),
			    SCENARIO_NAMES, "%s should show \"%s\", not\n%s",
			    cases[i].query, cases[i].seed, explained.bytes);
		else
			checkf(count_lines_starting(explained.bytes,
			    "    seed ") == 0, SCENARIO_NAMES,
			    "%s binds nothing, so it should generate no "
			    "seed, not\n%s", cases[i].query, explained.bytes);

		free(explained.bytes);
	}
}

/*
 * swap calls itself with its columns exchanged, so a call with the first
 * column bound makes one with the second bound, which makes one with the
 * first bound again.  Memoized, that is two adornments and a finite rewrite.
 */
static void
adornments_are_memoized(void)
{
	static const char program[] =
	    "edge(a, b).\n"
	    "edge(b, c).\n"
	    "swap(X, Y) :- edge(X, Y).\n"
	    "swap(X, Y) :- swap(Y, X).\n";
	text_t explained;
	diag_log_t log;

	if (!explain_answer(program, "swap(a, Y)?", 0, &explained, &log)) {
		check(false, SCENARIO_MEMO, "swap(a, Y)? should be answered");
		free(explained.bytes);
		return;
	}

	checkf(count_lines_starting(explained.bytes, "    adorn ") == 2 &&
	    has_line(explained.bytes, "    adorn swap/2 bf") &&
	    has_line(explained.bytes, "    adorn swap/2 fb"),
	    SCENARIO_MEMO,
	    "swap(a, Y)? should adorn swap/2 as bf and fb only, not\n%s",
	    explained.bytes);

	free(explained.bytes);
}

/*
 * Two rules that differ only in the names of their variables rewrite to the
 * same adorned rule and the same magic rule, and each is kept once.
 */
static void
equal_generated_rules_are_kept_once(void)
{
	static const char program[] =
	    "edge(a, b).\n"
	    "edge(b, c).\n"
	    "q(X, Y) :- edge(X, Y).\n"
	    "p(X, Z) :- edge(X, Y), q(Y, Z).\n"
	    "p(A, C) :- edge(A, B), q(B, C).\n";
	text_t explained;
	diag_log_t log;

	if (!explain_answer(program, "p(a, Z)?", 0, &explained, &log)) {
		check(false, SCENARIO_DEDUP, "p(a, Z)? should be answered");
		free(explained.bytes);
		return;
	}

	checkf(count_lines_starting(explained.bytes,
	    "        rule p$bf(") == 1 &&
	    count_lines_starting(explained.bytes,
	    "        rule magic$q$bf(") == 1,
	    SCENARIO_DEDUP,
	    "p(a, Z)? should generate one adorned rule for p and one magic "
	    "rule for q, not\n%s", explained.bytes);

	free(explained.bytes);
}

/*
 * Rules equal up to the names of their variables, expressions included,
 * rewrite to one adorned rule: the rewrite renumbers the slots inside
 * expression trees as it does those of atoms.  A rule that differs only in
 * an expression -- a constant, an operator, or which side of a subtraction
 * a variable is on -- is another rule, and is kept beside it.
 */
static void
rules_differing_in_an_expression_are_both_kept(void)
{
	static const char program[] =
	    "edge(1, 2).\n"
	    "edge(2, 3).\n"
	    "q(X, Y) :- edge(X, Y).\n"
	    "p(X, Z) :- edge(X, Y), q(Y, W), Z = W * 2 + 1.\n"
	    "p(A, C) :- edge(A, B), q(B, D), C = D * 2 + 1.\n"
	    "p(X, Z) :- edge(X, Y), q(Y, W), Z = W * 2 + 2.\n"
	    "p(X, Z) :- edge(X, Y), q(Y, W), Z = W * 2 - 1.\n"
	    "p(X, Z) :- edge(X, Y), q(Y, W), Z = W - Y.\n"
	    "p(X, Z) :- edge(X, Y), q(Y, W), Z = Y - W.\n";
	text_t explained;
	diag_log_t log;

	if (!explain_answer(program, "p(1, Z)?", 0, &explained, &log)) {
		check(false, SCENARIO_TREES, "p(1, Z)? should be answered");
		free(explained.bytes);
		return;
	}

	checkf(count_lines_starting(explained.bytes,
	    "        rule p$bf(") == 5, SCENARIO_TREES,
	    "p(1, Z)? should keep five adorned rules for p, the two spellings "
	    "of W * 2 + 1 as one, not\n%s", explained.bytes);
	checkf(has_line(explained.bytes, "        rule p$bf(X, Z) :- "
	    "magic$p$bf(X), edge(X, Y), q$bf(Y, W), Z = W * 2 + 1."),
	    SCENARIO_TREES, "the kept rule should read its expression over "
	    "the renumbered slots, not\n%s", explained.bytes);

	free(explained.bytes);
}

/*
 * The left-recursive rule calls itself with the column its head binds, so
 * the magic rule for that call would derive magic$path$bf from itself.  It is
 * dropped, and the magic predicate holds the seed alone.
 */
static void
a_rule_deriving_its_own_head_is_dropped(void)
{
	static const char program[] =
	    "edge(a, b).\n"
	    "edge(b, c).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- path(X, Y), edge(Y, Z).\n";
	text_t explained;
	diag_log_t log;

	if (!explain_answer(program, "path(a, X)?", 0, &explained, &log)) {
		check(false, SCENARIO_TAUTOLOGY,
		    "path(a, X)? should be answered");
		free(explained.bytes);
		return;
	}

	checkf(count_lines_starting(explained.bytes,
	    "        rule magic$path$bf(") == 0 &&
	    has_line(explained.bytes,
	    "    stratum 0, component 0: magic$path$bf/1"),
	    SCENARIO_TAUTOLOGY,
	    "path(a, X)? should generate no magic rule, and its magic "
	    "predicate should not be recursive, not\n%s", explained.bytes);

	free(explained.bytes);
}

/*
 * Whether -e showed a query answered through its rewrite: a line beginning
 * with the rewrite's heading, which an abandoned rewrite does not show.
 */
static bool
shows_rewrite(const char *explained)
{
	return (strncmp(explained, "magic rewrite of", 16) == 0 ||
	    strstr(explained, "\nmagic rewrite of") != NULL);
}

/*
 * path(a, X)? costs its rewrite 12 literals.  The search reads the query's
 * one literal and path's two rules for path$bf, 1 + 1 + 2.  The draft writes
 * the rewritten query, 1, the seed, which has no body, the magic rule of the
 * recursive call, 2, and the two adorned rules with their guards, 2 and 3.
 * A limit of 12 admits the rewrite.  One of 11 abandons it at the last
 * adorned rule, with a warning naming the limit and the predicate being
 * adorned and no error, and the query is answered from the whole program.  A
 * query of two calls outgrows a limit of one literal in its own walk, before
 * any predicate is adorned, and the warning names the query.
 */
static void
the_limit_holds_at_its_value(void)
{
	static const char program[] =
	    "edge(a, b).\n"
	    "edge(b, c).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n";
	static const char adorning[] =
	    "the query's Magic rewrite outgrew magic=11 while adorning path/2; "
	    "answering it from the whole program";
	static const char rewriting[] =
	    "the query's Magic rewrite outgrew magic=1 while rewriting the "
	    "query; answering it from the whole program";
	text_t explained;
	diag_log_t log;
	bool answered;

	answered = explain_answer(program, "path(a, X)?", 12, &explained,
	    &log);
	checkf(answered && log.errors == 0 && log.warnings == 0 &&
	    shows_rewrite(explained.bytes), SCENARIO_LIMIT,
	    "a limit of 12 should admit the rewrite of path(a, X)?, which "
	    "reported %zu errors and %zu warnings, the last \"%s\"",
	    log.errors, log.warnings, log.last_warning);
	free(explained.bytes);

	answered = explain_answer(program, "path(a, X)?", 11, &explained,
	    &log);
	checkf(answered && log.errors == 0 && log.warnings == 1 &&
	    strcmp(log.last_warning, adorning) == 0 &&
	    !shows_rewrite(explained.bytes), SCENARIO_LIMIT,
	    "a limit of 11 should abandon the rewrite of path(a, X)? and "
	    "answer it from the whole program, which reported %zu errors and "
	    "%zu warnings, the last \"%s\"", log.errors, log.warnings,
	    log.last_warning);
	free(explained.bytes);

	answered = explain_answer(program, "path(a, X), path(X, Y)?", 1,
	    &explained, &log);
	checkf(answered && log.errors == 0 && log.warnings == 1 &&
	    strcmp(log.last_warning, rewriting) == 0, SCENARIO_LIMIT,
	    "a limit of one literal should abandon the rewrite in the walk of "
	    "a query of two, which reported %zu errors and %zu warnings, the "
	    "last \"%s\"", log.errors, log.warnings, log.last_warning);
	free(explained.bytes);
}

/*
 * p(c, X)? calls p bound, and q's last rule calls p free, so the first search
 * keeps p$bf, and r$bf which p's rules call, before it reads p free; it then
 * reads r free too, and the search runs again with both read free from the
 * start.  The first search reads the query and the eleven rules of five
 * adornments, p$bf, r$bf, q$bf, p$ff and r$ff: 1 + 3 + 3 + 5 + 3 + 3 = 18
 * literals.  The second reads the query and the rules of the three it keeps,
 * p$ff, r$ff and q$bf, 1 + 3 + 3 + 5 = 12, though it replays the walks the
 * first recorded.  The draft writes the rewritten query and the nine rules of
 * the subsumed rewrite, 1 + 4 + 3 + 10 = 18.  So the rewrite costs 48: a
 * limit of 48 admits it, and one of 47 abandons it in the draft.  Were the
 * second search charged nothing, or the first's charge forgotten when the
 * second began, 47 would admit it.
 *
 * An adornment a free one subsumes costs nothing beyond the step of the walk
 * that reaches it.  path(X, Y)? reaches path free, and path's recursive rule
 * then calls it with a column bound, which path$ff subsumes: the query and
 * path's two rules cost 1 + 3 = 4, and the query reads path completely and
 * drafts nothing, so a limit of 4 admits it and one of 3 does not.
 */
static void
every_search_is_charged_what_it_reads(void)
{
	static const char program[] =
	    "e(a, b).\n"
	    "e(b, c).\n"
	    "start(c).\n"
	    "f(c, x).\n"
	    "f(x, y).\n"
	    "g(x).\n"
	    "r(X, Y) :- e(X, Y).\n"
	    "r(X, Z) :- e(X, Y), r(Y, Z).\n"
	    "q(X, Y) :- f(X, Y).\n"
	    "q(X, Z) :- f(X, Y), q(Y, Z).\n"
	    "q(X, Y) :- g(X), p(Y, _).\n"
	    "p(X, Y) :- r(X, Y).\n"
	    "p(X, Y) :- start(X), q(X, Y).\n";
	static const char path_program[] =
	    "edge(a, b).\n"
	    "edge(b, c).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n";
	static const char query[] = "p(c, X)?";
	static const char outgrown[] =
	    "the query's Magic rewrite outgrew magic=47 while adorning q/2; "
	    "answering it from the whole program";
	text_t explained;
	diag_log_t log;
	bool answered;

	answered = explain_answer(program, query, 48, &explained, &log);
	checkf(answered && log.errors == 0 && log.warnings == 0 &&
	    has_line(explained.bytes, "    adorn p/2 bf, subsumed by p/2 ff") &&
	    has_line(explained.bytes, "    adorn q/2 bf") &&
	    count_lines_starting(explained.bytes, "    adorn ") == 5,
	    SCENARIO_SEARCHES,
	    "a limit of 48 should admit %s through the subsumed rewrite, which "
	    "reported \"%s\" and showed\n%s", query,
	    log.warnings != 0 ? log.last_warning : log.last, explained.bytes);
	free(explained.bytes);

	answered = explain_answer(program, query, 47, &explained, &log);
	checkf(answered && log.errors == 0 && log.warnings == 1 &&
	    strcmp(log.last_warning, outgrown) == 0, SCENARIO_SEARCHES,
	    "a limit of 47 should abandon the rewrite of %s, which charges both "
	    "searches, and reported %zu errors and %zu warnings, the last "
	    "\"%s\"", query, log.errors, log.warnings, log.last_warning);
	free(explained.bytes);

	answered = explain_answer(path_program, "path(X, Y)?", 4, &explained,
	    &log);
	checkf(answered && log.errors == 0 && log.warnings == 0 &&
	    has_line(explained.bytes, "    adorn path/2 bf, subsumed by "
	    "path/2 ff") &&
	    has_line(explained.bytes, "    read completely: path/2"),
	    SCENARIO_SEARCHES,
	    "a limit of 4 should admit path(X, Y)?, which reported \"%s\" and "
	    "showed\n%s", log.warnings != 0 ? log.last_warning : log.last,
	    explained.bytes);
	free(explained.bytes);

	answered = explain_answer(path_program, "path(X, Y)?", 3, &explained,
	    &log);
	checkf(answered && log.errors == 0 && log.warnings == 1,
	    SCENARIO_SEARCHES,
	    "a limit of 3 should abandon the rewrite of path(X, Y)?, which "
	    "reported %zu errors and %zu warnings", log.errors, log.warnings);
	free(explained.bytes);
}

/* The arity of the widening program, whose query reaches 2^9 adornments. */
#define	WIDENING_ARITY	10

/*
 * With no limit configured, the widening program's rewrite outgrows the
 * default long before its 2^9 adornments: it is abandoned with one warning
 * naming the limit and the predicate being adorned, no error, and the query
 * answered from the whole program, which explain shows as the program's
 * components with no rewrite.  A generous limit lets the whole rewrite run.
 */
static void
a_rewrite_over_its_limit_is_abandoned(void)
{
	static const char query[] =
	    "p(c, Y2, Y3, Y4, Y5, Y6, Y7, Y8, Y9, Y10)?";
	static const char expected[] = "c c c c c c c c c\n";
	char *program;
	text_t explained;
	diag_log_t log;
	relic_t *ctx;
	char *rows;
	bool answered;

	program = widening_program(WIDENING_ARITY);
	if (program == NULL) {
		check(false, SCENARIO_ABANDONED, "the widening program is built");
		return;
	}

	ctx = context_with(program, 0, 0, &log);
	if (ctx == NULL) {
		free(program);
		return;
	}
	rows = answer(ctx, query);
	checkf(rows != NULL && strcmp(rows, expected) == 0, SCENARIO_ABANDONED,
	    "the widening query should answer one row of c, not\n%s",
	    rows != NULL ? rows : "(failed)");
	free(rows);
	checkf(log.errors == 0 && log.warnings == 1 && strstr(log.last_warning,
	    "Magic rewrite outgrew magic=") != NULL &&
	    strstr(log.last_warning, "while adorning p/10; answering it from "
	    "the whole program") != NULL, SCENARIO_ABANDONED,
	    "the abandoned rewrite should report one warning and no error, "
	    "not %zu errors and %zu warnings, the last \"%s\"", log.errors,
	    log.warnings, log.last_warning);
	relic_destroy(ctx);

	answered = explain_answer(program, query, 0, &explained, &log);
	checkf(answered && strstr(explained.bytes,
	    "abandoned magic rewrite of ") != NULL &&
	    !shows_rewrite(explained.bytes), SCENARIO_ABANDONED,
	    "an abandoned rewrite should be marked and not explained, but -e "
	    "showed\n%s", explained.bytes);
	free(explained.bytes);

	answered = explain_answer(program, query, 10000000, &explained, &log);
	checkf(answered && log.warnings == 0 && shows_rewrite(explained.bytes),
	    SCENARIO_ABANDONED, "a generous limit should let the rewrite run, "
	    "which reported %zu warnings, the last \"%s\"", log.warnings,
	    log.last_warning);
	free(explained.bytes);

	free(program);
}

/*
 * The self-calling program's arity at which the first search cannot read
 * its adornments within the default limit, its long rule's calls on f, and
 * that limit: 768 + 48 x 2048, the program's 2047 literals and the query's
 * one.
 */
#define	SELF_CALLING_WIDE_ARITY	11
#define	SELF_CALLING_LENGTH	2000
#define	SELF_CALLING_WIDE_LIMIT	99072

/*
 * The search that finds the adornments reads every rule of each, and is
 * charged the literals it reads, so a query reaching more adornments than
 * the limit can read is abandoned before a rule is drafted.  At arity 11 the
 * self-calling program's p has 2^10 adornments, each with rules of 2034
 * literals between them: read whole, they would cost some two million
 * literals.  The rewrite is driven step by step, as a query drives it, to
 * show where it stops: in the first search, having read no more than the
 * limit and drafted nothing.  f has no rule, so nothing need be evaluated
 * before the rewrite.
 */
static void
the_first_search_is_charged_what_it_reads(void)
{
	static const char text[] =
	    "p(c, Y2, Y3, Y4, Y5, Y6, Y7, Y8, Y9, Y10, Y11)?";
	relic_query_t *query;
	relic_magic_t magic;
	diag_log_t log;
	relic_t *ctx;
	char *program;
	int status;

	program = self_calling_program(SELF_CALLING_WIDE_ARITY,
	    SELF_CALLING_LENGTH);
	if (program == NULL) {
		check(false, SCENARIO_SEARCHED,
		    "the self-calling program is built");
		return;
	}

	ctx = context_with(program, 0, 0, &log);
	free(program);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, text);
	if (query == NULL || relic_program_ready(ctx) != 0) {
		check(false, SCENARIO_SEARCHED, "the query is prepared");
		relic_query_destroy(query);
		relic_destroy(ctx);
		return;
	}

	relic_magic_init(&magic, ctx);
	status = relic_magic_find_complete(&magic, query);
	if (status == 0)
		status = relic_magic_rewrite(&magic, &query->rule);

	checkf(status != 0 && magic.abandoned, SCENARIO_SEARCHED,
	    "the rewrite should be abandoned over its limit, and %s",
	    status == 0 ? "succeeded" : "failed otherwise");
	checkf(magic.rules.count == 0 &&
	    magic.work_spent <= magic.work_limit &&
	    magic.work_limit == SELF_CALLING_WIDE_LIMIT, SCENARIO_SEARCHED,
	    "the first search should read at most the default of %d literals "
	    "and draft nothing, and read %zu of %zu and kept %zu rules",
	    SELF_CALLING_WIDE_LIMIT, magic.work_spent, magic.work_limit,
	    magic.rules.count);
	checkf(magic.kept.count < 1024, SCENARIO_SEARCHED,
	    "the first search should stop short of p's 1024 adornments, and "
	    "found %zu", magic.kept.count);

	relic_magic_reset(&magic);
	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * The arity at which a rewrite's drafts and memory are measured against what
 * it keeps, whose query reaches KEPT_ADORNMENTS adornments of p, and the
 * bytes the rewrite's arena may differ by between two lengths of the
 * self-calling rule.
 */
#define	KEPT_ARITY	7
#define	KEPT_ADORNMENTS	64
#define	KEPT_SHORT	20
#define	KEPT_SLACK	(64 * 1024)
#define	KEPT_LIMIT	10000000

/*
 * Under a limit far above what it needs, a rewrite drafts every rule of the
 * adornments it keeps.  Each adornment of the self-calling program's p
 * drafts its long rule, whose head is its own first literal, and drops it;
 * the call on p restates the head and so drafts no magic rule.  The rules
 * kept are the same whatever the long rule's length.
 *
 * The literals charged are counted for every rule read and every draft,
 * kept or dropped, so lengthening the rule by 1980 calls charges each of
 * the 64 adornments 1980 more literals for the search's reading it and 1980
 * more for the draft of it it drops: twice that, however few it keeps.  The rewrite's arena holds what is kept, and what drafting a rule
 * needed is released once it is drafted, so neither the arena's size nor
 * what the scratch arena still holds can depend on that length: at 2000
 * calls each is within a few pages of its size at 20.  The walks the search
 * recorded, a step per literal of every rule it read, are released once the
 * rewrite has drafted from them, so the query runs without them.
 */
static void
a_rewrite_charges_its_drafts_and_holds_what_it_keeps(void)
{
	self_calling_rewrite_t shorter;
	self_calling_rewrite_t longer;
	size_t lengthened_by;

	if (rewrite_self_calling(KEPT_SHORT, &shorter) != 0 ||
	    rewrite_self_calling(SELF_CALLING_LENGTH, &longer) != 0)
		return;

	lengthened_by = 2 * KEPT_ADORNMENTS *
	    (size_t)(SELF_CALLING_LENGTH - KEPT_SHORT);
	checkf(longer.literals_charged >= shorter.literals_charged +
	    lengthened_by, SCENARIO_DROPPED,
	    "lengthening the dropped rule should charge at least %zu more "
	    "literals, not %zu at %u calls and %zu at %u", lengthened_by,
	    shorter.literals_charged, KEPT_SHORT, longer.literals_charged,
	    SELF_CALLING_LENGTH);
	checkf(longer.rules_kept == shorter.rules_kept, SCENARIO_KEPT,
	    "the rewrite should keep the same rules whatever the long rule's "
	    "length, not %zu and %zu", shorter.rules_kept, longer.rules_kept);
	checkf(longer.arena_bytes <= shorter.arena_bytes + KEPT_SLACK,
	    SCENARIO_KEPT,
	    "the rewrite's arena should hold what it keeps, %zu bytes at %u "
	    "calls, not %zu bytes at %u", shorter.arena_bytes, KEPT_SHORT,
	    longer.arena_bytes, SELF_CALLING_LENGTH);
	checkf(longer.scratch_bytes <= shorter.scratch_bytes + KEPT_SLACK,
	    SCENARIO_KEPT,
	    "the rewrite's scratch arena should be emptied of each rule's "
	    "drafts, holding %zu bytes at %u calls, not %zu bytes at %u",
	    shorter.scratch_bytes, KEPT_SHORT, longer.scratch_bytes,
	    SELF_CALLING_LENGTH);
	checkf(shorter.walk_bytes == 0 && longer.walk_bytes == 0,
	    SCENARIO_KEPT,
	    "the rewrite should release its walks once it has drafted, not "
	    "hold %zu bytes at %u calls and %zu at %u", shorter.walk_bytes,
	    KEPT_SHORT, longer.walk_bytes, SELF_CALLING_LENGTH);
}

/*
 * Rewrite the self-calling program's query at KEPT_ARITY, its long rule
 * making `length` calls, under KEPT_LIMIT, and measure the rewrite.
 * Returns -1 when the rewrite could not be made.
 */
static int
rewrite_self_calling(unsigned int length, self_calling_rewrite_t *rewritep)
{
	static const char text[] = "p(c, Y2, Y3, Y4, Y5, Y6, Y7)?";
	relic_query_t *query;
	relic_magic_t magic;
	diag_log_t log;
	relic_t *ctx;
	char *program;
	int status;

	program = self_calling_program(KEPT_ARITY, length);
	if (program == NULL) {
		check(false, SCENARIO_KEPT, "the self-calling program is built");
		return (-1);
	}

	ctx = context_with(program, 0, KEPT_LIMIT, &log);
	free(program);
	if (ctx == NULL)
		return (-1);

	query = relic_query_prepare(ctx, NULL, text);
	if (query == NULL || relic_program_ready(ctx) != 0) {
		check(false, SCENARIO_KEPT, "the query is prepared");
		relic_query_destroy(query);
		relic_destroy(ctx);
		return (-1);
	}

	relic_magic_init(&magic, ctx);
	status = relic_magic_find_complete(&magic, query);
	if (status == 0)
		status = relic_magic_rewrite(&magic, &query->rule);
	checkf(status == 0 && log.errors == 0, SCENARIO_KEPT,
	    "the rewrite at %u calls should succeed, the last error \"%s\"",
	    length, log.last);

	rewritep->arena_bytes = relic_arena_total_bytes(&magic.arena);
	rewritep->scratch_bytes = relic_arena_total_bytes(&magic.scratch);
	rewritep->walk_bytes = relic_arena_total_bytes(&magic.walk_arena);
	rewritep->rules_kept = magic.rules.count;
	rewritep->literals_charged = magic.work_spent;

	relic_magic_reset(&magic);
	relic_query_destroy(query);
	relic_destroy(ctx);

	return (status);
}

/*
 * The calls of the rule whose every cut is declined, and two limits on its
 * rewrite: the default, 768 + 48 x 205 for the program's 204 literals and
 * the query's one, and a larger one that still admits less than the rewrite
 * costs.
 */
#define	DECLINED_CALLS		200
#define	DECLINED_DEFAULT	10608
#define	DECLINED_RAISED		15000

/*
 * r's 32 columns are all bound by the query, and its body is a chain of
 * calls on p, each binding its first column with a variable a call before
 * it bound: p(X1, V), p(V, W), p(W, V), p(V, W), ...  Before every call
 * after the first, 33 or 34 slots are live, the head's 32 and the chain's,
 * more than a supplementary predicate may have columns, so the cut is
 * declined and the call's magic rule copies the whole prefix: L calls write
 * about L^2/2 literals.  Every prefix copied is charged, so
 * the default abandons the rewrite in the draft, as does a raised limit
 * below what it costs; no limit lifts the charge.  A limit above it admits
 * the rewrite, driven step by step to read what it kept: its rules hold at
 * least the L(L - 1)/2 literals of the prefixes, which shows that the cuts
 * were declined, and it was charged at least what they hold.
 */
static void
a_declined_cut_is_charged_every_prefix(void)
{
	char text[160];
	relic_query_t *query;
	relic_magic_t magic;
	text_t explained;
	diag_log_t log;
	relic_t *ctx;
	size_t literals;
	char *program;
	bool answered;
	int status;
	size_t i;

	program = declined_cuts_program(DECLINED_CALLS);
	if (program == NULL) {
		check(false, SCENARIO_DECLINED, "the program is built");
		return;
	}
	snprintf(text, sizeof(text), "r(1");
	for (i = 1; i < RELIC_MAX_ARITY; i++)
		strlcat(text, ", 1", sizeof(text));
	strlcat(text, ")?", sizeof(text));

	answered = explain_answer(program, text, 0, &explained, &log);
	checkf(answered && log.errors == 0 && log.warnings == 1 &&
	    strstr(log.last_warning, "outgrew magic=10608 while adorning "
	    "p/2") != NULL && !shows_rewrite(explained.bytes),
	    SCENARIO_DECLINED,
	    "the default of %d should abandon a rewrite whose cuts are "
	    "declined, which reported %zu errors and %zu warnings, the last "
	    "\"%s\"", DECLINED_DEFAULT, log.errors, log.warnings,
	    log.last_warning);
	free(explained.bytes);

	answered = explain_answer(program, text, DECLINED_RAISED, &explained,
	    &log);
	checkf(answered && log.errors == 0 && log.warnings == 1 &&
	    strstr(log.last_warning, "outgrew magic=15000 while adorning "
	    "p/2") != NULL, SCENARIO_DECLINED,
	    "a limit of %d should abandon the rewrite too, which reported %zu "
	    "errors and %zu warnings, the last \"%s\"", DECLINED_RAISED,
	    log.errors, log.warnings, log.last_warning);
	free(explained.bytes);

	ctx = context_with(program, 0, 1000000, &log);
	free(program);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, text);
	if (query == NULL || relic_program_ready(ctx) != 0) {
		check(false, SCENARIO_DECLINED, "the query is prepared");
		relic_query_destroy(query);
		relic_destroy(ctx);
		return;
	}

	relic_magic_init(&magic, ctx);
	status = relic_magic_find_complete(&magic, query);
	if (status == 0)
		status = relic_magic_rewrite(&magic, &query->rule);

	literals = 0;
	for (i = 0; i < magic.rules.count; i++)
		literals += magic.rules.entries[i].body_length;
	checkf(status == 0 && log.warnings == 0 &&
	    literals >= DECLINED_CALLS * (DECLINED_CALLS - 1) / 2 &&
	    magic.work_spent >= literals, SCENARIO_DECLINED,
	    "a generous limit should admit the rewrite of %d calls, whose "
	    "prefixes hold at least %d literals, and it held %zu and was "
	    "charged %zu, reporting %zu warnings, the last \"%s\"",
	    DECLINED_CALLS, DECLINED_CALLS * (DECLINED_CALLS - 1) / 2,
	    literals, magic.work_spent, log.warnings, log.last_warning);

	relic_magic_reset(&magic);
	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * The program of a_declined_cut_is_charged_every_prefix(): a fact of 32
 * columns, p over a fact, and r's chain of `calls` calls on p, which
 * alternates two variables to stay within a rule's 64.
 * Returns the text, which the caller frees, or NULL when memory ran out.
 */
static char *
declined_cuts_program(size_t calls)
{
	static const char facts[] = "e(1, 1).\np(X, Y) :- e(X, Y).\n";
	char piece[64];
	text_t text;
	size_t i;

	bzero(&text, sizeof(text));
	text_append(&text, facts, strlen(facts));
	for (i = 1; i <= RELIC_MAX_ARITY; i++) {
		snprintf(piece, sizeof(piece), "%s1", i == 1 ? "w(" : ", ");
		text_append(&text, piece, strlen(piece));
	}
	text_append(&text, ").\n", strlen(").\n"));

	for (i = 1; i <= RELIC_MAX_ARITY; i++) {
		snprintf(piece, sizeof(piece), "%sX%zu", i == 1 ? "r(" : ", ",
		    i);
		text_append(&text, piece, strlen(piece));
	}
	for (i = 1; i <= RELIC_MAX_ARITY; i++) {
		snprintf(piece, sizeof(piece), "%sX%zu",
		    i == 1 ? ") :- w(" : ", ", i);
		text_append(&text, piece, strlen(piece));
	}
	text_append(&text, "), p(X1, V)", strlen("), p(X1, V)"));
	for (i = 1; i < calls; i++) {
		snprintf(piece, sizeof(piece), "%s",
		    i % 2 == 1 ? ", p(V, W)" : ", p(W, V)");
		text_append(&text, piece, strlen(piece));
	}
	text_append(&text, ".\n", strlen(".\n"));

	if (text.failed) {
		free(text.bytes);
		return (NULL);
	}

	return (text.bytes);
}

/* How many facts of e the mixed program holds. */
#define	MIXED_FACTS	1000

/*
 * A predicate with facts and a rule alike is read on demand, and each of
 * its adornments reads the facts through one rule over its base view,
 * e(X, Y) :- e$base(X, Y), which the limit counts as two literals.  A
 * thousand facts of e behind one symmetric rule are rewritten for e(n7, X)?
 * without a warning, and the rewrite copies none of them: the rewritten
 * program holds a few rules, not a thousand.  Its plans read the base store
 * under the base view's name, since e names the derived relation.
 */
static void
facts_behind_a_rule_fit_the_rewrites_limit(void)
{
	char line[64];
	text_t program;
	text_t explained;
	diag_log_t log;
	size_t literals;
	size_t rules;
	bool answered;
	unsigned int i;

	bzero(&program, sizeof(program));
	for (i = 0; i < MIXED_FACTS; i++) {
		snprintf(line, sizeof(line), "e(n%u, n%u).\n", i, i + 1);
		text_append(&program, line, strlen(line));
	}
	text_append(&program, "e(X, Y) :- e(Y, X).\n",
	    strlen("e(X, Y) :- e(Y, X).\n"));
	if (program.failed) {
		free(program.bytes);
		check(false, SCENARIO_FACTS, "the mixed program is built");
		return;
	}

	answered = explain_answer(program.bytes, "e(n7, X)?", 0, &explained,
	    &log);
	checkf(answered && log.errors == 0 && log.warnings == 0 &&
	    strstr(explained.bytes, "magic rewrite of") != NULL,
	    SCENARIO_FACTS,
	    "e(n7, X)? should be answered through its rewrite, which "
	    "reported %zu errors and %zu warnings, the last \"%s\"",
	    log.errors, log.warnings, log.last_warning);

	if (answered) {
		count_generated(explained.bytes, &rules, &literals);
		checkf(rules < 16, SCENARIO_FACTS, "the rewrite should copy "
		    "none of the facts, but holds %zu rules", rules);
		checkf(strstr(explained.bytes,
		    "lookup e$base(X1, X2) key={0}") != NULL, SCENARIO_FACTS,
		    "the facts should be looked up as e$base, but -e "
		    "showed\n%s", explained.bytes);
	}

	free(explained.bytes);
	free(program.bytes);
}

/*
 * How many rules the rewritten program holds, and how many body literals
 * between them, as -e shows them: each rule of a rewritten component on a
 * line of its own, a fact counted in its component's heading.  A body's
 * literals are separated by commas outside parentheses.
 */
static void
count_generated(const char *explained, size_t *rulesp, size_t *literalsp)
{
	static const char rule_prefix[] = "        rule ";
	static const char facts_prefix[] = "        facts: ";
	const char *line;
	const char *at;
	size_t depth;

	*rulesp = 0;
	*literalsp = 0;
	for (line = explained; *line != '\0'; line = strchr(line, '\n') + 1) {
		if (strncmp(line, facts_prefix, strlen(facts_prefix)) == 0)
			*rulesp += strtoul(line + strlen(facts_prefix), NULL,
			    10);

		if (strncmp(line, rule_prefix, strlen(rule_prefix)) == 0) {
			(*rulesp)++;
			at = strstr(line, " :- ");
			if (at != NULL)
				(*literalsp)++;
			for (depth = 0; at != NULL && *at != '\n' &&
			    *at != '\0'; at++) {
				if (*at == '(')
					depth++;
				else if (*at == ')')
					depth--;
				else if (*at == ',' && depth == 0)
					(*literalsp)++;
			}
		}

		if (strchr(line, '\n') == NULL)
			break;
	}
}

/*
 * A body of `calls` calls on p, each of which the rule's head binds.
 * Returns the text, which the caller frees, or NULL when memory ran out.
 */
static char *
calls_program(size_t calls)
{
	static const char facts[] = "e(1, 2).\ne(2, 3).\np(X) :- e(X, _Y).\n";
	static const char first[] = "r(X) :- p(X)";
	static const char next[] = ", p(X)";
	text_t text;
	size_t i;

	bzero(&text, sizeof(text));
	text_append(&text, facts, sizeof(facts) - 1);
	text_append(&text, first, sizeof(first) - 1);
	for (i = 1; i < calls; i++)
		text_append(&text, next, sizeof(next) - 1);
	text_append(&text, ".\n", 2);

	if (text.failed) {
		free(text.bytes);
		return (NULL);
	}

	return (text.bytes);
}

/*
 * r(X) :- p(X), ..., p(X) with L calls, each called with X bound, generates
 * a magic rule per call.  Written out in full, the magic rule of the k-th
 * call repeats the k literals before it, so the rewrite holds about L^2/2
 * literals: doubling L would quadruple them.  Cut before every call after
 * the first, each prefix is one literal, and the rewrite is a part growing
 * in proportion to L and a positive part that does not depend on it, so
 * twice the rewrite for L bounds the one for 2L.  Each call has a magic
 * rule, so a rewrite of fewer rules than calls was not measured.  A
 * generous limit lets a quadratic rewrite run, which the default would
 * abandon before it could be measured.
 */
static void
the_rewrite_grows_linearly_in_a_bodys_calls(void)
{
	static const size_t lengths[] = { 50, 100 };
	size_t rules[2];
	size_t literals[2];
	text_t explained;
	diag_log_t log;
	char *program;
	bool answered;
	bool measured;
	size_t i;

	measured = true;
	for (i = 0; i < 2; i++) {
		program = calls_program(lengths[i]);
		if (program == NULL) {
			check(false, SCENARIO_LINEAR, "the program is built");
			return;
		}

		answered = explain_answer(program, "r(1)?", 1000000,
		    &explained, &log);
		rules[i] = 0;
		literals[i] = 0;
		if (answered)
			count_generated(explained.bytes, &rules[i],
			    &literals[i]);
		checkf(answered && log.errors == 0 && log.warnings == 0 &&
		    rules[i] >= lengths[i], SCENARIO_LINEAR,
		    "r(1)? over %zu calls should be answered through a rewrite "
		    "of a magic rule per call, which generated %zu rules and "
		    "reported %zu errors and %zu warnings, the last \"%s\"",
		    lengths[i], rules[i], log.errors, log.warnings,
		    log.last_warning);
		if (!answered || rules[i] < lengths[i])
			measured = false;

		free(explained.bytes);
		free(program);
	}

	if (!measured)
		return;

	checkf(rules[1] <= 2 * rules[0] && literals[1] <= 2 * literals[0],
	    SCENARIO_LINEAR,
	    "doubling the calls from %zu to %zu should at most double the "
	    "generated rules and literals, not take them from %zu to %zu "
	    "and from %zu to %zu", lengths[0], lengths[1], rules[0], rules[1],
	    literals[0], literals[1]);
}

/* The calls in the long body of a_long_bodys_rewrite_fits_the_limit(). */
#define	LONG_BODY_CALLS		400

/*
 * A rewrite that specializes the program fits the default limit however few
 * rules the program has.  r(X) :- p(X), ..., p(X) with 400 calls and the
 * rule deriving p are 2 rules and 2 + 401 = 403 literals, and with the
 * query's one a default of 768 + 48 x 404 = 20160.  The one adornment of r
 * keeps a magic rule and a supplementary rule per call, over 800 rules of
 * one or two literals each, well within the limit, which bounds the rules
 * kept as well: a bound of a multiple of the program's 2 rules would
 * abandon it.
 */
static void
a_long_bodys_rewrite_fits_the_limit(void)
{
	text_t explained;
	diag_log_t log;
	size_t literals;
	size_t rules;
	char *program;
	bool answered;

	program = calls_program(LONG_BODY_CALLS);
	if (program == NULL) {
		check(false, SCENARIO_LONG_BODY, "the program is built");
		return;
	}

	answered = explain_answer(program, "r(1)?", 0, &explained, &log);
	rules = 0;
	literals = 0;
	if (answered)
		count_generated(explained.bytes, &rules, &literals);
	checkf(answered && log.errors == 0 && log.warnings == 0 &&
	    rules > 2 * LONG_BODY_CALLS, SCENARIO_LONG_BODY,
	    "r(1)? over %d calls should be answered through a rewrite of "
	    "over %d rules, which generated %zu rules and reported %zu "
	    "errors and %zu warnings, the last \"%s\"", LONG_BODY_CALLS,
	    2 * LONG_BODY_CALLS, rules, log.errors, log.warnings,
	    log.last_warning);

	free(explained.bytes);
	free(program);
}

/*
 * The calls of a_long_name_is_charged_per_predicate_named() and the length
 * of its long name.
 */
#define	NAMED_CALLS		200
#define	NAMED_LONG		16384

/*
 * Every predicate the rewrite generates is named after the one it comes
 * from, so a long name is copied, hashed and stored once per predicate
 * generated from it, and is charged a literal per 64 bytes each time.  A
 * rule of 200 calls on p, its head named by 16384 bytes, cuts a prefix
 * before every call but the first, and each cut names a supplementary
 * predicate after the head's adorned one: 199 names of 256 literals each,
 * some 51000 between them, past the default of 768 + 48 x 460 = 22848 for
 * a program of 459, which counts the head's name once, and a query of one.  The rewrite is
 * abandoned and the query answered from the whole program.  The same rule
 * under a one-byte name costs nothing for its names, each shorter than 64
 * bytes, and is answered through its rewrite.
 */
static void
a_long_name_is_charged_per_predicate_named(void)
{
	static const size_t lengths[] = { 1, NAMED_LONG };
	text_t explained;
	diag_log_t log;
	char *program;
	char *query;
	bool answered;
	size_t i;

	for (i = 0; i < 2; i++) {
		program = named_calls_program(lengths[i], NAMED_CALLS, &query);
		if (program == NULL) {
			check(false, SCENARIO_LONG_NAME, "the program is built");
			return;
		}

		answered = explain_answer(program, query, 0, &explained, &log);
		if (lengths[i] == NAMED_LONG)
			checkf(answered && log.errors == 0 &&
			    log.warnings == 1 && strstr(log.last_warning,
			    "outgrew magic=22848 while adorning ") != NULL &&
			    !shows_rewrite(explained.bytes), SCENARIO_LONG_NAME,
			    "a head named by %zu bytes should abandon the "
			    "rewrite, which reported %zu errors and %zu "
			    "warnings", lengths[i], log.errors, log.warnings);
		else
			checkf(answered && log.errors == 0 &&
			    log.warnings == 0 && shows_rewrite(explained.bytes),
			    SCENARIO_LONG_NAME,
			    "a head named by %zu byte should be answered "
			    "through its rewrite, which reported %zu errors "
			    "and %zu warnings, the last \"%s\"", lengths[i],
			    log.errors, log.warnings, log.last_warning);

		free(explained.bytes);
		free(program);
		free(query);
	}
}

/*
 * calls_program()'s rule of `calls` calls on p, its head named by
 * `name_length` bytes, and in *queryp the query binding the head's column.
 * Returns the text, which the caller frees with *queryp, or NULL when
 * memory ran out.
 */
static char *
named_calls_program(size_t name_length, size_t calls, char **queryp)
{
	static const char facts[] = "e(1, 2).\ne(2, 3).\np(X) :- e(X, _Y).\n";
	text_t name;
	text_t text;
	text_t query;
	size_t i;

	bzero(&name, sizeof(name));
	for (i = 0; i < name_length; i++)
		text_append(&name, "r", 1);
	if (name.failed) {
		free(name.bytes);
		return (NULL);
	}

	bzero(&text, sizeof(text));
	text_append(&text, facts, strlen(facts));
	text_append(&text, name.bytes, name.length);
	text_append(&text, "(X) :- p(X)", strlen("(X) :- p(X)"));
	for (i = 1; i < calls; i++)
		text_append(&text, ", p(X)", strlen(", p(X)"));
	text_append(&text, ".\n", 2);

	bzero(&query, sizeof(query));
	text_append(&query, name.bytes, name.length);
	text_append(&query, "(1)?", strlen("(1)?"));
	free(name.bytes);

	if (text.failed || query.failed) {
		free(text.bytes);
		free(query.bytes);
		return (NULL);
	}

	*queryp = query.bytes;
	return (text.bytes);
}

/*
 * A query answered through the rewrite leaves only part of the fixed point
 * in the relations.  A later query with another binding, a later
 * materialization and a query answered from it must each see the whole
 * program, not that part.
 */
static void
what_the_rewrite_derives_does_not_leak(void)
{
	static const char program[] =
	    "edge(a, b).\n"
	    "edge(b, c).\n"
	    "edge(x, y).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n";
	static const char all_paths[] = "a b\na c\nb c\nx y\n";
	const relic_relation_t *paths;
	diag_log_t log;
	relic_t *ctx;
	relic_pred_id_t pred;
	char *rows;

	ctx = context_with(program, 0, 0, &log);
	if (ctx == NULL)
		return;

	rows = answer(ctx, "path(a, X)?");
	checkf(rows != NULL && strcmp(rows, "b\nc\n") == 0, SCENARIO_STATE,
	    "path(a, X)? should answer b and c, not\n%s",
	    rows != NULL ? rows : "(failed)");
	free(rows);

	rows = answer(ctx, "path(x, X)?");
	checkf(rows != NULL && strcmp(rows, "y\n") == 0, SCENARIO_STATE,
	    "path(x, X)? after path(a, X)? should answer y, not\n%s",
	    rows != NULL ? rows : "(failed)");
	free(rows);

	rows = answer(ctx, "path(X, Y)?");
	checkf(rows != NULL && strcmp(rows, all_paths) == 0, SCENARIO_STATE,
	    "path(X, Y)? after bound queries should answer every path, "
	    "not\n%s", rows != NULL ? rows : "(failed)");
	free(rows);

	checkf(relic_materialize(ctx) == 0, SCENARIO_STATE,
	    "the program should materialize after rewritten queries");

	paths = NULL;
	if (relic_pred_intern(ctx, "path", 4, 2, NULL, &pred) == 0)
		paths = relic_relation_read(ctx, pred);
	checkf(paths != NULL && relic_relation_count(paths) == 4,
	    SCENARIO_STATE,
	    "the materialized path relation should hold 4 tuples, not %zu",
	    paths != NULL ? relic_relation_count(paths) : 0);

	rows = answer(ctx, "path(a, X)?");
	checkf(rows != NULL && strcmp(rows, "b\nc\n") == 0, SCENARIO_STATE,
	    "path(a, X)? over the materialized program should answer b and "
	    "c, not\n%s", rows != NULL ? rows : "(failed)");
	free(rows);

	checkf(log.errors == 0 && log.warnings == 0, SCENARIO_STATE,
	    "the queries should report nothing, not %zu errors and %zu "
	    "warnings", log.errors, log.warnings);

	relic_destroy(ctx);
}

/*
 * Loading more of the program after a rewritten query must reach the next
 * query: a fact extending the graph, and a rule adding a way to derive path,
 * each change what path(a, X)? answers.
 */
static void
a_load_after_a_rewrite_reaches_the_next_query(void)
{
	static const char program[] =
	    "edge(a, b).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n";
	diag_log_t log;
	relic_t *ctx;
	char *rows;

	ctx = context_with(program, 0, 0, &log);
	if (ctx == NULL)
		return;

	rows = answer(ctx, "path(a, X)?");
	checkf(rows != NULL && strcmp(rows, "b\n") == 0, SCENARIO_STATE,
	    "path(a, X)? should answer b, not\n%s",
	    rows != NULL ? rows : "(failed)");
	free(rows);

	checkf(relic_load_string(ctx, "more.dl", "edge(b, c).\n") == 0,
	    SCENARIO_STATE, "a fact should load after a rewritten query");
	rows = answer(ctx, "path(a, X)?");
	checkf(rows != NULL && strcmp(rows, "b\nc\n") == 0, SCENARIO_STATE,
	    "path(a, X)? after loading edge(b, c) should answer b and c, "
	    "not\n%s", rows != NULL ? rows : "(failed)");
	free(rows);

	checkf(relic_load_string(ctx, "more.dl",
	    "hop(c, d).\npath(X, Y) :- hop(X, Y).\n") == 0, SCENARIO_STATE,
	    "a rule should load after a rewritten query");
	rows = answer(ctx, "path(a, X)?");
	checkf(rows != NULL && strcmp(rows, "b\nc\nd\n") == 0,
	    SCENARIO_STATE,
	    "path(a, X)? after loading a rule over hop should answer b, c "
	    "and d, not\n%s", rows != NULL ? rows : "(failed)");
	free(rows);

	checkf(log.errors == 0 && log.warnings == 0, SCENARIO_STATE,
	    "the loads and queries should report nothing, not %zu errors and "
	    "%zu warnings", log.errors, log.warnings);

	relic_destroy(ctx);
}

int
main(void)
{
	the_rewrite_answers_as_the_fixed_point_does();
	adornments_are_named_by_their_bound_columns();
	adornments_are_memoized();
	equal_generated_rules_are_kept_once();
	rules_differing_in_an_expression_are_both_kept();
	a_rule_deriving_its_own_head_is_dropped();
	the_limit_holds_at_its_value();
	every_search_is_charged_what_it_reads();
	a_rewrite_over_its_limit_is_abandoned();
	the_first_search_is_charged_what_it_reads();
	a_rewrite_charges_its_drafts_and_holds_what_it_keeps();
	a_declined_cut_is_charged_every_prefix();
	facts_behind_a_rule_fit_the_rewrites_limit();
	the_rewrite_grows_linearly_in_a_bodys_calls();
	a_long_bodys_rewrite_fits_the_limit();
	a_long_name_is_charged_per_predicate_named();
	what_the_rewrite_derives_does_not_leak();
	a_load_after_a_rewrite_reaches_the_next_query();

	return (check_summary("test_magic"));
}
