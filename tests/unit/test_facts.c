/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for a program's facts: how a load stages and commits them
 * into base stores, what a refused load leaves behind, how evaluation and
 * the Magic rewrite read them, and how the limits count them over a
 * session of loads and queries.
 *
 * The golden language tests see the answers and the messages; this suite
 * examines what they cannot -- which store holds a fact, whether the
 * derived relations were discarded, and what the limits were charged.
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

#define	SCENARIO_ADOPT		"a store adopts or appends by what it holds"
#define	SCENARIO_ARITY_ZERO	"a fact of no columns is a fact like any other"
#define	SCENARIO_BATCH_ROOM \
	"a batch is staged only where its columns can grow"
#define	SCENARIO_BUILDER_GROWTH \
	"a builder grows as insertions grow a relation"
#define	SCENARIO_NO_TREE	"a load's facts build no statements"
#define	SCENARIO_FACT_ADD	"an added fact is a load of one"
#define	SCENARIO_FUNNEL		"a predicate is read from the store it has"
#define	SCENARIO_LOAD		"a load keeps what is not a fact as it was"
#define	SCENARIO_MAGIC		"the Magic rewrite reads facts through a view"
#define	SCENARIO_REFUSED	"a refused load leaves the program as it was"
#define	SCENARIO_SESSION	"the limits count a session's facts once"

/* Room for the rows of a small query, and for one message. */
#define	ROWS_TEXT_MAX		256
#define	MESSAGE_MAX		256

/*
 * The facts of the session scenarios: enough that a store and its indexes
 * are much larger than the fixed costs of an evaluation.
 */
#define	SESSION_FACTS		400
#define	TREELESS_FACTS		5000	/* many arena chunks of statements */
#define	SESSION_LINE_MAX	32	/* the longest "e(N, N, N).\n" line */

/*
 * The bytes a limit may exceed the hungriest query's need by and still
 * refuse a second index over the session's facts, which takes at least a
 * slot per fact; and a limit no session scenario needs, of tuples or bytes,
 * where the search for the least one starts.
 */
#define	INDEX_SLACK		64
#define	LEAST_LIMIT_CEILING	((size_t)1 << 24)

/*
 * The query the session scenarios answer, which negates p/3 and so
 * evaluates the whole of it, and the facts a scenario loads and adds after
 * an evaluation.
 */
#define	SESSION_QUERY		"r(x)?"
#define	GROWN_FACTS		"e(b, b, b).\ne(c, c, c).\ne(a, a, a).\n"

/*
 * The session of queries that must not accumulate indexes: each query binds
 * a different column of e/3 and has the one row given, and the session asks
 * all of them in each round with a load after it.
 */
#define	SESSION_QUERIES		3
#define	SESSION_ROUNDS		2

static const char *const session_queries[SESSION_QUERIES] = {
	"p(1, Y, Z)?", "p(X, 2, Z)?", "p(X, Y, 3)?"
};
static const char *const session_rows[SESSION_QUERIES] = {
	"1 1", "2 2", "3 3"
};
static const char *const session_loads[SESSION_ROUNDS] = {
	"f(y).\n", "f(x).\n"
};

/* The facts of the small store beside c/2's in the multi-store refusal. */
#define	SMALL_STORE_FACTS	8

/*
 * The tuples the empty store of the adoption scenario is given room for
 * before any fact is loaded into it: far more capacity than the load's
 * builder takes.
 */
#define	RETAINED_TUPLES		((size_t)1 << 16)

/* The arity of the widening program whose rewrite outgrows its limit. */
#define	ABANDONED_ARITY		8

typedef struct diag_log diag_log_t;
typedef struct rows_text rows_text_t;

/* Which limit a scenario sets. */
enum limit_kind {
	LIMIT_TUPLES,
	LIMIT_MEMORY
};

/* What a context does, after loading a program, to measure a limit. */
enum step {
	STEP_LOAD,		/* nothing more */
	STEP_MATERIALIZE,	/* materialize the program */
	STEP_QUERY		/* answer a query */
};

/*
 * What the diagnostic callback has seen: the counts of errors and warnings,
 * and the last message and location of each.
 */
struct diag_log {
	size_t			errors;
	size_t			warnings;
	char			last_error[MESSAGE_MAX];
	char			last_warning[MESSAGE_MAX];
	relic_location_t	warning_where;
};

/*
 * A query's rows as text, in the order they were delivered: each row's
 * values separated by spaces, a row of no columns written `()`, and rows
 * separated by `|`.  `failed` is set when a row did not fit.
 */
struct rows_text {
	relic_t	*ctx;
	char	 text[ROWS_TEXT_MAX];
	size_t	 rows;
	bool	 failed;
};

static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static relic_t	*context_with(const relic_config_t *config, const char *program,
		    diag_log_t *log);
static int	 keep_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static int	 rows_of(relic_t *ctx, const char *query_text,
		    rows_text_t *rows);
static bool	 answers(relic_t *ctx, const char *query_text,
		    const char *expected);
static relic_pred_id_t predicate(relic_t *ctx, const char *name,
		    uint16_t arity);
static size_t	 facts_held(relic_t *ctx, const char *name, uint16_t arity);
static size_t	 indexes_of(relic_t *ctx, const char *name, uint16_t arity);
static char	*numbered_facts(const char *name, unsigned int arity,
		    size_t count);
static char	*concatenated(const char *first, const char *second);
static const char *kind_name(enum limit_kind kind);
static relic_value_t symbol(relic_t *ctx, const char *name);
static void	 a_load_keeps_what_is_not_a_fact(void);
static void	 a_failed_load_commits_no_fact(void);
static void	 a_refused_load_leaves_the_program_as_it_was(void);
static void	 refused_by(enum limit_kind kind);
static void	 a_refusal_names_the_first_store_that_grows(void);
static void	 a_refused_load_stops_parsing(void);
static bool	 symbol_interned(const relic_t *ctx, const char *name);
static void	 a_predicate_is_read_from_the_store_it_has(void);
static void	 an_added_fact_is_seen_and_kept_once(void);
static void	 an_added_fact_can_falsify_a_negation(void);
static void	 falsified_through(unsigned int flags, bool materialized,
		    const char *strategy);
static void	 an_added_fact_the_limit_refuses_changes_nothing(void);
static void	 queries_do_not_accumulate_indexes(void);
static size_t	 session_vocabulary_bytes(const char *program);
static char	*session_program(void);
static size_t	 least_limit(const char *program, enum limit_kind kind,
		    enum step step, const char *query_text);
static void	 facts_after_evaluation_are_not_refused(void);
static void	 facts_after(const char *program, const char *grown,
		    enum limit_kind kind, enum step step);
static void	 a_load_over_the_limit_grows_no_store(void);
static size_t	 bytes_after_loading(const char *facts);
static size_t	 bytes_held(const relic_t *ctx);
static void	 intern_symbols(relic_t *ctx, const char *const *names);
static void	 the_rewrite_reads_facts_through_a_view(void);
static void	 an_abandoned_rewrite_leaves_no_warning(void);
static void	 a_fact_of_no_columns_is_a_fact(void);
static void	 a_loads_facts_build_no_statements(void);
static void	 a_store_adopts_or_appends_by_what_it_holds(void);
static void	 a_batch_is_staged_only_where_its_columns_grow(void);
static void	 a_builder_grows_as_insertions_grow_a_relation(void);
static bool	 grown_alike(const relic_builder_t *builder,
		    const relic_relation_t *relation);
static bool	 holds_in_order(relic_t *ctx, const char *name,
		    const char *const *names);
static size_t	 base_bytes_by_hand(const relic_t *ctx);
static size_t	 tree_growth(relic_t *ctx, const char *text);

static void
record_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	diag_log_t *log;

	log = callback_ctx;
	if (level == RELIC_ERROR) {
		log->errors++;
		strlcpy(log->last_error, message, sizeof(log->last_error));
	} else if (level == RELIC_WARNING) {
		log->warnings++;
		strlcpy(log->last_warning, message, sizeof(log->last_warning));
		if (where != NULL)
			log->warning_where = *where;
	}
}

/*
 * A context configured by `config`, or by the defaults when it is NULL,
 * reporting to `log` and holding `program` unless that is NULL.
 */
static relic_t *
context_with(const relic_config_t *config, const char *program,
    diag_log_t *log)
{
	relic_t *ctx;

	bzero(log, sizeof(*log));

	ctx = config != NULL ? relic_create_config(config) : relic_create();
	if (ctx == NULL) {
		check(false, "creating a context", "a context is created");
		return (NULL);
	}
	relic_diag_set(ctx, record_diagnostic, log);

	if (program != NULL &&
	    relic_load_string(ctx, "test.dl", program) != 0) {
		check(false, "loading a test program", "the program loads");
		relic_destroy(ctx);
		return (NULL);
	}

	return (ctx);
}

static int
keep_row(void *callback_ctx, const relic_value_t *values, size_t count)
{
	rows_text_t *rows;
	char value[64];
	size_t i;

	rows = callback_ctx;
	if (rows->rows > 0)
		strlcat(rows->text, "|", sizeof(rows->text));
	if (count == 0 && strlcat(rows->text, "()", sizeof(rows->text)) >=
	    sizeof(rows->text))
		rows->failed = true;

	for (i = 0; i < count; i++) {
		if (values[i].type == RELIC_INTEGER)
			snprintf(value, sizeof(value), "%lld",
			    (long long)values[i].v.integer);
		else
			strlcpy(value, relic_string(rows->ctx, values[i].v.id,
			    NULL), sizeof(value));

		if (i > 0)
			strlcat(rows->text, " ", sizeof(rows->text));
		if (strlcat(rows->text, value, sizeof(rows->text)) >=
		    sizeof(rows->text))
			rows->failed = true;
	}
	rows->rows++;

	return (0);
}

/*
 * Answer `query_text`, writing its rows into *rows.  Returns 0, or -1 when
 * the query failed to prepare or to run.
 */
static int
rows_of(relic_t *ctx, const char *query_text, rows_text_t *rows)
{
	relic_query_t *query;
	int status;

	bzero(rows, sizeof(*rows));
	rows->ctx = ctx;

	query = relic_query_prepare(ctx, NULL, query_text);
	status = query != NULL ?
	    relic_query_run(ctx, query, keep_row, rows) : -1;
	relic_query_destroy(query);

	return (status == 0 ? 0 : -1);
}

/*
 * Whether `query_text` is answered, in order, with the rows `expected`
 * spells as rows_text_t writes them, no rows being an empty string.  A
 * mismatch is reported with what was delivered.
 */
static bool
answers(relic_t *ctx, const char *query_text, const char *expected)
{
	rows_text_t rows;
	int status;

	status = rows_of(ctx, query_text, &rows);
	if (status == 0 && !rows.failed && strcmp(rows.text, expected) == 0)
		return (true);

	fprintf(stderr, "%s answered \"%s\"%s, expected \"%s\"\n", query_text,
	    rows.text, status == 0 ? "" : " and failed", expected);
	return (false);
}

/*
 * The identifier of the predicate name/arity, interning it if nothing has.
 * Interning alone changes nothing a scenario asserts: a predicate interned
 * and never read has no store, no rule and no warning.
 */
static relic_pred_id_t
predicate(relic_t *ctx, const char *name, uint16_t arity)
{
	relic_pred_id_t pred;

	if (relic_pred_intern(ctx, name, strlen(name), arity, NULL,
	    &pred) != 0)
		return (RELIC_ID_NONE);

	return (pred);
}

/*
 * How many facts the base store of name/arity holds.
 */
static size_t
facts_held(relic_t *ctx, const char *name, uint16_t arity)
{
	return (relic_base_count(ctx, predicate(ctx, name, arity)));
}

/*
 * How many secondary indexes the base store of name/arity has, none when
 * nothing has created the store.
 */
static size_t
indexes_of(relic_t *ctx, const char *name, uint16_t arity)
{
	const relic_relation_t *store;

	store = relic_base_store(ctx, predicate(ctx, name, arity));

	return (store != NULL ? store->index_count : 0);
}

/*
 * `first` followed by `second`, in a string the caller frees, or NULL when
 * either is NULL or there was no memory for it.
 */
static char *
concatenated(const char *first, const char *second)
{
	size_t first_length;
	size_t second_length;
	char *text;

	if (first == NULL || second == NULL)
		return (NULL);

	first_length = strlen(first);
	second_length = strlen(second);
	text = malloc(first_length + second_length + 1);
	if (text == NULL)
		return (NULL);

	bcopy(first, text, first_length);
	bcopy(second, text + first_length, second_length + 1);

	return (text);
}

static const char *
kind_name(enum limit_kind kind)
{
	return (kind == LIMIT_TUPLES ? "a tuple limit" : "a memory limit");
}

/*
 * `count` facts of name/arity, the Nth holding N in every column, in a
 * string the caller frees, or NULL when there was no memory for it.  The
 * name is a letter and the arity at most 3, so a line fits
 * SESSION_LINE_MAX.
 */
static char *
numbered_facts(const char *name, unsigned int arity, size_t count)
{
	size_t capacity;
	size_t length;
	size_t i;
	unsigned int column;
	char *text;

	capacity = count * SESSION_LINE_MAX + 1;
	text = malloc(capacity);
	if (text == NULL)
		return (NULL);

	length = 0;
	text[0] = '\0';
	for (i = 0; i < count; i++) {
		length += (size_t)snprintf(text + length, capacity - length,
		    "%s(", name);
		for (column = 0; column < arity; column++)
			length += (size_t)snprintf(text + length,
			    capacity - length, column > 0 ? ", %zu" : "%zu", i);
		length += (size_t)snprintf(text + length, capacity - length,
		    ").\n");
	}

	return (text);
}

static relic_value_t
symbol(relic_t *ctx, const char *name)
{
	relic_value_t value;

	bzero(&value, sizeof(value));
	if (relic_value_symbol(ctx, name, strlen(name), &value) != 0)
		check(false, "building a value", "a symbol is interned");

	return (value);
}

/*
 * Only a ground body-less statement is a fact.  One with a variable is kept
 * as a statement and rejected by the check as unsafe, and its head names its
 * predicate, so a rule reading it draws no undefined-predicate warning.  A
 * loaded query whose literal is ground stays a query.  A fact in a query's
 * text is refused as a fact, and stores nothing.
 */
static void
a_load_keeps_what_is_not_a_fact(void)
{
	relic_query_t *query;
	diag_log_t log;
	relic_t *ctx;
	int status;

	ctx = context_with(NULL, "p(X).\nq(Y) :- p(Y).\n", &log);
	if (ctx != NULL) {
		checkf(ctx->ast.count == 2 && relic_base_total(ctx) == 0,
		    SCENARIO_LOAD, "a fact with a variable should be kept as a "
		    "statement, not %zu statements and %zu facts",
		    ctx->ast.count, relic_base_total(ctx));

		status = relic_check(ctx);
		checkf(status != 0 && log.errors == 1 && log.warnings == 0,
		    SCENARIO_LOAD, "the check should reject it with one error "
		    "and no warning, not %zu and %zu, the last \"%s\"",
		    log.errors, log.warnings, log.last_warning);
		relic_destroy(ctx);
	}

	ctx = context_with(NULL, "e(a).\ne(a)?\n", &log);
	if (ctx != NULL) {
		checkf(relic_query_count(ctx) == 1 &&
		    relic_base_total(ctx) == 1, SCENARIO_LOAD, "a ground query "
		    "should stay a query, not %zu queries and %zu facts",
		    relic_query_count(ctx), relic_base_total(ctx));
		check(answers(ctx, "e(a)?", "()"), SCENARIO_LOAD,
		    "the ground query holds");
		relic_destroy(ctx);
	}

	ctx = context_with(NULL, NULL, &log);
	if (ctx != NULL) {
		query = relic_query_prepare(ctx, NULL, "e(a).");
		checkf(query == NULL && log.errors == 1 &&
		    strcmp(log.last_error, "expected a query, found a fact") ==
		    0, SCENARIO_LOAD, "a fact in a query's text should be "
		    "refused as a fact, not with %zu errors, the last \"%s\"",
		    log.errors, log.last_error);
		checkf(relic_base_total(ctx) == 0, SCENARIO_LOAD,
		    "the refused query text should store nothing, not %zu "
		    "facts",
		    relic_base_total(ctx));
		relic_query_destroy(query);
		relic_destroy(ctx);
	}
}

/*
 * A load that fails to parse commits none of its facts, and a predicate its
 * facts first named is not defined by them: a check warns of nothing while
 * no rule reads it, and once a rule does, warns of it at the rule's atom.
 */
static void
a_failed_load_commits_no_fact(void)
{
	diag_log_t log;
	relic_t *ctx;
	int status;

	ctx = context_with(NULL, NULL, &log);
	if (ctx == NULL)
		return;

	status = relic_load_string(ctx, "bad.dl", "q(a).\nq(b\n");
	checkf(status != 0 && relic_base_total(ctx) == 0, SCENARIO_LOAD,
	    "a load that fails to parse should commit no fact, not %zu",
	    relic_base_total(ctx));

	log.warnings = 0;
	status = relic_check(ctx);
	checkf(status == 0 && log.warnings == 0, SCENARIO_LOAD, "nothing reads "
	    "q/1, so the check should warn of nothing, not \"%s\"",
	    log.last_warning);

	status = relic_load_string(ctx, "rule.dl", "\nr(X) :- q(X).\n");
	check(status == 0, SCENARIO_LOAD, "the rule loads");
	status = relic_check(ctx);
	checkf(status == 0 && log.warnings == 1 && strcmp(log.last_warning,
	    "predicate q/1 is referenced but never defined") == 0 &&
	    log.warning_where.line == 2 && log.warning_where.column == 9,
	    SCENARIO_LOAD, "q/1 should be warned about at the rule's atom, "
	    "not with %zu warnings, the last \"%s\" at %u:%u", log.warnings,
	    log.last_warning, log.warning_where.line,
	    log.warning_where.column);

	relic_destroy(ctx);
}

/*
 * A load refused by the tuple limit or the memory limit grows no base
 * store and leaves the program as it was: no statement added, and its new
 * predicate g/1, which no rule reads, neither defined nor warned about, so a
 * check gives the warnings it gives a context the load was never tried in.
 * Either limit refuses the load as it stages its facts, before the commit
 * would discard anything, so the relations stay as current as they were.
 */
static void
a_refused_load_leaves_the_program_as_it_was(void)
{
	refused_by(LIMIT_TUPLES);
	refused_by(LIMIT_MEMORY);
	a_refusal_names_the_first_store_that_grows();
	a_refused_load_stops_parsing();
}

/*
 * The refusal scenario under a limit of `kind`.  The tuple limit, 4,
 * admits the program's two facts and its two derived tuples, so the program
 * is materialized first, and loading the facts again, which changes
 * nothing, is seen to keep the relations current.  The memory limit is set
 * once the rule is loaded, at what the context then holds, so that it
 * admits no fact and the context holds the rule alone.  The pool and the
 * predicate table count against the limit, so the values the refused load
 * uses are interned, and the program checked so that the predicate table
 * holds the rule's predicates and has room for one more, before the limit
 * is set: what the load refuses is then the first store to grow, not a
 * predicate.
 */
static void
refused_by(enum limit_kind kind)
{
	static const char program[] = "p(X) :- e(X).\n";
	static const char facts[] = "e(a).\ne(b).\n";
	static const char more[] = "e(c).\ne(d).\ng(a).\n";
	static const char *const more_names[] = { "a", "c", "d", "g", NULL };
	relic_config_t config;
	diag_log_t control_log;
	diag_log_t log;
	relic_t *control;
	relic_t *ctx;
	char expected[MESSAGE_MAX];
	size_t statements;
	size_t facts_before;
	bool current;
	int status;

	bzero(&config, sizeof(config));
	if (kind == LIMIT_TUPLES) {
		config.max_tuples = 4;
		facts_before = 2;
	} else
		facts_before = 0;

	ctx = context_with(&config, program, &log);
	control = context_with(&config, program, &control_log);
	if (ctx == NULL || control == NULL) {
		relic_destroy(ctx);
		relic_destroy(control);
		return;
	}

	if (kind == LIMIT_TUPLES)
		snprintf(expected, sizeof(expected),
		    "limit exceeded: tuples=4 while loading e/1");
	else {
		intern_symbols(ctx, more_names);
		intern_symbols(control, more_names);
		check(relic_check(ctx) == 0 && relic_check(control) == 0,
		    SCENARIO_REFUSED, "the rule checks");
		ctx->account.limit = bytes_held(ctx);
		control->account.limit = bytes_held(control);
		snprintf(expected, sizeof(expected),
		    "limit exceeded: memory=%zu while loading e/1",
		    ctx->account.limit);
	}

	if (kind == LIMIT_TUPLES) {
		status = relic_load_string(ctx, "facts.dl", facts);
		check(status == 0, SCENARIO_REFUSED, "the facts load");
		status = relic_load_string(control, "facts.dl", facts);
		check(status == 0, SCENARIO_REFUSED,
		    "the control's facts load");
		status = relic_materialize(ctx);
		check(status == 0 && relic_relations_current(ctx),
		    SCENARIO_REFUSED, "the program is materialized");
		status = relic_materialize(control);
		check(status == 0, SCENARIO_REFUSED,
		    "the control is materialized");
		status = relic_load_string(ctx, "again.dl", facts);
		check(status == 0 && relic_relations_current(ctx),
		    SCENARIO_REFUSED,
		    "a load of duplicates alone leaves the relations current");
	}

	statements = ctx->ast.count;
	current = relic_relations_current(ctx);
	log.errors = 0;
	log.warnings = 0;
	control_log.warnings = 0;

	status = relic_load_string(ctx, "more.dl", more);
	checkf(status != 0 && log.errors == 1 &&
	    strcmp(log.last_error, expected) == 0, SCENARIO_REFUSED,
	    "the load should be refused with \"%s\", not with %zu errors, "
	    "the last \"%s\"", expected, log.errors, log.last_error);
	checkf(ctx->ast.count == statements &&
	    facts_held(ctx, "e", 1) == facts_before &&
	    facts_held(ctx, "g", 1) == 0, SCENARIO_REFUSED,
	    "no statement should be added and no base store grow, under %s",
	    kind == LIMIT_TUPLES ? "a tuple limit" : "a memory limit");
	check(relic_relations_current(ctx) == current, SCENARIO_REFUSED,
	    "a load refused as it stages leaves the relations as current as "
	    "they were");
	check(!relic_pred_is_defined(ctx, predicate(ctx, "g", 1)),
	    SCENARIO_REFUSED, "g/1 stays undefined");

	status = relic_check(ctx);
	check(status == 0, SCENARIO_REFUSED, "the program still checks");
	status = relic_check(control);
	check(status == 0, SCENARIO_REFUSED, "the control checks");
	checkf(log.warnings == control_log.warnings, SCENARIO_REFUSED,
	    "the check should give the control's %zu warnings, not %zu, the "
	    "last \"%s\"", control_log.warnings, log.warnings,
	    log.last_warning);

	relic_destroy(control);
	relic_destroy(ctx);
}

/*
 * A refusal names the first predicate, in the order the load names them,
 * whose store would grow: e/1's facts are all held already, so a load that
 * names e/1 and then g/1 is refused for g/1.
 */
static void
a_refusal_names_the_first_store_that_grows(void)
{
	static const char expected[] =
	    "limit exceeded: tuples=1 while loading g/1";
	relic_config_t config;
	diag_log_t log;
	relic_t *ctx;
	int status;

	bzero(&config, sizeof(config));
	config.max_tuples = 1;
	ctx = context_with(&config, "e(a).\n", &log);
	if (ctx == NULL)
		return;

	status = relic_load_string(ctx, "more.dl", "e(a).\ng(b).\n");
	checkf(status != 0 && strcmp(log.last_error, expected) == 0,
	    SCENARIO_REFUSED, "the load should be refused with \"%s\", not "
	    "\"%s\"", expected, log.last_error);

	relic_destroy(ctx);
}

/*
 * A load the tuple limit refuses stops parsing at the fact that takes the
 * base stores past the limit, rather than staging the whole text first, and
 * reads nothing after that fact.  Of a thousand facts e(s0) to e(s999)
 * under a limit of ten, the eleventh is refused: the lexer has interned its
 * s10 and no later symbol, and the unfinished statement that ends the text,
 * which a parse reading on would reach and report in place of the limit, is
 * never read.  Nor is a token the lexer refuses right after the refused
 * fact.
 */
static void
a_refused_load_stops_parsing(void)
{
	static const char expected[] =
	    "limit exceeded: tuples=10 while loading e/1";
	static const char tail_expected[] =
	    "limit exceeded: tuples=1 while loading e/1";
	char name[32];
	relic_config_t config;
	diag_log_t log;
	relic_t *ctx;
	size_t capacity;
	size_t length;
	size_t i;
	char *text;
	int status;

	capacity = 1000 * 16 + 8;
	text = malloc(capacity);
	if (text == NULL) {
		check(false, SCENARIO_REFUSED, "the text is built");
		return;
	}
	length = 0;
	for (i = 0; i < 1000; i++)
		length += (size_t)snprintf(text + length, capacity - length,
		    "e(s%zu).\n", i);
	snprintf(text + length, capacity - length, "e(");

	bzero(&config, sizeof(config));
	config.max_tuples = 10;
	ctx = context_with(&config, "", &log);
	if (ctx != NULL) {
		status = relic_load_string(ctx, "many.dl", text);
		checkf(status != 0 && log.errors == 1 &&
		    strcmp(log.last_error, expected) == 0, SCENARIO_REFUSED,
		    "the load should be refused with \"%s\" alone, not %zu "
		    "errors, the last \"%s\"", expected, log.errors,
		    log.last_error);
		snprintf(name, sizeof(name), "s%d", 10);
		check(symbol_interned(ctx, name), SCENARIO_REFUSED,
		    "the refused fact's symbol is interned");
		snprintf(name, sizeof(name), "s%d", 11);
		check(!symbol_interned(ctx, name), SCENARIO_REFUSED,
		    "no symbol after the refused fact is interned");
		relic_destroy(ctx);
	}
	free(text);

	config.max_tuples = 1;
	ctx = context_with(&config, "", &log);
	if (ctx != NULL) {
		status = relic_load_string(ctx, "tail.dl",
		    "e(s0).\ne(s1). @\n");
		checkf(status != 0 && log.errors == 1 &&
		    strcmp(log.last_error, tail_expected) == 0,
		    SCENARIO_REFUSED, "a refused fact followed by a token the "
		    "lexer refuses should be refused with \"%s\" alone, not "
		    "%zu errors, the last \"%s\"", tail_expected, log.errors,
		    log.last_error);
		relic_destroy(ctx);
	}
}

/* Whether `name` has been interned as a symbol of the context. */
static bool
symbol_interned(const relic_t *ctx, const char *name)
{
	return (relic_symbol_find(ctx, name, strlen(name),
	    relic_symbol_hash(ctx, name, strlen(name))) != RELIC_ID_NONE);
}

/*
 * A predicate named only in a query reads as empty.  One that gains its
 * first rule after its facts were loaded answers from facts and rules
 * together, facts first, whether through the rewrite or materialized.
 * Writing is refused for a predicate no rule derives.
 */
static void
a_predicate_is_read_from_the_store_it_has(void)
{
	relic_relation_t *written;
	diag_log_t log;
	relic_t *ctx;
	int status;

	ctx = context_with(NULL, "p(a).\n", &log);
	if (ctx == NULL)
		return;

	checkf(answers(ctx, "q(X)?", "") && log.warnings == 1,
	    SCENARIO_FUNNEL, "a predicate named only in a query should read "
	    "as empty with one warning, not %zu", log.warnings);
	check(answers(ctx, "p(X)?", "a"), SCENARIO_FUNNEL,
	    "a predicate of facts alone answers its facts");

	log.errors = 0;
	written = relic_relation_written(ctx, predicate(ctx, "p", 1));
	checkf(written == NULL && log.errors == 1, SCENARIO_FUNNEL,
	    "writing a predicate no rule derives should be refused with one "
	    "error, not %zu", log.errors);

	status = relic_load_string(ctx, "rules.dl", "p(X) :- e(X).\ne(b).\n");
	check(status == 0, SCENARIO_FUNNEL, "p gains a rule");
	check(answers(ctx, "p(X)?", "a|b"), SCENARIO_FUNNEL, "the rewrite "
	    "answers from p's facts and its rule, facts first");
	status = relic_materialize(ctx);
	check(status == 0, SCENARIO_FUNNEL, "the program is materialized");
	check(answers(ctx, "p(X)?", "a|b"), SCENARIO_FUNNEL, "the fixed point "
	    "answers from p's facts and its rule, facts first");

	relic_destroy(ctx);
}

/*
 * relic_fact_add() is a load of one fact: the next query sees it without a
 * check, and adding it again changes nothing, leaving the relations
 * current.
 */
static void
an_added_fact_is_seen_and_kept_once(void)
{
	relic_value_t value;
	diag_log_t log;
	relic_t *ctx;
	int status;

	ctx = context_with(NULL, "p(X) :- e(X).\n", &log);
	if (ctx == NULL)
		return;

	value = symbol(ctx, "a");
	status = relic_fact_add(ctx, "e", 1, &value);
	check(status == 0, SCENARIO_FACT_ADD, "a fact is added");
	check(answers(ctx, "p(X)?", "a"), SCENARIO_FACT_ADD,
	    "the next query sees an added fact without a check");

	status = relic_materialize(ctx);
	check(status == 0, SCENARIO_FACT_ADD, "the program is materialized");
	status = relic_fact_add(ctx, "e", 1, &value);
	checkf(status == 0 && facts_held(ctx, "e", 1) == 1 &&
	    relic_relations_current(ctx), SCENARIO_FACT_ADD,
	    "adding a fact twice should change nothing, but e/1 holds %zu "
	    "facts and the relations are %scurrent", facts_held(ctx, "e", 1),
	    relic_relations_current(ctx) ? "" : "not ");

	relic_destroy(ctx);
}

/*
 * A fact that makes a negated literal false changes the answer of a query
 * answered before it, whichever way the query is answered.
 */
static void
an_added_fact_can_falsify_a_negation(void)
{
	falsified_through(0, false, "the rewrite");
	falsified_through(0, true, "materialization");
	falsified_through(RELIC_F_NO_MAGIC, false, "the whole program");
}

/*
 * The negation scenario under `flags`, materializing the program before
 * each pair of queries when `materialized`.
 */
static void
falsified_through(unsigned int flags, bool materialized, const char *strategy)
{
	static const char program[] =
	    "s(a).\ns(b).\nr(X) :- s(X), not b(X).\nb(X) :- c(X).\n";
	relic_config_t config;
	relic_value_t value;
	diag_log_t log;
	relic_t *ctx;
	int status;

	bzero(&config, sizeof(config));
	config.flags = flags;
	ctx = context_with(&config, program, &log);
	if (ctx == NULL)
		return;

	if (materialized) {
		status = relic_materialize(ctx);
		check(status == 0, SCENARIO_FACT_ADD, "the program is "
		    "materialized before the fact");
	}
	checkf(answers(ctx, "r(a)?", "()") && answers(ctx, "r(X)?", "a|b"),
	    SCENARIO_FACT_ADD, "the negation should hold before the fact, "
	    "through %s", strategy);

	value = symbol(ctx, "a");
	status = relic_fact_add(ctx, "b", 1, &value);
	check(status == 0, SCENARIO_FACT_ADD, "the fact is added");
	if (materialized) {
		status = relic_materialize(ctx);
		check(status == 0, SCENARIO_FACT_ADD, "the program is "
		    "materialized after the fact");
	}
	checkf(answers(ctx, "r(X)?", "b") && answers(ctx, "r(a)?", ""),
	    SCENARIO_FACT_ADD, "an added fact should falsify the negation "
	    "through %s", strategy);

	relic_destroy(ctx);
}

/*
 * A fact the tuple limit refuses is reported and changes nothing: the
 * limit admits the one fact loaded, and no room to answer a query.
 */
static void
an_added_fact_the_limit_refuses_changes_nothing(void)
{
	static const char expected[] =
	    "limit exceeded: tuples=1 while loading e/1";
	relic_config_t config;
	relic_value_t value;
	diag_log_t log;
	relic_t *ctx;
	int status;

	bzero(&config, sizeof(config));
	config.max_tuples = 1;
	ctx = context_with(&config, "e(a).\n", &log);
	if (ctx == NULL)
		return;

	value = symbol(ctx, "b");
	status = relic_fact_add(ctx, "e", 1, &value);
	checkf(status != 0 && log.errors == 1 &&
	    strcmp(log.last_error, expected) == 0, SCENARIO_FACT_ADD,
	    "the fact should be refused with \"%s\", not with %zu errors, the "
	    "last \"%s\"", expected, log.errors, log.last_error);
	checkf(relic_base_total(ctx) == 1, SCENARIO_FACT_ADD,
	    "a refused fact should change nothing, but %zu facts are held",
	    relic_base_total(ctx));

	relic_destroy(ctx);
}

/*
 * Queries binding different columns of a base store each build an index
 * over it.  Each evaluation of a rewrite discards what the last one
 * derived, and the base store's indexes with it, so the indexes do not
 * accumulate: under a memory limit a few bytes above what the hungriest
 * query needs alone, every query is answered in turn, with a load between
 * them or without.  The names the session interns do accumulate, since a
 * name once interned stays, so the limit allows for them too.
 */
static void
queries_do_not_accumulate_indexes(void)
{
	relic_config_t config;
	diag_log_t log;
	relic_t *ctx;
	char *program;
	size_t need;
	size_t least;
	size_t round;
	size_t i;
	int status;

	program = session_program();
	if (program == NULL)
		return;

	need = 0;
	for (i = 0; i < SESSION_QUERIES; i++) {
		least = least_limit(program, LIMIT_MEMORY, STEP_QUERY,
		    session_queries[i]);
		if (least > need)
			need = least;
	}

	bzero(&config, sizeof(config));
	config.max_memory = need + session_vocabulary_bytes(program) +
	    INDEX_SLACK;
	ctx = context_with(&config, program, &log);
	free(program);
	if (ctx == NULL)
		return;

	for (round = 0; round < SESSION_ROUNDS; round++) {
		for (i = 0; i < SESSION_QUERIES; i++) {
			checkf(answers(ctx, session_queries[i],
			    session_rows[i]), SCENARIO_SESSION, "%s should be "
			    "answered under the limit in round %zu",
			    session_queries[i], round);
			checkf(indexes_of(ctx, "e", 3) == 1, SCENARIO_SESSION,
			    "the base store should hold one index after %s, "
			    "not %zu", session_queries[i],
			    indexes_of(ctx, "e", 3));
		}
		status = relic_load_string(ctx, "more.dl",
		    session_loads[round]);
		check(status == 0, SCENARIO_SESSION,
		    "a load between the rounds is admitted");
	}

	relic_destroy(ctx);
}

/*
 * The bytes the symbol pool and the predicate table grow by, past what
 * loading `program` holds, over the session of
 * queries_do_not_accumulate_indexes() run without a limit.  Each query's
 * rewrite keeps the predicates it makes, and each load interns its symbols;
 * a fresh context answering one query holds only that query's predicates,
 * so the least limit of one query alone does not cover the others'.
 */
static size_t
session_vocabulary_bytes(const char *program)
{
	rows_text_t rows;
	diag_log_t log;
	relic_t *ctx;
	size_t loaded;
	size_t grown;
	size_t round;
	size_t i;

	ctx = context_with(NULL, program, &log);
	if (ctx == NULL)
		return (0);

	loaded = ctx->account.pool_bytes + ctx->account.predtab_bytes;
	for (round = 0; round < SESSION_ROUNDS; round++) {
		for (i = 0; i < SESSION_QUERIES; i++)
			(void)rows_of(ctx, session_queries[i], &rows);
		(void)relic_load_string(ctx, "more.dl", session_loads[round]);
	}
	grown = ctx->account.pool_bytes + ctx->account.predtab_bytes - loaded;
	relic_destroy(ctx);

	return (grown);
}

/*
 * The program of the session scenarios: SESSION_FACTS facts of e/3, a rule
 * deriving p/3 from them, a fact of f/1 that a scenario can load again, and
 * a rule negating p, so that a query of it evaluates the whole of p.
 */
static char *
session_program(void)
{
	static const char rest[] = "p(X, Y, Z) :- e(X, Y, Z).\nf(x).\n"
	    "r(X) :- f(X), not p(X, X, X).\n";
	char *facts;
	char *program;

	facts = numbered_facts("e", 3, SESSION_FACTS);
	program = concatenated(facts, rest);
	free(facts);

	if (program == NULL)
		check(false, SCENARIO_SESSION, "the program is written");
	return (program);
}

/*
 * The least limit of `kind` under which a fresh context loads `program`
 * and then takes `step`, answering `query_text` for STEP_QUERY.  It is
 * found by bisection, since whether the steps fit is monotonic in the
 * limit.
 */
static size_t
least_limit(const char *program, enum limit_kind kind, enum step step,
    const char *query_text)
{
	relic_config_t config;
	rows_text_t rows;
	diag_log_t log;
	relic_t *ctx;
	size_t fails;
	size_t fits;
	size_t limit;
	bool done;

	fails = 0;
	fits = LEAST_LIMIT_CEILING;
	while (fits - fails > 1) {
		limit = fails + (fits - fails) / 2;
		bzero(&config, sizeof(config));
		if (kind == LIMIT_TUPLES)
			config.max_tuples = limit;
		else
			config.max_memory = limit;

		ctx = context_with(&config, NULL, &log);
		if (ctx == NULL)
			return (LEAST_LIMIT_CEILING);

		done = relic_load_string(ctx, "test.dl", program) == 0;
		if (done && step == STEP_MATERIALIZE)
			done = relic_materialize(ctx) == 0;
		else if (done && step == STEP_QUERY)
			done = rows_of(ctx, query_text, &rows) == 0;
		relic_destroy(ctx);

		if (done)
			fits = limit;
		else
			fails = limit;
	}

	return (fits);
}

/*
 * A load and an added fact after an evaluation are charged for the facts,
 * not for what the evaluation derived: the commit discards the derived
 * relations before it reserves.  Each limit is the least under which the
 * evaluation runs, and under which the grown program loads.
 *
 * The evaluation is a materialization, or the query r(x)?, which evaluates
 * the whole of p since it negates it.  Under a tuple limit the new facts
 * beside what either evaluation leaves derived would exceed it, so a commit
 * that did not discard first would be refused.  Under a memory limit the
 * arena an evaluation gives back leaves room for the facts either way, and
 * the scenario shows only that nothing is refused.
 */
static void
facts_after_evaluation_are_not_refused(void)
{
	char *grown;
	char *program;

	program = session_program();
	grown = concatenated(program, GROWN_FACTS);
	if (program == NULL || grown == NULL) {
		check(false, SCENARIO_SESSION, "the grown program is written");
		free(program);
		free(grown);
		return;
	}

	facts_after(program, grown, LIMIT_TUPLES, STEP_MATERIALIZE);
	facts_after(program, grown, LIMIT_TUPLES, STEP_QUERY);
	facts_after(program, grown, LIMIT_MEMORY, STEP_MATERIALIZE);
	facts_after(program, grown, LIMIT_MEMORY, STEP_QUERY);

	free(grown);
	free(program);
}

/*
 * The scenario under a limit of `kind`: evaluate `program` by `step`,
 * materializing it or answering SESSION_QUERY, then load two facts and add
 * a third, which together make `grown`.
 */
static void
facts_after(const char *program, const char *grown, enum limit_kind kind,
    enum step step)
{
	relic_value_t values[3];
	relic_config_t config;
	diag_log_t log;
	relic_t *ctx;
	const char *evaluation;
	size_t limit;
	size_t loads;
	int status;

	limit = least_limit(program, kind, step, SESSION_QUERY);
	loads = least_limit(grown, kind, STEP_LOAD, NULL);
	if (loads > limit)
		limit = loads;

	bzero(&config, sizeof(config));
	if (kind == LIMIT_TUPLES)
		config.max_tuples = limit;
	else
		config.max_memory = limit;
	evaluation = step == STEP_MATERIALIZE ? "materialization" : "a query";

	ctx = context_with(&config, program, &log);
	if (ctx == NULL)
		return;

	if (step == STEP_MATERIALIZE) {
		status = relic_materialize(ctx);
		checkf(status == 0, SCENARIO_SESSION, "the program should be "
		    "materialized under %s", kind_name(kind));
	} else {
		checkf(answers(ctx, SESSION_QUERY, "()"), SCENARIO_SESSION,
		    "%s should be answered under %s", SESSION_QUERY,
		    kind_name(kind));
	}

	status = relic_load_string(ctx, "more.dl",
	    "e(b, b, b).\ne(c, c, c).\n");
	checkf(status == 0, SCENARIO_SESSION, "a load after %s should not be "
	    "refused under %s, but reported \"%s\"", evaluation,
	    kind_name(kind), log.last_error);

	values[0] = symbol(ctx, "a");
	values[1] = values[0];
	values[2] = values[0];
	status = relic_fact_add(ctx, "e", 3, values);
	checkf(status == 0, SCENARIO_SESSION, "an added fact after %s should "
	    "not be refused under %s, but reported \"%s\"", evaluation,
	    kind_name(kind), log.last_error);

	relic_destroy(ctx);
}

/*
 * A load whose facts name several predicates and do not fit the memory
 * limit together grows none of their stores, and names the first predicate
 * a fact would grow.  The limit admits the store of c/2 alone, and that of
 * d/1 alone, beside b/1's, so only a check over both stores together
 * refuses the load of both.
 */
static void
a_load_over_the_limit_grows_no_store(void)
{
	relic_config_t config;
	char expected[MESSAGE_MAX];
	diag_log_t log;
	relic_t *ctx;
	char *c_facts;
	char *d_facts;
	char *both;
	size_t before;
	int status;

	c_facts = numbered_facts("c", 2, SESSION_FACTS);
	d_facts = numbered_facts("d", 1, SMALL_STORE_FACTS);
	both = concatenated(c_facts, d_facts);
	if (both == NULL) {
		check(false, SCENARIO_REFUSED, "the facts are written");
		free(c_facts);
		free(d_facts);
		return;
	}

	bzero(&config, sizeof(config));
	config.max_memory = bytes_after_loading(c_facts);
	snprintf(expected, sizeof(expected),
	    "limit exceeded: memory=%zu while loading c/2", config.max_memory);

	ctx = context_with(&config, "b(0).\n", &log);
	if (ctx != NULL) {
		status = relic_load_string(ctx, "c.dl", c_facts);
		check(status == 0, SCENARIO_REFUSED, "c/2's store fits alone");
		relic_destroy(ctx);
	}

	ctx = context_with(&config, "b(0).\n", &log);
	if (ctx != NULL) {
		status = relic_load_string(ctx, "d.dl", d_facts);
		check(status == 0, SCENARIO_REFUSED, "d/1's store fits alone");
		relic_destroy(ctx);
	}

	ctx = context_with(&config, "b(0).\n", &log);
	if (ctx != NULL) {
		before = ctx->account.store_bytes;
		status = relic_load_string(ctx, "both.dl", both);
		checkf(status != 0 && log.errors == 1 &&
		    strcmp(log.last_error, expected) == 0, SCENARIO_REFUSED,
		    "the load should be refused with \"%s\", not \"%s\"",
		    expected, log.last_error);
		checkf(facts_held(ctx, "b", 1) == 1 &&
		    facts_held(ctx, "c", 2) == 0 &&
		    facts_held(ctx, "d", 1) == 0 &&
		    ctx->account.store_bytes == before, SCENARIO_REFUSED,
		    "a refused load should grow none of the stores it names, "
		    "but holds %zu bytes where it held %zu",
		    ctx->account.store_bytes, before);
		relic_destroy(ctx);
	}

	free(both);
	free(c_facts);
	free(d_facts);
}

/*
 * The bytes the memory limit counts against a context once it has loaded
 * b(0) and then `facts` -- its stores, the integer table the facts'
 * integers grew and the predicate table holding its predicates -- or
 * SIZE_MAX when that could not be measured.
 */
static size_t
bytes_after_loading(const char *facts)
{
	diag_log_t log;
	relic_t *ctx;
	size_t bytes;

	ctx = context_with(NULL, "b(0).\n", &log);
	if (ctx == NULL)
		return (SIZE_MAX);

	bytes = SIZE_MAX;
	if (relic_load_string(ctx, "facts.dl", facts) == 0)
		bytes = bytes_held(ctx);
	relic_destroy(ctx);

	return (bytes);
}

/*
 * Intern each of a NULL-terminated list of names as a symbol value.
 */
static void
intern_symbols(relic_t *ctx, const char *const *names)
{
	relic_dom_t dom;

	for (; *names != NULL; names++)
		check(relic_value_intern_bytes(ctx, RELIC_SYMBOL, *names,
		    strlen(*names), &dom) == 0, SCENARIO_REFUSED,
		    "a name is interned");
}

/*
 * The bytes a context's stores, value table, pool and predicate table hold
 * between operations, when no arena or round holds any: the total the
 * memory limit is enforced against.
 */
static size_t
bytes_held(const relic_t *ctx)
{
	return (ctx->account.store_bytes + ctx->account.value_bytes +
	    ctx->account.pool_bytes + ctx->account.predtab_bytes);
}

/*
 * A bound query over a derived predicate with facts reads them through its
 * base view, p$base, which reads p's base store; its rows begin with its
 * facts, and running the query again evaluates the rewrite again, seeds
 * and all.
 */
static void
the_rewrite_reads_facts_through_a_view(void)
{
	const relic_relation_t *store;
	relic_query_t *query;
	rows_text_t rows;
	diag_log_t log;
	relic_t *ctx;
	relic_pred_id_t view;
	relic_pred_id_t viewed;
	size_t run;
	int status;

	ctx = context_with(NULL, "p(a, c).\np(a, b).\np(X, Y) :- e(X, Y).\n"
	    "e(a, d).\ne(b, e).\n", &log);
	if (ctx == NULL)
		return;

	query = relic_query_prepare(ctx, NULL, "p(a, Y)?");
	check(query != NULL, SCENARIO_MAGIC, "the query is prepared");
	for (run = 0; query != NULL && run < 2; run++) {
		bzero(&rows, sizeof(rows));
		rows.ctx = ctx;
		status = relic_query_run(ctx, query, keep_row, &rows);
		checkf(status == 0 && strcmp(rows.text, "c|b|d") == 0,
		    SCENARIO_MAGIC, "run %zu should answer the facts first, "
		    "c|b|d, not \"%s\"", run, rows.text);
	}
	relic_query_destroy(query);

	view = predicate(ctx, "p$base", 2);
	viewed = predicate(ctx, "p", 2);
	store = relic_base_store(ctx, viewed);
	check(relic_pred_of(ctx, view)->views == viewed, SCENARIO_MAGIC,
	    "p$base views p");
	checkf(store != NULL && relic_relation_read(ctx, view) == store &&
	    relic_relation_count(store) == 2, SCENARIO_MAGIC, "a base view "
	    "should read its predicate's base store of 2 facts, not %zu",
	    store != NULL ? relic_relation_count(store) : 0);
	checkf(log.errors == 0 && log.warnings == 0, SCENARIO_MAGIC,
	    "nothing should be reported, not %zu errors and %zu warnings",
	    log.errors, log.warnings);

	relic_destroy(ctx);
}

/*
 * A rewrite abandoned over its limit interned the predicates it generated,
 * p's base view among them.  Nothing reads the base view, so it is not
 * referenced, and a later check warns of none of them: the one warning is
 * the limit's.
 */
static void
an_abandoned_rewrite_leaves_no_warning(void)
{
	static const char query[] = "p(c, Y2, Y3, Y4, Y5, Y6, Y7, Y8)?";
	relic_pred_id_t view;
	diag_log_t log;
	relic_t *ctx;
	char *program;
	size_t before;
	size_t after;
	int status;

	program = widening_program(ABANDONED_ARITY);
	if (program == NULL) {
		check(false, SCENARIO_MAGIC, "the widening program is written");
		return;
	}
	ctx = context_with(NULL, program, &log);
	free(program);
	if (ctx == NULL)
		return;

	status = relic_load_string(ctx, "fact.dl",
	    "p(c, c, c, c, c, c, c, c).\n");
	check(status == 0, SCENARIO_MAGIC, "p gains a fact");

	before = relic_pred_count(ctx);
	checkf(answers(ctx, query, "c c c c c c c") && log.errors == 0 &&
	    log.warnings == 1, SCENARIO_MAGIC, "the rewrite should be "
	    "abandoned with one warning and the query answered, not with %zu "
	    "errors and %zu warnings", log.errors, log.warnings);
	after = relic_pred_count(ctx);

	view = predicate(ctx, "p$base", ABANDONED_ARITY);
	check(view > before && view <= after, SCENARIO_MAGIC,
	    "the rewrite interned p's base view before it was abandoned");
	check(!relic_pred_of(ctx, view)->referenced, SCENARIO_MAGIC,
	    "the base view is not referenced");

	log.warnings = 0;
	status = relic_load_string(ctx, "more.dl",
	    "q(X) :- p(X, X, X, X, X, X, X, X).\n");
	check(status == 0, SCENARIO_MAGIC, "a rule reading p loads");
	status = relic_check(ctx);
	checkf(status == 0 && log.warnings == 0, SCENARIO_MAGIC,
	    "a later check should warn of nothing, but gave %zu warnings, the "
	    "last \"%s\"", log.warnings, log.last_warning);

	relic_destroy(ctx);
}

/*
 * A load stages its facts as it parses them and keeps no statement for any
 * of them, so a file of facts costs the program's tree nothing: the program
 * arena, which holds the tree, grows by as much for thousands of facts and a
 * rule as a control's grows for the rule alone, and the facts are all in
 * their store.
 */
static void
a_loads_facts_build_no_statements(void)
{
	static const char first_rule[] = "r(X) :- e(X, X, X).\n";
	static const char second_rule[] = "s(X) :- r(X).\n";
	diag_log_t control_log;
	diag_log_t log;
	relic_t *control;
	relic_t *ctx;
	char *facts;
	char *text;
	size_t control_growth;
	size_t growth;

	facts = numbered_facts("e", 3, TREELESS_FACTS);
	text = NULL;
	if (facts != NULL)
		text = concatenated(facts, second_rule);
	ctx = context_with(NULL, first_rule, &log);
	control = context_with(NULL, first_rule, &control_log);
	if (text == NULL || ctx == NULL || control == NULL) {
		check(false, SCENARIO_NO_TREE,
		    "the contexts and text are built");
		free(text);
		free(facts);
		relic_destroy(ctx);
		relic_destroy(control);
		return;
	}

	growth = tree_growth(ctx, text);
	control_growth = tree_growth(control, second_rule);

	checkf(growth == control_growth, SCENARIO_NO_TREE, "loading %d "
	    "facts and a rule should grow the tree by %zu bytes as the rule "
	    "alone does, not %zu", TREELESS_FACTS, control_growth, growth);
	checkf(ctx->ast.count == 2 &&
	    facts_held(ctx, "e", 3) == TREELESS_FACTS, SCENARIO_NO_TREE,
	    "the two rules should be statements and the %d facts stored, not "
	    "%zu statements and %zu facts", TREELESS_FACTS, ctx->ast.count,
	    facts_held(ctx, "e", 3));

	free(text);
	free(facts);
	relic_destroy(ctx);
	relic_destroy(control);
}

/* The bytes the program arena obtains while `text` is loaded. */
static size_t
tree_growth(relic_t *ctx, const char *text)
{
	size_t before;
	int status;

	before = relic_arena_total_bytes(&ctx->program_arena);
	status = relic_load_string(ctx, "more.dl", text);
	check(status == 0, SCENARIO_NO_TREE, "the text loads");

	return (relic_arena_total_bytes(&ctx->program_arena) - before);
}

/*
 * A fact of no columns is staged once, stored, seeded into a derived
 * predicate and answered, like a fact of any arity.  done/0's rule reads a
 * predicate that derives nothing, so done holds only because its fact was
 * seeded.
 */
static void
a_fact_of_no_columns_is_a_fact(void)
{
	static const char program[] = "ready.\nready.\ngo :- ready.\n"
	    "done.\ndone :- blocked.\nblocked :- blocked.\n";
	const relic_relation_t *derived;
	diag_log_t log;
	relic_t *ctx;
	int status;

	ctx = context_with(NULL, program, &log);
	if (ctx == NULL)
		return;

	checkf(facts_held(ctx, "ready", 0) == 1 &&
	    facts_held(ctx, "done", 0) == 1 && relic_base_total(ctx) == 2,
	    SCENARIO_ARITY_ZERO, "each fact of no columns should be stored "
	    "once, not %zu facts in all", relic_base_total(ctx));
	check(answers(ctx, "go?", "()"), SCENARIO_ARITY_ZERO,
	    "a rule reading a fact of no columns holds");
	check(answers(ctx, "done?", "()"), SCENARIO_ARITY_ZERO,
	    "the rewrite reads the fact through the base view");

	status = relic_materialize(ctx);
	check(status == 0, SCENARIO_ARITY_ZERO, "the program is materialized");
	derived = relic_relation_read(ctx, predicate(ctx, "done", 0));
	checkf(relic_relation_count(derived) == 1, SCENARIO_ARITY_ZERO,
	    "the fact should be seeded into done/0's relation, which holds "
	    "%zu", relic_relation_count(derived));
	check(answers(ctx, "done?", "()"), SCENARIO_ARITY_ZERO,
	    "the fixed point answers the seeded fact");

	check(answers(ctx, "absent?", "") && log.warnings == 1,
	    SCENARIO_ARITY_ZERO, "a predicate of no columns nothing states "
	    "has no row");

	relic_destroy(ctx);
}

/*
 * One load gives facts to a store nothing has created, to one created
 * empty that kept capacity, and to one holding facts.  Each holds its new
 * facts once, after any it held, in the order the load first states them;
 * the empty store is the same relation afterwards; and the storage total is
 * what the stores hold.  The empty store's capacity is credited against the
 * payload that replaces it, so a limit that admits the load only with that
 * credit admits it.  A second load appends to the store the first filled.
 */
static void
a_store_adopts_or_appends_by_what_it_holds(void)
{
	static const char *const names[] = { "u", "v", "w", NULL };
	static const char *const held_order[] = { "u", "w", "v", NULL };
	static const char *const empty_order[] = { "v", "u", NULL };
	static const char *const empty_later[] = { "v", "u", "w", NULL };
	static const char *const absent_order[] = { "w", NULL };
	relic_tuple_growth_t growth;
	const relic_relation_t *empty_before;
	relic_relation_t *empty;
	diag_log_t log;
	relic_t *ctx;
	size_t retained;
	int status;

	ctx = context_with(NULL, "held(u).\n", &log);
	if (ctx == NULL)
		return;
	intern_symbols(ctx, names);

	empty = relic_relation_read(ctx, predicate(ctx, "empty", 1));
	bzero(&growth, sizeof(growth));
	growth.by_stripe[0] = RETAINED_TUPLES;
	if (empty == NULL || relic_relation_reserve(ctx, empty, &growth) != 0) {
		check(false, SCENARIO_ADOPT, "an empty store is given room");
		relic_destroy(ctx);
		return;
	}
	empty_before = empty;
	retained = relic_relation_bytes(empty);

	/*
	 * Below what the context holds before the load, on purpose: staging
	 * checks the limit against values and a floor of the facts' columns,
	 * which fit easily, and the commit fits only by giving back the empty
	 * store's capacity.
	 */
	ctx->account.limit = bytes_held(ctx) + ctx->account.arena_bytes -
	    retained / 2;

	status = relic_load_string(ctx, "mixed.dl",
	    "empty(v).\nheld(u).\nheld(w).\nabsent(w).\nempty(u).\n"
	    "held(v).\nempty(v).\nheld(w).\n");
	checkf(status == 0, SCENARIO_ADOPT, "a load that fits once the empty "
	    "store's %zu bytes are given back should commit, but reported "
	    "\"%s\"", retained, log.last_error);
	check(holds_in_order(ctx, "held", held_order) &&
	    holds_in_order(ctx, "empty", empty_order) &&
	    holds_in_order(ctx, "absent", absent_order), SCENARIO_ADOPT,
	    "each store holds its new facts once, in the order first stated");
	check(relic_base_store(ctx, predicate(ctx, "empty", 1)) ==
	    empty_before && relic_relation_bytes(empty_before) < retained,
	    SCENARIO_ADOPT, "the empty store is kept and its retained room "
	    "replaced");
	checkf(ctx->account.store_bytes == base_bytes_by_hand(ctx) &&
	    relic_base_total(ctx) == 6, SCENARIO_ADOPT, "the storage total "
	    "should be the stores' %zu bytes, not %zu",
	    base_bytes_by_hand(ctx), ctx->account.store_bytes);

	ctx->account.limit = 0;
	status = relic_load_string(ctx, "later.dl", "empty(w).\nempty(u).\n");
	check(status == 0 && holds_in_order(ctx, "empty", empty_later) &&
	    ctx->account.store_bytes == base_bytes_by_hand(ctx),
	    SCENARIO_ADOPT, "a later load appends to the store a load filled");

	relic_destroy(ctx);
}

/*
 * Whether the base store of name/1 holds exactly the symbols of the
 * NULL-terminated `names`, in that order.
 */
static bool
holds_in_order(relic_t *ctx, const char *name, const char *const *names)
{
	const relic_relation_t *store;
	const relic_dom_t *tuple;
	relic_dom_t dom;
	size_t i;

	store = relic_base_store(ctx, predicate(ctx, name, 1));
	if (store == NULL)
		return (false);

	for (i = 0; names[i] != NULL; i++) {
		tuple = relic_relation_tuple(store, (relic_tuple_id_t)(i + 1));
		if (tuple == NULL || relic_value_intern_bytes(ctx,
		    RELIC_SYMBOL, names[i], strlen(names[i]), &dom) != 0 ||
		    tuple[0] != dom)
			return (false);
	}

	return (relic_relation_count(store) == i);
}

/* The bytes every base store holds, summed store by store. */
static size_t
base_bytes_by_hand(const relic_t *ctx)
{
	const relic_relation_t *store;
	size_t bytes;
	size_t pred;

	bytes = 0;
	for (pred = 1; pred <= ctx->store.bases.count; pred++) {
		store = relic_base_store(ctx, (relic_pred_id_t)pred);
		if (store != NULL)
			bytes += relic_relation_bytes(store);
	}

	return (bytes);
}

/*
 * A batch of new tuples is staged only when the builder's columns can grow
 * to hold them: the capacity adding them one at a time reaches must be a
 * number of bytes a size_t holds, as well as a number of identifiers.  A
 * builder of 2^29 pairs whose columns are full passes the second and, with
 * a 32-bit size_t, fails the first, since one more pair doubles its 2^30
 * identifiers to 2^31, 2^33 bytes.  The builder's projection, which adding
 * one tuple grows as, judges the same.  The builder is given those counts
 * without the columns, which neither check reads.
 */
static void
a_batch_is_staged_only_where_its_columns_grow(void)
{
	relic_builder_projection_t projection;
	relic_tuple_growth_t growth;
	relic_staging_t staging;
	const relic_builder_t *staged;
	relic_builder_t *builder;
	relic_dom_t tuple[2];
	relic_pred_id_t pred;
	diag_log_t log;
	relic_t *ctx;
	enum relic_builder_verdict verdict;
	size_t capacity;
	size_t count;
	bool admitted;
	bool fits;

	ctx = context_with(NULL, NULL, &log);
	if (ctx == NULL)
		return;
	if (relic_pred_intern(ctx, "e", 1, 2, NULL, &pred) != 0 ||
	    relic_value_intern_bytes(ctx, RELIC_SYMBOL, "a", 1,
	    &tuple[0]) != 0 ||
	    relic_value_intern_bytes(ctx, RELIC_SYMBOL, "b", 1,
	    &tuple[1]) != 0) {
		check(false, SCENARIO_BATCH_ROOM, "the predicate and its "
		    "values are interned");
		relic_destroy(ctx);
		return;
	}

	relic_staging_init(&staging, ctx);
	if (relic_staging_add(&staging, pred, tuple) != 0) {
		check(false, SCENARIO_BATCH_ROOM, "a tuple is staged");
		relic_staging_reset(&staging);
		relic_destroy(ctx);
		return;
	}
	staged = relic_staging_builder(&staging, pred);
	builder = (relic_builder_t *)(uintptr_t)staged;
	count = builder->payload.count;
	capacity = builder->payload.column_capacity;
	builder->payload.count = (size_t)1 << 29;
	builder->payload.column_capacity = builder->payload.count * 2;

	fits = ((size_t)1 << 31) <= SIZE_MAX / sizeof(relic_dom_t);
	bzero(&growth, sizeof(growth));
	growth.by_stripe[0] = 1;
	admitted = relic_staging_admits(&staging, pred, &growth, 0);
	verdict = relic_builder_project(builder, &growth, &projection);

	builder->payload.count = count;
	builder->payload.column_capacity = capacity;
	checkf(admitted == fits, SCENARIO_BATCH_ROOM, "one more pair after "
	    "2^29 should be %s when 2^31 identifiers %s a size_t of bytes",
	    fits ? "admitted" : "refused", fits ? "fit" : "do not fit");
	checkf(verdict == (fits ? RELIC_BUILDER_FITS : RELIC_BUILDER_TOO_LARGE),
	    SCENARIO_BATCH_ROOM, "the builder's projection of one more pair "
	    "should be %s, not verdict %d", fits ? "that it fits" :
	    "too large", (int)verdict);

	relic_staging_reset(&staging);
	relic_destroy(ctx);
}

/*
 * A builder grows as a relation of its arity grows by insertion, tuple by
 * tuple, so that an empty base store adopting its payload holds what its
 * own insertions would have reached: after each tuple the columns and
 * every canonical stripe have the same capacities, past the smallest
 * array and through its doublings, and for arity zero, whose one tuple
 * has no columns.
 */
static void
a_builder_grows_as_insertions_grow_a_relation(void)
{
	static const uint16_t arities[] = { 0, 2 };
	relic_builder_t builder;
	char name[8];
	relic_dom_t tuple[2];
	relic_relation_t *relation;
	relic_pred_id_t pred;
	diag_log_t log;
	relic_t *ctx;
	uint64_t hash;
	size_t count;
	size_t a;
	size_t i;
	bool added;
	bool grew;

	ctx = context_with(NULL, NULL, &log);
	if (ctx == NULL)
		return;

	for (a = 0; a < sizeof(arities) / sizeof(arities[0]); a++) {
		snprintf(name, sizeof(name), "g%u", (unsigned int)arities[a]);
		relation = relic_relation_create(ctx, RELIC_ID_NONE, 0,
		    arities[a]);
		if (relation == NULL || relic_pred_intern(ctx, name,
		    strlen(name), arities[a], NULL, &pred) != 0) {
			check(false, SCENARIO_BUILDER_GROWTH, "a relation and "
			    "a predicate are made");
			relic_relation_free(relation);
			break;
		}
		relic_builder_init(&builder, ctx, pred);

		count = arities[a] == 0 ? 1 : 40;
		grew = true;
		for (i = 0; i < count && grew; i++) {
			grew = relic_value_intern_integer(ctx, (int64_t)i,
			    &tuple[0]) == 0;
			tuple[1] = tuple[0];
			hash = relic_tuple_hash(builder.hash_seed, tuple,
			    arities[a]);
			grew = grew && relic_builder_add_hashed(ctx, &builder,
			    tuple, hash) == 0 && relic_relation_insert(ctx,
			    relation, tuple, &added) == 0 && added &&
			    grown_alike(&builder, relation);
			checkf(grew, SCENARIO_BUILDER_GROWTH, "after tuple %zu "
			    "of arity %u, the builder should hold what the "
			    "relation holds", i + 1, (unsigned int)arities[a]);
		}

		relic_builder_reset(&builder);
		relic_relation_clear(ctx, relation);
		relic_relation_free(relation);
	}

	relic_destroy(ctx);
}

/*
 * Whether a builder and a relation hold their tuples in arrays of the same
 * capacities: the columns and every stripe of the canonical index.
 */
static bool
grown_alike(const relic_builder_t *builder, const relic_relation_t *relation)
{
	size_t s;

	if (builder->payload.count != relation->payload.count ||
	    builder->payload.column_capacity !=
	    relation->payload.column_capacity)
		return (false);

	for (s = 0; s < RELIC_STRIPES; s++) {
		if (builder->payload.index[s].capacity !=
		    relation->payload.index[s].capacity)
			return (false);
	}

	return (true);
}

int
main(void)
{
	a_load_keeps_what_is_not_a_fact();
	a_failed_load_commits_no_fact();
	a_refused_load_leaves_the_program_as_it_was();
	a_predicate_is_read_from_the_store_it_has();
	an_added_fact_is_seen_and_kept_once();
	an_added_fact_can_falsify_a_negation();
	an_added_fact_the_limit_refuses_changes_nothing();
	queries_do_not_accumulate_indexes();
	facts_after_evaluation_are_not_refused();
	a_load_over_the_limit_grows_no_store();
	the_rewrite_reads_facts_through_a_view();
	an_abandoned_rewrite_leaves_no_warning();
	a_fact_of_no_columns_is_a_fact();
	a_loads_facts_build_no_statements();
	a_store_adopts_or_appends_by_what_it_holds();
	a_batch_is_staged_only_where_its_columns_grow();
	a_builder_grows_as_insertions_grow_a_relation();

	return (check_summary("test_facts"));
}
