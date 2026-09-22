/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdint.h>
#include <strings.h>

#include "internal.h"

/*
 * Evaluation: running plans, reaching the program's fixed point, and
 * answering queries.
 *
 * The executor runs one plan, an array of operations ending in EMIT.  The
 * join a plan describes is a nest of loops, one per generator, and it is run
 * with one loop and an explicit level rather than a C call per level, so a
 * rule of any length uses the same stack as a rule of one literal.
 *
 * Descending means the operation at the current level produced a binding and
 * the next one should start afresh against it.  Backtracking means an
 * operation has nothing more, so the one before it is asked for its next
 * binding.  EMIT inserts the head tuple and then reports that there is
 * nothing below it, so the level never passes the end of the plan and the
 * loop needs no case for having got there.
 *
 * The naive fixed point runs every rule's plan until a full pass inserts no
 * new tuple.  It terminates because every value a rule can produce was
 * interned before evaluation began -- a head holds constants and variables,
 * and a variable is bound only from a tuple -- so each relation can hold only
 * finitely many tuples.  It is the least fixed point because every tuple it
 * inserts is justified by tuples already present.  A fact is a rule whose plan
 * is a lone EMIT, so the first pass loads the facts and nothing else has to.
 */

typedef struct relic_evaluation relic_evaluation_t;

/*
 * One evaluation: the plans it runs and the executor's state.
 *
 * `frame` holds one value per slot.  A slot is written by the operation that
 * binds it and read only by operations after that one, so a new binding
 * simply overwrites the old: nothing is saved on descending and nothing
 * restored on backtracking, because no operation reads a slot a later one
 * binds.
 *
 * `cursors` holds one range per operation of the longest plan, and is what a
 * SCAN resumes from.  Tests keep no state.  Both it and the plans live in the
 * evaluation arena, allocated once when the evaluation begins, so nothing is
 * allocated while a plan runs.
 *
 * `derived` is set whenever an EMIT inserts a tuple the relation did not
 * already hold, which is how a pass learns that the fixed point has not been
 * reached.
 */
struct relic_evaluation {
	relic_t			*ctx;
	relic_plan_t		*plans;
	relic_tuple_range_t	*cursors;
	size_t			 plan_count;
	relic_dom_t		 frame[RELIC_MAX_VARIABLES];
	bool			 derived;
};

static void	*evaluation_alloc(relic_evaluation_t *eval, size_t count,
		    size_t size);
static int	 evaluation_begin(relic_evaluation_t *eval, relic_t *ctx,
		    const relic_rule_t *rules, size_t count);
static void	 evaluation_end(relic_evaluation_t *eval);
static void	 build_tuple(const relic_evaluation_t *eval,
		    const relic_op_atom_t *atom, relic_dom_t *tuple);
static bool	 comparison_holds(const relic_t *ctx,
		    enum relic_compare_op compare, relic_dom_t left,
		    relic_dom_t right);
static int	 execute_plan(relic_evaluation_t *eval,
		    const relic_plan_t *plan);
static int	 materialize_program(relic_t *ctx);
static int	 naive_fixed_point(relic_evaluation_t *eval);
static int	 run_every_plan(relic_evaluation_t *eval);
static int	 step(relic_evaluation_t *eval, const relic_op_t *op,
		    relic_tuple_range_t *cursor, bool resume, bool *matchedp);
static int	 step_emit(relic_evaluation_t *eval,
		    const relic_op_atom_t *atom);
static bool	 step_filter(const relic_evaluation_t *eval,
		    const relic_op_filter_t *filter);
static bool	 step_member(const relic_evaluation_t *eval,
		    const relic_op_atom_t *atom);
static bool	 step_scan(relic_evaluation_t *eval,
		    const relic_op_atom_t *atom, relic_tuple_range_t *cursor,
		    bool resume);
static void	 stream_answer(relic_t *ctx, const relic_relation_t *answer,
		    relic_row_cb callback, void *callback_ctx);
static relic_dom_t term_value(const relic_evaluation_t *eval,
		    const relic_plan_term_t *term);
static bool	 tuple_matches(relic_evaluation_t *eval,
		    const relic_op_atom_t *atom, const relic_dom_t *tuple);

/*
 * The value a reading term stands for: its constant, or what its slot holds.
 */
static relic_dom_t
term_value(const relic_evaluation_t *eval, const relic_plan_term_t *term)
{
	if (term->action == RELIC_PLAN_CONSTANT)
		return (term->u.constant);

	return (eval->frame[term->u.slot]);
}

/*
 * Whether a tuple matches a generator's terms, binding the slots it binds.
 *
 * Columns are visited left to right, so a slot bound by one column is in the
 * frame when a later column of the same tuple reads it; that is how edge(X,
 * X) finds only loops.  A tuple that fails part way may leave slots it bound
 * in the frame.  Nothing reads them: the operations after this one run only
 * after a match, and the next match overwrites them first.
 */
static bool
tuple_matches(relic_evaluation_t *eval, const relic_op_atom_t *atom,
    const relic_dom_t *tuple)
{
	const relic_plan_term_t *term;
	uint16_t column;

	for (column = 0; column < atom->relation->arity; column++) {
		term = &atom->terms[column];

		switch (term->action) {
		case RELIC_PLAN_CONSTANT:
		case RELIC_PLAN_BOUND:
			if (tuple[column] != term_value(eval, term))
				return (false);
			break;
		case RELIC_PLAN_BIND:
			eval->frame[term->u.slot] = tuple[column];
			break;
		case RELIC_PLAN_ANY:
			break;
		}
	}

	return (true);
}

/*
 * The tuple a test or an EMIT reads: every term's value, in column order.
 * `tuple` has room for the largest arity there is.
 */
static void
build_tuple(const relic_evaluation_t *eval, const relic_op_atom_t *atom,
    relic_dom_t *tuple)
{
	uint16_t column;

	for (column = 0; column < atom->relation->arity; column++)
		tuple[column] = term_value(eval, &atom->terms[column]);
}

/*
 * Whether a comparison holds between two values.
 *
 * Equality is identity of domain identifiers, which is exact because every
 * value is interned: equal values have equal identifiers and nothing else
 * does.  Ordering goes through the value table, where values of different
 * types are unordered, and an unordered pair satisfies no ordering
 * comparison.  The binding is filtered out rather than reported as an error,
 * because a column may legitimately hold more than one type.
 */
static bool
comparison_holds(const relic_t *ctx, enum relic_compare_op compare,
    relic_dom_t left, relic_dom_t right)
{
	enum relic_order order;

	if (compare == RELIC_CMP_EQ)
		return (left == right);
	if (compare == RELIC_CMP_NE)
		return (left != right);

	order = relic_value_order(ctx, left, right);

	switch (compare) {
	case RELIC_CMP_LT:
		return (order == RELIC_ORDER_LESS);
	case RELIC_CMP_LE:
		return (order == RELIC_ORDER_LESS ||
		    order == RELIC_ORDER_EQUAL);
	case RELIC_CMP_GT:
		return (order == RELIC_ORDER_GREATER);
	case RELIC_CMP_GE:
		return (order == RELIC_ORDER_GREATER ||
		    order == RELIC_ORDER_EQUAL);
	case RELIC_CMP_EQ:
	case RELIC_CMP_NE:
		break;
	}

	return (false);
}

/*
 * A SCAN: find the next tuple of the relation that matches, starting from
 * the beginning when not resuming.
 *
 * The range is taken when the scan starts, so tuples inserted into the
 * relation while it runs -- by an EMIT below it into the same relation, say --
 * lie past its end and are left to the next scan that starts.  The tuple is
 * fetched afresh by identifier each time rather than held, because that same
 * insertion may have moved the storage a held pointer would point into.
 */
static bool
step_scan(relic_evaluation_t *eval, const relic_op_atom_t *atom,
    relic_tuple_range_t *cursor, bool resume)
{
	const relic_dom_t *tuple;
	relic_tuple_id_t id;

	if (!resume)
		*cursor = relic_relation_all(atom->relation);

	while (cursor->first < cursor->end) {
		id = cursor->first;
		cursor->first++;

		tuple = relic_relation_tuple(atom->relation, id);
		if (tuple_matches(eval, atom, tuple))
			return (true);
	}

	return (false);
}

static bool
step_member(const relic_evaluation_t *eval, const relic_op_atom_t *atom)
{
	relic_dom_t tuple[RELIC_MAX_ARITY];

	build_tuple(eval, atom, tuple);

	return (relic_relation_contains(atom->relation, tuple));
}

static bool
step_filter(const relic_evaluation_t *eval, const relic_op_filter_t *filter)
{
	return (comparison_holds(eval->ctx, filter->compare,
	    term_value(eval, &filter->left),
	    term_value(eval, &filter->right)));
}

/*
 * An EMIT: insert the head tuple, noting whether it was new.  Returns -1 when
 * the insertion was refused, which the relation store has reported.
 */
static int
step_emit(relic_evaluation_t *eval, const relic_op_atom_t *atom)
{
	relic_dom_t tuple[RELIC_MAX_ARITY];
	bool added;

	build_tuple(eval, atom, tuple);

	if (relic_relation_insert(eval->ctx, atom->relation, tuple,
	    &added) != 0)
		return (-1);

	if (added)
		eval->derived = true;

	return (0);
}

/*
 * Ask one operation for a match: a fresh one, or the next one when resuming.
 * Returns -1 when an insertion failed, and otherwise 0 with *matchedp saying
 * whether a binding was produced.
 *
 * A test succeeds at most once, so resuming one always finds nothing more.
 * An EMIT is never resumed: it never matches, so nothing below it is ever
 * entered and nothing backtracks into it.
 */
static int
step(relic_evaluation_t *eval, const relic_op_t *op,
    relic_tuple_range_t *cursor, bool resume, bool *matchedp)
{
	*matchedp = false;

	switch (op->kind) {
	case RELIC_OP_SCAN:
		*matchedp = step_scan(eval, &op->u.atom, cursor, resume);
		break;
	case RELIC_OP_MEMBER:
		if (!resume)
			*matchedp = step_member(eval, &op->u.atom);
		break;
	case RELIC_OP_FILTER:
		if (!resume)
			*matchedp = step_filter(eval, &op->u.filter);
		break;
	case RELIC_OP_EMIT:
		return (step_emit(eval, &op->u.atom));
	}

	return (0);
}

/*
 * Run one plan: every binding its join produces reaches the EMIT.
 *
 * `level` is the operation being asked for a match, and `resume` says whether
 * it is being asked for its next one or started afresh.  The loop ends when
 * the first operation has nothing more, which is when every binding has been
 * tried.
 */
static int
execute_plan(relic_evaluation_t *eval, const relic_plan_t *plan)
{
	size_t level;
	bool matched;
	bool resume;

	level = 0;
	resume = false;

	for (;;) {
		if (step(eval, &plan->ops[level], &eval->cursors[level], resume,
		    &matched) != 0)
			return (-1);

		if (matched) {
			level++;
			resume = false;
			continue;
		}

		if (level == 0)
			return (0);

		level--;
		resume = true;
	}
}

/*
 * One pass: every plan, in program order.
 */
static int
run_every_plan(relic_evaluation_t *eval)
{
	size_t i;

	for (i = 0; i < eval->plan_count; i++) {
		if (execute_plan(eval, &eval->plans[i]) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Run passes until one inserts nothing new.
 *
 * Within a pass a rule sees what the rules before it inserted, and what its
 * own earlier matches inserted, rather than only what was there when the pass
 * began.  That changes how many passes are needed and not where they end,
 * and it is deterministic, because rules run in program order and relations
 * iterate in insertion order.
 */
static int
naive_fixed_point(relic_evaluation_t *eval)
{
	do {
		eval->derived = false;

		if (run_every_plan(eval) != 0)
			return (-1);
	} while (eval->derived);

	return (0);
}

/*
 * Allocate from the evaluation arena, reporting exhaustion.  The product is
 * checked rather than assumed, like every other count-times-size in the
 * library.
 */
static void *
evaluation_alloc(relic_evaluation_t *eval, size_t count, size_t size)
{
	void *block;

	block = NULL;
	if (count <= SIZE_MAX / size)
		block = relic_arena_alloc(&eval->ctx->eval_arena, count * size);

	if (block == NULL)
		relic_diag_no_memory(eval->ctx);

	return (block);
}

/*
 * Plan every rule and size the executor's state for the longest plan.
 * Returns 0, or -1 with a diagnostic reported.  Either way the caller ends
 * the evaluation with evaluation_end(), which releases what was allocated.
 */
static int
evaluation_begin(relic_evaluation_t *eval, relic_t *ctx,
    const relic_rule_t *rules, size_t count)
{
	size_t longest;
	size_t i;

	bzero(eval, sizeof(*eval));
	eval->ctx = ctx;

	if (count == 0)
		return (0);

	eval->plans = evaluation_alloc(eval, count, sizeof(*eval->plans));
	if (eval->plans == NULL)
		return (-1);

	longest = 0;
	for (i = 0; i < count; i++) {
		if (relic_plan_rule(ctx, &ctx->eval_arena, &rules[i],
		    &eval->plans[i]) != 0)
			return (-1);

		if (eval->plans[i].op_count > longest)
			longest = eval->plans[i].op_count;
	}

	eval->cursors = evaluation_alloc(eval, longest,
	    sizeof(*eval->cursors));
	if (eval->cursors == NULL)
		return (-1);

	eval->plan_count = count;

	return (0);
}

/*
 * Release the plans and cursors.  Resetting the arena also clears a failure
 * it recorded, which this evaluation has already reported.
 */
static void
evaluation_end(relic_evaluation_t *eval)
{
	relic_arena_reset(&eval->ctx->eval_arena);
	bzero(eval, sizeof(*eval));
}

/*
 * Bring the relations to the fixed point of every rule of the program.
 *
 * The relations are already a fixed point of the first `materialized_rules`
 * rules, and rules are only ever appended, so nothing needs doing when no
 * rule has been added since.  When some have, the fixed point is run over all
 * of them, continuing from the tuples already held.  For a program without
 * negation that is sound: every tuple held is still justified, and the least
 * fixed point reached from them is the one reached from nothing.
 */
static int
materialize_program(relic_t *ctx)
{
	const relic_ruletab_t *rules;
	relic_evaluation_t eval;
	int status;

	rules = &ctx->program.rules;
	if (ctx->materialized_rules == rules->count)
		return (0);

	status = evaluation_begin(&eval, ctx, rules->entries, rules->count);
	if (status == 0)
		status = naive_fixed_point(&eval);
	evaluation_end(&eval);

	if (status != 0)
		return (-1);

	ctx->materialized_rules = rules->count;

	return (0);
}

int
relic_materialize(relic_t *ctx)
{
	if (relic_program_ready(ctx) != 0)
		return (-1);

	return (materialize_program(ctx));
}

/*
 * Pass every tuple of the answer relation to the callback as a row of
 * values, in insertion order, until the callback asks to stop.
 *
 * Nothing here can fail, which is what lets a query promise that no row is
 * delivered unless every row can be.  A value is found for every column
 * because every column holds an identifier the value table assigned: columns
 * are copied only from constants the parser interned and from other tuples.
 *
 * The tuple is fetched afresh for each row rather than held across the call
 * to the callback, so that no pointer into relation storage outlives a call
 * out of the library.
 */
static void
stream_answer(relic_t *ctx, const relic_relation_t *answer,
    relic_row_cb callback, void *callback_ctx)
{
	relic_value_t row[RELIC_MAX_ARITY];
	const relic_dom_t *tuple;
	relic_tuple_range_t range;
	relic_tuple_id_t id;
	uint16_t column;

	range = relic_relation_all(answer);

	for (id = range.first; id < range.end; id++) {
		tuple = relic_relation_tuple(answer, id);

		for (column = 0; column < answer->arity; column++)
			row[column] = *relic_value_of(ctx, tuple[column]);

		if (callback(callback_ctx, row, answer->arity) != 0)
			return;
	}
}

/*
 * Answer a query: bring the program to its fixed point, run the query's rule
 * once into its answer relation, and stream that relation.
 *
 * One pass computes the answer completely, because the answer predicate
 * appears in no body: its name holds `$`, which nothing a program writes can.
 * Duplicate answers collapse as they are inserted, which is what gives a
 * query set semantics.  Every step that can fail comes before the first row
 * is delivered, so a query either delivers its whole answer or none of it.
 */
int
relic_query_run(relic_t *ctx, relic_query_t *query, relic_row_cb callback,
    void *callback_ctx)
{
	relic_relation_t *answer;
	relic_evaluation_t eval;
	int status;

	if (query == NULL || callback == NULL) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "a query and a row callback must both be given");
		return (-1);
	}

	if (query->ctx != ctx) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "the query belongs to a different context");
		return (-1);
	}

	if (relic_materialize(ctx) != 0)
		return (-1);

	status = evaluation_begin(&eval, ctx, &query->rule, 1);
	if (status == 0)
		status = run_every_plan(&eval);
	evaluation_end(&eval);

	if (status != 0)
		return (-1);

	answer = relic_relation_for(ctx, query->rule.head);
	if (answer == NULL)
		return (-1);

	stream_answer(ctx, answer, callback, callback_ctx);

	return (0);
}
