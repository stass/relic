/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * The executor sees the store only as frozen.h presents it, which is what
 * keeps it from writing what it reads.  The headers that define a context, a
 * relation and an index refuse to be included where this is defined,
 * wherever in the file the include is.
 */
#define	RELIC_FROZEN_ONLY

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <strings.h>

#include "exec.h"

/*
 * The executor: running one plan, an array of operations ending in EMIT.
 *
 * The join a plan describes is a nest of loops, one per generator, and it is
 * run with one loop and an explicit level rather than a C call per level, so
 * a rule of any length uses the same stack as a rule of one literal.
 *
 * Descending means the operation at the current level produced a binding and
 * the next one should start afresh against it.  Backtracking means an
 * operation has nothing more, so the one before it is asked for its next
 * binding.  EMIT adds the head tuple to the scratch's batch and then reports
 * that there is nothing below it, so the level never passes the end of the
 * plan and the loop needs no case for having got there.
 *
 * When the EMIT fills the batch the loop stops and returns, recording in the
 * scratch the level and the resume flag it would have gone on with, so that
 * the next call continues the join exactly where it left off.  Everything
 * the join depends on -- the frame, the cursors, the views -- is in the
 * scratch or in relations nobody writes while the plan runs, so stopping
 * and continuing changes nothing it derives.
 *
 * The context and every relation are read-only here, as frozen.h makes
 * them, and only the scratch is written.  The views of a plan are resolved
 * when it starts, and no view reaches past the end its relation had then,
 * so whatever the caller does with a full batch before continuing -- insert
 * it into the relation the plan reads, even -- lies past every view and
 * changes nothing the plan reads.
 */

static void	 build_tuple(const relic_scratch_t *scratch,
		    const relic_op_atom_t *atom, relic_dom_t *tuple);
static bool	 comparison_holds(const relic_t *ctx,
		    enum relic_compare_op compare, relic_dom_t left,
		    relic_dom_t right);
static bool	 comparison_is_mixed(const relic_t *ctx,
		    enum relic_compare_op compare, relic_dom_t left,
		    relic_dom_t right);
static void	 emit_head(relic_scratch_t *scratch,
		    const relic_op_atom_t *atom);
static void	 resolve_views(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_plan_t *plan);
static bool	 step(relic_scratch_t *scratch, const relic_exec_env_t *env,
		    const relic_op_t *op, relic_op_state_t *state, bool resume);
static bool	 step_filter(relic_scratch_t *scratch,
		    const relic_exec_env_t *env,
		    const relic_op_filter_t *filter);
static bool	 step_lookup(relic_scratch_t *scratch,
		    const relic_op_atom_t *atom, relic_op_state_t *state,
		    bool resume);
static bool	 step_member(const relic_scratch_t *scratch,
		    const relic_op_atom_t *atom,
		    const relic_tuple_range_t *view);
static bool	 step_scan(relic_scratch_t *scratch,
		    const relic_op_atom_t *atom, relic_op_state_t *state,
		    bool resume);
static relic_dom_t term_value(const relic_scratch_t *scratch,
		    const relic_plan_term_t *term);
static bool	 tuple_matches(relic_scratch_t *scratch,
		    const relic_op_atom_t *atom, const relic_dom_t *tuple);
static enum relic_exec_status yield_after_emit(relic_scratch_t *scratch,
		    size_t level);

void
relic_scratch_init(relic_scratch_t *scratch, relic_op_state_t *states,
    size_t op_room, relic_dom_t *batch, uint16_t arity_room)
{
	bzero(scratch, sizeof(*scratch));
	scratch->states = states;
	scratch->op_room = op_room;
	scratch->batch = batch;
	scratch->arity_room = arity_room;
	scratch->finished = true;
}

/*
 * The value a reading term stands for: its constant, or what its slot holds.
 */
static relic_dom_t
term_value(const relic_scratch_t *scratch, const relic_plan_term_t *term)
{
	if (term->action == RELIC_PLAN_CONSTANT)
		return (term->u.constant);

	return (scratch->frame[term->u.slot]);
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
tuple_matches(relic_scratch_t *scratch, const relic_op_atom_t *atom,
    const relic_dom_t *tuple)
{
	const relic_plan_term_t *term;
	uint16_t column;

	for (column = 0; column < atom->arity; column++) {
		term = &atom->terms[column];

		switch (term->action) {
		case RELIC_PLAN_CONSTANT:
		case RELIC_PLAN_BOUND:
			if (tuple[column] != term_value(scratch, term))
				return (false);
			break;
		case RELIC_PLAN_BIND:
			scratch->frame[term->u.slot] = tuple[column];
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
build_tuple(const relic_scratch_t *scratch, const relic_op_atom_t *atom,
    relic_dom_t *tuple)
{
	uint16_t column;

	for (column = 0; column < atom->arity; column++)
		tuple[column] = term_value(scratch, &atom->terms[column]);
}

/*
 * Whether a comparison holds between two values.
 *
 * Equality is identity of domain identifiers, which is exact because every
 * value is interned: equal values have equal identifiers and nothing else
 * does.  Ordering decodes both identifiers and compares the values, and
 * values of different types are unordered, which satisfies no ordering
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
 * The identifiers an operation's view covers now: the whole relation, or a
 * range derived from the delta of the member being read, as plan.h sets out.
 */
relic_tuple_range_t
relic_exec_view(const relic_exec_env_t *env, const relic_op_atom_t *atom)
{
	relic_tuple_range_t delta;
	relic_tuple_range_t range;

	if (atom->view == RELIC_VIEW_ALL)
		return (relic_relation_all(atom->relation));

	assert(env->deltas != NULL);
	delta = env->deltas[relic_relation_pred(atom->relation) - 1];
	range.first = RELIC_TUPLE_FIRST;
	range.end = delta.end;

	switch (atom->view) {
	case RELIC_VIEW_OLD:
		range.end = delta.first;
		break;
	case RELIC_VIEW_DELTA:
		range.first = delta.first;
		break;
	case RELIC_VIEW_ALL:
	case RELIC_VIEW_CURRENT:
		break;
	}

	return (range);
}

/*
 * Resolve the view of every operation of `plan` that reads a relation,
 * before any of them runs.  Nothing has been inserted yet, so every range
 * ends at or before its relation's end as the plan starts, and the tuples
 * the plan derives, whenever its batches are inserted, lie past it: what a
 * plan reads cannot depend on when its derivations are written.
 */
static void
resolve_views(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_plan_t *plan)
{
	const relic_op_t *op;
	size_t level;

	for (level = 0; level < plan->op_count; level++) {
		op = &plan->ops[level];
		switch (op->kind) {
		case RELIC_OP_SCAN:
		case RELIC_OP_LOOKUP:
		case RELIC_OP_MEMBER:
		case RELIC_OP_ANTI_MEMBER:
			scratch->states[level].view = relic_exec_view(env,
			    &op->u.atom);
			break;
		case RELIC_OP_FILTER:
		case RELIC_OP_EMIT:
			break;
		}
	}
}

/*
 * A SCAN: find the next tuple of its view that matches, starting from the
 * beginning of the view when not resuming.  The tuple is fetched afresh by
 * identifier each time rather than held, because the caller may insert into
 * the relation, and move its storage, between two calls.
 */
static bool
step_scan(relic_scratch_t *scratch, const relic_op_atom_t *atom,
    relic_op_state_t *state, bool resume)
{
	relic_tuple_range_t *cursor;
	const relic_dom_t *tuple;
	relic_tuple_id_t id;

	cursor = &state->cursor;
	if (!resume)
		*cursor = state->view;

	while (cursor->first < cursor->end) {
		id = cursor->first;
		cursor->first++;
		scratch->counters.tuples_scanned++;

		tuple = relic_relation_tuple(atom->relation, id);
		if (tuple_matches(scratch, atom, tuple))
			return (true);
	}

	return (false);
}

/*
 * A LOOKUP: find the next tuple on the chain of the key its bound columns
 * hold that matches, starting when not resuming at the first chain tuple in
 * its view and stopping at the end of the view.  Every view but a delta
 * begins at the first identifier, so its tuples begin at the head of the
 * chain; a delta's begin at the first chain tuple at or past its start,
 * which the index finds from the key's frontier.
 *
 * Every tuple on the chain already agrees with the key columns; it is matched
 * against every term all the same, which binds the other columns and checks
 * a variable written twice among them, so a lookup accepts exactly the
 * tuples a scan with the same terms would.  The probe holds each key
 * column's value in that column, the key columns being those of the mask
 * the lookup was planned with, which is its index's; the index reads no
 * other column, and the rest are left unset.  The index is found by
 * position each time, since the array holding it can grow while the plan
 * exists.
 */
static bool
step_lookup(relic_scratch_t *scratch, const relic_op_atom_t *atom,
    relic_op_state_t *state, bool resume)
{
	relic_tuple_range_t *cursor;
	const relic_index_t *index;
	const relic_dom_t *tuple;
	relic_dom_t probe[RELIC_MAX_ARITY];
	relic_tuple_id_t id;
	uint16_t column;

	cursor = &state->cursor;
	index = relic_relation_index_at(atom->relation, atom->index_position);
	assert(relic_index_key_mask(index) == atom->key_mask);

	if (!resume) {
		for (column = 0; column < atom->arity; column++) {
			if ((atom->key_mask & (UINT32_C(1) << column)) != 0)
				probe[column] = term_value(scratch,
				    &atom->terms[column]);
		}

		if (atom->view == RELIC_VIEW_DELTA)
			cursor->first = relic_index_first_since(index,
			    atom->relation, probe, state->view.first);
		else
			cursor->first = relic_index_first(index,
			    atom->relation, probe);
		cursor->end = state->view.end;
	}

	while (cursor->first != RELIC_ID_NONE && cursor->first < cursor->end) {
		id = cursor->first;
		cursor->first = relic_index_next(index, id);
		scratch->counters.tuples_looked_up++;

		tuple = relic_relation_tuple(atom->relation, id);
		if (tuple_matches(scratch, atom, tuple))
			return (true);
	}

	return (false);
}

/*
 * A MEMBER, and the test an ANTI_MEMBER inverts: whether the relation holds
 * the tuple the terms name, at an identifier inside the operation's view.
 */
static bool
step_member(const relic_scratch_t *scratch, const relic_op_atom_t *atom,
    const relic_tuple_range_t *view)
{
	relic_dom_t tuple[RELIC_MAX_ARITY];
	relic_tuple_id_t id;

	build_tuple(scratch, atom, tuple);

	id = relic_relation_find(atom->relation, tuple);
	if (id == RELIC_ID_NONE)
		return (false);

	return (id >= view->first && id < view->end);
}

/*
 * Whether a comparison failed only because it ordered values of different
 * types, which is what RELIC_F_NOTE_MIXED reports.  Equality and inequality
 * are defined across types and never count.
 */
static bool
comparison_is_mixed(const relic_t *ctx, enum relic_compare_op compare,
    relic_dom_t left, relic_dom_t right)
{
	if (compare == RELIC_CMP_EQ || compare == RELIC_CMP_NE)
		return (false);

	return (relic_value_order(ctx, left, right) == RELIC_ORDER_UNORDERED);
}

/*
 * A FILTER: whether its comparison holds between the values of its
 * operands.
 *
 * When the environment asks for them, a comparison that fails because it
 * ordered values of different types is also recorded in the scratch, when it
 * is the first since the caller took the record, for the caller to report.
 * Otherwise a filter costs exactly the comparison and changes nothing.
 */
static bool
step_filter(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_filter_t *filter)
{
	relic_mixed_note_t *note;
	relic_dom_t left;
	relic_dom_t right;

	left = term_value(scratch, &filter->left);
	right = term_value(scratch, &filter->right);

	if (comparison_holds(env->ctx, filter->compare, left, right))
		return (true);

	note = &scratch->note;
	if (env->note_mixed && note->filter == NULL &&
	    comparison_is_mixed(env->ctx, filter->compare, left, right)) {
		note->filter = filter;
		note->left = left;
		note->right = right;
	}

	return (false);
}

/*
 * An EMIT: append the head tuple to the batch.  The caller empties a full
 * batch before the executor runs again, so there is always room.
 */
static void
emit_head(relic_scratch_t *scratch, const relic_op_atom_t *atom)
{
	uint16_t arity;

	arity = atom->arity;
	assert(arity <= scratch->arity_room);
	assert(scratch->batch_count < RELIC_INSERT_BATCH);

	build_tuple(scratch, atom, &scratch->batch[scratch->batch_count *
	    arity]);
	scratch->batch_count++;
}

/*
 * Ask one generator or test of the running plan for a match: a fresh one,
 * or the next one when resuming.
 *
 * A test succeeds at most once, so resuming one always finds nothing more.
 * An ANTI_MEMBER is the membership test inverted: the binding survives when
 * the tuple is absent.  The EMIT is not asked here: the run loop appends its
 * tuple, and treats it as never matching, so nothing below it is ever
 * entered and nothing backtracks into it.
 */
static bool
step(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_t *op, relic_op_state_t *state, bool resume)
{
	switch (op->kind) {
	case RELIC_OP_SCAN:
		return (step_scan(scratch, &op->u.atom, state, resume));
	case RELIC_OP_LOOKUP:
		return (step_lookup(scratch, &op->u.atom, state, resume));
	case RELIC_OP_MEMBER:
		if (resume)
			return (false);
		return (step_member(scratch, &op->u.atom, &state->view));
	case RELIC_OP_ANTI_MEMBER:
		if (resume)
			return (false);
		return (!step_member(scratch, &op->u.atom, &state->view));
	case RELIC_OP_FILTER:
		if (resume)
			return (false);
		return (step_filter(scratch, env, &op->u.filter));
	case RELIC_OP_EMIT:
		break;
	}

	return (false);
}

void
relic_exec_start(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_plan_t *plan)
{
	assert(plan->op_count <= scratch->op_room);
	assert(scratch->note.filter == NULL);

	scratch->plan = plan;
	scratch->env = env;
	resolve_views(scratch, env, plan);

	scratch->batch_count = 0;
	scratch->level = 0;
	scratch->resume = false;
	scratch->finished = false;
}

/*
 * A SCAN copies its view into its cursor each time it starts, so narrowing
 * the first operation's view to the piece is all it takes: the first
 * operation starts once, and every operation below it reads its own view.
 */
void
relic_exec_start_piece(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_plan_t *plan, relic_tuple_range_t piece)
{
	relic_op_state_t *first;

	relic_exec_start(scratch, env, plan);

	first = &scratch->states[0];
	assert(plan->ops[0].kind == RELIC_OP_SCAN);
	assert(first->view.first <= piece.first && piece.first <= piece.end &&
	    piece.end <= first->view.end);

	first->view = piece;
}

/*
 * Record where the join goes on from once the EMIT at `level` has filled
 * the batch, which is where it would have gone had the batch had room: back
 * to the operation before the EMIT, for its next match.  An EMIT that is
 * the plan's first operation has no operation before it, and the plan is
 * finished.  No rule is planned that way, since every body has a literal,
 * but the executor does not rely on it.
 */
static enum relic_exec_status
yield_after_emit(relic_scratch_t *scratch, size_t level)
{
	if (level == 0) {
		scratch->finished = true;
		return (RELIC_EXEC_FINISHED);
	}

	scratch->level = level - 1;
	scratch->resume = true;

	return (RELIC_EXEC_BATCH_FULL);
}

/*
 * Run the plan from where it last stopped until the batch is full or the
 * first operation has nothing more.  `level` is the operation being asked
 * for a match, and `resume` says whether it is being asked for its next one
 * or started afresh.
 *
 * A batch left full by the caller is reported full again rather than
 * appended to: the EMIT writes a tuple only into room the batch has, so the
 * caller's neglect costs it a turn and cannot write past the batch.
 */
enum relic_exec_status
relic_exec_run(relic_scratch_t *scratch)
{
	const relic_exec_env_t *env;
	const relic_plan_t *plan;
	const relic_op_t *op;
	size_t level;
	bool matched;
	bool resume;

	if (scratch->finished)
		return (RELIC_EXEC_FINISHED);
	if (scratch->batch_count == RELIC_INSERT_BATCH)
		return (RELIC_EXEC_BATCH_FULL);

	plan = scratch->plan;
	env = scratch->env;

	level = scratch->level;
	resume = scratch->resume;

	for (;;) {
		op = &plan->ops[level];

		if (op->kind == RELIC_OP_EMIT) {
			emit_head(scratch, &op->u.atom);
			if (scratch->batch_count == RELIC_INSERT_BATCH)
				return (yield_after_emit(scratch, level));
			matched = false;
		} else {
			matched = step(scratch, env, op,
			    &scratch->states[level], resume);
		}

		if (matched) {
			level++;
			resume = false;
			continue;
		}

		if (level == 0)
			break;

		level--;
		resume = true;
	}

	scratch->finished = true;

	return (RELIC_EXEC_FINISHED);
}
