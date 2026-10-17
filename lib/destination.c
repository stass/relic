/*
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * A destination owns the distinction between retaining a set of tuples and
 * counting their occurrences. Value publication belongs to the evaluator.
 * Count storage is indexed by the ordinary canonical relation's tuple IDs;
 * every path reserves and initializes counts before installing their keys.
 * The borrowed producer operations seal the complete pass whose source
 * bounds every contribution; no destination can accept an unrelated round.
 */
#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <strings.h>

#include "internal.h"

struct relic_count_store {
	uint32_t	*values;
	size_t		 capacity;
	const relic_op_t *producer_ops;
	relic_pred_id_t producer_pred;
	uint16_t	 output[RELIC_MAX_ARITY];
};

void
relic_destination_set(relic_destination_t *destination,
    relic_relation_t *relation)
{
	bzero(destination, sizeof(*destination));
	destination->relation = relation;
	destination->view.relation = relation;
	destination->view.input_arity = relation->arity;
	destination->view.arity = relation->arity;
	destination->view.kind = RELIC_DESTINATION_SET;
}

/* 1 constructed, 0 outside the counted grammar, -1 already reported. */
int
relic_destination_count(relic_t *ctx, relic_destination_t *destination,
    const relic_rule_t *rule, const relic_plan_t *plan,
    const uint16_t *columns, uint16_t width,
    const uint16_t *output, uint16_t result_arity)
{
	const relic_op_t *emit;
	uint16_t i, key;
	uint64_t seen;
	size_t operation;

	bzero(destination, sizeof(*destination));
	if (plan->op_count < 2 || plan->ops[0].kind != RELIC_OP_SCAN ||
	    plan->ops[0].u.atom.view != RELIC_VIEW_ALL ||
	    !relic_plan_derives_distinct(plan))
		return (0);
	emit = &plan->ops[plan->op_count - 1];
	if (emit->kind != RELIC_OP_EMIT || width > emit->u.emit.arity ||
	    emit->u.emit.arity > RELIC_MAX_ARITY || width >= RELIC_MAX_ARITY ||
	    result_arity != width + 1)
		return (0);
	seen = 0;
	for (i = 0; i < result_arity; i++) {
		if (output[i] > width ||
		    (seen & (UINT64_C(1) << output[i])) != 0)
			return (0);
		seen |= UINT64_C(1) << output[i];
	}
	for (operation = 1; operation + 1 < plan->op_count; operation++)
		if (plan->ops[operation].kind != RELIC_OP_ASSIGN &&
		    plan->ops[operation].kind != RELIC_OP_FILTER)
			return (0);
	for (key = 0; key < width; key++) {
		if (columns[key] >= emit->u.emit.arity ||
		    (key != 0 && columns[key] <= columns[key - 1]))
			return (0);
		destination->view.columns[key] = columns[key];
	}
	for (i = 0; i < emit->u.emit.arity; i++) {
		for (key = 0; key < width; key++)
			if (columns[key] == i)
				break;
		if (key != width)
			continue;
		if (emit->u.emit.terms[i].action != RELIC_PLAN_CONSTANT &&
		    emit->u.emit.terms[i].action != RELIC_PLAN_BOUND)
			return (0);
	}
	destination->relation = relic_relation_create(ctx, RELIC_ID_NONE, 0, width);
	if (destination->relation == NULL)
		return (-1);
	destination->count = calloc(1, sizeof(*destination->count));
	if (destination->count == NULL) {
		relic_relation_free(destination->relation);
		destination->relation = NULL;
		relic_diag_no_memory(ctx);
		return (-1);
	}
	destination->view.relation = destination->relation;
	destination->view.input_arity = emit->u.emit.arity;
	destination->view.arity = width;
	destination->view.source_bound =
	    (uint32_t)relic_relation_count(plan->ops[0].u.atom.relation);
	destination->view.kind = RELIC_DESTINATION_COUNT;
	destination->count->producer_ops = plan->ops;
	destination->count->producer_pred = rule->head;
	bcopy(output, destination->count->output,
	    result_arity * sizeof(*output));
	if (relic_destination_pair(ctx, destination, rule, plan) != 0) {
		relic_destination_close(ctx, destination);
		return (-1);
	}
	return (1);
}

const relic_destination_view_t *
relic_destination_view(const relic_destination_t *destination)
{
	return (&destination->view);
}

const relic_relation_t *
relic_destination_relation(const relic_destination_t *destination)
{
	return (destination->relation);
}

const relic_relation_t *
relic_destination_set_relation(const relic_destination_t *destination)
{
	if (destination->view.kind == RELIC_DESTINATION_SET)
		return (destination->relation);
	return (NULL);
}

uint16_t
relic_destination_input_arity(const relic_destination_t *destination)
{
	return (destination->view.input_arity);
}

uint16_t
relic_destination_arity(const relic_destination_t *destination)
{
	return (destination->view.arity);
}

const char *
relic_destination_describe(relic_t *ctx, const relic_destination_t *destination,
    char *buffer, size_t size)
{
	if (destination->view.kind == RELIC_DESTINATION_COUNT)
		return (relic_pred_describe(ctx, destination->count->producer_pred,
		    buffer, size));
	return (relic_relation_describe(ctx, destination->relation, buffer, size));
}

int
relic_destination_refuse_interrupted(relic_t *ctx, const char *verb,
    const relic_destination_t *destination)
{
	char described[RELIC_PRED_TEXT_MAX];

	if (!relic_signal_flag_raised(&ctx->interrupted))
		return (0);
	return (relic_refuse_interrupted_subject(ctx, verb,
	    relic_destination_describe(ctx, destination, described,
	    sizeof(described))));
}

uint32_t
relic_destination_count_value(const relic_destination_t *destination,
    relic_tuple_id_t id)
{
	assert(destination->view.kind == RELIC_DESTINATION_COUNT);
	assert(id != 0 && id <= relic_relation_count(destination->relation));
	return (destination->count->values[id - 1]);
}

uint16_t
relic_destination_result_arity(const relic_destination_t *destination)
{
	return (destination->view.arity +
	    (destination->view.kind == RELIC_DESTINATION_COUNT));
}

const relic_dom_t *
relic_destination_result(const relic_t *ctx,
    const relic_destination_t *destination, relic_tuple_id_t id,
    relic_dom_t *result)
{
	const relic_dom_t *key;
	relic_dom_t count;
	uint16_t column, source;

	key = relic_relation_tuple(destination->relation, id);
	if (key == NULL || destination->view.kind == RELIC_DESTINATION_SET)
		return (key);
	count = relic_value_find_integer(ctx,
	    relic_destination_count_value(destination, id));
	assert(count != RELIC_ID_NONE);
	for (column = 0; column < relic_destination_result_arity(destination); column++) {
		source = destination->count->output[column];
		result[column] = source == destination->view.arity ? count : key[source];
	}
	return (result);
}

int
relic_destination_pair(relic_t *ctx, const relic_destination_t *destination,
    const relic_rule_t *rule, const relic_plan_t *plan)
{
	const relic_op_t *emit;

	emit = plan->op_count == 0 ? NULL : &plan->ops[plan->op_count - 1];
	if (emit == NULL || emit->kind != RELIC_OP_EMIT ||
	    emit->u.emit.arity != rule->head_arity ||
	    emit->u.emit.arity != destination->view.input_arity) {
		relic_diag_internal(ctx, &rule->where,
		    "a rule's plan and its consumer have different arities");
		return (-1);
	}
	return (0);
}

int
relic_destination_begin_round(relic_t *ctx,
    const relic_destination_t *destination,
    const relic_plan_t *plan, size_t items, size_t targets)
{
	if (destination->view.kind == RELIC_DESTINATION_SET)
		return (0);
	if (items != 1 || targets != 1 || plan == NULL || plan->op_count < 2 ||
	    plan->ops != destination->count->producer_ops ||
	    relic_relation_count(plan->ops[0].u.atom.relation) !=
	    destination->view.source_bound ||
	    !relic_plan_derives_distinct(plan) ||
	    relic_relation_count(destination->relation) != 0) {
		relic_diag_internal(ctx, NULL, "a counted destination requires one complete producer into empty keys");
		return (-1);
	}
	return (0);
}

void
relic_destination_project(const relic_destination_view_t *view,
    relic_dom_t *tuples, size_t count)
{
	uint16_t width, column;
	size_t row;

	width = view->arity;
	if (width == view->input_arity)
		return;
	for (row = 0; row < count; row++)
		for (column = 0; column < width; column++)
			tuples[row * width + column] = tuples[row * view->input_arity +
			    view->columns[column]];
}

/* Counts grow by actual new keys, identically in serial and deferred appends. */
static int
reserve_counts(relic_t *ctx, relic_destination_t *destination, size_t added)
{
	uint32_t *counts;
	size_t capacity, bytes, old;

	if (destination->view.kind == RELIC_DESTINATION_SET || added == 0)
		return (0);
	capacity = relic_grow_array_capacity_stepwise(destination->count->capacity,
	    relic_relation_count(destination->relation), added, 1);
	if (capacity == 0 || capacity > SIZE_MAX / sizeof(*counts))
		return (-1);
	if (capacity == destination->count->capacity)
		return (0);
	bytes = capacity * sizeof(*counts);
	if (!relic_account_charge_round(&ctx->account, bytes))
		return (-1);
	counts = realloc(destination->count->values, bytes);
	if (counts == NULL) {
		relic_account_release_round(&ctx->account, bytes);
		return (-1);
	}
	old = destination->count->capacity * sizeof(*counts);
	destination->count->values = counts;
	destination->count->capacity = capacity;
	relic_account_release_round(&ctx->account, old);
	return (0);
}

int
relic_destination_insert(relic_t *ctx, relic_destination_t *destination,
    const relic_dom_t *tuples, size_t count, bool *added, size_t *completed)
{
	const relic_dom_t *tuple;
	relic_tuple_id_t id;
	size_t row;

	if (destination->view.kind == RELIC_DESTINATION_SET)
		return (relic_relation_insert_batch(ctx, destination->relation,
		    tuples, count, added, completed));
	*completed = 0;
	bzero(added, count * sizeof(*added));
	for (row = 0; row < count; row++) {
		tuple = tuples + row * destination->relation->arity;
		id = relic_relation_find(destination->relation, tuple);
		if (id != RELIC_ID_NONE) {
			destination->count->values[id - 1] = relic_contribution_combine(
			    &destination->view, destination->count->values[id - 1], 1);
		} else {
			if (reserve_counts(ctx, destination, 1) != 0) {
				relic_diag_no_memory(ctx);
				return (-1);
			}
			id = relic_relation_end(destination->relation);
			destination->count->values[id - 1] = 1;
			if (relic_relation_insert(ctx, destination->relation, tuple,
			    &added[row]) != 0)
				return (-1);
			assert(added[row]);
		}
		(*completed)++;
	}
	return (0);
}

int
relic_destination_append_open(relic_t *ctx,
    const relic_destination_t *destination, relic_destination_append_t *append)
{
	size_t bytes;

	if (relic_relation_append_open(ctx, destination->relation,
	    &append->relation) != 0)
		return (-1);
	if (destination->view.kind == RELIC_DESTINATION_SET ||
	    append->relation.count == 0)
		return (0);
	bytes = relic_saturating_product(append->relation.count,
	    sizeof(*append->contributions));
	if (bytes == SIZE_MAX)
		return (-1);
	if (!relic_account_charge_round(&ctx->account, bytes))
		return (-1);
	append->contributions = malloc(bytes);
	if (append->contributions == NULL) {
		relic_account_release_round(&ctx->account, bytes);
		return (-1);
	}
	append->charged = bytes;
	return (0);
}

void
relic_destination_stage(const relic_destination_t *destination,
    relic_destination_append_t *append, size_t position,
    const relic_dom_t *tuple, uint64_t hash, uint32_t contribution)
{
	assert(contribution != 0);
	relic_relation_stage(destination->relation, &append->relation, position,
	    tuple, hash);
	if (destination->view.kind == RELIC_DESTINATION_COUNT)
		append->contributions[position] = contribution;
}

size_t
relic_destination_bytes_during_append(const relic_destination_t *destination,
    const relic_destination_append_t *append)
{
	size_t capacity, growth;

	growth = 0;
	if (destination->view.kind == RELIC_DESTINATION_COUNT &&
	    append->relation.count != 0) {
		capacity = relic_grow_array_capacity_stepwise(destination->count->capacity,
		    relic_relation_count(destination->relation), append->relation.count, 1);
		if (capacity == 0 ||
		    capacity > SIZE_MAX / sizeof(*destination->count->values))
			return (SIZE_MAX);
		if (capacity != destination->count->capacity)
			growth = capacity * sizeof(*destination->count->values);
	}
	return (relic_saturating_sum(relic_relation_bytes_during_append(
	    destination->relation, &append->relation), growth));
}

int
relic_destination_reserve_append(relic_t *ctx, relic_destination_t *destination,
    relic_destination_append_t *append)
{
	if (reserve_counts(ctx, destination, append->relation.count) != 0)
		return (-1);
	return (relic_relation_reserve_append(ctx, destination->relation,
	    &append->relation));
}

void
relic_destination_take_append(relic_t *ctx, relic_destination_t *destination,
    const relic_destination_append_t *append)
{
	if (destination->view.kind == RELIC_DESTINATION_COUNT &&
	    append->relation.count != 0)
		bcopy(append->contributions,
		    destination->count->values + append->relation.first - 1,
		    append->relation.count * sizeof(*append->contributions));
	relic_relation_take_append(ctx, destination->relation, &append->relation);
}

void
relic_destination_append_close(relic_t *ctx, relic_destination_append_t *append)
{
	relic_relation_append_close(ctx, &append->relation);
	free(append->contributions);
	relic_account_release_round(&ctx->account, append->charged);
	bzero(append, sizeof(*append));
}

void
relic_destination_close(relic_t *ctx, relic_destination_t *destination)
{
	if (destination->view.kind == RELIC_DESTINATION_COUNT) {
		if (destination->relation != NULL) {
			relic_relation_clear(ctx, destination->relation);
			relic_relation_free(destination->relation);
		}
		free(destination->count->values);
		relic_account_release_round(&ctx->account,
		    destination->count->capacity * sizeof(*destination->count->values));
		free(destination->count);
	}
	bzero(destination, sizeof(*destination));
}
