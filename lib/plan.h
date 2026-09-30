/*
 * Copyright (c) 2026 Stanislav Sedov
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

typedef struct relic_op relic_op_t;
typedef struct relic_op_aggregate relic_op_aggregate_t;
typedef struct relic_op_assign relic_op_assign_t;
typedef struct relic_op_atom relic_op_atom_t;
typedef struct relic_op_filter relic_op_filter_t;
typedef struct relic_plan relic_plan_t;
typedef struct relic_plan_term relic_plan_term_t;
typedef struct relic_postfix relic_postfix_t;
typedef struct relic_step relic_step_t;
typedef struct relic_variant_plans relic_variant_plans_t;

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
 * MEMBER, ANTI_MEMBER and FILTER are tests: they read only constants and
 * bound slots, bind nothing, and succeed at most once.  ANTI_MEMBER is MEMBER
 * with the answer inverted, which is the whole of negation: the schedule
 * evaluates a negated relation completely before any rule that reads it
 * runs, so its absence is meaningful.  ASSIGN is a test that binds: it
 * computes one value from constants and bound slots and binds one slot to
 * it, succeeding once, or never when the value does not exist because an
 * operand was not an integer.  AGGREGATE is a test that binds too: it folds
 * the rows of a complete group relation under the key its bound slots give
 * and binds its result slot to the value, or, when the slot was bound
 * before, holds exactly when the value equals it; it fails when its
 * function has no value over the group.  EMIT derives the head tuple, which
 * the evaluation inserts, or delivers as a query's row, in batches, and
 * never succeeds, which is what makes it the last operation of every plan:
 * the deepest level of the join always rejects, so the executor always
 * backtracks from it.
 */
enum relic_op_kind {
	RELIC_OP_SCAN,		/* match and bind every tuple of a relation */
	RELIC_OP_LOOKUP,	/* the same, by an index on bound columns */
	RELIC_OP_MEMBER,	/* is this fully bound tuple present? */
	RELIC_OP_ANTI_MEMBER,	/* is this fully bound tuple absent? */
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
 * RELIC_PLAN_BIND and RELIC_PLAN_ANY occur only in a SCAN or a LOOKUP, and
 * never in a LOOKUP's key columns.  A test and an EMIT read constants and
 * bound slots and nothing else.
 *
 * A slot is of one of two kinds, fixed by the operation that binds it.  A
 * domain slot is bound by a generator, by an ASSIGN of a constant or of
 * another domain slot, or by the AGGREGATE of a `min` or a `max`, and its
 * frame entry is a domain identifier.  A number slot is bound by an ASSIGN
 * of a computed integer or of another number slot, or by the AGGREGATE of
 * a `count` or a `sum`; the integer is kept beside the frame, and its frame
 * entry is the
 * integer's identifier when the value table holds one and zero when it does
 * not.  Zero names no value and is in no tuple and no key, so every
 * operation over a relation reads a number slot through BOUND like any other
 * and matches nothing when the integer is new, which is right, since a
 * relation holds no integer the table lacks.  Only an EMIT needs the integer
 * itself, to store it, and reads a number slot through RELIC_PLAN_NUMBER.
 */
enum relic_term_action {
	RELIC_PLAN_CONSTANT,	/* must equal an interned constant */
	RELIC_PLAN_BOUND,	/* must equal a slot bound before this column */
	RELIC_PLAN_NUMBER,	/* an EMIT's number slot: its integer */
	RELIC_PLAN_BIND,	/* binds its slot from the tuple */
	RELIC_PLAN_ANY		/* `_`: matches anything, binds nothing */
};

struct relic_plan_term {
	union {
		relic_dom_t	constant;	/* RELIC_PLAN_CONSTANT */
		uint16_t	slot;		/* BOUND, NUMBER and BIND */
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
 * An operation over a relation: SCAN, LOOKUP, MEMBER, ANTI_MEMBER or EMIT.
 * `terms` holds a term for each of the relation's `arity` columns and is
 * NULL when that arity is zero.  The arity is the relation's, copied when
 * the operation is planned, since it never changes and the executor reads
 * it for every tuple.
 *
 * The relation is held directly rather than as a predicate identifier to
 * resolve on every step.  A relation never moves once created, so a plan may
 * keep the pointer for as long as the relation exists.  It is held
 * read-only: a plan reads relations, and the relation its EMIT names is
 * written by whoever takes the EMIT's tuples, through a pointer of its own.
 *
 * `view` is the view a SCAN, LOOKUP, MEMBER or ANTI_MEMBER reads through,
 * fixed by the planner; an ANTI_MEMBER's is always `all`.  The range it
 * denotes changes from one round to the next, and for `all` from one plan to
 * the next, so the evaluation resolves it when the plan starts.  An EMIT
 * writes past the end of every view of its relation.
 *
 * `key_mask` and `index_position` are set only on a LOOKUP: the columns it
 * looks up by, bit c for column c, and the position in relation->indexes of
 * the index keyed on exactly those columns.  The index is named by position
 * because positions are fixed until the relations are next discarded, which
 * no plan outlives, and addresses are not.
 */
struct relic_op_atom {
	const relic_relation_t	*relation;
	relic_plan_term_t	*terms;
	uint32_t		 key_mask;		/* LOOKUP only */
	uint32_t		 index_position;	/* LOOKUP only */
	enum relic_view		 view;
	uint16_t		 arity;
};

/*
 * One step of a postfix program: push a constant's identifier, a domain slot's
 * identifier or a number slot's integer, pop one integer and push its
 * negation, or pop two and push what a binary operator makes of them.
 */
enum relic_step_kind {
	RELIC_STEP_CONSTANT,
	RELIC_STEP_DOMAIN_SLOT,
	RELIC_STEP_NUMBER_SLOT,
	RELIC_STEP_NEGATE,
	RELIC_STEP_BINARY
};

struct relic_step {
	union {
		relic_dom_t		constant;	/* CONSTANT */
		uint16_t		slot;		/* the two SLOTs */
		enum relic_arith_op	op;		/* BINARY */
	}			u;
	enum relic_step_kind	kind;
};

/*
 * An expression in the form the executor runs, a postfix program: its tree's
 * nodes in postfix order, each operand's steps before its operator's, so
 * that running the steps in order over a stack leaves the expression's value
 * as the one cell left.  A term is a postfix program of one step.  `depth`
 * is the stack cells the run needs, at most RELIC_POSTFIX_DEPTH_MAX:
 * evaluating a tree of height h holds at most h + 1 cells at once, since a
 * node's left operand leaves one cell while its right operand is evaluated.
 *
 * Which kind of slot each step reads is part of the step, decided with the
 * plan, since a slot's kind is a fact about the plan and not the rule.
 */
struct relic_postfix {
	relic_step_t	*steps;
	size_t		 step_count;
	uint8_t		 depth;
};

#define	RELIC_POSTFIX_DEPTH_MAX	(RELIC_MAX_EXPRESSION_DEPTH + 1)

/*
 * A comparison of the values of two postfix programs.  `where` is where the
 * comparison was written, which a note about how it compared and an error in
 * computing it point at, and `comparison` is the comparison as the rule holds
 * it, which explain prints.
 */
struct relic_op_filter {
	relic_postfix_t			 left;
	relic_postfix_t			 right;
	const relic_comparison_t	*comparison;
	relic_location_t		 where;
	enum relic_compare_op		 compare;
};

/*
 * An assignment: bind `slot` to the value of `value`, a domain slot when
 * `number` is false and a number slot when it is true.  `expr` is the
 * expression as the rule holds it, which explain prints, and `where` is as
 * for a FILTER.
 */
struct relic_op_assign {
	relic_postfix_t		 value;
	const relic_expr_t	*expr;
	relic_location_t	 where;
	uint16_t		 slot;
	bool			 number;
};

/*
 * How an AGGREGATE reaches the rows of its group, the tuples of the group
 * relation whose first `key_count` columns hold the key: by walking the
 * whole relation, matching the key columns of each tuple when there are
 * any; by walking the index keyed on exactly the key columns; or, when
 * every column is a key column, by probing for the one tuple the key names.
 * A fold with no key walks the whole relation, which is then the group, and
 * so does one under RELIC_F_NO_INDEX, as a generator with bound columns is
 * scanned under it.
 */
enum relic_fold_access {
	RELIC_FOLD_SCAN,
	RELIC_FOLD_LOOKUP,
	RELIC_FOLD_PROBE
};

/*
 * A fold of the group relation `relation`, of `arity` columns, for the key
 * the slots `key_slots` hold, `key_count` of them, which are the relation's
 * leading columns, bit c of `key_mask` for column c.  `access` says how
 * the group's rows are reached, and for RELIC_FOLD_LOOKUP `index_position`
 * names the index keyed on `key_mask` as a LOOKUP's does.  `fn` folds
 * column `column` of each row, which `count` does not read.
 *
 * `binds` says whether the fold binds `slot` or, bound before, compares its
 * value with it; `number` says the slot is a number slot, which a count or
 * a sum binds and a min or a max does not, and which a compared slot is
 * when the operation that bound it made it one.  `aggregate` is the literal
 * as the rule holds it, which explain prints, and `where` is where its
 * function is written, which a note about the fold and an overflowing sum
 * point at.
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
	uint16_t		 slot;
	bool			 binds;
	bool			 number;
};

struct relic_op {
	union {
		relic_op_atom_t		atom;	/* over a relation's tuple */
		relic_op_filter_t	filter;	/* FILTER */
		relic_op_assign_t	assign;	/* ASSIGN */
		relic_op_aggregate_t	aggregate; /* AGGREGATE */
	}			u;
	enum relic_op_kind	kind;		/* which arm of u is live */
};

/*
 * One rule's plan.  Three invariants hold for every plan the planner builds,
 * and the executor relies on them without checking:
 *
 *	the last operation is EMIT, and it is the only one;
 *	every BOUND or NUMBER term and every slot a program or a fold reads
 *	is one an earlier operation, or an earlier column of the same
 *	operation, binds, a generator, an ASSIGN or an AGGREGATE;
 *	every test reads only constants and bound slots.
 *
 * A plan that was built therefore always has `ops`; a plan left unbuilt, as
 * a variant's plan no round would run is, has `ops` NULL and no operations.
 *
 * A plan lives in the evaluation arena and is discarded when the evaluation
 * it was built for ends.
 */
struct relic_plan {
	relic_op_t	*ops;
	size_t		 op_count;
};

/*
 * The plans of one semi-naive delta variant, between which each round
 * chooses by the size of the delta, known only when the round starts.
 *
 * `placed` orders the delta literal like any other positive literal, and
 * `delta_first` places it before them all.  When the placed order puts
 * another positive literal first, a round whose delta holds D tuples costs
 * the placed plan about its prefix estimate -- the bindings that reach the
 * delta literal, one lookup of the delta each -- and the delta-first plan
 * about D times its fan-out estimate -- the bindings each delta tuple leads
 * to.  A round runs `placed` when D exceeds `threshold`, the prefix over the
 * fan-out, and `delta_first` otherwise.  When the placed order puts the
 * delta literal first anyway, or under RELIC_F_NO_REORDER, there is one
 * order, `threshold` is zero and `placed` is the plan.  Under
 * RELIC_F_NO_INDEX alone, which would scan a placed delta once per binding
 * before it, `delta_first` is the plan and `threshold` RELIC_PLAN_NEVER.
 *
 * Both plans read every literal through the same view, so they derive the
 * same tuples and differ only in cost.  A plan no delta of at least one
 * tuple would select is not built: `placed` when `threshold` is
 * RELIC_PLAN_NEVER, and `delta_first` when `threshold` is zero.  At least
 * one of the two is always built.
 */
struct relic_variant_plans {
	relic_plan_t	placed;
	relic_plan_t	delta_first;
	uint64_t	threshold;
};

#define	RELIC_PLAN_NEVER	UINT64_MAX

/*
 * The delta literal of a rule's ordinary plan, in which every literal reads
 * its whole relation: none.
 */
#define	RELIC_PLAN_NO_DELTA	SIZE_MAX

/*
 * What the planner assumes about a relation it cannot measure, one the
 * component being planned is still deriving: how many tuples it holds, and
 * by how much binding one of its columns narrows it.  A member a variant
 * reads beside its own delta, through the old or current view, is taken to
 * hold RELIC_PLAN_DELTAS_HELD deltas' worth: a member is larger than its
 * delta, which ranks the delta ahead of every other member literal bound
 * alike.
 */
#define	RELIC_PLAN_UNKNOWN_TUPLES	1000u
#define	RELIC_PLAN_UNKNOWN_DISTINCT	10u
#define	RELIC_PLAN_DELTAS_HELD		10u

#endif /* RELIC_PLAN_H */
