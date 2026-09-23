/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdint.h>
#include <stdlib.h>
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
 * A program is evaluated one strongly connected component of its dependency
 * graph at a time, in the order the program's schedule gives: by stratum, and
 * every component after everything it depends on.  A component's rules
 * therefore read only relations that are already complete or that belong to
 * the component itself.  A component whose rules read none of its own
 * relations needs a single pass; a recursive one runs to a fixed point.
 *
 * A component's rules are planned when the component starts, once every
 * component before it has reached its fixed point.  The relations a rule
 * reads outside its own component are then complete, so the planner can
 * measure them, and building an index over one reads every tuple it will
 * ever hold.  A component that cannot be planned -- memory, or the memory
 * limit while an index is built -- fails the whole evaluation, as a failed
 * insertion does.
 *
 * The relations hold only what evaluation derived -- a fact is a rule too --
 * so they are either the fixed point of every rule the program has, or stale.
 * Stale relations are discarded and the fixed point computed again from
 * nothing.  Continuing from them would be sound for a program without
 * negation, where a later fact can only add to what was derived, but not for
 * one with it, where a later fact can make a derived tuple false; one rule
 * for every program keeps that question from ever being asked.
 *
 * Both fixed points terminate because every value a rule can produce was
 * interned before evaluation began -- a head holds constants and variables,
 * and a variable is bound only from a tuple -- so each relation can hold only
 * finitely many tuples.  Neither overshoots the least fixed point, because
 * every tuple either inserts is justified by tuples already present.  A fact
 * is a rule whose plan is a lone EMIT, so the first pass loads the facts and
 * nothing else has to.
 *
 * The naive fixed point runs the component's plans until a full pass inserts
 * no new tuple.  It reaches the least fixed point because it stops only when
 * every rule, run over everything, adds nothing.  It is kept as the reference
 * the semi-naive one is tested against.
 *
 * The semi-naive fixed point runs every plan once, and from then on runs only
 * delta variants: for each literal over a member of the component, the rule
 * with that literal reading just the tuples its relation gained in the
 * previous round, and every other literal reading the whole relation.  It
 * stops after a round that adds nothing.
 *
 * Why that reaches the least fixed point, finding every derivation: number
 * the rounds from zero, and say a tuple of a member belongs to the round that
 * inserted it, or to round -1 if it was there before round zero.  Take any
 * way of satisfying a rule's body, and let m be the latest round any of its
 * member tuples belongs to.  If m is -1, round zero ran every plan over
 * relations that already held them all.
 * Otherwise round m inserted something, so round m + 1 runs, and in it the
 * variant whose delta literal is matched by a tuple of round m finds that
 * tuple in its delta and every other tuple in the whole relations, which by
 * then hold everything from round m and before.
 *
 * The delta is a range of tuple identifiers rather than a copy.  The store is
 * append-only between discards -- nothing removes, reorders or renumbers one
 * tuple, and the discard of every relation runs only before an evaluation
 * begins -- so the tuples a relation gained in one round are exactly those
 * between its end before the round and its end after it.
 *
 * A round runs only the variants reading a member that gained something in
 * the round before.  A variant's delta literal is its first operation, so a
 * variant over an empty delta matches nothing and derives nothing, and
 * skipping it changes nothing but the cost.  Running the others in the order
 * they were planned makes a round do exactly what running every variant
 * would, while costing what changed rather than what the component holds: a
 * recursive chain of a hundred thousand predicates takes as many rounds as it
 * is long, and each round touches one of them.
 */

/*
 * How a component's relations are brought to their fixed point, decided when
 * the component is planned so that evaluating it runs exactly what was
 * planned for it.
 */
enum relic_fixed_point {
	RELIC_FIXED_ONE_PASS,	/* not recursive: one pass over the rules */
	RELIC_FIXED_NAIVE,	/* every rule, every round */
	RELIC_FIXED_SEMI_NAIVE	/* every rule once, then the delta variants */
};

typedef struct relic_component_plans relic_component_plans_t;
typedef struct relic_delta_work relic_delta_work_t;
typedef struct relic_evaluation relic_evaluation_t;
typedef struct relic_running_plan relic_running_plan_t;

/*
 * A plan and the executor state that runs it: one cursor per operation, the
 * range a SCAN resumes from.  A test keeps no state and leaves its cursor
 * unused, which lets the executor's level index the operations and the
 * cursors alike.
 *
 * A LOOKUP uses its cursor differently.  `first` is the next tuple on the
 * index chain, or RELIC_ID_NONE when the chain is exhausted, and `end` is the
 * relation's end when the lookup started.  A chain is in identifier order, so
 * the lookup stops at the first tuple at or past `end`, which leaves tuples
 * inserted while it runs to the next lookup that starts, exactly as a scan
 * does.
 *
 * `rule` and `delta_literal` are what the plan was built from, kept so that
 * the plan can be explained.  `head_member` is the position of the rule's
 * head among the members of its component, which is what a semi-naive round
 * records as having gained when the plan inserts something; a query's plan
 * belongs to no component and leaves it zero.
 */
struct relic_running_plan {
	relic_plan_t		 plan;
	relic_tuple_range_t	*cursors;
	const relic_rule_t	*rule;
	size_t			 delta_literal;
	size_t			 head_member;
};

/*
 * Which variants the next semi-naive round runs.
 *
 * `by_member` holds the indexes of the component's variants grouped by the
 * member their delta literal reads, ascending within each member: those of
 * member m are by_member[first_of_member[m]] up to
 * by_member[first_of_member[m + 1]].
 *
 * `gained` lists the members that have gained a tuple in the round being run,
 * each once, which `in_gained` marks.  When the next round starts they become
 * `changed`, the members whose deltas it reads, and `gained` starts again
 * empty.  `due` is where a round gathers the variants it runs.
 *
 * Everything is sized when the component is planned, so a round allocates
 * nothing and cannot fail except by an insertion.
 */
struct relic_delta_work {
	size_t	*by_member;
	size_t	*first_of_member;
	size_t	*gained;
	size_t	*changed;
	size_t	*due;
	bool	*in_gained;
	size_t	 gained_count;
	size_t	 changed_count;
};

/*
 * The plans of one component of the schedule.
 *
 * `base` holds one plan per rule, in program order, in which every literal
 * reads its whole relation.  `variants` holds the semi-naive delta variants:
 * for each rule in program order, one per positive literal over a member of
 * the component, in body order.  It is empty for a component that is not
 * recursive and under the naive fixed point, neither of which runs one.
 *
 * `members` are the relations of the component's predicates.  They are
 * resolved when the component is planned, which can fail, so that advancing
 * the deltas between rounds cannot.
 *
 * `fixed_point` is how the component is evaluated.  The variants and the
 * `work` of running them exist exactly when it is RELIC_FIXED_SEMI_NAIVE,
 * because the one decision sets both.
 */
struct relic_component_plans {
	relic_running_plan_t	 *base;
	relic_running_plan_t	 *variants;
	relic_relation_t	**members;
	relic_delta_work_t	  work;
	size_t			  base_count;
	size_t			  variant_count;
	size_t			  member_count;
	enum relic_fixed_point	  fixed_point;
};

/*
 * One evaluation: the plans it runs and the executor's state.
 *
 * `frame` holds one value per slot.  A slot is written by the operation that
 * binds it and read only by operations after that one, so a new binding
 * simply overwrites the old: nothing is saved on descending and nothing
 * restored on backtracking, because no operation reads a slot a later one
 * binds.
 *
 * `rules` is the table the schedule's rule indexes refer to: the program's,
 * or those a query's Magic rewrite generated.  `components` holds the plans
 * of each component of the schedule, at the same position, or of the single
 * query an answer is computed for.  They and
 * everything they point at live in the evaluation arena.  A component's
 * plans are allocated before any of them runs, so nothing is allocated while
 * a plan runs; only the relation store grows then.
 *
 * `deltas` and `is_member` are indexed by predicate identifier minus one and
 * hold `predicate_count` entries.  They exist only in an evaluation that runs
 * fixed points; a query's single pass has no component to plan variants for,
 * so none of its plans reads a delta.  `deltas` holds, for each member of
 * the component being evaluated semi-naively, the tuples it gained in the
 * previous round, and is where a delta SCAN takes its range from.
 * `is_member` marks the members of the component being planned, which is how
 * a literal is recognized as one that needs a delta variant, and is what
 * tells the planner which relations are still being derived.
 * `member_position` gives each marked member its position among the
 * component's members, and means nothing for a predicate not marked.
 *
 * `derived` is set whenever an EMIT inserts a tuple the relation did not
 * already hold, which is how a naive pass learns that the fixed point has not
 * been reached.
 */
struct relic_evaluation {
	relic_t			*ctx;
	const relic_ruletab_t	*rules;
	relic_component_plans_t	*components;
	size_t			 component_count;
	relic_tuple_range_t	*deltas;
	bool			*is_member;
	size_t			*member_position;
	size_t			 predicate_count;
	relic_dom_t		 frame[RELIC_MAX_VARIABLES];
	bool			 derived;
};

static int	 allocate_delta_work(relic_evaluation_t *eval,
		    relic_delta_work_t *work, size_t variants, size_t members);
static int	 derive_for_rewrite(relic_t *ctx, relic_magic_t *magic,
		    const relic_rule_t *query);
static int	 answer_from_program(relic_t *ctx, const relic_rule_t *query);
static int	 answer_through_rewrite(relic_t *ctx,
		    const relic_rule_t *query);
static int	 build_running_plan(relic_evaluation_t *eval,
		    const relic_rule_t *rule, size_t delta_literal,
		    relic_running_plan_t *running);
static enum relic_fixed_point choose_fixed_point(const relic_t *ctx,
		    const relic_component_t *component);
static int	 compare_indexes(const void *a, const void *b);
static int	 confirm_schedule_current(relic_t *ctx);
static size_t	 delta_member_of(const relic_evaluation_t *eval,
		    const relic_running_plan_t *variant);
static void	 discard_derived_state(relic_t *ctx);
static size_t	 count_member_literals(const relic_evaluation_t *eval,
		    const relic_rule_t *rule);
static void	*evaluation_alloc(relic_evaluation_t *eval, size_t count,
		    size_t size);
static int	 evaluation_begin(relic_evaluation_t *eval, relic_t *ctx,
		    size_t component_count);
static int	 evaluation_track_deltas(relic_evaluation_t *eval);
static void	 evaluation_end(relic_evaluation_t *eval);
static int	 evaluate_component(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static int	 evaluate_rewrite(relic_t *ctx, relic_magic_t *magic,
		    const relic_rule_t *query);
static int	 evaluate_schedule(relic_t *ctx, const relic_ruletab_t *rules,
		    const relic_schedule_t *schedule, const bool *wanted,
		    unsigned depth);
static void	 build_tuple(const relic_evaluation_t *eval,
		    const relic_op_atom_t *atom, relic_dom_t *tuple);
static bool	 comparison_holds(const relic_t *ctx,
		    enum relic_compare_op compare, relic_dom_t left,
		    relic_dom_t right);
static int	 execute_plan(relic_evaluation_t *eval,
		    const relic_running_plan_t *running);
static size_t	 gather_due_variants(relic_delta_work_t *work);
static size_t	 head_member_of(const relic_evaluation_t *eval,
		    const relic_rule_t *rule);
static void	 explain_component(const relic_evaluation_t *eval,
		    const relic_schedule_t *schedule, size_t position,
		    const relic_component_plans_t *plans, unsigned depth);
static bool	 literal_reads_member(const relic_evaluation_t *eval,
		    const relic_literal_t *literal);
static void	 mark_members(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule,
		    const relic_component_t *component, bool is_member);
static int	 materialize_program(relic_t *ctx);
static int	 naive_fixed_point(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans);
static int	 plan_base(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule,
		    const relic_component_t *component,
		    relic_component_plans_t *plans);
static int	 plan_delta_work(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static int	 plan_component(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule,
		    const relic_component_t *component,
		    relic_component_plans_t *plans);
static int	 plan_members(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule,
		    const relic_component_t *component,
		    relic_component_plans_t *plans);
static int	 plan_query(relic_evaluation_t *eval, const relic_rule_t *rule,
		    relic_component_plans_t *plans);
static int	 plan_variants(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule,
		    const relic_component_t *component,
		    relic_component_plans_t *plans);
static const relic_rule_t *rule_of(const relic_evaluation_t *eval,
		    const relic_schedule_t *schedule,
		    const relic_component_t *component, size_t i);
static int	 run_query_rule(relic_t *ctx, const relic_rule_t *rule);
static int	 run_plans(relic_evaluation_t *eval,
		    const relic_running_plan_t *plans, size_t count);
static int	 run_recording_gain(relic_evaluation_t *eval,
		    relic_delta_work_t *work,
		    const relic_running_plan_t *running);
static int	 semi_naive_fixed_point(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static void	 start_deltas(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static void	 take_deltas(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static int	 step(relic_evaluation_t *eval, const relic_op_t *op,
		    relic_tuple_range_t *cursor, bool resume, bool *matchedp);
static int	 step_emit(relic_evaluation_t *eval,
		    const relic_op_atom_t *atom);
static bool	 step_filter(const relic_evaluation_t *eval,
		    const relic_op_filter_t *filter);
static bool	 step_lookup(relic_evaluation_t *eval,
		    const relic_op_atom_t *atom, relic_tuple_range_t *cursor,
		    bool resume);
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
 * A SCAN: find the next tuple of its range that matches, starting from the
 * beginning of the range when not resuming.  The range is the whole relation,
 * or for a delta scan the tuples the relation gained in the previous round.
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

	if (!resume) {
		if (atom->delta)
			*cursor = eval->deltas[atom->relation->pred - 1];
		else
			*cursor = relic_relation_all(atom->relation);
	}

	while (cursor->first < cursor->end) {
		id = cursor->first;
		cursor->first++;
		eval->ctx->counters.tuples_scanned++;

		tuple = relic_relation_tuple(atom->relation, id);
		if (tuple_matches(eval, atom, tuple))
			return (true);
	}

	return (false);
}

/*
 * A LOOKUP: find the next tuple on the chain of the key its bound columns
 * hold that matches, starting from the head of the chain when not resuming.
 *
 * Every tuple on the chain already agrees with the key columns; it is matched
 * against every term all the same, which binds the other columns and checks
 * a variable written twice among them, so a lookup accepts exactly the
 * tuples a scan with the same terms would.  The probe holds each key
 * column's value in that column, the columns chosen by the index's own mask,
 * which is the only one it reads; the rest are left unset.  The index is
 * found by position each time, since the array holding it can grow while the
 * plan exists.
 */
static bool
step_lookup(relic_evaluation_t *eval, const relic_op_atom_t *atom,
    relic_tuple_range_t *cursor, bool resume)
{
	const relic_index_t *index;
	const relic_dom_t *tuple;
	relic_dom_t probe[RELIC_MAX_ARITY];
	relic_tuple_id_t id;
	uint16_t column;

	index = &atom->relation->indexes[atom->index_position];

	if (!resume) {
		for (column = 0; column < atom->relation->arity; column++) {
			if ((index->key_mask & (UINT32_C(1) << column)) != 0)
				probe[column] = term_value(eval,
				    &atom->terms[column]);
		}

		cursor->first = relic_index_first(index, atom->relation, probe);
		cursor->end = relic_relation_end(atom->relation);
	}

	while (cursor->first != RELIC_ID_NONE && cursor->first < cursor->end) {
		id = cursor->first;
		cursor->first = relic_index_next(index, id);
		eval->ctx->counters.tuples_looked_up++;

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

	eval->ctx->counters.derivations++;
	if (added) {
		eval->ctx->counters.tuples_inserted++;
		eval->derived = true;
	}

	return (0);
}

/*
 * Ask one operation for a match: a fresh one, or the next one when resuming.
 * Returns -1 when an insertion failed, and otherwise 0 with *matchedp saying
 * whether a binding was produced.
 *
 * A test succeeds at most once, so resuming one always finds nothing more.
 * An ANTI_MEMBER is the membership test inverted: the binding survives when
 * the tuple is absent.  An EMIT is never resumed: it never matches, so
 * nothing below it is ever entered and nothing backtracks into it.
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
	case RELIC_OP_LOOKUP:
		*matchedp = step_lookup(eval, &op->u.atom, cursor, resume);
		break;
	case RELIC_OP_MEMBER:
		if (!resume)
			*matchedp = step_member(eval, &op->u.atom);
		break;
	case RELIC_OP_ANTI_MEMBER:
		if (!resume)
			*matchedp = !step_member(eval, &op->u.atom);
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
execute_plan(relic_evaluation_t *eval, const relic_running_plan_t *running)
{
	size_t level;
	bool matched;
	bool resume;

	level = 0;
	resume = false;

	for (;;) {
		if (step(eval, &running->plan.ops[level],
		    &running->cursors[level], resume, &matched) != 0)
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
 * One pass over `count` plans, in order, which is program order within a
 * component.
 */
static int
run_plans(relic_evaluation_t *eval, const relic_running_plan_t *plans,
    size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (execute_plan(eval, &plans[i]) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Run passes over a component's plans until one inserts nothing new.
 *
 * Within a pass a rule sees what the rules before it inserted, and what its
 * own earlier matches inserted, rather than only what was there when the pass
 * began.  That changes how many passes are needed and not where they end,
 * and it is deterministic, because rules run in program order and relations
 * iterate in insertion order.
 */
static int
naive_fixed_point(relic_evaluation_t *eval,
    const relic_component_plans_t *plans)
{
	do {
		eval->derived = false;
		eval->ctx->counters.iterations++;

		if (run_plans(eval, plans->base, plans->base_count) != 0)
			return (-1);
	} while (eval->derived);

	return (0);
}

/*
 * Give every member an empty delta positioned at its current end, so that
 * the first delta taken holds what the first round inserts, and record that
 * no member has gained anything yet.
 */
static void
start_deltas(relic_evaluation_t *eval, relic_component_plans_t *plans)
{
	const relic_relation_t *member;
	size_t i;

	for (i = 0; i < plans->member_count; i++) {
		member = plans->members[i];
		eval->deltas[member->pred - 1] = relic_relation_since(member,
		    relic_relation_end(member));
		plans->work.in_gained[i] = false;
	}

	plans->work.gained_count = 0;
}

/*
 * Run one plan of a component being evaluated semi-naively, recording its
 * head as having gained when the plan inserted a tuple.  Every insertion
 * during the component's evaluation is into the head of the plan making it,
 * which is a member, so this records every member that gains.
 */
static int
run_recording_gain(relic_evaluation_t *eval, relic_delta_work_t *work,
    const relic_running_plan_t *running)
{
	eval->derived = false;
	if (execute_plan(eval, running) != 0)
		return (-1);

	if (eval->derived && !work->in_gained[running->head_member]) {
		work->in_gained[running->head_member] = true;
		work->gained[work->gained_count++] = running->head_member;
	}

	return (0);
}

/*
 * Begin a round: the members that gained in the round before become the
 * ones whose deltas this round reads, each delta becomes the tuples its
 * member gained since the previous one was taken, and nothing has gained in
 * this round yet.
 *
 * A member that did not gain keeps its old delta, which no variant this
 * round reads: only a variant whose delta literal reads a member that gained
 * is run.
 */
static void
take_deltas(relic_evaluation_t *eval, relic_component_plans_t *plans)
{
	relic_delta_work_t *work;
	const relic_relation_t *member;
	relic_tuple_range_t *delta;
	size_t *swap;
	size_t i;

	work = &plans->work;

	swap = work->changed;
	work->changed = work->gained;
	work->changed_count = work->gained_count;
	work->gained = swap;
	work->gained_count = 0;

	for (i = 0; i < work->changed_count; i++) {
		work->in_gained[work->changed[i]] = false;

		member = plans->members[work->changed[i]];
		delta = &eval->deltas[member->pred - 1];
		*delta = relic_relation_since(member, delta->end);
	}
}

static int
compare_indexes(const void *a, const void *b)
{
	size_t left;
	size_t right;

	left = *(const size_t *)a;
	right = *(const size_t *)b;

	return ((left > right) - (left < right));
}

/*
 * Gather into `due` the variants reading a member whose delta this round
 * reads, in the order they were planned, and return how many there are.  A
 * variant appears once, because it has one delta literal.
 */
static size_t
gather_due_variants(relic_delta_work_t *work)
{
	size_t member;
	size_t count;
	size_t i;
	size_t j;

	count = 0;
	for (i = 0; i < work->changed_count; i++) {
		member = work->changed[i];
		for (j = work->first_of_member[member];
		    j < work->first_of_member[member + 1]; j++)
			work->due[count++] = work->by_member[j];
	}

	qsort(work->due, count, sizeof(*work->due), compare_indexes);

	return (count);
}

/*
 * Run every plan of a component once, then its delta variants round after
 * round until a round inserts nothing.  Each round runs only the variants
 * whose delta holds something, in planning order.
 */
static int
semi_naive_fixed_point(relic_evaluation_t *eval,
    relic_component_plans_t *plans)
{
	relic_delta_work_t *work;
	size_t due_count;
	size_t i;

	work = &plans->work;

	start_deltas(eval, plans);

	eval->ctx->counters.iterations++;
	for (i = 0; i < plans->base_count; i++) {
		if (run_recording_gain(eval, work, &plans->base[i]) != 0)
			return (-1);
	}

	while (work->gained_count > 0) {
		eval->ctx->counters.iterations++;

		take_deltas(eval, plans);
		due_count = gather_due_variants(work);

		for (i = 0; i < due_count; i++) {
			if (run_recording_gain(eval, work,
			    &plans->variants[work->due[i]]) != 0)
				return (-1);
		}
	}

	return (0);
}

/*
 * Allocate from the evaluation arena, reporting exhaustion.
 */
static void *
evaluation_alloc(relic_evaluation_t *eval, size_t count, size_t size)
{
	void *block;

	block = relic_arena_allocarray(&eval->ctx->eval_arena, count, size);
	if (block == NULL)
		relic_diag_no_memory(eval->ctx);

	return (block);
}

/*
 * Begin an evaluation of `component_count` components, or of one query.
 * Returns 0, or -1 with a diagnostic reported.  Either way the caller ends
 * the evaluation with evaluation_end(), which releases what was allocated.
 */
static int
evaluation_begin(relic_evaluation_t *eval, relic_t *ctx,
    size_t component_count)
{
	bzero(eval, sizeof(*eval));
	eval->ctx = ctx;

	eval->components = evaluation_alloc(eval, component_count,
	    sizeof(*eval->components));
	if (eval->components == NULL)
		return (-1);
	bzero(eval->components, component_count * sizeof(*eval->components));
	eval->component_count = component_count;

	return (0);
}

/*
 * Give the evaluation the per-predicate delta ranges and membership marks
 * its fixed points need.  Returns 0, or -1 with a diagnostic reported.
 *
 * Each allocation is checked before the next is made: the arena's failure is
 * sticky, so a second request after a refused one would be refused too and
 * report the same exhaustion again.
 */
static int
evaluation_track_deltas(relic_evaluation_t *eval)
{
	eval->predicate_count = relic_pred_count(eval->ctx);

	eval->deltas = evaluation_alloc(eval, eval->predicate_count,
	    sizeof(*eval->deltas));
	if (eval->deltas == NULL)
		return (-1);
	bzero(eval->deltas, eval->predicate_count * sizeof(*eval->deltas));

	eval->is_member = evaluation_alloc(eval, eval->predicate_count,
	    sizeof(*eval->is_member));
	if (eval->is_member == NULL)
		return (-1);
	bzero(eval->is_member, eval->predicate_count *
	    sizeof(*eval->is_member));

	eval->member_position = evaluation_alloc(eval, eval->predicate_count,
	    sizeof(*eval->member_position));
	if (eval->member_position == NULL)
		return (-1);

	return (0);
}

/*
 * Plan one rule, or one delta variant of it, with the cursors that run it.
 * Returns 0, or -1 with a diagnostic reported.
 *
 * The relations marked as members are the ones the planner is told are
 * still being derived.  A query's evaluation marks none, having no
 * membership marks at all: everything a query reads is complete.
 */
static int
build_running_plan(relic_evaluation_t *eval, const relic_rule_t *rule,
    size_t delta_literal, relic_running_plan_t *running)
{
	running->rule = rule;
	running->delta_literal = delta_literal;
	running->head_member = 0;

	if (relic_plan_rule(eval->ctx, &eval->ctx->eval_arena, rule,
	    delta_literal, eval->is_member, &running->plan) != 0)
		return (-1);

	running->cursors = evaluation_alloc(eval, running->plan.op_count,
	    sizeof(*running->cursors));
	if (running->cursors == NULL)
		return (-1);

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
 * The `i`th rule of a component, in program order.
 */
static const relic_rule_t *
rule_of(const relic_evaluation_t *eval, const relic_schedule_t *schedule,
    const relic_component_t *component, size_t i)
{
	return (&eval->rules->entries[
	    schedule->rule_indexes[component->first_rule + i]]);
}

/*
 * Set or clear the membership mark of every predicate of a component.
 */
static void
mark_members(relic_evaluation_t *eval, const relic_schedule_t *schedule,
    const relic_component_t *component, bool is_member)
{
	relic_pred_id_t member;
	size_t i;

	for (i = 0; i < component->member_count; i++) {
		member = schedule->members[component->first_member + i];
		eval->is_member[member - 1] = is_member;
		eval->member_position[member - 1] = i;
	}
}

/*
 * Whether a body literal is a positive literal over a member of the
 * component being planned, which is what gives its rule a delta variant
 * reading it.  The members must be marked.
 */
static bool
literal_reads_member(const relic_evaluation_t *eval,
    const relic_literal_t *literal)
{
	return (literal->kind == RELIC_LITERAL_POSITIVE &&
	    eval->is_member[literal->u.atom.pred - 1]);
}

/*
 * How many delta variants a rule has: one per literal that reads a member.
 */
static size_t
count_member_literals(const relic_evaluation_t *eval,
    const relic_rule_t *rule)
{
	size_t count;
	size_t i;

	count = 0;
	for (i = 0; i < rule->body_length; i++) {
		if (literal_reads_member(eval, &rule->body[i]))
			count++;
	}

	return (count);
}

/*
 * Resolve the relations of a component's members.
 */
static int
plan_members(relic_evaluation_t *eval, const relic_schedule_t *schedule,
    const relic_component_t *component, relic_component_plans_t *plans)
{
	size_t i;

	plans->members = evaluation_alloc(eval, component->member_count,
	    sizeof(*plans->members));
	if (plans->members == NULL)
		return (-1);

	for (i = 0; i < component->member_count; i++) {
		plans->members[i] = relic_relation_for(eval->ctx,
		    schedule->members[component->first_member + i]);
		if (plans->members[i] == NULL)
			return (-1);
		plans->member_count++;
	}

	return (0);
}

/*
 * Plan every rule of a component as written, each literal reading its whole
 * relation.
 */
static int
plan_base(relic_evaluation_t *eval, const relic_schedule_t *schedule,
    const relic_component_t *component, relic_component_plans_t *plans)
{
	size_t i;

	plans->base = evaluation_alloc(eval, component->rule_count,
	    sizeof(*plans->base));
	if (plans->base == NULL)
		return (-1);

	for (i = 0; i < component->rule_count; i++) {
		if (build_running_plan(eval, rule_of(eval, schedule, component,
		    i), RELIC_PLAN_NO_DELTA, &plans->base[i]) != 0)
			return (-1);
		plans->base_count++;
	}

	return (0);
}

/*
 * Plan the delta variants of a component's rules: for each rule, one per
 * positive literal over a member, with that literal reading the delta.  The
 * members must be marked.
 */
static int
plan_variants(relic_evaluation_t *eval, const relic_schedule_t *schedule,
    const relic_component_t *component, relic_component_plans_t *plans)
{
	const relic_rule_t *rule;
	size_t total;
	size_t i;
	size_t j;

	total = 0;
	for (i = 0; i < component->rule_count; i++)
		total += count_member_literals(eval,
		    rule_of(eval, schedule, component, i));

	plans->variants = evaluation_alloc(eval, total,
	    sizeof(*plans->variants));
	if (plans->variants == NULL)
		return (-1);

	for (i = 0; i < component->rule_count; i++) {
		rule = rule_of(eval, schedule, component, i);

		for (j = 0; j < rule->body_length; j++) {
			if (!literal_reads_member(eval, &rule->body[j]))
				continue;

			if (build_running_plan(eval, rule, j,
			    &plans->variants[plans->variant_count]) != 0)
				return (-1);
			plans->variant_count++;
		}
	}

	return (0);
}

/*
 * Where, among the members of the component being planned, a rule's head
 * is, and where the member a variant's delta literal reads is.  The members
 * must be marked.
 */
static size_t
head_member_of(const relic_evaluation_t *eval, const relic_rule_t *rule)
{
	return (eval->member_position[rule->head - 1]);
}

static size_t
delta_member_of(const relic_evaluation_t *eval,
    const relic_running_plan_t *variant)
{
	return (eval->member_position[
	    variant->rule->body[variant->delta_literal].u.atom.pred - 1]);
}

/*
 * Allocate the arrays of a component's delta work, each checked before the
 * next is asked for, since a refused request leaves the arena refusing and
 * each refusal would be reported again.
 */
static int
allocate_delta_work(relic_evaluation_t *eval, relic_delta_work_t *work,
    size_t variants, size_t members)
{
	work->by_member = evaluation_alloc(eval, variants,
	    sizeof(*work->by_member));
	if (work->by_member == NULL)
		return (-1);

	work->due = evaluation_alloc(eval, variants, sizeof(*work->due));
	if (work->due == NULL)
		return (-1);

	work->first_of_member = evaluation_alloc(eval, members + 1,
	    sizeof(*work->first_of_member));
	if (work->first_of_member == NULL)
		return (-1);

	work->gained = evaluation_alloc(eval, members, sizeof(*work->gained));
	if (work->gained == NULL)
		return (-1);

	work->changed = evaluation_alloc(eval, members, sizeof(*work->changed));
	if (work->changed == NULL)
		return (-1);

	work->in_gained = evaluation_alloc(eval, members,
	    sizeof(*work->in_gained));
	if (work->in_gained == NULL)
		return (-1);

	return (0);
}

/*
 * Prepare what a semi-naive round needs to run only the variants that can
 * derive something: the member each plan's head is, and the variants
 * grouped by the member their delta literal reads.  The grouping is a
 * counting sort over the variants in planning order, so each member's
 * variants stay ascending.  The members must be marked.
 */
static int
plan_delta_work(relic_evaluation_t *eval, relic_component_plans_t *plans)
{
	relic_delta_work_t *work;
	size_t *next;
	size_t member;
	size_t members;
	size_t i;

	work = &plans->work;
	members = plans->member_count;

	if (allocate_delta_work(eval, work, plans->variant_count,
	    members) != 0)
		return (-1);

	next = evaluation_alloc(eval, members, sizeof(*next));
	if (next == NULL)
		return (-1);

	for (i = 0; i < plans->base_count; i++)
		plans->base[i].head_member = head_member_of(eval,
		    plans->base[i].rule);

	bzero(work->first_of_member,
	    (members + 1) * sizeof(*work->first_of_member));
	for (i = 0; i < plans->variant_count; i++) {
		plans->variants[i].head_member = head_member_of(eval,
		    plans->variants[i].rule);
		member = delta_member_of(eval, &plans->variants[i]);
		work->first_of_member[member + 1]++;
	}

	for (member = 0; member < members; member++) {
		work->first_of_member[member + 1] += work->first_of_member[member];
		next[member] = work->first_of_member[member];
	}

	for (i = 0; i < plans->variant_count; i++) {
		member = delta_member_of(eval, &plans->variants[i]);
		work->by_member[next[member]++] = i;
	}

	return (0);
}

/*
 * How a component is to reach its fixed point: in one pass when it is not
 * recursive, and otherwise by the strategy the context was configured with.
 */
static enum relic_fixed_point
choose_fixed_point(const relic_t *ctx, const relic_component_t *component)
{
	if (!component->recursive)
		return (RELIC_FIXED_ONE_PASS);

	if ((ctx->config.flags & RELIC_F_NAIVE) != 0)
		return (RELIC_FIXED_NAIVE);

	return (RELIC_FIXED_SEMI_NAIVE);
}

/*
 * Plan one component: how it reaches its fixed point, its members, its
 * rules, and, when it will be evaluated semi-naively, the delta variants of
 * its rules.  The members are marked throughout, which is how the planner
 * knows which relations it cannot yet measure.
 */
static int
plan_component(relic_evaluation_t *eval, const relic_schedule_t *schedule,
    const relic_component_t *component, relic_component_plans_t *plans)
{
	int status;

	plans->fixed_point = choose_fixed_point(eval->ctx, component);

	if (plan_members(eval, schedule, component, plans) != 0)
		return (-1);

	mark_members(eval, schedule, component, true);

	status = plan_base(eval, schedule, component, plans);
	if (status == 0 && plans->fixed_point == RELIC_FIXED_SEMI_NAIVE)
		status = plan_variants(eval, schedule, component, plans);
	if (status == 0 && plans->fixed_point == RELIC_FIXED_SEMI_NAIVE)
		status = plan_delta_work(eval, plans);

	mark_members(eval, schedule, component, false);

	return (status);
}

/*
 * Show a component's plans through the explain callback, if one is
 * installed: its heading, how many facts it has, and each other rule with
 * its plan and the delta variants planned for it.
 *
 * The variants are in rule order, and each records the rule it was built
 * from, so a rule's variants are the run of them that follows the previous
 * rule's.
 */
static void
explain_component(const relic_evaluation_t *eval,
    const relic_schedule_t *schedule, size_t position,
    const relic_component_plans_t *plans, unsigned depth)
{
	const relic_running_plan_t *base;
	size_t facts;
	size_t variant;
	size_t i;

	if (!relic_explaining(eval->ctx))
		return;

	facts = 0;
	for (i = 0; i < plans->base_count; i++) {
		if (plans->base[i].rule->body_length == 0)
			facts++;
	}

	relic_explain_component(eval->ctx, schedule, position, facts, depth);

	variant = 0;
	for (i = 0; i < plans->base_count; i++) {
		base = &plans->base[i];
		if (base->rule->body_length == 0)
			continue;

		relic_explain_rule(eval->ctx, base->rule, &base->plan, depth);

		for (; variant < plans->variant_count &&
		    plans->variants[variant].rule == base->rule; variant++)
			relic_explain_variant(eval->ctx, base->rule,
			    plans->variants[variant].delta_literal,
			    &plans->variants[variant].plan, depth);
	}
}

/*
 * Plan a query's rule as the single base plan of `plans`.
 */
static int
plan_query(relic_evaluation_t *eval, const relic_rule_t *rule,
    relic_component_plans_t *plans)
{
	plans->base = evaluation_alloc(eval, 1, sizeof(*plans->base));
	if (plans->base == NULL)
		return (-1);

	if (build_running_plan(eval, rule, RELIC_PLAN_NO_DELTA,
	    &plans->base[0]) != 0)
		return (-1);
	plans->base_count = 1;

	return (0);
}

/*
 * Bring one component's relations to their fixed point, given that every
 * component before it in the schedule is complete, in the way it was
 * planned for.
 *
 * A component that is not recursive has no rule reading any of its own
 * relations, so everything its rules read is complete before they run and
 * one pass derives everything they ever will.
 */
static int
evaluate_component(relic_evaluation_t *eval, relic_component_plans_t *plans)
{
	switch (plans->fixed_point) {
	case RELIC_FIXED_ONE_PASS:
		return (run_plans(eval, plans->base, plans->base_count));
	case RELIC_FIXED_NAIVE:
		return (naive_fixed_point(eval, plans));
	case RELIC_FIXED_SEMI_NAIVE:
		return (semi_naive_fixed_point(eval, plans));
	}

	relic_diagf(eval->ctx, RELIC_ERROR, NULL,
	    "internal error: a component was planned with no way to reach its "
	    "fixed point");

	return (-1);
}

/*
 * Confirm that the schedule describes every rule of the program, which a
 * successful check guarantees.  It is confirmed rather than assumed because
 * a stale schedule would not fail: it would silently leave rules out.
 */
static int
confirm_schedule_current(relic_t *ctx)
{
	if (relic_schedule_is_current(&ctx->program))
		return (0);

	relic_diagf(ctx, RELIC_ERROR, NULL,
	    "internal error: the evaluation order does not cover the "
	    "program's rules");

	return (-1);
}

/*
 * Forget everything evaluation derived, which is everything the relations
 * hold, so that they are the fixed point of no rules.  The facts are loaded
 * again by the next evaluation with the rest.
 */
static void
discard_derived_state(relic_t *ctx)
{
	relic_relation_discard_all(ctx);
	ctx->materialized_rules = 0;
}

/*
 * Plan and evaluate the components of a schedule over a table of rules, in
 * schedule order, each once every component before it is complete.  With
 * `wanted`, only the components it marks are evaluated; they must be closed
 * under dependency, so that everything a wanted component reads is wanted
 * too.  `depth` is how far the explanation of each component is indented.
 */
static int
evaluate_schedule(relic_t *ctx, const relic_ruletab_t *rules,
    const relic_schedule_t *schedule, const bool *wanted, unsigned depth)
{
	relic_component_plans_t *plans;
	relic_evaluation_t eval;
	size_t i;
	int status;

	status = evaluation_begin(&eval, ctx, schedule->component_count);
	eval.rules = rules;
	if (status == 0)
		status = evaluation_track_deltas(&eval);

	for (i = 0; status == 0 && i < schedule->component_count; i++) {
		if (wanted != NULL && !wanted[i])
			continue;

		plans = &eval.components[i];

		status = plan_component(&eval, schedule,
		    &schedule->components[i], plans);
		if (status != 0)
			break;

		explain_component(&eval, schedule, i, plans, depth);
		status = evaluate_component(&eval, plans);
	}

	evaluation_end(&eval);

	return (status);
}

/*
 * Bring the relations to the fixed point of every rule of the program, one
 * component of the schedule at a time.
 *
 * Rules are only ever appended, so the relations are current exactly when no
 * rule has been added since they were last brought to a fixed point, and
 * then nothing needs doing.  Otherwise what they hold is stale -- the fixed
 * point of an earlier program, part of one an evaluation that failed was
 * computing, or what a query's Magic rewrite derived -- and it is discarded
 * before every component is planned and evaluated in schedule order.  The
 * discard comes before the evaluation begins, so no delta range can span it.
 */
static int
materialize_program(relic_t *ctx)
{
	if (ctx->materialized_rules == ctx->program.rules.count)
		return (0);

	if (confirm_schedule_current(ctx) != 0)
		return (-1);

	discard_derived_state(ctx);

	if (evaluate_schedule(ctx, &ctx->program.rules, &ctx->program.schedule,
	    NULL, 0) != 0)
		return (-1);

	ctx->materialized_rules = ctx->program.rules.count;

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
 * out of the library.  The range is the one range that lives outside an
 * evaluation and across a call out of the library, so it is the one a discard
 * could invalidate.  A callback may not call back into the context, but one
 * that loads and queries regardless has the answer relation discarded and
 * refilled under the loop.  The stream then delivers whatever the refilled
 * relation holds at the identifiers it has left, and stops at the first one
 * the relation no longer has rather than read a tuple that is not there: a
 * broken contract gives wrong rows, never an invalid read.
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
		if (tuple == NULL)
			return;

		for (column = 0; column < answer->arity; column++)
			row[column] = *relic_value_of(ctx, tuple[column]);

		if (callback(callback_ctx, row, answer->arity) != 0)
			return;
	}
}

/*
 * Run a query's rule once into its answer relation, every relation it reads
 * being complete.  One pass computes the answer completely, because the
 * answer predicate appears in no body: its name holds `$`, which nothing a
 * program writes can.
 */
static int
run_query_rule(relic_t *ctx, const relic_rule_t *rule)
{
	relic_evaluation_t eval;
	int status;

	status = evaluation_begin(&eval, ctx, 1);
	if (status == 0)
		status = plan_query(&eval, rule, &eval.components[0]);
	if (status == 0) {
		relic_explain_query(ctx, rule,
		    &eval.components[0].base[0].plan);
		status = run_plans(&eval, eval.components[0].base,
		    eval.components[0].base_count);
	}
	evaluation_end(&eval);

	return (status);
}

/*
 * Stratify and evaluate the rules a query's rewrite generated, after showing
 * the rewrite of `query`, the query as written.  The rules are stratifiable
 * by construction -- a generated rule negates only predicates the query reads
 * completely, which no generated rule derives -- so a failure to stratify
 * them is an internal error rather than something the program did.  The
 * analysis still runs, because it is what orders the generated predicates
 * into components and finds the recursive ones.
 */
static int
evaluate_rewrite(relic_t *ctx, relic_magic_t *magic, const relic_rule_t *query)
{
	relic_schedule_t schedule;
	int status;

	relic_explain_rewrite(ctx, query, magic);

	relic_schedule_init(&schedule);

	status = -1;
	switch (relic_schedule_build(ctx, &magic->rules, &schedule)) {
	case RELIC_ANALYSIS_STRATIFIED:
		status = evaluate_schedule(ctx, &magic->rules, &schedule, NULL,
		    1);
		break;
	case RELIC_ANALYSIS_UNSTRATIFIED:
		relic_diagf(ctx, RELIC_ERROR, NULL, "internal error: "
		    "a query's Magic rewrite is not stratified");
		break;
	case RELIC_ANALYSIS_INCOMPLETE:
		relic_diag_no_memory(ctx);
		break;
	}

	relic_schedule_reset(&schedule);

	return (status);
}

/*
 * Compute what a query's rewrite needs before its rewritten rule can run:
 * the relations the query reads completely, evaluated from the program's own
 * schedule, and then the fixed point of the rules the rewrite generates.
 *
 * Whatever the relations held is discarded first.  What the rewrite derives
 * is only part of the program's fixed point, so the relations are left marked
 * as materializing no rule, and the next evaluation starts again from
 * nothing.
 */
static int
derive_for_rewrite(relic_t *ctx, relic_magic_t *magic,
    const relic_rule_t *query)
{
	if (confirm_schedule_current(ctx) != 0)
		return (-1);

	discard_derived_state(ctx);

	if (relic_magic_find_complete(magic, query) != 0)
		return (-1);

	if (evaluate_schedule(ctx, &ctx->program.rules, &ctx->program.schedule,
	    magic->complete_components, 0) != 0)
		return (-1);

	if (relic_magic_rewrite(magic, query) != 0)
		return (-1);

	if (magic->adornment_count == 0)
		return (0);

	return (evaluate_rewrite(ctx, magic, query));
}

/*
 * Answer a query into its answer relation from the fixed point of the whole
 * program, computing it first if the relations do not hold it.
 */
static int
answer_from_program(relic_t *ctx, const relic_rule_t *query)
{
	if (materialize_program(ctx) != 0)
		return (-1);

	return (run_query_rule(ctx, query));
}

/*
 * Answer a query into its answer relation through its Magic rewrite, which
 * derives only what the query's bindings can reach, running the rewritten
 * query rule last.
 *
 * A rewrite that outgrows its budget has been reported as a warning, and
 * the query is answered from the whole program instead: the rewrite is an
 * optimization, and must not decide whether a query can be answered.  What
 * the abandoned rewrite derived is discarded with the rest, since it left
 * the relations marked as materializing no rule.
 */
static int
answer_through_rewrite(relic_t *ctx, const relic_rule_t *query)
{
	relic_magic_t magic;
	bool over_budget;
	int status;

	relic_magic_init(&magic, ctx);
	status = derive_for_rewrite(ctx, &magic, query);
	if (status == 0)
		status = run_query_rule(ctx, &magic.query);
	over_budget = magic.over_budget;
	relic_magic_reset(&magic);

	if (status != 0 && over_budget)
		return (answer_from_program(ctx, query));

	return (status);
}

/*
 * Answer a query into its answer relation, then stream that relation.
 *
 * A program whose fixed point is already computed -- or any program, when
 * the rewrite is turned off -- answers from the whole fixed point.  Otherwise
 * the query is answered through its Magic rewrite.  Both give the same
 * answer, because the rewrite computes exactly the tuples of the fixed point
 * that the query reads.
 *
 * Duplicate answers collapse as they are inserted, which is what gives a
 * query set semantics.  Every step that can fail comes before the first row
 * is delivered, so a query either delivers its whole answer or none of it.
 */
int
relic_query_run(relic_t *ctx, relic_query_t *query, relic_row_cb callback,
    void *callback_ctx)
{
	relic_relation_t *answer;
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

	if (relic_program_ready(ctx) != 0)
		return (-1);

	if ((ctx->config.flags & RELIC_F_NO_MAGIC) != 0 ||
	    ctx->materialized_rules == ctx->program.rules.count)
		status = answer_from_program(ctx, &query->rule);
	else
		status = answer_through_rewrite(ctx, &query->rule);

	if (status != 0)
		return (-1);

	answer = relic_relation_for(ctx, query->rule.head);
	if (answer == NULL)
		return (-1);

	stream_answer(ctx, answer, callback, callback_ctx);

	return (0);
}

void
relic_stats(relic_t *ctx, relic_stats_t *stats)
{
	bzero(stats, sizeof(*stats));

	stats->iterations = ctx->counters.iterations;
	stats->derivations = ctx->counters.derivations;
	stats->tuples_inserted = ctx->counters.tuples_inserted;
	stats->tuples_scanned = ctx->counters.tuples_scanned;
	stats->tuples_looked_up = ctx->counters.tuples_looked_up;

	stats->tuples = relic_relation_total_tuples(ctx);
	stats->storage_bytes = relic_relation_total_bytes(ctx);
	stats->symbol_values = relic_value_count_of(ctx, RELIC_SYMBOL);
	stats->string_values = relic_value_count_of(ctx, RELIC_STRING);
	stats->integer_values = relic_value_count_of(ctx, RELIC_INTEGER);
}
