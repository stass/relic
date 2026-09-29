/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdint.h>

#include "internal.h"

/*
 * Questions about a compiled expression tree, and about a comparison of
 * two, that the checker, the planner and the Magic rewrite each ask.
 *
 * Every function here is pure: it reads a tree and changes nothing.  Each
 * walks the tree recursively, which is bounded because no tree is taller
 * than RELIC_MAX_EXPRESSION_DEPTH, a bound the parser enforces where the
 * expression is written.
 *
 * The rule deciding when an equality binds a variable rather than testing
 * it is stated once, in relic_comparison_assigns(), so that the safety
 * check, which asks whether every variable can be bound, and the planner,
 * which decides where each is, cannot disagree about it.
 */

static bool	 expr_is_variable(const relic_expr_t *expr, uint16_t *slotp);
static bool	 side_assigns(const relic_expr_t *target,
		    const relic_expr_t *value, uint64_t bound, uint16_t *slotp);

/*
 * The slots an expression reads: every variable among its leaves.
 */
uint64_t
relic_expr_slots(const relic_expr_t *expr)
{
	switch (expr->kind) {
	case RELIC_EXPR_CONSTANT:
		return (0);
	case RELIC_EXPR_VARIABLE:
		return (UINT64_C(1) << expr->u.slot);
	case RELIC_EXPR_NEGATE:
		return (relic_expr_slots(expr->u.operand));
	case RELIC_EXPR_BINARY:
		return (relic_expr_slots(expr->u.binary.left) |
		    relic_expr_slots(expr->u.binary.right));
	}

	return (0);
}

/*
 * The slots a comparison reads, on either side.
 */
uint64_t
relic_comparison_slots(const relic_comparison_t *comparison)
{
	return (relic_expr_slots(comparison->left) |
	    relic_expr_slots(comparison->right));
}

/*
 * The nodes of both of a comparison's expressions.
 */
size_t
relic_comparison_nodes(const relic_comparison_t *comparison)
{
	return (comparison->left->nodes + comparison->right->nodes);
}

/*
 * Whether an expression is a bare variable, and which slot it is.
 */
static bool
expr_is_variable(const relic_expr_t *expr, uint16_t *slotp)
{
	if (expr->kind != RELIC_EXPR_VARIABLE)
		return (false);

	*slotp = expr->u.slot;

	return (true);
}

/*
 * Whether `target` names a variable nothing in `bound` binds and `value`
 * reads only slots `bound` binds.
 */
static bool
side_assigns(const relic_expr_t *target, const relic_expr_t *value,
    uint64_t bound, uint16_t *slotp)
{
	uint16_t slot;

	if (!expr_is_variable(target, &slot))
		return (false);

	if ((bound & (UINT64_C(1) << slot)) != 0)
		return (false);

	if ((relic_expr_slots(value) & ~bound) != 0)
		return (false);

	*slotp = slot;

	return (true);
}

/*
 * Whether a comparison, placed where the slots in `bound` are bound, is an
 * assignment: an equality one side of which is a bare variable not bound,
 * and whose other side reads only bound slots.  Such an equality holds for
 * exactly one value of the variable, the other side's, so it binds the
 * variable to that value rather than testing it.  The slot it binds is
 * stored in *slotp and the expression it binds it to in *valuep.
 *
 * Only a bare variable is assigned: `X + 1 = Y` binds Y when X is bound, and
 * binds nothing when Y is bound and X is not, since solving for a variable
 * inside an expression is not attempted.  An equality with both sides free,
 * or one side free inside an expression, is not an assignment, and a
 * variable only it mentions is bound by nothing.  Every other comparison is
 * a test of values already bound.
 */
bool
relic_comparison_assigns(const relic_comparison_t *comparison, uint64_t bound,
    uint16_t *slotp, const relic_expr_t **valuep)
{
	if (comparison->op != RELIC_CMP_EQ)
		return (false);

	if (side_assigns(comparison->left, comparison->right, bound, slotp)) {
		*valuep = comparison->right;
		return (true);
	}

	if (side_assigns(comparison->right, comparison->left, bound, slotp)) {
		*valuep = comparison->left;
		return (true);
	}

	return (false);
}

/*
 * Whether two expressions are the same tree over the same slots and
 * constants.
 */
bool
relic_expr_equal(const relic_expr_t *a, const relic_expr_t *b)
{
	if (a->kind != b->kind)
		return (false);

	switch (a->kind) {
	case RELIC_EXPR_CONSTANT:
		return (a->u.constant == b->u.constant);
	case RELIC_EXPR_VARIABLE:
		return (a->u.slot == b->u.slot);
	case RELIC_EXPR_NEGATE:
		return (relic_expr_equal(a->u.operand, b->u.operand));
	case RELIC_EXPR_BINARY:
		return (a->u.binary.op == b->u.binary.op &&
		    relic_expr_equal(a->u.binary.left, b->u.binary.left) &&
		    relic_expr_equal(a->u.binary.right, b->u.binary.right));
	}

	return (false);
}
