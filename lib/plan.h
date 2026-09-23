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
 * runs, so its absence is meaningful.  EMIT inserts the head tuple and never
 * succeeds, which is what makes it the last operation of every plan: the
 * deepest level of the join always rejects, so the executor always
 * backtracks from it.
 */
enum relic_op_kind {
	RELIC_OP_SCAN,		/* match and bind every tuple of a relation */
	RELIC_OP_LOOKUP,	/* the same, through an index on bound columns */
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
 * RELIC_PLAN_BIND and RELIC_PLAN_ANY occur only in a SCAN or a LOOKUP, and
 * never in a LOOKUP's key columns.  A test and an EMIT read constants and
 * bound slots and nothing else.
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
 * An operation over a relation: SCAN, LOOKUP, MEMBER, ANTI_MEMBER or EMIT.
 * `terms` holds the relation's arity of columns and is NULL when that arity
 * is zero.
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
 *
 * `key_mask` and `index_position` are set only on a LOOKUP: the columns it
 * looks up by, bit c for column c, and the position in relation->indexes of
 * the index keyed on exactly those columns.  The index is named by position
 * because positions are fixed until the relations are next discarded, which
 * no plan outlives, and addresses are not.
 */
struct relic_op_atom {
	relic_relation_t	*relation;
	relic_plan_term_t	*terms;
	uint32_t		 key_mask;		/* LOOKUP only */
	uint32_t		 index_position;	/* LOOKUP only */
	bool			 delta;
};

/*
 * A comparison of two values, each a constant or a bound slot.  `where` is
 * where the comparison was written, which a note about how it compared
 * points at.
 */
struct relic_op_filter {
	relic_plan_term_t	left;
	relic_plan_term_t	right;
	relic_location_t	where;
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

/*
 * What the planner assumes about a relation it cannot measure, one the
 * component being planned is still deriving: how many tuples it holds, and
 * by how much binding one of its columns narrows it.
 */
#define	RELIC_PLAN_UNKNOWN_TUPLES	1000u
#define	RELIC_PLAN_UNKNOWN_DISTINCT	10u

#endif /* RELIC_PLAN_H */
