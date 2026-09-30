/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdint.h>

#include "internal.h"

/*
 * Questions about a compiled literal that several stages ask.
 *
 * Each is answered here once, so that the stages cannot come to disagree:
 * the safety check and the planner about when an aggregate binds its
 * result, and the dependency graph, the Magic rewrite and the planner about
 * which relation a literal reads.  Every function is pure.
 */

/*
 * Whether a literal reads a relation, and the predicate it reads into
 * *predp: the atom's, whether it is tested for presence or for absence, or
 * an aggregate's group predicate, which it folds.  A comparison reads only
 * values.  Every kind is named, so that one added later is placed here
 * before it compiles.
 */
bool
relic_literal_reads(const relic_literal_t *literal, relic_pred_id_t *predp)
{
	switch (literal->kind) {
	case RELIC_LITERAL_POSITIVE:
	case RELIC_LITERAL_NEGATED:
		*predp = literal->u.atom.pred;
		return (true);
	case RELIC_LITERAL_AGGREGATE:
		*predp = literal->u.aggregate.group;
		return (true);
	case RELIC_LITERAL_COMPARISON:
		break;
	}

	return (false);
}

/*
 * The slots an aggregate's key reads: those its shared columns are bound
 * from.
 */
uint64_t
relic_aggregate_shared_slots(const relic_aggregate_t *aggregate)
{
	uint64_t slots;
	uint16_t i;

	slots = 0;
	for (i = 0; i < aggregate->shared_count; i++)
		slots |= UINT64_C(1) << aggregate->shared[i];

	return (slots);
}

/*
 * Whether an aggregate placed where the slots in `bound` are bound binds its
 * result: whether every slot of its key is bound, so that the fold can run,
 * and its result is not, so that the fold's value is the result's rather
 * than a value to compare it with.
 */
bool
relic_aggregate_binds(const relic_aggregate_t *aggregate, uint64_t bound)
{
	if ((relic_aggregate_shared_slots(aggregate) & ~bound) != 0)
		return (false);

	return ((bound & (UINT64_C(1) << aggregate->result)) == 0);
}

/*
 * How an aggregate function is written.
 */
const char *
relic_aggregate_spelling(enum relic_aggregate_fn fn)
{
	switch (fn) {
	case RELIC_AGG_COUNT:
		return ("count");
	case RELIC_AGG_SUM:
		return ("sum");
	case RELIC_AGG_MIN:
		return ("min");
	case RELIC_AGG_MAX:
		return ("max");
	}

	return ("?");
}
