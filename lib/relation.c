/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * Relations: a set of tuples per predicate.
 *
 * Tuples are stored end to end in one array of domain identifiers with the
 * arity as the stride, and a hash index over the whole tuple answers
 * membership.  Everything the layer above needs follows from those two:
 * duplicate elimination is a lookup before an append, a tuple identifier is a
 * position in the array, and the tuples added since some earlier moment are a
 * range of identifiers rather than a relation of their own.
 *
 * The store is append-only.  A tuple identifier, once assigned, denotes the
 * same tuple for as long as the relation exists, and that is a property of
 * this interface rather than a rule its callers follow: there is no operation
 * that removes, reorders or renumbers a tuple, and the only thing that
 * releases one is destroying the whole table with the context.  Semi-naive
 * evaluation rests on it, because a delta is a pair of identifiers and means
 * nothing if the tuples under them can move.  Future incremental insertion or
 * deletion support has to confront that before it does anything else.
 *
 * A relation is created once and never moves, because the table holds
 * pointers to relations rather than relations.  That is what lets a plan or
 * an evaluation resolve a relation once and hold it.
 */

/*
 * Tuple identifiers are 32-bit and zero names no tuple, so a relation holds
 * at most this many.  Checked rather than assumed: an identifier that wrapped
 * would silently make one tuple into another.
 */
#define	RELIC_TUPLE_MAX		(UINT32_MAX - 1u)

static bool	 columns_holding(size_t tuples, uint16_t arity,
		    size_t *columnsp);
static int	 enforce_limits(relic_t *ctx, const relic_relation_t *relation);
static bool	 exceeds_memory_limit(const relic_t *ctx,
		    const relic_relation_t *relation);
static bool	 exceeds_tuple_limit(const relic_t *ctx);
static size_t	 relation_bytes_after_insert(const relic_relation_t *relation);
static bool	 relation_columns_after_insert(
		    const relic_relation_t *relation, size_t *columnsp);
static relic_relation_t *relation_create(relic_t *ctx, relic_pred_id_t pred);
static relic_tuple_id_t relation_find(const relic_relation_t *relation,
		    const relic_dom_t *columns, uint32_t hash);
static void	 relation_free(relic_relation_t *relation);
static int	 relation_make_room(relic_t *ctx, relic_relation_t *relation);
static int	 relation_reserve(relic_t *ctx, relic_relation_t *relation);
static int	 relations_cover(relic_t *ctx, relic_pred_id_t pred);
static void	 tuple_append(relic_relation_t *relation,
		    const relic_dom_t *columns, uint32_t hash);
static relic_dom_t *tuple_at(const relic_relation_t *relation,
		    relic_tuple_id_t id);
static bool	 tuple_equal(const relic_dom_t *left, const relic_dom_t *right,
		    uint16_t arity);
static uint32_t	 tuple_hash(const relic_dom_t *columns, uint16_t arity);
static uint32_t	 tuple_rehash(uint32_t id, void *ctx);

void
relic_reltab_init(relic_reltab_t *table)
{
	bzero(table, sizeof(*table));
}

/*
 * Release everything one relation owns, and the relation itself.
 */
static void
relation_free(relic_relation_t *relation)
{
	if (relation == NULL)
		return;

	relic_idtab_reset(&relation->index);
	free(relation->columns);
	free(relation);
}

void
relic_reltab_reset(relic_reltab_t *table)
{
	size_t position;

	for (position = 0; position < table->count; position++)
		relation_free(table->entries[position]);

	free(table->entries);
	bzero(table, sizeof(*table));
}

/*
 * Where a tuple's columns begin.  Identifier zero names no tuple, so the
 * array is indexed one lower; this is the only place that translation is
 * written.  The caller must have established that the identifier is within
 * what the relation has room for.
 *
 * The result is writable even though the relation is not, because tuple
 * storage is what a relation holds rather than what it is, and tuple_append()
 * writes a new tuple through this.  Every other caller only reads.
 */
static relic_dom_t *
tuple_at(const relic_relation_t *relation, relic_tuple_id_t id)
{
	return (relation->columns + ((size_t)id - 1) * relation->arity);
}

/*
 * The hash of a whole tuple, which is what the canonical index is keyed on.
 * A tuple of no columns has a defined hash, which is what lets a relation of
 * arity zero use this path like any other.
 */
static uint32_t
tuple_hash(const relic_dom_t *columns, uint16_t arity)
{
	return (relic_hash_words(columns, arity));
}

/*
 * Whether two tuples of one arity hold the same values.
 *
 * The columns are compared as identifiers rather than as bytes.  A dense
 * array of uint32_t has no padding, so a byte comparison would agree, but it
 * is not defined for the empty tuple: comparing zero bytes at a null pointer
 * is something the standard does not permit and a sanitizer reports.  The
 * loop is defined at every arity and needs no case for the one that would
 * break the other form.
 */
static bool
tuple_equal(const relic_dom_t *left, const relic_dom_t *right, uint16_t arity)
{
	uint16_t column;

	for (column = 0; column < arity; column++) {
		if (left[column] != right[column])
			return (false);
	}

	return (true);
}

/*
 * The hash of a tuple already stored, for the index to use when it grows.
 */
static uint32_t
tuple_rehash(uint32_t id, void *ctx)
{
	const relic_relation_t *relation;

	relation = ctx;

	return (tuple_hash(tuple_at(relation, id), relation->arity));
}

/*
 * The identifier of the tuple holding these columns, or RELIC_ID_NONE when
 * the relation does not hold it.
 */
static relic_tuple_id_t
relation_find(const relic_relation_t *relation, const relic_dom_t *columns,
    uint32_t hash)
{
	relic_idprobe_t probe;
	relic_tuple_id_t id;

	relic_idprobe_open(&probe, &relation->index, hash);
	while (relic_idprobe_next(&probe, &id)) {
		if (tuple_equal(tuple_at(relation, id), columns,
		    relation->arity))
			return (id);
	}

	return (RELIC_ID_NONE);
}

/*
 * How many domain identifiers a relation of this arity needs in order to hold
 * that many tuples, or false when the product is not representable.
 *
 * A 32-bit identifier space and an arity of at most 32 put the product inside
 * a 64-bit size_t but not inside a 32-bit one, so it is checked rather than
 * formed and hoped for.
 */
static bool
columns_holding(size_t tuples, uint16_t arity, size_t *columnsp)
{
	if (arity != 0 && tuples > SIZE_MAX / arity)
		return (false);

	*columnsp = tuples * arity;

	return (true);
}

/*
 * The same, for one more tuple than the relation holds now.  Both the
 * reservation and the projection that precedes it ask this question, and
 * writing the "one more" in one place is what keeps them asking the same one.
 */
static bool
relation_columns_after_insert(const relic_relation_t *relation,
    size_t *columnsp)
{
	return (columns_holding(relation->count + 1, relation->arity,
	    columnsp));
}

size_t
relic_relation_bytes(const relic_relation_t *relation)
{
	return (relation->column_capacity * sizeof(*relation->columns) +
	    relic_idtab_bytes(&relation->index));
}

/*
 * The bytes a relation would hold once it had room for one more tuple.
 *
 * Both growth policies are asked what they would do rather than measured
 * after they have done it, which is what lets the memory limit refuse an
 * allocation instead of reporting one that has already happened.
 *
 * A growth whose size cannot be expressed at all -- a column count or a byte
 * count that would overflow a size_t -- is reported as no growth, which is
 * the convention relic_idtab_bytes_after_reserve() follows for the same
 * condition.  The limit then admits the tuple and the reservation refuses it,
 * and that is the better division of labour: a relation too large to grow has
 * not exceeded a limit anybody configured, and the reservation names the real
 * obstacle where a limit diagnostic would name the wrong one.
 */
static size_t
relation_bytes_after_insert(const relic_relation_t *relation)
{
	size_t column_bytes;
	size_t grown_capacity;
	size_t index_bytes;
	size_t needed_columns;

	index_bytes = relic_idtab_bytes_after_reserve(&relation->index);

	if (!relation_columns_after_insert(relation, &needed_columns))
		return (relic_relation_bytes(relation));

	grown_capacity = relic_grow_array_capacity(relation->column_capacity,
	    needed_columns);
	if (grown_capacity == 0 ||
	    grown_capacity > SIZE_MAX / sizeof(*relation->columns))
		return (relic_relation_bytes(relation));

	column_bytes = grown_capacity * sizeof(*relation->columns);
	if (column_bytes > SIZE_MAX - index_bytes)
		return (relic_relation_bytes(relation));

	return (column_bytes + index_bytes);
}

/*
 * Whether one more tuple anywhere would take the program past the configured
 * number of them.
 *
 * The total counts tuples already held, so a total that has reached the limit
 * cannot admit another and the comparison is `>=` rather than `>`.  Its
 * sibling below has to project a size instead, because bytes grow in steps
 * that a tuple count does not predict.
 */
static bool
exceeds_tuple_limit(const relic_t *ctx)
{
	if (ctx->config.max_tuples == 0)
		return (false);

	return (ctx->relations.tuple_count >= ctx->config.max_tuples);
}

/*
 * Whether one more tuple in this relation would take relation and index
 * storage past the configured maximum.
 *
 * The projection replaces this relation's contribution to the total with what
 * that contribution would become, so the answer covers every relation without
 * revisiting any of them.  The subtraction is safe because the total is by
 * construction the sum of relic_relation_bytes() over every relation the
 * context holds, so this relation's bytes are always a part of it.
 */
static bool
exceeds_memory_limit(const relic_t *ctx, const relic_relation_t *relation)
{
	size_t elsewhere_bytes;
	size_t projected_bytes;

	if (ctx->config.max_memory == 0)
		return (false);

	elsewhere_bytes = ctx->relations.storage_bytes -
	    relic_relation_bytes(relation);
	projected_bytes = relation_bytes_after_insert(relation);

	/* Two sizes that fit in a size_t need not have a sum that does. */
	if (projected_bytes > SIZE_MAX - elsewhere_bytes)
		return (true);

	return (elsewhere_bytes + projected_bytes > ctx->config.max_memory);
}

/*
 * Apply the configured limits to one more tuple in this relation.  Returns 0
 * when they admit it, or -1 having reported which of them refused it.
 *
 * Each is applied to what the relation would hold after the insertion and
 * before anything has been modified, so a relation that refused a tuple is
 * exactly as it was.  Consistency after a refusal is a property of that
 * ordering rather than of an unwind path.
 *
 * The two share one message, because a user who set a limit wants to be told
 * which limit and what they set it to, and the answer has that shape whichever
 * one it was.  A ceiling nobody configured is a different kind of refusal and
 * is reported where the other structural refusals are.
 */
static int
enforce_limits(relic_t *ctx, const relic_relation_t *relation)
{
	const char *limit;
	size_t configured;
	char described[RELIC_PRED_TEXT_MAX];

	if (exceeds_tuple_limit(ctx)) {
		limit = "tuples";
		configured = ctx->config.max_tuples;
	} else if (exceeds_memory_limit(ctx, relation)) {
		limit = "memory";
		configured = ctx->config.max_memory;
	} else {
		return (0);
	}

	relic_diagf(ctx, RELIC_ERROR, NULL,
	    "limit exceeded: %s=%zu while deriving %s", limit, configured,
	    relic_pred_describe(ctx, relation->pred, described,
	    sizeof(described)));

	return (-1);
}

/*
 * Make room for one more tuple: a slot in the index and a tuple's worth of
 * columns.
 *
 * Every step that can fail is here, and none of them changes what the
 * relation holds.  That separation is what an insertion's consistency rests
 * on: storing the columns and placing the index entry both happen afterwards
 * and neither can fail, so there is no state in which a relation holds a
 * tuple its index has never heard of.  Such a tuple would not be a lost one
 * but an invisible one -- membership would miss it, the next insertion of the
 * same columns would append a second copy, and a relation holding a duplicate
 * is no longer a set, with every count, join and fixed-point decision
 * afterwards wrong and nothing having reported an error.
 *
 * Whatever a failed reservation did manage is kept.  Capacity is not wasted,
 * and the caller reconciles the storage total from what the relation actually
 * holds rather than from which step failed.
 */
static int
relation_reserve(relic_t *ctx, relic_relation_t *relation)
{
	relic_dom_t *columns;
	size_t needed_columns;
	char described[RELIC_PRED_TEXT_MAX];

	/*
	 * The two ways a relation can be too large to grow at all, neither of
	 * which any limit describes: it has used up the identifier space, or
	 * its columns would no longer be countable.
	 */
	if (relation->count >= RELIC_TUPLE_MAX ||
	    !relation_columns_after_insert(relation, &needed_columns)) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "relation %s is too large to grow",
		    relic_pred_describe(ctx, relation->pred, described,
		    sizeof(described)));
		return (-1);
	}

	if (relic_idtab_reserve(&relation->index, tuple_rehash,
	    relation) != 0) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	columns = relic_grow_array(relation->columns,
	    &relation->column_capacity, needed_columns,
	    sizeof(*relation->columns));
	if (columns == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	relation->columns = columns;

	return (0);
}

/*
 * Make room for one more tuple and keep the storage total in step.
 *
 * The total is reconciled from what the relation holds before and after, so
 * it stays exact whether the reservation succeeded, failed part way through,
 * or grew nothing at all.  A total maintained by adding at the successful
 * sites and subtracting at the failing ones would be a list of places to
 * remember, and the list would be wrong the first time a step was added.
 */
static int
relation_make_room(relic_t *ctx, relic_relation_t *relation)
{
	size_t bytes_before;
	int status;

	bytes_before = relic_relation_bytes(relation);
	status = relation_reserve(ctx, relation);
	ctx->relations.storage_bytes +=
	    relic_relation_bytes(relation) - bytes_before;

	return (status);
}

/*
 * Store a tuple the relation has room for and index it under its hash.
 *
 * Nothing here can fail, which is what lets the columns and the index entry
 * be published together.  The identifier is the one past the last, which is
 * what makes the store append-only: nothing here can name a tuple that
 * already exists.  The columns are written before the count admits them, so a
 * tuple another operation can reach is one that was written in full.
 */
static void
tuple_append(relic_relation_t *relation, const relic_dom_t *columns,
    uint32_t hash)
{
	relic_dom_t *destination;
	relic_tuple_id_t id;
	uint16_t column;

	id = (relic_tuple_id_t)(relation->count + 1);
	destination = tuple_at(relation, id);

	/*
	 * A loop rather than a block copy, for the reason tuple_equal() gives:
	 * `columns` may be null for a relation of arity zero, and a copy of
	 * zero bytes from a null pointer is not defined.
	 */
	for (column = 0; column < relation->arity; column++)
		destination[column] = columns[column];

	relation->count++;

	relic_idtab_place(&relation->index, hash, id);
}

/*
 * Insert a tuple, reporting through *addedp whether it was not already there.
 * Returns 0, or -1 with a diagnostic already reported.
 *
 * That single bit is what fixed-point detection is built on: an iteration
 * that added no tuple to any relation has reached the fixed point, and it
 * learns that without comparing, counting or sorting anything.
 *
 * Newness is reported through an output rather than as the return value
 * because an insertion can also fail, for memory or for a limit, and those
 * are the opposite of a duplicate rather than a kind of one: a duplicate
 * means the fixed point is closer, a failure means the answer is unknown.
 *
 * `columns` holds the relation's arity of identifiers the caller has already
 * interned, and may be anything for a relation of arity zero, which reads
 * none of them.
 *
 * It may point into this relation's own storage -- at a tuple the relation
 * already holds, say -- even though the reservation below can move that
 * storage.  Those two never meet: a pointer into the storage names a tuple
 * the relation holds, so the lookup finds it and returns before anything is
 * reserved.
 */
int
relic_relation_insert(relic_t *ctx, relic_relation_t *relation,
    const relic_dom_t *columns, bool *addedp)
{
	uint32_t hash;

	*addedp = false;

	hash = tuple_hash(columns, relation->arity);
	if (relation_find(relation, columns, hash) != RELIC_ID_NONE)
		return (0);

	if (enforce_limits(ctx, relation) != 0)
		return (-1);

	if (relation_make_room(ctx, relation) != 0)
		return (-1);

	tuple_append(relation, columns, hash);
	ctx->relations.tuple_count++;
	*addedp = true;

	return (0);
}

/*
 * Whether the relation already holds this tuple.  As with an insertion,
 * `columns` may be anything for a relation of arity zero.
 */
bool
relic_relation_contains(const relic_relation_t *relation,
    const relic_dom_t *columns)
{
	uint32_t hash;

	hash = tuple_hash(columns, relation->arity);

	return (relation_find(relation, columns, hash) != RELIC_ID_NONE);
}

/*
 * The columns of the tuple an identifier names, or NULL for one the relation
 * has not assigned.  The pointer is into the relation's storage and is
 * invalidated by the next insertion, so a caller that keeps a tuple copies
 * it.
 */
const relic_dom_t *
relic_relation_tuple(const relic_relation_t *relation, relic_tuple_id_t id)
{
	if (id == RELIC_ID_NONE || id > relation->count)
		return (NULL);

	return (tuple_at(relation, id));
}

size_t
relic_relation_count(const relic_relation_t *relation)
{
	return (relation->count);
}

/*
 * One past the identifier of the last tuple, which is both where iteration
 * stops and the mark a later delta is measured from.
 */
relic_tuple_id_t
relic_relation_end(const relic_relation_t *relation)
{
	return ((relic_tuple_id_t)(relation->count + 1));
}

/*
 * The tuples added since `first` was taken with relic_relation_end(), which
 * is the delta one semi-naive iteration derived.
 *
 * A mark from a relation that has not grown since yields an empty range.  A
 * mark outside the identifiers the relation has assigned is brought back
 * inside them at both ends: one past the end yields an empty range, and one
 * below the first identifier yields the whole relation rather than a range
 * whose first step would be the tuple that does not exist.  Neither can arise
 * from a mark relic_relation_end() produced, and answering both consistently
 * is cheaper than a range a caller has to validate.
 */
relic_tuple_range_t
relic_relation_since(const relic_relation_t *relation, relic_tuple_id_t first)
{
	relic_tuple_range_t range;

	range.end = relic_relation_end(relation);

	if (first < RELIC_TUPLE_FIRST)
		first = RELIC_TUPLE_FIRST;

	range.first = first < range.end ? first : range.end;

	return (range);
}

relic_tuple_range_t
relic_relation_all(const relic_relation_t *relation)
{
	return (relic_relation_since(relation, RELIC_TUPLE_FIRST));
}

/*
 * Extend the table so that it has a slot for every predicate up to `pred`.
 *
 * Only slots are added, and a new one is null: the relation behind it is
 * created when one is asked for.  A predicate that never holds a tuple
 * therefore costs one pointer, and passing over it here commits nothing.
 */
static int
relations_cover(relic_t *ctx, relic_pred_id_t pred)
{
	relic_reltab_t *table;
	relic_relation_t **entries;
	size_t position;

	table = &ctx->relations;

	entries = relic_grow_array(table->entries, &table->capacity, pred,
	    sizeof(*table->entries));
	if (entries == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	table->entries = entries;

	for (position = table->count; position < pred; position++)
		table->entries[position] = NULL;

	table->count = pred;

	return (0);
}

/*
 * Create the empty relation of a predicate, in an allocation of its own so
 * that it never moves.
 */
static relic_relation_t *
relation_create(relic_t *ctx, relic_pred_id_t pred)
{
	relic_relation_t *relation;

	relation = calloc(1, sizeof(*relation));
	if (relation == NULL) {
		relic_diag_no_memory(ctx);
		return (NULL);
	}

	relic_idtab_init(&relation->index);
	relation->pred = pred;
	relation->arity = relic_pred_of(ctx, pred)->arity;

	return (relation);
}

/*
 * The relation of a predicate, created empty if this is the first time one
 * has been asked for.  Returns NULL with a diagnostic already reported when
 * it cannot be created, or when the identifier names no predicate, which is
 * a mistake in the caller rather than in the program being run.
 *
 * The relation is owned by the context and lives as long as it does, so the
 * returned pointer stays valid however many relations are created afterwards.
 * A caller may hold as many of them as it likes.
 */
relic_relation_t *
relic_relation_for(relic_t *ctx, relic_pred_id_t pred)
{
	relic_reltab_t *table;
	relic_relation_t *relation;

	if (relic_pred_of(ctx, pred) == NULL) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "no relation for predicate %lu, which this context has "
		    "not interned", (unsigned long)pred);
		return (NULL);
	}

	table = &ctx->relations;

	if (pred > table->count && relations_cover(ctx, pred) != 0)
		return (NULL);

	relation = table->entries[pred - 1];
	if (relation != NULL)
		return (relation);

	relation = relation_create(ctx, pred);
	if (relation == NULL)
		return (NULL);

	table->entries[pred - 1] = relation;

	return (relation);
}

size_t
relic_relation_total_bytes(const relic_t *ctx)
{
	return (ctx->relations.storage_bytes);
}

size_t
relic_relation_total_tuples(const relic_t *ctx)
{
	return (ctx->relations.tuple_count);
}
