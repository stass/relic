/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for what evaluation does with the integers and strings it
 * computes.
 *
 * A computed integer the value table does not hold, or a computed string
 * the context has not assigned, travels in its batch as a pending marker,
 * and is interned in three steps: the measure promises each one an
 * identifier and projects the size of the table and the pool, the rewrite
 * writes the promised identifiers over the markers, and the commit appends
 * the new values.  The promises must be what the commit assigns, and the
 * table and the pool must reach the same size however the same values
 * arrive, since the memory limit's decisions and the parallel merge both
 * depend on it.  A string whose bytes the pool holds as a name or a
 * symbol's spelling is promised the pool's identifier for them, and needs
 * no pool entry, though it may need the string bits grown.
 *
 * A result arithmetic does not define is an evaluation error that crosses
 * the executor's boundary as a record beside the batch; the evaluation
 * fails and inserts nothing of the batch that met it, and the note about an
 * operand that is not an integer, recorded beside it, does not hide it.
 */

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_PROMISED	"the measure promises what the commit assigns"
#define	SCENARIO_STEPWISE	"the table grows alike however integers arrive"
#define	SCENARIO_REFUSED	"a refused batch leaves the table unchanged"
#define	SCENARIO_UNDEFINED	"an undefined result fails the evaluation"
#define	SCENARIO_NOTED		"a noted operand does not hide an error"
#define	SCENARIO_STRINGS	"the measure promises strings what is assigned"
#define	SCENARIO_NAME		"a computed name grows the string bits alone"
#define	SCENARIO_POOL		"the pool grows alike however strings arrive"
#define	SCENARIO_POOL_REFUSED	"a refused batch leaves the pool unchanged"

/*
 * How many new integers the stepwise scenario appends, in two batches of
 * half as many, in one batch, and one at a time.
 */
#define	STEPWISE_INTEGERS	128

/*
 * The names interned after a first string value in the name scenario,
 * enough that the last lies past what the string bits cover, and the room
 * a batch of pending values of the string scenarios has.
 */
#define	NAMES			4096
#define	BATCH_VALUES		128
#define	BATCH_BYTES		(BATCH_VALUES * 1024)
#define	VALUE_TEXT_MAX		32

/*
 * The diagnostics a context reported: the last error and note, and how many
 * of each.
 */
typedef struct diagnostics diagnostics_t;
typedef struct string_batch string_batch_t;

struct diagnostics {
	char	error[256];
	char	note[256];
	size_t	errors;
	size_t	notes;
};

/*
 * A batch's pending values as the executor hands them over: `count`
 * entries, the strings among them naming their bytes in `bytes`, of which
 * `used` are taken.
 */
struct string_batch {
	relic_pending_t	pending[BATCH_VALUES];
	char		bytes[BATCH_BYTES];
	size_t		count;
	size_t		used;
};

static void	 batch_string(string_batch_t *batch, const char *bytes,
		    size_t length);
static void	 batch_integer(string_batch_t *batch, int64_t integer);
static void	 value_text(char text[static VALUE_TEXT_MAX], size_t ordinal);
static bool	 append_strings(relic_t *ctx, size_t first, size_t count);
static bool	 named_context(relic_t *ctx);
static void	 the_measure_promises_strings_what_is_assigned(void);
static void	 a_computed_name_grows_the_string_bits_alone(void);
static void	 the_pool_grows_alike_however_strings_arrive(void);
static void	 a_refused_batch_leaves_the_pool_unchanged(void);
static void	 record_diagnostic(void *callback_ctx,
		    enum relic_diag_level level, const relic_location_t *where,
		    const char *message);
static int	 count_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static relic_t	*context_for(const char *program, unsigned int flags,
		    size_t max_memory, diagnostics_t *diagnostics);
static size_t	 measure_pending(const relic_t *ctx,
		    const relic_pending_t *pending, size_t count,
		    const char *bytes, relic_pending_map_t *map);
static bool	 append_measured(relic_t *ctx, const int64_t *pending,
		    size_t count);
static void	 the_measure_promises_what_the_commit_assigns(void);
static void	 the_table_grows_alike_however_integers_arrive(void);
static void	 a_refused_batch_leaves_the_table_unchanged(void);
static void	 an_undefined_result_fails_the_evaluation(void);
static void	 a_noted_operand_does_not_hide_an_error(void);

static void
record_diagnostic(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where RELIC_TEST_UNUSED, const char *message)
{
	diagnostics_t *diagnostics;

	diagnostics = callback_ctx;
	if (level == RELIC_ERROR) {
		strlcpy(diagnostics->error, message,
		    sizeof(diagnostics->error));
		diagnostics->errors++;
	} else if (level == RELIC_NOTE) {
		strlcpy(diagnostics->note, message, sizeof(diagnostics->note));
		diagnostics->notes++;
	}
}

static int
count_row(void *callback_ctx, const relic_value_t *values RELIC_TEST_UNUSED,
    size_t count RELIC_TEST_UNUSED)
{
	size_t *rows;

	rows = callback_ctx;
	(*rows)++;

	return (0);
}

/*
 * A context with the given flags and memory limit holding `program`, its
 * diagnostics recorded in `diagnostics`, or NULL having recorded a failure.
 */
static relic_t *
context_for(const char *program, unsigned int flags, size_t max_memory,
    diagnostics_t *diagnostics)
{
	relic_config_t config;
	relic_t *ctx;

	bzero(diagnostics, sizeof(*diagnostics));
	bzero(&config, sizeof(config));
	config.flags = flags;
	config.max_memory = max_memory;

	ctx = relic_create_config(&config, sizeof(config));
	if (ctx == NULL) {
		check(false, "creating a context", "a context is created");
		return (NULL);
	}
	relic_diag_set(ctx, record_diagnostic, diagnostics);

	if (relic_load_string(ctx, "test.dl", program) != 0) {
		checkf(false, "loading a test program", "the program loads: %s",
		    diagnostics->error);
		relic_destroy(ctx);
		return (NULL);
	}

	return (ctx);
}

/*
 * Measure `count` pending values, the strings' bytes in `bytes`, into
 * `map`, with a probe that lives only while it measures, as a merge's
 * does.  Returns what relic_pending_measure() returns, or SIZE_MAX when
 * the probe could not be made.
 */
static size_t
measure_pending(const relic_t *ctx, const relic_pending_t *pending,
    size_t count, const char *bytes, relic_pending_map_t *map)
{
	relic_arena_t arena;
	relic_pending_probe_t probe;
	size_t projected;

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);
	projected = SIZE_MAX;
	if (relic_pending_probe_init(&probe, &arena, count) == 0)
		projected = relic_pending_measure(ctx, pending, count, bytes,
		    &probe, map);
	relic_arena_reset(&arena);

	return (projected);
}

/*
 * Measure `count` pending integers into `map` as the executor hands them
 * over, each an entry of the batch's pending values, and return the bytes
 * the measure projects, or SIZE_MAX when the entries could not be made.
 */
static size_t
measure_integers(const relic_t *ctx, const int64_t *integers, size_t count,
    relic_pending_map_t *map)
{
	relic_pending_t *pending;
	size_t projected;
	size_t i;

	pending = calloc(count > 0 ? count : 1, sizeof(*pending));
	if (pending == NULL)
		return (SIZE_MAX);

	for (i = 0; i < count; i++) {
		pending[i].kind = RELIC_PENDING_INTEGER;
		pending[i].u.integer = integers[i];
	}
	projected = measure_pending(ctx, pending, count, NULL, map);
	free(pending);

	return (projected);
}

/*
 * Intern `count` pending integers as evaluation does, measuring, then
 * committing, and require the table to hold the bytes the measure
 * projected.  Returns whether every step succeeded.
 */
static bool
append_measured(relic_t *ctx, const int64_t *pending, size_t count)
{
	relic_pending_map_t map;
	relic_arena_t arena;
	size_t projected;
	bool appended;

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);
	appended = false;
	if (relic_pending_map_init(&map, &arena, count) == 0) {
		projected = measure_integers(ctx, pending, count, &map);
		appended = relic_pending_commit(ctx, &map) == 0 &&
		    relic_value_storage_bytes(ctx) == projected;
	}
	relic_arena_reset(&arena);

	return (appended);
}

/*
 * A batch's pending integers, some the table holds and some twice over, are
 * promised identifiers the table holds, one per distinct new integer and
 * after those the table already assigned; the rewrite writes them over the
 * markers, and the commit assigns exactly them.
 */
static void
the_measure_promises_what_the_commit_assigns(void)
{
	static const int64_t pending[] = { 30, 10, 30, INT64_MIN, 20, 40 };
	enum { COUNT = sizeof(pending) / sizeof(pending[0]) };
	relic_dom_t tuples[COUNT * 2];
	relic_pending_map_t map;
	relic_arena_t arena;
	relic_dom_t symbol;
	relic_dom_t ten;
	relic_dom_t twenty;
	relic_t *ctx;
	size_t projected;
	size_t i;

	ctx = relic_create();
	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);
	if (ctx == NULL ||
	    relic_value_intern_integer(ctx, 10, &ten) != 0 ||
	    relic_value_intern_integer(ctx, 20, &twenty) != 0 ||
	    relic_value_intern_bytes(ctx, RELIC_SYMBOL, "a", 1,
	    &symbol) != 0 ||
	    relic_pending_map_init(&map, &arena, COUNT) != 0) {
		check(false, SCENARIO_PROMISED, "the context is prepared");
		relic_arena_reset(&arena);
		relic_destroy(ctx);
		return;
	}

	for (i = 0; i < COUNT; i++) {
		tuples[2 * i] = symbol;
		tuples[2 * i + 1] = (relic_dom_t)(i + 1);
	}

	projected = measure_integers(ctx, pending, COUNT, &map);
	check(map.fresh_count == 3, SCENARIO_PROMISED,
	    "three of the integers are new");
	check(map.ids[1] == ten && map.ids[4] == twenty, SCENARIO_PROMISED,
	    "an integer the table holds is promised its identifier");
	check(map.ids[0] == map.ids[2] && map.ids[0] != map.ids[3] &&
	    map.ids[3] != map.ids[5], SCENARIO_PROMISED,
	    "one identifier is promised per distinct new integer");
	check(relic_value_find_integer(ctx, 30) == 0, SCENARIO_PROMISED,
	    "the measure changes nothing in the table");

	relic_pending_rewrite(&map, tuples, COUNT, 2);
	check(relic_pending_commit(ctx, &map) == 0, SCENARIO_PROMISED,
	    "the commit succeeds");
	check(relic_value_storage_bytes(ctx) == projected, SCENARIO_PROMISED,
	    "the table holds the bytes the measure projected");

	for (i = 0; i < COUNT; i++) {
		checkf(tuples[2 * i] == symbol, SCENARIO_PROMISED,
		    "the rewrite leaves tuple %zu's identifier alone", i);
		checkf(tuples[2 * i + 1] ==
		    relic_value_find_integer(ctx, pending[i]),
		    SCENARIO_PROMISED, "tuple %zu holds the identifier the "
		    "table assigned its integer", i);
	}

	relic_arena_reset(&arena);
	relic_destroy(ctx);
}

/*
 * The same new integers appended in two batches, in one, and one interning
 * at a time leave the table the same size: every growth follows the steps
 * one interning at a time would take.
 */
static void
the_table_grows_alike_however_integers_arrive(void)
{
	int64_t integers[STEPWISE_INTEGERS];
	relic_dom_t dom;
	relic_t *halves;
	relic_t *whole;
	relic_t *single;
	size_t half;
	size_t i;
	bool interned;

	for (i = 0; i < STEPWISE_INTEGERS; i++)
		integers[i] = (int64_t)(i * 7919);
	half = STEPWISE_INTEGERS / 2;

	halves = relic_create();
	whole = relic_create();
	single = relic_create();
	if (halves == NULL || whole == NULL || single == NULL) {
		check(false, SCENARIO_STEPWISE, "the contexts are created");
		relic_destroy(halves);
		relic_destroy(whole);
		relic_destroy(single);
		return;
	}

	check(append_measured(halves, integers, half) &&
	    append_measured(halves, integers + half, half), SCENARIO_STEPWISE,
	    "two batches are appended as measured");
	check(append_measured(whole, integers, STEPWISE_INTEGERS),
	    SCENARIO_STEPWISE, "one batch is appended as measured");

	interned = true;
	for (i = 0; i < STEPWISE_INTEGERS; i++)
		interned = interned &&
		    relic_value_intern_integer(single, integers[i], &dom) == 0;
	check(interned, SCENARIO_STEPWISE, "every integer is interned");

	checkf(relic_value_storage_bytes(halves) ==
	    relic_value_storage_bytes(whole) &&
	    relic_value_storage_bytes(whole) ==
	    relic_value_storage_bytes(single), SCENARIO_STEPWISE,
	    "the tables should hold one size, not %zu, %zu and %zu",
	    relic_value_storage_bytes(halves), relic_value_storage_bytes(whole),
	    relic_value_storage_bytes(single));
	check(halves->account.value_bytes + halves->account.pool_bytes ==
	    relic_value_storage_bytes(halves), SCENARIO_STEPWISE,
	    "the memory limit is charged what it holds");

	relic_destroy(halves);
	relic_destroy(whole);
	relic_destroy(single);
}

/*
 * A batch of integers whose table the memory limit does not admit is
 * refused before anything is appended.  In an evaluation the refusal fails
 * it, and what the limit counts stays within the limit.
 */
static void
a_refused_batch_leaves_the_table_unchanged(void)
{
	static const char program[] =
	    "n(0).\n"
	    "n(Y) :- n(X), X < 2000, Y = X + 1.\n";
	int64_t pending[RELIC_INSERT_BATCH];
	relic_pending_map_t map;
	relic_arena_t arena;
	diagnostics_t diagnostics;
	const relic_relation_t *relation;
	relic_pred_id_t pred;
	relic_t *ctx;
	size_t bytes_before;
	size_t count_before;
	size_t projected;
	size_t limit;
	size_t i;

	limit = 50000;
	ctx = context_for(program, 0, limit, &diagnostics);
	if (ctx == NULL)
		return;

	relation = NULL;
	if (relic_pred_intern(ctx, "n", 1, 1, NULL, &pred) == 0)
		relation = relic_base_store(ctx, pred);
	for (i = 0; i < RELIC_INSERT_BATCH; i++)
		pending[i] = (int64_t)i * 1000003 + 5;

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);
	if (relation == NULL ||
	    relic_pending_map_init(&map, &arena, RELIC_INSERT_BATCH) != 0) {
		check(false, SCENARIO_REFUSED, "the batch is prepared");
		relic_arena_reset(&arena);
		relic_destroy(ctx);
		return;
	}

	bytes_before = relic_value_storage_bytes(ctx);
	count_before = relic_value_count_of(ctx, RELIC_INTEGER);
	projected = measure_integers(ctx, pending, RELIC_INSERT_BATCH, &map);
	check(relic_relation_admit_values(ctx, relation, projected) == 0,
	    SCENARIO_REFUSED, "a batch the limit admits is admitted");
	check(relic_relation_admit_values(ctx, relation, limit + 1) != 0,
	    SCENARIO_REFUSED, "a batch past the limit is refused");
	checkf(strcmp(diagnostics.error,
	    "limit exceeded: memory=50000 while deriving n/1") == 0,
	    SCENARIO_REFUSED, "the refusal names the relation: \"%s\"",
	    diagnostics.error);
	check(relic_value_storage_bytes(ctx) == bytes_before &&
	    relic_value_count_of(ctx, RELIC_INTEGER) == count_before &&
	    relic_value_find_integer(ctx, pending[1]) == 0, SCENARIO_REFUSED,
	    "the refused batch appended nothing");
	relic_arena_reset(&arena);

	check(relic_materialize(ctx) != 0, SCENARIO_REFUSED,
	    "counting past the limit fails");
	checkf(ctx->account.store_bytes + relic_value_storage_bytes(ctx) <=
	    limit, SCENARIO_REFUSED, "the relations and the values should "
	    "hold at most %zu bytes, not %zu", limit,
	    ctx->account.store_bytes + relic_value_storage_bytes(ctx));
	check(ctx->account.value_bytes + ctx->account.pool_bytes ==
	    relic_value_storage_bytes(ctx), SCENARIO_REFUSED,
	    "the values are charged what they hold");

	relic_destroy(ctx);
}

/*
 * An overflow in the middle of a batch fails the evaluation with the error
 * naming the rule, and none of the batch's tuples, the ones derived before
 * the overflow among them, is left in the relation.  A later query over the
 * facts alone is answered.
 */
static void
an_undefined_result_fails_the_evaluation(void)
{
	static const char program[] =
	    "n(10). n(20). n(30). n(9223372036854775807). n(40).\n"
	    "next(Y) :- n(X), Y = X + 1.\n";
	diagnostics_t diagnostics;
	relic_query_t *query;
	relic_stats_t stats;
	relic_t *ctx;
	size_t rows;

	ctx = context_for(program, 0, 0, &diagnostics);
	if (ctx == NULL)
		return;

	check(relic_materialize(ctx) != 0, SCENARIO_UNDEFINED,
	    "the evaluation fails");
	checkf(diagnostics.errors == 1 &&
	    strcmp(diagnostics.error, "integer overflow in rule next/1") == 0,
	    SCENARIO_UNDEFINED, "one error names the rule: \"%s\"",
	    diagnostics.error);

	relic_stats(ctx, &stats, sizeof(stats));
	checkf(stats.tuples == 5, SCENARIO_UNDEFINED,
	    "only the five facts should be held, not %zu tuples",
	    stats.tuples);
	check(relic_value_find_integer(ctx, 11) == 0 &&
	    relic_value_find_integer(ctx, 31) == 0, SCENARIO_UNDEFINED,
	    "no integer of the failed batch is interned");

	query = relic_query_prepare(ctx, NULL, "n(X), X < 25?");
	rows = 0;
	check(query != NULL &&
	    relic_query_run(ctx, query, count_row, &rows) == 0 && rows == 2,
	    SCENARIO_UNDEFINED, "a query of the facts is answered");
	relic_query_destroy(query);

	relic_destroy(ctx);
}

/*
 * A rule that meets a symbol operand, which it notes, and then an overflow
 * fails with the error: the note and the error are recorded apart, so the
 * one does not stand in for the other.
 */
static void
a_noted_operand_does_not_hide_an_error(void)
{
	static const char program[] =
	    "v(a). v(9223372036854775807).\n"
	    "next(Y) :- v(X), Y = X + 1.\n";
	diagnostics_t diagnostics;
	relic_t *ctx;

	ctx = context_for(program, RELIC_F_NOTE_MIXED, 0, &diagnostics);
	if (ctx == NULL)
		return;

	check(relic_materialize(ctx) != 0, SCENARIO_NOTED,
	    "the evaluation fails");
	checkf(diagnostics.notes == 1 && strcmp(diagnostics.note,
	    "operand of + is a symbol; the comparison is false") == 0,
	    SCENARIO_NOTED, "the operand is noted: \"%s\"", diagnostics.note);
	checkf(diagnostics.errors == 1 &&
	    strcmp(diagnostics.error, "integer overflow in rule next/1") == 0,
	    SCENARIO_NOTED, "the overflow is reported: \"%s\"",
	    diagnostics.error);

	relic_destroy(ctx);
}

/*
 * Append a string to the batch's pending values, its bytes to the batch's
 * pending bytes, as an EMIT does.
 */
static void
batch_string(string_batch_t *batch, const char *bytes, size_t length)
{
	relic_pending_t *pending;

	assert(batch->count < BATCH_VALUES &&
	    length <= BATCH_BYTES - batch->used);
	pending = &batch->pending[batch->count];
	pending->kind = RELIC_PENDING_TEXT;
	pending->u.text.offset = batch->used;
	pending->u.text.length = length;
	bcopy(bytes, batch->bytes + batch->used, length);
	batch->used += length;
	batch->count++;
}

static void
batch_integer(string_batch_t *batch, int64_t integer)
{
	relic_pending_t *pending;

	assert(batch->count < BATCH_VALUES);
	pending = &batch->pending[batch->count];
	pending->kind = RELIC_PENDING_INTEGER;
	pending->u.integer = integer;
	batch->count++;
}

/*
 * The text of the `ordinal`th distinct string a scenario computes.
 */
static void
value_text(char text[static VALUE_TEXT_MAX], size_t ordinal)
{
	snprintf(text, VALUE_TEXT_MAX, "computed-%zu", ordinal);
}

/*
 * Intern the strings numbered `first` to `first + count - 1` as evaluation
 * does, measuring and then committing them as one batch, and require the
 * pool and the table to hold the bytes the measure projected.  Returns
 * whether every step succeeded.
 */
static bool
append_strings(relic_t *ctx, size_t first, size_t count)
{
	char text[VALUE_TEXT_MAX];
	string_batch_t *batch;
	relic_pending_map_t map;
	relic_arena_t arena;
	size_t projected;
	size_t i;
	bool appended;

	batch = calloc(1, sizeof(*batch));
	if (batch == NULL)
		return (false);
	for (i = 0; i < count; i++) {
		value_text(text, first + i);
		batch_string(batch, text, strlen(text));
	}

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);
	appended = false;
	if (relic_pending_map_init(&map, &arena, count) == 0) {
		projected = measure_pending(ctx, batch->pending,
		    batch->count, batch->bytes, &map);
		appended = relic_pending_commit(ctx, &map) == 0 &&
		    relic_value_storage_bytes(ctx) == projected;
	}
	relic_arena_reset(&arena);
	free(batch);

	return (appended);
}

/*
 * A batch's pending strings and integers together, in the three cases of
 * the measure: a string the context has assigned is promised its
 * identifier; one whose bytes the pool holds as a symbol's spelling is
 * promised the pool's identifier for them, with the string bit; and each
 * distinct new string is promised the pool's next identifier, in order of
 * first occurrence.  The measure writes nothing, and the commit assigns
 * exactly what was promised, making the symbol's spelling a string value
 * too without changing the symbol.
 */
static void
the_measure_promises_strings_what_is_assigned(void)
{
	/*
	 * The batch's pending values, in batch order, named by the case of
	 * the measure each is.
	 */
	enum {
		NEW_FIRST,		/* a new string */
		HELD,			/* a string the context assigned */
		NEW_INTEGER,		/* a new integer */
		SPELLED,		/* a symbol's spelling */
		NEW_FIRST_AGAIN,	/* the first new string once more */
		NEW_SECOND,		/* another new string */
		NEW_INTEGER_AGAIN,	/* the new integer once more */
		COUNT
	};
	static const struct {
		enum relic_pending_kind	 kind;
		const char		*text;		/* TEXT */
		int64_t			 integer;	/* INTEGER */
	} values[COUNT] = {
		[NEW_FIRST] = { RELIC_PENDING_TEXT, "new1", 0 },
		[HELD] = { RELIC_PENDING_TEXT, "held", 0 },
		[NEW_INTEGER] = { RELIC_PENDING_INTEGER, NULL, 77 },
		[SPELLED] = { RELIC_PENDING_TEXT, "sym", 0 },
		[NEW_FIRST_AGAIN] = { RELIC_PENDING_TEXT, "new1", 0 },
		[NEW_SECOND] = { RELIC_PENDING_TEXT, "new2", 0 },
		[NEW_INTEGER_AGAIN] = { RELIC_PENDING_INTEGER, NULL, 77 }
	};
	relic_dom_t tuples[COUNT];
	string_batch_t *batch;
	relic_pending_map_t map;
	relic_arena_t arena;
	relic_dom_t held;
	relic_dom_t symbol;
	relic_dom_t spelled;
	relic_dom_t first_new;
	relic_dom_t found;
	relic_t *ctx;
	size_t strings_before;
	size_t pool_count;
	size_t projected;
	size_t i;

	ctx = relic_create();
	batch = calloc(1, sizeof(*batch));
	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);
	if (ctx == NULL || batch == NULL ||
	    relic_value_intern_bytes(ctx, RELIC_STRING, "held", 4,
	    &held) != 0 ||
	    relic_value_intern_bytes(ctx, RELIC_SYMBOL, "sym", 3,
	    &symbol) != 0 ||
	    relic_pending_map_init(&map, &arena, COUNT) != 0) {
		check(false, SCENARIO_STRINGS, "the context is prepared");
		relic_arena_reset(&arena);
		free(batch);
		relic_destroy(ctx);
		return;
	}

	for (i = 0; i < COUNT; i++) {
		if (values[i].kind == RELIC_PENDING_INTEGER)
			batch_integer(batch, values[i].integer);
		else
			batch_string(batch, values[i].text,
			    strlen(values[i].text));
		tuples[i] = (relic_dom_t)(i + 1);
	}

	strings_before = relic_value_count_of(ctx, RELIC_STRING);
	pool_count = ctx->symbols.count;
	spelled = RELIC_DOM_STRING | (symbol & RELIC_DOM_INDEX_MASK);
	first_new = RELIC_DOM_STRING | (relic_dom_t)(pool_count + 1);

	projected = measure_pending(ctx, batch->pending, batch->count,
	    batch->bytes, &map);
	check(map.string_count == 2 && map.fresh_count == 1, SCENARIO_STRINGS,
	    "two strings and one integer are new");
	check(map.ids[HELD] == held, SCENARIO_STRINGS,
	    "a string the context assigned is promised its identifier");
	check(map.ids[SPELLED] == spelled, SCENARIO_STRINGS,
	    "a symbol's spelling is promised the pool's identifier for it");
	check(map.ids[NEW_FIRST] == first_new &&
	    map.ids[NEW_FIRST_AGAIN] == first_new &&
	    map.ids[NEW_SECOND] == first_new + 1, SCENARIO_STRINGS,
	    "each distinct new string is promised the pool's next identifier");
	check(map.ids[NEW_INTEGER] == map.ids[NEW_INTEGER_AGAIN] &&
	    (map.ids[NEW_INTEGER] & RELIC_DOM_TYPE_MASK) == RELIC_DOM_INTEGER,
	    SCENARIO_STRINGS, "the new integer is promised one identifier");
	check(ctx->symbols.count == pool_count &&
	    relic_value_find_string(ctx, "new1", 4) == 0 &&
	    relic_value_find_string(ctx, "sym", 3) == 0 &&
	    relic_value_find_integer(ctx, 77) == 0, SCENARIO_STRINGS,
	    "the measure changes nothing in the pool or the table");

	relic_pending_rewrite(&map, tuples, COUNT, 1);
	check(relic_pending_commit(ctx, &map) == 0, SCENARIO_STRINGS,
	    "the commit succeeds");
	check(relic_value_storage_bytes(ctx) == projected, SCENARIO_STRINGS,
	    "the pool and the table hold the bytes the measure projected");

	for (i = 0; i < COUNT; i++) {
		if (values[i].kind == RELIC_PENDING_INTEGER)
			found = relic_value_find_integer(ctx,
			    values[i].integer);
		else
			found = relic_value_find_string(ctx, values[i].text,
			    strlen(values[i].text));
		checkf(found != 0 && tuples[i] == found, SCENARIO_STRINGS,
		    "tuple %zu holds the identifier the context assigned its "
		    "value", i);
	}
	check(ctx->symbols.count == pool_count + 2 &&
	    relic_value_count_of(ctx, RELIC_STRING) == strings_before + 3,
	    SCENARIO_STRINGS, "two pool entries are added and three strings "
	    "become values");
	check(relic_value_intern_bytes(ctx, RELIC_SYMBOL, "sym", 3,
	    &found) == 0 && found == symbol, SCENARIO_STRINGS,
	    "the symbol keeps its identifier");

	relic_arena_reset(&arena);
	free(batch);
	relic_destroy(ctx);
}

/*
 * A computed string equal to the name the pool numbered last, which no
 * value has used, is promised the name's identifier and needs no pool
 * entry; but the string bits cover only the identifiers some value used,
 * so the measure projects their growth to the name's identifier, and the
 * commit makes exactly that growth.
 */
static void
a_computed_name_grows_the_string_bits_alone(void)
{
	char name[VALUE_TEXT_MAX];
	relic_pending_t pending;
	relic_pending_map_t map;
	relic_arena_t arena;
	relic_dom_t first;
	relic_dom_t tuple;
	relic_t *ctx;
	uint32_t id;
	size_t pool_count;
	size_t before;
	size_t projected;
	size_t i;
	bool named;

	ctx = relic_create();
	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);
	named = ctx != NULL && relic_value_intern_bytes(ctx, RELIC_STRING,
	    "a", 1, &first) == 0;
	id = 0;
	for (i = 0; named && i < NAMES; i++) {
		snprintf(name, sizeof(name), "name%zu", i);
		named = relic_symbol_intern(ctx, name, strlen(name), &id) == 0;
	}
	if (!named || relic_pending_map_init(&map, &arena, 1) != 0) {
		check(false, SCENARIO_NAME, "the context is prepared");
		relic_arena_reset(&arena);
		relic_destroy(ctx);
		return;
	}

	pending.kind = RELIC_PENDING_TEXT;
	pending.u.text.offset = 0;
	pending.u.text.length = strlen(name);
	pool_count = ctx->symbols.count;
	before = relic_value_storage_bytes(ctx);

	projected = measure_pending(ctx, &pending, 1, name, &map);
	check(map.string_count == 0 && map.ids[0] == (RELIC_DOM_STRING | id),
	    SCENARIO_NAME, "the name's identifier is promised, with no entry");
	check(projected > before, SCENARIO_NAME,
	    "the measure projects the string bits' growth");

	tuple = 1;
	relic_pending_rewrite(&map, &tuple, 1, 1);
	check(relic_pending_commit(ctx, &map) == 0 &&
	    relic_value_storage_bytes(ctx) == projected, SCENARIO_NAME,
	    "the commit grows the bits by what the measure projected");
	check(ctx->symbols.count == pool_count && tuple == map.ids[0] &&
	    relic_value_find_string(ctx, name, strlen(name)) == tuple,
	    SCENARIO_NAME, "the name is a string value, in no new entry");

	relic_arena_reset(&arena);
	relic_destroy(ctx);
}

/*
 * Give a context a string value and then NAMES names no value uses, as a
 * program's predicates are.  Returns whether it could; a NULL context
 * cannot.
 */
static bool
named_context(relic_t *ctx)
{
	char name[VALUE_TEXT_MAX];
	relic_dom_t first;
	uint32_t id;
	size_t i;

	if (ctx == NULL || relic_value_intern_bytes(ctx, RELIC_STRING, "a", 1,
	    &first) != 0)
		return (false);

	for (i = 0; i < NAMES; i++) {
		snprintf(name, sizeof(name), "name%zu", i);
		if (relic_symbol_intern(ctx, name, strlen(name), &id) != 0)
			return (false);
	}

	return (true);
}

/*
 * The same new strings appended in two batches, in one, and one interning
 * at a time leave the pool and the string bits the same size, as the
 * integer table is: every growth follows the steps one interning at a time
 * would take.  A string value and a run of names no value uses come first,
 * so that the first new string's bit lies far past what the bits cover and
 * they grow by a jump rather than by one.
 */
static void
the_pool_grows_alike_however_strings_arrive(void)
{
	char text[VALUE_TEXT_MAX];
	relic_t *halves;
	relic_t *whole;
	relic_t *single;
	relic_dom_t dom;
	size_t half;
	size_t i;
	bool interned;

	half = BATCH_VALUES / 2;
	halves = relic_create();
	whole = relic_create();
	single = relic_create();
	if (!named_context(halves) || !named_context(whole) ||
	    !named_context(single)) {
		check(false, SCENARIO_POOL, "the contexts are created");
		relic_destroy(halves);
		relic_destroy(whole);
		relic_destroy(single);
		return;
	}

	check(append_strings(halves, 0, half) &&
	    append_strings(halves, half, half), SCENARIO_POOL,
	    "two batches are appended as measured");
	check(append_strings(whole, 0, BATCH_VALUES), SCENARIO_POOL,
	    "one batch is appended as measured");

	interned = true;
	for (i = 0; i < BATCH_VALUES; i++) {
		value_text(text, i);
		interned = interned && relic_value_intern_bytes(single,
		    RELIC_STRING, text, strlen(text), &dom) == 0;
	}
	check(interned, SCENARIO_POOL, "every string is interned");

	checkf(halves->account.pool_bytes == whole->account.pool_bytes &&
	    whole->account.pool_bytes == single->account.pool_bytes &&
	    relic_value_storage_bytes(halves) ==
	    relic_value_storage_bytes(whole) &&
	    relic_value_storage_bytes(whole) ==
	    relic_value_storage_bytes(single), SCENARIO_POOL,
	    "the pools and the values should hold one size, not %zu, %zu "
	    "and %zu in all", relic_value_storage_bytes(halves),
	    relic_value_storage_bytes(whole),
	    relic_value_storage_bytes(single));
	check(halves->account.value_bytes + halves->account.pool_bytes ==
	    relic_value_storage_bytes(halves), SCENARIO_POOL,
	    "the memory limit is charged what they hold");

	relic_destroy(halves);
	relic_destroy(whole);
	relic_destroy(single);
}

/*
 * A batch of strings whose pool growth the memory limit does not admit is
 * refused at the measure, naming the relation, before anything is
 * appended: the pool and the string bits are as they were.
 */
static void
a_refused_batch_leaves_the_pool_unchanged(void)
{
	char value[BATCH_BYTES / BATCH_VALUES];
	char text[VALUE_TEXT_MAX];
	string_batch_t *batch;
	relic_pending_map_t map;
	relic_arena_t arena;
	diagnostics_t diagnostics;
	const relic_relation_t *relation;
	relic_pred_id_t pred;
	relic_t *ctx;
	size_t pool_before;
	size_t count_before;
	size_t projected;
	size_t limit;
	size_t i;
	bool unassigned;

	limit = 50000;
	ctx = context_for("s(\"x\").\n", 0, limit, &diagnostics);
	batch = calloc(1, sizeof(*batch));
	relation = NULL;
	if (ctx != NULL && relic_pred_intern(ctx, "s", 1, 1, NULL, &pred) == 0)
		relation = relic_base_store(ctx, pred);

	relic_arena_init(&arena, RELIC_ARENA_ANY_OBJECT);
	if (relation == NULL || batch == NULL ||
	    relic_pending_map_init(&map, &arena, BATCH_VALUES) != 0) {
		check(false, SCENARIO_POOL_REFUSED, "the batch is prepared");
		relic_arena_reset(&arena);
		free(batch);
		relic_destroy(ctx);
		return;
	}

	for (i = 0; i < BATCH_VALUES; i++) {
		value_text(text, i);
		memset(value, '.', sizeof(value));
		bcopy(text, value, strlen(text));
		batch_string(batch, value, sizeof(value));
	}

	pool_before = ctx->account.pool_bytes;
	count_before = ctx->symbols.count;
	projected = measure_pending(ctx, batch->pending, batch->count,
	    batch->bytes, &map);
	check(projected > limit, SCENARIO_POOL_REFUSED,
	    "the batch's strings take more than the limit");
	check(relic_relation_admit_values(ctx, relation, projected) != 0,
	    SCENARIO_POOL_REFUSED, "the batch is refused");
	checkf(strcmp(diagnostics.error,
	    "limit exceeded: memory=50000 while deriving s/1") == 0,
	    SCENARIO_POOL_REFUSED, "the refusal names the relation: \"%s\"",
	    diagnostics.error);

	unassigned = true;
	for (i = 0; i < batch->count; i++)
		unassigned = unassigned && relic_value_find_string(ctx,
		    batch->bytes + batch->pending[i].u.text.offset,
		    batch->pending[i].u.text.length) == 0;
	check(ctx->account.pool_bytes == pool_before &&
	    ctx->symbols.count == count_before && unassigned,
	    SCENARIO_POOL_REFUSED, "the refused batch appended nothing");

	relic_arena_reset(&arena);
	free(batch);
	relic_destroy(ctx);
}

int
main(void)
{
	the_measure_promises_what_the_commit_assigns();
	the_table_grows_alike_however_integers_arrive();
	a_refused_batch_leaves_the_table_unchanged();
	an_undefined_result_fails_the_evaluation();
	a_noted_operand_does_not_hide_an_error();
	the_measure_promises_strings_what_is_assigned();
	a_computed_name_grows_the_string_bits_alone();
	the_pool_grows_alike_however_strings_arrive();
	a_refused_batch_leaves_the_pool_unchanged();

	return (check_summary("test_compute"));
}
