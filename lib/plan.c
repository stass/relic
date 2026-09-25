/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * The planner: a compiled rule becomes an ordered array of operations.
 *
 * The walk keeps one set, the slots bound so far, and turns each literal into
 * an operation at the point it is placed.  A positive literal whose every
 * argument is already bound is a membership test; one with some bound
 * arguments is a lookup through the index keyed on exactly those columns,
 * which the relation builds if it has none; one with none bound is a scan.
 * A lookup and a scan bind what they can.  A comparison and a negated literal
 * bind nothing -- each is a test, a FILTER or an ANTI_MEMBER -- so each can
 * only be placed where every slot it reads is bound, and it is placed at the
 * earliest such point.  The head becomes the EMIT that ends the plan, writing
 * into the relation the caller names: a predicate's for a rule of the
 * program, the query's own answer for a query.
 *
 * Positive literals are placed greedily.  At each step the best positive
 * literal not yet placed, scored against the slots bound so far, is placed:
 * the one with more bound arguments, then the one with the smaller estimate
 * of the tuples it produces per binding, then the one written first.  The
 * estimate reads the relation's tuple count and the distinct values of its
 * bound columns, assuming the columns independent:
 *
 *	estimate = count
 *	for each bound column c: estimate = ceil(estimate / distinct(c))
 *
 * so an empty relation estimates zero and is placed first among the
 * literals binding as many arguments, ending the join once it is reached,
 * and a nonempty one never estimates below one.  A relation the
 * component being planned is still deriving is empty and cannot be measured;
 * it is taken to hold RELIC_PLAN_UNKNOWN_TUPLES tuples, narrowed by
 * RELIC_PLAN_UNKNOWN_DISTINCT per bound column.  The arithmetic is integer so
 * that the same input gives the same plan on every platform.
 *
 * The greedy order is computed in passes rather than steps.  A literal's
 * score depends only on the bound set and on statistics that nothing changes
 * while a plan is ordered, and the bound set only grows: placing a positive
 * literal adds the slots it binds and removes none.  So a pass scores every
 * positive literal not yet placed once, sorts them into a ranking by the key
 * above, and places literals from the front of the ranking for as long as
 * the bound set stays as the pass found it.  Until then no score has
 * changed, so the next literal of the ranking is the best literal not yet
 * placed, which is what the greedy order places.  Only a placement that
 * binds a new slot can change a score, and it ends the pass.  The bound set
 * is 64 bits wide, so no order takes more than 65 passes however long the
 * body, and each pass scores, sorts and offers the tests a place once:
 * ordering a body of N literals costs at most 65 x N log N rather than N
 * squared.  The scoring functions take the bound set as an argument, rather
 * than reading the planner, so that nothing else can enter a score unseen.
 *
 * A test's written position need not be one where its operands exist -- `N
 * >= 100, size(X, N)` and `not done(X), todo(X)` are safe bodies -- so it
 * goes where it first can.  The rule's safety is what guarantees such a point
 * exists: every slot a test reads is bound by some positive literal, so once
 * every positive literal is placed, every test can be.
 *
 * Each column's fate is decided here, while the bound set is known: matched
 * against a constant, matched against a slot already bound, binding its
 * slot, or ignored.  The executor then makes no decision of its own, and a
 * better planner is a change to this file and never to the one that runs
 * what it emits.
 *
 * A semi-naive delta variant of a rule is the same walk with one literal
 * placed ahead of the others: the delta literal, which reads only the tuples
 * its relation gained in the previous round.  It is always a SCAN, even when
 * every argument is a constant, because only a scan walks the delta's range.
 * Placed first, it is only ever scanned, so a delta never needs an index of
 * its own.
 *
 * Every positive literal of a variant reads through a view (plan.h), and the
 * view follows from body order alone: the delta literal reads `delta`, a
 * literal over a relation still being derived -- a member of the component,
 * which `incomplete` marks -- reads `old` when it is written before the
 * delta literal and `current` when it is written after it, and a literal
 * over a complete relation reads `all`.  A variant is refused unless its
 * delta literal reads a marked relation, so the marks a view is decided by
 * are always the component's.  The order the
 * walk chooses decides what each binding costs; the view decides which
 * tuples a literal can match, and the set of tuples a join produces does not
 * depend on its nesting, so the two questions are separate.
 *
 * The order is decided before any operation is built.  A first walk places
 * the literals -- recording, for each, where it goes and what it binds -- and
 * a second replays that order, turning each literal into its operation.  The
 * first walk builds no index and emits nothing, which is what lets the Magic
 * rewrite ask for the order alone, from a set of slots its caller already
 * binds, and so borrow the planner's notion of body order rather than keep
 * one of its own.
 *
 * Two switches select the references the default is tested against.
 * RELIC_F_NO_INDEX makes every lookup a scan matching the same columns, and
 * RELIC_F_NO_REORDER places positive literals in the order they are written.
 * Each changes how an answer is found and never what it is.
 */

typedef struct relic_plan_candidate relic_plan_candidate_t;
typedef struct relic_plan_score relic_plan_score_t;
typedef struct relic_planner relic_planner_t;

/*
 * A plan under construction.
 *
 * `order` receives the body index of each literal as it is placed, and
 * `order_count` says how many have been; `placed` records the same thing
 * indexed like the body.  `bound` is the set of slots bound so far: while the
 * order is decided, by the literals placed and the slots the caller started
 * with; while the operations are emitted, by the operations emitted.
 *
 * `ranking` holds the pass in progress: the `ranking_count` positive
 * literals not yet placed when it began, best first.  It has room for every
 * positive literal of the body, and is allocated for the one call and freed
 * when it returns, so that the plans an arena keeps do not keep it too.
 *
 * `ops` has room for one operation per body literal and one EMIT, which is
 * exactly what a plan holds, and is NULL when only the order is wanted.
 * `target` is the relation the EMIT writes, and NULL when only the order is
 * wanted.
 * `delta_literal` is the body index of the literal that reads the delta, or
 * RELIC_PLAN_NO_DELTA.  `incomplete`, when not NULL, is indexed by predicate
 * identifier minus one and marks the relations still being derived, which
 * the estimate cannot measure.
 */
struct relic_planner {
	relic_t			*ctx;
	relic_arena_t		*arena;
	const relic_rule_t	*rule;
	relic_relation_t	*target;
	const bool		*incomplete;
	size_t			*order;
	bool			*placed;
	relic_plan_candidate_t	*ranking;
	relic_op_t		*ops;
	size_t			 order_count;
	size_t			 ranking_count;
	size_t			 op_count;
	size_t			 delta_literal;
	uint64_t		 bound;
};

/*
 * How good a positive literal would be to place next: more bound arguments
 * is better, then a smaller estimate.
 */
struct relic_plan_score {
	size_t		estimate;
	uint16_t	bound_args;
};

/*
 * A positive literal in a ranking: its body index, and its score against the
 * bound set of the pass that ranked it.
 */
struct relic_plan_candidate {
	relic_plan_score_t	score;
	size_t			index;
};

static relic_op_t *append_atom_op(relic_planner_t *planner,
		    enum relic_op_kind kind, relic_relation_t *relation);
static relic_op_t *append_op(relic_planner_t *planner,
		    enum relic_op_kind kind);
static uint32_t	 all_columns(uint16_t arity);
static uint64_t	 arg_slots(const relic_arg_t *args, uint16_t count);
static bool	 atom_is_bound(const relic_atom_t *atom, uint16_t arity,
		    uint64_t bound);
static uint32_t	 bound_columns(const relic_planner_t *planner,
		    const relic_atom_t *atom, uint16_t arity);
static int	 build_plan(relic_planner_t *planner);
static uint16_t	 column_count(uint32_t columns);
static int	 compare_candidates(const void *a, const void *b);
static uint64_t	 comparison_slots(const relic_comparison_t *comparison);
static int	 confirm_delta_literal(const relic_planner_t *planner);
static int	 confirm_order_complete(const relic_planner_t *planner);
static int	 confirm_plan_complete(const relic_planner_t *planner);
static int	 confirm_target_fits(const relic_planner_t *planner);
static int	 decide_order(relic_planner_t *planner);
static size_t	 divide_rounding_up(size_t dividend, size_t divisor);
static int	 emit_body(relic_planner_t *planner);
static size_t	 estimate_unknown(uint16_t arity, uint32_t bound);
static int	 estimate_measured(relic_t *ctx, relic_relation_t *relation,
		    uint32_t columns, size_t *estimatep);
static bool	 is_incomplete(const bool *incomplete, relic_pred_id_t pred);
static int	 order_body(relic_planner_t *planner);
static void	 order_positive(relic_planner_t *planner, size_t index);
static void	 order_ranked(relic_planner_t *planner);
static void	 order_ready_tests(relic_planner_t *planner);
static int	 place_atom(relic_planner_t *planner, const relic_atom_t *atom,
		    enum relic_view view);
static int	 planning_status(relic_t *ctx, relic_arena_t *arena,
		    int status);
static void	 place_comparison(relic_planner_t *planner,
		    const relic_literal_t *literal);
static int	 place_emit(relic_planner_t *planner);
static int	 place_negated(relic_planner_t *planner,
		    const relic_atom_t *atom);
static size_t	 positive_count(const relic_rule_t *rule);
static enum relic_op_kind positive_kind(const relic_planner_t *planner,
		    const relic_relation_t *relation, uint32_t bound,
		    enum relic_view view);
static enum relic_view positive_view(const relic_planner_t *planner,
		    size_t index);
static int	 place_test(relic_planner_t *planner,
		    const relic_literal_t *literal);
static int	 planner_init(relic_planner_t *planner, relic_t *ctx,
		    relic_arena_t *arena, const relic_rule_t *rule,
		    relic_relation_t *target, size_t delta_literal,
		    const bool *incomplete);
static void	 planner_release(relic_planner_t *planner);
static int	 rank_positives(relic_planner_t *planner);
static void	 read_term(const relic_arg_t *arg, relic_plan_term_t *term);
static void	 scan_term(relic_planner_t *planner, const relic_arg_t *arg,
		    relic_plan_term_t *term);
static int	 score_positive(relic_t *ctx, const bool *incomplete,
		    const relic_atom_t *atom, uint64_t bound,
		    relic_plan_score_t *scorep);
static uint64_t	 slot_bit(uint16_t slot);
static relic_plan_term_t *terms_alloc(relic_planner_t *planner,
		    uint16_t arity);
static bool	 test_is_ready(relic_t *ctx, const relic_literal_t *literal,
		    uint64_t bound);

static uint64_t
slot_bit(uint16_t slot)
{
	return (UINT64_C(1) << slot);
}

/*
 * Room for a relation's arity of terms, or NULL for arity zero, which has no
 * columns to describe.  A null return for a nonzero arity means the arena
 * refused, which the caller's boundary reports.
 */
static relic_plan_term_t *
terms_alloc(relic_planner_t *planner, uint16_t arity)
{
	if (arity == 0)
		return (NULL);

	return (relic_arena_alloc(planner->arena,
	    (size_t)arity * sizeof(relic_plan_term_t)));
}

/*
 * The next operation of the plan, zeroed and of the given kind.  There is
 * always room: the array was sized for every literal and the EMIT.
 */
static relic_op_t *
append_op(relic_planner_t *planner, enum relic_op_kind kind)
{
	relic_op_t *op;

	op = &planner->ops[planner->op_count];
	planner->op_count++;

	bzero(op, sizeof(*op));
	op->kind = kind;

	return (op);
}

/*
 * The next operation of the plan, of a kind that reads a relation, with room
 * for a term per column for the caller to fill in.  Returns NULL when the
 * arena refused the terms, which the caller's boundary reports.
 */
static relic_op_t *
append_atom_op(relic_planner_t *planner, enum relic_op_kind kind,
    relic_relation_t *relation)
{
	relic_op_t *op;

	op = append_op(planner, kind);
	op->u.atom.relation = relation;
	op->u.atom.view = RELIC_VIEW_ALL;
	op->u.atom.terms = terms_alloc(planner, relation->arity);
	if (relation->arity > 0 && op->u.atom.terms == NULL)
		return (NULL);

	return (op);
}

/*
 * The term a test or an EMIT reads for an argument: a constant, or a slot.
 * The caller places the reader only where its slots are bound, which
 * confirm_plan_complete() checks once per plan.
 *
 * A reader never meets `_`.  Safety rejects `_` anywhere but a positive
 * literal, and a literal holding one is never a test, because atom_is_bound()
 * says it is not bound.  Should one arrive regardless, it reads as matching
 * anything rather than as a slot it does not have.
 */
static void
read_term(const relic_arg_t *arg, relic_plan_term_t *term)
{
	bzero(term, sizeof(*term));

	switch (arg->kind) {
	case RELIC_ARG_CONSTANT:
		term->action = RELIC_PLAN_CONSTANT;
		term->u.constant = arg->u.constant;
		return;
	case RELIC_ARG_VARIABLE:
		term->action = RELIC_PLAN_BOUND;
		term->u.slot = arg->u.slot;
		return;
	case RELIC_ARG_WILDCARD:
		break;
	}

	term->action = RELIC_PLAN_ANY;
}

/*
 * The term a SCAN uses for an argument, binding the argument's slot when
 * nothing has bound it yet.
 *
 * The bound set is updated column by column, so a variable written twice in
 * one atom -- edge(X, X) -- binds at its first column and is matched at its
 * second against the value just bound.
 */
static void
scan_term(relic_planner_t *planner, const relic_arg_t *arg,
    relic_plan_term_t *term)
{
	bzero(term, sizeof(*term));

	switch (arg->kind) {
	case RELIC_ARG_CONSTANT:
		term->action = RELIC_PLAN_CONSTANT;
		term->u.constant = arg->u.constant;
		return;
	case RELIC_ARG_WILDCARD:
		term->action = RELIC_PLAN_ANY;
		return;
	case RELIC_ARG_VARIABLE:
		break;
	}

	term->u.slot = arg->u.slot;

	if ((planner->bound & slot_bit(arg->u.slot)) != 0) {
		term->action = RELIC_PLAN_BOUND;
		return;
	}

	term->action = RELIC_PLAN_BIND;
	planner->bound |= slot_bit(arg->u.slot);
}

/*
 * Every column of a relation of this arity, bit c for column c.
 */
static uint32_t
all_columns(uint16_t arity)
{
	if (arity >= 32)
		return (UINT32_MAX);

	return ((UINT32_C(1) << arity) - 1);
}

static uint16_t
column_count(uint32_t columns)
{
	uint16_t count;

	count = 0;
	for (; columns != 0; columns &= columns - 1)
		count++;

	return (count);
}

/*
 * The columns of an atom that hold a constant or a slot of `bound`.  A
 * wildcard is never bound: it asks for any value, and only a generator can
 * supply one.  Neither is a variable that only an earlier column of the same
 * atom binds, since that binding does not exist until a tuple has been read.
 */
uint32_t
relic_plan_bound_columns(const relic_atom_t *atom, uint16_t arity,
    uint64_t bound)
{
	const relic_arg_t *arg;
	uint32_t columns;
	uint16_t i;

	columns = 0;
	for (i = 0; i < arity; i++) {
		arg = &atom->args[i];

		if (arg->kind == RELIC_ARG_CONSTANT ||
		    (arg->kind == RELIC_ARG_VARIABLE &&
		    (bound & slot_bit(arg->u.slot)) != 0))
			columns |= UINT32_C(1) << i;
	}

	return (columns);
}

/*
 * The columns of an atom an operation placed now could look it up by.
 */
static uint32_t
bound_columns(const relic_planner_t *planner, const relic_atom_t *atom,
    uint16_t arity)
{
	return (relic_plan_bound_columns(atom, arity, planner->bound));
}

/*
 * Whether every argument of an atom is a constant or a bound slot, which is
 * what makes it a membership test rather than a generator.
 */
static bool
atom_is_bound(const relic_atom_t *atom, uint16_t arity, uint64_t bound)
{
	return (relic_plan_bound_columns(atom, arity, bound) ==
	    all_columns(arity));
}

/*
 * The operation a positive literal becomes with `bound` its bound columns:
 * MEMBER when every column is bound, LOOKUP when some are, SCAN when none
 * are.  A literal that reads the delta is a SCAN whatever is bound, since
 * only a scan walks the delta's range, and under RELIC_F_NO_INDEX a literal
 * that would be looked up is scanned instead.
 */
static enum relic_op_kind
positive_kind(const relic_planner_t *planner,
    const relic_relation_t *relation, uint32_t bound, enum relic_view view)
{
	if (view == RELIC_VIEW_DELTA)
		return (RELIC_OP_SCAN);

	if (bound == all_columns(relation->arity))
		return (RELIC_OP_MEMBER);

	if (bound == 0 ||
	    (planner->ctx->config.flags & RELIC_F_NO_INDEX) != 0)
		return (RELIC_OP_SCAN);

	return (RELIC_OP_LOOKUP);
}

/*
 * Place a positive literal as the operation positive_kind() chooses.  A
 * LOOKUP names the index keyed on its bound columns, which is built here when
 * the relation has none, so a plan never names an index that does not exist.
 *
 * The bound columns are taken before any term is decided, because deciding
 * a scan's terms binds slots: in edge(X, X) with X unbound, the second
 * column is matched against the first rather than looked up.
 */
static int
place_atom(relic_planner_t *planner, const relic_atom_t *atom,
    enum relic_view view)
{
	relic_relation_t *relation;
	enum relic_op_kind kind;
	relic_op_t *op;
	uint32_t bound;
	uint32_t index_position;
	uint16_t i;

	relation = relic_relation_for(planner->ctx, atom->pred);
	if (relation == NULL)
		return (-1);

	bound = bound_columns(planner, atom, relation->arity);
	kind = positive_kind(planner, relation, bound, view);

	index_position = 0;
	if (kind == RELIC_OP_LOOKUP && relic_relation_index(planner->ctx,
	    relation, bound, &index_position) != 0)
		return (-1);

	op = append_atom_op(planner, kind, relation);
	if (op == NULL)
		return (-1);
	op->u.atom.view = view;
	if (kind == RELIC_OP_LOOKUP) {
		op->u.atom.key_mask = bound;
		op->u.atom.index_position = index_position;
	}

	for (i = 0; i < relation->arity; i++) {
		if (kind == RELIC_OP_MEMBER)
			read_term(&atom->args[i], &op->u.atom.terms[i]);
		else
			scan_term(planner, &atom->args[i],
			    &op->u.atom.terms[i]);
	}

	return (0);
}

/*
 * The slots a run of arguments reads.
 */
static uint64_t
arg_slots(const relic_arg_t *args, uint16_t count)
{
	uint64_t slots;
	uint16_t i;

	slots = 0;
	for (i = 0; i < count; i++) {
		if (args[i].kind == RELIC_ARG_VARIABLE)
			slots |= slot_bit(args[i].u.slot);
	}

	return (slots);
}

static uint64_t
comparison_slots(const relic_comparison_t *comparison)
{
	return (arg_slots(&comparison->left, 1) |
	    arg_slots(&comparison->right, 1));
}

/*
 * Place a negated literal as an ANTI_MEMBER that reads every column.  The
 * caller places it only where every argument is bound, so each column is a
 * constant or a bound slot.
 */
static int
place_negated(relic_planner_t *planner, const relic_atom_t *atom)
{
	relic_relation_t *relation;
	relic_op_t *op;
	uint16_t i;

	relation = relic_relation_for(planner->ctx, atom->pred);
	if (relation == NULL)
		return (-1);

	op = append_atom_op(planner, RELIC_OP_ANTI_MEMBER, relation);
	if (op == NULL)
		return (-1);

	for (i = 0; i < relation->arity; i++)
		read_term(&atom->args[i], &op->u.atom.terms[i]);

	return (0);
}

/*
 * Place a comparison as a FILTER reading its two operands.  The caller places
 * it only where both are bound.
 */
static void
place_comparison(relic_planner_t *planner, const relic_literal_t *literal)
{
	const relic_comparison_t *comparison;
	relic_op_t *op;

	comparison = &literal->u.comparison;

	op = append_op(planner, RELIC_OP_FILTER);
	op->u.filter.compare = comparison->op;
	op->u.filter.where = literal->where;
	read_term(&comparison->left, &op->u.filter.left);
	read_term(&comparison->right, &op->u.filter.right);
}

/*
 * Whether a literal is a test -- a comparison or a negated literal -- that
 * can be placed now, which is when every slot it reads is bound.  This is
 * the one place a literal is classified as a test; a positive literal is
 * never one.
 *
 * A negated literal is ready exactly when it would be a membership test were
 * it positive, so the one question answers both.  One holding `_` is never
 * ready; safety rejects such a rule, and should one arrive regardless it is
 * left out of the plan, which confirm_plan_complete() reports.
 */
static bool
test_is_ready(relic_t *ctx, const relic_literal_t *literal, uint64_t bound)
{
	const relic_pred_t *pred;

	switch (literal->kind) {
	case RELIC_LITERAL_COMPARISON:
		return ((comparison_slots(&literal->u.comparison) & ~bound) ==
		    0);
	case RELIC_LITERAL_NEGATED:
		pred = relic_pred_of(ctx, literal->u.atom.pred);
		return (pred != NULL &&
		    atom_is_bound(&literal->u.atom, pred->arity, bound));
	case RELIC_LITERAL_POSITIVE:
		break;
	}

	return (false);
}

/*
 * Place a literal test_is_ready() accepted as the operation that tests it.
 * A positive literal cannot arrive here, and is reported as the internal
 * error it would be rather than dropped from the plan.
 */
static int
place_test(relic_planner_t *planner, const relic_literal_t *literal)
{
	switch (literal->kind) {
	case RELIC_LITERAL_COMPARISON:
		place_comparison(planner, literal);
		return (0);
	case RELIC_LITERAL_NEGATED:
		return (place_negated(planner, &literal->u.atom));
	case RELIC_LITERAL_POSITIVE:
		break;
	}

	relic_diagf(planner->ctx, RELIC_ERROR, &literal->where,
	    "internal error: a positive literal was placed as a test");

	return (-1);
}

/*
 * Place every test not yet placed whose slots are now bound, in the order
 * the tests are written.
 *
 * Called before the first literal and after every positive literal that
 * binds a slot, which are the only points at which a test can become ready,
 * so each test is placed at the earliest point it can be.
 */
static void
order_ready_tests(relic_planner_t *planner)
{
	const relic_rule_t *rule;
	size_t i;

	rule = planner->rule;

	for (i = 0; i < rule->body_length; i++) {
		if (planner->placed[i] || !test_is_ready(planner->ctx,
		    &rule->body[i], planner->bound))
			continue;

		planner->order[planner->order_count] = i;
		planner->order_count++;
		planner->placed[i] = true;
	}
}

/*
 * Place the positive literal at body index `index`, which binds every
 * variable it mentions, then, when that bound a slot not bound before,
 * every test it made placeable.
 */
static void
order_positive(relic_planner_t *planner, size_t index)
{
	const relic_atom_t *atom;
	const relic_pred_t *pred;
	uint64_t bound_before;

	atom = &planner->rule->body[index].u.atom;
	pred = relic_pred_of(planner->ctx, atom->pred);
	bound_before = planner->bound;

	planner->order[planner->order_count] = index;
	planner->order_count++;
	planner->placed[index] = true;
	if (pred != NULL)
		planner->bound |= arg_slots(atom->args, pred->arity);

	if (planner->bound != bound_before)
		order_ready_tests(planner);
}

/*
 * Whether a predicate's relation is one still being derived, which the
 * estimate cannot measure.
 */
static bool
is_incomplete(const bool *incomplete, relic_pred_id_t pred)
{
	return (incomplete != NULL && incomplete[pred - 1]);
}

/*
 * The quotient rounded up, for a divisor that is not zero.  Written without
 * adding to the dividend, which could wrap.
 */
static size_t
divide_rounding_up(size_t dividend, size_t divisor)
{
	return (dividend / divisor + (dividend % divisor != 0));
}

/*
 * The estimate for a relation still being derived: the assumed size,
 * narrowed by the assumed selectivity of each bound column.
 */
static size_t
estimate_unknown(uint16_t arity, uint32_t bound)
{
	size_t estimate;
	uint16_t column;

	estimate = RELIC_PLAN_UNKNOWN_TUPLES;
	for (column = 0; column < arity; column++) {
		if ((bound & (UINT32_C(1) << column)) != 0)
			estimate = divide_rounding_up(estimate,
			    RELIC_PLAN_UNKNOWN_DISTINCT);
	}

	return (estimate);
}

/*
 * The estimated number of tuples of a complete relation matching one binding
 * of the columns in `columns`, as the file comment describes.  Returns 0, or -1
 * with a diagnostic reported when a distinct count could not be computed.
 *
 * A column of a nonempty relation holds at least one distinct value, so no
 * division is by zero.
 */
static int
estimate_measured(relic_t *ctx, relic_relation_t *relation, uint32_t columns,
    size_t *estimatep)
{
	size_t distinct;
	size_t estimate;
	uint16_t column;

	estimate = relic_relation_count(relation);
	for (column = 0; estimate != 0 && column < relation->arity; column++) {
		if ((columns & (UINT32_C(1) << column)) == 0)
			continue;

		if (relic_relation_distinct(ctx, relation, column,
		    &distinct) != 0)
			return (-1);

		estimate = divide_rounding_up(estimate, distinct);
	}

	*estimatep = estimate;

	return (0);
}

/*
 * Score a positive literal against the slots in `bound`.  Returns 0, or -1
 * with a diagnostic reported.  `incomplete` is the planner's.
 *
 * A relation still being derived is not even looked up: its estimate is an
 * assumption, and looking it up would create a relation that the Magic
 * rewrite, which orders rules over predicates it never evaluates, would leave
 * lying empty in the context.
 */
static int
score_positive(relic_t *ctx, const bool *incomplete, const relic_atom_t *atom,
    uint64_t bound, relic_plan_score_t *scorep)
{
	const relic_pred_t *pred;
	relic_relation_t *relation;
	uint32_t columns;

	pred = relic_pred_of(ctx, atom->pred);
	if (pred == NULL) {
		relic_diagf(ctx, RELIC_ERROR, &atom->where,
		    "internal error: a literal names no predicate");
		return (-1);
	}

	columns = relic_plan_bound_columns(atom, pred->arity, bound);
	scorep->bound_args = column_count(columns);

	if (is_incomplete(incomplete, atom->pred)) {
		scorep->estimate = estimate_unknown(pred->arity, columns);
		return (0);
	}

	relation = relic_relation_for(ctx, atom->pred);
	if (relation == NULL)
		return (-1);

	return (estimate_measured(ctx, relation, columns, &scorep->estimate));
}

/*
 * The order of two candidates in a ranking, best first: more bound arguments,
 * then the smaller estimate, then the one written first.  Body indexes are
 * distinct, so no two different candidates compare equal and the sort has
 * one result.
 */
static int
compare_candidates(const void *a, const void *b)
{
	const relic_plan_candidate_t *left;
	const relic_plan_candidate_t *right;

	left = a;
	right = b;

	if (left->score.bound_args != right->score.bound_args)
		return (left->score.bound_args > right->score.bound_args ?
		    -1 : 1);

	if (left->score.estimate != right->score.estimate)
		return (left->score.estimate < right->score.estimate ? -1 : 1);

	return (left->index < right->index ? -1 :
	    left->index > right->index);
}

/*
 * Begin a pass: rank every positive literal not yet placed against the slots
 * bound now, best first.  Returns 0, or -1 with a diagnostic reported when a
 * literal could not be scored.
 *
 * Literals are scored in body order, and each against the bound set of its
 * pass, so a statistic that cannot be computed fails at the first literal
 * the greedy order's comparison at that bound set would have scored.  Under
 * RELIC_F_NO_REORDER nothing is scored, every score is equal, and the
 * ranking is body order.
 */
static int
rank_positives(relic_planner_t *planner)
{
	const relic_rule_t *rule;
	relic_plan_candidate_t *candidate;
	bool reorder;
	size_t i;

	rule = planner->rule;
	reorder = (planner->ctx->config.flags & RELIC_F_NO_REORDER) == 0;
	planner->ranking_count = 0;

	for (i = 0; i < rule->body_length; i++) {
		if (rule->body[i].kind != RELIC_LITERAL_POSITIVE ||
		    planner->placed[i])
			continue;

		candidate = &planner->ranking[planner->ranking_count];
		planner->ranking_count++;
		bzero(candidate, sizeof(*candidate));
		candidate->index = i;

		if (reorder && score_positive(planner->ctx, planner->incomplete,
		    &rule->body[i].u.atom, planner->bound,
		    &candidate->score) != 0)
			return (-1);
	}

	if (planner->ranking_count > 1)
		qsort(planner->ranking, planner->ranking_count,
		    sizeof(*planner->ranking), compare_candidates);

	return (0);
}

/*
 * Place literals from the front of the ranking until one binds a new slot or
 * the ranking runs out.
 *
 * While the bound set stays as the pass found it, every score in the ranking
 * is still what it was when the pass began, so the best literal not yet
 * placed is the next one in the ranking.  Once a placement binds a slot the
 * rest of the ranking may be stale, and the pass ends.
 */
static void
order_ranked(relic_planner_t *planner)
{
	uint64_t bound_at_start;
	size_t i;

	bound_at_start = planner->bound;

	for (i = 0; i < planner->ranking_count &&
	    planner->bound == bound_at_start; i++)
		order_positive(planner, planner->ranking[i].index);
}

/*
 * How many positive literals a rule's body holds.
 */
static size_t
positive_count(const relic_rule_t *rule)
{
	size_t count;
	size_t i;

	count = 0;
	for (i = 0; i < rule->body_length; i++) {
		if (rule->body[i].kind == RELIC_LITERAL_POSITIVE)
			count++;
	}

	return (count);
}

/*
 * Decide the order: the tests ready against the slots the caller started
 * with, the delta literal when there is one, then the other positive
 * literals in passes, as the file comment describes, and each test at the
 * earliest point its slots are bound.
 *
 * Every pass places at least one literal, and every pass but the last ends
 * because the bound set grew, so there are no more passes than positive
 * literals, and at most 65: one more than the 64 slots.
 */
static int
order_body(relic_planner_t *planner)
{
	order_ready_tests(planner);

	if (planner->delta_literal != RELIC_PLAN_NO_DELTA)
		order_positive(planner, planner->delta_literal);

	for (;;) {
		if (rank_positives(planner) != 0)
			return (-1);

		if (planner->ranking_count == 0)
			return (0);

		order_ranked(planner);
	}
}

/*
 * The view the positive literal at body index `index` reads through: `all`
 * in an ordinary plan and for a complete relation, and in a delta variant
 * `delta` for the delta literal and `old` or `current` for a literal over
 * another relation still being derived, as it is written before or after
 * the delta literal.
 */
static enum relic_view
positive_view(const relic_planner_t *planner, size_t index)
{
	const relic_atom_t *atom;

	atom = &planner->rule->body[index].u.atom;

	if (planner->delta_literal == RELIC_PLAN_NO_DELTA)
		return (RELIC_VIEW_ALL);

	if (index == planner->delta_literal)
		return (RELIC_VIEW_DELTA);

	if (!is_incomplete(planner->incomplete, atom->pred))
		return (RELIC_VIEW_ALL);

	return (index < planner->delta_literal ? RELIC_VIEW_OLD :
	    RELIC_VIEW_CURRENT);
}

/*
 * Emit one operation per literal, in the order order_body() decided, from
 * nothing bound.  Each operation binds what the literal bound when the order
 * was decided, so every literal meets the bound set it was placed against.
 */
static int
emit_body(relic_planner_t *planner)
{
	const relic_literal_t *literal;
	size_t index;
	size_t i;

	planner->bound = 0;

	for (i = 0; i < planner->order_count; i++) {
		index = planner->order[i];
		literal = &planner->rule->body[index];

		if (literal->kind == RELIC_LITERAL_POSITIVE) {
			if (place_atom(planner, &literal->u.atom,
			    positive_view(planner, index)) != 0)
				return (-1);
		} else if (place_test(planner, literal) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Confirm, once the body has been placed, that every body literal became
 * exactly one operation and that every head argument is bound.  Returns 0,
 * or -1 having reported which did not hold.
 *
 * A checked rule always passes.  Safety guarantees that every slot a test
 * or the head reads is bound by some positive literal, so once every
 * positive literal is placed, every test has been too.  The
 * question is asked anyway, once per plan, because failing it would be
 * silent: a literal missing from the plan is a filter nobody applies, and an
 * unbound head argument is a column read from whatever the frame last held.
 *
 * Counting operations rather than checking each kind of literal is what makes
 * the check cover every kind there is.  A kind of literal the planner does
 * not place leaves the count short, rather than vanishing from the plan
 * because nobody thought to look for it.
 */
static int
confirm_plan_complete(const relic_planner_t *planner)
{
	const relic_rule_t *rule;
	uint64_t unbound;

	rule = planner->rule;
	unbound = arg_slots(rule->head_args, rule->head_arity) &
	    ~planner->bound;

	if (planner->op_count == rule->body_length && unbound == 0)
		return (0);

	relic_diagf(planner->ctx, RELIC_ERROR, &rule->where,
	    "internal error: the plan leaves a literal out or reads a "
	    "variable nothing binds");

	return (-1);
}

/*
 * Confirm that the order holds every literal of the body.  A checked rule
 * always passes, for the reason confirm_plan_complete() gives; a literal left
 * out would be a filter nobody applies.
 */
static int
confirm_order_complete(const relic_planner_t *planner)
{
	if (planner->order_count == planner->rule->body_length)
		return (0);

	relic_diagf(planner->ctx, RELIC_ERROR, &planner->rule->where,
	    "internal error: the planner could not place every literal");

	return (-1);
}

/*
 * Confirm that the delta literal, when one is asked for, is a positive
 * literal of the rule over a relation `incomplete` marks as still being
 * derived.  Anything else is a caller's mistake that would not fail on its
 * own: an index past the body would be read, a comparison or a negated
 * literal taken for an atom, and without the marks every other member
 * literal would read the whole relation, deriving what other variants
 * derive too.
 */
static int
confirm_delta_literal(const relic_planner_t *planner)
{
	const relic_rule_t *rule;
	size_t index;

	rule = planner->rule;
	index = planner->delta_literal;

	if (index == RELIC_PLAN_NO_DELTA)
		return (0);

	if (index >= rule->body_length ||
	    rule->body[index].kind != RELIC_LITERAL_POSITIVE) {
		relic_diagf(planner->ctx, RELIC_ERROR, &rule->where,
		    "internal error: a delta variant was asked for a literal "
		    "that is not a positive literal of the rule");
		return (-1);
	}

	if (!is_incomplete(planner->incomplete,
	    rule->body[index].u.atom.pred)) {
		relic_diagf(planner->ctx, RELIC_ERROR, &rule->where,
		    "internal error: a delta variant was asked for a literal "
		    "over a relation not being derived");
		return (-1);
	}

	return (0);
}

/*
 * Confirm that the relation the EMIT writes has a column for each head
 * argument, no more and no fewer.  The EMIT reads `head_arity` arguments into
 * a tuple of the relation's arity, so this is what makes the head arity a
 * bound the plan can trust: a rule built with the wrong one, or handed the
 * wrong relation, fails here rather than reading past its arguments or
 * storing a tuple with columns nobody wrote.
 */
static int
confirm_target_fits(const relic_planner_t *planner)
{
	char described[RELIC_PRED_TEXT_MAX];

	if (planner->target->arity == planner->rule->head_arity)
		return (0);

	relic_diagf(planner->ctx, RELIC_ERROR, &planner->rule->where,
	    "internal error: a rule of %u head arguments was planned into "
	    "relation %s", (unsigned int)planner->rule->head_arity,
	    relic_relation_describe(planner->ctx, planner->target, described,
	    sizeof(described)));

	return (-1);
}

/*
 * Place the EMIT that ends every plan, reading the head's arguments into the
 * target relation.
 */
static int
place_emit(relic_planner_t *planner)
{
	const relic_rule_t *rule;
	relic_op_t *op;
	uint16_t i;

	rule = planner->rule;

	if (confirm_target_fits(planner) != 0)
		return (-1);

	if (confirm_plan_complete(planner) != 0)
		return (-1);

	op = append_atom_op(planner, RELIC_OP_EMIT, planner->target);
	if (op == NULL)
		return (-1);

	for (i = 0; i < rule->head_arity; i++)
		read_term(&rule->head_args[i], &op->u.atom.terms[i]);

	return (0);
}

/*
 * Prepare to order a rule's body: the record of which literals are placed,
 * in the arena, and room for a ranking of its positive literals, which
 * planner_release() frees.  `target` is the relation the EMIT writes, or NULL
 * when only the order is wanted.  The caller supplies where the order goes.
 * Returns -1 when an allocation was refused, which marks the arena failed
 * whichever allocation it was, so the caller's boundary reports it.
 */
static int
planner_init(relic_planner_t *planner, relic_t *ctx, relic_arena_t *arena,
    const relic_rule_t *rule, relic_relation_t *target, size_t delta_literal,
    const bool *incomplete)
{
	size_t count;

	bzero(planner, sizeof(*planner));
	planner->ctx = ctx;
	planner->arena = arena;
	planner->rule = rule;
	planner->target = target;
	planner->incomplete = incomplete;
	planner->delta_literal = delta_literal;

	planner->placed = relic_arena_allocarray(arena, rule->body_length,
	    sizeof(*planner->placed));
	if (planner->placed == NULL)
		return (-1);
	bzero(planner->placed, rule->body_length * sizeof(bool));

	count = positive_count(rule);
	planner->ranking = calloc(count, sizeof(*planner->ranking));
	if (planner->ranking == NULL && count > 0) {
		relic_arena_fail(arena);
		return (-1);
	}

	return (0);
}

/*
 * Free the ranking, the one part of a planner not in the caller's arena.
 */
static void
planner_release(relic_planner_t *planner)
{
	free(planner->ranking);
	planner->ranking = NULL;
}

/*
 * Decide the order of the body and confirm it is complete.
 */
static int
decide_order(relic_planner_t *planner)
{
	if (confirm_delta_literal(planner) != 0)
		return (-1);

	if (order_body(planner) != 0)
		return (-1);

	return (confirm_order_complete(planner));
}

/*
 * Build the plan: room for the order and the operations, the order, then an
 * operation per literal and the EMIT.  The body length comes from a compiled
 * rule held in memory, so one more than it cannot wrap.
 */
static int
build_plan(relic_planner_t *planner)
{
	size_t length;

	length = planner->rule->body_length;

	planner->order = relic_arena_allocarray(planner->arena, length,
	    sizeof(*planner->order));
	planner->ops = relic_arena_allocarray(planner->arena, length + 1,
	    sizeof(*planner->ops));
	if (planner->order == NULL || planner->ops == NULL)
		return (-1);

	if (decide_order(planner) != 0)
		return (-1);

	if (emit_body(planner) != 0)
		return (-1);

	return (place_emit(planner));
}

/*
 * Report an arena refusal once, however far into planning it struck, and
 * turn it into the failure of the whole call.  A relation that could not be
 * created, or a statistic that could not be computed, reported itself.
 */
static int
planning_status(relic_t *ctx, relic_arena_t *arena, int status)
{
	if (relic_arena_failed(arena)) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	return (status);
}

/*
 * Plan a rule.  Returns 0 with the plan in *plan, or -1 with a diagnostic
 * reported: memory or the memory limit refused an index or a statistic, or
 * the planner broke one of a plan's invariants, which is an internal error.
 *
 * The EMIT writes into `target`, whose arity must be the rule's head arity.
 * `delta_literal` is RELIC_PLAN_NO_DELTA for the rule's ordinary plan, or
 * the body index of a positive literal for the semi-naive variant in which
 * that literal reads the delta and is placed first.  `incomplete`, when not
 * NULL, is indexed by predicate identifier minus one and marks the relations
 * still being derived; every other relation is measured as it stands.
 *
 * The plan and everything it points at except its relations are allocated
 * from `arena`, and live as long as the caller keeps it.  The relations the
 * body reads, and the indexes its lookups read, are created here if they do
 * not yet exist; they belong to the context, which is what lets a plan hold
 * them.  The target belongs to the caller, who keeps it for as long as the
 * plan.
 */
int
relic_plan_rule(relic_t *ctx, relic_arena_t *arena, const relic_rule_t *rule,
    relic_relation_t *target, size_t delta_literal, const bool *incomplete,
    relic_plan_t *plan)
{
	relic_planner_t planner;
	int status;

	bzero(plan, sizeof(*plan));

	status = planner_init(&planner, ctx, arena, rule, target,
	    delta_literal, incomplete);
	if (status == 0)
		status = build_plan(&planner);
	planner_release(&planner);

	if (planning_status(ctx, arena, status) != 0)
		return (-1);

	plan->ops = planner.ops;
	plan->op_count = planner.op_count;

	return (0);
}

/*
 * The order the planner would place a rule's body literals in, starting from
 * the slots in `bound` rather than from nothing.  Fills `order`, which has
 * room for the body's length, with body indexes.  Returns 0, or -1 with a
 * diagnostic reported: memory refused a statistic or the arena its scratch,
 * or the order left a literal out, which is an internal error.
 *
 * `incomplete` is as for relic_plan_rule().  Nothing is emitted and no index
 * is built; the relations of complete predicates are measured, and created
 * if they do not yet exist.  Scratch space comes from `arena`.
 */
int
relic_plan_order(relic_t *ctx, relic_arena_t *arena, const relic_rule_t *rule,
    uint64_t bound, const bool *incomplete, size_t *order)
{
	relic_planner_t planner;
	int status;

	status = planner_init(&planner, ctx, arena, rule, NULL,
	    RELIC_PLAN_NO_DELTA, incomplete);
	if (status == 0) {
		planner.order = order;
		planner.bound = bound;
		status = decide_order(&planner);
	}
	planner_release(&planner);

	return (planning_status(ctx, arena, status));
}
