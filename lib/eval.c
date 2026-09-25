/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
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
 * the semi-naive one is tested against, and every operation of its plans
 * reads the whole relation.
 *
 * The semi-naive fixed point makes every derivation exactly once.  Call a
 * rule recursive when a positive literal of its body reads a member of the
 * component, and an exit rule otherwise.  Round 0 runs the facts and the exit
 * rules, each once, in program order; a recursive rule has no ordinary plan
 * and runs only as its delta variants, one per member literal.  Each member
 * has a delta, the range of identifiers of the tuples it gained in the
 * previous round, which starts empty at the first identifier and after round
 * 0 is the whole member.  In variant i of a rule whose member literals are,
 * in body order, m1 to mk, literal mi reads the delta, the member literals
 * before it read the `old` view, the tuples older than their member's delta,
 * and those after it the `current` view, everything up to their delta's end
 * (plan.h).  Rounds run until one inserts nothing.
 *
 * Why every derivation is made, and made once: say a tuple of a member
 * belongs to the round that inserted it, take any way of satisfying a rule's
 * body with member tuples t1 to tk in body order, and let m be the latest
 * round any of them belongs to, and i the first position whose tuple belongs
 * to round m.  Round m inserted a tuple, so round m + 1 runs, and in it
 * variant i finds ti in its delta, every earlier tj -- all of rounds before
 * m -- in the old view, and every later one -- none of a round after m -- in
 * the current view: the combination is derived.  Variant j < i would need tj
 * in its delta, but tj is older than round m; variant j > i would need ti in
 * its old view, but ti is of round m; an earlier round has not yet inserted
 * ti; and a later round's deltas hold no tuple of round m or before.  No
 * view of a round reaches past the ends its members had when the round
 * began, so what the round inserts is invisible to it by construction.
 *
 * The delta is a range of tuple identifiers rather than a copy.  The store is
 * append-only between discards -- nothing removes, reorders or renumbers one
 * tuple, and the discard of every relation runs only before an evaluation
 * begins -- so the tuples a relation gained in one round are exactly those
 * between its end before the round and its end after it.
 *
 * A round runs only the variants whose delta literal reads a member that
 * gained something in the round before, and among those only the ones whose
 * every old view holds something: a join over an empty operand derives
 * nothing, so skipping either kind changes nothing but the cost.  Running
 * the others in the order they were planned makes a round do exactly what
 * running every variant would, while costing what changed rather than what
 * the component holds: a recursive chain of a hundred thousand predicates
 * takes as many rounds as it is long, and each round touches one of them.
 */

/*
 * How a component's relations are brought to their fixed point, decided when
 * the component is planned so that evaluating it runs exactly what was
 * planned for it.
 */
enum relic_fixed_point {
	RELIC_FIXED_ONE_PASS,	/* not recursive: one pass over the rules */
	RELIC_FIXED_NAIVE,	/* every rule, every round */
	RELIC_FIXED_SEMI_NAIVE	/* facts and exit rules, then the variants */
};

typedef struct relic_component_plans relic_component_plans_t;
typedef struct relic_delta_work relic_delta_work_t;
typedef struct relic_evaluation relic_evaluation_t;
typedef struct relic_member_entry relic_member_entry_t;
typedef struct relic_rule_plans relic_rule_plans_t;
typedef struct relic_running_plan relic_running_plan_t;
typedef struct relic_variant relic_variant_t;

/*
 * A plan and the executor state that runs it: one cursor per operation, the
 * range a SCAN resumes from.  A test keeps no state and leaves its cursor
 * unused, which lets the executor's level index the operations and the
 * cursors alike.
 *
 * A LOOKUP uses its cursor differently.  `first` is the next tuple on the
 * index chain, or RELIC_ID_NONE when the chain is exhausted, and `end` is the
 * end of the view it reads through, taken when the lookup started.  A chain
 * is in identifier order, so the lookup stops at the first tuple at or past
 * `end`, which leaves tuples inserted while it runs to the next lookup that
 * starts, exactly as a scan does.
 *
 * `rule` is what the plan was built from, kept so that the plan can be
 * explained and its head found among the component's members.
 */
struct relic_running_plan {
	relic_plan_t		 plan;
	relic_tuple_range_t	*cursors;
	const relic_rule_t	*rule;
};

/*
 * A semi-naive delta variant of `rule`, in which body literal
 * `delta_literal` reads its member's delta.  `placed` runs it with the delta
 * literal ordered like any other and `delta_first` with the delta literal
 * placed first; a round runs `placed` when the delta holds more than
 * `threshold` tuples and `delta_first` otherwise, since which is cheaper
 * depends on a size known only when the round starts.  Each is NULL when no
 * round would run it, and at least one is not (relic_variant_plans_t).
 * Both read every literal through the same view, so they derive the same
 * tuples.
 *
 * `empty_old_views` counts the literals the variant reads through the old
 * view while that view is still empty, one for each member literal written
 * before the delta literal until its member's delta first starts past the
 * first identifier.  A delta's start never moves back, so a view that holds
 * something always will, and the count only falls.  The variant can derive
 * nothing while it is not zero.
 */
struct relic_variant {
	const relic_rule_t	*rule;
	relic_running_plan_t	*placed;
	relic_running_plan_t	*delta_first;
	uint64_t		 threshold;
	size_t			 delta_literal;
	size_t			 empty_old_views;
};

/*
 * A rule of a component with a body, and what runs it.  `base` is the
 * rule's ordinary plan, in which every literal reads its whole relation;
 * `variants` are its `variant_count` delta variants, a run of the
 * component's.  A recursive rule under the semi-naive fixed point has only
 * variants, and every other rule only a base plan.
 */
struct relic_rule_plans {
	const relic_rule_t	*rule;
	relic_running_plan_t	*base;
	relic_variant_t		*variants;
	size_t			 variant_count;
};

/*
 * A variant listed under a member of the component, from which the
 * variants of each member are grouped.
 */
struct relic_member_entry {
	size_t	member;
	size_t	variant;
};

/*
 * Which variants the next semi-naive round runs.
 *
 * `by_member` holds the indexes of the component's variants grouped by the
 * member their delta literal reads, ascending within each member: those of
 * member m are by_member[first_of_member[m]] up to
 * by_member[first_of_member[m + 1]].  `old_readers` groups them the same way
 * by each member they read through the old view, once per such literal, from
 * `first_old_reader`.
 *
 * `gained` lists the members that have gained a tuple in the round being run,
 * each once, which `in_gained` marks.  `changed` lists the members whose
 * delta the round reads holds something, which are those that gained in the
 * round before it.  `due` is where a round gathers the variants it runs.
 *
 * Everything is sized when the component is planned, so a round allocates
 * nothing and cannot fail except by an insertion.
 */
struct relic_delta_work {
	size_t	*by_member;
	size_t	*first_of_member;
	size_t	*old_readers;
	size_t	*first_old_reader;
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
 * `rule_indexes` holds the component's `rule_count` rules, as indexes into
 * the evaluation's table of rules, in program order.  `planned` holds, in
 * the same order, the `planned_count` of them that have a body, each with
 * what runs it.  A fact has no plan: its head is all constants, and a pass
 * inserts it when its turn comes, so that a program's facts cost the
 * evaluation arena nothing, and the component's facts are the rules it has
 * that were not planned.  `variants` holds every rule's semi-naive delta
 * variants: for each rule in program order, one per positive literal over a
 * member of the component, in body order.  It is empty for a component that
 * is not recursive and under the naive fixed point, neither of which runs
 * one.
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
	const size_t		 *rule_indexes;
	relic_rule_plans_t	 *planned;
	relic_variant_t		 *variants;
	relic_relation_t	**members;
	relic_delta_work_t	  work;
	size_t			  rule_count;
	size_t			  planned_count;
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
 * so every operation of its plan reads through the `all` view.  `deltas`
 * holds, for each member of the component being evaluated semi-naively, the
 * tuples it gained in the previous round, and is where every view of a
 * member takes its range from.
 * `is_member` marks the members of the component being planned, which is how
 * a literal is recognized as one that needs a delta variant, and is what
 * tells the planner which relations are still being derived.
 * `member_position` gives each member of the component being planned or
 * evaluated its position among the component's members.  It is written when
 * the component is planned and holds until the next component is, which is
 * what lets a rule find the member its head is while the component runs; it
 * means nothing for any other predicate.
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

static void	 advance_delta(relic_evaluation_t *eval,
		    relic_component_plans_t *plans, size_t member);
static void	 advance_deltas(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static int	 allocate_delta_work(relic_evaluation_t *eval,
		    relic_delta_work_t *work, size_t variants, size_t members);
static int	 derive_for_rewrite(relic_t *ctx, relic_magic_t *magic,
		    const relic_query_t *query);
static int	 answer_from_program(relic_t *ctx, const relic_query_t *query);
static int	 answer_through_rewrite(relic_t *ctx,
		    const relic_query_t *query);
static bool	 query_answerable(relic_t *ctx, const relic_query_t *query,
		    relic_row_cb callback);
static int	 compute_answer(relic_t *ctx, const relic_query_t *query);
static int	 build_running_plan(relic_evaluation_t *eval,
		    const relic_rule_t *rule, relic_relation_t *target,
		    relic_running_plan_t **runningp);
static enum relic_fixed_point choose_fixed_point(const relic_t *ctx,
		    const relic_component_t *component);
static int	 compare_indexes(const void *a, const void *b);
static int	 confirm_planned(const relic_evaluation_t *eval,
		    const relic_component_plans_t *plans, size_t planned,
		    const relic_rule_t *rule);
static int	 confirm_schedule_current(relic_t *ctx);
static size_t	 delta_member_of(const relic_evaluation_t *eval,
		    const relic_variant_t *variant);
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
		    const relic_query_t *query);
static int	 evaluate_schedule(relic_t *ctx, const relic_ruletab_t *rules,
		    const relic_schedule_t *schedule, const bool *wanted,
		    unsigned depth);
static size_t	 first_wanted(const relic_schedule_t *schedule,
		    const bool *wanted);
static void	 note_planning_component(relic_t *ctx,
		    const relic_schedule_t *schedule, size_t position);
static void	 note_query_activity(relic_t *ctx, const char *verb,
		    const relic_relation_t *answer);
static void	 build_tuple(const relic_evaluation_t *eval,
		    const relic_op_atom_t *atom, relic_dom_t *tuple);
static bool	 comparison_holds(const relic_t *ctx,
		    enum relic_compare_op compare, relic_dom_t left,
		    relic_dom_t right);
static bool	 comparison_is_mixed(const relic_t *ctx,
		    enum relic_compare_op compare, relic_dom_t left,
		    relic_dom_t right);
static int	 confirm_fact(const relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    const relic_rule_t *rule);
static bool	 fact_head_is_constant(const relic_rule_t *rule);
static int	 execute_plan(relic_evaluation_t *eval,
		    const relic_running_plan_t *running);
static int	 insert_derived(relic_evaluation_t *eval,
		    relic_relation_t *relation, const relic_dom_t *tuple);
static int	 insert_fact(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    const relic_rule_t *rule);
static size_t	 gather_due_variants(relic_component_plans_t *plans);
static int	 group_by_delta_member(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static int	 group_by_member(relic_evaluation_t *eval,
		    const relic_member_entry_t *entries, size_t count,
		    size_t members, size_t **groupedp, size_t **firstp);
static int	 group_by_old_member(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static bool	 op_reads_old(const relic_op_t *op);
static size_t	 head_member_of(const relic_evaluation_t *eval,
		    const relic_rule_t *rule);
static relic_relation_t *head_relation_of(const relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    const relic_rule_t *rule);
static void	 explain_component(const relic_evaluation_t *eval,
		    const relic_schedule_t *schedule, size_t position,
		    const relic_component_plans_t *plans, unsigned depth);
static bool	*mixed_noted_flag(relic_t *ctx, const relic_rule_t *rule);
static void	 note_mixed_comparison(relic_t *ctx, const relic_rule_t *rule,
		    const relic_op_filter_t *filter, relic_dom_t left,
		    relic_dom_t right);
static const char *type_phrase(const relic_t *ctx, relic_dom_t dom);
static bool	 literal_reads_member(const relic_evaluation_t *eval,
		    const relic_literal_t *literal);
static void	 mark_members(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule,
		    const relic_component_t *component, bool is_member);
static int	 make_running_plan(relic_evaluation_t *eval,
		    const relic_rule_t *rule, const relic_plan_t *plan,
		    relic_running_plan_t **runningp);
static int	 materialize_program(relic_t *ctx);
static int	 naive_fixed_point(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans);
static int	 plan_base(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    relic_rule_plans_t *rule_plans);
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
		    relic_relation_t *answer, relic_component_plans_t *plans);
static int	 plan_rules(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule,
		    const relic_component_t *component,
		    relic_component_plans_t *plans);
static int	 plan_variant(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    const relic_rule_t *rule, size_t j,
		    relic_variant_t *variant);
static int	 plan_variants(relic_evaluation_t *eval,
		    relic_component_plans_t *plans,
		    relic_rule_plans_t *rule_plans);
static const relic_rule_t *rule_of(const relic_evaluation_t *eval,
		    const relic_schedule_t *schedule,
		    const relic_component_t *component, size_t i);
static int	 run_query_rule(relic_t *ctx, const relic_rule_t *rule,
		    relic_relation_t *answer);
static void	 count_empty_old_views(relic_component_plans_t *plans);
static void	 empty_every_delta(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static void	 record_gain(relic_delta_work_t *work, size_t member);
static void	 record_initial_gains(relic_component_plans_t *plans);
static int	 run_base_pass(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    relic_delta_work_t *work);
static bool	 rule_is_recursive(const relic_evaluation_t *eval,
		    const relic_rule_t *rule);
static int	 run_variant(relic_evaluation_t *eval, relic_delta_work_t *work,
		    const relic_variant_t *variant);
static const relic_plan_t *variant_any_plan(const relic_variant_t *variant);
static int	 semi_naive_fixed_point(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static void	 start_deltas(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static int	 step(relic_evaluation_t *eval, const relic_rule_t *rule,
		    const relic_op_t *op, relic_tuple_range_t *cursor,
		    bool resume, bool *matchedp);
static int	 step_emit(relic_evaluation_t *eval,
		    const relic_op_atom_t *atom);
static bool	 step_filter(const relic_evaluation_t *eval,
		    const relic_rule_t *rule, const relic_op_filter_t *filter);
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
static const relic_running_plan_t *variant_plan(
		    const relic_evaluation_t *eval,
		    const relic_variant_t *variant);
static relic_tuple_range_t view_range(const relic_evaluation_t *eval,
		    const relic_op_atom_t *atom);

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
 * The identifiers an operation's view covers, taken as the operation starts:
 * the whole relation as it stands, or a range derived from the delta of the
 * member being read, as plan.h sets out.
 */
static relic_tuple_range_t
view_range(const relic_evaluation_t *eval, const relic_op_atom_t *atom)
{
	relic_tuple_range_t delta;
	relic_tuple_range_t range;

	if (atom->view == RELIC_VIEW_ALL)
		return (relic_relation_all(atom->relation));

	assert(eval->deltas != NULL);
	delta = eval->deltas[atom->relation->pred - 1];
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
 * A SCAN: find the next tuple of its view that matches, starting from the
 * beginning of the view when not resuming.
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
		*cursor = view_range(eval, atom);

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
 * column's value in that column, the columns chosen by the index's own mask,
 * which is the only one it reads; the rest are left unset.  The index is
 * found by position each time, since the array holding it can grow while the
 * plan exists.
 */
static bool
step_lookup(relic_evaluation_t *eval, const relic_op_atom_t *atom,
    relic_tuple_range_t *cursor, bool resume)
{
	relic_index_t *index;
	const relic_dom_t *tuple;
	relic_dom_t probe[RELIC_MAX_ARITY];
	relic_tuple_range_t view;
	relic_tuple_id_t id;
	uint16_t column;

	index = &atom->relation->indexes[atom->index_position];

	if (!resume) {
		for (column = 0; column < atom->relation->arity; column++) {
			if ((index->key_mask & (UINT32_C(1) << column)) != 0)
				probe[column] = term_value(eval,
				    &atom->terms[column]);
		}

		view = view_range(eval, atom);
		if (atom->view == RELIC_VIEW_DELTA)
			cursor->first = relic_index_first_since(index,
			    atom->relation, probe, view.first);
		else
			cursor->first = relic_index_first(index,
			    atom->relation, probe);
		cursor->end = view.end;
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

/*
 * A MEMBER, and the test an ANTI_MEMBER inverts: whether the relation holds
 * the tuple the terms name, at an identifier inside the operation's view.
 */
static bool
step_member(const relic_evaluation_t *eval, const relic_op_atom_t *atom)
{
	relic_dom_t tuple[RELIC_MAX_ARITY];
	relic_tuple_range_t view;
	relic_tuple_id_t id;

	build_tuple(eval, atom, tuple);

	id = relic_relation_find(atom->relation, tuple);
	if (id == RELIC_ID_NONE)
		return (false);

	view = view_range(eval, atom);

	return (id >= view.first && id < view.end);
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
 * A value's type as a note names it, with its article.
 */
static const char *
type_phrase(const relic_t *ctx, relic_dom_t dom)
{
	const relic_value_t *value;

	value = relic_value_of(ctx, dom);
	if (value == NULL)
		return ("an unknown value");

	switch (value->type) {
	case RELIC_INTEGER:
		return ("an integer");
	case RELIC_STRING:
		return ("a string");
	case RELIC_SYMBOL:
		break;
	}

	return ("a symbol");
}

/*
 * The flag recording that a mixed-type comparison of `rule` has been
 * reported.
 *
 * A rule of the program keeps its flag on the program rule it came from, so
 * the copies the Magic rewrite makes of it share one flag, and so do its
 * semi-naive variants, which are the same rule.  The rules of a query share
 * one flag on the context, which is cleared when each answer begins.
 */
static bool *
mixed_noted_flag(relic_t *ctx, const relic_rule_t *rule)
{
	if (rule->origin == RELIC_ORIGIN_QUERY)
		return (&ctx->query_mixed_noted);

	return (&ctx->program.rules.entries[rule->origin].mixed_noted);
}

/*
 * Report a mixed-type ordering comparison of a rule, unless the rule it was
 * written as has had one reported already.  Nothing is allocated, so asking
 * for the note cannot make an evaluation fail.
 */
static void
note_mixed_comparison(relic_t *ctx, const relic_rule_t *rule,
    const relic_op_filter_t *filter, relic_dom_t left, relic_dom_t right)
{
	bool *noted;

	noted = mixed_noted_flag(ctx, rule);
	if (*noted)
		return;
	*noted = true;

	relic_diagf(ctx, RELIC_NOTE, &filter->where,
	    "ordering comparison between %s and %s is false",
	    type_phrase(ctx, left), type_phrase(ctx, right));
}

/*
 * A FILTER of a plan built from `rule`: whether its comparison holds between
 * the values of its operands.
 *
 * Under RELIC_F_NOTE_MIXED a comparison that fails because it ordered values
 * of different types is also reported, which calls the diagnostic callback
 * and marks the rule as reported.  Without the switch a filter costs exactly
 * the comparison and changes nothing.
 */
static bool
step_filter(const relic_evaluation_t *eval, const relic_rule_t *rule,
    const relic_op_filter_t *filter)
{
	relic_dom_t left;
	relic_dom_t right;

	left = term_value(eval, &filter->left);
	right = term_value(eval, &filter->right);

	if (comparison_holds(eval->ctx, filter->compare, left, right))
		return (true);

	if ((eval->ctx->config.flags & RELIC_F_NOTE_MIXED) != 0 &&
	    comparison_is_mixed(eval->ctx, filter->compare, left, right))
		note_mixed_comparison(eval->ctx, rule, filter, left, right);

	return (false);
}

/*
 * An EMIT: insert the head tuple.  Returns -1 when the insertion was refused,
 * which the relation store has reported.
 */
static int
step_emit(relic_evaluation_t *eval, const relic_op_atom_t *atom)
{
	relic_dom_t tuple[RELIC_MAX_ARITY];

	build_tuple(eval, atom, tuple);

	return (insert_derived(eval, atom->relation, tuple));
}

/*
 * Insert a tuple a rule derived, counting the derivation and noting whether
 * the tuple was new.  Both ways a rule produces a tuple -- an EMIT, and a
 * fact's turn in a pass -- insert through here, so they are counted alike.
 * Returns -1 when the insertion was refused, which the relation store has
 * reported.
 */
static int
insert_derived(relic_evaluation_t *eval, relic_relation_t *relation,
    const relic_dom_t *tuple)
{
	bool added;

	if (relic_relation_insert(eval->ctx, relation, tuple, &added) != 0)
		return (-1);

	eval->ctx->counters.derivations++;
	if (added) {
		eval->ctx->counters.tuples_inserted++;
		eval->derived = true;
	}

	return (0);
}

/*
 * Ask one operation of a plan built from `rule` for a match: a fresh one, or
 * the next one when resuming.  Returns -1 when an insertion failed, which the
 * relation store has reported, and otherwise 0 with *matchedp saying whether
 * a binding was produced.
 *
 * A test succeeds at most once, so resuming one always finds nothing more.
 * An ANTI_MEMBER is the membership test inverted: the binding survives when
 * the tuple is absent.  An EMIT is never resumed: it never matches, so
 * nothing below it is ever entered and nothing backtracks into it.
 */
static int
step(relic_evaluation_t *eval, const relic_rule_t *rule, const relic_op_t *op,
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
			*matchedp = step_filter(eval, rule, &op->u.filter);
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
		if (step(eval, running->rule, &running->plan.ops[level],
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
 * A fact's turn in a pass: insert its head, every argument of which is a
 * constant, exactly as the EMIT of a plan for it would.
 */
static int
insert_fact(relic_evaluation_t *eval, const relic_component_plans_t *plans,
    const relic_rule_t *rule)
{
	relic_dom_t tuple[RELIC_MAX_ARITY];
	uint16_t column;

	for (column = 0; column < rule->head_arity; column++)
		tuple[column] = rule->head_args[column].u.constant;

	return (insert_derived(eval, head_relation_of(eval, plans, rule),
	    tuple));
}

/*
 * Confirm that the `planned`th record of a component's plans is the plans of
 * `rule`, which it is because the component's rules with a body were planned
 * in program order.  It is confirmed rather than assumed because a record
 * out of step would not fail: it would run one rule's plan in another's turn.
 */
static int
confirm_planned(const relic_evaluation_t *eval,
    const relic_component_plans_t *plans, size_t planned,
    const relic_rule_t *rule)
{
	if (planned < plans->planned_count &&
	    plans->planned[planned].rule == rule)
		return (0);

	relic_diagf(eval->ctx, RELIC_ERROR, &rule->where,
	    "internal error: a rule's plans are out of step with the program");

	return (-1);
}

/*
 * One pass over a component's rules, each once, in program order: a fact
 * inserts its head, and a rule with a base plan runs it.  A recursive rule
 * under the semi-naive fixed point has none and waits for the rounds that
 * run its variants.  With `work`, each member that gains a tuple is recorded
 * for those rounds.  `derived` is left set exactly when something in the
 * pass was new.
 */
static int
run_base_pass(relic_evaluation_t *eval, const relic_component_plans_t *plans,
    relic_delta_work_t *work)
{
	const relic_rule_plans_t *rule_plans;
	const relic_rule_t *rule;
	size_t planned;
	size_t i;
	bool derived;
	int status;

	derived = false;
	planned = 0;

	for (i = 0; i < plans->rule_count; i++) {
		rule = &eval->rules->entries[plans->rule_indexes[i]];

		eval->derived = false;
		if (rule->body_length == 0) {
			status = insert_fact(eval, plans, rule);
		} else {
			if (confirm_planned(eval, plans, planned, rule) != 0)
				return (-1);
			rule_plans = &plans->planned[planned];
			planned++;
			if (rule_plans->base == NULL)
				continue;
			status = execute_plan(eval, rule_plans->base);
		}
		if (status != 0)
			return (-1);

		if (eval->derived && work != NULL)
			record_gain(work, head_member_of(eval, rule));
		derived = derived || eval->derived;
	}

	assert(planned == plans->planned_count);
	eval->derived = derived;

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

		if (run_base_pass(eval, plans, NULL) != 0)
			return (-1);
	} while (eval->derived);

	return (0);
}

/*
 * Prepare a component for round 0, in three steps.
 */
static void
start_deltas(relic_evaluation_t *eval, relic_component_plans_t *plans)
{
	empty_every_delta(eval, plans);
	count_empty_old_views(plans);
	record_initial_gains(plans);
}

/*
 * Give every member an empty delta at the first identifier, so that its old
 * view is empty and the first delta taken is the whole member, and record
 * that no member has gained anything or holds a delta with anything in it.
 */
static void
empty_every_delta(relic_evaluation_t *eval, relic_component_plans_t *plans)
{
	relic_delta_work_t *work;
	const relic_relation_t *member;
	size_t i;

	work = &plans->work;

	for (i = 0; i < plans->member_count; i++) {
		member = plans->members[i];
		eval->deltas[member->pred - 1].first = RELIC_TUPLE_FIRST;
		eval->deltas[member->pred - 1].end = RELIC_TUPLE_FIRST;
		work->in_gained[i] = false;
	}
	work->gained_count = 0;
	work->changed_count = 0;
}

/*
 * Every old view is empty while every delta starts at the first identifier,
 * so each variant waits on one per operation of its plan that reads one.
 */
static void
count_empty_old_views(relic_component_plans_t *plans)
{
	const relic_delta_work_t *work;
	size_t i;

	work = &plans->work;

	for (i = 0; i < plans->variant_count; i++)
		plans->variants[i].empty_old_views = 0;
	for (i = 0; i < work->first_old_reader[plans->member_count]; i++)
		plans->variants[work->old_readers[i]].empty_old_views++;
}

/*
 * Record a member that already holds tuples as having gained them, as
 * though round 0 had inserted them, so that the first delta is the whole of
 * every member whatever the relations held when the component began.
 */
static void
record_initial_gains(relic_component_plans_t *plans)
{
	size_t i;

	for (i = 0; i < plans->member_count; i++) {
		if (relic_relation_count(plans->members[i]) > 0)
			record_gain(&plans->work, i);
	}
}

/*
 * Record that a member gained a tuple in the round being run, once however
 * many times it gains.  Every insertion during a component's evaluation is
 * into the head of the rule making it, which is a member, so recording the
 * head of every rule that inserted records every member that gains.
 */
static void
record_gain(relic_delta_work_t *work, size_t member)
{
	if (work->in_gained[member])
		return;

	work->in_gained[member] = true;
	work->gained[work->gained_count] = member;
	work->gained_count++;
}

/*
 * The plan of a variant this round runs: the placed plan when its delta
 * holds more tuples than the variant's threshold, and the delta-first plan
 * otherwise, or whichever of the two was built when only one was.
 */
static const relic_running_plan_t *
variant_plan(const relic_evaluation_t *eval, const relic_variant_t *variant)
{
	const relic_tuple_range_t *delta;

	if (variant->placed == NULL)
		return (variant->delta_first);
	if (variant->delta_first == NULL)
		return (variant->placed);

	delta = &eval->deltas[variant->rule->body[
	    variant->delta_literal].u.atom.pred - 1];
	if (delta->end - delta->first > variant->threshold)
		return (variant->placed);

	return (variant->delta_first);
}

/*
 * Run one delta variant of a component being evaluated semi-naively,
 * recording its head as having gained when it inserted a tuple.
 */
static int
run_variant(relic_evaluation_t *eval, relic_delta_work_t *work,
    const relic_variant_t *variant)
{
	eval->derived = false;
	if (execute_plan(eval, variant_plan(eval, variant)) != 0)
		return (-1);

	if (eval->derived)
		record_gain(work, head_member_of(eval, variant->rule));

	return (0);
}

/*
 * Advance one member's delta to the tuples it gained since the delta's end:
 * what the round just run inserted into it, or an empty range at its end.
 *
 * The first time a delta starts past the first identifier, the member's old
 * view holds something from then on, so every variant reading it through
 * that view has one empty old view fewer.
 */
static void
advance_delta(relic_evaluation_t *eval, relic_component_plans_t *plans,
    size_t member)
{
	const relic_delta_work_t *work;
	const relic_relation_t *relation;
	relic_tuple_range_t *delta;
	bool old_was_empty;
	size_t i;

	work = &plans->work;
	relation = plans->members[member];
	delta = &eval->deltas[relation->pred - 1];

	old_was_empty = delta->first == RELIC_TUPLE_FIRST;
	*delta = relic_relation_since(relation, delta->end);
	if (!old_was_empty || delta->first == RELIC_TUPLE_FIRST)
		return;

	for (i = work->first_old_reader[member];
	    i < work->first_old_reader[member + 1]; i++)
		plans->variants[work->old_readers[i]].empty_old_views--;
}

/*
 * The boundary between two rounds.  Afterwards every member's delta is
 * exactly what it gained in the round just run, possibly nothing, and ends
 * at the member's end; the members that gained become the ones whose deltas
 * hold something; and nothing has gained in the round about to run.
 *
 * Only two kinds of member can need advancing: those that gained, whose
 * delta must become what they gained, and those whose delta held something,
 * which must become empty.  Every other member's delta is already empty at
 * its end.  Each member is advanced once, the previous round's nonempty
 * deltas first, skipping the members that gained, then the gainers, since
 * advancing a member twice would empty the delta it had just been given.
 * The boundary therefore costs the two sets rather than the component.
 */
static void
advance_deltas(relic_evaluation_t *eval, relic_component_plans_t *plans)
{
	relic_delta_work_t *work;
	size_t *swap;
	size_t i;

	work = &plans->work;

	for (i = 0; i < work->changed_count; i++) {
		if (!work->in_gained[work->changed[i]])
			advance_delta(eval, plans, work->changed[i]);
	}

	for (i = 0; i < work->gained_count; i++) {
		work->in_gained[work->gained[i]] = false;
		advance_delta(eval, plans, work->gained[i]);
	}

	swap = work->changed;
	work->changed = work->gained;
	work->changed_count = work->gained_count;
	work->gained = swap;
	work->gained_count = 0;
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
 * Gather into `due` the variants that can derive something this round, in
 * the order they were planned, and return how many there are: those whose
 * delta literal reads a member whose delta holds something, and whose every
 * old view does too.  A variant appears once, because it has one delta
 * literal.
 */
static size_t
gather_due_variants(relic_component_plans_t *plans)
{
	relic_delta_work_t *work;
	size_t variant;
	size_t member;
	size_t count;
	size_t i;
	size_t j;

	work = &plans->work;

	count = 0;
	for (i = 0; i < work->changed_count; i++) {
		member = work->changed[i];
		for (j = work->first_of_member[member];
		    j < work->first_of_member[member + 1]; j++) {
			variant = work->by_member[j];
			if (plans->variants[variant].empty_old_views == 0)
				work->due[count++] = variant;
		}
	}

	qsort(work->due, count, sizeof(*work->due), compare_indexes);

	return (count);
}

/*
 * Run round 0 of a component -- its facts and exit rules -- then its delta
 * variants round after round until a round inserts nothing.  Each round runs
 * only the variants that can derive something, in planning order.
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
	if (run_base_pass(eval, plans, work) != 0)
		return (-1);

	while (work->gained_count > 0) {
		eval->ctx->counters.iterations++;

		advance_deltas(eval, plans);
		due_count = gather_due_variants(plans);

		for (i = 0; i < due_count; i++) {
			if (run_variant(eval, work,
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
 * Plan one rule's ordinary plan, with the cursors that run it, into
 * *runningp, its EMIT writing into `target`.  Returns 0, or -1 with a
 * diagnostic reported.
 *
 * The relations marked as members are the ones the planner is told are
 * still being derived.  A query's evaluation marks none, having no
 * membership marks at all: everything a query reads is complete.
 */
static int
build_running_plan(relic_evaluation_t *eval, const relic_rule_t *rule,
    relic_relation_t *target, relic_running_plan_t **runningp)
{
	relic_plan_t plan;

	*runningp = NULL;
	if (relic_plan_rule(eval->ctx, &eval->ctx->eval_arena, rule, target,
	    eval->is_member, &plan) != 0)
		return (-1);

	return (make_running_plan(eval, rule, &plan, runningp));
}

/*
 * Pair a plan built for `rule` with the cursors that run it, in *runningp.
 */
static int
make_running_plan(relic_evaluation_t *eval, const relic_rule_t *rule,
    const relic_plan_t *plan, relic_running_plan_t **runningp)
{
	relic_running_plan_t *running;

	*runningp = NULL;
	running = evaluation_alloc(eval, 1, sizeof(*running));
	if (running == NULL)
		return (-1);

	running->plan = *plan;
	running->rule = rule;
	running->cursors = evaluation_alloc(eval, plan->op_count,
	    sizeof(*running->cursors));
	if (running->cursors == NULL)
		return (-1);

	*runningp = running;
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
 * Whether every argument of a fact's head is a constant, which is what lets
 * its turn in a pass insert the head's constants with no plan.
 */
static bool
fact_head_is_constant(const relic_rule_t *rule)
{
	uint16_t column;

	for (column = 0; column < rule->head_arity; column++) {
		if (rule->head_args[column].kind != RELIC_ARG_CONSTANT)
			return (false);
	}

	return (true);
}

/*
 * Confirm that a fact's head is constants alone, as many as the relation it
 * goes into has columns.  A checked rule always passes -- safety rejects a
 * variable or `_` in the head of a rule with no body -- and the question is
 * asked anyway, once per fact, because a failure would not be silent
 * otherwise: a variable's slot number would be inserted as though it were a
 * constant's identifier.
 */
static int
confirm_fact(const relic_evaluation_t *eval,
    const relic_component_plans_t *plans, const relic_rule_t *rule)
{
	char described[RELIC_PRED_TEXT_MAX];

	if (head_relation_of(eval, plans, rule)->arity == rule->head_arity &&
	    fact_head_is_constant(rule))
		return (0);

	relic_diagf(eval->ctx, RELIC_ERROR, &rule->where,
	    "internal error: a fact of %s holds something other than one "
	    "constant per column", relic_pred_describe(eval->ctx, rule->head,
	    described, sizeof(described)));

	return (-1);
}

/*
 * Whether a rule of the component being planned is recursive: whether a
 * positive literal of its body reads a member, which is what gives it delta
 * variants.  The members must be marked.
 */
static bool
rule_is_recursive(const relic_evaluation_t *eval, const relic_rule_t *rule)
{
	return (count_member_literals(eval, rule) > 0);
}

/*
 * Plan a rule's ordinary plan, every literal reading its whole relation.
 */
static int
plan_base(relic_evaluation_t *eval, const relic_component_plans_t *plans,
    relic_rule_plans_t *rule_plans)
{
	const relic_rule_t *rule;

	rule = rule_plans->rule;

	return (build_running_plan(eval, rule, head_relation_of(eval, plans,
	    rule), &rule_plans->base));
}

/*
 * Plan the variant of `rule` whose delta literal is body literal
 * `delta_literal`: the plans a round may choose between, each left NULL when
 * the planner built none because no round would run it, and the threshold
 * a round chooses by.
 */
static int
plan_variant(relic_evaluation_t *eval, const relic_component_plans_t *plans,
    const relic_rule_t *rule, size_t delta_literal, relic_variant_t *variant)
{
	relic_variant_plans_t variant_plans;

	bzero(variant, sizeof(*variant));
	variant->rule = rule;
	variant->delta_literal = delta_literal;

	if (relic_plan_variant(eval->ctx, &eval->ctx->eval_arena, rule,
	    head_relation_of(eval, plans, rule), delta_literal,
	    eval->is_member, &variant_plans) != 0)
		return (-1);
	variant->threshold = variant_plans.threshold;

	if (variant_plans.placed.ops != NULL &&
	    make_running_plan(eval, rule, &variant_plans.placed,
	    &variant->placed) != 0)
		return (-1);

	if (variant_plans.delta_first.ops != NULL &&
	    make_running_plan(eval, rule, &variant_plans.delta_first,
	    &variant->delta_first) != 0)
		return (-1);

	return (0);
}

/*
 * Plan a recursive rule's delta variants, one per positive literal over a
 * member, in body order, as the next run of the component's variants.
 */
static int
plan_variants(relic_evaluation_t *eval, relic_component_plans_t *plans,
    relic_rule_plans_t *rule_plans)
{
	const relic_rule_t *rule;
	relic_variant_t *variant;
	size_t j;

	rule = rule_plans->rule;
	rule_plans->variants = &plans->variants[plans->variant_count];

	for (j = 0; j < rule->body_length; j++) {
		if (!literal_reads_member(eval, &rule->body[j]))
			continue;

		variant = &plans->variants[plans->variant_count];
		if (plan_variant(eval, plans, rule, j, variant) != 0)
			return (-1);
		plans->variant_count++;
		rule_plans->variant_count++;
	}

	return (0);
}

/*
 * Plan every rule of a component, in program order: confirm that a fact can
 * be inserted as it stands, give a recursive rule its delta variants when
 * the component is evaluated semi-naively, and give every other rule its
 * ordinary plan.  The members must be marked and their relations resolved.
 */
static int
plan_rules(relic_evaluation_t *eval, const relic_schedule_t *schedule,
    const relic_component_t *component, relic_component_plans_t *plans)
{
	relic_rule_plans_t *rule_plans;
	const relic_rule_t *rule;
	size_t with_body;
	size_t variants;
	size_t i;
	int status;

	plans->rule_indexes = &schedule->rule_indexes[component->first_rule];
	plans->rule_count = component->rule_count;

	with_body = 0;
	variants = 0;
	for (i = 0; i < component->rule_count; i++) {
		rule = rule_of(eval, schedule, component, i);
		if (rule->body_length > 0)
			with_body++;
		if (plans->fixed_point == RELIC_FIXED_SEMI_NAIVE)
			variants += count_member_literals(eval, rule);
	}

	plans->planned = evaluation_alloc(eval, with_body,
	    sizeof(*plans->planned));
	if (plans->planned == NULL)
		return (-1);
	bzero(plans->planned, with_body * sizeof(*plans->planned));

	plans->variants = evaluation_alloc(eval, variants,
	    sizeof(*plans->variants));
	if (plans->variants == NULL)
		return (-1);

	for (i = 0; i < component->rule_count; i++) {
		rule = rule_of(eval, schedule, component, i);

		if (rule->body_length == 0) {
			if (confirm_fact(eval, plans, rule) != 0)
				return (-1);
			continue;
		}

		rule_plans = &plans->planned[plans->planned_count];
		rule_plans->rule = rule;
		plans->planned_count++;

		if (plans->fixed_point == RELIC_FIXED_SEMI_NAIVE &&
		    rule_is_recursive(eval, rule))
			status = plan_variants(eval, plans, rule_plans);
		else
			status = plan_base(eval, plans, rule_plans);
		if (status != 0)
			return (-1);
	}

	return (0);
}

/*
 * Where, among the members of the component being planned or evaluated, a
 * rule's head is, and where the member a variant's delta literal reads is.
 * `member_position` must be that component's, which it is from the moment
 * the component is planned until the next one is.
 */
static size_t
head_member_of(const relic_evaluation_t *eval, const relic_rule_t *rule)
{
	return (eval->member_position[rule->head - 1]);
}

/*
 * The relation a rule of the component being planned or evaluated derives
 * into: its head predicate's, which is one of the component's members.  The
 * members' relations must be resolved, and `member_position` must be the
 * component's, as for head_member_of().
 */
static relic_relation_t *
head_relation_of(const relic_evaluation_t *eval,
    const relic_component_plans_t *plans, const relic_rule_t *rule)
{
	relic_relation_t *relation;

	relation = plans->members[head_member_of(eval, rule)];
	assert(relation->pred == rule->head);

	return (relation);
}

static size_t
delta_member_of(const relic_evaluation_t *eval, const relic_variant_t *variant)
{
	return (eval->member_position[variant->rule->body[
	    variant->delta_literal].u.atom.pred - 1]);
}

/*
 * Allocate the sets of a component's delta work, each checked before the
 * next is asked for, since a refused request leaves the arena refusing and
 * each refusal would be reported again.
 */
static int
allocate_delta_work(relic_evaluation_t *eval, relic_delta_work_t *work,
    size_t variants, size_t members)
{
	work->due = evaluation_alloc(eval, variants, sizeof(*work->due));
	if (work->due == NULL)
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
 * Group `count` entries by member, as a counting sort: afterwards the
 * variants listed under member m are (*groupedp)[(*firstp)[m]] up to
 * (*groupedp)[(*firstp)[m + 1]], in the order the entries give them.
 */
static int
group_by_member(relic_evaluation_t *eval, const relic_member_entry_t *entries,
    size_t count, size_t members, size_t **groupedp, size_t **firstp)
{
	size_t *grouped;
	size_t *first;
	size_t *next;
	size_t member;
	size_t i;

	grouped = evaluation_alloc(eval, count, sizeof(*grouped));
	if (grouped == NULL)
		return (-1);
	first = evaluation_alloc(eval, members + 1, sizeof(*first));
	if (first == NULL)
		return (-1);
	next = evaluation_alloc(eval, members, sizeof(*next));
	if (next == NULL)
		return (-1);

	bzero(first, (members + 1) * sizeof(*first));
	for (i = 0; i < count; i++)
		first[entries[i].member + 1]++;

	for (member = 0; member < members; member++) {
		first[member + 1] += first[member];
		next[member] = first[member];
	}

	for (i = 0; i < count; i++)
		grouped[next[entries[i].member]++] = entries[i].variant;

	*groupedp = grouped;
	*firstp = first;

	return (0);
}

/*
 * List, in planning order, every variant under the member its delta literal
 * reads, and group them.
 */
static int
group_by_delta_member(relic_evaluation_t *eval,
    relic_component_plans_t *plans)
{
	relic_member_entry_t *entries;
	size_t i;

	entries = evaluation_alloc(eval, plans->variant_count,
	    sizeof(*entries));
	if (entries == NULL)
		return (-1);

	for (i = 0; i < plans->variant_count; i++) {
		entries[i].member = delta_member_of(eval, &plans->variants[i]);
		entries[i].variant = i;
	}

	return (group_by_member(eval, entries, plans->variant_count,
	    plans->member_count, &plans->work.by_member,
	    &plans->work.first_of_member));
}

/*
 * One of the plans a variant was given, it does not matter which.
 */
static const relic_plan_t *
variant_any_plan(const relic_variant_t *variant)
{
	if (variant->placed != NULL)
		return (&variant->placed->plan);

	return (&variant->delta_first->plan);
}

/*
 * List, in planning order, every variant under each member one of its
 * operations reads through the old view, once per such operation, and group
 * them.  The list is read off the plans themselves, so the count of empty
 * old views a variant waits on is exactly what its plans read; both of a
 * variant's plans read every literal through the same view, so either
 * serves.
 */
static int
group_by_old_member(relic_evaluation_t *eval, relic_component_plans_t *plans)
{
	relic_member_entry_t *entries;
	const relic_plan_t *plan;
	const relic_op_t *op;
	size_t count;
	size_t i;
	size_t j;

	count = 0;
	for (i = 0; i < plans->variant_count; i++) {
		plan = variant_any_plan(&plans->variants[i]);
		for (j = 0; j < plan->op_count; j++) {
			if (op_reads_old(&plan->ops[j]))
				count++;
		}
	}

	entries = evaluation_alloc(eval, count, sizeof(*entries));
	if (entries == NULL)
		return (-1);

	count = 0;
	for (i = 0; i < plans->variant_count; i++) {
		plan = variant_any_plan(&plans->variants[i]);
		for (j = 0; j < plan->op_count; j++) {
			op = &plan->ops[j];
			if (!op_reads_old(op))
				continue;

			entries[count].member = eval->member_position[
			    op->u.atom.relation->pred - 1];
			entries[count].variant = i;
			count++;
		}
	}

	return (group_by_member(eval, entries, count, plans->member_count,
	    &plans->work.old_readers, &plans->work.first_old_reader));
}

/*
 * Whether an operation reads a member through the old view.
 */
static bool
op_reads_old(const relic_op_t *op)
{
	return (op->kind != RELIC_OP_FILTER &&
	    op->u.atom.view == RELIC_VIEW_OLD);
}

/*
 * Prepare what a semi-naive round needs to run only the variants that can
 * derive something: the variants grouped by the member their delta literal
 * reads and by each member they read through the old view, and room for the
 * sets a round keeps.  The members must be marked.
 */
static int
plan_delta_work(relic_evaluation_t *eval, relic_component_plans_t *plans)
{
	if (allocate_delta_work(eval, &plans->work, plans->variant_count,
	    plans->member_count) != 0)
		return (-1);

	if (group_by_delta_member(eval, plans) != 0)
		return (-1);

	return (group_by_old_member(eval, plans));
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

	status = plan_rules(eval, schedule, component, plans);
	if (status == 0 && plans->fixed_point == RELIC_FIXED_SEMI_NAIVE)
		status = plan_delta_work(eval, plans);

	mark_members(eval, schedule, component, false);

	return (status);
}

/*
 * Show a component's plans through the explain callback, if one is
 * installed: its heading, how many facts it has -- its rules without a plan
 * -- and each other rule with its plan or its delta variants.
 */
static void
explain_component(const relic_evaluation_t *eval,
    const relic_schedule_t *schedule, size_t position,
    const relic_component_plans_t *plans, unsigned depth)
{
	const relic_rule_plans_t *rule_plans;
	const relic_variant_t *variant;
	size_t facts;
	size_t i;
	size_t j;

	if (!relic_explaining(eval->ctx))
		return;

	facts = plans->rule_count - plans->planned_count;

	relic_explain_component(eval->ctx, schedule, position, facts, depth);

	for (i = 0; i < plans->planned_count; i++) {
		rule_plans = &plans->planned[i];

		relic_explain_rule(eval->ctx, rule_plans->rule,
		    rule_plans->base == NULL ? NULL : &rule_plans->base->plan,
		    depth);

		for (j = 0; j < rule_plans->variant_count; j++) {
			variant = &rule_plans->variants[j];
			relic_explain_variant(eval->ctx, rule_plans->rule,
			    variant->delta_literal,
			    variant->placed == NULL ? NULL :
			    &variant->placed->plan,
			    variant->delta_first == NULL ? NULL :
			    &variant->delta_first->plan, variant->threshold,
			    depth);
		}
	}
}

/*
 * Plan a query's rule as the single rule of `plans`, deriving into the
 * query's answer relation.
 */
static int
plan_query(relic_evaluation_t *eval, const relic_rule_t *rule,
    relic_relation_t *answer, relic_component_plans_t *plans)
{
	relic_rule_plans_t *rule_plans;

	plans->planned = evaluation_alloc(eval, 1, sizeof(*plans->planned));
	if (plans->planned == NULL)
		return (-1);
	rule_plans = &plans->planned[0];
	bzero(rule_plans, sizeof(*rule_plans));
	rule_plans->rule = rule;
	plans->planned_count = 1;

	return (build_running_plan(eval, rule, answer, &rule_plans->base));
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
		return (run_base_pass(eval, plans, NULL));
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
 * Record that the evaluation is planning component `position` of a
 * schedule, named by its first member, for a refusal by the memory limit to
 * name.  Planning is the only part of evaluating a component that allocates
 * from the evaluation arena.
 */
static void
note_planning_component(relic_t *ctx, const relic_schedule_t *schedule,
    size_t position)
{
	char described[RELIC_PRED_TEXT_MAX];
	relic_pred_id_t first_member;

	first_member = schedule->members[
	    schedule->components[position].first_member];

	relic_diag_activity(ctx, "planning", relic_pred_describe(ctx,
	    first_member, described, sizeof(described)));
}

/*
 * Record that a query is being planned or rewritten, named by its answer
 * relation as every diagnostic about the query names it.
 */
static void
note_query_activity(relic_t *ctx, const char *verb,
    const relic_relation_t *answer)
{
	char described[RELIC_PRED_TEXT_MAX];

	relic_diag_activity(ctx, verb, relic_relation_describe(ctx, answer,
	    described, sizeof(described)));
}

/*
 * The position of the first component `wanted` marks, every component when
 * it is NULL, or the component count when it marks none.
 */
static size_t
first_wanted(const relic_schedule_t *schedule, const bool *wanted)
{
	size_t i;

	for (i = 0; i < schedule->component_count; i++) {
		if (wanted == NULL || wanted[i])
			break;
	}

	return (i);
}

/*
 * Plan and evaluate the components of a schedule over a table of rules, in
 * schedule order, each once every component before it is complete.  With
 * `wanted`, only the components it marks are evaluated; they must be closed
 * under dependency, so that everything a wanted component reads is wanted
 * too.  `depth` is how far the explanation of each component is indented.
 *
 * What the evaluation allocates before its first component it allocates to
 * plan that component, and a refusal by the memory limit there names it.  An
 * evaluation that plans no component is part of a query's rewrite, whose
 * activity then stands.
 */
static int
evaluate_schedule(relic_t *ctx, const relic_ruletab_t *rules,
    const relic_schedule_t *schedule, const bool *wanted, unsigned depth)
{
	relic_component_plans_t *plans;
	relic_evaluation_t eval;
	size_t i;
	int status;

	i = first_wanted(schedule, wanted);
	if (i < schedule->component_count)
		note_planning_component(ctx, schedule, i);

	status = evaluation_begin(&eval, ctx, schedule->component_count);
	eval.rules = rules;
	if (status == 0)
		status = evaluation_track_deltas(&eval);

	for (i = 0; status == 0 && i < schedule->component_count; i++) {
		if (wanted != NULL && !wanted[i])
			continue;

		plans = &eval.components[i];
		note_planning_component(ctx, schedule, i);

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
	int status;

	if (relic_enter(ctx) != 0)
		return (-1);

	status = relic_program_ready(ctx);
	if (status == 0)
		status = materialize_program(ctx);

	relic_leave(ctx);

	return (status);
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
 * The context is busy while the callback runs, so a callback that calls
 * back to load or query is refused, and the answer relation cannot be
 * emptied or refilled under the loop.  The tuple is fetched afresh for each
 * row all the same, so that no pointer into relation storage is held across a
 * call out of the library, and the loop stops at an identifier the relation
 * does not hold rather than read past it, should that ever stop being true.
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
 * Run a query's rule -- the one written, or the one its rewrite produced --
 * once into the query's answer relation, every relation it reads being
 * complete.  One pass computes the answer completely, because no body reads
 * the answer relation: it belongs to no predicate, so no literal can name
 * it.
 *
 * The answer is emptied first, so the run starts from nothing whatever the
 * relation held and whatever ran before it, and emptied again when the run
 * fails, so the answer holds tuples only after a run that succeeded and
 * nothing a caller does after a failure -- such as falling back to the whole
 * program, which discards every relation -- meets a partial one.
 */
static int
run_query_rule(relic_t *ctx, const relic_rule_t *rule,
    relic_relation_t *answer)
{
	relic_evaluation_t eval;
	int status;

	relic_relation_clear(ctx, answer);
	note_query_activity(ctx, "planning", answer);

	status = evaluation_begin(&eval, ctx, 1);
	if (status == 0)
		status = plan_query(&eval, rule, answer, &eval.components[0]);
	if (status == 0) {
		relic_explain_query(ctx, rule, answer,
		    &eval.components[0].planned[0].base->plan);
		status = execute_plan(&eval, eval.components[0].planned[0].base);
	}
	evaluation_end(&eval);

	if (status != 0)
		relic_relation_clear(ctx, answer);

	return (status);
}

/*
 * Stratify and evaluate the rules a query's rewrite generated, after showing
 * the rewrite of the query as written.  The rules are stratifiable
 * by construction -- a generated rule negates only predicates the query reads
 * completely, which no generated rule derives -- so a failure to stratify
 * them is an internal error rather than something the program did.  The
 * analysis still runs, because it is what orders the generated predicates
 * into components and finds the recursive ones.
 */
static int
evaluate_rewrite(relic_t *ctx, relic_magic_t *magic, const relic_query_t *query)
{
	relic_schedule_t schedule;
	int status;

	relic_explain_rewrite(ctx, &query->rule, query->answer, magic);

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
 * Finding what the query reads completely and generating the rules are the
 * rewrite's, and a refusal by the memory limit in either names the query;
 * the evaluations between them name what they plan.
 *
 * Whatever the relations held is discarded first.  What the rewrite derives
 * is only part of the program's fixed point, so the relations are left marked
 * as materializing no rule, and the next evaluation starts again from
 * nothing.
 */
static int
derive_for_rewrite(relic_t *ctx, relic_magic_t *magic,
    const relic_query_t *query)
{
	if (confirm_schedule_current(ctx) != 0)
		return (-1);

	discard_derived_state(ctx);

	note_query_activity(ctx, "rewriting", query->answer);
	if (relic_magic_find_complete(magic, &query->rule) != 0)
		return (-1);

	if (evaluate_schedule(ctx, &ctx->program.rules, &ctx->program.schedule,
	    magic->complete_components, 0) != 0)
		return (-1);

	note_query_activity(ctx, "rewriting", query->answer);
	if (relic_magic_rewrite(magic, &query->rule) != 0)
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
answer_from_program(relic_t *ctx, const relic_query_t *query)
{
	if (materialize_program(ctx) != 0)
		return (-1);

	return (run_query_rule(ctx, &query->rule, query->answer));
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
answer_through_rewrite(relic_t *ctx, const relic_query_t *query)
{
	relic_magic_t magic;
	bool over_budget;
	int status;

	relic_magic_init(&magic, ctx);
	status = derive_for_rewrite(ctx, &magic, query);
	if (status == 0)
		status = run_query_rule(ctx, &magic.query, query->answer);
	over_budget = magic.over_budget;
	relic_magic_reset(&magic);

	if (status != 0 && over_budget)
		return (answer_from_program(ctx, query));

	return (status);
}

/*
 * Answer a query into its answer relation, then stream that relation.
 *
 * The relation is the query's own, emptied before its rule runs and again
 * once the rows are delivered, so outside a run it holds nothing and counts
 * nothing against the limits.  The context is busy throughout, so no other
 * operation sees it holding tuples.
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
	int status;

	if (relic_enter(ctx) != 0)
		return (-1);

	status = relic_query_answer(ctx, query, callback, callback_ctx);

	relic_leave(ctx);

	return (status);
}

int
relic_query_answer(relic_t *ctx, relic_query_t *query, relic_row_cb callback,
    void *callback_ctx)
{
	int status;

	if (!query_answerable(ctx, query, callback))
		return (-1);

	query->running = true;

	status = compute_answer(ctx, query);
	if (status == 0)
		stream_answer(ctx, query->answer, callback, callback_ctx);

	/*
	 * The answer is transient: once delivered, or once the query has
	 * failed part way through computing it, nothing keeps it, so the
	 * relation is emptied and what it counted against the limits given
	 * back.
	 */
	relic_relation_clear(ctx, query->answer);

	query->running = false;

	return (status);
}

/*
 * Whether `query` can be answered in `ctx` into `callback`, reporting why
 * not.  A query from another context would be read with identifiers that
 * mean something else here.
 */
static bool
query_answerable(relic_t *ctx, const relic_query_t *query,
    relic_row_cb callback)
{
	if (query == NULL || callback == NULL) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "a query and a row callback must both be given");
		return (false);
	}

	if (query->ctx != ctx) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "the query belongs to a different context");
		return (false);
	}

	return (true);
}

/*
 * Fill the query's answer relation, from the relations as they stand when
 * the program is materialized or the rewrite is turned off, and through the
 * query's Magic rewrite otherwise.
 */
static int
compute_answer(relic_t *ctx, const relic_query_t *query)
{
	if (relic_program_ready(ctx) != 0)
		return (-1);

	ctx->query_mixed_noted = false;

	if ((ctx->config.flags & RELIC_F_NO_MAGIC) != 0 ||
	    ctx->materialized_rules == ctx->program.rules.count)
		return (answer_from_program(ctx, query));

	return (answer_through_rewrite(ctx, query));
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
