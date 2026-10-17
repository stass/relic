/*
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_DESTINATION_H
#define RELIC_DESTINATION_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frozen.h"
#include "plan.h"

typedef struct relic_count_store relic_count_store_t;
typedef struct relic_destination relic_destination_t;
typedef struct relic_destination_append relic_destination_append_t;
typedef struct relic_destination_view relic_destination_view_t;

/* A closed distinction: sets keep one occurrence, counts add contributions. */
enum relic_destination_kind {
	RELIC_DESTINATION_SET,
	RELIC_DESTINATION_COUNT
};

/*
 * Workers borrow this immutable schema, which exposes no writable store.
 * Projection preserves producer-column order. A counted construction proves
 * that the complete producer emits at most source_bound tuples, so combining
 * its disjoint contributions cannot overflow a 32-bit word.
 */
struct relic_destination_view {
	const relic_relation_t	*relation;
	uint16_t		 columns[RELIC_MAX_ARITY];
	uint32_t		 source_bound;
	uint16_t		 input_arity;
	uint16_t		 arity;
	enum relic_destination_kind kind;
};

const relic_destination_view_t *relic_destination_view(
	    const relic_destination_t *destination);
const relic_relation_t *relic_destination_relation(
	    const relic_destination_t *destination);
const relic_relation_t *relic_destination_set_relation(
	    const relic_destination_t *destination);
uint16_t relic_destination_input_arity(const relic_destination_t *destination);
uint16_t relic_destination_arity(const relic_destination_t *destination);
void	 relic_destination_project(const relic_destination_view_t *view,
	    relic_dom_t *tuples, size_t count);

static inline uint32_t
relic_contribution_combine(const relic_destination_view_t *view,
    uint32_t first, uint32_t additional)
{
	if (view->kind == RELIC_DESTINATION_SET)
		return (first);
	assert(additional <= view->source_bound);
	assert(first <= view->source_bound - additional);
	return (first + additional);
}

#ifndef RELIC_FROZEN_ONLY
#include "relation.h"

/* SET borrows its relation; COUNT owns the anonymous relation and count store. */
struct relic_destination {
	relic_relation_t		*relation;
	relic_count_store_t	*count;
	relic_destination_view_t	 view;
};

/*
 * A prepared append owns the relation's staging and counted contributions.
 * It starts zeroed, with relation.count/first/growth supplied by the merge.
 * Close is required after any open attempt, successful or not.
 */
struct relic_destination_append {
	relic_append_t		 relation;
	uint32_t		*contributions;
	size_t			 charged;
};

/*
 * Set construction borrows a non-NULL relation and cannot fail. Counted
 * construction checks the original producer and derives its source bound:
 * 1 owns an empty counted destination, 0 declines with no allocation, and
 * -1 reports a preparation failure. Close accepts a zeroed or constructed
 * destination, releases COUNT storage and its charge, and leaves SET storage
 * with its owner. A borrowed producer plan outlives its counted destination.
 * COUNT copies both maps, checks output as a permutation of keys plus count,
 * and checks the supplied result arity against the derived key arity plus one.
 */
void	 relic_destination_set(relic_destination_t *destination,
	    relic_relation_t *relation);
int	 relic_destination_count(relic_t *ctx, relic_destination_t *destination,
	    const relic_rule_t *rule, const relic_plan_t *plan,
	    const uint16_t *columns, uint16_t width,
	    const uint16_t *output, uint16_t result_arity);
void	 relic_destination_close(relic_t *ctx,
	    relic_destination_t *destination);

/* Pair and round admission return 0, or -1 with an internal error reported. */
int	 relic_destination_pair(relic_t *ctx,
	    const relic_destination_t *destination, const relic_rule_t *rule,
	    const relic_plan_t *plan);
int	 relic_destination_begin_round(relic_t *ctx,
	    const relic_destination_t *destination, const relic_plan_t *plan,
	    size_t items, size_t targets);
const char *relic_destination_describe(relic_t *ctx,
	    const relic_destination_t *destination, char *buffer, size_t size);
int	 relic_destination_refuse_interrupted(relic_t *ctx, const char *verb,
	    const relic_destination_t *destination);
uint32_t relic_destination_count_value(const relic_destination_t *destination,
	    relic_tuple_id_t id);

/*
 * Read a completed destination after all count integers have been published.
 * SET borrows its stored tuple; COUNT fills the caller's result buffer through
 * its checked permutation. The buffer holds at least result_arity domain
 * values. Neither operation allocates or publishes values.
 */
uint16_t relic_destination_result_arity(const relic_destination_t *destination);
const relic_dom_t *relic_destination_result(const relic_t *ctx,
	    const relic_destination_t *destination, relic_tuple_id_t id,
	    relic_dom_t *result);

/*
 * Consume canonical tuples in order. Returns 0, or -1 with a diagnostic and
 * *completed naming the successful prefix, as ordinary relation insertion
 * does. Counts are reserved and initialized before a new key is installed.
 */
int	 relic_destination_insert(relic_t *ctx,
	    relic_destination_t *destination, const relic_dom_t *tuples,
	    size_t count, bool *added, size_t *completed);

/*
 * Deferred preparation reports nothing: open/reserve return 0 or -1.
 * A refused reservation changes no contents; grown capacities stay owned
 * and accounted until close or retry. Stage follows successful open. Take
 * follows successful reservation and value publication: it installs counts
 * before keys in one interval with no failure, callback or interruption.
 * Canonical-index application and finish use the contained relation append.
 */
int	 relic_destination_append_open(relic_t *ctx,
	    const relic_destination_t *destination,
	    relic_destination_append_t *append);
void	 relic_destination_stage(const relic_destination_t *destination,
	    relic_destination_append_t *append, size_t position,
	    const relic_dom_t *tuple, uint64_t hash, uint32_t contribution);
size_t	 relic_destination_bytes_during_append(
	    const relic_destination_t *destination,
	    const relic_destination_append_t *append);
int	 relic_destination_reserve_append(relic_t *ctx,
	    relic_destination_t *destination, relic_destination_append_t *append);
void	 relic_destination_take_append(relic_t *ctx,
	    relic_destination_t *destination,
	    const relic_destination_append_t *append);
void	 relic_destination_append_close(relic_t *ctx,
	    relic_destination_append_t *append);
#endif
#endif
