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
 * An expression is a postfix program run on a stack of cells, each holding a
 * domain identifier or a computed integer.  An operator reads a cell as an
 * integer, decoding an identifier through the integer table, and a cell
 * that is a symbol or a string has no integer: the run has no value, and the
 * operation running it rejects the binding.  A result arithmetic does not
 * define ends the run with an error instead, recorded in the scratch, and
 * the plan ends as though every generator were exhausted.
 *
 * The context and every relation are read-only here, as frozen.h makes
 * them, and only the scratch is written.  The views of a plan are resolved
 * when it starts, and no view reaches past the end its relation had then,
 * so whatever the caller does with a full batch before continuing -- insert
 * it into the relation the plan reads, even -- lies past every view and
 * changes nothing the plan reads.
 */

typedef struct relic_cell relic_cell_t;
typedef struct relic_fold relic_fold_t;

/*
 * One cell of the stack a postfix program runs on: a domain identifier
 * pushed from a constant or a domain slot, or an integer pushed from a
 * number slot or computed.
 */
struct relic_cell {
	union {
		relic_dom_t	dom;
		int64_t		number;
	}		u;
	bool		is_number;
};

/*
 * The accumulator of one AGGREGATE's fold: the rows it has folded, the
 * exact sum of their column for a sum, and for a min or a max the extreme
 * value so far, meaningful once `rows` is not zero.
 */
struct relic_fold {
	relic_sum_t	sum;
	uint64_t	rows;
	relic_dom_t	extreme;
};

/*
 * How a postfix program's run ended: with a value, with none because an
 * operand was not an integer, or with a result arithmetic does not define.
 */
enum relic_run_end {
	RELIC_RUN_VALUE,
	RELIC_RUN_NO_VALUE,
	RELIC_RUN_UNDEFINED
};

static void	 build_tuple(const relic_scratch_t *scratch,
		    const relic_op_atom_t *atom, relic_dom_t *tuple);
static relic_dom_t cell_type(relic_cell_t cell);
static bool	 cell_integer(const relic_exec_env_t *env, relic_cell_t cell,
		    int64_t *integerp);
static enum relic_order cells_order(const relic_exec_env_t *env,
		    relic_cell_t left, relic_cell_t right);
static relic_cell_t dom_cell(relic_dom_t dom);
static relic_dom_t emit_column(relic_scratch_t *scratch,
		    const relic_plan_term_t *term);
static void	 emit_head(relic_scratch_t *scratch,
		    const relic_op_atom_t *atom);
static bool	 fold_group(relic_scratch_t *scratch,
		    const relic_exec_env_t *env,
		    const relic_op_aggregate_t *aggregate,
		    const relic_tuple_range_t *view, relic_fold_t *fold);
static bool	 fold_row(relic_scratch_t *scratch, const relic_exec_env_t *env,
		    const relic_op_aggregate_t *aggregate, relic_fold_t *fold,
		    const relic_dom_t *tuple);
static bool	 fold_value(relic_scratch_t *scratch,
		    const relic_op_aggregate_t *aggregate,
		    const relic_fold_t *fold, relic_cell_t *valuep);
static bool	 key_matches(const relic_scratch_t *scratch,
		    const relic_op_aggregate_t *aggregate,
		    const relic_dom_t *tuple);
static relic_cell_t number_cell(int64_t number);
static const char *operator_spelling(const relic_step_t *step);
static bool	 order_satisfies(enum relic_compare_op compare,
		    enum relic_order order);
static void	 record_aggregate_note(relic_scratch_t *scratch,
		    const relic_exec_env_t *env,
		    const relic_op_aggregate_t *aggregate,
		    enum relic_note_kind kind);
static void	 record_error(relic_scratch_t *scratch,
		    const relic_location_t *where,
		    enum relic_arith_status status);
static void	 record_operand_note(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_location_t *where,
		    const relic_step_t *step, relic_cell_t operand);
static void	 record_ordering_note(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_location_t *where,
		    relic_cell_t left, relic_cell_t right);
static enum relic_run_end run_postfix(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_postfix_t *postfix,
		    const relic_location_t *where, relic_cell_t *valuep);
static void	 resolve_views(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_plan_t *plan);
static bool	 step(relic_scratch_t *scratch, const relic_exec_env_t *env,
		    const relic_op_t *op, relic_op_state_t *state, bool resume);
static bool	 step_aggregate(relic_scratch_t *scratch,
		    const relic_exec_env_t *env,
		    const relic_op_aggregate_t *aggregate,
		    const relic_tuple_range_t *view);
static bool	 step_assign(relic_scratch_t *scratch,
		    const relic_exec_env_t *env,
		    const relic_op_assign_t *assign);
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
    size_t op_room, relic_dom_t *batch, int64_t *pending, uint16_t arity_room)
{
	bzero(scratch, sizeof(*scratch));
	scratch->states = states;
	scratch->op_room = op_room;
	scratch->batch = batch;
	scratch->pending = pending;
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
		case RELIC_PLAN_NUMBER:
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
 * Whether a comparison holds between two values that `order` orders.  Two
 * values of different types are unordered, which satisfies no ordering
 * comparison and makes them unequal.  The binding is filtered out rather
 * than reported as an error, because a column may legitimately hold more
 * than one type.
 */
static bool
order_satisfies(enum relic_compare_op compare, enum relic_order order)
{
	switch (compare) {
	case RELIC_CMP_EQ:
		return (order == RELIC_ORDER_EQUAL);
	case RELIC_CMP_NE:
		return (order != RELIC_ORDER_EQUAL);
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
	}

	return (false);
}

static relic_cell_t
dom_cell(relic_dom_t dom)
{
	relic_cell_t cell;

	bzero(&cell, sizeof(cell));
	cell.u.dom = dom;

	return (cell);
}

static relic_cell_t
number_cell(int64_t number)
{
	relic_cell_t cell;

	bzero(&cell, sizeof(cell));
	cell.u.number = number;
	cell.is_number = true;

	return (cell);
}

/*
 * The integer a cell holds, when it holds one: its own, or the one its
 * identifier names.  A symbol or a string has none.
 */
static bool
cell_integer(const relic_exec_env_t *env, relic_cell_t cell, int64_t *integerp)
{
	if (cell.is_number) {
		*integerp = cell.u.number;
		return (true);
	}

	return (relic_value_integer(env->ctx, cell.u.dom, integerp));
}

/*
 * The type of a cell's value, as a domain identifier's type bits.
 */
static relic_dom_t
cell_type(relic_cell_t cell)
{
	if (cell.is_number)
		return (RELIC_DOM_INTEGER);

	return (cell.u.dom & RELIC_DOM_TYPE_MASK);
}

/*
 * How two cells' values order, by the rules of comparison: two identifiers
 * as their values order, two integers numerically, and an integer against
 * an identifier as against the integer it names, unordered against a symbol
 * or a string.
 */
static enum relic_order
cells_order(const relic_exec_env_t *env, relic_cell_t left, relic_cell_t right)
{
	int64_t left_integer;
	int64_t right_integer;

	if (!left.is_number && !right.is_number)
		return (relic_value_order(env->ctx, left.u.dom, right.u.dom));

	if (!cell_integer(env, left, &left_integer) ||
	    !cell_integer(env, right, &right_integer))
		return (RELIC_ORDER_UNORDERED);

	if (left_integer < right_integer)
		return (RELIC_ORDER_LESS);
	if (left_integer > right_integer)
		return (RELIC_ORDER_GREATER);

	return (RELIC_ORDER_EQUAL);
}

/*
 * Record, when the environment asks and nothing is recorded yet, that an
 * ordering comparison at `where` was false because its values' types
 * differ.
 */
static void
record_ordering_note(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_location_t *where, relic_cell_t left, relic_cell_t right)
{
	relic_mixed_note_t *note;

	note = &scratch->note;
	if (!env->note_mixed || note->where != NULL)
		return;

	bzero(note, sizeof(*note));
	note->where = where;
	note->kind = RELIC_NOTE_ORDERING;
	note->left_type = cell_type(left);
	note->right_type = cell_type(right);
}

/*
 * Record, when the environment asks and nothing is recorded yet, that the
 * operator of `step` at `where` was given an operand that is not an integer.
 */
static void
record_operand_note(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_location_t *where, const relic_step_t *step,
    relic_cell_t operand)
{
	relic_mixed_note_t *note;

	note = &scratch->note;
	if (!env->note_mixed || note->where != NULL)
		return;

	bzero(note, sizeof(*note));
	note->where = where;
	note->kind = RELIC_NOTE_OPERAND;
	note->operator = operator_spelling(step);
	note->left_type = cell_type(operand);
}

/*
 * Record, when the environment asks and nothing is recorded yet, that an
 * aggregate had no value because of the types its group's rows hold.
 */
static void
record_aggregate_note(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_aggregate_t *aggregate, enum relic_note_kind kind)
{
	relic_mixed_note_t *note;

	note = &scratch->note;
	if (!env->note_mixed || note->where != NULL)
		return;

	bzero(note, sizeof(*note));
	note->where = &aggregate->where;
	note->kind = kind;
	note->aggregate = aggregate->aggregate;
}

/*
 * Record the first result of the running plan that arithmetic does not
 * define.
 */
static void
record_error(relic_scratch_t *scratch, const relic_location_t *where,
    enum relic_arith_status status)
{
	if (scratch->error.where != NULL)
		return;

	scratch->error.where = where;
	scratch->error.status = status;
}

/*
 * How an arithmetic step's operator is written.
 */
static const char *
operator_spelling(const relic_step_t *step)
{
	if (step->kind == RELIC_STEP_NEGATE)
		return ("-");

	switch (step->u.op) {
	case RELIC_ARITH_ADD:
		return ("+");
	case RELIC_ARITH_SUBTRACT:
		return ("-");
	case RELIC_ARITH_MULTIPLY:
		return ("*");
	case RELIC_ARITH_DIVIDE:
		return ("/");
	case RELIC_ARITH_REMAINDER:
		break;
	}

	return ("mod");
}

/*
 * Run a postfix program of a literal written at `where`, leaving its value
 * in *valuep.  Each step pushes a cell or pops its operands and pushes its
 * result, and the planner sized the program so that the stack never holds
 * more than RELIC_POSTFIX_DEPTH_MAX cells.  An operand that is not an integer
 * ends the run with no value, and a result arithmetic does not define ends
 * it with the error recorded.
 */
static enum relic_run_end
run_postfix(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_postfix_t *postfix, const relic_location_t *where,
    relic_cell_t *valuep)
{
	relic_cell_t stack[RELIC_POSTFIX_DEPTH_MAX];
	enum relic_arith_status status;
	const relic_step_t *step;
	int64_t right;
	int64_t left;
	int64_t result;
	size_t held;
	size_t i;

	assert(postfix->depth <= RELIC_POSTFIX_DEPTH_MAX);
	held = 0;

	for (i = 0; i < postfix->step_count; i++) {
		step = &postfix->steps[i];

		switch (step->kind) {
		case RELIC_STEP_CONSTANT:
			assert(held < postfix->depth);
			stack[held++] = dom_cell(step->u.constant);
			continue;
		case RELIC_STEP_DOMAIN_SLOT:
			assert(held < postfix->depth);
			stack[held++] = dom_cell(scratch->frame[step->u.slot]);
			continue;
		case RELIC_STEP_NUMBER_SLOT:
			assert(held < postfix->depth);
			stack[held++] = number_cell(
			    scratch->numbers[step->u.slot]);
			continue;
		case RELIC_STEP_NEGATE:
			assert(held >= 1);
			if (!cell_integer(env, stack[held - 1], &right)) {
				record_operand_note(scratch, env, where, step,
				    stack[held - 1]);
				return (RELIC_RUN_NO_VALUE);
			}
			status = relic_arith_negate(right, &result);
			break;
		case RELIC_STEP_BINARY:
			assert(held >= 2);
			if (!cell_integer(env, stack[held - 2], &left)) {
				record_operand_note(scratch, env, where, step,
				    stack[held - 2]);
				return (RELIC_RUN_NO_VALUE);
			}
			if (!cell_integer(env, stack[held - 1], &right)) {
				record_operand_note(scratch, env, where, step,
				    stack[held - 1]);
				return (RELIC_RUN_NO_VALUE);
			}
			held--;
			status = relic_arith_binary(step->u.op, left, right,
			    &result);
			break;
		}

		if (status != RELIC_ARITH_DEFINED) {
			record_error(scratch, where, status);
			return (RELIC_RUN_UNDEFINED);
		}
		stack[held - 1] = number_cell(result);
	}

	assert(held == 1);
	*valuep = stack[0];

	return (RELIC_RUN_VALUE);
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
		case RELIC_OP_AGGREGATE:
			scratch->states[level].view = relic_relation_all(
			    op->u.aggregate.relation);
			break;
		case RELIC_OP_FILTER:
		case RELIC_OP_ASSIGN:
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
 * A FILTER: whether its comparison holds between the values of its two
 * postfix programs.  A side with no value makes the comparison false, so
 * the right side is not run once the left has none: an undefined result
 * the right side would meet is not met, as it is not when an earlier
 * filter drops the binding.
 *
 * Two identifiers are equal exactly when they are one identifier, since
 * every value is interned, so an equality of identifiers compares them
 * without decoding.  Otherwise the values are ordered, and when the
 * environment asks for them, a comparison that fails because it ordered
 * values of different types is recorded in the scratch for the caller to
 * report, when it is the first since the caller took the record.
 */
static bool
step_filter(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_filter_t *filter)
{
	enum relic_order order;
	relic_cell_t left;
	relic_cell_t right;

	if (run_postfix(scratch, env, &filter->left, &filter->where,
	    &left) != RELIC_RUN_VALUE)
		return (false);

	if (run_postfix(scratch, env, &filter->right, &filter->where,
	    &right) != RELIC_RUN_VALUE)
		return (false);

	if (!left.is_number && !right.is_number &&
	    (filter->compare == RELIC_CMP_EQ ||
	    filter->compare == RELIC_CMP_NE))
		return ((left.u.dom == right.u.dom) ==
		    (filter->compare == RELIC_CMP_EQ));

	order = cells_order(env, left, right);
	if (order_satisfies(filter->compare, order))
		return (true);

	if (order == RELIC_ORDER_UNORDERED &&
	    filter->compare != RELIC_CMP_EQ && filter->compare != RELIC_CMP_NE)
		record_ordering_note(scratch, env, &filter->where, left, right);

	return (false);
}

/*
 * An ASSIGN: bind its slot to the value of its program, an identifier into
 * a domain slot or an integer into a number slot, whose frame entry is then
 * the integer's identifier when the value table holds one and zero when it
 * does not.  A program with no value binds nothing and rejects the binding.
 */
static bool
step_assign(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_assign_t *assign)
{
	relic_cell_t value;

	if (run_postfix(scratch, env, &assign->value, &assign->where,
	    &value) != RELIC_RUN_VALUE)
		return (false);

	assert(value.is_number == assign->number);
	if (!assign->number) {
		scratch->frame[assign->slot] = value.u.dom;
		return (true);
	}

	scratch->numbers[assign->slot] = value.u.number;
	scratch->frame[assign->slot] = relic_value_find_integer(env->ctx,
	    value.u.number);

	return (true);
}

/*
 * Whether a row of an AGGREGATE's group relation holds the key: whether its
 * leading columns hold what the key slots do.  A key slot that is a number
 * slot holds the integer's identifier, or zero when the value table lacks
 * the integer, which no column holds, as no relation holds such an integer.
 */
static bool
key_matches(const relic_scratch_t *scratch,
    const relic_op_aggregate_t *aggregate, const relic_dom_t *tuple)
{
	uint16_t column;

	for (column = 0; column < aggregate->key_count; column++) {
		if (tuple[column] !=
		    scratch->frame[aggregate->key_slots[column]])
			return (false);
	}

	return (true);
}

/*
 * Fold one row into the accumulator.  Returns false when the row puts the
 * group outside the function's domain -- a sum's row that is not an
 * integer, or a min's or a max's row of another type than the first --
 * having recorded the note, and the fold then has no value.  A count reads
 * no column: its group may have none, and then a tuple holds no value.
 */
static bool
fold_row(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_aggregate_t *aggregate, relic_fold_t *fold,
    const relic_dom_t *tuple)
{
	relic_dom_t value;
	enum relic_order order;
	int64_t integer;

	switch (aggregate->fn) {
	case RELIC_AGG_COUNT:
		break;
	case RELIC_AGG_SUM:
		if (!relic_value_integer(env->ctx, tuple[aggregate->column],
		    &integer)) {
			record_aggregate_note(scratch, env, aggregate,
			    RELIC_NOTE_SUM_TYPE);
			return (false);
		}
		relic_sum_add(&fold->sum, integer);
		break;
	case RELIC_AGG_MIN:
	case RELIC_AGG_MAX:
		value = tuple[aggregate->column];
		if (fold->rows == 0) {
			fold->extreme = value;
			break;
		}
		if ((value & RELIC_DOM_TYPE_MASK) !=
		    (fold->extreme & RELIC_DOM_TYPE_MASK)) {
			record_aggregate_note(scratch, env, aggregate,
			    RELIC_NOTE_EXTREME_TYPES);
			return (false);
		}
		order = relic_value_order(env->ctx, value, fold->extreme);
		if ((aggregate->fn == RELIC_AGG_MIN &&
		    order == RELIC_ORDER_LESS) ||
		    (aggregate->fn == RELIC_AGG_MAX &&
		    order == RELIC_ORDER_GREATER))
			fold->extreme = value;
		break;
	}

	fold->rows++;

	return (true);
}

/*
 * Fold every row of the group the key slots name, reached as the
 * operation's access says, within `view`.  Returns false when a row put the
 * group outside the function's domain.  The walk holds no tuple past the
 * row it folds, and the relation is complete, so nothing inserts into it
 * while the walk runs.
 */
static bool
fold_group(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_aggregate_t *aggregate, const relic_tuple_range_t *view,
    relic_fold_t *fold)
{
	const relic_index_t *index;
	const relic_dom_t *tuple;
	relic_dom_t key[RELIC_MAX_ARITY];
	relic_tuple_id_t id;
	uint16_t column;

	for (column = 0; column < aggregate->key_count; column++)
		key[column] = scratch->frame[aggregate->key_slots[column]];

	switch (aggregate->access) {
	case RELIC_FOLD_SCAN:
		for (id = view->first; id < view->end; id++) {
			scratch->counters.tuples_scanned++;
			tuple = relic_relation_tuple(aggregate->relation, id);
			if (key_matches(scratch, aggregate, tuple) &&
			    !fold_row(scratch, env, aggregate, fold, tuple))
				return (false);
		}
		return (true);
	case RELIC_FOLD_LOOKUP:
		index = relic_relation_index_at(aggregate->relation,
		    aggregate->index_position);
		assert(relic_index_key_mask(index) == aggregate->key_mask);
		for (id = relic_index_first(index, aggregate->relation, key);
		    id != RELIC_ID_NONE && id < view->end;
		    id = relic_index_next(index, id)) {
			scratch->counters.tuples_looked_up++;
			tuple = relic_relation_tuple(aggregate->relation, id);
			if (!fold_row(scratch, env, aggregate, fold, tuple))
				return (false);
		}
		return (true);
	case RELIC_FOLD_PROBE:
		id = relic_relation_find(aggregate->relation, key);
		if (id == RELIC_ID_NONE || id < view->first || id >= view->end)
			return (true);
		scratch->counters.tuples_looked_up++;
		tuple = relic_relation_tuple(aggregate->relation, id);
		return (fold_row(scratch, env, aggregate, fold, tuple));
	}

	return (true);
}

/*
 * The value a completed fold gives: the row count for a count, the total
 * for a sum, and the extreme for a min or a max, whose empty group has no
 * value.  A total that does not fit int64_t records the overflow, which
 * ends the plan.
 */
static bool
fold_value(relic_scratch_t *scratch, const relic_op_aggregate_t *aggregate,
    const relic_fold_t *fold, relic_cell_t *valuep)
{
	switch (aggregate->fn) {
	case RELIC_AGG_COUNT:
		*valuep = number_cell((int64_t)fold->rows);
		return (true);
	case RELIC_AGG_SUM:
		if (!relic_sum_fits(&fold->sum)) {
			record_error(scratch, &aggregate->where,
			    RELIC_ARITH_OVERFLOW);
			return (false);
		}
		*valuep = number_cell(relic_sum_value(&fold->sum));
		return (true);
	case RELIC_AGG_MIN:
	case RELIC_AGG_MAX:
		break;
	}

	if (fold->rows == 0)
		return (false);

	*valuep = dom_cell(fold->extreme);

	return (true);
}

/*
 * An AGGREGATE: fold the group its key names and bind the result slot to
 * the value, an integer into a number slot, whose frame entry is then the
 * integer's identifier or zero, and an identifier into a domain slot.  When
 * the slot was bound before, the fold's value is compared with the slot's
 * value instead, each read as the cell it is, so that a count of two equals
 * a number slot holding two whether or not the value table holds the
 * integer.  A fold with no value rejects the binding.
 */
static bool
step_aggregate(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_aggregate_t *aggregate, const relic_tuple_range_t *view)
{
	relic_fold_t fold;
	relic_cell_t bound;
	relic_cell_t value;

	bzero(&fold, sizeof(fold));
	if (!fold_group(scratch, env, aggregate, view, &fold))
		return (false);
	if (!fold_value(scratch, aggregate, &fold, &value))
		return (false);

	if (!aggregate->binds) {
		bound = aggregate->number ?
		    number_cell(scratch->numbers[aggregate->slot]) :
		    dom_cell(scratch->frame[aggregate->slot]);
		return (cells_order(env, value, bound) == RELIC_ORDER_EQUAL);
	}

	assert(value.is_number == aggregate->number);
	if (!aggregate->number) {
		scratch->frame[aggregate->slot] = value.u.dom;
		return (true);
	}

	scratch->numbers[aggregate->slot] = value.u.number;
	scratch->frame[aggregate->slot] = relic_value_find_integer(env->ctx,
	    value.u.number);

	return (true);
}

/*
 * The value an EMIT writes for one column.  A number slot whose integer the
 * value table holds writes its identifier; one whose integer it does not
 * hold appends the integer to the scratch's pending integers and writes a
 * pending marker, the integer's position there counting from one, which has
 * no type bits and so is no identifier.
 */
static relic_dom_t
emit_column(relic_scratch_t *scratch, const relic_plan_term_t *term)
{
	if (term->action != RELIC_PLAN_NUMBER ||
	    scratch->frame[term->u.slot] != 0)
		return (term_value(scratch, term));

	assert(scratch->pending != NULL && scratch->pending_count <
	    (size_t)RELIC_INSERT_BATCH * scratch->arity_room);
	scratch->pending[scratch->pending_count] =
	    scratch->numbers[term->u.slot];
	scratch->pending_count++;

	return ((relic_dom_t)scratch->pending_count);
}

/*
 * An EMIT: append the head tuple to the batch.  The caller empties a full
 * batch before the executor runs again, so there is always room.
 */
static void
emit_head(relic_scratch_t *scratch, const relic_op_atom_t *atom)
{
	relic_dom_t *tuple;
	uint16_t arity;
	uint16_t column;

	arity = atom->arity;
	assert(arity <= scratch->arity_room);
	assert(scratch->batch_count < RELIC_INSERT_BATCH);

	tuple = &scratch->batch[scratch->batch_count * arity];
	for (column = 0; column < arity; column++)
		tuple[column] = emit_column(scratch, &atom->terms[column]);
	scratch->batch_count++;
}

/*
 * Ask one generator or test of the running plan for a match: a fresh one,
 * or the next one when resuming.
 *
 * A test, an ASSIGN and an AGGREGATE succeed at most once, so resuming one
 * always finds nothing more.
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
	case RELIC_OP_ASSIGN:
		if (resume)
			return (false);
		return (step_assign(scratch, env, &op->u.assign));
	case RELIC_OP_AGGREGATE:
		if (resume)
			return (false);
		return (step_aggregate(scratch, env, &op->u.aggregate,
		    &state->view));
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
	assert(scratch->note.where == NULL && scratch->error.where == NULL);

	scratch->plan = plan;
	scratch->env = env;
	resolve_views(scratch, env, plan);

	scratch->batch_count = 0;
	scratch->pending_count = 0;
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
 * caller's neglect costs it a turn and cannot write past the batch.  An
 * operation that records an error ends the run there, and the plan is
 * finished.
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
			if (scratch->error.where != NULL)
				break;
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
