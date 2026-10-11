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
#define	SCENARIO_CHAIN		"a bound chain derives linearly"
#define	SCENARIO_DECLINED	"a declined cut is charged every prefix"
#define	SCENARIO_DEDUP		"equal generated rules are kept once"
#define	SCENARIO_TREES		"rules equal but for an expression differ"
#define	SCENARIO_DROPPED	"dropped rules are charged literals"
#define	SCENARIO_EQUIVALENT	"the rewrite answers as the fixed point does"
#define	SCENARIO_FACTORED	"factoring answers as its references do"
#define	SCENARIO_FACTOR_FITS	"factoring fits the rule representation"
#define	SCENARIO_FACTOR_SAME	"a factored rewrite is deterministic"
#define	SCENARIO_FACTS		"facts behind a rule fit the rewrite's limit"
#define	SCENARIO_LIMIT		"the rewrite's limit holds at its value"
#define	SCENARIO_MEMO		"adornments are memoized"
#define	SCENARIO_LINEAR		"the rewrite grows linearly in a body's calls"
#define	SCENARIO_LONG_BODY	"a long body's rewrite fits the limit"
#define	SCENARIO_LONG_NAME	"a long name is charged per predicate named"
#define	SCENARIO_NAMES		"adornments are named by their bound columns"
#define	SCENARIO_REJECTED	"a rejected certificate fits the default limit"
#define	SCENARIO_STATE		"what the rewrite derives does not leak"
#define	SCENARIO_TAUTOLOGY	"a rule deriving its own head is dropped"
#define	SCENARIO_SEARCHED	"the first search is charged what it reads"
#define	SCENARIO_SEARCHES	"every search is charged what it reads"
#define	SCENARIO_KEPT		"a rewrite holds only what it keeps"
#define	SCENARIO_MENTIONS	"a generated rule is planned from its mentions"

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
static char	*answer_rows(relic_t *ctx, const char *query_text, bool sort);
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
static bool	 explain_with(const char *program, const char *query_text,
		    unsigned int flags, size_t max_magic, text_t *explained,
		    diag_log_t *log);
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
static void	 a_rejected_certificate_fits_the_default_limit(void);
static void	 every_mask_query(unsigned int arity, text_t *text);
static void	 a_long_name_is_charged_per_predicate_named(void);
static void	 a_bound_chain_derives_linearly(void);
static void	 factoring_answers_as_its_references_do(void);
static void	 walk_of(const char *adorn, char *walk, size_t size);
static void	 factoring_fits_the_rule_representation(void);
static void	 check_factoring_fits(const char *what, const char *program,
		    const char *query_text, const char *adorn);
static char	*wide_bound_program(unsigned int bound);
static char	*many_slots_program(unsigned int base_slots);
static void	 a_factored_rewrite_is_deterministic(void);
static void	 a_walk_is_charged_its_name(void);
static char	*named_closure_program(size_t name_length, char **queryp);
static size_t	 least_admitted_limit(const char *program,
		    const char *query_text, unsigned int flags);
static void	 an_abandoned_factored_rewrite_leaves_its_context_usable(void);
static void	 a_load_after_a_rewrite_reaches_the_next_query(void);
static void	 a_generated_rule_is_planned_from_its_mentions(void);
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
	return (answer_rows(ctx, query_text, true));
}

/* answer(), its rows in the order they were delivered unless `sort`. */
static char *
answer_rows(relic_t *ctx, const char *query_text, bool sort)
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
	if (sort && rows.count > 0)
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
	return (explain_with(program, query_text, 0, max_magic, explained,
	    log));
}

/* explain_answer() in a context configured with `flags`. */
static bool
explain_with(const char *program, const char *query_text, unsigned int flags,
    size_t max_magic, text_t *explained, diag_log_t *log)
{
	relic_t *ctx;
	char *rows;

	bzero(explained, sizeof(*explained));
	text_append(explained, "", 0);

	ctx = context_with(program, flags, max_magic, log);
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
 * path with a column bound.  A call binding path's first column factors,
 * since the recursive rule passes the second through; one binding only the
 * second does not.
 */
static void
adornments_are_named_by_their_bound_columns(void)
{
	static const struct {
		const char	*query;
		const char	*adorn;
		const char	*seed;
	} cases[] = {
		{ "path(a, X)?", "    adorn path/2 bf, factored",
		    "    seed magic$path$bf(a)." },
		{ "path(X, c)?", "    adorn path/2 fb",
		    "    seed magic$path$fb(c)." },
		{ "path(a, c)?", "    adorn path/2 bb, factored",
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
 * Unfactored, path(a, X)? costs its rewrite 12 literals.  The search reads
 * the query's one literal and path's two rules for path$bf, 1 + 1 + 2.  The
 * draft writes the rewritten query, 1, the seed, which has no body, the
 * magic rule of the recursive call, 2, and the two adorned rules with their
 * guards, 2 and 3.  A limit of 12 admits the rewrite.  One of 11 abandons it
 * at the last adorned rule, with a warning naming the limit and the
 * predicate being adorned and no error, and the query is answered from the
 * whole program.
 *
 * Factored, it costs 10.  The search certifies path$bf, reading the same
 * 1 + 1 + 2, and the draft writes the rewritten query and its seed, 1, the
 * walk's seed, 1, the answer rule of the base rule, 2, and the transition
 * of the recursive one, 2.  A limit of 10 admits it, and one of 9 abandons
 * it at the transition.
 *
 * A certificate a rule fails is charged what it read, and the walks that
 * follow are charged every body again.  p's recursive rule observes its
 * output, so p(a, X)? reads p's rules twice, 1 + 3 each, beside the query's
 * literal, and drafts the rewritten query, 1, the magic rule of the
 * recursive call, 2, and the two adorned rules with their guards, 2 and 4:
 * 18, where the unfactored rewrite, which reads them once, costs 14.
 *
 * A second search replays the certificate the first found, charged as the
 * walks it would otherwise replay are.  The query reaches r bound before
 * free, so the search runs twice.  The first reads the query's three
 * literals, p's rules for the certificate of p$bf, 1 + 2, r's for the
 * certificate of r$bf, which the left-recursive rule fails, and again for
 * its walks, 1 + 2 each, and r's for r$ff, 1 + 2: 15.  The second reads
 * the query and p's and r's rules once more for p$bf and r$ff, 9.  The
 * draft writes the query's seed and the rewritten query, 1 + 3, p's walk
 * seed, transition and answer rule, 1 + 2 + 2, and r$ff's two rules,
 * 1 + 2: 12.  So the rewrite costs 36, and were the replay charged
 * nothing, 33 would admit it.
 *
 * A query of two calls outgrows a limit of one literal in its own walk,
 * before any predicate is adorned, and the warning names the query.
 */
static void
the_limit_holds_at_its_value(void)
{
	static const char path_program[] =
	    "edge(a, b).\n"
	    "edge(b, c).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n";
	static const char observing_program[] =
	    "edge(a, b).\n"
	    "edge(b, c).\n"
	    "p(X, Y) :- edge(X, Y).\n"
	    "p(X, Z) :- edge(X, Y), p(Y, W), edge(W, Z).\n";
	static const char replaying_program[] =
	    "edge(a, b).\n"
	    "edge(b, c).\n"
	    "edge(c, d).\n"
	    "p(X, Y) :- edge(X, Y).\n"
	    "p(X, Z) :- edge(X, Y), p(Y, Z).\n"
	    "r(X, Y) :- edge(X, Y).\n"
	    "r(X, Z) :- r(X, Y), edge(Y, Z).\n";
	static const struct {
		const char	*program;
		const char	*query;
		unsigned int	 flags;
		size_t		 admitted;
		const char	*adorning;
	} cases[] = {
		{ path_program, "path(a, X)?", RELIC_F_NO_FACTOR, 12,
		    "path/2" },
		{ path_program, "path(a, X)?", 0, 10, "path/2" },
		{ observing_program, "p(a, X)?", RELIC_F_NO_FACTOR, 14,
		    "p/2" },
		{ observing_program, "p(a, X)?", 0, 18, "p/2" },
		{ replaying_program, "p(a, X), r(b, Y), r(Z, W)?", 0, 36,
		    "r/2" }
	};
	static const char rewriting[] =
	    "the query's Magic rewrite outgrew magic=1 while rewriting the "
	    "query; answering it from the whole program";
	char outgrown[256];
	text_t explained;
	diag_log_t log;
	size_t limit;
	size_t i;
	bool answered;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		limit = cases[i].admitted;
		answered = explain_with(cases[i].program, cases[i].query,
		    cases[i].flags, limit, &explained, &log);
		checkf(answered && log.errors == 0 && log.warnings == 0 &&
		    shows_rewrite(explained.bytes), SCENARIO_LIMIT,
		    "a limit of %zu should admit the rewrite of %s with flags "
		    "%#x, which reported %zu errors and %zu warnings, the "
		    "last \"%s\"", limit, cases[i].query, cases[i].flags,
		    log.errors, log.warnings, log.last_warning);
		free(explained.bytes);

		limit--;
		snprintf(outgrown, sizeof(outgrown), "the query's Magic "
		    "rewrite outgrew magic=%zu while adorning %s; answering it "
		    "from the whole program", limit, cases[i].adorning);
		answered = explain_with(cases[i].program, cases[i].query,
		    cases[i].flags, limit, &explained, &log);
		checkf(answered && log.errors == 0 && log.warnings == 1 &&
		    strcmp(log.last_warning, outgrown) == 0 &&
		    !shows_rewrite(explained.bytes), SCENARIO_LIMIT,
		    "a limit of %zu should abandon the rewrite of %s with "
		    "flags %#x and answer it from the whole program, which "
		    "reported %zu errors and %zu warnings, the last \"%s\"",
		    limit, cases[i].query, cases[i].flags, log.errors,
		    log.warnings, log.last_warning);
		free(explained.bytes);
	}

	answered = explain_answer(path_program, "path(a, X), path(X, Y)?", 1,
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
 * that limit: 1024 + 64 x 2048, the program's 2047 literals and the query's
 * one.
 */
#define	SELF_CALLING_WIDE_ARITY	11
#define	SELF_CALLING_LENGTH	2000
#define	SELF_CALLING_WIDE_LIMIT	132096

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
 * rewrite: the default, 1024 + 64 x 205 for the program's 204 literals and
 * the query's one, and a larger one that still admits less than the rewrite
 * costs.
 */
#define	DECLINED_CALLS		200
#define	DECLINED_DEFAULT	14144
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
	    strstr(log.last_warning, "outgrew magic=14144 while adorning "
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
 * query's one a default of 1024 + 64 x 404 = 26880.  The one adornment of r
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

/* The columns of p in a_rejected_certificate_fits_the_default_limit(). */
#define	REJECTED_ARITY		7

/*
 * A rejected certificate's rules are read twice, to certify and to walk,
 * and the default limit allows for it.  p's three rules hold bodies of 8, 9
 * and 8 atoms and a comparison of size 3, 31 literals with their guards,
 * and the query calls p under each of its 127 masks binding a column, for
 * 158 and a default of 1024 + 64 x 158 = 11136.  The pad atoms make each
 * reading of p's rules cost as much as the program's size beside the long
 * query.  An adornment leaving a column free fails its certificate at the
 * recursive rule, whose call changes that column; the one binding every
 * column passes it there and fails at the comparison, in the last rule.
 * So the first search charges the bodies of every adornment before walking
 * them, and the rewrite needs some 10,400 where the unfactored one needs
 * some 8,300.  Both are rewritten, with no warning and no adornment
 * factored, and answer as the whole program does.
 */
static void
a_rejected_certificate_fits_the_default_limit(void)
{
	static const char program[] =
	    "e(a, a, a, a, a, a, a, b, b, b, b, b, b, b).\n"
	    "s(b, b, b, b, b, b, b).\npad(a).\npad(b).\n"
	    "p(X1, X2, X3, X4, X5, X6, X7) :- s(X1, X2, X3, X4, X5, X6, X7),"
	    " pad(X1), pad(X2), pad(X3), pad(X4), pad(X5), pad(X6),"
	    " pad(X7).\n"
	    "p(X1, X2, X3, X4, X5, X6, X7) :-"
	    " e(X1, X2, X3, X4, X5, X6, X7, Y1, Y2, Y3, Y4, Y5, Y6, Y7),"
	    " pad(X1), pad(X2), pad(X3), pad(X4), pad(X5), pad(X6),"
	    " pad(X7), p(Y1, Y2, Y3, Y4, Y5, Y6, Y7).\n"
	    "p(X1, X2, X3, X4, X5, X6, X7) :- s(X1, X2, X3, X4, X5, X6, X7),"
	    " pad(X1), pad(X2), pad(X3), pad(X4), pad(X5), pad(X6),"
	    " pad(X7), X1 != zz.\n";
	static const unsigned int flags[] = { 0, RELIC_F_NO_FACTOR };
	text_t query_text;
	text_t explained;
	diag_log_t log;
	relic_t *ctx;
	char *expected;
	char *rows;
	size_t f;

	every_mask_query(REJECTED_ARITY, &query_text);
	if (query_text.failed) {
		check(false, SCENARIO_REJECTED, "the query is built");
		free(query_text.bytes);
		return;
	}

	ctx = context_with(program, RELIC_F_NO_MAGIC, 0, &log);
	expected = ctx != NULL ? answer(ctx, query_text.bytes) : NULL;
	relic_destroy(ctx);

	for (f = 0; f < sizeof(flags) / sizeof(flags[0]); f++) {
		bzero(&explained, sizeof(explained));
		text_append(&explained, "", 0);
		ctx = context_with(program, flags[f], 0, &log);
		if (ctx != NULL)
			relic_explain_set(ctx, record_explained, &explained);
		rows = ctx != NULL ? answer(ctx, query_text.bytes) : NULL;
		relic_destroy(ctx);

		checkf(rows != NULL && log.errors == 0 && log.warnings == 0 &&
		    !explained.failed && shows_rewrite(explained.bytes),
		    SCENARIO_REJECTED, "the 127 calls on p with flags %#x "
		    "should be rewritten within the default limit, and "
		    "reported %zu errors and %zu warnings, the last \"%s\"",
		    flags[f], log.errors, log.warnings,
		    log.errors != 0 ? log.last : log.last_warning);
		checkf(!explained.failed &&
		    has_line(explained.bytes, "    adorn p/7 bbbbbbb") &&
		    strstr(explained.bytes, ", factored") == NULL,
		    SCENARIO_REJECTED, "with flags %#x no adornment of p "
		    "should factor, the one binding every column among them, "
		    "but -e showed\n%.2000s", flags[f], explained.bytes);
		checkf(expected != NULL && rows != NULL &&
		    strcmp(rows, expected) == 0, SCENARIO_REJECTED,
		    "with flags %#x the 127 calls answered\n%s\nwhere the "
		    "whole program answered\n%s", flags[f],
		    rows != NULL ? rows : "(failed)",
		    expected != NULL ? expected : "(failed)");

		free(rows);
		free(explained.bytes);
	}
	free(expected);
	free(query_text.bytes);
}

/*
 * In *text, a query calling p of `arity` columns once under each mask
 * binding a column: a in the bound columns and _ in the free ones, so the
 * calls reach every adornment of p but the free one.  text->failed is set
 * when memory ran out.
 */
static void
every_mask_query(unsigned int arity, text_t *text)
{
	unsigned int mask;
	unsigned int column;

	bzero(text, sizeof(*text));
	for (mask = 1; mask < 1u << arity; mask++) {
		if (mask > 1)
			text_append(text, ", ", 2);
		text_append(text, "p(", 2);
		for (column = 0; column < arity; column++) {
			if (column > 0)
				text_append(text, ", ", 2);
			text_append(text, (mask & (1u << column)) != 0 ?
			    "a" : "_", 1);
		}
		text_append(text, ")", 1);
	}
	text_append(text, "?", 1);
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
 * some 51000 between them, past the default of 1024 + 64 x 460 = 30464 for
 * a program of 459, which counts the head's name once, and a query of one.
 * The rewrite is abandoned and the query answered from the whole program.
 * The same rule under a one-byte name costs nothing for its names, each
 * shorter than 64 bytes, and is answered through its rewrite.
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
			    "outgrew magic=30464 while adorning ") != NULL &&
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

/* The edges of the chain a_bound_chain_derives_linearly() closes. */
#define	CHAIN_EDGES	1000

/*
 * A right-recursive closure over a chain n0 -> n1 -> ... of CHAIN_EDGES
 * edges, queried from n0, answers every later node.  Factored, the rewrite
 * walks from n0 alone: one walk tuple and one answer per node reached, a
 * few derivations per edge, and at most 8N + 64 between the seeds, the
 * walk, the answers and the query's rows.  The ordinary rewrite derives
 * path$bf for every node the demand reaches, every suffix of the chain,
 * N(N + 1)/2 tuples, which no linear bound admits.  Explain shows the
 * adornment factored, so the bound is met by the transformation rather
 * than by a planner that happens to do less, and RELIC_F_NO_FACTOR, which
 * answers the same, passes the bound, so the bound tells the two apart.
 */
static void
a_bound_chain_derives_linearly(void)
{
	static const unsigned int flags[] = { 0, RELIC_F_NO_FACTOR };
	char line[64];
	relic_stats_t stats;
	text_t explained;
	text_t program;
	diag_log_t log;
	relic_t *ctx;
	char *rows;
	size_t answered;
	uint64_t bound;
	unsigned int i;
	bool factors;
	size_t f;

	bzero(&program, sizeof(program));
	for (i = 0; i < CHAIN_EDGES; i++) {
		snprintf(line, sizeof(line), "edge(n%u, n%u).\n", i, i + 1);
		text_append(&program, line, strlen(line));
	}
	text_append(&program, "path(X, Y) :- edge(X, Y).\n",
	    strlen("path(X, Y) :- edge(X, Y).\n"));
	text_append(&program, "path(X, Z) :- edge(X, Y), path(Y, Z).\n",
	    strlen("path(X, Z) :- edge(X, Y), path(Y, Z).\n"));
	if (program.failed) {
		free(program.bytes);
		check(false, SCENARIO_CHAIN, "the chain program is built");
		return;
	}

	bound = 8 * (uint64_t)CHAIN_EDGES + 64;
	for (f = 0; f < sizeof(flags) / sizeof(flags[0]); f++) {
		factors = (flags[f] & RELIC_F_NO_FACTOR) == 0;
		ctx = context_with(program.bytes, flags[f], 0, &log);
		if (ctx == NULL)
			break;

		bzero(&explained, sizeof(explained));
		text_append(&explained, "", 0);
		relic_explain_set(ctx, record_explained, &explained);
		rows = answer(ctx, "path(n0, X)?");
		relic_stats(ctx, &stats);

		answered = rows != NULL ? count_lines_starting(rows, "n") : 0;
		checkf(answered == CHAIN_EDGES && log.errors == 0 &&
		    log.warnings == 0, SCENARIO_CHAIN,
		    "path(n0, X)? with flags %#x should answer the chain's %d "
		    "later nodes, and answered %zu with %zu errors and %zu "
		    "warnings", flags[f], CHAIN_EDGES, answered, log.errors,
		    log.warnings);

		checkf((stats.derivations <= bound) == factors, SCENARIO_CHAIN,
		    "path(n0, X)? over a chain of %d edges with flags %#x "
		    "should derive %s %llu tuples, not %llu", CHAIN_EDGES,
		    flags[f], factors ? "at most" : "more than",
		    (unsigned long long)bound,
		    (unsigned long long)stats.derivations);
		checkf(!explained.failed && has_line(explained.bytes,
		    "    adorn path/2 bf, factored") == factors,
		    SCENARIO_CHAIN, "path(n0, X)? with flags %#x should %s "
		    "path/2 bf, but -e showed\n%.2000s", flags[f],
		    factors ? "factor" : "not factor", explained.bytes);

		free(rows);
		free(explained.bytes);
		relic_destroy(ctx);
	}
	free(program.bytes);
}

/* The most queries per program in factoring_answers_as_its_references_do(). */
#define	FACTOR_QUERIES	3

/*
 * Over generated graphs, every query below answers through the default
 * rewrite as it does through the unfactored one and from the naive whole
 * fixed point, and -e shows the adornment named beside it: factored where
 * the program passes the certificate, ordinary or subsumed where it does
 * not, or nothing in particular where the query reads what it reaches
 * completely.  The structure does not depend on the facts, so it is
 * checked on the first graph.
 *
 * The factored programs give the base and the transition different
 * relations, base rules with constants and repeated variables in their
 * heads, origins that are no answer of their own, several recursive and
 * base rules beside facts, several origins and requests that grow from
 * earlier answers, bound columns that do not lead, a state of two columns
 * requested by origins that share one of them, a repeated state and
 * repeated outputs.  The others break one check each, one of them in a
 * single rule beside valid ones: an output the recursive call repeats, a `_`
 * or a variable only the recursive call holds in its bound columns,
 * an observed or a changed output, a second recursive call, mutual
 * recursion, a comparison, a negation, a call on a predicate read on
 * demand, or a free call that subsumes the bound one.  One query reaches a
 * free call only in a second search, which replays p$bf's certificate.
 */
static void
factoring_answers_as_its_references_do(void)
{
	static const char closure[] =
	    "p(X, Y) :- edge(X, Y).\n"
	    "p(X, Z) :- edge(X, Y), p(Y, Z).\n";
	static const struct {
		const char	*name;
		const char	*rules;
		const char	*extra;
		struct {
			const char	*text;
			const char	*adorn;
		} queries[FACTOR_QUERIES];
	} programs[] = {
		{ "closure", closure, "", {
		    { "p(n0, X)?", "    adorn p/2 bf, factored" },
		    { "p(n3, n3)?", "    adorn p/2 bb, factored" } } },
		{ "distinct-base", "p(X, Y) :- edge(Y, X), mark(Y).\n"
		    "p(X, Z) :- edge(X, Y), mark(X), p(Y, Z).\n", "", {
		    { "p(n1, X)?", "    adorn p/2 bf, factored" },
		    { "p(n2, X)?", "    adorn p/2 bf, factored" } } },
		{ "marked-ends", "p(X, X) :- mark(X).\n"
		    "p(X, Z) :- edge(X, Y), p(Y, Z).\n", "", {
		    { "p(n0, X)?", "    adorn p/2 bf, factored" },
		    { "p(n1, X)?", "    adorn p/2 bf, factored" } } },
		{ "several", "p(n0, n5).\np(n7, n7).\n"
		    "p(X, Y) :- edge(X, Y), mark(Y).\n"
		    "p(X, Y) :- edge(Y, X), mark(X).\n"
		    "p(X, Z) :- edge(X, Y), p(Y, Z).\n"
		    "p(X, Z) :- edge(Y, X), node(Y), p(Y, Z).\n", "", {
		    { "p(n0, X)?", "    adorn p/2 bf, factored" },
		    { "p(n4, X)?", "    adorn p/2 bf, factored" } } },
		{ "origins", closure, "", {
		    { "mark(S), p(S, T)?", "    adorn p/2 bf, factored" },
		    { "p(n0, X), p(X, n2)?", "    adorn p/2 bb, factored" },
		    { "p(n0, X), p(X, Y)?", "    adorn p/2 bf, factored" } } },
		{ "growing", closure,
		    "r(X, Y) :- p(X, Y), mark(Y).\n"
		    "r(X, Z) :- r(X, Y), p(Y, Z), mark(Z).\n", {
		    { "r(n0, X)?", "    adorn p/2 bf, factored" } } },
		{ "second-column", "p(Y, X) :- edge(X, Y).\n"
		    "p(Z, X) :- edge(X, Y), p(Z, Y).\n", "", {
		    { "p(X, n0)?", "    adorn p/2 fb, factored" },
		    { "p(n1, X)?", "    adorn p/2 bf" } } },
		{ "constants", "p(n1, Y) :- edge(n2, Y).\n"
		    "p(X, X) :- mark(X).\n"
		    "p(X, Z) :- edge(X, X), p(n3, Z).\n"
		    "p(X, Z) :- edge(X, Y), p(Y, Z).\n", "", {
		    { "p(n1, X)?", "    adorn p/2 bf, factored" },
		    { "p(n0, X)?", "    adorn p/2 bf, factored" },
		    { "p(n3, n3)?", "    adorn p/2 bb, factored" } } },
		{ "vector-state", "s(X, C, Y) :- edge(X, Y), edge(C, Y).\n"
		    "s(X, C, Z) :- edge(X, Y), edge(C, D), s(Y, D, Z).\n", "", {
		    { "s(n0, n1, Z)?", "    adorn s/3 bbf, factored" },
		    { "s(n2, n2, Z)?", "    adorn s/3 bbf, factored" },
		    { "s(n0, C, Z)?", "    adorn s/3 bff" } } },
		{ "correlated-state", "s(X, C, Y) :- edge(X, Y), edge(C, Y).\n"
		    "s(X, C, Z) :- edge(X, Y), edge(C, D), s(Y, D, Z).\n",
		    "start(n0, n1).\nstart(n0, n2).\nstart(n1, n2).\n", {
		    { "start(S, C), s(S, C, Z)?",
		      "    adorn s/3 bbf, factored" } } },
		{ "repeated-state", "s(X, Y, Z) :- edge(X, Z), edge(Y, Z).\n"
		    "s(X, X, Z) :- edge(X, Y), s(Y, Y, Z).\n", "", {
		    { "s(n0, n0, Z)?", "    adorn s/3 bbf, factored" },
		    { "s(n0, n1, Z)?", "    adorn s/3 bbf, factored" } } },
		{ "repeated-output", "q(X, Y, Y) :- edge(X, Y).\n"
		    "q(X, Y, Z) :- edge(X, W), q(W, Y, Z).\n", "", {
		    { "q(n0, Y, Z)?", "    adorn q/3 bff, factored" },
		    { "q(n1, Y, Y)?", "    adorn q/3 bff, factored" } } },
		{ "repeated-call-output", "q(X, Y, Y) :- edge(X, Y).\n"
		    "q(X, Y, Y) :- edge(X, W), q(W, Y, Y).\n", "", {
		    { "q(n0, Y, Z)?", "    adorn q/3 bff" } } },
		{ "wildcard-successor", "p(X, Y) :- edge(X, Y).\n"
		    "p(X, Z) :- edge(X, _), p(_, Z).\n", "", {
		    { "p(n0, X)?", "    adorn p/2 bf, subsumed by p/2 ff" } } },
		{ "unbound-successor", "s(X, C, Y) :- edge(X, Y), edge(C, Y).\n"
		    "s(X, C, Z) :- edge(X, C), s(W, W, Z).\n", "", {
		    { "s(n0, n1, Z)?",
		      "    adorn s/3 bbf, subsumed by s/3 fff" } } },
		{ "observed-output", "p(X, Y) :- edge(X, Y).\n"
		    "p(X, Z) :- edge(X, Y), p(Y, Z), mark(Z).\n", "", {
		    { "p(n0, X)?", "    adorn p/2 bf" },
		    { "p(n2, X)?", "    adorn p/2 bf" } } },
		{ "one-bad-rule", closure,
		    "p(X, Z) :- mark(X), p(X, W), edge(Z, W).\n", {
		    { "p(n0, X)?", "    adorn p/2 bf" },
		    { "p(n3, X)?", "    adorn p/2 bf" } } },
		{ "changed-output", "p(X, Y) :- edge(X, Y).\n"
		    "p(X, Z) :- edge(X, Y), p(Y, W), edge(W, Z).\n", "", {
		    { "p(n0, X)?", "    adorn p/2 bf" } } },
		{ "nonlinear", "p(X, Y) :- edge(X, Y).\n"
		    "p(X, Z) :- edge(X, Y), p(Y, W), p(W, Z).\n", "", {
		    { "p(n0, X)?", "    adorn p/2 bf" } } },
		{ "mutual", "p(X, Y) :- edge(X, Y).\n"
		    "p(X, Z) :- edge(X, Y), q(Y, Z).\n"
		    "q(X, Z) :- edge(X, Y), p(Y, Z).\n", "", {
		    { "p(n0, X)?", "    adorn p/2 bf" } } },
		{ "compared", "p(X, Y) :- edge(X, Y).\n"
		    "p(X, Z) :- edge(X, Y), Y != n3, p(Y, Z).\n", "", {
		    { "p(n0, X)?", "    adorn p/2 bf" } } },
		{ "negated", "p(X, Y) :- edge(X, Y).\n"
		    "p(X, Z) :- edge(X, Y), not mark(Y), p(Y, Z).\n", "", {
		    { "p(n0, X)?", "    adorn p/2 bf" } } },
		{ "demanded-input", "r(X, Y) :- edge(X, Y).\n"
		    "r(X, Z) :- edge(X, Y), r(Y, Z).\n"
		    "p(X, Y) :- edge(X, Y).\n"
		    "p(X, Z) :- r(X, Y), p(Y, Z).\n", "", {
		    { "p(n0, X)?", "    adorn p/2 bf" } } },
		{ "subsumed", closure, "", {
		    { "p(n0, X), p(Y, Z)?",
		      "    adorn p/2 bf, subsumed by p/2 ff" } } },
		{ "replayed", closure,
		    "r(X, Y) :- edge(X, Y).\n"
		    "r(X, Z) :- r(X, Y), edge(Y, Z).\n", {
		    { "p(n0, X), r(n1, Y), r(Z, W)?",
		      "    adorn p/2 bf, factored" } } },
		{ "read-completely", closure,
		    "c(X) :- node(X), not p(n0, X).\n", {
		    { "c(X)?", NULL },
		    { "p(n0, X), c(X)?", NULL } } }
	};
	char rules[PROGRAM_MAX];
	char program[PROGRAM_MAX];
	char walk[QUERY_MAX];
	const char *query_text;
	const char *adorn;
	text_t explained;
	diag_log_t log;
	diag_log_t unfactored_log;
	diag_log_t whole_log;
	relic_t *factored;
	relic_t *unfactored;
	relic_t *whole;
	char *expected;
	char *reference;
	char *actual;
	size_t compared;
	size_t p;
	size_t q;
	unsigned int seed;

	compared = 0;
	for (seed = 0; seed < GRAPH_SEEDS; seed++) {
		for (p = 0; p < sizeof(programs) / sizeof(programs[0]); p++) {
			snprintf(rules, sizeof(rules), "%s%s",
			    programs[p].rules, programs[p].extra);
			graph_program(seed, rules, program, sizeof(program));

			for (q = 0; q < FACTOR_QUERIES; q++) {
				query_text = programs[p].queries[q].text;
				adorn = programs[p].queries[q].adorn;
				if (query_text == NULL)
					break;

				factored = context_with(program, 0, 0, &log);
				unfactored = context_with(program,
				    RELIC_F_NO_FACTOR, 0, &unfactored_log);
				whole = context_with(program,
				    RELIC_F_NO_MAGIC | RELIC_F_NAIVE, 0,
				    &whole_log);
				if (factored == NULL || unfactored == NULL ||
				    whole == NULL) {
					relic_destroy(factored);
					relic_destroy(unfactored);
					relic_destroy(whole);
					return;
				}

				bzero(&explained, sizeof(explained));
				text_append(&explained, "", 0);
				if (seed == 0)
					relic_explain_set(factored,
					    record_explained, &explained);

				expected = answer(whole, query_text);
				reference = answer(unfactored, query_text);
				actual = answer(factored, query_text);

				checkf(expected != NULL && reference != NULL &&
				    actual != NULL &&
				    strcmp(expected, actual) == 0 &&
				    strcmp(reference, actual) == 0,
				    SCENARIO_FACTORED,
				    "program %s, seed %u, %s: the default "
				    "answered\n%s\nthe unfactored rewrite\n%s\n"
				    "the fixed point\n%s", programs[p].name,
				    seed, query_text,
				    actual != NULL ? actual : "(failed)",
				    reference != NULL ? reference : "(failed)",
				    expected != NULL ? expected : "(failed)");
				checkf(log.errors == 0 && log.warnings == 0,
				    SCENARIO_FACTORED,
				    "program %s, seed %u, %s: the default "
				    "reported %zu errors and %zu warnings, the "
				    "last \"%s\"", programs[p].name, seed,
				    query_text, log.errors, log.warnings,
				    log.errors != 0 ? log.last :
				    log.last_warning);
				if (seed == 0 && adorn != NULL) {
					walk_of(adorn, walk, sizeof(walk));
					checkf(!explained.failed &&
					    has_line(explained.bytes, adorn) &&
					    (strstr(explained.bytes, walk) !=
					    NULL) == (strstr(adorn,
					    ", factored") != NULL),
					    SCENARIO_FACTORED,
					    "program %s, %s should show "
					    "\"%s\", and %s only if "
					    "factored, not\n%s",
					    programs[p].name, query_text,
					    adorn, walk, explained.bytes);
				}

				free(expected);
				free(reference);
				free(actual);
				free(explained.bytes);
				relic_destroy(factored);
				relic_destroy(unfactored);
				relic_destroy(whole);
				compared++;
			}
		}
	}

	checkf(compared > 0, SCENARIO_FACTORED,
	    "some query should be compared");
}

/*
 * The start of the rules -e shows over the walk of the adornment an adorn
 * line `adorn` names, "walk$p$bf(" for "    adorn p/2 bf, factored".
 */
static void
walk_of(const char *adorn, char *walk, size_t size)
{
	const char *pred;
	const char *slash;
	const char *mask;

	pred = adorn + strlen("    adorn ");
	slash = strchr(pred, '/');
	mask = slash != NULL ? strchr(slash, ' ') : NULL;
	if (mask == NULL) {
		snprintf(walk, size, "(no walk in \"%s\")", adorn);
		return;
	}
	mask++;
	snprintf(walk, size, "walk$%.*s$%.*s(", (int)(slash - pred), pred,
	    (int)strcspn(mask, ","), mask);
}

/*
 * The walk holds two columns per bound one, and a factored rule an origin
 * slot per bound column beside the source slots it copies, so factoring
 * stops where those would not fit a predicate or a rule.  w binds sixteen
 * columns into a walk of RELIC_MAX_ARITY columns, which factors, and
 * seventeen, which does not.  p's rules copy 63 source slots beside the
 * origin's, the transition's output slot not among them though its own
 * rule holds RELIC_MAX_VARIABLES, which factors with the origin in the last
 * slot there is; a base rule holding RELIC_MAX_VARIABLES slots of its own
 * leaves no room for the origin, and does not.  Every query answers as the
 * unfactored rewrite does.
 */
static void
factoring_fits_the_rule_representation(void)
{
	char adorn[64];
	char query_text[256];
	char *program;
	size_t used;
	unsigned int bound;
	unsigned int column;

	for (bound = RELIC_MAX_ARITY / 2; bound <= RELIC_MAX_ARITY / 2 + 1;
	    bound++) {
		program = wide_bound_program(bound);
		if (program == NULL) {
			check(false, SCENARIO_FACTOR_FITS,
			    "the wide program is built");
			return;
		}

		used = (size_t)snprintf(query_text, sizeof(query_text),
		    "w(a");
		for (column = 1; column < bound; column++)
			used += (size_t)snprintf(query_text + used,
			    sizeof(query_text) - used, ", c");
		snprintf(query_text + used, sizeof(query_text) - used,
		    ", Y)?");

		used = (size_t)snprintf(adorn, sizeof(adorn),
		    "    adorn w/%u ", bound + 1);
		for (column = 0; column < bound; column++)
			adorn[used++] = 'b';
		snprintf(adorn + used, sizeof(adorn) - used, "f%s",
		    bound <= RELIC_MAX_ARITY / 2 ? ", factored" : "");

		check_factoring_fits("a wide bound call", program, query_text,
		    adorn);
		free(program);
	}

	program = many_slots_program(RELIC_MAX_VARIABLES - 1);
	if (program != NULL)
		check_factoring_fits("rules filling the last slot", program,
		    "p(a, X)?", "    adorn p/2 bf, factored");
	else
		check(false, SCENARIO_FACTOR_FITS,
		    "the program filling the last slot is built");
	free(program);

	program = many_slots_program(RELIC_MAX_VARIABLES);
	if (program != NULL)
		check_factoring_fits("a base rule of every slot", program,
		    "p(a, X)?", "    adorn p/2 bf");
	else
		check(false, SCENARIO_FACTOR_FITS,
		    "the program of every slot is built");
	free(program);
}

/*
 * One case of factoring_fits_the_rule_representation(): `query_text`
 * answers over `program` as the unfactored rewrite does, with no
 * diagnostic, and -e shows `adorn`.
 */
static void
check_factoring_fits(const char *what, const char *program,
    const char *query_text, const char *adorn)
{
	text_t explained;
	diag_log_t log;
	diag_log_t unfactored_log;
	relic_t *factored;
	relic_t *unfactored;
	char *reference;
	char *actual;

	factored = context_with(program, 0, 0, &log);
	unfactored = context_with(program, RELIC_F_NO_FACTOR, 0,
	    &unfactored_log);
	if (factored == NULL || unfactored == NULL) {
		relic_destroy(factored);
		relic_destroy(unfactored);
		return;
	}

	bzero(&explained, sizeof(explained));
	text_append(&explained, "", 0);
	relic_explain_set(factored, record_explained, &explained);
	reference = answer(unfactored, query_text);
	actual = answer(factored, query_text);

	checkf(reference != NULL && actual != NULL &&
	    strcmp(reference, "b\nc\n") == 0 && strcmp(reference, actual) == 0,
	    SCENARIO_FACTOR_FITS, "%s: %s answered\n%s\nwhere the unfactored "
	    "rewrite answered\n%s", what, query_text,
	    actual != NULL ? actual : "(failed)",
	    reference != NULL ? reference : "(failed)");
	checkf(log.errors == 0 && log.warnings == 0, SCENARIO_FACTOR_FITS,
	    "%s: %s reported %zu errors and %zu warnings, the last \"%s\"",
	    what, query_text, log.errors, log.warnings,
	    log.errors != 0 ? log.last : log.last_warning);
	checkf(!explained.failed && has_line(explained.bytes, adorn),
	    SCENARIO_FACTOR_FITS, "%s: %s should show \"%s\", not\n%.3000s",
	    what, query_text, adorn, explained.bytes);

	free(reference);
	free(actual);
	free(explained.bytes);
	relic_destroy(factored);
	relic_destroy(unfactored);
}

/*
 * A closure over a -> b -> c of `bound` + 1 columns, the last free: the
 * first bound column walks along edge, and every other holds c, which
 * one() states.
 *
 *	w(X1, X2, ..., Y) :- edge(X1, Y), one(X2), ...
 *	w(X1, X2, ..., Y) :- edge(X1, Z), w(Z, X2, ..., Y).
 */
static char *
wide_bound_program(unsigned int bound)
{
	text_t text;
	char term[32];
	unsigned int column;
	unsigned int rule;

	bzero(&text, sizeof(text));
	text_append(&text, "edge(a, b).\nedge(b, c).\none(c).\n",
	    strlen("edge(a, b).\nedge(b, c).\none(c).\n"));

	for (rule = 0; rule < 2; rule++) {
		text_append(&text, "w(X1", 4);
		for (column = 2; column <= bound; column++) {
			snprintf(term, sizeof(term), ", X%u", column);
			text_append(&text, term, strlen(term));
		}
		text_append(&text, ", Y) :- ", 8);

		if (rule == 0) {
			text_append(&text, "edge(X1, Y)", 11);
			for (column = 2; column <= bound; column++) {
				snprintf(term, sizeof(term), ", one(X%u)",
				    column);
				text_append(&text, term, strlen(term));
			}
		} else {
			text_append(&text, "edge(X1, Z), w(Z", 16);
			for (column = 2; column <= bound; column++) {
				snprintf(term, sizeof(term), ", X%u", column);
				text_append(&text, term, strlen(term));
			}
			text_append(&text, ", Y)", 4);
		}
		text_append(&text, ".\n", 2);
	}

	if (text.failed) {
		free(text.bytes);
		return (NULL);
	}

	return (text.bytes);
}

/*
 * A closure over a -> b -> c whose base rule holds `base_slots` slots: X,
 * Y and `base_slots` - 2 more, each read twice by same(), which holds c.
 * The recursive rule reads RELIC_MAX_VARIABLES - 3 of them, so that with X,
 * Y and its output Z it holds RELIC_MAX_VARIABLES slots.
 */
static char *
many_slots_program(unsigned int base_slots)
{
	text_t text;
	char term[32];
	unsigned int v;

	bzero(&text, sizeof(text));
	text_append(&text, "edge(a, b).\nedge(b, c).\nsame(c, c).\n",
	    strlen("edge(a, b).\nedge(b, c).\nsame(c, c).\n"));

	text_append(&text, "p(X, Y) :- edge(X, Y)", 21);
	for (v = 0; v + 2 < base_slots; v++) {
		snprintf(term, sizeof(term), ", same(V%u, V%u)", v, v);
		text_append(&text, term, strlen(term));
	}
	text_append(&text, ".\n", 2);

	text_append(&text, "p(X, Z) :- edge(X, Y)", 21);
	for (v = 0; v + 3 < RELIC_MAX_VARIABLES; v++) {
		snprintf(term, sizeof(term), ", same(V%u, V%u)", v, v);
		text_append(&text, term, strlen(term));
	}
	text_append(&text, ", p(Y, Z).\n", 11);

	if (text.failed) {
		free(text.bytes);
		return (NULL);
	}

	return (text.bytes);
}

/*
 * A factored rewrite's plans and rows do not depend on how it runs: on the
 * calling thread, with every round deferred to 1, 2, 4 and 16 threads, and
 * a second time each way, the query shows the same plans and delivers the
 * same rows in the same order.  Its several origins share states, so a
 * walk mixing them would show in the rows.
 */
static void
a_factored_rewrite_is_deterministic(void)
{
	static const size_t threads[] = { 0, 1, 2, 4, 16 };
	static const char rules[] =
	    "p(n0, n5).\n"
	    "p(X, Y) :- edge(X, Y), mark(Y).\n"
	    "p(X, Z) :- edge(X, Y), p(Y, Z).\n"
	    "p(X, Z) :- edge(Y, X), node(Y), p(Y, Z).\n";
	static const char query_text[] = "node(S), p(S, T)?";
	char program[PROGRAM_MAX];
	relic_config_t config;
	text_t explained;
	diag_log_t log;
	relic_t *ctx;
	char *first_rows;
	char *first_explained;
	char *rows;
	size_t run;
	size_t t;

	graph_program(3, rules, program, sizeof(program));
	first_rows = NULL;
	first_explained = NULL;

	for (t = 0; t < sizeof(threads) / sizeof(threads[0]); t++) {
		for (run = 0; run < 2; run++) {
			bzero(&config, sizeof(config));
			config.threads = threads[t];
			if (threads[t] > 0)
				config.flags = RELIC_F_PARALLEL_ALWAYS;
			ctx = context_configured(program, &config, &log);
			if (ctx == NULL)
				break;

			bzero(&explained, sizeof(explained));
			text_append(&explained, "", 0);
			relic_explain_set(ctx, record_explained, &explained);
			rows = answer_rows(ctx, query_text, false);

			checkf(rows != NULL && !explained.failed &&
			    has_line(explained.bytes,
			    "    adorn p/2 bf, factored") &&
			    log.errors == 0 && log.warnings == 0,
			    SCENARIO_FACTOR_SAME, "%s on %zu threads should "
			    "be answered through a factored rewrite, and "
			    "reported %zu errors and %zu warnings and "
			    "showed\n%s", query_text, threads[t], log.errors,
			    log.warnings, explained.bytes);

			if (first_rows == NULL && rows != NULL &&
			    !explained.failed) {
				first_rows = rows;
				first_explained = explained.bytes;
				relic_destroy(ctx);
				continue;
			}

			checkf(rows != NULL && first_rows != NULL &&
			    strcmp(rows, first_rows) == 0, SCENARIO_FACTOR_SAME,
			    "%s on %zu threads, run %zu, delivered\n%s\nwhere "
			    "the first run delivered\n%s", query_text,
			    threads[t], run + 1, rows != NULL ? rows :
			    "(failed)", first_rows != NULL ? first_rows :
			    "(failed)");
			checkf(first_explained != NULL && !explained.failed &&
			    strcmp(explained.bytes, first_explained) == 0,
			    SCENARIO_FACTOR_SAME, "%s on %zu threads, run %zu, "
			    "showed\n%s\nwhere the first run showed\n%s",
			    query_text, threads[t], run + 1, explained.bytes,
			    first_explained != NULL ? first_explained :
			    "(failed)");

			free(rows);
			free(explained.bytes);
			relic_destroy(ctx);
		}
	}

	free(first_rows);
	free(first_explained);
}

/* The largest limit least_admitted_limit() tries. */
#define	ADMITTED_LIMIT_MAX	((size_t)1 << 22)

/*
 * A factored adornment names a walk after its source as well as its
 * answers and requests, and the walk's name is charged as theirs are.  A
 * closure named by NAMED_LONG bytes raises the least limit the factored
 * rewrite of a bound call fits by at least NAMED_LONG / 64 literals more
 * than it raises the unfactored rewrite's, which names no walk.
 */
static void
a_walk_is_charged_its_name(void)
{
	static const size_t lengths[] = { 1, NAMED_LONG };
	static const unsigned int flags[] = { 0, RELIC_F_NO_FACTOR };
	size_t least[2][2];
	char *program;
	char *query;
	size_t l;
	size_t f;

	for (l = 0; l < 2; l++) {
		program = named_closure_program(lengths[l], &query);
		if (program == NULL) {
			check(false, SCENARIO_LONG_NAME,
			    "the named closure is built");
			return;
		}
		for (f = 0; f < 2; f++)
			least[l][f] = least_admitted_limit(program, query,
			    flags[f]);
		free(program);
		free(query);
	}

	checkf(least[0][0] != 0 && least[0][1] != 0 && least[1][0] != 0 &&
	    least[1][1] != 0 && least[1][0] - least[0][0] >=
	    least[1][1] - least[0][1] + NAMED_LONG / 64, SCENARIO_LONG_NAME,
	    "a closure named by %d bytes should cost its factored rewrite at "
	    "least %d literals more than its unfactored one, and the least "
	    "limits were %zu and %zu factored, %zu and %zu not, for a name "
	    "of 1 and %d bytes", NAMED_LONG, NAMED_LONG / 64, least[0][0],
	    least[1][0], least[0][1], least[1][1], NAMED_LONG);
}

/*
 * A closure over a -> b -> c whose predicate is named by `name_length`
 * bytes, and in *queryp the call binding its first column to a.  The
 * caller frees both; NULL when memory ran out.
 */
static char *
named_closure_program(size_t name_length, char **queryp)
{
	text_t name;
	text_t text;
	size_t i;

	*queryp = NULL;
	bzero(&name, sizeof(name));
	text_append(&name, "p", 1);
	for (i = 1; i < name_length; i++)
		text_append(&name, "q", 1);

	bzero(&text, sizeof(text));
	text_append(&text, "edge(a, b).\nedge(b, c).\n",
	    strlen("edge(a, b).\nedge(b, c).\n"));
	text_append(&text, name.bytes, name.length);
	text_append(&text, "(X, Y) :- edge(X, Y).\n",
	    strlen("(X, Y) :- edge(X, Y).\n"));
	text_append(&text, name.bytes, name.length);
	text_append(&text, "(X, Z) :- edge(X, Y), ",
	    strlen("(X, Z) :- edge(X, Y), "));
	text_append(&text, name.bytes, name.length);
	text_append(&text, "(Y, Z).\n", strlen("(Y, Z).\n"));

	if (!name.failed) {
		text_append(&name, "(a, X)?", strlen("(a, X)?"));
		*queryp = name.bytes;
	}
	if (name.failed || text.failed) {
		free(name.bytes);
		free(text.bytes);
		*queryp = NULL;
		return (NULL);
	}

	return (text.bytes);
}

/*
 * The least limit on the rewrite's work at which `query_text` is answered
 * over `program`, configured with `flags`, through its rewrite, with no
 * diagnostic; 0 when none up to ADMITTED_LIMIT_MAX is.  A larger limit
 * admits whatever a smaller one does, so the least is found by bisection.
 */
static size_t
least_admitted_limit(const char *program, const char *query_text,
    unsigned int flags)
{
	diag_log_t log;
	relic_t *ctx;
	char *rows;
	size_t low;
	size_t high;
	size_t middle;
	bool admitted;

	low = 1;
	high = ADMITTED_LIMIT_MAX + 1;
	while (low < high) {
		middle = low + (high - low) / 2;
		ctx = context_with(program, flags, middle, &log);
		if (ctx == NULL)
			return (0);
		rows = answer(ctx, query_text);
		admitted = rows != NULL && log.errors == 0 &&
		    log.warnings == 0;
		free(rows);
		relic_destroy(ctx);

		if (admitted)
			high = middle;
		else
			low = middle + 1;
	}

	return (low <= ADMITTED_LIMIT_MAX ? low : 0);
}

/*
 * A factored rewrite abandoned at its limit leaves its context usable:
 * path(a, X)? over the closure, whose factored rewrite costs 10, is
 * answered from the whole program under a limit of 9, with a warning, and
 * asked again reads the path that evaluation left complete, needing no
 * rewrite and warning no more, with the same rows.
 */
static void
an_abandoned_factored_rewrite_leaves_its_context_usable(void)
{
	static const char program[] =
	    "edge(a, b).\nedge(b, c).\nedge(c, a).\n"
	    "path(X, Y) :- edge(X, Y).\n"
	    "path(X, Z) :- edge(X, Y), path(Y, Z).\n";
	diag_log_t log;
	relic_t *ctx;
	char *rows;
	size_t run;

	ctx = context_with(program, 0, 9, &log);
	if (ctx == NULL)
		return;

	for (run = 0; run < 2; run++) {
		rows = answer(ctx, "path(a, X)?");
		checkf(rows != NULL && strcmp(rows, "a\nb\nc\n") == 0 &&
		    log.errors == 0 && log.warnings == 1 &&
		    strstr(log.last_warning, "outgrew magic=9 while adorning "
		    "path/2") != NULL, SCENARIO_ABANDONED,
		    "path(a, X)? under magic=9, run %zu, should answer a, b "
		    "and c after the one warning of the first run, and "
		    "answered\n%s\nwith %zu errors and %zu warnings, the last "
		    "\"%s\"", run + 1, rows != NULL ? rows : "(failed)",
		    log.errors, log.warnings, log.last_warning);
		free(rows);
	}

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

/*
 * A rule the rewrite generates is planned from the variables it mentions,
 * not from the rule it was drafted from.  In r(X, W) :- a(X, Y), b(Y, Z),
 * c(Z, W) called with X bound, the prefix before b's call is cut into a
 * supplementary predicate over X and Y, which r reads later, and b's magic
 * rule, magic$b$bf(Y) :- sup$r$bf$0$2(X, Y), mentions X once.  X is dead in
 * that rule though r mentions it three times, so its plan scans the
 * supplementary relation matching anything in X's column and binding Y.
 */
static void
a_generated_rule_is_planned_from_its_mentions(void)
{
	static const char *const program =
	    "e(1, 2).\ne(2, 3).\n"
	    "a(X, Y) :- e(X, Y).\nb(X, Y) :- e(X, Y).\nc(X, Y) :- e(X, Y).\n"
	    "r(X, W) :- a(X, Y), b(Y, Z), c(Z, W).\n";
	const relic_plan_term_t *terms;
	const relic_rule_t *rule;
	const relic_pred_t *head;
	relic_relation_t *target;
	relic_query_t *query;
	relic_magic_t magic;
	relic_arena_t arena;
	relic_plan_t plan;
	diag_log_t log;
	relic_t *ctx;
	size_t i;
	bool planned;
	int status;

	ctx = context_with(program, 0, 0, &log);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, "r(1, W)?");
	if (query == NULL || relic_program_ready(ctx) != 0) {
		check(false, SCENARIO_MENTIONS, "the query is prepared");
		relic_query_destroy(query);
		relic_destroy(ctx);
		return;
	}

	relic_magic_init(&magic, ctx);
	status = relic_magic_find_complete(&magic, query);
	if (status == 0)
		status = relic_magic_rewrite(&magic, &query->rule);

	rule = NULL;
	for (i = 0; status == 0 && i < magic.rules.count; i++) {
		head = relic_pred_of(ctx, magic.rules.entries[i].head);
		if (head != NULL && strcmp(head->name.bytes, "magic$b$bf") == 0)
			rule = &magic.rules.entries[i];
	}

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);
	planned = false;
	if (rule != NULL && rule->body_length == 1) {
		target = relic_relation_read(ctx, rule->head);
		planned = target != NULL && relic_plan_rule(ctx, &arena, rule,
		    target, NULL, &plan) == 0 && plan.op_count == 2;
	}
	checkf(planned, SCENARIO_MENTIONS, "b's magic rule should be drafted "
	    "with one literal and planned, and the rewrite %s",
	    status == 0 ? "succeeded" : "failed");

	if (planned) {
		terms = plan.ops[0].u.atom.terms;
		check(plan.ops[0].kind == RELIC_OP_SCAN &&
		    rule->body[0].u.atom.args[0].kind == RELIC_ARG_VARIABLE &&
		    terms[0].action == RELIC_PLAN_ANY &&
		    terms[1].action == RELIC_PLAN_BIND, SCENARIO_MENTIONS,
		    "the scan matches anything for X and binds Y");
	}

	relic_arena_reset(&arena);
	relic_magic_reset(&magic);
	relic_query_destroy(query);
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
	a_rejected_certificate_fits_the_default_limit();
	a_long_name_is_charged_per_predicate_named();
	what_the_rewrite_derives_does_not_leak();
	a_load_after_a_rewrite_reaches_the_next_query();
	a_generated_rule_is_planned_from_its_mentions();
	a_bound_chain_derives_linearly();
	factoring_answers_as_its_references_do();
	factoring_fits_the_rule_representation();
	a_factored_rewrite_is_deterministic();
	a_walk_is_charged_its_name();
	an_abandoned_factored_rewrite_leaves_its_context_usable();

	return (check_summary("test_magic"));
}
