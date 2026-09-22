/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <strings.h>

#include "internal.h"

/*
 * The planner: a compiled rule becomes an ordered array of operations.
 *
 * The walk keeps one set, the slots bound so far, and turns each literal into
 * an operation at the point it is placed.  A positive literal whose every
 * argument is already bound is a membership test; any other positive literal
 * is a scan that binds what it can.  A comparison and a negated literal bind
 * nothing -- each is a test, a FILTER or an ANTI_MEMBER -- so each can only be
 * placed where every slot it reads is bound, and it is placed at the earliest
 * such point.  The head becomes the EMIT that ends the plan.
 *
 * Positive literals are placed in the order they are written.  A test cannot
 * always be: its written position need not be one where its operands exist
 * -- `N >= 100, size(X, N)` and `not done(X), todo(X)` are safe bodies -- so
 * it goes where it first can.  The rule's safety is what guarantees such a
 * point exists: every slot a test reads is bound by some positive literal, so
 * once every positive literal is placed, every test can be.
 *
 * Each column's fate is decided here, while the bound set is known: matched
 * against a constant, matched against a slot already bound, binding its
 * slot, or ignored.  The executor then makes no decision of its own, and a
 * better planner is a change to this file and never to the one that runs
 * what it emits.
 *
 * A semi-naive delta variant of a rule is the same walk with one literal
 * placed ahead of the others: the delta literal, which reads only the tuples
 * its relation gained in the previous iteration.  It is always a SCAN, even
 * when every argument is a constant, because a membership test would consult
 * the whole relation rather than the delta.  Placed first, it is only ever
 * scanned, so a delta never needs an index of its own.
 */

typedef struct relic_planner relic_planner_t;

/*
 * A plan under construction.
 *
 * `ops` has room for one operation per body literal and one EMIT, which is
 * exactly what a plan holds.  `placed` records which tests have been placed,
 * indexed like the body.  `bound` is the set of slots the operations
 * placed so far bind.  `delta_literal` is the body index of the literal that
 * reads the delta, or RELIC_PLAN_NO_DELTA.
 */
struct relic_planner {
	relic_t			*ctx;
	relic_arena_t		*arena;
	const relic_rule_t	*rule;
	relic_op_t		*ops;
	bool			*placed;
	size_t			 op_count;
	size_t			 delta_literal;
	uint64_t		 bound;
};

static relic_op_t *append_atom_op(relic_planner_t *planner,
		    enum relic_op_kind kind, relic_relation_t *relation);
static relic_op_t *append_op(relic_planner_t *planner,
		    enum relic_op_kind kind);
static uint64_t	 arg_slots(const relic_arg_t *args, uint16_t count);
static bool	 atom_is_bound(const relic_planner_t *planner,
		    const relic_atom_t *atom, uint16_t arity);
static int	 build_plan(relic_planner_t *planner);
static uint64_t	 comparison_slots(const relic_comparison_t *comparison);
static int	 confirm_delta_literal(const relic_planner_t *planner);
static int	 confirm_plan_complete(const relic_planner_t *planner,
		    uint16_t head_arity);
static int	 place_atom(relic_planner_t *planner, const relic_atom_t *atom,
		    bool delta);
static int	 place_body(relic_planner_t *planner);
static void	 place_comparison(relic_planner_t *planner,
		    const relic_comparison_t *comparison);
static int	 place_emit(relic_planner_t *planner);
static int	 place_negated(relic_planner_t *planner,
		    const relic_atom_t *atom);
static int	 place_positive(relic_planner_t *planner, size_t index);
static int	 place_ready_tests(relic_planner_t *planner);
static int	 place_test(relic_planner_t *planner,
		    const relic_literal_t *literal);
static int	 planner_init(relic_planner_t *planner, relic_t *ctx,
		    relic_arena_t *arena, const relic_rule_t *rule,
		    size_t delta_literal);
static void	 read_term(const relic_arg_t *arg, relic_plan_term_t *term);
static void	 scan_term(relic_planner_t *planner, const relic_arg_t *arg,
		    relic_plan_term_t *term);
static uint64_t	 slot_bit(uint16_t slot);
static relic_plan_term_t *terms_alloc(relic_planner_t *planner,
		    uint16_t arity);
static bool	 test_is_ready(const relic_planner_t *planner,
		    const relic_literal_t *literal);

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
 * Whether every argument of an atom is a constant or a bound slot, which is
 * what makes it a membership test rather than a scan.  A wildcard is never
 * bound: it asks for any value, and only a scan can supply one.
 */
static bool
atom_is_bound(const relic_planner_t *planner, const relic_atom_t *atom,
    uint16_t arity)
{
	const relic_arg_t *arg;
	uint16_t i;

	for (i = 0; i < arity; i++) {
		arg = &atom->args[i];

		if (arg->kind == RELIC_ARG_WILDCARD)
			return (false);

		if (arg->kind == RELIC_ARG_VARIABLE &&
		    (planner->bound & slot_bit(arg->u.slot)) == 0)
			return (false);
	}

	return (true);
}

/*
 * Place a positive literal: a MEMBER when it is fully bound, a SCAN that
 * binds the rest otherwise.  A literal that reads the delta is a SCAN
 * whatever is bound, since only a scan walks the delta's range.
 */
static int
place_atom(relic_planner_t *planner, const relic_atom_t *atom, bool delta)
{
	relic_relation_t *relation;
	enum relic_op_kind kind;
	relic_op_t *op;
	uint16_t i;

	relation = relic_relation_for(planner->ctx, atom->pred);
	if (relation == NULL)
		return (-1);

	if (!delta && atom_is_bound(planner, atom, relation->arity))
		kind = RELIC_OP_MEMBER;
	else
		kind = RELIC_OP_SCAN;

	op = append_atom_op(planner, kind, relation);
	if (op == NULL)
		return (-1);
	op->u.atom.delta = delta;

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
place_comparison(relic_planner_t *planner,
    const relic_comparison_t *comparison)
{
	relic_op_t *op;

	op = append_op(planner, RELIC_OP_FILTER);
	op->u.filter.compare = comparison->op;
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
test_is_ready(const relic_planner_t *planner, const relic_literal_t *literal)
{
	const relic_pred_t *pred;

	switch (literal->kind) {
	case RELIC_LITERAL_COMPARISON:
		return ((comparison_slots(&literal->u.comparison) &
		    ~planner->bound) == 0);
	case RELIC_LITERAL_NEGATED:
		pred = relic_pred_of(planner->ctx, literal->u.atom.pred);
		return (pred != NULL &&
		    atom_is_bound(planner, &literal->u.atom, pred->arity));
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
		place_comparison(planner, &literal->u.comparison);
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
 * Called before the first literal and after every positive literal, which
 * are the only points at which the bound set changes, so each test is placed
 * at the earliest point it can be.
 */
static int
place_ready_tests(relic_planner_t *planner)
{
	const relic_rule_t *rule;
	const relic_literal_t *literal;
	size_t i;

	rule = planner->rule;

	for (i = 0; i < rule->body_length; i++) {
		literal = &rule->body[i];

		if (planner->placed[i] || !test_is_ready(planner, literal))
			continue;

		if (place_test(planner, literal) != 0)
			return (-1);
		planner->placed[i] = true;
	}

	return (0);
}

/*
 * Place the positive literal at body index `index`, then every test it made
 * placeable.
 */
static int
place_positive(relic_planner_t *planner, size_t index)
{
	if (place_atom(planner, &planner->rule->body[index].u.atom,
	    index == planner->delta_literal) != 0)
		return (-1);

	return (place_ready_tests(planner));
}

/*
 * Place the delta literal first when there is one, then the other positive
 * literals in the order they are written, and each test at the earliest
 * point its slots are bound.
 *
 * Tests are offered a place before the first literal and after every
 * positive one, which are the only points at which the bound set changes.
 */
static int
place_body(relic_planner_t *planner)
{
	const relic_rule_t *rule;
	size_t i;

	rule = planner->rule;

	if (place_ready_tests(planner) != 0)
		return (-1);

	if (planner->delta_literal != RELIC_PLAN_NO_DELTA &&
	    place_positive(planner, planner->delta_literal) != 0)
		return (-1);

	for (i = 0; i < rule->body_length; i++) {
		if (rule->body[i].kind != RELIC_LITERAL_POSITIVE ||
		    i == planner->delta_literal)
			continue;

		if (place_positive(planner, i) != 0)
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
confirm_plan_complete(const relic_planner_t *planner, uint16_t head_arity)
{
	const relic_rule_t *rule;
	uint64_t unbound;

	rule = planner->rule;
	unbound = arg_slots(rule->head_args, head_arity) & ~planner->bound;

	if (planner->op_count == rule->body_length && unbound == 0)
		return (0);

	relic_diagf(planner->ctx, RELIC_ERROR, &rule->where,
	    "internal error: the plan leaves a literal out or reads a "
	    "variable nothing binds");

	return (-1);
}

/*
 * Confirm that the delta literal, when one is asked for, is a positive
 * literal of the rule.  Anything else is a caller's mistake that would not
 * fail on its own: an index past the body would be read, and a comparison
 * or a negated literal taken for an atom.
 */
static int
confirm_delta_literal(const relic_planner_t *planner)
{
	const relic_rule_t *rule;
	size_t index;

	rule = planner->rule;
	index = planner->delta_literal;

	if (index == RELIC_PLAN_NO_DELTA || (index < rule->body_length &&
	    rule->body[index].kind == RELIC_LITERAL_POSITIVE))
		return (0);

	relic_diagf(planner->ctx, RELIC_ERROR, &rule->where,
	    "internal error: a delta variant was asked for a literal that is "
	    "not a positive literal of the rule");

	return (-1);
}

/*
 * Place the EMIT that ends every plan, reading the head's arguments.
 */
static int
place_emit(relic_planner_t *planner)
{
	const relic_rule_t *rule;
	relic_relation_t *relation;
	relic_op_t *op;
	uint16_t i;

	rule = planner->rule;

	relation = relic_relation_for(planner->ctx, rule->head);
	if (relation == NULL)
		return (-1);

	if (confirm_plan_complete(planner, relation->arity) != 0)
		return (-1);

	op = append_atom_op(planner, RELIC_OP_EMIT, relation);
	if (op == NULL)
		return (-1);

	for (i = 0; i < relation->arity; i++)
		read_term(&rule->head_args[i], &op->u.atom.terms[i]);

	return (0);
}

static int
planner_init(relic_planner_t *planner, relic_t *ctx, relic_arena_t *arena,
    const relic_rule_t *rule, size_t delta_literal)
{
	bzero(planner, sizeof(*planner));
	planner->ctx = ctx;
	planner->arena = arena;
	planner->rule = rule;
	planner->delta_literal = delta_literal;

	/*
	 * The body length comes from a compiled rule held in memory, so one
	 * more than it cannot wrap.
	 */
	planner->ops = relic_arena_allocarray(arena, rule->body_length + 1,
	    sizeof(*planner->ops));
	planner->placed = relic_arena_allocarray(arena, rule->body_length,
	    sizeof(*planner->placed));
	if (planner->ops == NULL || planner->placed == NULL)
		return (-1);

	bzero(planner->placed, rule->body_length * sizeof(bool));

	return (0);
}

static int
build_plan(relic_planner_t *planner)
{
	if (confirm_delta_literal(planner) != 0)
		return (-1);

	if (place_body(planner) != 0)
		return (-1);

	return (place_emit(planner));
}

/*
 * Plan a rule.  Returns 0 with the plan in *plan, or -1 with a diagnostic
 * reported: memory ran out, or the planner broke one of a plan's
 * invariants, which is an internal error.
 *
 * `delta_literal` is RELIC_PLAN_NO_DELTA for the rule's ordinary plan, or
 * the body index of a positive literal for the semi-naive variant in which
 * that literal reads the delta and is placed first.
 *
 * The plan and everything it points at except its relations are allocated
 * from `arena`, and live as long as the caller keeps it.  The relations are
 * created here if they do not yet exist; they belong to the context and never
 * move, which is what lets a plan hold them.
 */
int
relic_plan_rule(relic_t *ctx, relic_arena_t *arena, const relic_rule_t *rule,
    size_t delta_literal, relic_plan_t *plan)
{
	relic_planner_t planner;
	int status;

	bzero(plan, sizeof(*plan));

	status = planner_init(&planner, ctx, arena, rule, delta_literal);
	if (status == 0)
		status = build_plan(&planner);

	/*
	 * An arena refusal is reported here rather than where it happened, so
	 * that one exhaustion produces one message however far into the plan
	 * it struck.  A relation that could not be created reported itself.
	 */
	if (relic_arena_failed(arena)) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	if (status != 0)
		return (-1);

	plan->ops = planner.ops;
	plan->op_count = planner.op_count;

	return (0);
}
