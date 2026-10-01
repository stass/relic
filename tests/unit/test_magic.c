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
 * itself with alternating bindings gives a finite rewrite, that rules equal
 * up to the names of their variables are generated once, that a rule
 * deriving its head from itself is dropped, and that the limits on generated
 * predicates and rules are enforced exactly at their values.  A rewrite that
 * outgrows the budget the library sets when no limit is configured is
 * abandoned and the query answered from the whole program, and so is one
 * whose generated literals outgrow theirs, the literals it reads to find
 * its adornments counted with those it drafts.  Last, what the rewrite
 * derives never leaks into a later answer or a later evaluation.
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

#define	SCENARIO_BUDGET		"a rewrite over its budget is abandoned"
#define	SCENARIO_DEDUP		"equal generated rules are kept once"
#define	SCENARIO_TREES		"rules equal but for an expression differ"
#define	SCENARIO_DROPPED	"dropped rules are charged to the budget"
#define	SCENARIO_EQUIVALENT	"the rewrite answers as the fixed point does"
#define	SCENARIO_FACTS		"facts behind a rule fit the literal budget"
#define	SCENARIO_LIMITS		"the rewrite's limits hold at their values"
#define	SCENARIO_LITERALS	"a rewrite over its literal budget is abandoned"
#define	SCENARIO_MEMO		"adornments are memoized"
#define	SCENARIO_LINEAR		"the rewrite grows linearly in a body's calls"
#define	SCENARIO_LONG_BODY	"a long body's rewrite fits the budgets"
#define	SCENARIO_NAMES		"adornments are named by their bound columns"
#define	SCENARIO_STATE		"what the rewrite derives does not leak"
#define	SCENARIO_TAUTOLOGY	"a rule deriving its own head is dropped"
#define	SCENARIO_SEARCHED	"the first search is charged what it reads"

/*
 * The generated graphs: GRAPH_NODES nodes and GRAPH_EDGES edges drawn from a
 * fixed linear congruential sequence per seed, so every run tests the same
 * graphs.  Small enough that the whole fixed point is cheap, dense enough to
 * have cycles and nodes nothing reaches.
 */
#define	GRAPH_SEEDS	24
#define	GRAPH_NODES	10
#define	GRAPH_EDGES	14

/*
 * A limit on generated predicates far above what any program here needs --
 * each has at most two binary predicates, so at most sixteen adorned and
 * magic ones -- which turns a rewrite that never ends into a failed query
 * rather than a hung test.
 */
#define	PREDS_GUARD	64

/* The ways shape_query() binds p's two columns. */
#define	QUERY_SHAPES	5

/* Room for a generated program and for a query's text. */
#define	PROGRAM_MAX	4096
#define	QUERY_MAX	64

typedef struct diag_log diag_log_t;
typedef struct rows rows_t;
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
static int	 collect_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static int	 compare_lines(const void *left, const void *right);
static relic_t	*context_configured(const char *program,
		    const relic_config_t *config, diag_log_t *log);
static relic_t	*context_with(const char *program, unsigned int flags,
		    size_t max_preds, size_t max_rules, diag_log_t *log);
static void	 count_generated(const char *explained, size_t *rulesp,
		    size_t *literalsp);
static size_t	 count_lines_starting(const char *text, const char *prefix);
static bool	 explain_answer(const char *program, const char *query_text,
		    size_t max_preds, size_t max_rules, text_t *explained,
		    diag_log_t *log);
static void	 graph_program(unsigned int seed, const char *rules,
		    char *program, size_t size);
static bool	 has_line(const char *text, const char *line);
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
static void	 a_rewrite_over_its_budget_is_abandoned(void);
static void	 a_rewrite_over_its_literal_budget_is_abandoned(void);
static void	 dropped_rules_are_charged_to_the_literal_budget(void);
static void	 the_first_search_is_charged_what_it_reads(void);
static void	 the_first_search_is_held_to_the_rule_limit(void);
static void	 facts_behind_a_rule_fit_the_literal_budget(void);
static void	 the_rewrite_grows_linearly_in_a_bodys_calls(void);
static void	 a_long_bodys_rewrite_fits_the_budgets(void);
static void	 a_load_after_a_rewrite_reaches_the_next_query(void);
static void	 the_limits_hold_at_their_values(void);
static void	 the_search_is_held_to_the_limits(void);
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
 * A context holding `program`, configured with `flags` and the two limits on
 * the rewrite, reporting its diagnostics to `log`.
 */
static relic_t *
context_with(const char *program, unsigned int flags, size_t max_preds,
    size_t max_rules, diag_log_t *log)
{
	relic_config_t config;

	bzero(&config, sizeof(config));
	config.flags = flags;
	config.max_magic_preds = max_preds;
	config.max_magic_rules = max_rules;

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
explain_answer(const char *program, const char *query_text, size_t max_preds,
    size_t max_rules, text_t *explained, diag_log_t *log)
{
	relic_t *ctx;
	char *rows;

	bzero(explained, sizeof(*explained));
	text_append(explained, "", 0);

	ctx = context_with(program, 0, max_preds, max_rules, log);
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

				magic = context_with(program, 0, PREDS_GUARD,
				    0, &magic_log);
				whole = context_with(program, RELIC_F_NO_MAGIC,
				    0, 0, &whole_log);
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
		if (!explain_answer(program, cases[i].query, 0, 0, &explained,
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

	if (!explain_answer(program, "swap(a, Y)?", PREDS_GUARD, 0, &explained,
	    &log)) {
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

	if (!explain_answer(program, "p(a, Z)?", 0, 0, &explained, &log)) {
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

	if (!explain_answer(program, "p(1, Z)?", 0, 0, &explained, &log)) {
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

	if (!explain_answer(program, "path(a, X)?", 0, 0, &explained,
	    &log)) {
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
 * path(a, X)? generates two predicates, path$bf and magic$path$bf, and keeps
 * four rules: the seed, the magic rule and the two adorned rules.  A limit
 * at each count is met and the query answered; one below it fails the query,
 * naming the limit and the predicate being adorned.
 */
static void
the_limits_hold_at_their_values(void)
{
	static const char program[] =
	    "edge(a, b).\n"
	    "edge(b, c).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n";
	text_t explained;
	diag_log_t log;
	bool answered;

	answered = explain_answer(program, "path(a, X)?", 2, 4, &explained,
	    &log);
	checkf(answered && log.errors == 0, SCENARIO_LIMITS,
	    "limits of two predicates and four rules should admit "
	    "path(a, X)?, which reported \"%s\"", log.last);
	free(explained.bytes);

	answered = explain_answer(program, "path(a, X)?", 1, 0, &explained,
	    &log);
	checkf(!answered && log.errors == 1 && strcmp(log.last,
	    "limit exceeded: magic-preds=1 while adorning path/2") == 0,
	    SCENARIO_LIMITS,
	    "a limit of one predicate should refuse path(a, X)?, which "
	    "reported \"%s\"", log.last);
	free(explained.bytes);

	answered = explain_answer(program, "path(a, X)?", 0, 3, &explained,
	    &log);
	checkf(!answered && log.errors == 1 && strcmp(log.last,
	    "limit exceeded: magic-rules=3 while adorning path/2") == 0,
	    SCENARIO_LIMITS,
	    "a limit of three rules should refuse path(a, X)?, which "
	    "reported \"%s\"", log.last);
	free(explained.bytes);
}

/*
 * p(c, X)? calls p bound, and q's last rule calls p free, so the first search
 * keeps p$bf, and r$bf which p's rules call, before it reads p free; it then
 * reads r free too, and the search runs again with both read free from the
 * start.  The finished rewrite generates four predicates, p$ff, r$ff, q$bf
 * and magic$q$bf.  The first search reaches the eight the rewrite without
 * subsumption would generate, three bound adornments with their magic
 * predicates and two free ones, and reads the eleven rules of those five
 * adornments.  The limits hold the search to those counts: at them the query
 * is answered through the subsumed rewrite, and one below either refuses it.
 *
 * An adornment a free one subsumes is generated by no rewrite and counted by
 * no limit.  path(X, Y)? reaches path free, and path's recursive rule then
 * calls it with a column bound, which path$ff subsumes: a limit of one
 * predicate admits the query, which reads path completely, though the
 * search reads both of path's rules.
 */
static void
the_search_is_held_to_the_limits(void)
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
	text_t explained;
	diag_log_t log;
	bool answered;

	answered = explain_answer(program, query, 8, 11, &explained, &log);
	checkf(answered && log.errors == 0 &&
	    has_line(explained.bytes, "    adorn p/2 bf, subsumed by p/2 ff") &&
	    has_line(explained.bytes, "    adorn q/2 bf") &&
	    count_lines_starting(explained.bytes, "    adorn ") == 5,
	    SCENARIO_LIMITS,
	    "limits of eight predicates and eleven rules should admit %s "
	    "through the subsumed rewrite, which reported \"%s\" and "
	    "showed\n%s", query, log.last, explained.bytes);
	free(explained.bytes);

	answered = explain_answer(program, query, 7, 0, &explained, &log);
	checkf(!answered && log.errors == 1 && strcmp(log.last,
	    "limit exceeded: magic-preds=7 while adorning r/2") == 0,
	    SCENARIO_LIMITS,
	    "a limit of seven predicates should refuse %s, which reported "
	    "\"%s\"", query, log.last);
	free(explained.bytes);

	answered = explain_answer(program, query, 0, 10, &explained, &log);
	checkf(!answered && log.errors == 1 && strcmp(log.last,
	    "limit exceeded: magic-rules=10 while adorning r/2") == 0,
	    SCENARIO_LIMITS,
	    "a limit of ten rules should refuse %s, which reported \"%s\"",
	    query, log.last);
	free(explained.bytes);

	answered = explain_answer(path_program, "path(X, Y)?", 1, 2,
	    &explained, &log);
	checkf(answered && log.errors == 0 &&
	    has_line(explained.bytes, "    adorn path/2 bf, subsumed by "
	    "path/2 ff") &&
	    has_line(explained.bytes, "    read completely: path/2"),
	    SCENARIO_LIMITS,
	    "limits of one predicate and two rules should admit path(X, Y)?, "
	    "which reported \"%s\" and showed\n%s", log.last,
	    explained.bytes);
	free(explained.bytes);
}

/* The arity of the widening program, whose query reaches 2^9 adornments. */
#define	WIDENING_ARITY	10

/*
 * With no limit configured, the widening program's rewrite outgrows its
 * budget long before its 2^9 adornments: it is abandoned with one warning
 * naming the budget and the predicate being adorned, no error, and the query
 * answered from the whole program, which explain shows as the program's
 * components with no rewrite.  A configured limit replaces the budget, so a
 * generous one lets the whole rewrite run.
 */
static void
a_rewrite_over_its_budget_is_abandoned(void)
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
		check(false, SCENARIO_BUDGET, "the widening program is built");
		return;
	}

	ctx = context_with(program, 0, 0, 0, &log);
	if (ctx == NULL) {
		free(program);
		return;
	}
	rows = answer(ctx, query);
	checkf(rows != NULL && strcmp(rows, expected) == 0, SCENARIO_BUDGET,
	    "the widening query should answer one row of c, not\n%s",
	    rows != NULL ? rows : "(failed)");
	free(rows);
	checkf(log.errors == 0 && log.warnings == 1 && strstr(log.last_warning,
	    "Magic rewrite outgrew its budget of ") != NULL &&
	    strstr(log.last_warning, "while adorning p/10; answering it from "
	    "the whole program") != NULL, SCENARIO_BUDGET,
	    "the abandoned rewrite should report one warning and no error, "
	    "not %zu errors and %zu warnings, the last \"%s\"", log.errors,
	    log.warnings, log.last_warning);
	relic_destroy(ctx);

	answered = explain_answer(program, query, 0, 0, &explained, &log);
	checkf(answered && strstr(explained.bytes, "magic rewrite of") ==
	    NULL, SCENARIO_BUDGET,
	    "an abandoned rewrite should not be explained, but -e showed\n%s",
	    explained.bytes);
	free(explained.bytes);

	answered = explain_answer(program, query, 1000000, 1000000, &explained,
	    &log);
	checkf(answered && log.warnings == 0 && strstr(explained.bytes,
	    "magic rewrite of") != NULL, SCENARIO_BUDGET,
	    "configured limits should replace the budget and let the rewrite "
	    "run, which reported %zu warnings", log.warnings);
	free(explained.bytes);

	free(program);
}

/*
 * The padded widening program: its query reaches 2^5 adornments, few
 * enough that the whole rewrite runs quickly, and every body is filled out
 * to 20 literals.
 */
#define	PADDED_ARITY		6
#define	PADDED_BODY_LENGTH	20

/*
 * The padded widening program has 7 rules of 20 literals, a budget of 256 +
 * 16 x 147 = 2608 generated literals, its size in literals counting each
 * body and one for its guard.  Its fact is f's, which no rule derives, so it
 * adds no base view.  Finding the adornments reads every rule of each whole,
 * and is charged what it reads, so the literals outgrow their budget: the
 * rewrite is abandoned with one warning naming them and the query answered
 * from the whole program.  Configuring
 * either Magic limit lifts both budgets, so a generous limit on predicates
 * alone, or on rules alone, lets the whole rewrite run.
 */
static void
a_rewrite_over_its_literal_budget_is_abandoned(void)
{
	static const char query[] = "p(c, Y2, Y3, Y4, Y5, Y6)?";
	static const char expected[] = "c c c c c\n";
	static const char warning[] =
	    "the query's Magic rewrite outgrew its budget of 2608 generated "
	    "literals while adorning p/6; answering it from the whole program";
	static const struct {
		size_t	max_preds;
		size_t	max_rules;
	} generous[] = {
		{ 1000000, 0 },
		{ 0, 1000000 }
	};
	char *program;
	text_t explained;
	diag_log_t log;
	relic_t *ctx;
	char *rows;
	bool answered;
	size_t i;

	program = padded_widening_program(PADDED_ARITY, PADDED_BODY_LENGTH);
	if (program == NULL) {
		check(false, SCENARIO_LITERALS,
		    "the padded widening program is built");
		return;
	}

	ctx = context_with(program, 0, 0, 0, &log);
	if (ctx == NULL) {
		free(program);
		return;
	}
	rows = answer(ctx, query);
	checkf(rows != NULL && strcmp(rows, expected) == 0, SCENARIO_LITERALS,
	    "the padded widening query should answer one row of c, not\n%s",
	    rows != NULL ? rows : "(failed)");
	free(rows);
	checkf(log.errors == 0 && log.warnings == 1 &&
	    strcmp(log.last_warning, warning) == 0, SCENARIO_LITERALS,
	    "the abandoned rewrite should report one warning naming its "
	    "literals and no error, not %zu errors and %zu warnings, the last "
	    "\"%s\"", log.errors, log.warnings, log.last_warning);
	relic_destroy(ctx);

	for (i = 0; i < sizeof(generous) / sizeof(generous[0]); i++) {
		answered = explain_answer(program, query,
		    generous[i].max_preds, generous[i].max_rules, &explained,
		    &log);
		checkf(answered && log.errors == 0 && log.warnings == 0 &&
		    strstr(explained.bytes, "magic rewrite of") != NULL,
		    SCENARIO_LITERALS,
		    "limits of %zu predicates and %zu rules should lift the "
		    "budgets and let the rewrite run, which reported %zu "
		    "errors and %zu warnings, the last \"%s\"",
		    generous[i].max_preds, generous[i].max_rules, log.errors,
		    log.warnings, log.last_warning);
		free(explained.bytes);
	}

	free(program);
}

/*
 * The self-calling program's arities: one whose adornments the first search
 * reads within the literal budget, and one whose adornments it does not.
 * And its long rule's calls on f.
 */
#define	SELF_CALLING_ARITY	5
#define	SELF_CALLING_WIDE_ARITY	11
#define	SELF_CALLING_LENGTH	2000

/*
 * Every adornment of the self-calling program's p drafts two rules of some
 * two thousand literals and keeps neither.  Counting only the rules kept, a
 * rewrite would see a few short rules per adornment and complete, however
 * much its drafts had cost.  The budget charges every draft.  At arity 5 the
 * program's size in literals is 6 + 5 x 3 + 2002 + 200 x 2 = 2423, and its
 * literal budget 256 + 16 x 2423 = 39024.  The first search reads the rules of
 * p's 16 adornments, 16 x 2016 = 32256 literals, within the budget, so the
 * rewrite is abandoned while it drafts them, and the query answered from the
 * whole program.
 */
static void
dropped_rules_are_charged_to_the_literal_budget(void)
{
	static const char query[] = "p(c, Y2, Y3, Y4, Y5)?";
	static const char expected[] = "c c c c\n";
	static const char warning[] =
	    "the query's Magic rewrite outgrew its budget of 39024 generated "
	    "literals while adorning p/5; answering it from the whole program";
	char *program;
	diag_log_t log;
	relic_t *ctx;
	char *rows;

	program = self_calling_program(SELF_CALLING_ARITY,
	    SELF_CALLING_LENGTH);
	if (program == NULL) {
		check(false, SCENARIO_DROPPED,
		    "the self-calling program is built");
		return;
	}

	ctx = context_with(program, 0, 0, 0, &log);
	free(program);
	if (ctx == NULL)
		return;

	rows = answer(ctx, query);
	checkf(rows != NULL && strcmp(rows, expected) == 0, SCENARIO_DROPPED,
	    "the self-calling query should answer one row of c, not\n%s",
	    rows != NULL ? rows : "(failed)");
	free(rows);
	checkf(log.errors == 0 && log.warnings == 1 &&
	    strcmp(log.last_warning, warning) == 0, SCENARIO_DROPPED,
	    "the rewrite should be abandoned with one warning naming its "
	    "literals and no error, not %zu errors and %zu warnings, the "
	    "last error \"%s\" and warning \"%s\"", log.errors, log.warnings,
	    log.last, log.last_warning);

	relic_destroy(ctx);
}

/*
 * The search that finds the adornments reads every rule of each, and is
 * charged the literals it reads against the literal budget, so a query
 * reaching more adornments than the budget can read is abandoned before a
 * rule is drafted.  At arity 11 the self-calling program's p has 2^10
 * adornments, each with rules of 2016 literals between them, and a literal
 * budget of 256 + 16 x 2447 = 39408: read whole, its adornments would cost
 * some two million literals.  The rewrite is driven step by step, as a query
 * drives it, to show where it stops: in the first search, having read no more
 * than the budget and drafted nothing.  f has no rule, so nothing need be
 * evaluated before the rewrite.
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

	ctx = context_with(program, 0, 0, 0, &log);
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

	checkf(status != 0 && magic.over_budget, SCENARIO_SEARCHED,
	    "the rewrite should be abandoned over its budget, and %s",
	    status == 0 ? "succeeded" : "failed otherwise");
	checkf(magic.generated_literals == 0 &&
	    magic.searched_literals <= magic.literal_budget &&
	    magic.literal_budget == 39408, SCENARIO_SEARCHED,
	    "the first search should read at most its budget of 39408 literals "
	    "and draft none, and read %zu of %zu and drafted %zu",
	    magic.searched_literals, magic.literal_budget,
	    magic.generated_literals);
	checkf(magic.kept.count < 1024, SCENARIO_SEARCHED,
	    "the first search should stop short of p's 1024 adornments, and "
	    "found %zu", magic.kept.count);

	relic_magic_reset(&magic);
	relic_query_destroy(query);
	relic_destroy(ctx);
}

/*
 * The arity of the widening program the rule limit is tested against, whose
 * query reaches 2^11 adornments, each read through 13 rules.
 */
#define	LIMITED_ARITY	12

/* The rule limit that program is held to. */
#define	LIMITED_RULES	50

/*
 * A configured limit lifts the budgets, and the rule limit then bounds what
 * the search that finds the adornments reads, as the budget does otherwise.
 * The widening program's query reaches 2^11 adornments; with only the rule
 * limit set, the rewrite is refused in the first search, having read no
 * more rules than the limit, drafted nothing and named no predicate.
 */
static void
the_first_search_is_held_to_the_rule_limit(void)
{
	static const char text[] =
	    "p(c, Y2, Y3, Y4, Y5, Y6, Y7, Y8, Y9, Y10, Y11, Y12)?";
	relic_query_t *query;
	relic_magic_t magic;
	diag_log_t log;
	relic_t *ctx;
	char *program;
	size_t preds_before;
	int status;

	program = widening_program(LIMITED_ARITY);
	if (program == NULL) {
		check(false, SCENARIO_SEARCHED,
		    "the widening program is built");
		return;
	}

	ctx = context_with(program, 0, 0, LIMITED_RULES, &log);
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

	preds_before = relic_pred_count(ctx);
	relic_magic_init(&magic, ctx);
	status = relic_magic_find_complete(&magic, query);
	if (status == 0)
		status = relic_magic_rewrite(&magic, &query->rule);

	checkf(status != 0 && !magic.over_budget && log.errors == 1 &&
	    strcmp(log.last, "limit exceeded: magic-rules=50 while adorning "
	    "p/12") == 0, SCENARIO_SEARCHED,
	    "the rewrite should be refused by the rule limit, which reported "
	    "\"%s\"", log.last);
	checkf(magic.generated_literals == 0 &&
	    magic.searched_rules <= LIMITED_RULES, SCENARIO_SEARCHED,
	    "the first search should read at most %d rules and draft none, and "
	    "read %zu and drafted %zu literals", LIMITED_RULES,
	    magic.searched_rules, magic.generated_literals);
	checkf(magic.kept.count < 2048 &&
	    relic_pred_count(ctx) == preds_before, SCENARIO_SEARCHED,
	    "the first search should stop short of p's 2048 adornments and "
	    "name none, and found %zu and named %zu predicates",
	    magic.kept.count, relic_pred_count(ctx) - preds_before);

	relic_magic_reset(&magic);
	relic_query_destroy(query);
	relic_destroy(ctx);
}

/* How many facts of e the mixed program holds. */
#define	MIXED_FACTS	1000

/*
 * A predicate with facts and a rule alike is read on demand, and each of
 * its adornments reads the facts through one rule over its base view,
 * e(X, Y) :- e$base(X, Y), which the budget counts as two literals.  A
 * thousand facts of e behind one symmetric rule are rewritten for e(n7, X)?
 * without a warning, and the rewrite copies none of them: the rewritten
 * program holds a few rules, not a thousand.  Its plans read the base store
 * under the base view's name, since e names the derived relation.
 */
static void
facts_behind_a_rule_fit_the_literal_budget(void)
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

	answered = explain_answer(program.bytes, "e(n7, X)?", 0, 0,
	    &explained, &log);
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
 * generous limit on rules lifts the budgets, which would otherwise abandon a
 * quadratic rewrite before it could be measured.
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

		answered = explain_answer(program, "r(1)?", 0, 1000000,
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

/* The calls in the long body of a_long_bodys_rewrite_fits_the_budgets(). */
#define	LONG_BODY_CALLS		400

/*
 * A rewrite that specializes the program fits its budgets however few rules
 * the program has.  r(X) :- p(X), ..., p(X) with 400 calls and the rule
 * deriving p are 2 rules and 2 + 401 = 403 literals, a literal budget of
 * 256 + 16 x 403 = 6704.  The one adornment of r keeps a magic rule and a
 * supplementary rule per call, over 800 rules of one or two literals each,
 * well within the literal budget, which bounds the rules kept as well: a
 * budget of 256 + 16 x 2 rules would abandon it.
 */
static void
a_long_bodys_rewrite_fits_the_budgets(void)
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

	answered = explain_answer(program, "r(1)?", 0, 0, &explained, &log);
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
	uint32_t symbol;
	relic_pred_id_t pred;
	char *rows;

	ctx = context_with(program, 0, 0, 0, &log);
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
	if (relic_symbol_intern(ctx, "path", 4, &symbol) == 0 &&
	    relic_pred_intern(ctx, symbol, 2, NULL, &pred) == 0)
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

	ctx = context_with(program, 0, 0, 0, &log);
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
	the_limits_hold_at_their_values();
	the_search_is_held_to_the_limits();
	a_rewrite_over_its_budget_is_abandoned();
	a_rewrite_over_its_literal_budget_is_abandoned();
	dropped_rules_are_charged_to_the_literal_budget();
	the_first_search_is_charged_what_it_reads();
	the_first_search_is_held_to_the_rule_limit();
	facts_behind_a_rule_fit_the_literal_budget();
	the_rewrite_grows_linearly_in_a_bodys_calls();
	a_long_bodys_rewrite_fits_the_budgets();
	what_the_rewrite_derives_does_not_leak();
	a_load_after_a_rewrite_reaches_the_next_query();

	return (check_summary("test_magic"));
}
