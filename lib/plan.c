/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * The planner: a compiled rule becomes an ordered array of operations.
 *
 * The walk keeps one set, the slots bound so far, and turns each literal into
 * an operation at the point it is placed.  A positive literal whose every
 * argument is already bound is a membership test; one with some bound
 * arguments is a lookup through the index keyed on exactly those columns;
 * one with none bound is a scan.
 * A lookup and a scan bind what they can.  A positive literal that would
 * bind nothing the rule reads -- every argument it does not have bound is
 * `_` or a dead variable -- is an existence check instead, a test keyed on
 * its bound columns; as a generator it would continue the join once per
 * tuple it matched, each time with the same frame, so that k such literals
 * multiplied the join's rows by the product of their matches.
 *
 * A variable is dead when the rule mentions it exactly once, head included,
 * and that mention is in a positive literal.  Nothing binds it before that
 * literal, since nothing else holds it, and nothing reads it after, so it is
 * a `_` with a name: it binds nothing, and a generator matches anything in
 * its column.  Whether a variable is dead is a fact about the rule alone,
 * computed once as a mask before the order is decided, and the bound set
 * grows by every slot a literal binds that is not dead, in the order and in
 * the plan alike.  A literal's score reads the bound set only through its
 * own columns and a test reads no dead slot, so leaving dead slots out of
 * the bound set changes no score and no test's place: the order is the one
 * the walk would choose if dead variables were bound.
 *
 * A negated literal binds nothing,
 * and neither does a comparison, unless it is an equality one side of which
 * is a variable not yet bound while the other side is: that one is an
 * ASSIGN, and binds the variable.  Every other comparison is a FILTER and a
 * negated literal an ANTI_MEMBER, tests that can only be placed where every
 * slot they read is bound.  An aggregate is an AGGREGATE, placed where its
 * key -- the slots it shares with its group -- is bound, which binds its
 * result, or compares with it when something before it bound it.  Tests,
 * aggregates and assignments are placed at the earliest
 * point they can be, and an assignment placed can make more of them
 * placeable, so they are placed until none more is before the next positive
 * literal is chosen.  The head becomes the EMIT that ends the plan, writing
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
 * and a nonempty one never estimates below one.  A relation the component
 * being planned is still deriving is empty and cannot be measured; it is
 * taken to hold RELIC_PLAN_UNKNOWN_TUPLES tuples, or RELIC_PLAN_DELTAS_HELD
 * times as many when a variant reads it beside its delta, narrowed by
 * RELIC_PLAN_UNKNOWN_DISTINCT per bound column.  The arithmetic is integer
 * so that the same input gives the same plan on every platform.
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
 * exists: every slot a test reads is bound by some positive literal or by an
 * assignment the closure of the safety check reaches, and once every
 * positive literal is placed the bound set holds all of the first, so every
 * such assignment can then be placed, in the closure's order if no other,
 * and then every test.
 *
 * A slot bound by an ASSIGN of a computed integer is a number slot (plan.h),
 * and every operation reading it is told so as it is emitted.  An
 * expression becomes a postfix program, its tree in postfix order, in the
 * evaluation's arena with the rest of the plan.
 *
 * Each column's fate is decided here, while the bound set is known: matched
 * against a constant, matched against a slot already bound, binding its
 * slot, or ignored.  The executor then makes no decision of its own, and a
 * better planner is a change to this file and never to the one that runs
 * what it emits.
 *
 * A semi-naive delta variant of a rule is the same walk, in which one
 * literal, the delta literal, reads only the tuples its relation gained in
 * the previous round.  It becomes the operation its bound columns give it
 * where it is placed: a SCAN of the delta, a LOOKUP whose walk the index
 * starts at the delta through the key's frontier, or a MEMBER that holds
 * only for a tuple of the delta.
 *
 * Where the delta literal should go depends on the delta's size, which no
 * plan can know: a delta of one tuple costs little to scan, and a guard of
 * a thousand tuples costs a thousand lookups of the delta whatever it
 * holds.  So relic_plan_variant() decides two orders, by two policies,
 * before it emits anything.  The delta-first order pins the delta literal
 * before every other positive literal.  The input-first order ranks the
 * delta literal with the others, but below any of them binding as many of
 * its arguments, whatever its size: in
 *
 *	tc(X, Z) :- guard(X), par(X, Y), tc(Y, Z).
 *
 * guard and par go before the delta of tc however large guard is, and tc
 * is looked up on Y.  Neither policy reads a guess of the delta's size, so
 * which plans a variant has never depends on one.  Two identical orders are
 * kept once.  When two are kept, each has a recipe from which a round,
 * knowing the sizes of its views and the keys of its indexes, estimates
 * its work, and the cost of building the indexes it lacks, and runs the
 * cheaper (lib/cost.c).  Under RELIC_F_NO_REORDER
 * the delta literal stays where it is written and there is one order;
 * under RELIC_F_NO_INDEX alone, which would scan a deferred delta once per
 * binding before it, the delta goes first.
 *
 * Every positive literal of a variant reads through a view (plan.h), and the
 * view follows from body order alone: the delta literal reads `delta`, a
 * literal over a relation still being derived -- a member of the component,
 * which `incomplete` marks -- reads `old` when it is written before the
 * delta literal and `current` when it is written after it, and a literal
 * over a complete relation reads `all`.  A variant is refused unless its
 * delta literal reads a marked relation, so the marks a view is decided by
 * are always the component's.  The order the walk chooses decides what each
 * binding costs; the view decides which tuples a literal can match, and the
 * set of tuples a join produces does not depend on its nesting, so the two
 * questions are separate.
 *
 * The order is decided before any operation is built.  A first walk places
 * the literals -- recording, for each, where it goes and what it binds -- and
 * a second replays that order, turning each literal into its operation.  The
 * first walk builds no index and emits nothing, which is what lets the Magic
 * rewrite ask for the order alone, from a set of slots its caller already
 * binds, and so borrow the planner's notion of body order rather than keep
 * one of its own.
 *
 * The second walk emits a draft (plan.h), which names each index an
 * operation reads by its relation and mask and builds none.
 * relic_plan_prepare() makes the plan the executor runs: it has the
 * relation build every index the draft names and lacks, and gives each
 * operation the position of its index.  An ordinary plan is prepared as
 * soon as it is emitted.  A variant's candidates stay drafts, so that the
 * evaluation can prepare each when a round first runs it, and a plan no
 * round chooses costs its relations no index.  Whether an operation reads
 * a secondary index is decided in one place, op_index_read(), which both
 * the draft's list of indexes and the binding of positions follow.
 *
 * Three switches select the references the default is tested against.
 * RELIC_F_NO_INDEX makes every lookup a scan matching the same columns,
 * RELIC_F_NO_REORDER places positive literals in the order they are written,
 * and RELIC_F_NO_EXISTS plans no existence check: no variable is dead, and a
 * positive literal is a member, a lookup or a scan by its bound columns
 * alone.  Each changes how an answer is found and never what it is.
 */

/*
 * How a planner orders the positive literals.  GREEDY ranks every literal
 * alike, which is how an ordinary plan is ordered and, under
 * RELIC_F_NO_REORDER, where nothing is ranked, a written variant.
 * DELTA_FIRST pins a variant's delta literal before every other positive
 * literal.  INPUT_FIRST ranks the delta literal below any other literal
 * binding as many of its arguments.
 */
enum relic_order_policy {
	RELIC_POLICY_GREEDY,
	RELIC_POLICY_DELTA_FIRST,
	RELIC_POLICY_INPUT_FIRST
};

typedef struct relic_planner relic_planner_t;
typedef struct relic_ranked_literal relic_ranked_literal_t;
typedef struct relic_plan_score relic_plan_score_t;

/*
 * A plan under construction.
 *
 * `order` receives the body index of each literal as it is placed, and
 * `order_count` says how many have been; `placed` records the same thing
 * indexed like the body.  `bound` is the set of slots bound so far: while the
 * order is decided, by the literals placed and the slots the caller started
 * with; while the operations are emitted, by the operations emitted.  `dead`
 * is the set of the rule's dead variables, which no literal binds, and is
 * empty under RELIC_F_NO_EXISTS.
 * `numbers` and `texts` are the subsets of those bound to a computed
 * integer and to a computed string, the number slots and the text slots.
 * `text_base` is where the texts the next operation computes begin: one
 * more than the position of the last ASSIGN emitted that binds a text slot,
 * whose string they are built above, or zero when none has been.
 *
 * `ranking` holds the pass in progress: the `ranking_count` positive
 * literals not yet placed when it began, best first.  It has room for every
 * positive literal of the body, and is allocated for the one call and freed
 * when it returns, so that the plans an arena keeps do not keep it too.
 *
 * `ops` has room for one operation per body literal and one EMIT, which is
 * exactly what a plan holds, and is NULL when only the order is wanted.
 * `accesses` has as much room, and holds the `access_count` indexes the
 * operations emitted so far read, one per operation reading one, until
 * coalesce_accesses() keeps each index once.
 * `target` is the relation the EMIT writes, and NULL when only the order is
 * wanted.
 * `delta_literal` is the body index of the literal that reads the delta, or
 * RELIC_PLAN_NO_DELTA, and `policy` says where it goes.  `incomplete`, when
 * not NULL, is indexed by predicate identifier minus one and marks the
 * relations still being derived, which the estimate cannot measure.
 */
struct relic_planner {
	relic_t			*ctx;
	relic_arena_t		*arena;
	const relic_rule_t	*rule;
	relic_relation_t	*target;
	const bool		*incomplete;
	size_t			*order;
	bool			*placed;
	relic_ranked_literal_t	*ranking;
	relic_op_t		*ops;
	relic_plan_access_t	*accesses;
	size_t			 access_count;
	size_t			 order_count;
	size_t			 ranking_count;
	size_t			 op_count;
	size_t			 delta_literal;
	uint64_t		 bound;
	uint64_t		 dead;
	uint64_t		 numbers;
	uint64_t		 texts;
	size_t			 text_base;
	enum relic_order_policy	 policy;
};

/*
 * How good a positive literal would be to place next: more bound arguments
 * is better, then a literal that is not `deferred`, then a smaller
 * estimate.  Only the delta literal of an input-first order is deferred,
 * and its estimate is never computed, since no comparison reaches it.
 */
struct relic_plan_score {
	size_t		estimate;
	uint16_t	bound_args;
	bool		deferred;
};

/*
 * A positive literal in a ranking: its body index, and its score against the
 * bound set of the pass that ranked it.
 */
struct relic_ranked_literal {
	relic_plan_score_t	score;
	size_t			index;
};

static relic_op_t *append_atom_op(relic_planner_t *planner,
		    enum relic_op_kind kind, relic_relation_t *relation);
static relic_op_t *append_op(relic_planner_t *planner,
		    enum relic_op_kind kind);
static unsigned int append_steps(const relic_planner_t *planner,
		    const relic_expr_t *expr, relic_postfix_t *postfix);
static int	 access_compare(const void *left, const void *right);
static uint64_t	 arg_slots(const relic_arg_t *args, uint16_t count);
static bool	 atom_slots(const relic_op_atom_t *atom, uint64_t *bindsp,
		    uint64_t *readsp);
static int	 bind_index(relic_t *ctx, relic_op_t *op);
static void	 bind_slot(relic_planner_t *planner, uint16_t slot,
		    enum relic_slot_kind kind);
static uint32_t	 bound_columns(const relic_planner_t *planner,
		    const relic_atom_t *atom, uint16_t arity);
static int	 coalesce_accesses(relic_planner_t *planner);
static uint16_t	 column_count(uint32_t columns);
static int	 compare_ranked(const void *a, const void *b);
static int	 compile_postfix(relic_planner_t *planner,
		    const relic_expr_t *expr, const relic_location_t *where,
		    relic_postfix_t *postfix);
static int	 compile_recipe(relic_arena_t *arena,
		    const relic_plan_draft_t *draft,
		    relic_cost_recipe_t *recipe);
static int	 confirm_delta_literal(relic_t *ctx, const relic_rule_t *rule,
		    size_t delta_literal, const bool *incomplete);
static int	 confirm_order_complete(const relic_planner_t *planner);
static int	 confirm_plan_complete(const relic_planner_t *planner);
static int	 confirm_target_fits(const relic_planner_t *planner);
static uint64_t	 dead_slots(const relic_t *ctx, const relic_rule_t *rule);
static int	 decide_order(relic_planner_t *planner);
static size_t	 divide_rounding_up(size_t dividend, size_t divisor);
static int	 emit_body(relic_planner_t *planner);
static int	 emit_family(relic_planner_t *planners, size_t count,
		    relic_plan_family_t *family);
static int	 emit_draft(relic_planner_t *planner,
		    relic_plan_draft_t *draft);
static size_t	 estimate_unknown(uint16_t arity, uint32_t bound,
		    enum relic_view view);
static uint32_t	 free_columns(const relic_planner_t *planner,
		    const relic_atom_t *atom, uint16_t arity);
static int	 estimate_measured(relic_t *ctx, relic_relation_t *relation,
		    uint32_t columns, size_t *estimatep);
static enum relic_slot_kind expr_kind(const relic_planner_t *planner,
		    const relic_expr_t *expr);
static size_t	 family_policies(const relic_t *ctx,
		    enum relic_order_policy *policies);
static bool	 is_incomplete(const bool *incomplete, relic_pred_id_t pred);
static uint32_t	 named_columns(const relic_atom_t *atom, uint16_t arity);
static uint32_t	*op_index_read(relic_op_t *op,
		    const relic_relation_t **relationp, uint32_t *maskp);
static int	 order_body(relic_planner_t *planner);
static bool	 orders_equal(const relic_planner_t *left,
		    const relic_planner_t *right);
static void	 order_positive(relic_planner_t *planner, size_t index);
static void	 order_ranked(relic_planner_t *planner);
static void	 order_ready_tests(relic_planner_t *planner);
static int	 place_atom(relic_planner_t *planner, const relic_atom_t *atom,
		    enum relic_view view);
static int	 planning_status(relic_t *ctx, relic_arena_t *arena,
		    int status);
static int	 place_assign(relic_planner_t *planner,
		    const relic_literal_t *literal, uint16_t slot,
		    const relic_expr_t *value);
static int	 place_comparison(relic_planner_t *planner,
		    const relic_literal_t *literal);
static int	 place_aggregate(relic_planner_t *planner,
		    const relic_literal_t *literal);
static int	 place_emit(relic_planner_t *planner);
static int	 place_negated(relic_planner_t *planner,
		    const relic_atom_t *atom);
static int	 place_probe(relic_planner_t *planner, enum relic_op_kind kind,
		    const relic_atom_t *atom, relic_relation_t *relation,
		    enum relic_view view, uint32_t read);
static size_t	 positive_count(const relic_rule_t *rule);
static bool	 probe_is_bound(const relic_atom_t *atom, uint16_t arity,
		    uint64_t bound);
static enum relic_op_kind positive_kind(const relic_planner_t *planner,
		    const relic_relation_t *relation, uint32_t bound,
		    uint32_t unbound);
static uint32_t	 probe_key(const relic_planner_t *planner,
		    const relic_relation_t *relation, uint32_t read);
static enum relic_view positive_view(const relic_planner_t *planner,
		    size_t index);
static bool	 postfix_calls(const relic_postfix_t *postfix);
static bool	 postfix_computes(const relic_postfix_t *postfix);
static int	 place_test(relic_planner_t *planner,
		    const relic_literal_t *literal);
static int	 plan_family(relic_planner_t *planners, relic_t *ctx,
		    relic_arena_t *arena, const relic_rule_t *rule,
		    relic_relation_t *target, size_t delta_literal,
		    const bool *incomplete, relic_plan_family_t *family);
static int	 planner_init(relic_planner_t *planner, relic_t *ctx,
		    relic_arena_t *arena, const relic_rule_t *rule,
		    relic_relation_t *target, size_t delta_literal,
		    enum relic_order_policy policy, const bool *incomplete);
static int	 planner_prepare_order(relic_planner_t *planner);
static void	 planner_release(relic_planner_t *planner);
static int	 rank_positives(relic_planner_t *planner);
static void	 read_term(const relic_arg_t *arg, relic_plan_term_t *term);
static void	 record_access(relic_planner_t *planner,
		    relic_op_t *op, relic_relation_t *relation);
static void	 scan_term(relic_planner_t *planner, const relic_arg_t *arg,
		    relic_plan_term_t *term);
static int	 score_positive(relic_t *ctx, const bool *incomplete,
		    const relic_atom_t *atom, enum relic_view view,
		    uint64_t bound, bool deferred, relic_plan_score_t *scorep);
static uint64_t	 slot_bit(uint16_t slot);
static enum relic_slot_kind slot_kind(const relic_planner_t *planner,
		    uint16_t slot);
static enum relic_step_kind slot_step(enum relic_slot_kind kind);
static relic_plan_term_t *terms_alloc(relic_planner_t *planner,
		    uint16_t arity);
static bool	 test_is_placeable(relic_t *ctx,
		    const relic_literal_t *literal, uint64_t bound);

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
	op->u.atom.arity = relation->arity;
	op->u.atom.view = RELIC_VIEW_ALL;
	op->u.atom.terms = terms_alloc(planner, relation->arity);
	if (relation->arity > 0 && op->u.atom.terms == NULL)
		return (NULL);

	return (op);
}

/*
 * The secondary index an operation reads, if it reads one: its position
 * field, with the relation and the key mask in *relationp and *maskp, or
 * NULL.  A LOOKUP reads one, as do an EXISTS and an ANTI_MEMBER whose key
 * names some columns but not all, and an AGGREGATE folding through a
 * lookup.  The draft's list of indexes and the positions
 * relic_plan_prepare() binds both follow this, so no operation reads an
 * index its plan did not name.  The switch names every kind of operation,
 * so that one added later must be classified here rather than read an
 * index no draft names.
 */
static uint32_t *
op_index_read(relic_op_t *op, const relic_relation_t **relationp,
    uint32_t *maskp)
{
	relic_op_atom_t *atom;
	uint32_t *positionp;

	atom = &op->u.atom;
	positionp = NULL;
	switch (op->kind) {
	case RELIC_OP_SCAN:
	case RELIC_OP_MEMBER:
	case RELIC_OP_FILTER:
	case RELIC_OP_ASSIGN:
	case RELIC_OP_EMIT:
		break;
	case RELIC_OP_LOOKUP:
		positionp = &atom->index_position;
		break;
	case RELIC_OP_EXISTS:
	case RELIC_OP_ANTI_MEMBER:
		if (relic_plan_probe_shape(atom->key_mask, atom->arity) ==
		    RELIC_PROBE_INDEX)
			positionp = &atom->index_position;
		break;
	case RELIC_OP_AGGREGATE:
		if (op->u.aggregate.access != RELIC_FOLD_LOOKUP)
			break;
		*relationp = op->u.aggregate.relation;
		*maskp = op->u.aggregate.key_mask;
		return (&op->u.aggregate.index_position);
	}

	if (positionp != NULL) {
		*relationp = atom->relation;
		*maskp = atom->key_mask;
	}

	return (positionp);
}

/*
 * Record the index an operation just emitted over `relation` reads, if it
 * reads one.  The operation holds its relation read-only, and building the
 * index needs it writable, so the caller passes the relation it placed the
 * operation over.  There is always room: no operation reads more than one
 * index.  coalesce_accesses() keeps each index once when the draft is
 * complete.
 */
static void
record_access(relic_planner_t *planner, relic_op_t *op,
    relic_relation_t *relation)
{
	const relic_relation_t *read;
	relic_plan_access_t *access;
	uint32_t key_mask;

	if (op_index_read(op, &read, &key_mask) == NULL)
		return;
	assert(read == relation);

	access = &planner->accesses[planner->access_count];
	planner->access_count++;
	access->relation = relation;
	access->key_mask = key_mask;
}

/*
 * qsort(3) order of pointers into the recorded accesses: the accesses of
 * one index adjacent, and among them the one recorded first, first.  Only
 * the grouping is observable, so the relations may be ordered by address.
 */
static int
access_compare(const void *left, const void *right)
{
	const relic_plan_access_t *left_access;
	const relic_plan_access_t *right_access;

	left_access = *(const relic_plan_access_t *const *)left;
	right_access = *(const relic_plan_access_t *const *)right;

	if (left_access->relation != right_access->relation)
		return ((uintptr_t)left_access->relation <
		    (uintptr_t)right_access->relation ? -1 : 1);
	if (left_access->key_mask != right_access->key_mask)
		return (left_access->key_mask < right_access->key_mask ?
		    -1 : 1);
	if (left_access != right_access)
		return ((uintptr_t)left_access < (uintptr_t)right_access ?
		    -1 : 1);

	return (0);
}

/*
 * Keep each index the draft's operations read once, where it is first
 * read.  The accesses are sorted through pointers so that each index's are
 * adjacent, every one after the first of its index is marked a duplicate by
 * a NULL relation, and the rest close up in the order they were recorded,
 * which drops the duplicates: N log N for N accesses
 * however many distinct indexes a long body reads.  Returns -1 when the
 * pointers could not be allocated, which marks the arena failed so that
 * the caller's boundary reports it.
 */
static int
coalesce_accesses(relic_planner_t *planner)
{
	relic_plan_access_t **sorted;
	relic_plan_access_t *first;
	size_t count;
	size_t kept;
	size_t i;

	count = planner->access_count;
	if (count < 2)
		return (0);

	sorted = calloc(count, sizeof(*sorted));
	if (sorted == NULL) {
		relic_arena_fail(planner->arena);
		return (-1);
	}
	for (i = 0; i < count; i++)
		sorted[i] = &planner->accesses[i];
	qsort(sorted, count, sizeof(*sorted), access_compare);

	first = sorted[0];
	for (i = 1; i < count; i++) {
		if (sorted[i]->relation == first->relation &&
		    sorted[i]->key_mask == first->key_mask)
			sorted[i]->relation = NULL;
		else
			first = sorted[i];
	}
	free(sorted);

	kept = 0;
	for (i = 0; i < count; i++) {
		if (planner->accesses[i].relation != NULL)
			planner->accesses[kept++] = planner->accesses[i];
	}
	planner->access_count = kept;

	return (0);
}

/*
 * The term a test or an EMIT reads for an argument: a constant, a slot, or
 * for `_` a column the reader does not read.  The caller places the reader
 * only where its slots are bound, which confirm_plan_complete() checks once
 * per plan.
 *
 * No reader of a planned rule meets `_`: safety rejects it in the head and
 * in a comparison, a MEMBER is placed only where every column is a constant
 * or a bound slot, and a probe reads only the columns its caller names.
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
 * nothing has bound it yet, unless the variable is dead: nothing reads what
 * it would bind, so it is matched like `_` and binds nothing.
 *
 * The bound set is updated column by column, so a variable written twice in
 * one atom -- edge(X, X) -- binds at its first column and is matched at its
 * second against the value just bound.  Such a variable is never dead, and
 * neither is one bound before the atom, since a dead variable is held by
 * nothing else.
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

	if ((planner->dead & slot_bit(arg->u.slot)) != 0) {
		term->action = RELIC_PLAN_ANY;
		return;
	}

	term->action = RELIC_PLAN_BIND;
	planner->bound |= slot_bit(arg->u.slot);
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
 * The columns of an atom holding a variable that is neither bound nor dead:
 * those a generator placed now would bind, or match against what it binds.
 * A positive literal with none binds nothing the rule reads.
 */
static uint32_t
free_columns(const relic_planner_t *planner, const relic_atom_t *atom,
    uint16_t arity)
{
	const relic_arg_t *arg;
	uint32_t columns;
	uint16_t i;

	columns = 0;
	for (i = 0; i < arity; i++) {
		arg = &atom->args[i];

		if (arg->kind == RELIC_ARG_VARIABLE &&
		    ((planner->bound | planner->dead) &
		    slot_bit(arg->u.slot)) == 0)
			columns |= UINT32_C(1) << i;
	}

	return (columns);
}

/*
 * The columns of an atom that are not `_`, which are the columns a negated
 * literal reads.
 */
static uint32_t
named_columns(const relic_atom_t *atom, uint16_t arity)
{
	uint32_t columns;
	uint16_t i;

	columns = 0;
	for (i = 0; i < arity; i++) {
		if (atom->args[i].kind != RELIC_ARG_WILDCARD)
			columns |= UINT32_C(1) << i;
	}

	return (columns);
}

/*
 * Whether every column a negated literal reads holds a constant or a bound
 * slot, which is when its probe can be asked.
 */
static bool
probe_is_bound(const relic_atom_t *atom, uint16_t arity, uint64_t bound)
{
	return (relic_plan_bound_columns(atom, arity, bound) ==
	    named_columns(atom, arity));
}

/*
 * The operation a positive literal becomes with `bound` its bound columns and
 * `unbound` those free_columns() gives, whichever view it reads through:
 * MEMBER when every column is bound; EXISTS when some column is not but
 * none is free, so that every column not bound is `_` or a dead variable;
 * otherwise a generator, LOOKUP when some columns are bound and SCAN when
 * none are.  Under RELIC_F_NO_INDEX a literal that would be looked up is
 * scanned instead, and under RELIC_F_NO_EXISTS one that would be checked is
 * a generator, binding nothing and matching anything where it is `_`.
 */
static enum relic_op_kind
positive_kind(const relic_planner_t *planner,
    const relic_relation_t *relation, uint32_t bound, uint32_t unbound)
{
	if (bound == relic_plan_all_columns(relation->arity))
		return (RELIC_OP_MEMBER);

	if (unbound == 0 &&
	    (planner->ctx->config.flags & RELIC_F_NO_EXISTS) == 0)
		return (RELIC_OP_EXISTS);

	if (bound == 0 ||
	    (planner->ctx->config.flags & RELIC_F_NO_INDEX) != 0)
		return (RELIC_OP_SCAN);

	return (RELIC_OP_LOOKUP);
}

/*
 * Place a positive literal as the operation positive_kind() chooses: an
 * EXISTS probing by its bound columns, or a generator or a MEMBER.  A LOOKUP
 * reads the index keyed on its bound columns, which the draft names.
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
	uint16_t i;

	relation = relic_relation_read(planner->ctx, atom->pred);
	if (relation == NULL)
		return (-1);

	bound = bound_columns(planner, atom, relation->arity);
	kind = positive_kind(planner, relation, bound,
	    free_columns(planner, atom, relation->arity));
	if (kind == RELIC_OP_EXISTS)
		return (place_probe(planner, RELIC_OP_EXISTS, atom, relation,
		    view, bound));

	op = append_atom_op(planner, kind, relation);
	if (op == NULL)
		return (-1);
	op->u.atom.view = view;
	if (kind == RELIC_OP_LOOKUP)
		op->u.atom.key_mask = bound;
	record_access(planner, op, relation);

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

/*
 * The key a probe asks by, given `read`, the columns it reads: every column
 * when it reads them all, answered through the canonical index; the columns
 * read when they are a nonempty proper subset, answered through the
 * secondary index on them; and none, answered by a scan of the view, when
 * it reads no column or when such a subset is planned under
 * RELIC_F_NO_INDEX, as a lookup is planned as a scan.
 */
static uint32_t
probe_key(const relic_planner_t *planner, const relic_relation_t *relation,
    uint32_t read)
{
	if (read == relic_plan_all_columns(relation->arity))
		return (read);

	if ((planner->ctx->config.flags & RELIC_F_NO_INDEX) != 0)
		return (0);

	return (read);
}

/*
 * Place a probe of `relation` through `view`: an EXISTS or an ANTI_MEMBER
 * asking whether a tuple agrees with `atom` on the columns in `read`.  The
 * caller places it only where each of those columns is a constant or a
 * bound slot; every other column is one the probe does not read, `_` or a
 * dead variable, and its term is ANY.  A key that names some columns reads
 * the index on exactly them, as a lookup does.
 */
static int
place_probe(relic_planner_t *planner, enum relic_op_kind kind,
    const relic_atom_t *atom, relic_relation_t *relation,
    enum relic_view view, uint32_t read)
{
	relic_plan_term_t *term;
	relic_op_t *op;
	uint32_t key;
	uint16_t i;

	key = probe_key(planner, relation, read);

	op = append_atom_op(planner, kind, relation);
	if (op == NULL)
		return (-1);
	op->u.atom.view = view;
	op->u.atom.key_mask = key;
	record_access(planner, op, relation);

	for (i = 0; i < relation->arity; i++) {
		term = &op->u.atom.terms[i];
		if ((read & (UINT32_C(1) << i)) != 0)
			read_term(&atom->args[i], term);
		else {
			bzero(term, sizeof(*term));
			term->action = RELIC_PLAN_ANY;
		}
	}

	return (0);
}

/*
 * Place a negated literal as an ANTI_MEMBER of the whole relation, reading
 * the columns that are not `_`, which the caller places only where every
 * one of them is bound.
 */
static int
place_negated(relic_planner_t *planner, const relic_atom_t *atom)
{
	relic_relation_t *relation;

	relation = relic_relation_read(planner->ctx, atom->pred);
	if (relation == NULL)
		return (-1);

	return (place_probe(planner, RELIC_OP_ANTI_MEMBER, atom, relation,
	    RELIC_VIEW_ALL, named_columns(atom, relation->arity)));
}

/*
 * The kind of slot `slot` is at this point of the plan.
 */
static enum relic_slot_kind
slot_kind(const relic_planner_t *planner, uint16_t slot)
{
	if ((planner->numbers & slot_bit(slot)) != 0)
		return (RELIC_SLOT_NUMBER);
	if ((planner->texts & slot_bit(slot)) != 0)
		return (RELIC_SLOT_TEXT);

	return (RELIC_SLOT_DOMAIN);
}

/*
 * The step that reads a slot of `kind`.
 */
static enum relic_step_kind
slot_step(enum relic_slot_kind kind)
{
	switch (kind) {
	case RELIC_SLOT_NUMBER:
		return (RELIC_STEP_NUMBER_SLOT);
	case RELIC_SLOT_TEXT:
		return (RELIC_STEP_TEXT_SLOT);
	case RELIC_SLOT_DOMAIN:
		break;
	}

	return (RELIC_STEP_DOMAIN_SLOT);
}

/*
 * Append an expression's steps to `postfix` in postfix order -- each
 * operand's or argument's steps, then its operator's or function's -- and
 * give the stack cells running them needs, by the recurrence plan.h states
 * over relic_postfix_t: a leaf needs one, and a node the greatest of what
 * each operand or argument needs plus the cells of those before it.  A
 * variable is read as the kind of slot it is at this point of the plan.
 * The recursion is as deep as the tree is tall, which the parser bounds.
 */
static unsigned int
append_steps(const relic_planner_t *planner, const relic_expr_t *expr,
    relic_postfix_t *postfix)
{
	const relic_expr_call_t *call;
	relic_step_t *step;
	unsigned int need;
	unsigned int held;
	uint8_t i;

	need = 1;
	switch (expr->kind) {
	case RELIC_EXPR_CONSTANT:
	case RELIC_EXPR_VARIABLE:
		break;
	case RELIC_EXPR_NEGATE:
		need = append_steps(planner, expr->u.operand, postfix);
		break;
	case RELIC_EXPR_BINARY:
		need = append_steps(planner, expr->u.binary.left, postfix);
		held = append_steps(planner, expr->u.binary.right, postfix) + 1;
		if (held > need)
			need = held;
		break;
	case RELIC_EXPR_CALL:
		call = &expr->u.call;
		for (i = 0; i < call->argc; i++) {
			held = append_steps(planner, call->args[i], postfix) +
			    i;
			if (held > need)
				need = held;
		}
		break;
	}

	step = &postfix->steps[postfix->step_count];
	postfix->step_count++;
	bzero(step, sizeof(*step));

	switch (expr->kind) {
	case RELIC_EXPR_CONSTANT:
		step->kind = RELIC_STEP_CONSTANT;
		step->u.constant = expr->u.constant;
		break;
	case RELIC_EXPR_VARIABLE:
		step->kind = slot_step(slot_kind(planner, expr->u.slot));
		step->u.slot = expr->u.slot;
		break;
	case RELIC_EXPR_NEGATE:
		step->kind = RELIC_STEP_NEGATE;
		break;
	case RELIC_EXPR_BINARY:
		step->kind = RELIC_STEP_BINARY;
		step->u.op = expr->u.binary.op;
		break;
	case RELIC_EXPR_CALL:
		step->kind = RELIC_STEP_CALL;
		step->u.call.fn = expr->u.call.fn;
		step->u.call.pattern = expr->u.call.pattern;
		break;
	}

	return (need);
}

/*
 * Build the postfix program of an expression in the planner's arena: a step
 * per node of the tree, and the stack its run needs.  Returns -1 when the
 * arena refused, which the caller's boundary reports, or with an internal
 * error reported when the tree needs more stack than any tree the parser
 * admits, which the executor's fixed stack could not hold.
 */
static int
compile_postfix(relic_planner_t *planner, const relic_expr_t *expr,
    const relic_location_t *where, relic_postfix_t *postfix)
{
	unsigned int need;

	bzero(postfix, sizeof(*postfix));

	postfix->steps = relic_arena_allocarray(planner->arena, expr->nodes,
	    sizeof(*postfix->steps));
	if (postfix->steps == NULL)
		return (-1);

	need = append_steps(planner, expr, postfix);
	assert(postfix->step_count == expr->nodes);
	if (need > RELIC_POSTFIX_DEPTH_MAX) {
		relic_diag_internal(planner->ctx, where,
		    "an expression needs more stack than any the parser "
		    "admits");
		return (-1);
	}
	postfix->depth = (uint8_t)need;

	return (0);
}

/*
 * The kind of value an expression leaves: an identifier for a constant or a
 * domain slot, a computed integer for an operator, a number slot or a
 * function whose result is an integer, and a computed string for a text
 * slot or a function whose result is a string.
 */
static enum relic_slot_kind
expr_kind(const relic_planner_t *planner, const relic_expr_t *expr)
{
	switch (expr->kind) {
	case RELIC_EXPR_CONSTANT:
		return (RELIC_SLOT_DOMAIN);
	case RELIC_EXPR_VARIABLE:
		return (slot_kind(planner, expr->u.slot));
	case RELIC_EXPR_NEGATE:
	case RELIC_EXPR_BINARY:
		return (RELIC_SLOT_NUMBER);
	case RELIC_EXPR_CALL:
		break;
	}

	if (relic_function_info(expr->u.call.fn)->result == RELIC_KIND_STRING)
		return (RELIC_SLOT_TEXT);

	return (RELIC_SLOT_NUMBER);
}

/*
 * Record that `slot` is bound, as a slot of `kind`.
 */
static void
bind_slot(relic_planner_t *planner, uint16_t slot, enum relic_slot_kind kind)
{
	planner->bound |= slot_bit(slot);

	switch (kind) {
	case RELIC_SLOT_NUMBER:
		planner->numbers |= slot_bit(slot);
		break;
	case RELIC_SLOT_TEXT:
		planner->texts |= slot_bit(slot);
		break;
	case RELIC_SLOT_DOMAIN:
		break;
	}
}

/*
 * Place an equality that assigns as an ASSIGN binding `slot` to `value`,
 * the equality's other side, whose slots are bound.  The slot is of the
 * kind of value the expression leaves.  An ASSIGN that binds a text slot
 * keeps its string on the text stack, so the texts of every operation
 * after it begin above it.
 */
static int
place_assign(relic_planner_t *planner, const relic_literal_t *literal,
    uint16_t slot, const relic_expr_t *value)
{
	relic_op_assign_t *assign;

	assign = &append_op(planner, RELIC_OP_ASSIGN)->u.assign;
	assign->expr = value;
	assign->where = literal->where;
	assign->slot = slot;
	assign->kind = expr_kind(planner, value);
	assign->text_base = planner->text_base;
	if (compile_postfix(planner, value, &literal->where,
	    &assign->value) != 0)
		return (-1);

	bind_slot(planner, slot, assign->kind);
	if (assign->kind == RELIC_SLOT_TEXT)
		planner->text_base = planner->op_count;

	return (0);
}

/*
 * Place a comparison: as an ASSIGN when it assigns here, and otherwise as a
 * FILTER running the postfix programs of its two sides, which the caller
 * places only where every slot they read is bound.
 */
static int
place_comparison(relic_planner_t *planner, const relic_literal_t *literal)
{
	const relic_comparison_t *comparison;
	const relic_expr_t *value;
	relic_op_filter_t *filter;
	uint16_t slot;

	comparison = &literal->u.comparison;
	if (relic_comparison_assigns(comparison, planner->bound, &slot, &value))
		return (place_assign(planner, literal, slot, value));

	filter = &append_op(planner, RELIC_OP_FILTER)->u.filter;
	filter->comparison = comparison;
	filter->compare = comparison->op;
	filter->where = literal->where;
	filter->text_base = planner->text_base;
	if (compile_postfix(planner, comparison->left, &literal->where,
	    &filter->left) != 0)
		return (-1);

	return (compile_postfix(planner, comparison->right, &literal->where,
	    &filter->right));
}

/*
 * How an AGGREGATE reaches its group's rows, from which of the relation's
 * columns are key columns: its tuple count for an unkeyed count, all of
 * its rows for other folds with no key, the one tuple the
 * key names when every column is one, and otherwise those an index on the
 * key columns holds, or, under RELIC_F_NO_INDEX, those a walk of the whole
 * relation matches.
 */
static enum relic_fold_access
fold_access(const relic_planner_t *planner, enum relic_aggregate_fn fn,
    uint32_t key_mask, uint16_t arity)
{
	if (key_mask == 0 && fn == RELIC_AGG_COUNT)
		return (RELIC_FOLD_CARDINALITY);
	if (key_mask == 0)
		return (RELIC_FOLD_SCAN);

	if (key_mask == relic_plan_all_columns(arity))
		return (RELIC_FOLD_PROBE);

	if ((planner->ctx->config.flags & RELIC_F_NO_INDEX) != 0)
		return (RELIC_FOLD_SCAN);

	return (RELIC_FOLD_LOOKUP);
}

/*
 * Place an aggregate literal, whose shared slots are bound, as an AGGREGATE
 * over the relation it folds.  It binds its result slot when nothing has, a
 * number slot for a count or a sum and a domain slot for a min or a max, and
 * otherwise compares with it, reading it as the kind of slot it is.  A
 * lookup reads the index keyed on the literal's key columns.
 */
static int
place_aggregate(relic_planner_t *planner, const relic_literal_t *literal)
{
	const relic_aggregate_t *aggregate;
	relic_op_aggregate_t *fold;
	relic_relation_t *relation;
	relic_op_t *op;

	aggregate = &literal->u.aggregate;
	relation = relic_relation_read(planner->ctx, aggregate->source);
	if (relation == NULL)
		return (-1);

	op = append_op(planner, RELIC_OP_AGGREGATE);
	fold = &op->u.aggregate;
	fold->relation = relation;
	fold->aggregate = aggregate;
	fold->key_slots = aggregate->shared;
	fold->where = aggregate->where;
	fold->fn = aggregate->fn;
	fold->arity = relation->arity;
	fold->key_count = aggregate->shared_count;
	fold->column = aggregate->column;
	fold->slot = aggregate->result;
	fold->key_mask = aggregate->key_mask;
	fold->access = fold_access(planner, fold->fn, fold->key_mask,
	    fold->arity);
	record_access(planner, op, relation);

	fold->binds = (planner->bound & slot_bit(fold->slot)) == 0;
	if (!fold->binds) {
		fold->kind = slot_kind(planner, fold->slot);
		return (0);
	}

	fold->kind = fold->fn == RELIC_AGG_COUNT || fold->fn == RELIC_AGG_SUM ?
	    RELIC_SLOT_NUMBER : RELIC_SLOT_DOMAIN;
	bind_slot(planner, fold->slot, fold->kind);

	return (0);
}

/*
 * The slots a literal binds when placed where `bound` is bound, as
 * internal.h describes.
 */
uint64_t
relic_plan_literal_binds(const relic_t *ctx, const relic_literal_t *literal,
    uint64_t bound)
{
	const relic_expr_t *value;
	const relic_pred_t *pred;
	uint16_t slot;

	switch (literal->kind) {
	case RELIC_LITERAL_POSITIVE:
		pred = relic_pred_of(ctx, literal->u.atom.pred);
		if (pred == NULL)
			return (0);
		return (arg_slots(literal->u.atom.args, pred->arity));
	case RELIC_LITERAL_COMPARISON:
		if (!relic_comparison_assigns(&literal->u.comparison, bound,
		    &slot, &value))
			return (0);
		return (slot_bit(slot));
	case RELIC_LITERAL_AGGREGATE:
		if (!relic_aggregate_binds(&literal->u.aggregate, bound))
			return (0);
		return (slot_bit(literal->u.aggregate.result));
	case RELIC_LITERAL_NEGATED:
		break;
	}

	return (0);
}

/*
 * Whether a literal is a test -- a comparison, a negated literal or an
 * aggregate -- that can be placed now: an assignment, whose value reads
 * only bound slots, a test every slot of which is bound, or an aggregate
 * whose key is bound.  This is the one place a literal is classified as a
 * test; a positive literal is never one here.  One that binds nothing the
 * rule reads is planned as an EXISTS where it is placed, but it is ranked
 * and placed as every positive literal is, so the order does not depend on
 * which operation it becomes.
 *
 * A negated literal is ready once every column it reads, every one that is
 * not `_`, is a constant or a bound slot; `_` is a column its probe leaves
 * out, so nothing need bind it.
 */
static bool
test_is_placeable(relic_t *ctx, const relic_literal_t *literal,
    uint64_t bound)
{
	const relic_pred_t *pred;

	switch (literal->kind) {
	case RELIC_LITERAL_COMPARISON:
		return (relic_plan_literal_binds(ctx, literal, bound) != 0 ||
		    (relic_comparison_slots(&literal->u.comparison) &
		    ~bound) == 0);
	case RELIC_LITERAL_NEGATED:
		pred = relic_pred_of(ctx, literal->u.atom.pred);
		return (pred != NULL &&
		    probe_is_bound(&literal->u.atom, pred->arity, bound));
	case RELIC_LITERAL_AGGREGATE:
		return ((relic_aggregate_shared_slots(&literal->u.aggregate) &
		    ~bound) == 0);
	case RELIC_LITERAL_POSITIVE:
		break;
	}

	return (false);
}

/*
 * Place a literal test_is_placeable() accepted as the operation that tests
 * or assigns by it.
 * A positive literal cannot arrive here, and is reported as the internal
 * error it would be rather than dropped from the plan.
 */
static int
place_test(relic_planner_t *planner, const relic_literal_t *literal)
{
	switch (literal->kind) {
	case RELIC_LITERAL_COMPARISON:
		return (place_comparison(planner, literal));
	case RELIC_LITERAL_NEGATED:
		return (place_negated(planner, &literal->u.atom));
	case RELIC_LITERAL_AGGREGATE:
		return (place_aggregate(planner, literal));
	case RELIC_LITERAL_POSITIVE:
		break;
	}

	relic_diag_internal(planner->ctx, &literal->where,
	    "a positive literal was placed as a test");

	return (-1);
}

/*
 * Place every test and assignment not yet placed that can be placed now, in
 * the order they are written, and again for as long as an assignment placed
 * binds a slot that may make another placeable.
 *
 * Each pass places what the slots bound when it began make placeable, so a
 * literal an assignment of the pass makes placeable waits for the next pass
 * rather than overtaking one written after it that was placeable already:
 * every literal goes at the earliest point it can, and in written order
 * among those placeable at the same point.  What a literal binds is asked
 * of the slots bound as it is placed, so that of two assignments of one
 * variable placeable at one point, the second is a test of what the first
 * bound.
 *
 * Called before the first literal and after every positive literal that
 * binds a slot, which with the assignments placed here are the only points
 * at which a test or an assignment can become ready, so each is placed at
 * the earliest point it can be.  Each repetition places an assignment, which
 * binds a slot nothing bound before, so there are no more repetitions than
 * slots.
 */
static void
order_ready_tests(relic_planner_t *planner)
{
	const relic_literal_t *literal;
	const relic_rule_t *rule;
	uint64_t pass_bound;
	uint64_t binds;
	bool assigned;
	size_t i;

	rule = planner->rule;

	do {
		pass_bound = planner->bound;
		assigned = false;

		for (i = 0; i < rule->body_length; i++) {
			literal = &rule->body[i];
			if (planner->placed[i] ||
			    literal->kind == RELIC_LITERAL_POSITIVE ||
			    !test_is_placeable(planner->ctx, literal,
			    pass_bound))
				continue;

			binds = relic_plan_literal_binds(planner->ctx, literal,
			    planner->bound);

			planner->order[planner->order_count] = i;
			planner->order_count++;
			planner->placed[i] = true;
			if (binds != 0) {
				planner->bound |= binds;
				assigned = true;
			}
		}
	} while (assigned);
}

/*
 * Place the positive literal at body index `index`, which binds every
 * variable it mentions that is not dead, then, when that bound a slot not
 * bound before, every test it made placeable.
 */
static void
order_positive(relic_planner_t *planner, size_t index)
{
	uint64_t bound_before;

	bound_before = planner->bound;

	planner->order[planner->order_count] = index;
	planner->order_count++;
	planner->placed[index] = true;
	planner->bound |= relic_plan_literal_binds(planner->ctx,
	    &planner->rule->body[index], planner->bound) & ~planner->dead;

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
 * The estimate for a relation still being derived, read through `view`: the
 * assumed size, larger for a member read beside its own delta, narrowed by
 * the assumed selectivity of each bound column.
 */
static size_t
estimate_unknown(uint16_t arity, uint32_t bound, enum relic_view view)
{
	size_t estimate;
	uint16_t column;

	estimate = RELIC_PLAN_UNKNOWN_TUPLES;
	if (view == RELIC_VIEW_OLD || view == RELIC_VIEW_CURRENT)
		estimate *= RELIC_PLAN_DELTAS_HELD;
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
 * Score a positive literal, read through `view`, against the slots in
 * `bound`.  Returns 0, or -1 with a diagnostic reported.  `incomplete` is the
 * planner's.  A `deferred` literal, an input-first order's delta literal,
 * ranks below every other binding as many arguments, so its estimate would
 * decide nothing and is not made.
 *
 * A relation still being derived is not even looked up: its estimate is an
 * assumption, and looking it up would create a relation that the Magic
 * rewrite, which orders rules over predicates it never evaluates, would leave
 * lying empty in the context.
 */
static int
score_positive(relic_t *ctx, const bool *incomplete, const relic_atom_t *atom,
    enum relic_view view, uint64_t bound, bool deferred,
    relic_plan_score_t *scorep)
{
	const relic_pred_t *pred;
	relic_relation_t *relation;
	uint32_t columns;

	pred = relic_pred_of(ctx, atom->pred);
	if (pred == NULL) {
		relic_diag_internal(ctx, &atom->where,
		    "a literal names no predicate");
		return (-1);
	}

	columns = relic_plan_bound_columns(atom, pred->arity, bound);
	scorep->bound_args = column_count(columns);
	scorep->deferred = deferred;
	if (deferred)
		return (0);

	if (is_incomplete(incomplete, atom->pred)) {
		scorep->estimate = estimate_unknown(pred->arity, columns, view);
		return (0);
	}

	relation = relic_relation_read(ctx, atom->pred);
	if (relation == NULL)
		return (-1);

	return (estimate_measured(ctx, relation, columns, &scorep->estimate));
}

/*
 * The order of two literals in a ranking, best first: more bound arguments,
 * then one that is not deferred, then the smaller estimate, then the one
 * written first.  At most one literal is deferred, so two compared by their
 * estimates were both scored.  Body indexes are distinct, so no two
 * different literals compare equal and the sort has one result.
 */
static int
compare_ranked(const void *a, const void *b)
{
	const relic_ranked_literal_t *left;
	const relic_ranked_literal_t *right;

	left = a;
	right = b;

	if (left->score.bound_args != right->score.bound_args)
		return (left->score.bound_args > right->score.bound_args ?
		    -1 : 1);

	if (left->score.deferred != right->score.deferred)
		return (left->score.deferred ? 1 : -1);

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
	relic_ranked_literal_t *ranked;
	bool reorder;
	bool deferred;
	size_t i;

	rule = planner->rule;
	reorder = (planner->ctx->config.flags & RELIC_F_NO_REORDER) == 0;
	planner->ranking_count = 0;

	for (i = 0; i < rule->body_length; i++) {
		if (rule->body[i].kind != RELIC_LITERAL_POSITIVE ||
		    planner->placed[i])
			continue;

		ranked = &planner->ranking[planner->ranking_count];
		planner->ranking_count++;
		bzero(ranked, sizeof(*ranked));
		ranked->index = i;
		if (!reorder)
			continue;

		deferred = planner->policy == RELIC_POLICY_INPUT_FIRST &&
		    i == planner->delta_literal;
		if (score_positive(planner->ctx, planner->incomplete,
		    &rule->body[i].u.atom, positive_view(planner, i),
		    planner->bound, deferred, &ranked->score) != 0)
			return (-1);
	}

	if (planner->ranking_count > 1)
		qsort(planner->ranking, planner->ranking_count,
		    sizeof(*planner->ranking), compare_ranked);

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
 * with, the delta literal when it is pinned first, then the positive
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

	if (planner->policy == RELIC_POLICY_DELTA_FIRST)
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
	planner->numbers = 0;
	planner->texts = 0;
	planner->text_base = 0;

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
 * or the head reads is bound by some positive literal or an assignment the
 * safety closure reaches, so once every positive literal is placed, every
 * such assignment and every test has been too.  The
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

	relic_diag_internal(planner->ctx, &rule->where,
	    "the plan leaves a literal out or reads a variable nothing binds");

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

	relic_diag_internal(planner->ctx, &planner->rule->where,
	    "the planner could not place every literal");

	return (-1);
}

/*
 * Confirm that a variant's delta literal is a positive literal of the rule
 * over a relation `incomplete` marks as still being derived.  Anything else
 * is a caller's mistake that would not fail on its own: an index past the
 * body would be read, a comparison or a negated literal taken for an atom,
 * and without the marks every other member literal would read the whole
 * relation, deriving what other variants derive too.  It is confirmed once
 * for a family, before either of its orders is decided.
 */
static int
confirm_delta_literal(relic_t *ctx, const relic_rule_t *rule,
    size_t delta_literal, const bool *incomplete)
{
	if (delta_literal >= rule->body_length ||
	    rule->body[delta_literal].kind != RELIC_LITERAL_POSITIVE) {
		relic_diag_internal(ctx, &rule->where,
		    "a delta variant was asked for a literal that is not a "
		    "positive literal of the rule");
		return (-1);
	}

	if (!is_incomplete(incomplete, rule->body[delta_literal].u.atom.pred)) {
		relic_diag_internal(ctx, &rule->where,
		    "a delta variant was asked for a literal over a relation "
		    "not being derived");
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

	relic_diag_internal(planner->ctx, &planner->rule->where,
	    "a rule of %u head arguments was planned into relation %s",
	    (unsigned int)planner->rule->head_arity,
	    relic_relation_identify(planner->ctx, planner->target, described,
	    sizeof(described)));

	return (-1);
}

/*
 * Place the EMIT that ends every plan.  It carries the head's terms and
 * arity, and the predicate the head names, or none for a query.  The
 * relation the tuples are written into belongs to the caller:
 * confirm_target_fits() has required its arity to be the head's, and the
 * operation does not hold the relation.
 */
static int
place_emit(relic_planner_t *planner)
{
	const relic_rule_t *rule;
	relic_plan_term_t *term;
	relic_op_t *op;
	uint16_t i;

	rule = planner->rule;

	if (confirm_target_fits(planner) != 0)
		return (-1);

	if (confirm_plan_complete(planner) != 0)
		return (-1);

	op = append_op(planner, RELIC_OP_EMIT);
	op->u.emit.pred = rule->head;
	op->u.emit.arity = rule->head_arity;
	op->u.emit.terms = terms_alloc(planner, rule->head_arity);
	if (rule->head_arity > 0 && op->u.emit.terms == NULL)
		return (-1);

	for (i = 0; i < rule->head_arity; i++) {
		term = &op->u.emit.terms[i];
		read_term(&rule->head_args[i], term);
		if (term->action != RELIC_PLAN_BOUND)
			continue;

		switch (slot_kind(planner, term->u.slot)) {
		case RELIC_SLOT_NUMBER:
			term->action = RELIC_PLAN_NUMBER;
			break;
		case RELIC_SLOT_TEXT:
			term->action = RELIC_PLAN_TEXT;
			break;
		case RELIC_SLOT_DOMAIN:
			break;
		}
	}

	return (0);
}

/*
 * The rule's dead variables: the slots it mentions exactly once, head
 * included, where that mention is in a positive literal.  A positive literal
 * mentions the variable in each of its columns, one in two columns twice; a
 * negated literal its named arguments; a comparison the slots of its two
 * sides; an aggregate its result and the slots its key shares with the
 * rule, its body's own variables belonging to the rule compiled for it; and
 * the head the variable in each of its columns.
 *
 * The positive literals' mentions are counted, up to two, and every other
 * mention only noted, since one is enough to keep a variable alive: a slot
 * mentioned outside the positive literals is either mentioned there too,
 * and so twice, or not in a positive literal at all.  A query's head holds
 * its every named variable and a group rule's every variable of its body,
 * so neither has a dead variable; a `_` takes no slot and is not counted.
 * The count is read from the rule's literals, which are what the plan
 * reads, rather than from the record of occurrences the singleton warning
 * reads, so that it holds for every rule planned, those the Magic rewrite
 * generates included, whatever that record says.
 *
 * A literal naming no predicate is an internal error that the walk will
 * report, and it leaves the rule with no dead variable rather than mistake a
 * slot it holds for one mentioned once.
 */
static uint64_t
dead_slots(const relic_t *ctx, const relic_rule_t *rule)
{
	const relic_literal_t *literal;
	const relic_pred_t *pred;
	uint64_t once;
	uint64_t twice;
	uint64_t elsewhere;
	uint64_t slot;
	size_t i;
	uint16_t column;

	once = 0;
	twice = 0;
	elsewhere = arg_slots(rule->head_args, rule->head_arity);

	for (i = 0; i < rule->body_length; i++) {
		literal = &rule->body[i];

		switch (literal->kind) {
		case RELIC_LITERAL_POSITIVE:
			pred = relic_pred_of(ctx, literal->u.atom.pred);
			if (pred == NULL)
				return (0);
			for (column = 0; column < pred->arity; column++) {
				slot = arg_slots(&literal->u.atom.args[column],
				    1);
				twice |= once & slot;
				once |= slot;
			}
			break;
		case RELIC_LITERAL_NEGATED:
			pred = relic_pred_of(ctx, literal->u.atom.pred);
			if (pred == NULL)
				return (0);
			elsewhere |= arg_slots(literal->u.atom.args,
			    pred->arity);
			break;
		case RELIC_LITERAL_COMPARISON:
			elsewhere |= relic_comparison_slots(
			    &literal->u.comparison);
			break;
		case RELIC_LITERAL_AGGREGATE:
			elsewhere |= relic_aggregate_shared_slots(
			    &literal->u.aggregate) |
			    slot_bit(literal->u.aggregate.result);
			break;
		}
	}

	return (once & ~twice & ~elsewhere);
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
    enum relic_order_policy policy, const bool *incomplete)
{
	size_t count;

	bzero(planner, sizeof(*planner));
	planner->ctx = ctx;
	planner->arena = arena;
	planner->rule = rule;
	planner->target = target;
	planner->incomplete = incomplete;
	planner->delta_literal = delta_literal;
	planner->policy = policy;
	if ((ctx->config.flags & RELIC_F_NO_EXISTS) == 0)
		planner->dead = dead_slots(ctx, rule);

	planner->placed = relic_arena_allocarray(arena, rule->body_length,
	    sizeof(*planner->placed));
	if (planner->placed == NULL)
		return (-1);
	bzero(planner->placed, rule->body_length * sizeof(*planner->placed));

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
	if (order_body(planner) != 0)
		return (-1);

	return (confirm_order_complete(planner));
}

/*
 * Give a planner room in its arena for the order it will decide.
 */
static int
planner_prepare_order(relic_planner_t *planner)
{
	planner->order = relic_arena_allocarray(planner->arena,
	    planner->rule->body_length, sizeof(*planner->order));

	return (planner->order == NULL ? -1 : 0);
}

/*
 * Emit the draft of a decided order into *draft: an operation per literal
 * and the EMIT, and the indexes they read.  Emitting creates the relations
 * the plan reads, but builds none of the indexes.  The body length comes
 * from a compiled rule held in memory, so one more than it cannot wrap.
 */
static int
emit_draft(relic_planner_t *planner, relic_plan_draft_t *draft)
{
	size_t room;

	room = planner->rule->body_length + 1;
	planner->ops = relic_arena_allocarray(planner->arena, room,
	    sizeof(*planner->ops));
	planner->accesses = relic_arena_allocarray(planner->arena, room,
	    sizeof(*planner->accesses));
	if (planner->ops == NULL || planner->accesses == NULL)
		return (-1);

	if (emit_body(planner) != 0)
		return (-1);
	if (place_emit(planner) != 0)
		return (-1);
	if (coalesce_accesses(planner) != 0)
		return (-1);

	draft->ops = planner->ops;
	draft->op_count = planner->op_count;
	draft->accesses = planner->accesses;
	draft->access_count = planner->access_count;

	return (0);
}

/*
 * The ordering policies of a delta variant's candidate plans, into
 * `policies`, in the order its family keeps them, and how many there are.
 *
 * The written order of RELIC_F_NO_REORDER is the textbook scheme and is not
 * second-guessed: it is the only plan.  Without indexes a delta placed
 * after other literals would be scanned whole once for each binding that
 * reaches it, while the delta-first plan scans it once, so under
 * RELIC_F_NO_INDEX alone the delta-first order is the only plan.  Otherwise
 * both orders are candidates.
 */
static size_t
family_policies(const relic_t *ctx, enum relic_order_policy *policies)
{
	unsigned int flags;

	flags = ctx->config.flags;

	if ((flags & RELIC_F_NO_REORDER) != 0) {
		policies[0] = RELIC_POLICY_GREEDY;
		return (1);
	}

	policies[RELIC_FAMILY_DELTA_FIRST] = RELIC_POLICY_DELTA_FIRST;
	if ((flags & RELIC_F_NO_INDEX) != 0)
		return (1);

	policies[RELIC_FAMILY_INPUT_FIRST] = RELIC_POLICY_INPUT_FIRST;

	return (2);
}

/*
 * Whether two decided orders of one rule place every literal, tests and
 * assignments included, at the same position.  Two such orders read the
 * same views, so they would emit the same operations: one plan twice.
 */
static bool
orders_equal(const relic_planner_t *left, const relic_planner_t *right)
{
	size_t i;

	if (left->order_count != right->order_count)
		return (false);

	for (i = 0; i < left->order_count; i++) {
		if (left->order[i] != right->order[i])
			return (false);
	}

	return (true);
}

/*
 * Compile the work recipe of an emitted draft into *recipe: a step per
 * operation, in plan order.  Returns -1 when the arena refused, which the
 * caller's boundary reports.
 */
static int
compile_recipe(relic_arena_t *arena, const relic_plan_draft_t *draft,
    relic_cost_recipe_t *recipe)
{
	relic_cost_step_t *steps;
	size_t i;

	steps = relic_arena_allocarray(arena, draft->op_count, sizeof(*steps));
	if (steps == NULL)
		return (-1);

	for (i = 0; i < draft->op_count; i++) {
		bzero(&steps[i], sizeof(steps[i]));
		steps[i].op = &draft->ops[i];
		steps[i].kind = relic_cost_kind_of(&draft->ops[i]);
	}

	recipe->steps = steps;
	recipe->step_count = draft->op_count;

	return (0);
}

/*
 * Emit the drafts of the first `count` decided orders into a family, in
 * family order, each with its recipe when there are two to choose between.
 * The count is set last, so that a family whose emission failed part way
 * holds no candidate.
 */
static int
emit_family(relic_planner_t *planners, size_t count,
    relic_plan_family_t *family)
{
	relic_plan_candidate_t *candidate;
	size_t i;

	for (i = 0; i < count; i++) {
		candidate = &family->candidates[i];
		if (emit_draft(&planners[i], &candidate->draft) != 0)
			return (-1);
		if (count == 1)
			continue;
		if (compile_recipe(planners[i].arena, &candidate->draft,
		    &candidate->recipe) != 0)
			return (-1);
	}

	family->count = count;

	return (0);
}

/*
 * Decide a delta variant's orders, one per policy family_policies() gives,
 * with the planners in `planners`, which have room for RELIC_FAMILY_MAX and
 * are zeroed, and emit the distinct ones into *family.  Both orders are
 * decided before either is emitted, so that an order both policies reach
 * is emitted once.
 */
static int
plan_family(relic_planner_t *planners, relic_t *ctx, relic_arena_t *arena,
    const relic_rule_t *rule, relic_relation_t *target, size_t delta_literal,
    const bool *incomplete, relic_plan_family_t *family)
{
	enum relic_order_policy policies[RELIC_FAMILY_MAX];
	size_t count;
	size_t i;

	if (confirm_delta_literal(ctx, rule, delta_literal, incomplete) != 0)
		return (-1);

	count = family_policies(ctx, policies);
	for (i = 0; i < count; i++) {
		if (planner_init(&planners[i], ctx, arena, rule, target,
		    delta_literal, policies[i], incomplete) != 0)
			return (-1);
		if (planner_prepare_order(&planners[i]) != 0)
			return (-1);
		if (decide_order(&planners[i]) != 0)
			return (-1);
	}

	if (count == RELIC_FAMILY_MAX &&
	    orders_equal(&planners[RELIC_FAMILY_DELTA_FIRST],
	    &planners[RELIC_FAMILY_INPUT_FIRST]))
		count = 1;

	return (emit_family(planners, count, family));
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
 * Plan a rule's ordinary plan, in which every literal reads its whole
 * relation.  Returns 0 with the plan in *plan, or -1 with a diagnostic
 * reported: memory or the memory limit refused an index or a statistic, or
 * the planner broke one of a plan's invariants, which is an internal error.
 *
 * The caller passes `target` so the head arity can be checked against it.
 * The plan does not retain `target`.  `incomplete`, when not NULL, is
 * indexed by predicate identifier minus one and marks the relations still
 * being derived; every other relation is measured as it stands.
 *
 * The plan and everything it points at except its relations are allocated
 * from `arena`, and live as long as the caller keeps it.  The relations the
 * body reads, and the indexes its lookups read, are created here if they do
 * not yet exist; they belong to the context, which is what lets a plan hold
 * them.  The caller keeps `target` and pairs it with the plan when the plan
 * runs.
 */
int
relic_plan_rule(relic_t *ctx, relic_arena_t *arena, const relic_rule_t *rule,
    relic_relation_t *target, const bool *incomplete, relic_plan_t *plan)
{
	relic_planner_t planner;
	relic_plan_draft_t draft;
	int status;

	bzero(plan, sizeof(*plan));

	status = planner_init(&planner, ctx, arena, rule, target,
	    RELIC_PLAN_NO_DELTA, RELIC_POLICY_GREEDY, incomplete);
	if (status == 0)
		status = planner_prepare_order(&planner);
	if (status == 0)
		status = decide_order(&planner);
	if (status == 0)
		status = emit_draft(&planner, &draft);
	planner_release(&planner);

	status = planning_status(ctx, arena, status);
	if (status != 0)
		return (-1);

	return (relic_plan_prepare(ctx, arena, &draft, plan));
}

/*
 * Give an operation copied from a draft the position of the index it reads,
 * if it reads one.  relic_plan_prepare() has had the relation build every
 * index the draft names, and the draft names every index an operation
 * reads, so a miss is an internal error rather than an index to build.
 */
static int
bind_index(relic_t *ctx, relic_op_t *op)
{
	const relic_relation_t *relation;
	uint32_t key_mask;
	uint32_t *positionp;

	positionp = op_index_read(op, &relation, &key_mask);
	if (positionp == NULL)
		return (0);

	if (relic_relation_index_find(relation, key_mask, positionp))
		return (0);

	relic_diag_internal(ctx, NULL,
	    "a plan reads an index its draft does not name");

	return (-1);
}

/*
 * Make the plan the executor runs of a draft, into *plan: copy the
 * operations into `arena`, have each relation build the indexes the draft
 * names that it lacks, then give each operation the position of the index
 * it reads.  Returns 0, or -1 with a diagnostic reported and *plan empty:
 * memory refused the copy, memory or the memory limit refused an index, or
 * an operation reads an index the draft does not name, which is an
 * internal error.  The copy is made first, so a refused copy builds
 * nothing; after either later failure the indexes built so far stay built,
 * and are maintained like any other, and nothing reads a plan that was not
 * made.
 */
int
relic_plan_prepare(relic_t *ctx, relic_arena_t *arena,
    const relic_plan_draft_t *draft, relic_plan_t *plan)
{
	const relic_plan_access_t *access;
	relic_op_t *ops;
	uint32_t position;
	size_t i;

	bzero(plan, sizeof(*plan));

	ops = relic_arena_allocarray(arena, draft->op_count, sizeof(*ops));
	if (ops == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	bcopy(draft->ops, ops, draft->op_count * sizeof(*ops));

	for (i = 0; i < draft->access_count; i++) {
		access = &draft->accesses[i];
		if (relic_relation_index(ctx, access->relation,
		    access->key_mask, &position) != 0)
			return (-1);
	}

	for (i = 0; i < draft->op_count; i++) {
		if (bind_index(ctx, &ops[i]) != 0)
			return (-1);
	}

	plan->ops = ops;
	plan->op_count = draft->op_count;

	return (0);
}

/*
 * Plan a semi-naive delta variant of a rule into *family: one or two
 * candidate plans, each with its recipe when there are two (plan.h).
 * Arguments and failures are as for relic_plan_rule(), except that no
 * index is built, so none is refused, and a request without a delta
 * literal is refused as an internal error.  A failure leaves the family
 * with no candidate.
 */
int
relic_plan_variant(relic_t *ctx, relic_arena_t *arena,
    const relic_rule_t *rule, relic_relation_t *target, size_t delta_literal,
    const bool *incomplete, relic_plan_family_t *family)
{
	relic_planner_t planners[RELIC_FAMILY_MAX];
	int status;
	size_t i;

	bzero(family, sizeof(*family));
	bzero(planners, sizeof(planners));
	if (delta_literal == RELIC_PLAN_NO_DELTA) {
		relic_diag_internal(ctx, &rule->where,
		    "a delta variant was asked for with no delta literal");
		return (-1);
	}

	status = plan_family(planners, ctx, arena, rule, target,
	    delta_literal, incomplete, family);

	for (i = 0; i < RELIC_FAMILY_MAX; i++)
		planner_release(&planners[i]);

	return (planning_status(ctx, arena, status));
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
	    RELIC_PLAN_NO_DELTA, RELIC_POLICY_GREEDY, incomplete);
	if (status == 0) {
		planner.order = order;
		planner.bound = bound;
		status = decide_order(&planner);
	}
	planner_release(&planner);

	return (planning_status(ctx, arena, status));
}

/*
 * The slots `arity` terms bind, and the ones they read bound, into
 * *bindsp and *readsp.  Returns false when a column matches with `_`,
 * which binds nothing, so that two tuples differing only there both
 * pass.  The switch names every action, so that one added later must be
 * classified here rather than be ignored.
 */
static bool
term_slots(const relic_plan_term_t *terms, uint16_t arity, uint64_t *bindsp,
    uint64_t *readsp)
{
	const relic_plan_term_t *term;
	uint16_t column;
	bool matches_all;

	*bindsp = 0;
	*readsp = 0;
	matches_all = true;

	for (column = 0; column < arity; column++) {
		term = &terms[column];

		switch (term->action) {
		case RELIC_PLAN_CONSTANT:
			break;
		case RELIC_PLAN_BOUND:
		case RELIC_PLAN_NUMBER:
		case RELIC_PLAN_TEXT:
			*readsp |= slot_bit(term->u.slot);
			break;
		case RELIC_PLAN_BIND:
			*bindsp |= slot_bit(term->u.slot);
			break;
		case RELIC_PLAN_ANY:
			matches_all = false;
			break;
		}
	}

	return (matches_all);
}

/*
 * The slots the columns of an operation over a relation bind, and the ones
 * they read bound.  The arity is the operation's, copied from the relation
 * when it was planned.
 */
static bool
atom_slots(const relic_op_atom_t *atom, uint64_t *bindsp, uint64_t *readsp)
{
	return (term_slots(atom->terms, atom->arity, bindsp, readsp));
}

/*
 * A plan derives each head tuple once when no generator has a wildcard and
 * its EMIT reads every slot a generator binds.  An ASSIGN and an AGGREGATE
 * bind a slot too, but to a function of the slots bound before it -- a fold
 * of a relation that does not change while the plan runs -- so two bindings
 * that agree on every slot a generator binds agree on it as well, and the
 * EMIT need not read it.
 *
 * Three facts about what a plan reads make that so, over relations that do
 * not change while it runs: a relation holds no tuple twice, a view lists
 * each tuple in its range once, and a test lets a binding through at most
 * once.  Two tuples a generator reads under one binding of the slots before
 * it both match its constants and its bound slots, so, holding no wildcard,
 * they differ in a column it binds and bind different values there.  That
 * holds when a slot repeats in the atom, as X does in e(X, X): the first
 * occurrence binds X and the second is a bound slot matched against it, so
 * two tuples differing at the second differ at the first as well.  Each
 * path through the generators therefore reaches the EMIT with a binding no
 * other path has, and an EMIT that reads every bound slot derives a tuple
 * no other path derives.  A wildcard, or a slot the EMIT does not read,
 * would let two bindings that differ only there derive one tuple twice.
 *
 * The switch names every kind of operation, and term_slots() every action a
 * column may have, so that a kind or an action added later must be decided
 * rather than be assumed to keep the property.
 */
bool
relic_plan_derives_distinct(const relic_plan_t *plan)
{
	const relic_op_t *op;
	uint64_t emitted;
	uint64_t generated;
	uint64_t binds;
	uint64_t reads;
	size_t i;

	generated = 0;
	emitted = 0;
	for (i = 0; i < plan->op_count; i++) {
		op = &plan->ops[i];

		switch (op->kind) {
		case RELIC_OP_SCAN:
		case RELIC_OP_LOOKUP:
			if (!atom_slots(&op->u.atom, &binds, &reads))
				return (false);
			generated |= binds;
			break;
		case RELIC_OP_MEMBER:
		case RELIC_OP_EXISTS:
		case RELIC_OP_ANTI_MEMBER:
		case RELIC_OP_FILTER:
		case RELIC_OP_ASSIGN:
		case RELIC_OP_AGGREGATE:
			break;
		case RELIC_OP_EMIT:
			(void)term_slots(op->u.emit.terms, op->u.emit.arity,
			    &binds, &reads);
			emitted |= reads;
			break;
		}
	}

	return ((generated & ~emitted) == 0);
}

/*
 * Whether a postfix program applies an operator or a function, rather than
 * reading one term.
 */
static bool
postfix_computes(const relic_postfix_t *postfix)
{
	size_t i;

	for (i = 0; i < postfix->step_count; i++) {
		switch (postfix->steps[i].kind) {
		case RELIC_STEP_CONSTANT:
		case RELIC_STEP_DOMAIN_SLOT:
		case RELIC_STEP_NUMBER_SLOT:
		case RELIC_STEP_TEXT_SLOT:
			break;
		case RELIC_STEP_NEGATE:
		case RELIC_STEP_BINARY:
		case RELIC_STEP_CALL:
			return (true);
		}
	}

	return (false);
}

/*
 * Whether a postfix program calls a function.
 */
static bool
postfix_calls(const relic_postfix_t *postfix)
{
	size_t i;

	for (i = 0; i < postfix->step_count; i++) {
		if (postfix->steps[i].kind == RELIC_STEP_CALL)
			return (true);
	}

	return (false);
}

bool
relic_plan_calls(const relic_plan_t *plan)
{
	const relic_op_t *op;
	size_t i;

	for (i = 0; i < plan->op_count; i++) {
		op = &plan->ops[i];

		switch (op->kind) {
		case RELIC_OP_FILTER:
			if (postfix_calls(&op->u.filter.left) ||
			    postfix_calls(&op->u.filter.right))
				return (true);
			break;
		case RELIC_OP_ASSIGN:
			if (postfix_calls(&op->u.assign.value))
				return (true);
			break;
		case RELIC_OP_SCAN:
		case RELIC_OP_LOOKUP:
		case RELIC_OP_MEMBER:
		case RELIC_OP_EXISTS:
		case RELIC_OP_ANTI_MEMBER:
		case RELIC_OP_AGGREGATE:
		case RELIC_OP_EMIT:
			break;
		}
	}

	return (false);
}

/*
 * Each kind is named, so a new one does not compile until it is placed in
 * the prefix, as the driver, or as the end of recognition.
 */
size_t
relic_plan_driver(const relic_plan_t *plan)
{
	size_t i;

	for (i = 0; i < plan->op_count; i++) {
		switch (plan->ops[i].kind) {
		case RELIC_OP_SCAN:
		case RELIC_OP_LOOKUP:
			return (i);
		case RELIC_OP_MEMBER:
		case RELIC_OP_EXISTS:
		case RELIC_OP_ANTI_MEMBER:
		case RELIC_OP_FILTER:
			continue;
		case RELIC_OP_ASSIGN:
		case RELIC_OP_AGGREGATE:
		case RELIC_OP_EMIT:
			break;
		}
		break;
	}

	return (RELIC_PLAN_NO_DRIVER);
}

const relic_op_atom_t *
relic_op_atom(const relic_op_t *op)
{
	switch (op->kind) {
	case RELIC_OP_SCAN:
	case RELIC_OP_LOOKUP:
	case RELIC_OP_MEMBER:
	case RELIC_OP_EXISTS:
	case RELIC_OP_ANTI_MEMBER:
		return (&op->u.atom);
	case RELIC_OP_EMIT:
	case RELIC_OP_FILTER:
	case RELIC_OP_ASSIGN:
	case RELIC_OP_AGGREGATE:
		break;
	}

	return (NULL);
}

bool
relic_plan_computes(const relic_plan_t *plan)
{
	const relic_op_t *op;
	size_t i;

	for (i = 0; i < plan->op_count; i++) {
		op = &plan->ops[i];

		switch (op->kind) {
		case RELIC_OP_FILTER:
			if (postfix_computes(&op->u.filter.left) ||
			    postfix_computes(&op->u.filter.right))
				return (true);
			break;
		case RELIC_OP_ASSIGN:
			if (postfix_computes(&op->u.assign.value))
				return (true);
			break;
		case RELIC_OP_AGGREGATE:
			if (op->u.aggregate.fn == RELIC_AGG_COUNT ||
			    op->u.aggregate.fn == RELIC_AGG_SUM)
				return (true);
			break;
		case RELIC_OP_SCAN:
		case RELIC_OP_LOOKUP:
		case RELIC_OP_MEMBER:
		case RELIC_OP_EXISTS:
		case RELIC_OP_ANTI_MEMBER:
		case RELIC_OP_EMIT:
			break;
		}
	}

	return (false);
}

bool
relic_plan_folds(const relic_plan_t *plan)
{
	size_t i;

	for (i = 0; i < plan->op_count; i++) {
		if (plan->ops[i].kind == RELIC_OP_AGGREGATE)
			return (true);
	}

	return (false);
}
