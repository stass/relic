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
#include "relation.h"
#include "util.h"

/*
 * A plan: how one rule is executed, as an ordered array of operations.
 *
 * The order of the array is the nesting of the join.  Each operation either
 * produces bindings (a generator), lets a binding through or rejects it (a
 * test), or inserts the head tuple, and each states every decision in advance
 * -- which relation it reads, and for every column whether it is matched
 * against a constant, matched against a slot already bound, binds a slot, or
 * is ignored.  The executor therefore makes no decision a plan could have
 * made, and every optimization is a change in what the planner emits rather
 * than in how a plan is run.
 */

typedef struct relic_op relic_op_t;
typedef struct relic_op_atom relic_op_atom_t;
typedef struct relic_op_filter relic_op_filter_t;
typedef struct relic_plan relic_plan_t;
typedef struct relic_plan_term relic_plan_term_t;

/*
 * The operations.
 *
 * SCAN is a generator: it succeeds once per tuple of its relation that
 * matches, binding the columns it binds.  MEMBER, ANTI_MEMBER and FILTER are
 * tests: they read only constants and bound slots, bind nothing, and succeed
 * at most once.  ANTI_MEMBER is MEMBER with the answer inverted, which is the
 * whole of negation: the schedule evaluates a negated relation completely
 * before any rule that reads it runs, so its absence is meaningful.  EMIT
 * inserts the head tuple and never succeeds, which is what makes it the last
 * operation of every plan: the deepest level of the join always rejects, so
 * the executor always backtracks from it.
 */
enum relic_op_kind {
	RELIC_OP_SCAN,		/* match and bind every tuple of a relation */
	RELIC_OP_MEMBER,	/* is this fully bound tuple present? */
	RELIC_OP_ANTI_MEMBER,	/* is this fully bound tuple absent? */
	RELIC_OP_FILTER,	/* does a comparison of bound values hold? */
	RELIC_OP_EMIT		/* insert the head tuple */
};

/*
 * What an operation does with one column, decided when the plan is built.
 *
 * Whether a slot is bound at a given point in a plan is static: it is bound
 * when an earlier operation, or an earlier column of the same operation,
 * binds it.  The planner therefore settles which of these applies to every
 * column, and the executor carries no bound-mask and never asks.
 *
 * RELIC_PLAN_BIND and RELIC_PLAN_ANY occur only in a SCAN.  A test and an
 * EMIT read constants and bound slots and nothing else.
 */
enum relic_term_action {
	RELIC_PLAN_CONSTANT,	/* must equal an interned constant */
	RELIC_PLAN_BOUND,	/* must equal a slot bound before this column */
	RELIC_PLAN_BIND,	/* binds its slot from the tuple */
	RELIC_PLAN_ANY		/* `_`: matches anything, binds nothing */
};

struct relic_plan_term {
	union {
		relic_dom_t	constant;	/* RELIC_PLAN_CONSTANT */
		uint16_t	slot;		/* BOUND and BIND */
	}			u;
	enum relic_term_action	action;		/* which arm of u is live */
};

/*
 * An operation over a relation: SCAN, MEMBER, ANTI_MEMBER or EMIT.  `terms`
 * holds the relation's arity of columns and is NULL when that arity is zero.
 *
 * The relation is held directly rather than as a predicate identifier to
 * resolve on every step.  A relation never moves once created, so a plan may
 * keep the pointer for as long as the relation exists.
 *
 * `delta` is set only on the SCAN a semi-naive variant begins with.  That
 * scan walks the tuples its relation gained in the previous iteration rather
 * than the whole relation; which identifiers those are changes from one
 * iteration to the next, so the evaluation supplies the range when the scan
 * starts.
 */
struct relic_op_atom {
	relic_relation_t	*relation;
	relic_plan_term_t	*terms;
	bool			 delta;
};

/*
 * A comparison of two values, each a constant or a bound slot.
 */
struct relic_op_filter {
	relic_plan_term_t	left;
	relic_plan_term_t	right;
	enum relic_compare_op	compare;
};

struct relic_op {
	union {
		relic_op_atom_t		atom;	/* all but FILTER */
		relic_op_filter_t	filter;	/* FILTER */
	}			u;
	enum relic_op_kind	kind;		/* which arm of u is live */
};

/*
 * One rule's plan.  Three invariants hold for every plan the planner builds,
 * and the executor relies on them without checking:
 *
 *	the last operation is EMIT, and it is the only one;
 *	every BOUND term reads a slot an earlier operation, or an earlier
 *	column of the same operation, binds;
 *	every test reads only constants and bound slots.
 *
 * A plan lives in the evaluation arena and is discarded when the evaluation
 * it was built for ends.
 */
struct relic_plan {
	relic_op_t	*ops;
	size_t		 op_count;
};

/*
 * The delta literal argument of relic_plan_rule() that asks for the rule's
 * ordinary plan, in which every literal reads its whole relation.
 */
#define	RELIC_PLAN_NO_DELTA	SIZE_MAX

#endif /* RELIC_PLAN_H */
