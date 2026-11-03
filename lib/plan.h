/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_PLAN_H
#define RELIC_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "frozen.h"
#include "regex.h"
#include "util.h"

/*
 * A plan: how one rule is executed, as an ordered array of operations.
 *
 * The order of the array is the nesting of the join.  Each operation either
 * produces bindings (a generator), lets a binding through or rejects it (a
 * test), or derives the head tuple, and each states every decision in advance
 * -- which relation it reads, and for every column whether it is matched
 * against a constant, matched against a slot already bound, binds a slot, or
 * is ignored.  The executor therefore makes no decision a plan could have
 * made, and every optimization is a change in what the planner emits rather
 * than in how a plan is run.
 */

typedef struct relic_cost_recipe relic_cost_recipe_t;
typedef struct relic_cost_step relic_cost_step_t;
typedef struct relic_op relic_op_t;
typedef struct relic_op_aggregate relic_op_aggregate_t;
typedef struct relic_op_assign relic_op_assign_t;
typedef struct relic_op_atom relic_op_atom_t;
typedef struct relic_op_emit relic_op_emit_t;
typedef struct relic_op_filter relic_op_filter_t;
typedef struct relic_plan relic_plan_t;
typedef struct relic_plan_access relic_plan_access_t;
typedef struct relic_plan_candidate relic_plan_candidate_t;
typedef struct relic_plan_draft relic_plan_draft_t;
typedef struct relic_plan_family relic_plan_family_t;
typedef struct relic_plan_term relic_plan_term_t;
typedef struct relic_postfix relic_postfix_t;
typedef struct relic_step relic_step_t;

/*
 * The operations.
 *
 * SCAN is a generator: it succeeds once per tuple of its relation that
 * matches, binding the columns it binds.  LOOKUP is the same generator
 * reading only the tuples a secondary index holds under the values of its
 * key columns, every one of which is a constant or a bound slot; it visits
 * exactly the tuples a SCAN with the same terms would match, in the same
 * order, and differs only in what it reads to find them.
 *
 * MEMBER, EXISTS, ANTI_MEMBER and FILTER are tests: they read only constants
 * and bound slots, bind nothing, and succeed at most once.  EXISTS is a
 * positive literal that binds nothing the rule reads: some of its columns
 * are `_` or a variable the rule mentions nowhere else, and every other
 * column is a constant or a bound slot.  It asks whether the relation holds,
 * in its view, a tuple agreeing with it on those other columns, and lets the
 * binding through once when it does, where a generator would continue the
 * join once per tuple it matched with the same frame each time.  MEMBER is
 * the same question when every column is bound.  ANTI_MEMBER asks the same
 * question of a negated literal's columns that are not `_`, and succeeds
 * when the answer is no, which is the whole of negation: the schedule
 * evaluates a negated relation completely before any rule that reads it
 * runs, so its absence is meaningful.  ASSIGN is a test that binds: it
 * computes one value from constants and bound slots and binds one slot to it,
 * succeeding once, or never when the value does not exist because an operand
 * was not an integer.  AGGREGATE is a test that binds too: it folds the rows
 * of a complete group relation under the key its bound slots give and binds
 * its result slot to the value, or, when the slot was bound before, holds
 * exactly when the value equals it; it fails when its function has no value
 * over the group.  EMIT derives the head tuple, which the evaluation inserts,
 * or delivers as a query's row, in batches, and never succeeds, which is what
 * makes it the last operation of every plan: the deepest level of the join
 * always rejects, so the executor always backtracks from it.
 */
enum relic_op_kind {
	RELIC_OP_SCAN,		/* match and bind every tuple of a relation */
	RELIC_OP_LOOKUP,	/* the same, by an index on bound columns */
	RELIC_OP_MEMBER,	/* is this fully bound tuple present? */
	RELIC_OP_EXISTS,	/* does a tuple agree on the bound columns? */
	RELIC_OP_ANTI_MEMBER,	/* does no tuple agree on the named columns? */
	RELIC_OP_FILTER,	/* does a comparison of bound values hold? */
	RELIC_OP_ASSIGN,	/* bind a slot to a value from bound ones */
	RELIC_OP_AGGREGATE,	/* fold a group relation's rows for a key */
	RELIC_OP_EMIT		/* derive the head tuple */
};

/*
 * What an operation does with one column, decided when the plan is built.
 *
 * Whether a slot is bound at a given point in a plan is static: it is bound
 * when an earlier operation, or an earlier column of the same operation,
 * binds it.  The planner therefore settles which of these applies to every
 * column, and the executor carries no bound-mask and never asks.
 *
 * RELIC_PLAN_BIND occurs only in a SCAN or a LOOKUP.  RELIC_PLAN_ANY occurs
 * in those, in an EXISTS and in an ANTI_MEMBER, for a column nothing reads:
 * a `_`, or in a positive literal a variable the rule mentions only there,
 * which is a `_` with a name.  Neither ever occurs in a key column.  Every
 * other test and an EMIT read constants and bound slots and nothing else.
 *
 * A slot is of one of three kinds, fixed by the operation that binds it
 * (enum relic_slot_kind).  A domain slot is bound by a generator, by an
 * ASSIGN of a constant or of another domain slot, or by the AGGREGATE of a
 * `min` or a `max`, and its frame entry is a domain identifier.  A number
 * slot is bound by an ASSIGN of a computed integer or of another number
 * slot, or by the AGGREGATE of a `count` or a `sum`; the integer is kept
 * beside the frame.  A text slot is bound by an ASSIGN of a string a
 * function computed or of another text slot; its bytes are kept in the
 * executor's text stack.  The frame entry of a number or a text slot is the
 * value's identifier when the context has assigned the value one, and zero
 * when it has not.  Zero names no value and is in no tuple and no key, so
 * every operation over a relation reads such a slot through BOUND like any
 * other and matches nothing when the value is new, which is right, since a
 * relation holds no value the context has not assigned.  Only an EMIT needs
 * the value itself, to store it, and reads a number slot through
 * RELIC_PLAN_NUMBER and a text slot through RELIC_PLAN_TEXT.
 */
enum relic_term_action {
	RELIC_PLAN_CONSTANT,	/* must equal an interned constant */
	RELIC_PLAN_BOUND,	/* must equal a slot bound before this column */
	RELIC_PLAN_NUMBER,	/* an EMIT's number slot: its integer */
	RELIC_PLAN_TEXT,	/* an EMIT's text slot: its bytes */
	RELIC_PLAN_BIND,	/* binds its slot from the tuple */
	RELIC_PLAN_ANY		/* `_`: matches anything, binds nothing */
};

/*
 * Whether an EMIT column read through `action` holds a value that already
 * has an identifier.  A constant does, and so does BOUND: the planner turns
 * every EMIT read of a number or a text slot into NUMBER or TEXT, so an
 * EMIT's BOUND reads a domain slot.  NUMBER and TEXT hold what the round
 * computed, which may be new.  BIND and ANY never occur in an EMIT.  The
 * switch names every action, so the compiler asks for a decision on one
 * added later.
 */
static inline bool
relic_emit_term_assigned(enum relic_term_action action)
{
	switch (action) {
	case RELIC_PLAN_CONSTANT:
	case RELIC_PLAN_BOUND:
		return (true);
	case RELIC_PLAN_NUMBER:
	case RELIC_PLAN_TEXT:
	case RELIC_PLAN_BIND:
	case RELIC_PLAN_ANY:
		return (false);
	}
	return (false);
}

/*
 * The kind of a slot, as the paragraph above sets out, and so of the value
 * a postfix program leaves: an identifier, a computed integer, or a
 * computed string.
 */
enum relic_slot_kind {
	RELIC_SLOT_DOMAIN,
	RELIC_SLOT_NUMBER,
	RELIC_SLOT_TEXT
};

struct relic_plan_term {
	union {
		relic_dom_t	constant;	/* RELIC_PLAN_CONSTANT */
		uint16_t	slot;		/* BOUND, NUMBER, TEXT, BIND */
	}			u;
	enum relic_term_action	action;		/* which arm of u is live */
};

/*
 * Which of a relation's tuples an operation may read.
 *
 * A member of the recursive component being evaluated semi-naively has a
 * delta: the half-open range [first, end) of the identifiers of the tuples it
 * gained in the previous round.  That one range defines three views of the
 * member, and with the whole relation they are the four an operation reads
 * through:
 *
 *	all	[1, relation end), the end taken when the plan starts
 *	old	[1, first), the member's tuples older than its delta
 *	delta	[first, end), what the member gained in the previous round
 *	current	[1, end), everything the member held when the round began
 *
 * `all` is the view of every operation of an ordinary plan and of every
 * operation over a complete relation.  The other three occur only in a
 * semi-naive delta variant, only over members of the component, and are
 * assigned by body order: the delta literal reads `delta`, a member literal
 * written before it `old`, and one written after it `current`.  That is what
 * makes every derivation happen in exactly one variant of one round.
 *
 * None of the member views reaches past the delta's end, which was the
 * member's end when the round began, so nothing a round inserts is visible
 * to any variant of that round.
 */
enum relic_view {
	RELIC_VIEW_ALL,		/* the relation when the plan started */
	RELIC_VIEW_OLD,		/* a member: tuples older than its delta */
	RELIC_VIEW_DELTA,	/* a member: its delta */
	RELIC_VIEW_CURRENT	/* a member: everything up to its delta's end */
};

/*
 * An operation over a relation: SCAN, LOOKUP, MEMBER, EXISTS or
 * ANTI_MEMBER.  An EMIT is not one: it carries its own head schema and
 * names no relation.
 * `terms` holds a term for each of the relation's `arity` columns and is
 * NULL when that arity is zero.  The arity is the relation's, copied when
 * the operation is planned, since it never changes and the executor reads
 * it for every tuple.
 *
 * The relation is held directly rather than as a predicate identifier to
 * resolve on every step.  A relation never moves once created, so a plan may
 * keep the pointer for as long as the relation exists.  It is held
 * read-only: a plan reads relations.  Whoever takes an EMIT's tuples holds
 * the relation it writes, through a pointer of its own.
 *
 * `view` is the view a SCAN, LOOKUP, MEMBER, EXISTS or ANTI_MEMBER reads
 * through, fixed by the planner; an ANTI_MEMBER's is always `all`.  The
 * range it denotes changes from one round to the next, and for `all` from
 * one plan to the next, so the evaluation resolves it when the plan
 * starts.
 *
 * `key_mask` and `index_position` are set on a LOOKUP, an EXISTS and an
 * ANTI_MEMBER: the columns it looks up by, bit c for column c, and the
 * position in relation->indexes of the index keyed on exactly those columns.
 * The planner sets the mask; relic_plan_prepare() sets the position, which
 * in a draft's operations (relic_plan_draft_t) is unset and means nothing.
 * The index is named by
 * position because positions are fixed until the relations are next
 * discarded, which no plan outlives, and addresses are not.  The mask of an
 * EXISTS or an ANTI_MEMBER, the probes, is one of the three shapes of enum
 * relic_probe_shape, which the planner chooses and the executor and explain
 * read back through relic_plan_probe_shape() alone.
 */
struct relic_op_atom {
	const relic_relation_t	*relation;
	relic_plan_term_t	*terms;
	uint32_t		 key_mask;
	uint32_t		 index_position;
	enum relic_view		 view;
	uint16_t		 arity;
};

/*
 * The head an EMIT derives: a term per column, `arity` of them, and the
 * predicate `pred` the head names, or RELIC_ID_NONE when the plan is a
 * query's and the head names none.  The relation the tuples are written
 * into belongs to the taker that runs the plan, not to the operation, so
 * an EMIT cannot be read as a source.
 */
struct relic_op_emit {
	relic_plan_term_t	*terms;
	relic_pred_id_t		 pred;
	uint16_t		 arity;
};

/*
 * One step of a postfix program: push a constant's identifier, a domain
 * slot's identifier, a number slot's integer or a text slot's bytes, pop one
 * integer and push its negation, pop two and push what a binary operator
 * makes of them, or pop a function's arguments and push its result.  A CALL
 * of match whose pattern was a constant carries the pattern compiled with
 * the rule, and every other CALL carries NULL.
 */
enum relic_step_kind {
	RELIC_STEP_CONSTANT,
	RELIC_STEP_DOMAIN_SLOT,
	RELIC_STEP_NUMBER_SLOT,
	RELIC_STEP_TEXT_SLOT,
	RELIC_STEP_NEGATE,
	RELIC_STEP_BINARY,
	RELIC_STEP_CALL
};

struct relic_step {
	union {
		relic_dom_t		constant;	/* CONSTANT */
		uint16_t		slot;		/* the three SLOTs */
		enum relic_arith_op	op;		/* BINARY */
		struct {
			const relic_pattern_t	*pattern;
			enum relic_function	 fn;
		}			call;		/* CALL */
	}			u;
	enum relic_step_kind	kind;
};

/*
 * An expression in the form the executor runs, a postfix program: its tree's
 * nodes in postfix order, each operand's steps before its operator's, so
 * that running the steps in order over a stack leaves the expression's value
 * as the one cell left.  A term is a postfix program of one step.
 *
 * `depth` is the most cells the run holds at once.  A leaf holds one, and a
 * node with arguments holds each earlier argument's cell while a later one
 * is evaluated, so its need is the greatest of need(argument i) + i over
 * its arguments, counting i from zero.  A level of a tree adds at most two
 * cells, since no node has more than three arguments, so a tree of height
 * h needs at most 2h - 1 cells, and RELIC_POSTFIX_DEPTH_MAX is that bound
 * at the greatest height the parser admits.
 *
 * Which kind of slot each step reads is part of the step, decided with the
 * plan, since a slot's kind is a fact about the plan and not the rule.
 */
struct relic_postfix {
	relic_step_t	*steps;
	size_t		 step_count;
	uint8_t		 depth;
};

#define	RELIC_POSTFIX_DEPTH_MAX	(2 * RELIC_MAX_EXPRESSION_DEPTH - 1)

_Static_assert(RELIC_MAX_FUNCTION_ARITY <= 3,
    "a level of a tree adds at most two cells to a program's stack");

/*
 * Where the strings an operation's programs compute are built in the
 * executor's text stack: above the string the last ASSIGN before the
 * operation that binds a text slot keeps there, `text_base` being that
 * ASSIGN's position in the plan plus one, or from the stack's start when
 * `text_base` is zero because no ASSIGN before it binds one.  Every slot
 * such an operation reads was bound at or before that ASSIGN, or by an
 * operation that keeps nothing on the stack, so what it builds overwrites
 * no string a slot it reads names.
 */

/*
 * A comparison of the values of two postfix programs.  `where` is where the
 * comparison was written, which a note about how it compared and an error in
 * computing it point at, and `comparison` is the comparison as the rule holds
 * it, which explain prints.  `text_base` is as the paragraph above says.
 */
struct relic_op_filter {
	relic_postfix_t			 left;
	relic_postfix_t			 right;
	const relic_comparison_t	*comparison;
	relic_location_t		 where;
	size_t				 text_base;
	enum relic_compare_op		 compare;
};

/*
 * An assignment: bind `slot`, of kind `kind`, to the value of `value`.
 * `expr` is the expression as the rule holds it, which explain prints, and
 * `where` and `text_base` are as for a FILTER.
 */
struct relic_op_assign {
	relic_postfix_t		 value;
	const relic_expr_t	*expr;
	relic_location_t	 where;
	size_t			 text_base;
	enum relic_slot_kind	 kind;
	uint16_t		 slot;
};

/*
 * How an AGGREGATE reaches the rows of its group, the tuples of the
 * relation it folds whose key columns hold the key: by walking the
 * whole relation, matching the key columns of each tuple when there are
 * any; by walking the index keyed on exactly the key columns; or, when
 * every column is a key column, by probing for the one tuple the key names.
 * A count with no key reads the complete view's tuple count. Other folds
 * with no key walk the whole relation, which is then the group, as do
 * keyed folds under RELIC_F_NO_INDEX.
 */
enum relic_fold_access {
	RELIC_FOLD_SCAN,
	RELIC_FOLD_LOOKUP,
	RELIC_FOLD_PROBE,
	RELIC_FOLD_CARDINALITY
};

/*
 * A fold of the relation `relation`, of `arity` columns, for the key the
 * slots `key_slots` hold, `key_count` of them: the slot of each key column,
 * in increasing column order, bit c of `key_mask` marking column c.  The
 * relation is a group relation, whose key columns are its leading ones, or
 * the relation of an atom folded in place, whose key columns are whichever
 * its shared variables stand in.  `access` says how the group's rows are
 * reached, and for RELIC_FOLD_LOOKUP `index_position` names the index
 * keyed on `key_mask` as a LOOKUP's does, once relic_plan_prepare() has set
 * it.  `fn` folds column `column` of
 * each row, which `count` does not read.
 *
 * `binds` says whether the fold binds `slot` or, bound before, compares its
 * value with it; `kind` is the slot's kind, a number slot when a count or
 * a sum binds it and a domain slot when a min or a max does, and whatever
 * the operation that bound it made it when it is compared.  `aggregate` is
 * the literal as the rule holds it, which explain prints, and `where` is
 * where its function is written, which a note about the fold and an
 * overflowing sum point at.
 */
struct relic_op_aggregate {
	const relic_relation_t	*relation;
	const relic_aggregate_t	*aggregate;
	const uint16_t		*key_slots;
	relic_location_t	 where;
	uint32_t		 key_mask;
	uint32_t		 index_position;	/* RELIC_FOLD_LOOKUP */
	enum relic_fold_access	 access;
	enum relic_aggregate_fn	 fn;
	uint16_t		 arity;
	uint16_t		 key_count;
	uint16_t		 column;
	enum relic_slot_kind	 kind;
	uint16_t		 slot;
	bool			 binds;
};

struct relic_op {
	union {
		relic_op_atom_t		atom;	/* over a relation's tuple */
		relic_op_emit_t		emit;	/* EMIT */
		relic_op_filter_t	filter;	/* FILTER */
		relic_op_assign_t	assign;	/* ASSIGN */
		relic_op_aggregate_t	aggregate; /* AGGREGATE */
	}			u;
	enum relic_op_kind	kind;		/* which arm of u is live */
};

/*
 * One rule's plan, as the executor runs it.  Four invariants hold for every
 * plan relic_plan_prepare() makes, and the executor relies on them without
 * checking:
 *
 *	the last operation is EMIT, and it is the only one;
 *	every BOUND or NUMBER term and every slot a program or a fold reads
 *	is one an earlier operation, or an earlier column of the same
 *	operation, binds, a generator, an ASSIGN or an AGGREGATE;
 *	every test reads only constants and bound slots;
 *	every operation that reads a secondary index names, by position, a
 *	published index of its relation keyed on its mask.
 *
 * A plan that was built therefore always has `ops`.
 *
 * A plan lives in the evaluation arena and is discarded when the evaluation
 * it was built for ends.
 */
struct relic_plan {
	relic_op_t	*ops;
	size_t		 op_count;
};

/*
 * A secondary index a plan's operations read: the one of `relation` keyed
 * on the columns of `key_mask`.  It names the index whether or not the
 * relation has built it.
 */
struct relic_plan_access {
	relic_relation_t	*relation;
	uint32_t		 key_mask;
};

/*
 * A plan as the planner emits it, before its indexes are bound: `ops` and
 * `op_count` as a plan's, except that an operation names the index it reads
 * by its mask alone and its position means nothing, and `accesses`, the
 * indexes the operations read, `access_count` of them, each once, in the
 * order the operations first read them.  Every invariant of a plan holds
 * of a draft except that its operations name a published index by
 * position, which is what relic_plan_prepare() establishes when it makes a
 * plan of one.  The executor never reads a draft, so emitting one
 * builds no index: a draft can be held, explained and estimated without
 * the relations it reads paying for the indexes it would need.
 */
struct relic_plan_draft {
	const relic_op_t		*ops;
	const relic_plan_access_t	*accesses;
	size_t				 op_count;
	size_t				 access_count;
};

/*
 * How the work estimator counts one operation of a plan (lib/cost.c).
 * Each operation has exactly one class, chosen by its kind and, for an
 * AGGREGATE, by how it reaches its group's rows.  B is the estimated
 * bindings reaching the operation, N the tuples of the view it reads, and
 * Q = ceil(B * N / max(1, K)) the tuples a lookup visits, K being the
 * distinct keys of the index it reads:
 *
 *	SCAN		B * N visits; B * N bindings onward
 *	LOOKUP		B probes and Q visits; Q onward
 *	PROBE		a MEMBER or an EXISTS: B probes; none onward from an
 *			empty view, B otherwise
 *	ANTI_PROBE	an ANTI_MEMBER: B probes; B onward
 *	OPERATION	a FILTER or an ASSIGN: B operations; B onward
 *	FOLD_SCAN	B folds and B * N group-row visits; B onward
 *	FOLD_LOOKUP	B folds and Q visits; B onward
 *	FOLD_PROBE	B folds and B visits, none when the relation is
 *			empty; B onward
 *	EMIT		B emissions
 */
enum relic_cost_kind {
	RELIC_COST_SCAN,
	RELIC_COST_LOOKUP,
	RELIC_COST_PROBE,
	RELIC_COST_ANTI_PROBE,
	RELIC_COST_OPERATION,
	RELIC_COST_FOLD_SCAN,
	RELIC_COST_FOLD_LOOKUP,
	RELIC_COST_FOLD_PROBE,
	RELIC_COST_EMIT
};

/*
 * One step of a work recipe: the operation `op` of the candidate's draft,
 * counted as `kind` says.  A step of kind LOOKUP or FOLD_LOOKUP reads K
 * from the index of its operation's relation keyed on its mask, when the
 * relation has built it.  A recipe is complete once compiled and nothing
 * writes it afterwards.
 */
struct relic_cost_step {
	const relic_op_t	*op;
	enum relic_cost_kind	 kind;
};

/*
 * A plan's work recipe: a step for each of its operations, in plan order.
 * It is compiled from the draft and reads nothing the draft does not, so
 * estimating it can neither change the plan nor run any of it.
 */
struct relic_cost_recipe {
	relic_cost_step_t	*steps;
	size_t			 step_count;
};

/*
 * One of a delta variant's plans, as a draft, with the recipe that
 * estimates its work.  The recipe is compiled only when the variant has two
 * plans to choose between; a lone plan is never estimated and its recipe
 * is empty.  What runs a candidate is the plan relic_plan_prepare() makes
 * of its draft, which the evaluation holds.
 */
struct relic_plan_candidate {
	relic_plan_draft_t	draft;
	relic_cost_recipe_t	recipe;
};

#define	RELIC_FAMILY_DELTA_FIRST	0
#define	RELIC_FAMILY_INPUT_FIRST	1
#define	RELIC_FAMILY_MAX		2

/*
 * The plans of one semi-naive delta variant, between which each round
 * chooses by estimated work, known only when the round starts.
 *
 * `count` is one or two.  With two, candidate RELIC_FAMILY_DELTA_FIRST
 * places the delta literal before every other positive literal and
 * candidate RELIC_FAMILY_INPUT_FIRST prefers any other positive literal to
 * the delta when their bound columns tie; the two orders differ, since an
 * order both policies reach is kept once.  With one, the candidate is the
 * variant's only plan: the orders coincided, or a reference configuration
 * asked for one.
 *
 * Every candidate reads each literal through the same view, so all of a
 * family's plans derive the same tuples and differ only in cost.
 */
struct relic_plan_family {
	relic_plan_candidate_t	candidates[RELIC_FAMILY_MAX];
	size_t			count;
};

/*
 * The delta literal of a rule's ordinary plan, in which every literal reads
 * its whole relation: none.
 */
#define	RELIC_PLAN_NO_DELTA	SIZE_MAX

/*
 * What the planner assumes about a relation it cannot measure, one the
 * component being planned is still deriving: how many tuples it holds, and
 * by how much binding one of its columns narrows it.  A member a variant
 * reads beside its own delta, through the old or current view, holds the
 * deltas of the rounds before and is taken to hold RELIC_PLAN_DELTAS_HELD
 * deltas' worth when it is ranked against the relations it is measured
 * beside.  These guesses only rank literals against each other; neither
 * a variant's delta literal nor which of its plans exist depends on them.
 */
#define	RELIC_PLAN_UNKNOWN_TUPLES	1000u
#define	RELIC_PLAN_UNKNOWN_DISTINCT	10u
#define	RELIC_PLAN_DELTAS_HELD		10u

/*
 * Every column of a relation of this arity, bit c for column c.
 */
static inline uint32_t
relic_plan_all_columns(uint16_t arity)
{
	if (arity >= 32)
		return (UINT32_MAX);

	return ((UINT32_C(1) << arity) - 1);
}

/*
 * How a probe, an EXISTS or an ANTI_MEMBER, asks whether its relation holds,
 * inside its view, a tuple that agrees with its terms on the columns its key
 * names:
 *
 *	MEMBER	the key is every column: find the tuple through the canonical
 *		index, as a MEMBER does
 *	INDEX	the key is a nonempty proper subset: read the first tuple of
 *		the key's chain in the secondary index at `index_position`
 *	SCAN	no key: scan the view for the first tuple the terms match, `_`
 *		matching anything, which is how a probe naming no column is
 *		asked, and one naming some under RELIC_F_NO_INDEX
 *
 * A relation of arity zero has no column, so its every-column key is the
 * empty one, and it is a MEMBER.  An EXISTS always leaves a column out, so
 * its probe is never a MEMBER: a positive literal whose every column is
 * bound is planned as the MEMBER operation itself.
 */
enum relic_probe_shape {
	RELIC_PROBE_MEMBER,
	RELIC_PROBE_INDEX,
	RELIC_PROBE_SCAN
};

/*
 * The shape of the probe an EXISTS or an ANTI_MEMBER keyed on `key_mask` over
 * a relation of this arity makes.
 */
static inline enum relic_probe_shape
relic_plan_probe_shape(uint32_t key_mask, uint16_t arity)
{
	if (key_mask == relic_plan_all_columns(arity))
		return (RELIC_PROBE_MEMBER);

	if (key_mask != 0)
		return (RELIC_PROBE_INDEX);

	return (RELIC_PROBE_SCAN);
}

#endif /* RELIC_PLAN_H */
