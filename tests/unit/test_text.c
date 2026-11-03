/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the computed strings of a running plan: the text
 * stack they are built in and the pending bytes a head tuple carries them
 * in, run directly rather than through an evaluation.
 *
 * Each scenario loads a program, plans its one rule into its head's
 * relation, and runs the plan on a scratch holding a round's text buffers,
 * a call at a time, taking each batch as a caller would.  What the plan
 * derives is checked against what the rule means, and the buffers against
 * what the plan needed: a text stack cut back as bindings are replaced
 * never grows however many strings the plan computes, and the pending
 * bytes grow only for a tuple no empty batch could hold.
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

#define	SCENARIO_CUT_BACK	"the text stack is cut back as bindings change"
#define	SCENARIO_BELOW		"a string bound below keeps its bytes"
#define	SCENARIO_FULL		"a batch is full when its pending bytes are"
#define	SCENARIO_OVERSIZED	"a tuple no batch holds grows the bytes once"
#define	SCENARIO_REFUSED	"a text stack the limit refuses fails the plan"
#define	SCENARIO_RESOLVED	"an evaluation resolves every batch whole"
#define	SCENARIO_CONSTANT	"a constant pattern runs as its rule compiled it"
#define	SCENARIO_PER_BINDING	"a variable pattern is compiled per binding"
#define	SCENARIO_INVALID	"an invalid pattern fails on its binding"

/*
 * The facts n(X) whose decimal text begins with the digit it ends with:
 * the ten of one digit, nine of two, and ninety of three.
 */
#define	SAME_ENDS	(10 + 9 + 90)

/*
 * The binding at which the invalid-pattern scenario's pattern stops being
 * one: every n(X) below it matches, and n(INVALID_AT) fails the plan.
 */
#define	INVALID_AT	501

/*
 * The facts every scenario's rule reads: n(0) to n(FACTS - 1).
 */
#define	FACTS		1000

/*
 * A forty-byte string, and a thousand of them computed one per fact, which
 * together are ten times what the text stack starts with: a stack that
 * kept any of them past its binding would have to grow.
 */
#define	PREFIX		"0123456789abcdefghij0123456789abcdefghij"

_Static_assert(FACTS * (sizeof(PREFIX) - 1) > 8 * RELIC_TEXT_STACK_INITIAL,
    "the strings a plan computes would outgrow a stack that kept them");

/*
 * The bytes of the long string the pending-bytes scenarios concatenate:
 * five tuples holding one fit the pending bytes and six do not.  The
 * oversized scenario doubles it, past what an empty batch holds.
 */
#define	LONG_BYTES	200000u
#define	LONG_TUPLES	20

_Static_assert(5 * (LONG_BYTES + 2) <= RELIC_PENDING_BYTES &&
    6 * LONG_BYTES > RELIC_PENDING_BYTES,
    "five tuples fit the pending bytes and six do not");

typedef struct fixture fixture_t;
typedef struct batch_log batch_log_t;

/*
 * A context holding a program, the plan of its one rule, and the scratch
 * that runs it, with a round's text buffers charged to the context.
 */
struct fixture {
	relic_t			*ctx;
	relic_arena_t		 arena;
	relic_plan_t		 plan;
	relic_exec_env_t	 env;
	relic_op_state_t	*states;
	relic_dom_t		*batch;
	relic_pending_t		*pending;
	relic_scratch_t		 scratch;
	bool			 texts;
};

/*
 * What a run's batches held: how many there were, the most tuples one
 * held, the tuples in all, and whether each pending string held the bytes
 * the scenario's check said it should.
 */
struct batch_log {
	size_t	batches;
	size_t	most_tuples;
	size_t	tuples;
	bool	strings_hold;
};

/*
 * What a scenario checks of each pending string a batch holds.
 */
typedef bool (*string_check_t)(const char *bytes, size_t length);

static char	*program_with(const char *rule, size_t long_bytes);
static bool	 fixture_open(fixture_t *fixture, const char *program,
		    const char *scenario);
static void	 fixture_close(fixture_t *fixture);
static bool	 run_to_end(fixture_t *fixture, const char *scenario,
		    string_check_t check_string, batch_log_t *log);
static bool	 run_rule(const char *rule, size_t long_bytes,
		    const char *scenario, string_check_t check_string,
		    batch_log_t *log, size_t *stack_capacityp,
		    size_t *pending_capacityp);
static bool	 any_string(const char *bytes, size_t length);
static bool	 prefix_then_digits(const char *bytes, size_t length);
static bool	 long_then_digits(const char *bytes, size_t length);
static bool	 digits(const char *bytes, size_t length);
static bool	 cut_then_digit(const char *bytes, size_t length);
static void	 the_text_stack_is_cut_back(void);
static void	 a_string_bound_below_keeps_its_bytes(void);
static void	 a_batch_is_full_when_its_pending_bytes_are(void);
static void	 a_tuple_no_batch_holds_grows_the_bytes_once(void);
static void	 a_text_stack_the_limit_refuses_fails_the_plan(void);
static void	 an_evaluation_resolves_every_batch_whole(void);
static void	 a_constant_pattern_runs_as_its_rule_compiled_it(void);
static void	 a_variable_pattern_is_compiled_per_binding(void);
static void	 an_invalid_pattern_fails_on_its_binding(void);
static const relic_pattern_t *planned_pattern(const relic_plan_t *plan);
static size_t	 round_bytes(const relic_t *ctx);
static int	 count_rows(void *callback_ctx, const relic_value_t *values,
		    size_t count);
static int	 check_row(void *callback_ctx, const relic_value_t *values,
		    size_t count);

/*
 * The facts n(0) to n(FACTS - 1), the fact long(L) whose string is
 * `long_bytes` bytes of 'x' when that is not zero, and `rule`.  Returns
 * NULL, having failed no check, when the text could not be allocated.
 */
static char *
program_with(const char *rule, size_t long_bytes)
{
	size_t room;
	size_t used;
	char *program;
	int i;

	room = (size_t)FACTS * 16 + long_bytes + strlen(rule) + 64;
	program = malloc(room);
	if (program == NULL)
		return (NULL);

	used = 0;
	for (i = 0; i < FACTS; i++)
		used += (size_t)snprintf(program + used, room - used,
		    "n(%d).\n", i);
	if (long_bytes > 0) {
		used += (size_t)snprintf(program + used, room - used,
		    "long(\"");
		memset(program + used, 'x', long_bytes);
		used += long_bytes;
		used += (size_t)snprintf(program + used, room - used,
		    "\").\n");
	}
	snprintf(program + used, room - used, "%s", rule);

	return (program);
}

/*
 * Load `program`, plan its last rule into its head's relation in the order
 * it is written, and ready the scratch to run it with a round's text
 * buffers.  Returns whether all
 * of that succeeded; the fixture is closed either way by fixture_close().
 */
static bool
fixture_open(fixture_t *fixture, const char *program, const char *scenario)
{
	enum relic_exec_failure failure;
	relic_config_t config;
	const relic_rule_t *rule;
	relic_relation_t *target;
	uint16_t arity;

	bzero(fixture, sizeof(*fixture));
	relic_arena_init(&fixture->arena, RELIC_ARENA_ANY_OBJECT);

	bzero(&config, sizeof(config));
	config.flags = RELIC_UNSTABLE_NO_REORDER;
	fixture->ctx = relic_create_config(&config, sizeof(config));
	if (fixture->ctx == NULL ||
	    relic_load_string(fixture->ctx, "text.dl", program) != 0 ||
	    relic_check(fixture->ctx) != 0) {
		check(false, scenario, "the program should load");
		return (false);
	}

	rule = &fixture->ctx->program.rules.entries[
	    fixture->ctx->program.rules.count - 1];
	target = relic_relation_read(fixture->ctx, rule->head);
	if (target == NULL || relic_plan_rule(fixture->ctx, &fixture->arena,
	    rule, NULL, &fixture->plan) != 0) {
		check(false, scenario, "the rule should be planned");
		return (false);
	}

	arity = relic_relation_arity(target);
	fixture->states = calloc(fixture->plan.op_count,
	    sizeof(*fixture->states));
	fixture->batch = calloc((size_t)RELIC_INSERT_BATCH * arity,
	    sizeof(*fixture->batch));
	fixture->pending = calloc((size_t)RELIC_INSERT_BATCH * arity,
	    sizeof(*fixture->pending));
	if (fixture->states == NULL || fixture->batch == NULL ||
	    fixture->pending == NULL) {
		check(false, scenario, "the scratch should be allocated");
		return (false);
	}

	relic_scratch_init(&fixture->scratch, fixture->states,
	    fixture->plan.op_count, fixture->batch, fixture->pending, arity);
	if (relic_scratch_begin_texts(&fixture->scratch,
	    &fixture->ctx->account, &failure) != 0) {
		check(false, scenario, "the text buffers should be allocated");
		return (false);
	}
	fixture->texts = true;

	fixture->env.ctx = fixture->ctx;
	relic_exec_start(&fixture->scratch, &fixture->env, &fixture->plan);

	return (true);
}

static void
fixture_close(fixture_t *fixture)
{
	if (fixture->texts)
		relic_scratch_end_texts(&fixture->scratch);
	free(fixture->states);
	free(fixture->batch);
	free(fixture->pending);
	relic_arena_reset(&fixture->arena);
	relic_destroy(fixture->ctx);
}

/*
 * Run the plan to its end, taking each batch as a caller does -- its
 * tuples, its pending values and its pending bytes all emptied -- after
 * checking every pending string it holds with `check_string`.  Returns
 * whether the plan finished without recording a failure.
 */
static bool
run_to_end(fixture_t *fixture, const char *scenario,
    string_check_t check_string, batch_log_t *log)
{
	const relic_pending_t *pending;
	relic_scratch_t *scratch;
	enum relic_exec_status status;
	size_t i;

	bzero(log, sizeof(*log));
	log->strings_hold = true;
	scratch = &fixture->scratch;

	do {
		status = relic_exec_run(scratch);
		if (scratch->batch_count > 0)
			log->batches++;
		if (scratch->batch_count > log->most_tuples)
			log->most_tuples = scratch->batch_count;
		log->tuples += scratch->batch_count;

		for (i = 0; i < scratch->pending_count; i++) {
			pending = &scratch->pending[i];
			if (pending->kind != RELIC_PENDING_TEXT)
				continue;
			if (!check_string(scratch->pending_bytes.bytes +
			    pending->u.text.offset, pending->u.text.length))
				log->strings_hold = false;
		}

		scratch->batch_count = 0;
		scratch->pending_count = 0;
		scratch->pending_bytes.used = 0;
	} while (status == RELIC_EXEC_PAUSED);

	if (scratch->error.failed) {
		check(false, scenario, "the plan should not fail");
		return (false);
	}

	return (true);
}

/*
 * Load the facts, the long string of `long_bytes` bytes when that is not
 * zero, and `rule`; run the rule's plan to its end; and give what its
 * batches held and what the two buffers had grown to by then.  Returns
 * whether the rule ran.
 */
static bool
run_rule(const char *rule, size_t long_bytes, const char *scenario,
    string_check_t check_string, batch_log_t *log, size_t *stack_capacityp,
    size_t *pending_capacityp)
{
	fixture_t fixture;
	char *program;
	bool ran;

	program = program_with(rule, long_bytes);
	if (program == NULL) {
		check(false, scenario, "the program should be written");
		return (false);
	}

	ran = fixture_open(&fixture, program, scenario) &&
	    run_to_end(&fixture, scenario, check_string, log);
	if (ran) {
		*stack_capacityp = fixture.scratch.text_stack.capacity;
		*pending_capacityp = fixture.scratch.pending_bytes.capacity;
	}

	fixture_close(&fixture);
	free(program);

	return (ran);
}

static bool
any_string(const char *bytes RELIC_TEST_UNUSED,
    size_t length RELIC_TEST_UNUSED)
{
	return (true);
}

/*
 * Whether bytes are only decimal digits, at least one.
 */
static bool
digits(const char *bytes, size_t length)
{
	size_t i;

	if (length == 0)
		return (false);

	for (i = 0; i < length; i++) {
		if (bytes[i] < '0' || bytes[i] > '9')
			return (false);
	}

	return (true);
}

/*
 * Whether a string is PREFIX followed by a number.
 */
static bool
prefix_then_digits(const char *bytes, size_t length)
{
	size_t prefix;

	prefix = sizeof(PREFIX) - 1;

	return (length > prefix && bcmp(bytes, PREFIX, prefix) == 0 &&
	    digits(bytes + prefix, length - prefix));
}

/*
 * Whether a string is a run of 'x's as long as some number of the long
 * strings the scenarios concatenate, followed by a number.
 */
static bool
long_then_digits(const char *bytes, size_t length)
{
	size_t xs;

	xs = 0;
	while (xs < length && bytes[xs] == 'x')
		xs++;

	return (xs > 0 && xs % LONG_BYTES == 0 &&
	    digits(bytes + xs, length - xs));
}

/*
 * Whether a string is the first `length` - 1 bytes of PREFIX followed by
 * one digit: what the rule of the bound-below scenario derives.
 */
static bool
cut_then_digit(const char *bytes, size_t length)
{
	return (length >= 1 && length - 1 <= sizeof(PREFIX) - 1 &&
	    bcmp(bytes, PREFIX, length - 1) == 0 &&
	    digits(bytes + length - 1, 1));
}

/*
 * A rule computing a string per binding, in an assignment that binds it,
 * in a filter, and in an assignment of its length, derives every tuple and
 * leaves the text stack as it began: each binding's strings are given back
 * when the next binding replaces them.
 */
static void
the_text_stack_is_cut_back(void)
{
	static const char *const rules[] = {
		"r(S) :- n(X), S = cat(\"" PREFIX "\", str(X)).\n",
		"r(X) :- n(X), length(cat(\"" PREFIX "\", str(X))) > 40.\n",
		"r(N) :- n(X), N = length(cat(\"" PREFIX "\", str(X))).\n",
		"r(S, T) :- n(X), S = cat(\"" PREFIX "\", str(X)), "
		    "T = cat(S, S), length(T) > 0.\n"
	};
	batch_log_t log;
	size_t stack_capacity;
	size_t pending_capacity;
	size_t i;

	for (i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
		if (!run_rule(rules[i], 0, SCENARIO_CUT_BACK,
		    i == 0 ? prefix_then_digits : any_string, &log,
		    &stack_capacity, &pending_capacity))
			continue;

		checkf(log.tuples == FACTS, SCENARIO_CUT_BACK,
		    "rule %zu should derive %d tuples, not %zu", i, FACTS,
		    log.tuples);
		checkf(log.strings_hold, SCENARIO_CUT_BACK,
		    "rule %zu should derive the strings it computes", i);
		checkf(stack_capacity == RELIC_TEXT_STACK_INITIAL,
		    SCENARIO_CUT_BACK, "rule %zu should leave the text stack "
		    "at %u bytes, not %zu", i, RELIC_TEXT_STACK_INITIAL,
		    stack_capacity);
		checkf(pending_capacity == RELIC_PENDING_BYTES,
		    SCENARIO_CUT_BACK, "rule %zu should leave the pending "
		    "bytes at %u, not %zu", i, RELIC_PENDING_BYTES,
		    pending_capacity);
	}
}

/*
 * A text slot bound by an outer assignment lies below the strings an inner
 * generator's bindings compute above it, and keeps its bytes while they are
 * replaced; as the outer binding changes, its new, longer string replaces
 * the old at the bottom of the stack.  A substring of a bound string is
 * the string's own bytes, and binding it moves nothing.
 */
static void
a_string_bound_below_keeps_its_bytes(void)
{
	static const char *const rules[] = {
		"r(T) :- n(X), X < 40, S = substr(\"" PREFIX "\", 0, X), "
		    "n(Y), Y < 10, T = cat(S, str(Y)).\n",
		"r(T) :- n(X), X < 40, W = cat(\"" PREFIX "\", \"\"), "
		    "S = substr(W, 0, X), n(Y), Y < 10, T = cat(S, str(Y)).\n"
	};
	batch_log_t log;
	size_t stack_capacity;
	size_t pending_capacity;
	size_t i;

	for (i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
		if (!run_rule(rules[i], 0, SCENARIO_BELOW, cut_then_digit,
		    &log, &stack_capacity, &pending_capacity))
			continue;

		checkf(log.tuples == 400, SCENARIO_BELOW, "rule %zu should "
		    "derive 400 tuples, not %zu", i, log.tuples);
		checkf(log.strings_hold, SCENARIO_BELOW, "rule %zu should "
		    "derive each prefix followed by a digit", i);
		checkf(stack_capacity == RELIC_TEXT_STACK_INITIAL,
		    SCENARIO_BELOW, "rule %zu should leave the text stack at "
		    "%u bytes, not %zu", i, RELIC_TEXT_STACK_INITIAL,
		    stack_capacity);
	}
}

/*
 * Tuples whose strings take a fifth of the pending bytes each fill a batch
 * at five: the sixth is not written until the batch is taken, and is then
 * written once, so every tuple arrives whole and the pending bytes never
 * grow.
 */
static void
a_batch_is_full_when_its_pending_bytes_are(void)
{
	batch_log_t log;
	size_t stack_capacity;
	size_t pending_capacity;

	if (!run_rule("r(S) :- long(L), n(X), X < 20, S = cat(L, str(X)).\n",
	    LONG_BYTES, SCENARIO_FULL, long_then_digits, &log,
	    &stack_capacity, &pending_capacity))
		return;

	checkf(log.tuples == LONG_TUPLES && log.batches == 4 &&
	    log.most_tuples == 5, SCENARIO_FULL, "twenty tuples should come "
	    "in four batches of five, not %zu in %zu of at most %zu",
	    log.tuples, log.batches, log.most_tuples);
	check(log.strings_hold, SCENARIO_FULL,
	    "every tuple should hold its whole string");
	checkf(pending_capacity == RELIC_PENDING_BYTES, SCENARIO_FULL,
	    "the pending bytes should stay at %u, not %zu",
	    RELIC_PENDING_BYTES, pending_capacity);
}

/*
 * A tuple whose strings do not fit an empty batch's pending bytes grows
 * them once, doubling them until it fits; the next such tuple
 * then fills a batch of its own without growing them again.
 */
static void
a_tuple_no_batch_holds_grows_the_bytes_once(void)
{
	batch_log_t log;
	size_t stack_capacity;
	size_t pending_capacity;
	size_t grown;

	if (!run_rule("r(S) :- long(L), n(X), X < 3, "
	    "S = cat(cat(L, L), str(X)).\n", LONG_BYTES * 3, SCENARIO_OVERSIZED,
	    long_then_digits, &log, &stack_capacity, &pending_capacity))
		return;

	grown = relic_grow_capacity_doubling(RELIC_PENDING_BYTES,
	    6 * LONG_BYTES + 1);
	check(log.tuples == 3 && log.batches == 3, SCENARIO_OVERSIZED,
	    "each tuple should fill a batch of its own");
	checkf(pending_capacity == grown, SCENARIO_OVERSIZED,
	    "the pending bytes should grow once, to %zu, not to %zu", grown,
	    pending_capacity);
}

/*
 * A text stack the memory limit will not let grow ends the plan with the
 * failure recorded as the limit's, with no tuple derived from the binding
 * that needed it.
 */
static void
a_text_stack_the_limit_refuses_fails_the_plan(void)
{
	fixture_t fixture;
	relic_scratch_t *scratch;
	char *program;

	program = program_with("r(S) :- long(L), n(X), X < 3, "
	    "S = cat(L, str(X)).\n", 2 * RELIC_TEXT_STACK_INITIAL);
	if (program == NULL) {
		check(false, SCENARIO_REFUSED, "the program should be written");
		return;
	}

	if (fixture_open(&fixture, program, SCENARIO_REFUSED)) {
		scratch = &fixture.scratch;
		fixture.ctx->account.limit = 1;

		(void)relic_exec_run(scratch);
		check(scratch->finished && scratch->batch_count == 0 &&
		    scratch->error.failed &&
		    scratch->error.failure == RELIC_FAIL_LIMIT,
		    SCENARIO_REFUSED, "the plan should end on the limit "
		    "having derived nothing");
		checkf(scratch->text_stack.capacity == RELIC_TEXT_STACK_INITIAL,
		    SCENARIO_REFUSED, "the text stack should stay at %u "
		    "bytes, not %zu", RELIC_TEXT_STACK_INITIAL,
		    scratch->text_stack.capacity);
		bzero(&scratch->error, sizeof(scratch->error));
	}

	fixture_close(&fixture);
	free(program);
}

/*
 * What the rows of the resolved scenario's query held: how many there
 * were, and whether each held a long string ending in a digit below seven
 * beside the integer its digit was computed with.
 */
typedef struct resolved_rows resolved_rows_t;

struct resolved_rows {
	relic_t	*ctx;
	size_t	 rows;
	bool	 hold;
};

static int
check_row(void *callback_ctx, const relic_value_t *values, size_t count)
{
	resolved_rows_t *rows;
	const char *bytes;
	size_t length;
	int64_t digit;

	rows = callback_ctx;
	rows->rows++;

	bytes = NULL;
	length = 0;
	if (count == 2 && values[0].type == RELIC_STRING)
		bytes = relic_string(rows->ctx, values[0].v.id, &length);
	if (bytes == NULL || values[1].type != RELIC_INTEGER ||
	    length != LONG_BYTES + 1 || !long_then_digits(bytes, length)) {
		rows->hold = false;
		return (0);
	}

	digit = bytes[LONG_BYTES] - '0';
	if (values[1].v.integer % 1000003 != 0 ||
	    (values[1].v.integer / 1000003) % 7 != digit)
		rows->hold = false;

	return (0);
}

/*
 * An evaluation whose batches fill by their pending bytes resolves each
 * whole before the next: its seven strings, which repeat from one batch to
 * another, and its twenty integers, of which 0 and 1000003 the program
 * states, are interned by the batch that first holds them and found by
 * every later one, and every tuple is inserted once, holding exactly what
 * the rule computed.  The program is checked before it is counted, since
 * the check is what interns its rule's constants.
 */
static void
an_evaluation_resolves_every_batch_whole(void)
{
	resolved_rows_t rows;
	relic_query_t *query;
	relic_stats_t before;
	relic_stats_t after;
	relic_t *ctx;
	char *program;
	int status;

	program = program_with("r(S, Y) :- long(L), n(X), X < 20, "
	    "S = cat(L, str(X mod 7)), Y = X * 1000003.\n", LONG_BYTES);
	ctx = relic_create();
	if (program == NULL || ctx == NULL ||
	    relic_load_string(ctx, "resolved.dl", program) != 0 ||
	    relic_check(ctx) != 0) {
		check(false, SCENARIO_RESOLVED, "the program should load");
		relic_destroy(ctx);
		free(program);
		return;
	}

	relic_stats(ctx, &before, sizeof(before));
	status = relic_materialize(ctx);
	relic_stats(ctx, &after, sizeof(after));
	check(status == 0, SCENARIO_RESOLVED, "the materialization succeeds");
	checkf(after.string_values == before.string_values + 7 &&
	    after.integer_values == before.integer_values + 18,
	    SCENARIO_RESOLVED, "seven strings and eighteen integers should be "
	    "new, not %zu and %zu", after.string_values - before.string_values,
	    after.integer_values - before.integer_values);

	bzero(&rows, sizeof(rows));
	rows.ctx = ctx;
	rows.hold = true;
	query = relic_query_prepare(ctx, NULL, "r(S, Y)?");
	status = query != NULL ? relic_query_run(ctx, query, check_row,
	    &rows) : -1;
	checkf(status == 0 && rows.rows == 20 && rows.hold, SCENARIO_RESOLVED,
	    "twenty rows should each hold what the rule computed, not %zu",
	    rows.rows);

	relic_query_destroy(query);
	relic_destroy(ctx);
	free(program);
}

/*
 * The pattern the first match step of the plan's filters carries.
 */
static const relic_pattern_t *
planned_pattern(const relic_plan_t *plan)
{
	const relic_postfix_t *postfix;
	size_t i;
	size_t j;

	for (i = 0; i < plan->op_count; i++) {
		if (plan->ops[i].kind != RELIC_OP_FILTER)
			continue;
		postfix = &plan->ops[i].u.filter.left;
		for (j = 0; j < postfix->step_count; j++) {
			if (postfix->steps[j].kind == RELIC_STEP_CALL &&
			    postfix->steps[j].u.call.fn == RELIC_FN_MATCH)
				return (postfix->steps[j].u.call.pattern);
		}
	}

	return (NULL);
}

static size_t
round_bytes(const relic_t *ctx)
{
	return (relic_atomic_size_load(&ctx->account.round_bytes));
}

static int
count_rows(void *callback_ctx, const relic_value_t *values RELIC_TEST_UNUSED,
    size_t count RELIC_TEST_UNUSED)
{
	size_t *rowsp;

	rowsp = callback_ctx;
	(*rowsp)++;

	return (0);
}

/*
 * A match whose pattern is a constant runs the program its rule compiled,
 * which the plan's step shares rather than copies: every binding matches
 * against it, the match space's program room is never needed, and only
 * the work space grows, once.
 */
static void
a_constant_pattern_runs_as_its_rule_compiled_it(void)
{
	const relic_rule_t *rule;
	fixture_t fixture;
	batch_log_t log;
	char *program;

	program = program_with("r(X) :- n(X), match(str(X), \"7$\") >= 0.\n",
	    0);
	if (program == NULL) {
		check(false, SCENARIO_CONSTANT, "the program should be written");
		return;
	}

	if (fixture_open(&fixture, program, SCENARIO_CONSTANT) &&
	    run_to_end(&fixture, SCENARIO_CONSTANT, any_string, &log)) {
		rule = &fixture.ctx->program.rules.entries[
		    fixture.ctx->program.rules.count - 1];
		check(planned_pattern(&fixture.plan) != NULL &&
		    planned_pattern(&fixture.plan) ==
		    rule->body[1].u.comparison.left->u.call.pattern,
		    SCENARIO_CONSTANT, "the step shares the rule's program");
		checkf(log.tuples == FACTS / 10, SCENARIO_CONSTANT,
		    "%d numbers end in 7, not %zu", FACTS / 10, log.tuples);
		check(planned_pattern(&fixture.plan) != NULL &&
		    fixture.scratch.match.capacity == 0 &&
		    fixture.scratch.match.work.capacity ==
		    relic_grow_capacity_doubling(0,
		    planned_pattern(&fixture.plan)->count), SCENARIO_CONSTANT,
		    "only the work space grows, once");
	}

	fixture_close(&fixture);
	free(program);
}

/*
 * A pattern computed from each binding is compiled for that binding into
 * the match space, and matched as that binding's pattern: a start anchor
 * followed by the binding's last digit finds exactly the numbers that
 * begin with the digit they end with.  The match space is the round's,
 * charged beside the text buffers while the plan runs and given back with
 * them.
 */
static void
a_variable_pattern_is_compiled_per_binding(void)
{
	fixture_t fixture;
	batch_log_t log;
	char *program;
	size_t charged;
	size_t buffers;

	program = program_with("r(X) :- n(X), "
	    "match(str(X), cat(\"^\", str(X mod 10))) = 0.\n", 0);
	if (program == NULL) {
		check(false, SCENARIO_PER_BINDING,
		    "the program should be written");
		return;
	}

	if (fixture_open(&fixture, program, SCENARIO_PER_BINDING) &&
	    run_to_end(&fixture, SCENARIO_PER_BINDING, any_string, &log)) {
		check(planned_pattern(&fixture.plan) == NULL,
		    SCENARIO_PER_BINDING, "the step carries no program");
		checkf(log.tuples == SAME_ENDS, SCENARIO_PER_BINDING,
		    "%d numbers begin with their last digit, not %zu",
		    SAME_ENDS, log.tuples);
		check(fixture.scratch.match.capacity > 0 &&
		    fixture.scratch.match.work.capacity > 0,
		    SCENARIO_PER_BINDING, "the match space grows");

		buffers = fixture.scratch.text_stack.capacity +
		    fixture.scratch.pending_bytes.capacity +
		    fixture.scratch.match.capacity *
		    sizeof(relic_regex_inst_t) +
		    fixture.scratch.match.work.capacity *
		    RELIC_REGEX_WORK_WORDS * sizeof(uint32_t);
		charged = round_bytes(fixture.ctx);
		checkf(charged == buffers, SCENARIO_PER_BINDING, "the round "
		    "should be charged its %zu bytes of buffers, not %zu",
		    buffers, charged);

		relic_scratch_end_texts(&fixture.scratch);
		fixture.texts = false;
		checkf(round_bytes(fixture.ctx) == 0, SCENARIO_PER_BINDING,
		    "ending the round should give back every byte, not "
		    "leave %zu", round_bytes(fixture.ctx));
	}

	fixture_close(&fixture);
	free(program);
}

/*
 * A pattern that is not a regular expression ends the plan on the binding
 * that supplies it, with the failure and its reason recorded at the
 * literal: every binding before it has matched, and none after it runs.
 * Through an evaluation, the failure is reported naming the rule and the
 * reason, and fails every query that needs the rule, with no rows.
 */
static void
an_invalid_pattern_fails_on_its_binding(void)
{
	static const char rule[] = "r(X) :- n(X), "
	    "match(\"xa\", cat(\"a\", substr(\")\", 0, X - 500))) >= 0.\n";
	relic_scratch_t *scratch;
	enum relic_exec_status status;
	relic_query_t *query;
	const char *reported;
	fixture_t fixture;
	size_t tuples;
	size_t rows;
	char *program;
	relic_t *ctx;

	program = program_with(rule, 0);
	if (program == NULL) {
		check(false, SCENARIO_INVALID, "the program should be written");
		return;
	}

	if (fixture_open(&fixture, program, SCENARIO_INVALID)) {
		scratch = &fixture.scratch;
		tuples = 0;
		do {
			status = relic_exec_run(scratch);
			tuples += scratch->batch_count;
			scratch->batch_count = 0;
			scratch->pending_count = 0;
			scratch->pending_bytes.used = 0;
		} while (status == RELIC_EXEC_PAUSED);

		check(scratch->error.failed &&
		    scratch->error.failure == RELIC_FAIL_PATTERN &&
		    scratch->error.pattern == RELIC_RE_UNMATCHED_CLOSE &&
		    scratch->error.where != NULL, SCENARIO_INVALID,
		    "the plan should fail on the unmatched ) at the literal");
		checkf(tuples == INVALID_AT, SCENARIO_INVALID, "the %d "
		    "bindings before it should match, not %zu", INVALID_AT,
		    tuples);
		bzero(&scratch->error, sizeof(scratch->error));
	}
	fixture_close(&fixture);

	ctx = relic_create();
	if (ctx == NULL || relic_load_string(ctx, "invalid.dl", program) != 0) {
		check(false, SCENARIO_INVALID, "the program should load");
		relic_destroy(ctx);
		free(program);
		return;
	}
	check(relic_materialize(ctx) != 0, SCENARIO_INVALID,
	    "the evaluation should fail");
	reported = relic_error(ctx);
	check(reported != NULL && strstr(reported, "invalid regular "
	    "expression in rule r/1: unmatched )") != NULL, SCENARIO_INVALID,
	    "the error should name the rule and the reason");

	rows = 0;
	query = relic_query_prepare(ctx, NULL, "r(X)?");
	check(query != NULL && relic_query_run(ctx, query, count_rows,
	    &rows) != 0 && rows == 0, SCENARIO_INVALID,
	    "a query of the rule should fail with no rows");

	relic_query_destroy(query);
	relic_destroy(ctx);
	free(program);
}

int
main(void)
{
	the_text_stack_is_cut_back();
	a_string_bound_below_keeps_its_bytes();
	a_batch_is_full_when_its_pending_bytes_are();
	a_tuple_no_batch_holds_grows_the_bytes_once();
	a_text_stack_the_limit_refuses_fails_the_plan();
	an_evaluation_resolves_every_batch_whole();
	a_constant_pattern_runs_as_its_rule_compiled_it();
	a_variable_pattern_is_compiled_per_binding();
	an_invalid_pattern_fails_on_its_binding();

	return (check_summary("text"));
}
