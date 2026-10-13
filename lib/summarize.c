/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <strings.h>

#include "cost.h"
#include "internal.h"

/*
 * The choice of the summaries of stored relations a component's folds
 * read (specs/design-group-reduction.md, reusable summaries), made at the
 * component's turn, before any of its plans is prepared and so before any
 * index they would read is built.
 *
 * The component's plans are drafted as they will be prepared (lib/eval.c,
 * choose_summaries()).  A fold a draft would answer by reading rows,
 * through a lookup or a walk of the relation, whose family (lib/uses.c)
 * has no summary yet, makes its family a candidate.  Each candidate is
 * weighed by lib/cost.c from the bindings the draft is estimated to bring
 * to each of its folds, the relation's size, the groups its key is
 * estimated to have, the accumulators it would update, and the index its
 * lookups would build, unless that index is built already or another of
 * the drafts' operations reads it.  An index a later component reads is
 * not foreseen, and is then built beside the summary.  The candidates of
 * one relation are chosen together, since one walk of the relation builds
 * them all, and those chosen are built before the component's plans are
 * made, which then read them in place of the rows.
 *
 * Every estimate is a count the store keeps or the walk derives from the
 * draft, so the choice depends on the relations and the rules alone: not
 * on the number of workers, the time, or a predicate's name.  A draft of
 * a recursive rule reads its own component's relations empty, so a fold
 * after such a read is reached by no binding, and a family no binding
 * reaches is never chosen; a summary the run already holds serves it all
 * the same.
 */

typedef struct relic_summary_candidate relic_summary_candidate_t;

/*
 * A family the drafts would fold by reading rows: its relation, its cost
 * as lib/cost.c weighs it, and whether a fold of it is a lookup, whose
 * index the folds may be alone in needing.
 */
struct relic_summary_candidate {
	relic_fold_family_t	*family;
	relic_relation_t	*relation;
	relic_cost_summary_t	 cost;
	bool			 looks_up;
};

static bool	 reads_rows(relic_t *ctx, const relic_op_t *op,
		    relic_fold_family_t **familyp);
static int	 estimate_groups(relic_t *ctx, relic_relation_t *relation,
		    uint32_t key_mask, relic_tuple_id_t *groupsp);
static relic_summary_candidate_t *candidate_of(relic_t *ctx,
		    relic_summary_candidate_t *candidates, size_t *countp,
		    relic_fold_family_t *family);
static bool	 index_needed_anyway(const relic_plan_draft_t *drafts,
		    size_t draft_count, const relic_summary_candidate_t *candidate);
static void	 group_by_relation(relic_summary_candidate_t *candidates,
		    size_t count, relic_summary_candidate_t *spare);
static int	 build_chosen(relic_t *ctx, relic_arena_t *arena,
		    relic_summary_candidate_t *candidates, size_t count,
		    const relic_fold_family_t **built, size_t *builtp);

int
relic_summarize_drafts(relic_t *ctx, relic_arena_t *arena,
    const relic_plan_draft_t *drafts, size_t draft_count,
    const relic_exec_env_t *env, const relic_fold_family_t ***builtp,
    size_t *built_countp)
{
	const relic_fold_family_t **built;
	relic_summary_candidate_t *candidates;
	relic_summary_candidate_t *candidate;
	relic_fold_family_t *family;
	const relic_op_t *op;
	size_t folds;
	size_t count;
	size_t at;
	size_t d;
	size_t i;

	*builtp = NULL;
	*built_countp = 0;

	folds = 0;
	for (d = 0; d < draft_count; d++)
		for (i = 0; i < drafts[d].op_count; i++)
			if (reads_rows(ctx, &drafts[d].ops[i], &family))
				folds++;
	if (folds == 0)
		return (0);

	candidates = relic_arena_allocarray(arena, 2 * folds,
	    sizeof(*candidates));
	built = relic_arena_allocarray(arena, folds, sizeof(*built));
	if (candidates == NULL || built == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	count = 0;
	for (d = 0; d < draft_count; d++) {
		for (i = 0; i < drafts[d].op_count; i++) {
			op = &drafts[d].ops[i];
			if (!reads_rows(ctx, op, &family))
				continue;
			candidate = candidate_of(ctx, candidates, &count,
			    family);
			if (candidate == NULL)
				return (-1);
			if (op->u.aggregate.access == RELIC_FOLD_LOOKUP)
				candidate->looks_up = true;
			relic_cost_add_fold(&candidate->cost, &op->u.aggregate,
			    relic_cost_reaching(drafts[d].ops, i, env));
		}
	}

	for (at = 0; at < count; at++) {
		candidate = &candidates[at];
		if (candidate->looks_up &&
		    !index_needed_anyway(drafts, draft_count, candidate))
			relic_cost_add_index(&candidate->cost,
			    (relic_tuple_id_t)relic_relation_count(
			    candidate->relation));
	}

	group_by_relation(candidates, count, candidates + folds);
	if (build_chosen(ctx, arena, candidates, count, built,
	    built_countp) != 0)
		return (-1);
	*builtp = built;

	return (0);
}

/*
 * Whether an operation is a fold the draft answers by reading rows, a
 * lookup or a walk of the relation, of a family not yet summarized, which
 * it gives in *familyp.
 */
static bool
reads_rows(relic_t *ctx, const relic_op_t *op,
    relic_fold_family_t **familyp)
{
	if (op->kind != RELIC_OP_AGGREGATE ||
	    (op->u.aggregate.access != RELIC_FOLD_LOOKUP &&
	    op->u.aggregate.access != RELIC_FOLD_SCAN))
		return (false);

	*familyp = relic_reduction_family(ctx, op->u.aggregate.aggregate);

	return (*familyp != NULL);
}

/*
 * Estimate into *groupsp the groups of `relation` on the key columns of
 * `key_mask`: the exact key count of its index on them when it has built
 * one, and otherwise the product of the key columns' distinct values,
 * which no relation's groups exceed, and never more than its tuples.
 */
static int
estimate_groups(relic_t *ctx, relic_relation_t *relation, uint32_t key_mask,
    relic_tuple_id_t *groupsp)
{
	relic_tuple_id_t tuples;
	uint64_t groups;
	uint32_t position;
	size_t distinct;
	uint16_t column;

	tuples = (relic_tuple_id_t)relic_relation_count(relation);
	if (relic_relation_index_find(relation, key_mask, &position)) {
		*groupsp = (relic_tuple_id_t)relic_index_key_count(
		    relic_relation_index_at(relation, position));
		return (0);
	}

	groups = tuples > 0 ? 1 : 0;
	for (column = 0; column < relation->arity && groups < tuples;
	    column++) {
		if ((key_mask & (UINT32_C(1) << column)) == 0)
			continue;
		if (relic_relation_distinct(ctx, relation, column,
		    &distinct) != 0)
			return (-1);
		groups = distinct == 0 || groups <= tuples / distinct ?
		    groups * distinct : tuples;
	}

	*groupsp = groups < tuples ? (relic_tuple_id_t)groups : tuples;
	return (0);
}

/*
 * The candidate of `family` among the `*countp` made so far, made when it
 * is the first fold of it, its groups estimated then.  The caller has
 * room for one per fold.
 */
static relic_summary_candidate_t *
candidate_of(relic_t *ctx, relic_summary_candidate_t *candidates, size_t *countp,
    relic_fold_family_t *family)
{
	relic_summary_candidate_t *candidate;
	size_t i;

	for (i = 0; i < *countp; i++)
		if (candidates[i].family == family)
			return (&candidates[i]);

	candidate = &candidates[*countp];
	bzero(candidate, sizeof(*candidate));
	candidate->family = family;
	candidate->cost.accumulators = family->signature_count;
	candidate->relation = relic_relation_read(ctx, family->pred);
	if (candidate->relation == NULL)
		return (NULL);
	if (estimate_groups(ctx, candidate->relation, family->key_mask,
	    &candidate->cost.groups) != 0)
		return (NULL);
	(*countp)++;

	return (candidate);
}

/*
 * Whether the index a candidate's lookups would read is built already, or
 * an operation of the drafts other than a fold reads it, so that it is
 * built whether or not the folds read a summary, and is no saving of one.
 */
static bool
index_needed_anyway(const relic_plan_draft_t *drafts, size_t draft_count,
    const relic_summary_candidate_t *candidate)
{
	const relic_op_t *op;
	uint32_t position;
	size_t d;
	size_t i;

	if (relic_relation_index_find(candidate->relation,
	    candidate->family->key_mask, &position))
		return (true);
	for (d = 0; d < draft_count; d++) {
		for (i = 0; i < drafts[d].op_count; i++) {
			op = &drafts[d].ops[i];
			if (op->kind != RELIC_OP_AGGREGATE &&
			    relic_plan_reads_index(op, candidate->relation,
			    candidate->family->key_mask))
				return (true);
		}
	}

	return (false);
}

/*
 * Order the candidates so that those of one relation are adjacent, the
 * relations in the order of their first candidate and each one's
 * candidates in the order they were made, through `spare`, which has room
 * for them all.
 */
static void
group_by_relation(relic_summary_candidate_t *candidates, size_t count,
    relic_summary_candidate_t *spare)
{
	size_t placed;
	size_t i;
	size_t j;

	placed = 0;
	for (i = 0; i < count; i++) {
		for (j = 0; j < i; j++)
			if (candidates[j].relation == candidates[i].relation)
				break;
		if (j < i)
			continue;
		for (j = i; j < count; j++)
			if (candidates[j].relation == candidates[i].relation)
				spare[placed++] = candidates[j];
	}

	bcopy(spare, candidates, count * sizeof(*candidates));
}

/*
 * Choose among each relation's candidates, adjacent in `candidates`, and
 * build those chosen in one walk of the relation, adding them to the
 * `*builtp` of `built`.
 */
static int
build_chosen(relic_t *ctx, relic_arena_t *arena,
    relic_summary_candidate_t *candidates, size_t count,
    const relic_fold_family_t **built, size_t *builtp)
{
	relic_fold_family_t **chosen;
	relic_cost_summary_t *costs;
	size_t first;
	size_t end;
	size_t n;
	size_t i;

	chosen = relic_arena_allocarray(arena, count, sizeof(*chosen));
	costs = relic_arena_allocarray(arena, count, sizeof(*costs));
	if (chosen == NULL || costs == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	for (first = 0; first < count; first = end) {
		for (end = first; end < count &&
		    candidates[end].relation == candidates[first].relation;
		    end++)
			costs[end - first] = candidates[end].cost;
		if (!relic_cost_choose_summaries(costs, end - first,
		    (relic_tuple_id_t)relic_relation_count(
		    candidates[first].relation)))
			continue;

		n = 0;
		for (i = first; i < end; i++)
			if (costs[i - first].chosen)
				chosen[n++] = candidates[i].family;
		if (relic_summarize_relation(ctx, arena,
		    candidates[first].relation, chosen, n) != 0)
			return (-1);
		for (i = 0; i < n; i++)
			built[(*builtp)++] = chosen[i];
	}

	return (0);
}
