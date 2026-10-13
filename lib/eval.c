/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
 * The executor (lib/exec.c) runs one plan in the evaluation's scratch and
 * returns each time it has filled a batch of head tuples.  The evaluation
 * takes the batch -- inserting it into the relation the plan's EMIT names,
 * or delivering it to a caller as rows -- and lets the executor go on, until
 * the plan has derived everything it will.
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
 * A program's facts are held in base stores, which evaluation reads and
 * never writes.  A derived predicate's relation holds what evaluation put
 * there: its facts, seeded from its base store before its component's first
 * pass, and what its rules derived.  The derived relations are therefore
 * either the fixed point of the program as it stands, or stale.  Stale
 * relations are discarded and the fixed point computed again from the
 * facts.  Continuing from them would be sound for a program without
 * negation, where a later fact can only add to what was derived, but not for
 * one with it, where a later fact can make a derived tuple false; one rule
 * for every program keeps that question from ever being asked.
 *
 * Without expressions both fixed points terminate: a head holds constants
 * and variables, a variable is bound from a tuple or is an aggregate's
 * result, and an aggregate folds a complete relation once for each of
 * finitely many groups, so every value a head can hold is one of finitely
 * many and each relation can hold only finitely many tuples.  A rule that
 * computes can produce new values without end, so a program that computes
 * terminates only if its rules do; the limits and an interrupt bound one
 * that does not.  Neither overshoots the least fixed point, because
 * every tuple either inserts is justified by tuples already present or by a
 * fact.
 *
 * The naive fixed point runs the component's plans until a full pass inserts
 * no new tuple.  It reaches the least fixed point because it stops only when
 * every rule, run over everything, adds nothing.  It is kept as the reference
 * the semi-naive one is tested against, and every operation of its plans
 * reads the whole relation as it stood when the plan started, so a plan
 * sees what earlier plans of the pass derived but not its own tuples.
 *
 * The semi-naive fixed point makes every derivation exactly once.  Call a rule
 * recursive when a positive literal of its body reads a member of the
 * component, and an exit rule otherwise.  Round 0 runs the exit rules, each
 * once, in program order, after the members were seeded with their facts; a
 * recursive rule has no ordinary plan and runs only as its delta variants, one
 * per member literal.  Each member has a delta, the range of identifiers of the
 * tuples it gained in the previous round, which starts empty at the first
 * identifier and after round 0 is the whole member.  In variant i of a rule
 * whose member literals are, in body order, m1 to mk, literal mi reads the
 * delta, the member literals before it read the `old` view, the tuples older
 * than their member's delta, and those after it the `current` view, everything
 * up to their delta's end (plan.h).  Rounds run until one inserts nothing.
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
 * tuple, and neither the discard of what was derived nor a load's commit to
 * the base stores runs while an evaluation does -- so the tuples a relation
 * gained in one round are exactly those between its end before the round and
 * its end after it.
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
	RELIC_FIXED_SEMI_NAIVE	/* seeds and exit rules, then the variants */
};

typedef struct relic_component_plans relic_component_plans_t;
typedef struct relic_delta_work relic_delta_work_t;
typedef struct relic_emit_target relic_emit_target_t;
typedef struct relic_evaluation relic_evaluation_t;
typedef struct relic_group_run relic_group_run_t;
typedef struct relic_member_entry relic_member_entry_t;
typedef struct relic_rule_plans relic_rule_plans_t;
typedef struct relic_row_sink relic_row_sink_t;
typedef struct relic_running_plan relic_running_plan_t;
typedef struct relic_variant relic_variant_t;

/*
 * The caller a query's rows are delivered to: the callback that receives
 * each, and the context it is called with.
 */
struct relic_row_sink {
	relic_row_cb	 callback;
	void		*callback_ctx;
};

/*
 * Where a running plan's EMIT sends the head tuples it derives.  They have
 * the arity of `relation`, the relation the EMIT names, and are inserted
 * into it; or, when `rows` has a callback, each is delivered to it as a row
 * instead, and the relation is never written.  Only a query's rule is ever
 * given rows, and only when query_streams() admits its plan.  With
 * `reducer`, the tuples are reduced into it instead, and the relation,
 * the source of the reduction, is still never written: only a source's
 * producer is given one, by reduce_source().
 */
struct relic_emit_target {
	relic_relation_t	*relation;
	relic_reducer_t		*reducer;
	relic_row_sink_t	 rows;
};

/*
 * A plan and where what it derives goes.
 *
 * `rule` is what the plan was built from, kept so that the plan can be
 * explained, its head found among the component's members, and a note about
 * one of its comparisons attributed to it.  `target` is where its EMIT's
 * tuples go: the relation the EMIT names, held here writable where the plan
 * holds it read-only, unless the plan is a query's that delivers its rows to
 * the caller.  A running plan is only read while it runs; the executor's
 * state is the evaluation's scratch.  `history` is what the plan did the
 * last time it ran, from which a round estimates whether it is worth
 * sharing among threads, and `insertions` is where what its runs inserted
 * is recorded, from which a round expects the candidates its workers will
 * gather: `own_insertions`, or for the second plan of a delta variant the
 * first's, since the two derive the same tuples.  They are used only on a
 * context whose rounds may be deferred.
 */
struct relic_running_plan {
	relic_plan_t		 plan;
	const relic_rule_t	*rule;
	relic_emit_target_t	 target;
	relic_plan_history_t	 history;
	relic_insert_history_t	*insertions;
	relic_insert_history_t	 own_insertions;
};

/*
 * A semi-naive delta variant of `rule`, in which body literal
 * `delta_literal` reads its member's delta.  `family` holds the drafts of
 * its one or two candidate plans and their recipes (relic_plan_family_t),
 * and `running` what runs each, at the same position, once it has been
 * prepared: the first candidate's when the component is planned, and the
 * second's when a round first chooses it, NULL until then.  With two, each
 * round runs whichever its recipe estimates the less work for, the work of
 * building the indexes it lacks included, since which is cheaper depends
 * on sizes known only when the round starts.  Every candidate reads every
 * literal through the same view, so they derive the same tuples.
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
	relic_plan_family_t	 family;
	relic_running_plan_t	*running[RELIC_FAMILY_MAX];
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
 * round before it.  `due` is where a round gathers the variants it runs,
 * and `chosen` which candidate of each it runs, at the same position.
 *
 * Everything is sized when the component is planned, so a round allocates
 * nothing and cannot fail except by an insertion, or by preparing a
 * candidate it is the first to choose.
 */
struct relic_delta_work {
	size_t	*by_member;
	size_t	*first_of_member;
	size_t	*old_readers;
	size_t	*first_old_reader;
	size_t	*gained;
	size_t	*changed;
	size_t	*due;
	size_t	*chosen;
	bool	*in_gained;
	size_t	 gained_count;
	size_t	 changed_count;
};

/*
 * The plans of one component of the schedule.
 *
 * `rule_indexes` holds the component's `rule_count` rules, as indexes into the
 * evaluation's table of rules, in program order.  `planned` holds, in the same
 * order, the `planned_count` of them that have a body, each with what runs it.
 *  A rule with no body is a seed of the Magic rewrite, since a program's facts
 * are stored rather than compiled, and it has no plan: its head is all
 * constants, and a pass inserts it when its turn comes, so the component's
 * seeds are the rules it has that were not planned.  `variants` holds every
 * rule's semi-naive delta variants: for each rule in program order, one per
 * positive literal over a member of the component, in body order.  It is empty
 * for a component that is not recursive and under the naive fixed point,
 * neither of which runs one.
 *
 * `members` are the relations of the component's predicates.  They are
 * resolved when the component is planned, which can fail, so that advancing
 * the deltas between rounds cannot.  They are the targets of every round
 * the component runs, and `items` has room for the `item_capacity` items
 * the largest of its rounds holds: a pass over its rules, or every
 * variant.
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
	relic_round_item_t	 *items;
	relic_delta_work_t	  work;
	size_t			  rule_count;
	size_t			  planned_count;
	size_t			  variant_count;
	size_t			  member_count;
	size_t			  item_capacity;
	enum relic_fixed_point	  fixed_point;
};

/*
 * One evaluation: the plans it runs and the executor's state.
 *
 * `arena` is where the evaluation allocates, and what ending it releases:
 * the context's evaluation arena, or the arena of a query's group run,
 * whose plans outlast the evaluations that follow it until the query's own
 * plan is explained beside them.  Both are charged to the context's
 * account.
 *
 * `rules` is the table the schedule's rule indexes refer to: the program's,
 * or those a query's Magic rewrite generated.  `components` holds the plans
 * of each component of the schedule, at the same position, or of the single
 * query an answer is computed for.  They and
 * everything they point at live in the evaluation's arena.  A component's
 * plans are allocated when it is planned, except a variant's second
 * candidate, which is prepared between two rounds when one first chooses
 * it; nothing is allocated while a plan runs, when only the relation store
 * grows.
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
 * what lets a rule find the member its head is while the component runs.
 * For any other predicate it is zero or a position in another component, so
 * a relation is a member only if it is the member at its position.
 *
 * `derived` is set whenever a derived tuple is inserted that the relation
 * did not already hold, which is how a naive pass learns that the fixed
 * point has not been reached.
 *
 * `env` is what every plan of the evaluation reads besides its relations:
 * the context, `deltas`, and whether mixed-type comparisons are noted.
 *
 * `scratch` is what the executor runs every plan of the evaluation in, with
 * room for the widest head any of the evaluation's rules can have and for
 * the longest plan it has built.  The batch it fills is taken whole, which
 * overlaps the cache misses of the duplicate checks, when it holds
 * RELIC_INSERT_BATCH tuples and when the plan ends, before anything reads
 * `derived`.  Taking the tuples in batches changes no tuple a plan reads,
 * because no view reaches past the end its relation had when the plan
 * started: a delta variant's views end where the round began, and the `all`
 * view where the plan did.
 *
 * `stopped` is set when the caller a query's plan delivers rows to asks for
 * no more, which ends the plan as though every binding had been tried.
 */
struct relic_evaluation {
	relic_t			*ctx;
	relic_arena_t		*arena;
	const relic_ruletab_t	*rules;
	relic_component_plans_t	*components;
	size_t			 component_count;
	relic_tuple_range_t	*deltas;
	bool			*is_member;
	size_t			*member_position;
	size_t			 predicate_count;
	relic_exec_env_t	 env;
	relic_scratch_t		 scratch;
	relic_pending_map_t	 pending_map;
	relic_pending_probe_t	 pending_probe;
	bool			 derived;
	bool			 stopped;
};

/*
 * The group rules of the query being answered, and what runs them.
 *
 * A query's group rules are its own, not the program's, so answering it
 * runs them itself: once each, in body order, into their group relations,
 * once the relations they read are complete and before any rule a rewrite
 * generated runs, since a generated rule may hold a copy of the aggregate
 * that folds one.  `eval` plans and runs them in `arena`, and is kept with
 * its plans until the query's own plan has been explained, so that explain
 * shows each group rule, with the plan that ran it, under the query's
 * heading.  `begun` says `eval` holds an evaluation to end.
 *
 * The group relations are emptied when the answer is finished, as the
 * answer relation is, so outside an answer a query holds nothing derived
 * and running it again derives the same rows in the same order.
 */
struct relic_group_run {
	relic_arena_t		 arena;
	relic_evaluation_t	 eval;
	const relic_rule_t	*rules;
	size_t			 count;
	bool			 begun;
};

static void	 advance_delta(relic_evaluation_t *eval,
		    relic_component_plans_t *plans, size_t member);
static void	 advance_deltas(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static int	 allocate_delta_work(relic_evaluation_t *eval,
		    relic_delta_work_t *work, size_t variants, size_t members);
static int	 derive_for_rewrite(relic_t *ctx, relic_magic_t *magic,
		    const relic_query_t *query, relic_group_run_t *groups);
static void	 explain_group_rules(relic_t *ctx,
		    const relic_group_run_t *groups);
static void	 group_run_finish(relic_t *ctx, relic_group_run_t *groups);
static void	 group_run_init(relic_group_run_t *groups, relic_t *ctx,
		    const relic_query_t *query);
static int	 plan_group_rules(relic_evaluation_t *eval,
		    const relic_group_run_t *groups);
static int	 run_group_rules(relic_t *ctx, relic_group_run_t *groups);
static int	 answer_from_program(relic_t *ctx, const relic_query_t *query,
		    relic_group_run_t *groups, const relic_row_sink_t *rows);
static int	 answer_through_rewrite(relic_t *ctx,
		    const relic_query_t *query, relic_group_run_t *groups,
		    const relic_row_sink_t *rows);
static bool	 query_answerable(relic_t *ctx, const relic_query_t *query,
		    relic_row_cb callback);
static int	 compute_answer(relic_t *ctx, const relic_query_t *query,
		    const relic_row_sink_t *rows);
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
static void	 record_materialized(relic_t *ctx);
static size_t	 count_member_literals(const relic_evaluation_t *eval,
		    const relic_rule_t *rule);
static void	*evaluation_alloc(relic_evaluation_t *eval, size_t count,
		    size_t size);
static int	 evaluation_begin(relic_evaluation_t *eval, relic_t *ctx,
		    relic_arena_t *arena,
		    size_t component_count, uint16_t head_arity_max);
static int	 evaluation_track_deltas(relic_evaluation_t *eval);
static uint16_t	 widest_head(const relic_ruletab_t *rules);
static void	 evaluation_end(relic_evaluation_t *eval);
static int	 evaluate_component(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static int	 evaluate_scheduled(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule, size_t position,
		    unsigned depth);
static bool	 reduction_certified(const relic_t *ctx,
		    const relic_component_plans_t *plans);
static int	 reduce_source(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule, size_t position,
		    relic_component_plans_t *plans,
		    relic_reduced_source_t *source, unsigned depth);
static int	 run_reduction(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    relic_reducer_t *reducer, relic_summary_t **summaryp);
static int	 project_keys(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule, size_t position,
		    const relic_key_projection_t *projection,
		    const relic_reduced_source_t *source, unsigned depth);
static int	 reduce_batch(relic_evaluation_t *eval,
		    const relic_emit_target_t *target);
static int	 seed_member(relic_evaluation_t *eval,
		    relic_relation_t *derived);
static int	 seed_members(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans);
static int	 evaluate_read_completely(relic_t *ctx, relic_magic_t *magic,
		    const relic_query_t *query);
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
static int	 confirm_seed(const relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    const relic_rule_t *rule);
static bool	 seed_head_is_constant(const relic_rule_t *rule);
static int	 execute_plan(relic_evaluation_t *eval,
		    const relic_plan_t *plan, const relic_rule_t *rule,
		    const relic_emit_target_t *target);
static void	 count_derivation(relic_evaluation_t *eval, bool added);
static void	 decode_row(const relic_t *ctx, const relic_dom_t *tuple,
		    uint16_t arity, relic_value_t *row);
static void	 deliver_batch(relic_evaluation_t *eval,
		    const relic_row_sink_t *sink, uint16_t arity);
static int	 insert_batch(relic_evaluation_t *eval,
		    relic_relation_t *relation);
static const relic_relation_t *plan_head(const relic_plan_t *plan);
static relic_pending_t *make_pending_room(relic_evaluation_t *eval);
static int	 fit_scratch(relic_evaluation_t *eval,
		    const relic_plan_t *plan);
static void	 set_delta(relic_evaluation_t *eval, relic_relation_t *member,
		    relic_tuple_range_t delta);
static int	 take_batch(relic_evaluation_t *eval,
		    const relic_rule_t *rule,
		    const relic_emit_target_t *target);
static void	 take_counters(relic_t *ctx, relic_scratch_t *scratch);
static int	 insert_seed(relic_evaluation_t *eval,
		    const relic_rule_t *rule, relic_relation_t *relation);
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
		    const relic_mixed_note_t *note);
static void	 report_failure(relic_t *ctx, const relic_rule_t *rule,
		    const relic_relation_t *relation,
		    const relic_exec_error_t *error);
static void	 report_texts_refused(relic_t *ctx,
		    const relic_relation_t *relation,
		    enum relic_exec_failure failure);
static int	 resolve_pending(relic_evaluation_t *eval,
		    relic_relation_t *relation);
static const char *describe_fold(const relic_aggregate_t *aggregate,
		    char *buffer, size_t size);
static const char *type_phrase(relic_dom_t type);
static bool	 literal_reads_member(const relic_evaluation_t *eval,
		    const relic_literal_t *literal);
static void	 mark_members(relic_evaluation_t *eval,
		    const relic_schedule_t *schedule,
		    const relic_component_t *component, bool is_member);
static int	 make_running_plan(relic_evaluation_t *eval,
		    const relic_rule_t *rule, const relic_plan_t *plan,
		    relic_relation_t *target, relic_running_plan_t **runningp);
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
static bool	 query_streams(const relic_t *ctx, const relic_plan_t *plan);
static int	 run_query_rule(relic_t *ctx, const relic_rule_t *rule,
		    relic_relation_t *answer, const relic_group_run_t *groups,
		    const relic_row_sink_t *rows);
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
static int	 gather_variant_round(relic_evaluation_t *eval,
		    relic_component_plans_t *plans, relic_round_t *round);
static int	 run_item(relic_evaluation_t *eval, const relic_round_t *round,
		    relic_round_item_t *item);
static bool	 rounds_may_be_deferred(const relic_t *ctx);
static int	 run_round(relic_evaluation_t *eval, relic_round_t *round);
static uint64_t	 counted_work(const relic_t *ctx);
static void	 begin_round(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    relic_round_t *round);
static void	 add_item(const relic_evaluation_t *eval, relic_round_t *round,
		    relic_running_plan_t *running, const relic_rule_t *rule);
static bool	 record_round_gains(const relic_round_t *round,
		    relic_delta_work_t *work);
static int	 gather_base_pass(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    relic_round_t *round);
static int	 allocate_round_items(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static const relic_plan_draft_t *variant_any_draft(
		    const relic_variant_t *variant);
static int	 semi_naive_fixed_point(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static void	 start_deltas(relic_evaluation_t *eval,
		    relic_component_plans_t *plans);
static bool	 choose_query_target(const relic_t *ctx,
		    relic_running_plan_t *running,
		    const relic_row_sink_t *rows);
static int	 deliver_gathered_answer(relic_t *ctx,
		    const relic_relation_t *answer,
		    const relic_row_sink_t *sink);
static int	 stream_query_plan(relic_evaluation_t *eval,
		    const relic_running_plan_t *running);
static int	 gather_query_answer(relic_evaluation_t *eval,
		    const relic_running_plan_t *running);
static int	 prepare_candidate(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    relic_variant_t *variant, size_t candidate);
static void	 raise_read_members(relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    const relic_plan_draft_t *draft);
static bool	 is_component_member(const relic_evaluation_t *eval,
		    const relic_component_plans_t *plans,
		    const relic_relation_t *relation);
static void	 raise_indexes(relic_relation_t *member,
		    relic_tuple_id_t since);

/*
 * A type, given as a domain identifier's type bits, as a note names it,
 * with its article.
 */
static const char *
type_phrase(relic_dom_t type)
{
	switch (type) {
	case RELIC_DOM_INTEGER:
		return ("an integer");
	case RELIC_DOM_STRING:
		return ("a string");
	case RELIC_DOM_SYMBOL:
		return ("a symbol");
	}

	return ("an unknown value");
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
 * Report a comparison or assignment of a rule that was false only because of
 * a value's type, unless the rule it was written as has had one reported
 * already.  Nothing is allocated, so asking for the note cannot make an
 * evaluation fail.
 */
static void
note_mixed_comparison(relic_t *ctx, const relic_rule_t *rule,
    const relic_mixed_note_t *note)
{
	char folded[RELIC_NAME_TEXT_MAX + 8];
	bool *noted;

	noted = mixed_noted_flag(ctx, rule);
	if (*noted)
		return;
	*noted = true;

	switch (note->kind) {
	case RELIC_NOTE_ORDERING:
		relic_diag_note(ctx, note->where,
		    "ordering comparison between %s and %s is false",
		    type_phrase(note->left_type),
		    type_phrase(note->right_type));
		return;
	case RELIC_NOTE_OPERAND:
		relic_diag_note(ctx, note->where,
		    "operand of %s is %s; the comparison is false",
		    note->operator, type_phrase(note->left_type));
		return;
	case RELIC_NOTE_SUM_TYPE:
		relic_diag_note(ctx, note->where,
		    "a row of %s holds a value that is not an integer; the "
		    "aggregate has no value", describe_fold(note->aggregate,
		    folded, sizeof(folded)));
		return;
	case RELIC_NOTE_EXTREME_TYPES:
		relic_diag_note(ctx, note->where,
		    "the rows of %s hold values of more than one type; the "
		    "aggregate has no value", describe_fold(note->aggregate,
		    folded, sizeof(folded)));
		return;
	}
}

/*
 * An aggregate's function and aggregated variable as the rule writes them,
 * sum(Size) or min(N), for a note about its fold.  The variable is the
 * compiled body's, whose slot is the aggregated column; its spelling is all
 * identifier bytes, so it is safe to quote.
 */
static const char *
describe_fold(const relic_aggregate_t *aggregate, char *buffer, size_t size)
{
	const relic_variable_t *variable;
	char named[RELIC_NAME_TEXT_MAX];

	variable = &aggregate->body->variables[aggregate->column];

	snprintf(buffer, size, "%s(%s)",
	    relic_aggregate_spelling(aggregate->fn),
	    relic_diag_name(variable->name.bytes, named, sizeof(named)));

	return (buffer);
}

/*
 * Report the failure a plan of `rule` recorded while deriving into
 * `relation`.  A result that is not a value, or a pattern that is not a
 * regular expression, names the program rule it came from, or the query,
 * at the literal that met it; a pattern's message then says why it is not
 * one.  A buffer the memory limit would not let grow is reported as the
 * limit refusing an insertion into `relation` is, and one the allocator
 * would not give as every allocation failure is.
 */
static void
report_failure(relic_t *ctx, const relic_rule_t *rule,
    const relic_relation_t *relation, const relic_exec_error_t *error)
{
	char described[RELIC_PRED_TEXT_MAX];
	const char *separator;
	const char *detail;
	const char *what;

	what = "integer overflow";
	separator = "";
	detail = "";
	switch (error->failure) {
	case RELIC_FAIL_OVERFLOW:
		break;
	case RELIC_FAIL_ZERO_DIVISOR:
		what = "division by zero";
		break;
	case RELIC_FAIL_TOO_LONG:
		what = "string too long";
		break;
	case RELIC_FAIL_PATTERN:
		what = "invalid regular expression";
		separator = ": ";
		detail = relic_regex_message(error->pattern);
		break;
	case RELIC_FAIL_LIMIT:
	case RELIC_FAIL_NO_MEMORY:
		report_texts_refused(ctx, relation, error->failure);
		return;
	}

	if (rule->origin == RELIC_ORIGIN_QUERY) {
		relic_diag_error(ctx, RELIC_ERROR_EVALUATION, error->where,
		    "%s in the query%s%s", what, separator, detail);
		return;
	}

	relic_diag_error(ctx, RELIC_ERROR_EVALUATION, error->where,
	    "%s in rule %s%s%s", what, relic_pred_describe(ctx,
	    ctx->program.rules.entries[rule->origin].head, described,
	    sizeof(described)), separator, detail);
}

/*
 * Report that a buffer for the strings a round's plans compute could not
 * be had, for `failure`: the memory limit refusing it, as it refuses an
 * insertion into `relation`, or the allocator.
 */
static void
report_texts_refused(relic_t *ctx, const relic_relation_t *relation,
    enum relic_exec_failure failure)
{
	char described[RELIC_PRED_TEXT_MAX];

	if (failure != RELIC_FAIL_LIMIT) {
		relic_diag_no_memory(ctx);
		return;
	}

	relic_diag_limit(ctx, "memory", ctx->account.limit, "deriving",
	    relic_relation_describe(ctx, relation, described,
	    sizeof(described)));
}

/*
 * Give the values a batch derived into `relation` holds pending their
 * identifiers, interning those the context has not assigned, and write the
 * identifiers over the markers, so that the batch is a batch of tuples
 * again.  The memory limit is asked about the growth of the value table
 * and the pool before anything is interned, so a batch it refuses leaves
 * them as they were.  Returns 0, or -1 with the refusal reported.  The
 * pending strings' bytes are read until the commit has copied them, and
 * the batch's pending values are empty after it either way.
 */
static int
resolve_pending(relic_evaluation_t *eval, relic_relation_t *relation)
{
	relic_scratch_t *scratch;
	size_t projected;
	int status;

	scratch = &eval->scratch;
	if (scratch->pending_count == 0)
		return (0);

	projected = relic_pending_measure(eval->ctx, scratch->pending,
	    scratch->pending_count, scratch->pending_bytes.bytes,
	    &eval->pending_probe, &eval->pending_map);
	status = relic_relation_admit_values(eval->ctx, relation, projected);
	if (status == 0) {
		relic_pending_rewrite(&eval->pending_map, scratch->batch,
		    scratch->batch_count, relation->arity);
		status = relic_pending_commit(eval->ctx, &eval->pending_map);
	}

	scratch->pending_count = 0;
	scratch->pending_bytes.used = 0;

	return (status);
}

/*
 * The values of a tuple, as the row a callback receives.  Every column
 * decodes, because every column holds an identifier the context assigned:
 * columns are copied only from constants the parser interned and from other
 * tuples.
 */
static void
decode_row(const relic_t *ctx, const relic_dom_t *tuple, uint16_t arity,
    relic_value_t *row)
{
	uint16_t column;
	bool known RELIC_UNUSED;

	for (column = 0; column < arity; column++) {
		known = relic_value_decode(ctx, tuple[column], &row[column]);
		assert(known);
	}
}

/*
 * Deliver the batch to the caller, one row per tuple in the order they were
 * derived, and empty it.  Each is counted as a derivation and not as an
 * inserted tuple, since nothing was inserted.  A callback that asks for no
 * more rows gets none: the rest of the batch is dropped, and `stopped` ends
 * the plan.
 */
static void
deliver_batch(relic_evaluation_t *eval, const relic_row_sink_t *sink,
    uint16_t arity)
{
	relic_value_t row[RELIC_MAX_ARITY];
	relic_scratch_t *scratch;
	size_t i;

	scratch = &eval->scratch;

	for (i = 0; i < scratch->batch_count && !eval->stopped; i++) {
		decode_row(eval->ctx, &scratch->batch[i * arity], arity, row);
		count_derivation(eval, false);

		if (sink->callback(sink->callback_ctx, row, arity) != 0)
			eval->stopped = true;
	}

	scratch->batch_count = 0;
}

/*
 * Insert the batch into `relation`, counting a derivation for each tuple
 * whose insertion completed and noting which were new, and empty it.
 * Returns -1 when an insertion was refused, which the relation store has
 * reported.
 */
static int
insert_batch(relic_evaluation_t *eval, relic_relation_t *relation)
{
	bool added[RELIC_INSERT_BATCH];
	relic_scratch_t *scratch;
	size_t completed;
	size_t i;
	int status;

	scratch = &eval->scratch;
	if (scratch->batch_count == 0)
		return (0);

	status = relic_relation_insert_batch(eval->ctx, relation,
	    scratch->batch, scratch->batch_count, added, &completed);

	for (i = 0; i < completed; i++)
		count_derivation(eval, added[i]);
	scratch->batch_count = 0;

	return (status);
}

/*
 * Reduce the batch into the target's reducer, counting a derivation for
 * each tuple the reducer took, none of them inserted, and empty it.  Its
 * pending values are read where the batch's markers name them: the
 * reducer keeps what it retains of them, and they are not interned until
 * the reduction ends.  Returns -1 when the reducer refused, which it has
 * reported.
 */
static int
reduce_batch(relic_evaluation_t *eval, const relic_emit_target_t *target)
{
	relic_reduce_batch_t batch;
	relic_scratch_t *scratch;
	size_t taken;
	size_t i;
	int status;

	scratch = &eval->scratch;
	batch.rows = scratch->batch;
	batch.count = scratch->batch_count;
	batch.pending = scratch->pending;
	batch.pending_count = scratch->pending_count;
	batch.bytes = scratch->pending_bytes.bytes;
	batch.byte_count = scratch->pending_bytes.used;
	status = relic_reducer_take(eval->ctx, target->reducer,
	    target->relation, &batch, &taken);

	for (i = 0; i < taken; i++)
		count_derivation(eval, false);
	scratch->batch_count = 0;
	scratch->pending_count = 0;
	scratch->pending_bytes.used = 0;

	return (status);
}

/*
 * Add the work the executor counted to the context's counters, and start
 * counting afresh.
 */
static void
take_counters(relic_t *ctx, relic_scratch_t *scratch)
{
	relic_exec_counters_add(&ctx->counters.read, &scratch->counters);
	bzero(&scratch->counters, sizeof(scratch->counters));
}

/*
 * A deferred round's merge reports each task's note through here, as
 * take_batch() reports a batch's, so the two ways a round runs report the
 * same note for the same rule.
 */
void
relic_report_mixed_note(relic_t *ctx, const relic_rule_t *rule,
    relic_mixed_note_t *note)
{
	if (note->where == NULL)
		return;

	note_mixed_comparison(ctx, rule, note);
	note->where = NULL;
}

/*
 * Take what the executor has gathered for a plan of `rule` since it was last
 * taken, and empty its batch: count its work, report the note it recorded,
 * and send its tuples to `target`, inserting them, their pending values
 * interned first, or delivering them as rows.  Returns -1 when the plan met
 * a failure, which is reported here and discards the batch, or when an
 * insertion or its values were refused, which the store has reported; a
 * delivery cannot fail.
 *
 * This is the one place that tells the kinds of target apart, so a tuple
 * is built the same way whichever it is for.  The note goes before
 * the tuples, since the comparison it names was made before the batch's
 * later tuples were derived, and the counts go first of all, so that a row
 * callback reading the statistics finds the work that derived its row.  A
 * plan that streams computes nothing, so a batch delivered as rows holds no
 * pending value and no failure.
 */
static int
take_batch(relic_evaluation_t *eval, const relic_rule_t *rule,
    const relic_emit_target_t *target)
{
	relic_scratch_t *scratch;

	scratch = &eval->scratch;
	take_counters(eval->ctx, scratch);
	relic_report_mixed_note(eval->ctx, rule, &scratch->note);

	if (scratch->error.failed) {
		report_failure(eval->ctx, rule, target->relation,
		    &scratch->error);
		bzero(&scratch->error, sizeof(scratch->error));
		scratch->batch_count = 0;
		scratch->pending_count = 0;
		scratch->pending_bytes.used = 0;
		return (-1);
	}

	if (target->reducer != NULL)
		return (reduce_batch(eval, target));

	if (target->rows.callback == NULL) {
		if (resolve_pending(eval, target->relation) != 0)
			return (-1);
		return (insert_batch(eval, target->relation));
	}

	assert(scratch->pending_count == 0);

	deliver_batch(eval, &target->rows, target->relation->arity);

	return (0);
}

/*
 * Count one completed insertion of a derived tuple, and note whether it was
 * new.  Every way a tuple is derived -- a batch of an EMIT's, a seed's turn
 * in a pass, and a fact seeded from a base store -- counts through here, so
 * they are counted alike.
 */
static void
count_derivation(relic_evaluation_t *eval, bool added)
{
	eval->ctx->counters.derivations++;
	if (added) {
		eval->ctx->counters.tuples_inserted++;
		eval->derived = true;
	}
}

/*
 * Every caller is at a point where its operation could already fail, so a
 * failure here leaves the context as any other failure there does.
 */
int
relic_refuse_interrupted(relic_t *ctx, const char *verb,
    const relic_relation_t *relation)
{
	char described[RELIC_PRED_TEXT_MAX];

	if (!relic_signal_flag_raised(&ctx->interrupted))
		return (0);

	relic_diag_error(ctx, RELIC_ERROR_INTERRUPTED, NULL,
	    "interrupted while %s %s", verb, relic_relation_describe(ctx,
	    relation, described, sizeof(described)));

	return (-1);
}

/*
 * Run a plan of `rule` to its end, its EMIT's tuples going to `target`:
 * every batch the executor fills is taken as it fills, and the last one
 * before the plan returns, so a caller reading `derived` reads the whole
 * plan's result.  Returns 0, or -1 when an insertion was refused, which the
 * relation store has reported, or the caller interrupted the evaluation;
 * what the refused batch held after the refused tuple is discarded, as the
 * failed evaluation discards everything else.  The plan ends early when the
 * caller it delivers rows to has asked for no more.
 *
 * The executor pauses at least every quantum of work, with its batch full
 * or not, and the interrupt is looked for at every return, the last
 * included: an evaluation of many plans that each finish within a quantum
 * is stopped between them as a long plan is stopped within one.  A caller
 * that has asked for no more rows has had its answer, and the plan ends
 * with success whether or not an interrupt came too, as a gathered answer's
 * delivery does.
 */
static int
execute_plan(relic_evaluation_t *eval, const relic_plan_t *plan,
    const relic_rule_t *rule, const relic_emit_target_t *target)
{
	enum relic_exec_status status;

	relic_exec_start(&eval->scratch, &eval->env, plan);

	do {
		status = relic_exec_run(&eval->scratch);
		if (take_batch(eval, rule, target) != 0)
			return (-1);
		if (eval->stopped)
			return (0);
		if (relic_refuse_interrupted(eval->ctx, "deriving",
		    target->relation) != 0)
			return (-1);
	} while (status == RELIC_EXEC_PAUSED);

	return (0);
}

/*
 * A seed's turn in a pass: insert its head, every argument of which is a
 * constant, into `relation`, and count it as the batch of a plan's EMIT
 * counts its tuples.
 * Returns -1 when the insertion was refused, which the relation store has
 * reported.
 */
static int
insert_seed(relic_evaluation_t *eval, const relic_rule_t *rule,
    relic_relation_t *relation)
{
	relic_dom_t tuple[RELIC_MAX_ARITY];
	uint16_t column;
	bool added;

	for (column = 0; column < rule->head_arity; column++)
		tuple[column] = rule->head_args[column].u.constant;

	if (relic_relation_insert(eval->ctx, relation, tuple, &added) != 0)
		return (-1);

	count_derivation(eval, added);

	return (0);
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

	relic_diag_internal(eval->ctx, &rule->where,
	    "a rule's plans are out of step with the program");

	return (-1);
}

/*
 * Run a round's items in order, each inserting what it derives into its
 * target, and mark each item whose turn inserted a tuple its target did
 * not hold.  Returns 0, or -1 when an insertion was refused, which the
 * relation store has reported.
 *
 * On a context with threads, or under RELIC_F_PARALLEL_ALWAYS, every round
 * is offered for deferral: its items run over a store nothing writes until
 * they have all finished, on as many threads as the pool has, and what
 * they derived is then inserted in the order they would have inserted it
 * (lib/round.c).  A round too small to repay the threads, one that reads
 * what it writes, or one that could not go on is declined before it has
 * changed anything, and runs here, inserting as it derives.  Either way
 * the relations, the counters and what is reported come out the same.  A
 * context of one thread without the switch offers nothing, and its rounds
 * run here as they always have.
 *
 * A round run here whose plans call a function has the scratch's buffers
 * for the strings they compute for as long as it runs, charged to the
 * memory account from its start to its end, as a deferred round's are.
 */
static int
run_round(relic_evaluation_t *eval, relic_round_t *round)
{
	const relic_round_item_t *calling;
	enum relic_exec_failure failure;
	int status;
	size_t i;

	if (rounds_may_be_deferred(eval->ctx)) {
		switch (relic_round_run(eval->ctx, round)) {
		case RELIC_ROUND_MERGED:
			return (0);
		case RELIC_ROUND_FAILED:
			return (-1);
		case RELIC_ROUND_DECLINED:
			break;
		}
	}

	calling = relic_round_first_call(round);
	if (calling != NULL &&
	    relic_scratch_begin_texts(&eval->scratch, &eval->ctx->account,
	    &failure) != 0) {
		report_texts_refused(eval->ctx, round->targets[calling->target],
		    failure);
		return (-1);
	}

	status = 0;
	for (i = 0; i < round->item_count && status == 0; i++)
		status = run_item(eval, round, &round->items[i]);

	relic_scratch_end_texts(&eval->scratch);

	return (status);
}

/*
 * An item's turn in a round that inserts as it derives: a seed inserts its
 * head and a plan runs, and the item is marked when its turn inserted a
 * tuple its target did not hold.  An item that keeps a history records the
 * work its turn did and the tuples it inserted.
 */
static int
run_item(relic_evaluation_t *eval, const relic_round_t *round,
    relic_round_item_t *item)
{
	relic_emit_target_t target;
	relic_driver_reads_t reads;
	uint64_t inserted_before;
	uint64_t work_before;
	int status;

	bzero(&target, sizeof(target));
	target.relation = round->targets[item->target];
	work_before = counted_work(eval->ctx);
	inserted_before = eval->ctx->counters.tuples_inserted;

	eval->derived = false;
	bzero(&reads, sizeof(reads));
	if (item->plan == NULL) {
		status = insert_seed(eval, item->rule, target.relation);
	} else {
		status = execute_plan(eval, item->plan, item->rule, &target);
		reads = eval->scratch.driver_reads;
	}
	item->gained = eval->derived;

	relic_round_record(item, counted_work(eval->ctx) - work_before,
	    &reads, eval->ctx->counters.tuples_inserted - inserted_before);

	return (status);
}

/*
 * Whether a context offers its rounds for deferral: it has threads, or
 * RELIC_F_PARALLEL_ALWAYS asks for every round to be deferred.
 */
static bool
rounds_may_be_deferred(const relic_t *ctx)
{
	return (ctx->config.threads > 1 ||
	    (ctx->config.flags & RELIC_F_PARALLEL_ALWAYS) != 0);
}

/*
 * The work a context has counted: the tuples it read and derived.
 */
static uint64_t
counted_work(const relic_t *ctx)
{
	return (relic_exec_work(ctx->counters.derivations,
	    &ctx->counters.read));
}

/*
 * Begin a round of a component's with no items: its targets are the
 * component's members, and its items are kept where the component keeps
 * them.
 */
static void
begin_round(relic_evaluation_t *eval, const relic_component_plans_t *plans,
    relic_round_t *round)
{
	round->items = plans->items;
	round->targets = plans->members;
	round->env = &eval->env;
	round->item_count = 0;
	round->item_capacity = plans->item_capacity;
	round->target_count = plans->member_count;
}

/*
 * Add an item to a round of the component being evaluated: a seed of
 * `rule` when `running` is NULL, and otherwise the running plan of it,
 * with the plan's history and its record of insertions on a context whose
 * rounds may be deferred.  Its target is the member the rule's head is,
 * and a component's round targets its members in the order of their
 * positions.
 */
static void
add_item(const relic_evaluation_t *eval, relic_round_t *round,
    relic_running_plan_t *running, const relic_rule_t *rule)
{
	relic_round_item_t *item;

	assert(round->item_count < round->item_capacity);
	item = &round->items[round->item_count];
	round->item_count++;

	item->plan = NULL;
	item->history = NULL;
	item->insertions = NULL;
	if (running != NULL) {
		item->plan = &running->plan;
		if (rounds_may_be_deferred(eval->ctx)) {
			item->history = &running->history;
			item->insertions = running->insertions;
		}
	}
	item->rule = rule;
	item->target = head_member_of(eval, rule);
	item->gained = false;
}

/*
 * Whether any item of a round gained a tuple, recording in `work`, when
 * there is one, the member each such item gained it in.
 */
static bool
record_round_gains(const relic_round_t *round, relic_delta_work_t *work)
{
	bool gained;
	size_t i;

	gained = false;
	for (i = 0; i < round->item_count; i++) {
		if (!round->items[i].gained)
			continue;

		gained = true;
		if (work != NULL)
			record_gain(work, round->items[i].target);
	}

	return (gained);
}

/*
 * Gather a pass over a component's rules, each once, in program order, into
 * a round: a seed, whose turn inserts its head, and each rule with a base
 * plan.  A recursive rule under the semi-naive fixed point has none and
 * waits for the rounds that run its variants.
 */
static int
gather_base_pass(relic_evaluation_t *eval,
    const relic_component_plans_t *plans, relic_round_t *round)
{
	const relic_rule_plans_t *rule_plans;
	const relic_rule_t *rule;
	size_t planned;
	size_t i;

	begin_round(eval, plans, round);
	planned = 0;

	for (i = 0; i < plans->rule_count; i++) {
		rule = &eval->rules->entries[plans->rule_indexes[i]];

		if (rule->body_length == 0) {
			add_item(eval, round, NULL, rule);
			continue;
		}

		if (confirm_planned(eval, plans, planned, rule) != 0)
			return (-1);
		rule_plans = &plans->planned[planned];
		planned++;
		if (rule_plans->base != NULL)
			add_item(eval, round, rule_plans->base, rule);
	}

	assert(planned == plans->planned_count);

	return (0);
}

/*
 * One pass over a component's rules, each once, in program order.  With
 * `work`, each member that gains a tuple is recorded for the rounds that
 * follow.  `derived` is left set exactly when something in the pass was
 * new.
 *
 * A pass of the naive fixed point reads the members as the rules before it
 * in the pass left them, so it is never deferred.  Every other pass reads
 * no relation it writes -- a component that is not recursive has no rule
 * reading one of its members, and the first pass of the semi-naive fixed
 * point runs only its exit rules, which read none -- and may be.
 */
static int
run_base_pass(relic_evaluation_t *eval, const relic_component_plans_t *plans,
    relic_delta_work_t *work)
{
	relic_round_t round;

	if (gather_base_pass(eval, plans, &round) != 0)
		return (-1);
	if (run_round(eval, &round) != 0)
		return (-1);

	eval->derived = record_round_gains(&round, work);

	return (0);
}

/*
 * Run passes over a component's plans until one inserts nothing new.
 *
 * Within a pass a rule sees what the rules before it inserted, rather than
 * only what was there when the pass began, and none of what it derives
 * itself, since its plan reads each relation as it stood when the plan
 * started.  That changes how many passes are needed and not where they end,
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
	static const relic_tuple_range_t empty = {
		RELIC_TUPLE_FIRST, RELIC_TUPLE_FIRST
	};
	relic_delta_work_t *work;
	size_t i;

	work = &plans->work;

	for (i = 0; i < plans->member_count; i++) {
		set_delta(eval, plans->members[i], empty);
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
 * Gather the variants due this round into a round, in the order they were
 * planned, each with the candidate its estimated work selects.  Returns 0,
 * or -1 with a diagnostic reported when a candidate could not be prepared,
 * which fails the evaluation.
 *
 * Every view of a variant is a member's old, delta or current view, fixed
 * by the deltas before the round begins, so the round reads nothing it
 * writes.  The round is gathered in three steps, none of which runs a
 * plan.  First every due variant's candidate is chosen, so that every
 * choice sees the same indexes: choosing builds nothing.  Then each chosen
 * candidate not yet prepared is prepared, in due order, which builds the
 * indexes it lacks and raises them to their members' deltas; another
 * candidate needing one of them finds it built.  Last the items are added,
 * so a
 * round never holds a plan whose indexes are missing.  A candidate once
 * prepared stays so, its indexes being kept and maintained like any other.
 */
static int
gather_variant_round(relic_evaluation_t *eval,
    relic_component_plans_t *plans, relic_round_t *round)
{
	relic_delta_work_t *work;
	relic_variant_t *variant;
	size_t due_count;
	size_t i;

	work = &plans->work;
	begin_round(eval, plans, round);
	due_count = gather_due_variants(plans);

	for (i = 0; i < due_count; i++) {
		variant = &plans->variants[work->due[i]];
		work->chosen[i] = relic_cost_choose(&variant->family,
		    &eval->env);
	}

	for (i = 0; i < due_count; i++) {
		variant = &plans->variants[work->due[i]];
		if (variant->running[work->chosen[i]] != NULL)
			continue;
		if (prepare_candidate(eval, plans, variant,
		    work->chosen[i]) != 0)
			return (-1);
	}

	for (i = 0; i < due_count; i++) {
		variant = &plans->variants[work->due[i]];
		add_item(eval, round, variant->running[work->chosen[i]],
		    variant->rule);
	}

	return (0);
}

/*
 * Prepare candidate `candidate` of a variant into what runs it: the plan of
 * its draft, which builds the indexes it reads and its relations lack, and
 * the record of insertions the variant's first candidate keeps, since every
 * candidate derives the same tuples.  Returns 0, or -1 with a diagnostic
 * reported, leaving the candidate unprepared.
 *
 * An index built here between rounds is raised to its member's delta
 * before the plan can run, so it knows the start of every delta lookup made
 * from it, as set_delta() ensures for the indexes a member has when its
 * delta is set.  An index built when the component is planned is raised by
 * start_deltas(), its deltas not yet being set, which makes the raise here
 * change nothing.
 */
static int
prepare_candidate(relic_evaluation_t *eval,
    const relic_component_plans_t *plans, relic_variant_t *variant,
    size_t candidate)
{
	const relic_plan_draft_t *draft;
	relic_running_plan_t *running;
	relic_plan_t plan;

	assert(candidate == RELIC_FAMILY_DELTA_FIRST ||
	    variant->running[RELIC_FAMILY_DELTA_FIRST] != NULL);

	draft = &variant->family.candidates[candidate].draft;
	if (relic_plan_prepare(eval->ctx, eval->arena, draft, &plan) != 0)
		return (-1);
	raise_read_members(eval, plans, draft);

	if (make_running_plan(eval, variant->rule, &plan,
	    head_relation_of(eval, plans, variant->rule), &running) != 0)
		return (-1);

	if (candidate != RELIC_FAMILY_DELTA_FIRST)
		running->insertions =
		    variant->running[RELIC_FAMILY_DELTA_FIRST]->insertions;
	variant->running[candidate] = running;

	return (0);
}

/*
 * Raise the indexes of every member of the component a draft reads through
 * an index to the start of the member's delta.  A relation the draft reads
 * outside the component has no delta, and is left alone.
 */
static void
raise_read_members(relic_evaluation_t *eval,
    const relic_component_plans_t *plans, const relic_plan_draft_t *draft)
{
	relic_relation_t *relation;
	size_t i;

	for (i = 0; i < draft->access_count; i++) {
		relation = draft->accesses[i].relation;
		if (is_component_member(eval, plans, relation))
			raise_indexes(relation,
			    eval->deltas[relation->pred - 1].first);
	}
}

/*
 * Whether a relation is a member of the component being evaluated: the
 * member at its predicate's position, which for any other predicate is
 * zero or a position in another component.  A base store shares its
 * predicate with the derived relation and is not the member.
 */
static bool
is_component_member(const relic_evaluation_t *eval,
    const relic_component_plans_t *plans, const relic_relation_t *relation)
{
	size_t position;

	assert(relation->pred != RELIC_ID_NONE &&
	    relation->pred <= eval->predicate_count);
	position = eval->member_position[relation->pred - 1];

	return (position < plans->member_count &&
	    plans->members[position] == relation);
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
	relic_relation_t *relation;
	relic_tuple_range_t delta;
	bool old_was_empty;
	size_t i;

	work = &plans->work;
	relation = plans->members[member];
	delta = eval->deltas[relation->pred - 1];

	old_was_empty = delta.first == RELIC_TUPLE_FIRST;
	delta = relic_relation_since(relation, delta.end);
	set_delta(eval, relation, delta);
	if (!old_was_empty || delta.first == RELIC_TUPLE_FIRST)
		return;

	for (i = work->first_old_reader[member];
	    i < work->first_old_reader[member + 1]; i++)
		plans->variants[work->old_readers[i]].empty_old_views--;
}

/*
 * Set a member's delta, and raise every index of the member to the delta's
 * start, which is where every lookup of the delta begins: a delta view reads
 * only its own member, from the delta's first identifier, and a delta is
 * fixed until it is next set.  So an index knows the start before any
 * lookup is made from it, and a lookup needs to write nothing but a key's
 * frontier.  An index built while a delta holds is raised to it by
 * prepare_candidate(), the one place such an index is built.
 *
 * Once an evaluation has begun, this is the only place a delta is written,
 * which is what makes every lookup of a delta one its index was raised for:
 * a delta set anywhere else could be looked up from a start its index had
 * never heard of, which relic_index_first_since() answers correctly and
 * slowly.
 */
static void
set_delta(relic_evaluation_t *eval, relic_relation_t *member,
    relic_tuple_range_t delta)
{
	eval->deltas[member->pred - 1] = delta;
	raise_indexes(member, delta.first);
}

/*
 * Raise every index of a member to a delta start `since`.
 */
static void
raise_indexes(relic_relation_t *member, relic_tuple_id_t since)
{
	size_t position;

	for (position = 0; position < member->index_count; position++)
		relic_index_raise_since(&member->indexes[position], since);
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
 * Run round 0 of a component -- its seeds and exit rules -- then its delta
 * variants round after round until a round inserts nothing.  Each round runs
 * only the variants that can derive something, in planning order.
 */
static int
semi_naive_fixed_point(relic_evaluation_t *eval,
    relic_component_plans_t *plans)
{
	relic_delta_work_t *work;
	relic_round_t round;

	work = &plans->work;

	start_deltas(eval, plans);

	eval->ctx->counters.iterations++;
	if (run_base_pass(eval, plans, work) != 0)
		return (-1);

	while (work->gained_count > 0) {
		eval->ctx->counters.iterations++;

		advance_deltas(eval, plans);
		if (gather_variant_round(eval, plans, &round) != 0)
			return (-1);
		if (run_round(eval, &round) != 0)
			return (-1);
		record_round_gains(&round, work);
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

	block = relic_arena_allocarray(eval->arena, count, size);
	if (block == NULL)
		relic_diag_no_memory(eval->ctx);

	return (block);
}

/*
 * Begin an evaluation of `component_count` components, or of one query,
 * whose plans write heads of at most `head_arity_max` columns, allocating
 * from `arena`.  Returns 0,
 * or -1 with a diagnostic reported.  Either way the caller ends the
 * evaluation with evaluation_end(), which releases what was allocated.
 *
 * The scratch's batch is sized here, once, for the widest head, so every
 * plan the evaluation builds has room for its tuples; a head of no columns
 * is given a column of room, so the batch always exists.  Its states, and
 * its room for pending integers, are sized as plans are built, by
 * fit_scratch(), since only the plans of the components an evaluation
 * reaches need room, and only a plan that computes needs pending integers.
 */
static int
evaluation_begin(relic_evaluation_t *eval, relic_t *ctx, relic_arena_t *arena,
    size_t component_count, uint16_t head_arity_max)
{
	relic_dom_t *batch;
	uint16_t arity_room;

	bzero(eval, sizeof(*eval));
	eval->ctx = ctx;
	eval->arena = arena;
	eval->env.ctx = ctx;
	eval->env.note_mixed = (ctx->config.flags & RELIC_F_NOTE_MIXED) != 0;

	eval->components = evaluation_alloc(eval, component_count,
	    sizeof(*eval->components));
	if (eval->components == NULL)
		return (-1);
	bzero(eval->components, component_count * sizeof(*eval->components));
	eval->component_count = component_count;

	arity_room = head_arity_max > 0 ? head_arity_max : 1;
	batch = evaluation_alloc(eval, (size_t)RELIC_INSERT_BATCH * arity_room,
	    sizeof(*batch));
	if (batch == NULL)
		return (-1);

	relic_scratch_init(&eval->scratch, NULL, 0, batch, NULL, arity_room);

	return (0);
}

/*
 * The widest head among `rules`, which bounds the head of every plan built
 * from them.
 */
static uint16_t
widest_head(const relic_ruletab_t *rules)
{
	uint16_t widest;
	size_t i;

	widest = 0;
	for (i = 0; i < rules->count; i++) {
		if (rules->entries[i].head_arity > widest)
			widest = rules->entries[i].head_arity;
	}

	return (widest);
}

/*
 * Give the evaluation the per-predicate delta ranges and membership marks
 * its fixed points need.  Returns 0, or -1 with a diagnostic reported.
 *
 * Each allocation is checked before the next is made, so that the work
 * stops at the first refusal rather than asking more of an arena whose
 * failure is sticky.
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
	eval->env.deltas = eval->deltas;

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
	bzero(eval->member_position, eval->predicate_count *
	    sizeof(*eval->member_position));

	return (0);
}

/*
 * Plan one rule's ordinary plan into *runningp, its EMIT writing into
 * `target`.  Returns 0, or -1 with a diagnostic reported.
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
	if (relic_plan_rule(eval->ctx, eval->arena, rule, target,
	    eval->is_member, &plan) != 0)
		return (-1);

	return (make_running_plan(eval, rule, &plan, target, runningp));
}

/*
 * Give the scratch room for the states of `plan`'s operations, when it has
 * less, and, when `plan` computes and the scratch has no room for pending
 * values yet, room for a batch's worth of them and the map that resolves
 * them.  Returns 0, or -1 with the arena's refusal reported.
 *
 * Plans are built before any of them runs -- a component's when it starts
 * or, for a variant's second candidate, between two of its rounds, and a
 * query's before it is answered -- so no plan is running on the scratch
 * when its storage is replaced.  relic_scratch_init() starts the scratch
 * afresh, forgetting its counts and its note, which have been taken: every
 * run of a plan is followed by take_batch().  The scratch grows to the
 * longest plan the evaluation builds and no further, and at most once per
 * plan; the pending room, once, sized for the widest head, as the batch is.
 */
static int
fit_scratch(relic_evaluation_t *eval, const relic_plan_t *plan)
{
	relic_scratch_t *scratch;
	relic_op_state_t *states;
	relic_pending_t *pending;
	size_t op_room;

	scratch = &eval->scratch;
	states = scratch->states;
	op_room = scratch->op_room;
	pending = scratch->pending;

	if (plan->op_count > op_room) {
		states = evaluation_alloc(eval, plan->op_count,
		    sizeof(*states));
		if (states == NULL)
			return (-1);
		op_room = plan->op_count;
	}

	if (pending == NULL && relic_plan_computes(plan)) {
		pending = make_pending_room(eval);
		if (pending == NULL)
			return (-1);
	}

	if (states != scratch->states || pending != scratch->pending)
		relic_scratch_init(scratch, states, op_room, scratch->batch,
		    pending, scratch->arity_room);

	return (0);
}

/*
 * Room for the pending values of a batch of the scratch's width, and the
 * map that resolves them and the probe that measures them: one pending
 * value per column at most.  Returns the pending array, or NULL with the
 * arena's refusal reported.
 */
static relic_pending_t *
make_pending_room(relic_evaluation_t *eval)
{
	relic_pending_t *pending;
	size_t columns;

	columns = (size_t)RELIC_INSERT_BATCH * eval->scratch.arity_room;
	pending = evaluation_alloc(eval, columns, sizeof(*pending));
	if (pending == NULL)
		return (NULL);

	if (relic_pending_map_init(&eval->pending_map, eval->arena,
	    columns) != 0) {
		relic_diag_no_memory(eval->ctx);
		return (NULL);
	}
	if (relic_pending_probe_init(&eval->pending_probe, eval->arena,
	    columns) != 0) {
		relic_diag_no_memory(eval->ctx);
		return (NULL);
	}

	return (pending);
}

/*
 * The relation `plan` writes: the one its last operation, the EMIT, names.
 */
static const relic_relation_t *
plan_head(const relic_plan_t *plan)
{
	return (plan->ops[plan->op_count - 1].u.atom.relation);
}

/*
 * Pair a plan built for `rule` with `target`, the relation its EMIT names,
 * in *runningp.  Every plan an evaluation runs is made here, so this is
 * where it is confirmed to write the relation it is paired with and to fit
 * the evaluation's batch: the EMIT packs its tuples at the arity of the
 * relation it names and the batch is taken at the target's, and a head
 * wider than the batch's room would have its tuples written past the
 * batch.  The scratch's states are grown here to hold the plan's.
 */
static int
make_running_plan(relic_evaluation_t *eval, const relic_rule_t *rule,
    const relic_plan_t *plan, relic_relation_t *target,
    relic_running_plan_t **runningp)
{
	relic_running_plan_t *running;

	*runningp = NULL;
	if (plan_head(plan) != target) {
		relic_diag_internal(eval->ctx, &rule->where,
		    "a rule's plan writes other than the relation it was "
		    "planned for");
		return (-1);
	}
	if (target->arity > eval->scratch.arity_room) {
		relic_diag_internal(eval->ctx, &rule->where,
		    "a rule's plan does not fit the scratch it runs in");
		return (-1);
	}

	if (fit_scratch(eval, plan) != 0)
		return (-1);

	running = evaluation_alloc(eval, 1, sizeof(*running));
	if (running == NULL)
		return (-1);

	running->plan = *plan;
	running->rule = rule;
	bzero(&running->target, sizeof(running->target));
	running->target.relation = target;
	bzero(&running->history, sizeof(running->history));
	bzero(&running->own_insertions, sizeof(running->own_insertions));
	running->insertions = &running->own_insertions;

	*runningp = running;
	return (0);
}

/*
 * Release the plans and the scratch.  Resetting the arena also clears a
 * failure it recorded, which this evaluation has already reported.
 */
static void
evaluation_end(relic_evaluation_t *eval)
{
	relic_arena_reset(eval->arena);
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
 * Resolve the relations of a component's members: the derived relations its
 * rules write.  Every member is derived, since a component is scheduled
 * only for the rules deriving it.
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
		plans->members[i] = relic_relation_written(eval->ctx,
		    schedule->members[component->first_member + i]);
		if (plans->members[i] == NULL)
			return (-1);
		plans->member_count++;
	}

	return (0);
}

/*
 * Whether every argument of a seed's head is a constant, which is what lets
 * its turn in a pass insert the head's constants with no plan.
 */
static bool
seed_head_is_constant(const relic_rule_t *rule)
{
	uint16_t column;

	for (column = 0; column < rule->head_arity; column++) {
		if (rule->head_args[column].kind != RELIC_ARG_CONSTANT)
			return (false);
	}

	return (true);
}

/*
 * Confirm that a seed's head is constants alone, as many as the relation it
 * goes into has columns.  A checked rule always passes -- safety rejects a
 * variable or `_` in the head of a rule with no body -- and the question is
 * asked anyway, once per seed, because a failure would not be silent
 * otherwise: a variable's slot number would be inserted as though it were a
 * constant's identifier.
 */
static int
confirm_seed(const relic_evaluation_t *eval,
    const relic_component_plans_t *plans, const relic_rule_t *rule)
{
	char described[RELIC_PRED_TEXT_MAX];

	if (head_relation_of(eval, plans, rule)->arity == rule->head_arity &&
	    seed_head_is_constant(rule))
		return (0);

	relic_diag_internal(eval->ctx, &rule->where,
	    "a seed of %s holds something other than one constant per column",
	    relic_pred_identify(eval->ctx, rule->head, described,
	    sizeof(described)));

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
 * `delta_literal`: the family of candidates a round chooses between, and
 * what runs its first, the delta-first plan or the only one.  A second
 * candidate is prepared when a round first chooses it, so that the indexes
 * only it reads are not built for a plan no round runs.  The variant holds
 * no candidate until its first has what runs it.
 */
static int
plan_variant(relic_evaluation_t *eval, const relic_component_plans_t *plans,
    const relic_rule_t *rule, size_t delta_literal, relic_variant_t *variant)
{
	relic_plan_family_t family;

	bzero(variant, sizeof(*variant));
	variant->rule = rule;
	variant->delta_literal = delta_literal;

	if (relic_plan_variant(eval->ctx, eval->arena, rule,
	    head_relation_of(eval, plans, rule), delta_literal,
	    eval->is_member, &family) != 0)
		return (-1);

	variant->family = family;
	if (prepare_candidate(eval, plans, variant,
	    RELIC_FAMILY_DELTA_FIRST) != 0) {
		bzero(&variant->family, sizeof(variant->family));
		return (-1);
	}

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
 * Plan every rule of a component, in program order: confirm that a seed can
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
			if (confirm_seed(eval, plans, rule) != 0)
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
 * Allocate the sets of a component's delta work, stopping at the first the
 * arena refuses, since a refused request leaves the arena refusing.
 */
static int
allocate_delta_work(relic_evaluation_t *eval, relic_delta_work_t *work,
    size_t variants, size_t members)
{
	work->due = evaluation_alloc(eval, variants, sizeof(*work->due));
	if (work->due == NULL)
		return (-1);

	work->chosen = evaluation_alloc(eval, variants,
	    sizeof(*work->chosen));
	if (work->chosen == NULL)
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
 * The draft of one of a variant's candidates, it does not matter which.
 */
static const relic_plan_draft_t *
variant_any_draft(const relic_variant_t *variant)
{
	return (&variant->family.candidates[0].draft);
}

/*
 * List, in planning order, every variant under each member one of its
 * operations reads through the old view, once per such operation, and group
 * them.  The list is read off the drafts themselves, so the count of empty
 * old views a variant waits on is exactly what its plans read; both of a
 * variant's candidates read every literal through the same view, so either
 * serves.
 */
static int
group_by_old_member(relic_evaluation_t *eval, relic_component_plans_t *plans)
{
	relic_member_entry_t *entries;
	const relic_plan_draft_t *plan;
	const relic_op_t *op;
	size_t count;
	size_t i;
	size_t j;

	count = 0;
	for (i = 0; i < plans->variant_count; i++) {
		plan = variant_any_draft(&plans->variants[i]);
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
		plan = variant_any_draft(&plans->variants[i]);
		for (j = 0; j < plan->op_count; j++) {
			op = &plan->ops[j];
			if (!op_reads_old(op))
				continue;

			entries[count].member = eval->member_position[
			    relic_op_atom(op)->relation->pred - 1];
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
	const relic_op_atom_t *atom;

	atom = relic_op_atom(op);

	return (atom != NULL && atom->view == RELIC_VIEW_OLD);
}

/*
 * Prepare what a semi-naive round needs to run only the variants that can
 * derive something, and to choose each one's plan: the variants grouped by
 * the member their delta literal reads and by each member they read
 * through the old view, and room for the sets a round keeps.  The members
 * must be marked.
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
	if (status == 0)
		status = allocate_round_items(eval, plans);

	mark_members(eval, schedule, component, false);

	return (status);
}

/*
 * Give a component room for the items of its largest round: a pass over its
 * rules has at most one per rule, and a semi-naive round at most one per
 * variant.
 */
static int
allocate_round_items(relic_evaluation_t *eval, relic_component_plans_t *plans)
{
	size_t room;

	room = plans->rule_count > plans->variant_count ?
	    plans->rule_count : plans->variant_count;

	plans->items = evaluation_alloc(eval, room, sizeof(*plans->items));
	if (plans->items == NULL)
		return (-1);
	plans->item_capacity = room;

	return (0);
}

/*
 * Show a component's plans through the explain callback, if one is installed:
 * its heading, how many facts it has -- its seeds, the rules without a plan,
 * and the facts its members' base stores hold -- and each other rule with its
 * plan or its delta variants.
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
	for (i = 0; i < plans->member_count; i++)
		facts += relic_base_count(eval->ctx, plans->members[i]->pred);

	relic_explain_component(eval->ctx, schedule, position, facts, depth);

	for (i = 0; i < plans->planned_count; i++) {
		rule_plans = &plans->planned[i];

		relic_explain_rule(eval->ctx, rule_plans->rule,
		    rule_plans->base == NULL ? NULL : &rule_plans->base->plan,
		    depth);

		for (j = 0; j < rule_plans->variant_count; j++) {
			variant = &rule_plans->variants[j];
			relic_explain_variant(eval->ctx, rule_plans->rule,
			    variant->delta_literal, &variant->family, depth);
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
	if (seed_members(eval, plans) != 0)
		return (-1);

	switch (plans->fixed_point) {
	case RELIC_FIXED_ONE_PASS:
		return (run_base_pass(eval, plans, NULL));
	case RELIC_FIXED_NAIVE:
		return (naive_fixed_point(eval, plans));
	case RELIC_FIXED_SEMI_NAIVE:
		return (semi_naive_fixed_point(eval, plans));
	}

	relic_diag_internal(eval->ctx, NULL,
	    "a component was planned with no way to reach its fixed point");

	return (-1);
}

/*
 * Seed each member of a component with the facts its base store holds,
 * before the component's first pass: the whole store, in identifier order,
 * inserted in batches as a plan's EMIT inserts them and counted the same
 * way.  The semi-naive fixed point records the members' first deltas after
 * this, so every seeded tuple is a gain of round 0.
 */
static int
seed_members(relic_evaluation_t *eval, const relic_component_plans_t *plans)
{
	size_t i;

	for (i = 0; i < plans->member_count; i++) {
		if (seed_member(eval, plans->members[i]) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Insert every fact of `derived`'s base store into it.  A store of arity
 * zero has no columns, and its batches are read through a buffer that is
 * never null, as a batch insertion requires.
 */
static int
seed_member(relic_evaluation_t *eval, relic_relation_t *derived)
{
	static const relic_dom_t no_columns[1];
	const relic_relation_t *store;
	const relic_dom_t *tuples;
	bool added[RELIC_INSERT_BATCH];
	size_t completed;
	size_t first;
	size_t count;
	size_t held;
	size_t i;
	int status;

	store = relic_base_store(eval->ctx, derived->pred);
	held = store == NULL ? 0 : relic_relation_count(store);

	for (first = 0; first < held; first += count) {
		count = held - first < RELIC_INSERT_BATCH ?
		    held - first : RELIC_INSERT_BATCH;
		tuples = store->arity == 0 ? no_columns :
		    relic_relation_tuple(store,
		    (relic_tuple_id_t)(RELIC_TUPLE_FIRST + first));

		status = relic_relation_insert_batch(eval->ctx, derived,
		    tuples, count, added, &completed);
		for (i = 0; i < completed; i++)
			count_derivation(eval, added[i]);
		if (status != 0)
			return (-1);
	}

	return (0);
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

	relic_diag_internal(ctx, NULL,
	    "the evaluation order does not cover the program's rules");

	return (-1);
}

/*
 * Forget everything evaluation derived, so that the derived relations are
 * the fixed point of nothing, and the record of what they materialized with
 * it.  The facts stay in their base stores, and the next evaluation seeds
 * from them.
 */
void
relic_discard_derived(relic_t *ctx)
{
	relic_relation_discard_all(ctx);
	ctx->materialized.recorded = false;
}

/*
 * Whether the relations are the fixed point of the program as it stands:
 * a fixed point was recorded, and neither the program's rules nor the
 * facts of its base stores have been added to since.
 */
bool
relic_relations_current(const relic_t *ctx)
{
	return (ctx->materialized.recorded &&
	    ctx->materialized.rules == ctx->program.rules.count &&
	    ctx->materialized.facts == relic_base_total(ctx));
}

/*
 * Record that the relations are the fixed point of the program as it
 * stands.
 */
static void
record_materialized(relic_t *ctx)
{
	ctx->materialized.rules = ctx->program.rules.count;
	ctx->materialized.facts = relic_base_total(ctx);
	ctx->materialized.recorded = true;
}

/*
 * Record that the evaluation is planning component `position` of a
 * schedule, named by its first member, for a refusal by the memory limit to
 * name.  Planning, and the preparation of a variant's second candidate
 * between rounds, are the only parts of evaluating a component that
 * allocate from the evaluation arena, so a refusal during either names the
 * component being planned.
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
 * `wanted`, only the components it marks are evaluated; everything a wanted
 * component reads must be wanted too or already evaluated, so that no
 * component runs over a relation short of its fixed point.  `depth` is how
 * far the explanation of each component is indented.
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
	relic_evaluation_t eval;
	size_t i;
	int status;

	i = first_wanted(schedule, wanted);
	if (i < schedule->component_count)
		note_planning_component(ctx, schedule, i);

	status = evaluation_begin(&eval, ctx, &ctx->eval_arena,
	    schedule->component_count, widest_head(rules));
	eval.rules = rules;
	if (status == 0)
		status = evaluation_track_deltas(&eval);

	for (i = 0; status == 0 && i < schedule->component_count; i++) {
		if (wanted != NULL && !wanted[i])
			continue;

		note_planning_component(ctx, schedule, i);
		status = evaluate_scheduled(&eval, schedule, i, depth);
	}

	evaluation_end(&eval);

	return (status);
}

/*
 * Plan and evaluate component `position` of a schedule.  Over the
 * program's rules, in a run with a reduction scope (lib/uses.c), the
 * component may be a source the run reduces or a projection its keys
 * fill; every other component, and every component of a rewrite's
 * generated rules, is evaluated as planned.  A projection whose source was
 * reduced is not planned at all, since its rule reads rows the source does
 * not hold; one whose source was stored is evaluated as any other.  The
 * source comes earlier in the schedule than its projections, which read
 * it, so its fate is decided by the time theirs is.
 */
static int
evaluate_scheduled(relic_evaluation_t *eval, const relic_schedule_t *schedule,
    size_t position, unsigned depth)
{
	const relic_key_projection_t *projection;
	const relic_component_t *component;
	relic_component_plans_t *plans;
	relic_reduced_source_t *source;
	relic_pred_id_t pred;
	relic_t *ctx;

	ctx = eval->ctx;
	component = &schedule->components[position];
	plans = &eval->components[position];
	source = NULL;
	if (ctx->reduction != NULL && eval->rules == &ctx->program.rules &&
	    component->member_count == 1) {
		pred = schedule->members[component->first_member];
		projection = relic_reduction_projection(ctx, pred);
		if (projection != NULL) {
			source = &ctx->reduction->sources[projection->source];
			if (source->state == RELIC_SOURCE_REDUCED)
				return (project_keys(eval, schedule, position,
				    projection, source, depth));
		}
		source = relic_reduction_source(ctx, pred);
	}

	if (plan_component(eval, schedule, component, plans) != 0)
		return (-1);

	if (source != NULL && source->state == RELIC_SOURCE_PROMISED)
		return (reduce_source(eval, schedule, position, plans, source,
		    depth));

	explain_component(eval, schedule, position, plans, depth);
	return (evaluate_component(eval, plans));
}

/*
 * Whether a source's producer, as planned, derives each of its rows once
 * (specs/design-group-reduction.md): one rule with a base plan that proves
 * it derives no tuple twice over relations that do not change while it
 * runs, which a component that does not read itself guarantees, folding
 * nothing, and no fact beside it.
 */
static bool
reduction_certified(const relic_t *ctx, const relic_component_plans_t *plans)
{
	const relic_running_plan_t *base;

	if (plans->member_count != 1 || plans->rule_count != 1 ||
	    plans->planned_count != 1 ||
	    plans->fixed_point != RELIC_FIXED_ONE_PASS)
		return (false);

	base = plans->planned[0].base;
	return (base != NULL && relic_plan_derives_distinct(&base->plan) &&
	    !relic_plan_folds(&base->plan) &&
	    relic_base_count(ctx, plans->members[0]->pred) == 0);
}

/*
 * Evaluate a source the run chose to reduce, at its component's turn.  A
 * producer the certificate admits runs into a reducer rather than the
 * source's relation, which stays empty, and the summary the reducer
 * becomes stands in for the rows; one it does not admit is evaluated as
 * any other, and the source is stored, its folds reading its rows.  Either
 * way the source's fate is settled for the rest of the run.
 */
static int
reduce_source(relic_evaluation_t *eval, const relic_schedule_t *schedule,
    size_t position, relic_component_plans_t *plans,
    relic_reduced_source_t *source, unsigned depth)
{
	char described[RELIC_PRED_TEXT_MAX];
	relic_summary_t *summary;
	relic_reducer_t *reducer;
	uint64_t derived_before;
	size_t peak;
	relic_t *ctx;
	int status;

	ctx = eval->ctx;
	explain_component(eval, schedule, position, plans, depth);
	if (!reduction_certified(ctx, plans)) {
		source->state = RELIC_SOURCE_STORED;
		return (evaluate_component(eval, plans));
	}
	relic_explain_reduction(ctx, source, depth);

	relic_diag_activity(ctx, "deriving", relic_relation_describe(ctx,
	    plans->members[0], described, sizeof(described)));
	if (relic_reducer_open(ctx, source, plans->members[0]->arity,
	    &reducer) != 0)
		return (-1);
	derived_before = ctx->counters.derivations;
	status = run_reduction(eval, plans, reducer, &summary);
	peak = relic_reducer_peak(reducer);
	relic_reducer_close(ctx, reducer);
	if (status != 0)
		return (-1);

	source->summary = summary;
	source->state = RELIC_SOURCE_REDUCED;
	ctx->counters.sources_reduced++;
	ctx->counters.reduced_rows += ctx->counters.derivations -
	    derived_before;
	ctx->counters.reduced_groups += relic_summary_groups(summary);
	if (peak > ctx->counters.reduction_peak)
		ctx->counters.reduction_peak = peak;

	return (0);
}

/*
 * Run a certified producer's plan into `reducer`, serially, and make the
 * summary of what it derived.  The texts a plan's calls need are begun
 * around it, as a round's are.
 */
static int
run_reduction(relic_evaluation_t *eval, const relic_component_plans_t *plans,
    relic_reducer_t *reducer, relic_summary_t **summaryp)
{
	const relic_rule_plans_t *rule_plans;
	enum relic_exec_failure failure;
	relic_emit_target_t target;
	int status;

	rule_plans = &plans->planned[0];
	bzero(&target, sizeof(target));
	target.relation = plans->members[0];
	target.reducer = reducer;
	if (relic_plan_calls(&rule_plans->base->plan) &&
	    relic_scratch_begin_texts(&eval->scratch, &eval->ctx->account,
	    &failure) != 0) {
		report_texts_refused(eval->ctx, target.relation, failure);
		return (-1);
	}

	status = execute_plan(eval, &rule_plans->base->plan, rule_plans->rule,
	    &target);
	relic_scratch_end_texts(&eval->scratch);
	if (status != 0)
		return (-1);

	return (relic_reducer_finish(eval->ctx, reducer, target.relation,
	    summaryp));
}

/*
 * Fill a projection's relation from its source's summary in place of its
 * rule: each group's key once, permuted into the head, in the order the
 * keys first occurred, each counted as the rule's EMIT would count it.
 * The rule is not planned, so the component's relation is resolved here,
 * and the interrupt is looked for every batch's worth of keys, as a plan
 * looks for it every quantum.
 */
static int
project_keys(relic_evaluation_t *eval, const relic_schedule_t *schedule,
    size_t position, const relic_key_projection_t *projection,
    const relic_reduced_source_t *source, unsigned depth)
{
	relic_dom_t tuple[RELIC_MAX_ARITY];
	const relic_component_t *component;
	relic_component_plans_t *plans;
	relic_relation_t *relation;
	const relic_dom_t *key;
	size_t groups;
	size_t g;
	uint16_t column;
	bool added;

	component = &schedule->components[position];
	plans = &eval->components[position];
	if (plan_members(eval, schedule, component, plans) != 0)
		return (-1);
	relation = plans->members[0];

	if (relic_explaining(eval->ctx)) {
		relic_explain_component(eval->ctx, schedule, position, 0,
		    depth);
		relic_explain_rule(eval->ctx, rule_of(eval, schedule,
		    component, 0), NULL, depth);
		relic_explain_projection(eval->ctx, projection, source, depth);
	}

	groups = relic_summary_groups(source->summary);
	for (g = 0; g < groups; g++) {
		key = relic_summary_key(source->summary, g);
		for (column = 0; column < relation->arity; column++)
			tuple[column] = key[projection->keys[column]];
		if (relic_relation_insert(eval->ctx, relation, tuple,
		    &added) != 0)
			return (-1);
		count_derivation(eval, added);
		if ((g + 1) % RELIC_INSERT_BATCH == 0 &&
		    relic_refuse_interrupted(eval->ctx, "deriving",
		    relation) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Bring the relations to the fixed point of every rule of the program, one
 * component of the schedule at a time.
 *
 * When the relations are current nothing needs doing.  Otherwise what they
 * hold is stale -- the fixed point of an earlier program, part of one an
 * evaluation that failed was computing, or what a query's Magic rewrite
 * derived -- and it is discarded before every component is planned and
 * evaluated in schedule order.  The discard comes before the evaluation
 * begins, so no delta range can span it.
 */
static int
materialize_program(relic_t *ctx)
{
	if (relic_relations_current(ctx))
		return (0);

	if (confirm_schedule_current(ctx) != 0)
		return (-1);

	relic_discard_derived(ctx);

	if (evaluate_schedule(ctx, &ctx->program.rules, &ctx->program.schedule,
	    NULL, 0) != 0)
		return (-1);

	record_materialized(ctx);

	return (0);
}

int
relic_materialize(relic_t *ctx)
{
	int status;

	if (relic_enter(ctx, NULL, NULL) != 0)
		return (-1);

	status = relic_program_ready(ctx);
	if (status == 0)
		status = materialize_program(ctx);

	relic_leave(ctx);

	return (status);
}

/*
 * Pass every tuple of a gathered answer relation to the caller as a row of
 * values, in insertion order, until the callback asks to stop.  Returns 0,
 * or -1 when the caller interrupted the delivery, which is looked for
 * before each row, so that a callback writing a large answer somewhere slow
 * can be stopped.
 *
 * Nothing else here can fail, which is what lets a query promise that no
 * row is delivered unless every row can be; an interrupt is the caller's
 * own request, and the rows delivered before it are a prefix of the answer.
 *
 * The context is busy while the callback runs, so a callback that calls
 * back to load or query is refused, and the answer relation cannot be
 * emptied or refilled under the loop.  The tuple is fetched afresh for each
 * row all the same, so that no pointer into relation storage is held across a
 * call out of the library, and the loop stops at an identifier the relation
 * does not hold rather than read past it, should that ever stop being true.
 */
static int
deliver_gathered_answer(relic_t *ctx, const relic_relation_t *answer,
    const relic_row_sink_t *sink)
{
	relic_value_t row[RELIC_MAX_ARITY];
	const relic_dom_t *tuple;
	relic_tuple_range_t range;
	relic_tuple_id_t id;

	range = relic_relation_all(answer);

	for (id = range.first; id < range.end; id++) {
		if (relic_refuse_interrupted(ctx, "delivering", answer) != 0)
			return (-1);

		tuple = relic_relation_tuple(answer, id);
		if (tuple == NULL)
			return (0);

		decode_row(ctx, tuple, answer->arity, row);
		if (sink->callback(sink->callback_ctx, row, answer->arity) != 0)
			return (0);
	}

	return (0);
}

/*
 * Run a query plan whose EMIT delivers its rows to the caller.
 *
 * Such a run inserts nothing and allocates nothing: its plan, and the
 * states the scratch holds for it, were allocated when it was planned,
 * every index it looks up was built then, and its EMIT writes no relation.
 * So it fails only when the caller interrupts it, and otherwise ends early
 * only when the caller asks for no more rows.  The count of inserted
 * tuples and the account's totals are asserted to be what they were before
 * it; the totals move when a relation's storage grows or an arena takes a
 * new chunk, so an allocation that fits a chunk already taken passes
 * unseen.  Its status is returned all the same, so that a build without
 * assertions reports a failure that should be impossible rather than hide
 * it.
 */
static int
stream_query_plan(relic_evaluation_t *eval,
    const relic_running_plan_t *running)
{
#ifndef NDEBUG
	relic_account_t account_before;
	uint64_t inserted_before;
#endif
	int status;

#ifndef NDEBUG
	account_before = eval->ctx->account;
	inserted_before = eval->ctx->counters.tuples_inserted;
#endif

	status = execute_plan(eval, &running->plan, running->rule,
	    &running->target);

	assert(status == 0 ||
	    relic_signal_flag_raised(&eval->ctx->interrupted));
	assert(eval->ctx->account.store_bytes == account_before.store_bytes);
	assert(eval->ctx->account.arena_bytes == account_before.arena_bytes);
	assert(eval->ctx->counters.tuples_inserted == inserted_before);

	return (status);
}

/*
 * Run a query plan whose EMIT inserts into the query's answer relation, as
 * a round of that one item.  Nothing reads the answer, so the round may be
 * deferred.
 */
static int
gather_query_answer(relic_evaluation_t *eval,
    const relic_running_plan_t *running)
{
	relic_relation_t *answer;
	relic_round_item_t item;
	relic_round_t round;

	answer = running->target.relation;

	item.plan = &running->plan;
	item.rule = running->rule;
	item.history = NULL;
	item.insertions = NULL;
	item.target = 0;
	item.gained = false;

	round.items = &item;
	round.targets = &answer;
	round.env = &eval->env;
	round.item_count = 1;
	round.item_capacity = 1;
	round.target_count = 1;

	return (run_round(eval, &round));
}

/*
 * Whether a query's plan delivers its rows as it derives them rather than
 * gathering them in its answer relation first.
 *
 * Its derivations must be its rows already, each derived once, which the
 * planner decides from the plan.  It must compute nothing, since an
 * expression can meet an undefined result at any binding, which fails the
 * query, and a query that fails must deliver no row; only an interrupt the
 * caller makes may follow one.  It must fold nothing
 * either: a sum can fail as an expression can, a count can derive an
 * integer a streamed row could not hold, and a min or a max is gathered
 * with them, so that whether an aggregate streams does not depend on which
 * one it is.  And no configured
 * limit may bound the relations.  Under a limit a query that is refused must
 * be refused before its first row, so that it delivers all of its rows or
 * none; a row that is delivered without being stored is never refused, so a
 * streamed answer would hand over any number of rows the limit would not
 * have let the answer hold.  The query gathers its answer instead, and the
 * limit bounds it as it bounds any relation.
 */
static bool
query_streams(const relic_t *ctx, const relic_plan_t *plan)
{
	if ((ctx->config.flags & RELIC_F_NO_STREAM) != 0)
		return (false);

	if (relic_limits_bound_relations(ctx))
		return (false);

	if (relic_plan_computes(plan) || relic_plan_folds(plan))
		return (false);

	return (relic_plan_derives_distinct(plan));
}

/*
 * Point a planned query's EMIT at the caller's rows when query_streams()
 * admits its plan, and report whether it did.  Otherwise the EMIT keeps the
 * target it was planned with, the query's answer relation.
 */
static bool
choose_query_target(const relic_t *ctx, relic_running_plan_t *running,
    const relic_row_sink_t *rows)
{
	if (!query_streams(ctx, &running->plan))
		return (false);

	running->target.rows = *rows;

	return (true);
}

/*
 * Run a query's rule -- the one written, or the one its rewrite produced --
 * once, every relation it reads being complete, and deliver its rows to the
 * caller `rows` names.  Its explanation shows the group rules `groups` ran
 * under the query's heading, before the query's plan.  One pass computes
 * the answer completely, because no body reads `answer`, the query's answer
 * relation: it belongs to no predicate, so no literal can name it.
 *
 * A plan that query_streams() admits delivers each tuple to the caller as
 * its EMIT derives it, and the answer relation is never written.  Its
 * derivations are distinct, so the relation would have rejected none of them
 * and listed them in the order they were derived: the rows are the ones it
 * would have held, in its order.  Every step that can fail -- beginning the
 * evaluation, planning, building the indexes the plan looks up -- comes
 * before the first row, so a query that fails delivers nothing, unless the
 * caller interrupts it after its first row.
 *
 * Any other plan fills the answer relation, which drops duplicates as they
 * are inserted and counts against the limits, and the rows the relation
 * gathered are delivered once the rule has run.
 *
 * The answer is emptied first, so the run starts from nothing whatever the
 * relation held and whatever ran before it, and emptied again when the run
 * fails, so the answer holds tuples only after a run that succeeded and
 * nothing a caller does after a failure -- such as falling back to the whole
 * program, which discards every derived relation -- meets a partial one.
 */
static int
run_query_rule(relic_t *ctx, const relic_rule_t *rule,
    relic_relation_t *answer, const relic_group_run_t *groups,
    const relic_row_sink_t *rows)
{
	relic_running_plan_t *running;
	relic_evaluation_t eval;
	bool streams;
	int status;

	relic_relation_clear(ctx, answer);
	note_query_activity(ctx, "planning", answer);

	streams = false;
	status = evaluation_begin(&eval, ctx, &ctx->eval_arena, 1,
	    answer->arity);
	if (status == 0)
		status = plan_query(&eval, rule, answer, &eval.components[0]);
	if (status == 0) {
		running = eval.components[0].planned[0].base;
		streams = choose_query_target(ctx, running, rows);
		relic_explain_query(ctx, rule, answer);
		explain_group_rules(ctx, groups);
		relic_explain_query_plan(ctx, rule, &running->plan, streams);
		if (streams)
			status = stream_query_plan(&eval, running);
		else
			status = gather_query_answer(&eval, running);
	}
	evaluation_end(&eval);

	if (status != 0) {
		relic_relation_clear(ctx, answer);
		return (-1);
	}

	if (!streams)
		return (deliver_gathered_answer(ctx, answer, rows));

	return (0);
}

/*
 * Ready a query's group run: its group rules, and an arena charged to the
 * context's account, as the evaluation arena is.
 */
static void
group_run_init(relic_group_run_t *groups, relic_t *ctx,
    const relic_query_t *query)
{
	bzero(groups, sizeof(*groups));
	relic_arena_init_charged(&groups->arena, &ctx->account);
	groups->rules = query->group_rules;
	groups->count = query->group_rule_count;
}

/*
 * Plan each group rule into its group relation, as a query's rule is
 * planned: one pass, every literal reading its whole relation.  The plans
 * are the one component of the run's evaluation, in body order.
 */
static int
plan_group_rules(relic_evaluation_t *eval, const relic_group_run_t *groups)
{
	relic_component_plans_t *plans;
	relic_rule_plans_t *rule_plans;
	relic_relation_t *relation;
	const relic_rule_t *rule;
	size_t i;

	plans = &eval->components[0];
	plans->planned = evaluation_alloc(eval, groups->count,
	    sizeof(*plans->planned));
	if (plans->planned == NULL)
		return (-1);

	for (i = 0; i < groups->count; i++) {
		rule = &groups->rules[i];
		relation = relic_relation_written(eval->ctx, rule->head);
		if (relation == NULL)
			return (-1);

		rule_plans = &plans->planned[i];
		bzero(rule_plans, sizeof(*rule_plans));
		rule_plans->rule = rule;
		plans->planned_count++;
		if (build_running_plan(eval, rule, relation,
		    &rule_plans->base) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Run a query's group rules, step 3 of answering it: every relation they
 * read is complete, so one round of one pass each derives each group
 * relation whole.  A rule reads no group relation, so the round reads none
 * of its targets and may wait for its end to insert.  Returns 0, or -1 with
 * a diagnostic reported, and the run is then finished by its caller.
 */
static int
run_group_rules(relic_t *ctx, relic_group_run_t *groups)
{
	relic_component_plans_t *plans;
	relic_relation_t **targets;
	relic_round_item_t *items;
	relic_round_t round;
	uint16_t widest;
	size_t i;
	int status;

	if (groups->count == 0)
		return (0);

	widest = 0;
	for (i = 0; i < groups->count; i++) {
		if (groups->rules[i].head_arity > widest)
			widest = groups->rules[i].head_arity;
	}

	status = evaluation_begin(&groups->eval, ctx, &groups->arena, 1,
	    widest);
	groups->begun = true;
	if (status != 0 || plan_group_rules(&groups->eval, groups) != 0)
		return (-1);

	plans = &groups->eval.components[0];
	items = evaluation_alloc(&groups->eval, groups->count, sizeof(*items));
	if (items == NULL)
		return (-1);
	targets = evaluation_alloc(&groups->eval, groups->count,
	    sizeof(*targets));
	if (targets == NULL)
		return (-1);

	for (i = 0; i < groups->count; i++) {
		bzero(&items[i], sizeof(items[i]));
		items[i].plan = &plans->planned[i].base->plan;
		items[i].rule = plans->planned[i].rule;
		items[i].target = i;
		targets[i] = plans->planned[i].base->target.relation;
	}

	round.items = items;
	round.targets = targets;
	round.env = &groups->eval.env;
	round.item_count = groups->count;
	round.item_capacity = groups->count;
	round.target_count = groups->count;

	return (run_round(&groups->eval, &round));
}

/*
 * Show each group rule a query's run planned, with the plan that ran it,
 * one level in, as the rules of a component are shown.
 */
static void
explain_group_rules(relic_t *ctx, const relic_group_run_t *groups)
{
	const relic_component_plans_t *plans;
	size_t i;

	if (!groups->begun)
		return;

	plans = &groups->eval.components[0];
	for (i = 0; i < plans->planned_count; i++) {
		if (plans->planned[i].base != NULL)
			relic_explain_rule(ctx, plans->planned[i].rule,
			    &plans->planned[i].base->plan, 0);
	}
}

/*
 * Finish a query's group run: empty the group relations it wrote, and end
 * its evaluation, releasing the plans.  The run is then as group_run_init()
 * left it, so an answer that falls back to the whole program can run the
 * group rules again.
 */
static void
group_run_finish(relic_t *ctx, relic_group_run_t *groups)
{
	const relic_component_plans_t *plans;
	size_t i;

	if (!groups->begun)
		return;

	plans = groups->eval.components;
	for (i = 0; plans != NULL && i < plans->planned_count; i++) {
		if (plans->planned[i].base != NULL)
			relic_relation_clear(ctx,
			    plans->planned[i].base->target.relation);
	}

	evaluation_end(&groups->eval);
	groups->begun = false;
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
		relic_diag_internal(ctx, NULL,
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
 * Evaluate from the program's own schedule the components a query reads
 * completely because its rewrite found every predicate it reaches read
 * free, after showing the rewrite that found so.  The rewrite generated no
 * rule, and the query runs as written over what this evaluates.
 */
static int
evaluate_read_completely(relic_t *ctx, relic_magic_t *magic,
    const relic_query_t *query)
{
	relic_explain_rewrite(ctx, &query->rule, query->answer, magic);

	return (evaluate_schedule(ctx, &ctx->program.rules,
	    &ctx->program.schedule, magic->completed_late, 0));
}

/*
 * Compute what a query's rewrite needs before its rewritten rule can run:
 * the relations the query reads completely, evaluated from the program's own
 * schedule, then the query's group relations, which read only those, and
 * then the fixed point of the rules the rewrite generates, or of the
 * program's rules for the components it found read completely.  The group
 * relations come before the generated rules because a generated rule may
 * fold one.  The sources the run reduces are chosen once what the query
 * reads completely is known, which they must be, and before any of it is
 * evaluated, since their producers run at their components' turns.
 * Finding what the query reads completely and generating the rules are the
 * rewrite's, and a refusal by the memory limit in either names the query;
 * the evaluations between them name what they plan.
 *
 * Whatever the relations held is discarded first.  What the rewrite derives
 * is only part of the program's fixed point, so no materialized program is
 * recorded, and the next evaluation starts again from the facts.
 */
static int
derive_for_rewrite(relic_t *ctx, relic_magic_t *magic,
    const relic_query_t *query, relic_group_run_t *groups)
{
	if (confirm_schedule_current(ctx) != 0)
		return (-1);

	/*
	 * A group run's plans would name relations the discard frees, so
	 * none may have begun.
	 */
	assert(!groups->begun);
	relic_discard_derived(ctx);

	note_query_activity(ctx, "rewriting", query->answer);
	if (relic_magic_find_complete(magic, query) != 0)
		return (-1);
	if (relic_reduction_select(ctx, magic, query) != 0)
		return (-1);

	if (evaluate_schedule(ctx, &ctx->program.rules, &ctx->program.schedule,
	    magic->complete_components, 0) != 0)
		return (-1);

	if (run_group_rules(ctx, groups) != 0)
		return (-1);

	note_query_activity(ctx, "rewriting", query->answer);
	if (relic_magic_rewrite(magic, &query->rule) != 0)
		return (-1);

	if (magic->kept.count == 0)
		return (0);

	if (magic->reads_completely)
		return (evaluate_read_completely(ctx, magic, query));

	return (evaluate_rewrite(ctx, magic, query));
}

/*
 * Answer a query from the fixed point of the whole program, computing it
 * first if the relations do not hold it, then its group relations, and
 * deliver its rows.
 *
 * A group run an abandoned rewrite began is finished first: its plans hold
 * group relations the rewrite derived, which computing the fixed point
 * discards and so frees, and the group rules run again over the whole
 * fixed point.
 */
static int
answer_from_program(relic_t *ctx, const relic_query_t *query,
    relic_group_run_t *groups, const relic_row_sink_t *rows)
{
	group_run_finish(ctx, groups);

	if (materialize_program(ctx) != 0)
		return (-1);

	if (run_group_rules(ctx, groups) != 0)
		return (-1);

	return (run_query_rule(ctx, &query->rule, query->answer, groups,
	    rows));
}

/*
 * Answer a query through its Magic rewrite, which derives only what the
 * query's bindings can reach, running the rewritten query rule last and
 * delivering its rows.
 *
 * A rewrite that outgrows its limit has been reported as a warning, and
 * the query is answered from the whole program instead: the rewrite is an
 * optimization, and must not decide whether a query can be answered.  What
 * the abandoned rewrite derived is discarded with the rest, since it
 * recorded no materialized program.  No row has been delivered when the
 * whole program is turned to, because run_query_rule() delivers none
 * before everything but an interrupt has stopped being able to fail it, an
 * interrupt does not abandon a rewrite, and a rewrite is abandoned before
 * it is run.
 *
 * The run's reduction scope lives exactly as long as the rewrite: the
 * group run, whose plans may read the scope's summaries, is finished
 * before the scope ends, and an answer from the whole program has none.
 */
static int
answer_through_rewrite(relic_t *ctx, const relic_query_t *query,
    relic_group_run_t *groups, const relic_row_sink_t *rows)
{
	relic_reduction_t reduction;
	relic_magic_t magic;
	bool abandoned;
	int status;

	relic_magic_init(&magic, ctx);
	relic_reduction_begin(&reduction, ctx);
	status = derive_for_rewrite(ctx, &magic, query, groups);
	if (status == 0)
		status = run_query_rule(ctx, &magic.query, query->answer,
		    groups, rows);
	abandoned = magic.abandoned;
	relic_magic_reset(&magic);
	group_run_finish(ctx, groups);
	relic_reduction_end(&reduction, ctx);

	if (status != 0 && abandoned) {
		relic_explain_abandoned_rewrite(ctx, &query->rule,
		    query->answer);
		return (answer_from_program(ctx, query, groups, rows));
	}

	return (status);
}

/*
 * Answer a query and deliver its rows, as they are derived or from its
 * answer relation once it is filled, whichever run_query_rule() chooses.
 *
 * The relation is the query's own, emptied before its rule runs and again
 * once the rows are delivered, so outside a run it holds nothing and counts
 * nothing against the limits; while a query streams, it holds nothing even
 * during the run.  The context is busy throughout, so no other operation
 * sees it holding tuples.
 *
 * A program whose fixed point is already computed -- or any program, when
 * the rewrite is turned off -- answers from the whole fixed point.  Otherwise
 * the query is answered through its Magic rewrite.  Both give the same
 * answer, because the rewrite computes exactly the tuples of the fixed point
 * that the query reads.
 *
 * The rows are distinct, which is what gives a query set semantics: a rule
 * that streams derives no row twice, and any other's duplicates collapse as
 * they are inserted.  Every step that can fail comes before the first row is
 * delivered, so a query either delivers its whole answer or none of it, but
 * for one the caller interrupts, which delivers a prefix of it and fails.
 */
int
relic_query_run(relic_t *ctx, relic_query_t *query, relic_row_cb callback,
    void *callback_ctx)
{
	int status;

	if (relic_enter(ctx, NULL, NULL) != 0)
		return (-1);

	status = relic_query_answer(ctx, query, callback, callback_ctx);

	relic_leave(ctx);

	return (status);
}

int
relic_query_answer(relic_t *ctx, relic_query_t *query, relic_row_cb callback,
    void *callback_ctx)
{
	relic_row_sink_t rows;
	int status;

	if (!query_answerable(ctx, query, callback))
		return (-1);

	query->running = true;

	rows.callback = callback;
	rows.callback_ctx = callback_ctx;
	status = compute_answer(ctx, query, &rows);

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
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "a query and a row callback must both be given");
		return (false);
	}

	if (query->ctx != ctx) {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "the query belongs to a different context");
		return (false);
	}

	return (true);
}

/*
 * Answer the query and deliver its rows as `rows` says: from the relations
 * as they stand when the program is materialized or the rewrite is turned
 * off, and through the query's Magic rewrite otherwise.  Either way the
 * query's group relations are emptied once the answer is finished.
 */
static int
compute_answer(relic_t *ctx, const relic_query_t *query,
    const relic_row_sink_t *rows)
{
	relic_group_run_t groups;
	int status;

	if (relic_program_ready(ctx) != 0)
		return (-1);

	ctx->query_mixed_noted = false;

	group_run_init(&groups, ctx, query);
	if ((ctx->config.flags & RELIC_F_NO_MAGIC) != 0 ||
	    relic_relations_current(ctx))
		status = answer_from_program(ctx, query, &groups, rows);
	else
		status = answer_through_rewrite(ctx, query, &groups, rows);
	group_run_finish(ctx, &groups);

	return (status);
}

void
relic_stats(relic_t *ctx, relic_stats_t *stats)
{
	bzero(stats, sizeof(*stats));

	stats->iterations = ctx->counters.iterations;
	stats->derivations = ctx->counters.derivations;
	stats->tuples_inserted = ctx->counters.tuples_inserted;
	stats->tuples_scanned = ctx->counters.read.tuples_scanned;
	stats->tuples_looked_up = ctx->counters.read.tuples_looked_up;
	stats->parallel_rounds = ctx->counters.parallel_rounds;

	stats->tuples = relic_relation_total_tuples(ctx);
	stats->storage_bytes = relic_relation_total_bytes(ctx) +
	    relic_value_storage_bytes(ctx) + relic_pred_table_bytes(ctx);
	stats->symbol_values = relic_value_count_of(ctx, RELIC_SYMBOL);
	stats->string_values = relic_value_count_of(ctx, RELIC_STRING);
	stats->integer_values = relic_value_count_of(ctx, RELIC_INTEGER);
}
