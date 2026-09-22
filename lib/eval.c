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
 * A program is evaluated one strongly connected component of its dependency
 * graph at a time, in the order the program's schedule gives: by stratum, and
 * every component after everything it depends on.  A component's rules
 * therefore read only relations that are already complete or that belong to
 * the component itself.  A component whose rules read none of its own
 * relations needs a single pass; a recursive one runs to a fixed point.
 *
 * Every rule is planned before the first component runs, so a rule that
 * cannot be planned, or memory that runs out while planning, stops the
 * evaluation before it has derived anything.
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
 * append-only -- it offers no operation that removes, reorders or renumbers a
 * tuple -- so the tuples a relation gained in one round are exactly those
 * between its end before the round and its end after it.
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
typedef struct relic_evaluation relic_evaluation_t;
typedef struct relic_running_plan relic_running_plan_t;

/*
 * A plan and the executor state that runs it: one cursor per operation, the
 * range a SCAN resumes from.  A test keeps no state and leaves its cursor
 * unused, which lets the executor's level index the operations and the
 * cursors alike.
 */
struct relic_running_plan {
	relic_plan_t		 plan;
	relic_tuple_range_t	*cursors;
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
 * `fixed_point` is how the component is evaluated.  The variants exist
 * exactly when it is RELIC_FIXED_SEMI_NAIVE, because the one decision sets
 * both.
 */
struct relic_component_plans {
	relic_running_plan_t	 *base;
	relic_running_plan_t	 *variants;
	relic_relation_t	**members;
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
 * `components` holds the plans of each component of the schedule, at the
 * same position, or of the single query an answer is computed for.  They and
 * everything they point at live in the evaluation arena and are allocated
 * before anything runs, so nothing is allocated while a plan runs.
 *
 * `deltas` and `is_member` are indexed by predicate identifier minus one and
 * hold `predicate_count` entries.  They exist only in an evaluation that runs
 * fixed points; a query's single pass has no component to plan variants for,
 * so none of its plans reads a delta.  `deltas` holds, for each member of
 * the component being evaluated semi-naively, the tuples it gained in the
 * previous round, and is where a delta SCAN takes its range from.
 * `is_member` marks the members of the component being planned, which is how
 * a literal is recognized as one that needs a delta variant.
 *
 * `derived` is set whenever an EMIT inserts a tuple the relation did not
 * already hold, which is how a naive pass learns that the fixed point has not
 * been reached.
 */
struct relic_evaluation {
	relic_t			*ctx;
	relic_component_plans_t	*components;
	size_t			 component_count;
	relic_tuple_range_t	*deltas;
	bool			*is_member;
	size_t			 predicate_count;
	relic_dom_t		 frame[RELIC_MAX_VARIABLES];
	bool			 derived;
};

static bool	 advance_deltas(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans);
static int	 build_running_plan(relic_evaluation_t *eval,
		    const relic_rule_t *rule, size_t delta_literal,
		    relic_running_plan_t *running);
static enum relic_fixed_point choose_fixed_point(const relic_t *ctx,
		    const relic_component_t *component);
static int	 confirm_schedule_current(relic_t *ctx);
static size_t	 count_member_literals(const relic_evaluation_t *eval,
		    const relic_rule_t *rule);
static void	*evaluation_alloc(relic_evaluation_t *eval, size_t count,
		    size_t size);
static int	 evaluation_begin(relic_evaluation_t *eval, relic_t *ctx,
		    size_t component_count);
static int	 evaluation_track_deltas(relic_evaluation_t *eval);
static void	 evaluation_end(relic_evaluation_t *eval);
static int	 evaluate_component(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans);
static void	 build_tuple(const relic_evaluation_t *eval,
		    const relic_op_atom_t *atom, relic_dom_t *tuple);
static bool	 comparison_holds(const relic_t *ctx,
		    enum relic_compare_op compare, relic_dom_t left,
		    relic_dom_t right);
static int	 execute_plan(relic_evaluation_t *eval,
		    const relic_running_plan_t *running);
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
static int	 plan_schedule(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule);
static int	 plan_variants(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule,
		    const relic_component_t *component,
		    relic_component_plans_t *plans);
static const relic_rule_t *rule_of(const relic_evaluation_t *eval,
		    const relic_schedule_t *schedule,
		    const relic_component_t *component, size_t i);
static int	 run_plans(relic_evaluation_t *eval,
		    const relic_running_plan_t *plans, size_t count);
static int	 semi_naive_fixed_point(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans);
static void	 start_deltas(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans);
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
 * the first advance measures what the first round inserts.
 */
static void
start_deltas(relic_evaluation_t *eval, const relic_component_plans_t *plans)
{
	const relic_relation_t *member;
	size_t i;

	for (i = 0; i < plans->member_count; i++) {
		member = plans->members[i];
		eval->deltas[member->pred - 1] = relic_relation_since(member,
		    relic_relation_end(member));
	}
}

/*
 * Make each member's delta the tuples it gained in the round just run: from
 * where the previous delta ended to where the relation ends now.  Returns
 * whether any member gained a tuple, which is whether another round is
 * needed.
 */
static bool
advance_deltas(relic_evaluation_t *eval, const relic_component_plans_t *plans)
{
	const relic_relation_t *member;
	relic_tuple_range_t *delta;
	bool gained;
	size_t i;

	gained = false;

	for (i = 0; i < plans->member_count; i++) {
		member = plans->members[i];
		delta = &eval->deltas[member->pred - 1];

		*delta = relic_relation_since(member, delta->end);
		if (delta->first < delta->end)
			gained = true;
	}

	return (gained);
}

/*
 * Run every plan of a component once, then its delta variants round after
 * round until a round inserts nothing.
 */
static int
semi_naive_fixed_point(relic_evaluation_t *eval,
    const relic_component_plans_t *plans)
{
	start_deltas(eval, plans);

	eval->ctx->counters.iterations++;
	if (run_plans(eval, plans->base, plans->base_count) != 0)
		return (-1);

	while (advance_deltas(eval, plans)) {
		eval->ctx->counters.iterations++;
		if (run_plans(eval, plans->variants, plans->variant_count) != 0)
			return (-1);
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

	return (0);
}

/*
 * Plan one rule, or one delta variant of it, with the cursors that run it.
 * Returns 0, or -1 with a diagnostic reported.
 */
static int
build_running_plan(relic_evaluation_t *eval, const relic_rule_t *rule,
    size_t delta_literal, relic_running_plan_t *running)
{
	if (relic_plan_rule(eval->ctx, &eval->ctx->eval_arena, rule,
	    delta_literal, &running->plan) != 0)
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
	return (&eval->ctx->program.rules.entries[
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
 * its rules.
 */
static int
plan_component(relic_evaluation_t *eval, const relic_schedule_t *schedule,
    const relic_component_t *component, relic_component_plans_t *plans)
{
	int status;

	plans->fixed_point = choose_fixed_point(eval->ctx, component);

	if (plan_members(eval, schedule, component, plans) != 0)
		return (-1);

	if (plan_base(eval, schedule, component, plans) != 0)
		return (-1);

	if (plans->fixed_point != RELIC_FIXED_SEMI_NAIVE)
		return (0);

	mark_members(eval, schedule, component, true);
	status = plan_variants(eval, schedule, component, plans);
	mark_members(eval, schedule, component, false);

	return (status);
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
 * Plan every component the schedule lists, in schedule order.
 */
static int
plan_schedule(relic_evaluation_t *eval, const relic_schedule_t *schedule)
{
	size_t i;

	for (i = 0; i < schedule->component_count; i++) {
		if (plan_component(eval, schedule, &schedule->components[i],
		    &eval->components[i]) != 0)
			return (-1);
	}

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
evaluate_component(relic_evaluation_t *eval,
    const relic_component_plans_t *plans)
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
 * Bring the relations to the fixed point of every rule of the program, one
 * component of the schedule at a time.
 *
 * The relations are already a fixed point of the first `materialized_rules`
 * rules, and rules are only ever appended, so nothing needs doing when no
 * rule has been added since.  When some have, every component is evaluated
 * again in schedule order, continuing from the tuples already held.  For a
 * program without negation that is sound: every tuple held is still
 * justified, and the least fixed point reached from them is the one reached
 * from nothing.  Both fixed points begin with a pass over every rule, so the
 * tuples held beforehand are joined like any others.
 */
static int
materialize_program(relic_t *ctx)
{
	const relic_schedule_t *schedule;
	relic_evaluation_t eval;
	size_t i;
	int status;

	if (ctx->materialized_rules == ctx->program.rules.count)
		return (0);

	if (confirm_schedule_current(ctx) != 0)
		return (-1);

	schedule = &ctx->program.schedule;

	status = evaluation_begin(&eval, ctx, schedule->component_count);
	if (status == 0)
		status = evaluation_track_deltas(&eval);
	if (status == 0)
		status = plan_schedule(&eval, schedule);

	for (i = 0; status == 0 && i < schedule->component_count; i++)
		status = evaluate_component(&eval, &eval.components[i]);

	evaluation_end(&eval);

	if (status != 0)
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

	status = evaluation_begin(&eval, ctx, 1);
	if (status == 0)
		status = plan_query(&eval, &query->rule, &eval.components[0]);
	if (status == 0)
		status = run_plans(&eval, eval.components[0].base,
		    eval.components[0].base_count);
	evaluation_end(&eval);

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

	stats->tuples = relic_relation_total_tuples(ctx);
	stats->storage_bytes = relic_relation_total_bytes(ctx);
	stats->symbol_values = relic_value_count_of(ctx, RELIC_SYMBOL);
	stats->string_values = relic_value_count_of(ctx, RELIC_STRING);
	stats->integer_values = relic_value_count_of(ctx, RELIC_INTEGER);
}
