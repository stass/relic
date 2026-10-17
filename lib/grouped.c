/*
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Certify one full grouped count and pair its unchanged producer plan with
 * a counted destination. Ordinary evaluation owns execution, publication
 * and serial/deferred failure handling; this owner retains no pending values.
 */
#include "config.h"

#include <strings.h>

#include "internal.h"

#define GROUP_VISITS 256u

typedef struct grouped_answer grouped_answer_t;

struct grouped_answer {
	relic_t			*ctx;
	const relic_query_t	*query;
	const relic_rule_t	*producer;
	uint16_t		 key_columns[RELIC_MAX_ARITY];
	uint16_t		 width;
};

static uint16_t
atom_arity(const relic_t *ctx, const relic_atom_t *atom)
{
	return (ctx->program.predicates.entries[atom->pred - 1].arity);
}

static bool
distinct_variables(const relic_arg_t *args, uint16_t arity)
{
	uint64_t seen;
	uint16_t i;

	seen = 0;
	for (i = 0; i < arity; i++) {
		if (args[i].kind != RELIC_ARG_VARIABLE ||
		    (seen & (UINT64_C(1) << args[i].u.slot)) != 0)
			return (false);
		seen |= UINT64_C(1) << args[i].u.slot;
	}
	return (true);
}

/* Header searches and literal inspections spend the same bounded allowance. */
static const relic_rule_t *
unique_rule(relic_t *ctx, relic_pred_id_t pred, size_t *visits)
{
	const relic_rule_t *rule;
	size_t i;

	if (ctx->program.predicates.entries[pred - 1].program_rules != 1 ||
	    relic_base_count(ctx, pred) != 0)
		return (NULL);
	for (i = 0; i < ctx->program.rules.count; i++) {
		if (*visits == GROUP_VISITS)
			return (NULL);
		(*visits)++;
		rule = &ctx->program.rules.entries[i];
		if (rule->head != pred)
			continue;
		if (rule->body_length > GROUP_VISITS - *visits)
			return (NULL);
		*visits += rule->body_length;
		return (rule);
	}
	return (NULL);
}

/* Recognize the root, the count guarded by a projection, and its producer. */
static bool
recognize_query(grouped_answer_t *answer, const relic_rule_t **resultp,
    const relic_rule_t **guardp, const relic_atom_t **guard_atomp,
    const relic_aggregate_t **foldp)
{
	const relic_rule_t *root, *result, *guard;
	const relic_atom_t *root_atom, *guard_atom, *fold_atom;
	const relic_aggregate_t *fold;
	const relic_literal_t *literal;
	uint16_t root_arity, key_arity, i;
	size_t visits;

	root = &answer->query->rule;
	if (answer->query->group_rule_count != 0 || root->body_length != 1 ||
	    root->body[0].kind != RELIC_LITERAL_POSITIVE)
		return (false);
	root_atom = &root->body[0].u.atom;
	root_arity = atom_arity(answer->ctx, root_atom);
	if (root->head_arity != root_arity ||
	    !distinct_variables(root_atom->args, root_arity))
		return (false);

	visits = 2;
	result = unique_rule(answer->ctx, root_atom->pred, &visits);
	if (result == NULL || result->body_length != 2)
		return (false);
	guard_atom = NULL;
	fold = NULL;
	for (i = 0; i < 2; i++) {
		literal = &result->body[i];
		if (literal->kind == RELIC_LITERAL_POSITIVE)
			guard_atom = &literal->u.atom;
		else if (literal->kind == RELIC_LITERAL_AGGREGATE)
			fold = &literal->u.aggregate;
	}
	if (guard_atom == NULL || fold == NULL)
		return (false);
	key_arity = atom_arity(answer->ctx, guard_atom);
	if (fold->fn != RELIC_AGG_COUNT || fold->shared_count != key_arity ||
	    result->head_arity != key_arity + 1 ||
	    !distinct_variables(guard_atom->args, key_arity) ||
	    !distinct_variables(result->head_args, result->head_arity) ||
	    fold->body->head != RELIC_ID_NONE || fold->body->body_length != 1 ||
	    visits > GROUP_VISITS - 2 ||
	    fold->body->body[0].kind != RELIC_LITERAL_POSITIVE)
		return (false);
	visits += 2;
	fold_atom = &fold->body->body[0].u.atom;
	if (fold_atom->pred != fold->source ||
	    !distinct_variables(fold_atom->args, atom_arity(answer->ctx, fold_atom)))
		return (false);

	guard = unique_rule(answer->ctx, guard_atom->pred, &visits);
	if (guard == NULL || guard->body_length != 1 ||
	    guard->body[0].kind != RELIC_LITERAL_POSITIVE ||
	    guard->body[0].u.atom.pred != fold->source ||
	    !distinct_variables(guard->head_args, guard->head_arity))
		return (false);
	answer->producer = unique_rule(answer->ctx, fold->source, &visits);
	if (answer->producer == NULL)
		return (false);
	answer->width = key_arity;
	*resultp = result;
	*guardp = guard;
	*guard_atomp = guard_atom;
	*foldp = fold;
	return (true);
}

/*
 * A key's guard-head position and fold slot must describe the same P column.
 * Keeping these maps in P order also determines pending-value publication.
 */
static bool
map_columns(grouped_answer_t *answer, const relic_rule_t *result,
    const relic_rule_t *guard, const relic_atom_t *guard_atom,
    const relic_aggregate_t *fold, uint16_t *output)
{
	const relic_rule_t *root = &answer->query->rule;
	const relic_atom_t *root_atom = &root->body[0].u.atom;
	const relic_atom_t *projection = &guard->body[0].u.atom;
	uint16_t result_columns[RELIC_MAX_ARITY];
	uint16_t producer_arity, i, j, key;
	uint64_t projected_slots;

	producer_arity = answer->producer->head_arity;
	key = 0;
	projected_slots = 0;
	for (i = 0; i < producer_arity; i++) {
		if (projection->args[i].kind == RELIC_ARG_CONSTANT)
			return (false);
		if (projection->args[i].kind == RELIC_ARG_VARIABLE) {
			if ((projected_slots & (UINT64_C(1) << projection->args[i].u.slot)) != 0)
				return (false);
			projected_slots |= UINT64_C(1) << projection->args[i].u.slot;
		}
		if ((fold->key_mask & (UINT32_C(1) << i)) == 0)
			continue;
		if (key >= answer->width || projection->args[i].kind != RELIC_ARG_VARIABLE)
			return (false);
		for (j = 0; j < guard->head_arity; j++)
			if (guard->head_args[j].u.slot == projection->args[i].u.slot)
				break;
		if (j == guard->head_arity || guard_atom->args[j].u.slot != fold->shared[key])
			return (false);
		answer->key_columns[key++] = i;
	}
	if (key != answer->width)
		return (false);

	for (i = 0; i < result->head_arity; i++) {
		for (j = 0; j < answer->width; j++)
			if (result->head_args[i].u.slot == fold->shared[j])
				break;
		if (j == answer->width && result->head_args[i].u.slot != fold->result)
			return (false);
		result_columns[i] = j;
	}
	for (i = 0; i < root->head_arity; i++) {
		for (j = 0; j < root->head_arity; j++)
			if (root->head_args[i].kind == RELIC_ARG_VARIABLE &&
			    root->head_args[i].u.slot == root_atom->args[j].u.slot)
				break;
		if (j == root->head_arity)
			return (false);
		output[i] = result_columns[j];
	}
	return (true);
}

static bool
certify(grouped_answer_t *answer, uint16_t *output)
{
	const relic_rule_t *result, *guard;
	const relic_atom_t *guard_atom;
	const relic_aggregate_t *fold;
	const relic_literal_t *literal;
	size_t i;
	bool source;

	if (!recognize_query(answer, &result, &guard, &guard_atom, &fold))
		return (false);
	source = false;
	for (i = 0; i < answer->producer->body_length; i++) {
		literal = &answer->producer->body[i];
		if (literal->kind == RELIC_LITERAL_COMPARISON)
			continue;
		if (literal->kind != RELIC_LITERAL_POSITIVE || source ||
		    relic_pred_is_derived(answer->ctx, literal->u.atom.pred))
			return (false);
		source = true;
	}
	if (!source)
		return (false);
	return (map_columns(answer, result, guard, guard_atom, fold, output));
}


/* A declined certificate publishes nothing; a started evaluation is final. */
int
relic_grouped_answer(relic_t *ctx, const relic_query_t *query,
    const relic_row_sink_t *rows)
{
	grouped_answer_t answer;
	relic_destination_t destination;
	relic_arena_t arena;
	relic_plan_t plan;
	uint16_t output[RELIC_MAX_ARITY];
	int status;

	bzero(&answer, sizeof(answer));
	bzero(&destination, sizeof(destination));
	answer.ctx = ctx;
	answer.query = query;
	if (ctx->config.max_tuples != 0 || ctx->config.max_memory != 0 ||
	    ctx->config.max_magic != 0 || !certify(&answer, output))
		return (0);

	relic_arena_init_charged(&arena, &ctx->account);
	relic_discard_derived(ctx);
	status = -1;
	if (relic_plan_rule(ctx, &arena, answer.producer, NULL, &plan) != 0)
		goto done;
	status = relic_destination_count(ctx, &destination, answer.producer,
	    &plan, answer.key_columns, answer.width, output, query->rule.head_arity);
	if (status <= 0)
		goto done;
	status = -1;
	relic_explain_counted(ctx, answer.producer, &plan,
	    answer.key_columns, answer.width, query->answer);
	if (relic_refuse_interrupted(ctx, "deriving", query->answer) != 0)
		goto done;
	if (relic_evaluate_counted(ctx, query, answer.producer, &plan,
	    &destination, rows) == 0)
		status = 1;
done:
	relic_destination_close(ctx, &destination);
	relic_arena_reset(&arena);
	return (status);
}
