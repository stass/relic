/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * Relations: a set of tuples per predicate, and one per query's answer.
 *
 * Tuples are stored end to end in one array of domain identifiers with the
 * arity as the stride, and a hash index over the whole tuple answers
 * membership.  Everything the layer above needs follows from those two:
 * duplicate elimination is a lookup before an append, a tuple identifier is a
 * position in the array, and the tuples added since some earlier moment are a
 * range of identifiers rather than a relation of their own.
 *
 * The store is append-only between clears.  A tuple identifier, once
 * assigned, denotes the same tuple until its relation is emptied by
 * relic_relation_clear(), and that is a property of this interface rather
 * than a rule its callers follow: there is no operation that removes,
 * reorders or renumbers one tuple, and the one that removes any removes all
 * of the relation's.  Semi-naive evaluation rests on it, because a delta is a
 * pair of identifiers and means nothing if the tuples under them can move;
 * an evaluation holds its ranges in its own state and empties relations only
 * before it begins, so no range outlives the span it was taken in.  Future
 * incremental insertion or deletion support has to confront that before it
 * does anything else.
 *
 * A relation is created once, in an allocation of its own, and never moves.
 * The table holds pointers to the predicates' relations rather than the
 * relations, and a query holds a pointer to its answer's, which is what lets
 * a plan or an evaluation resolve a relation once and hold it.  Nothing below
 * asks which of the two a relation is, except to say what it is called.
 */

/*
 * Tuple identifiers are 32-bit and zero names no tuple, so a relation holds
 * at most this many.  Checked rather than assumed: an identifier that wrapped
 * would silently make one tuple into another.
 */
#define	RELIC_TUPLE_MAX		(UINT32_MAX - 1u)

static uint32_t	 column_rehash(uint32_t id, void *ctx);
static bool	 columns_holding(size_t tuples, uint16_t arity,
		    size_t *columnsp);
static int	 count_distinct(relic_t *ctx, const relic_relation_t *relation,
		    uint16_t column, size_t *distinctp);
static int	 enforce_limits(relic_t *ctx, const relic_relation_t *relation);
static bool	 exceeds_tuple_limit(const relic_t *ctx);
static int	 index_build(relic_t *ctx, const relic_relation_t *relation,
		    relic_index_t *index);
static int	 index_make_room(relic_t *ctx, const relic_relation_t *relation,
		    relic_index_t *index, size_t tuples);
static bool	 key_mask_fits(const relic_relation_t *relation,
		    uint32_t key_mask);
static void	 report_limit(relic_t *ctx, const char *limit,
		    size_t configured, const char *activity,
		    const relic_relation_t *relation);
static size_t	 relation_bytes_after_insert(const relic_relation_t *relation);
static bool	 relation_columns_after_insert(
		    const relic_relation_t *relation, size_t *columnsp);
static void	 relation_empty(relic_relation_t *relation);
static relic_tuple_id_t relation_find_hashed(
		    const relic_relation_t *relation, const relic_dom_t *columns,
		    uint32_t hash);
static int	 relation_make_room(relic_t *ctx, relic_relation_t *relation);
static int	 relation_reserve(relic_t *ctx, relic_relation_t *relation);
static int	 relations_cover(relic_t *ctx, relic_pred_id_t pred);
static void	 tuple_append(relic_relation_t *relation,
		    const relic_dom_t *columns, uint32_t hash);
static relic_dom_t *tuple_at(const relic_relation_t *relation,
		    relic_tuple_id_t id);
static bool	 tuple_equal(const relic_dom_t *left, const relic_dom_t *right,
		    uint16_t arity);
static uint32_t	 tuple_hash(const relic_relation_t *relation,
		    const relic_dom_t *columns);
static uint32_t	 tuple_rehash(uint32_t id, void *ctx);

void
relic_reltab_init(relic_reltab_t *table)
{
	bzero(table, sizeof(*table));
}

/*
 * Release a relation's tuples and every index, leaving it empty and in
 * place.  An index asked for again is built over the tuples held by then at
 * the same cost per tuple as keeping it up would have had, so dropping it
 * loses nothing, and it keeps a later program from paying, on every
 * insertion and against the memory limit, for indexes only an earlier one
 * asked for.  The header array is kept for reuse.  The cached statistics go:
 * the relation may be refilled to the tuple count they were taken at, with
 * other tuples.
 */
static void
relation_empty(relic_relation_t *relation)
{
	size_t position;

	relic_idtab_reset(&relation->index);
	free(relation->columns);
	relation->columns = NULL;
	relation->column_capacity = 0;
	relation->count = 0;

	for (position = 0; position < relation->index_count; position++)
		relic_index_empty(&relation->indexes[position]);
	relation->index_count = 0;

	bzero(&relation->stats, sizeof(relation->stats));
}

/*
 * Create an empty relation of `arity` columns, in an allocation of its own so
 * that it never moves.  `pred` is the predicate whose tuples it holds, or
 * RELIC_ID_NONE for the answer of the query whose serial number is `serial`.
 * Returns NULL having reported when memory ran out.
 */
relic_relation_t *
relic_relation_create(relic_t *ctx, relic_pred_id_t pred, size_t serial,
    uint16_t arity)
{
	relic_relation_t *relation;

	relation = calloc(1, sizeof(*relation));
	if (relation == NULL) {
		relic_diag_no_memory(ctx);
		return (NULL);
	}

	relic_idtab_init(&relation->index);
	relation->hash_seed = ctx->hash_seed;
	relation->pred = pred;
	relation->serial = serial;
	relation->arity = arity;

	return (relation);
}

/*
 * Empty a relation and give back what it counted against both totals.  This
 * is the one way a live relation loses tuples -- freeing one releases only a
 * relation already emptied, or one going with its whole context -- so each
 * total stays the sum of what every relation holds, whichever relations are
 * emptied, and in the table or not.
 */
void
relic_relation_clear(relic_t *ctx, relic_relation_t *relation)
{
	ctx->relations.tuple_count -= relation->count;
	ctx->account.store_bytes -= relic_relation_bytes(relation);
	relation_empty(relation);
}

/*
 * Release everything one relation owns, and the relation itself, touching
 * nothing else: in particular not the context's totals, so a query's answer
 * can be freed after its context is gone.  The relation should have been
 * emptied with relic_relation_clear(); one still holding tuples is freed only
 * together with its context, whose totals go with it.
 */
void
relic_relation_free(relic_relation_t *relation)
{
	if (relation == NULL)
		return;

	relation_empty(relation);
	free(relation->indexes);
	free(relation);
}

void
relic_reltab_reset(relic_reltab_t *table)
{
	size_t position;

	for (position = 0; position < table->count; position++)
		relic_relation_free(table->entries[position]);

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
tuple_hash(const relic_relation_t *relation, const relic_dom_t *columns)
{
	return (relic_hash_words(relation->hash_seed, columns,
	    relation->arity));
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

	return (tuple_hash(relation, tuple_at(relation, id)));
}

/*
 * The identifier of the tuple holding these columns, or RELIC_ID_NONE when
 * the relation does not hold it.
 */
static relic_tuple_id_t
relation_find_hashed(const relic_relation_t *relation,
    const relic_dom_t *columns, uint32_t hash)
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

/*
 * The bytes of tuple and index storage a relation holds: its columns, its
 * canonical index and every secondary index.
 */
size_t
relic_relation_bytes(const relic_relation_t *relation)
{
	size_t bytes;
	size_t position;

	bytes = relation->column_capacity * sizeof(*relation->columns) +
	    relic_idtab_bytes(&relation->index);

	for (position = 0; position < relation->index_count; position++)
		bytes += relic_index_bytes(&relation->indexes[position]);

	return (bytes);
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
	size_t bytes;
	size_t canonical_bytes;
	size_t column_bytes;
	size_t grown_capacity;
	size_t needed_columns;
	size_t position;
	size_t secondary_bytes;

	canonical_bytes = relic_idtab_bytes_after_reserve(&relation->index);

	if (!relation_columns_after_insert(relation, &needed_columns))
		return (relic_relation_bytes(relation));

	grown_capacity = relic_grow_array_capacity(relation->column_capacity,
	    needed_columns);
	if (grown_capacity == 0 ||
	    grown_capacity > SIZE_MAX / sizeof(*relation->columns))
		return (relic_relation_bytes(relation));

	column_bytes = grown_capacity * sizeof(*relation->columns);
	if (column_bytes > SIZE_MAX - canonical_bytes)
		return (relic_relation_bytes(relation));
	bytes = column_bytes + canonical_bytes;

	for (position = 0; position < relation->index_count; position++) {
		secondary_bytes = relic_index_bytes_after_reserve(
		    &relation->indexes[position], relation->count + 1);
		if (secondary_bytes > SIZE_MAX - bytes)
			return (relic_relation_bytes(relation));
		bytes += secondary_bytes;
	}

	return (bytes);
}

/*
 * Whether one more tuple anywhere would take the program past the configured
 * number of them.
 *
 * The total counts tuples already held, so a total that has reached the limit
 * cannot admit another and the comparison is `>=` rather than `>`.  The
 * memory limit has to project a size instead, because bytes grow in steps
 * that a tuple count does not predict, and relic_account_exceeds() does.
 */
static bool
exceeds_tuple_limit(const relic_t *ctx)
{
	if (ctx->config.max_tuples == 0)
		return (false);

	return (ctx->relations.tuple_count >= ctx->config.max_tuples);
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
	if (exceeds_tuple_limit(ctx)) {
		report_limit(ctx, "tuples", ctx->config.max_tuples, "deriving",
		    relation);
		return (-1);
	}

	if (relic_account_exceeds(&ctx->account, relic_relation_bytes(relation),
	    relation_bytes_after_insert(relation))) {
		report_limit(ctx, "memory", ctx->account.limit, "deriving",
		    relation);
		return (-1);
	}

	return (0);
}

/*
 * Report a configured limit refusing something, in the form every limit
 * shares: the limit, what it was set to, and what was being done to which
 * relation -- deriving into it, or indexing it.
 */
static void
report_limit(relic_t *ctx, const char *limit, size_t configured,
    const char *activity, const relic_relation_t *relation)
{
	char described[RELIC_PRED_TEXT_MAX];

	relic_diagf(ctx, RELIC_ERROR, NULL,
	    "limit exceeded: %s=%zu while %s %s", limit, configured, activity,
	    relic_relation_describe(ctx, relation, described,
	    sizeof(described)));
}

/*
 * Make room for one more tuple: a slot in the canonical index, a tuple's
 * worth of columns, and room in every secondary index.
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
	size_t position;
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
		    relic_relation_describe(ctx, relation, described,
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

	for (position = 0; position < relation->index_count; position++) {
		if (relic_index_reserve(&relation->indexes[position], relation,
		    relation->count + 1) != 0) {
			relic_diag_no_memory(ctx);
			return (-1);
		}
	}

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
	ctx->account.store_bytes +=
	    relic_relation_bytes(relation) - bytes_before;

	return (status);
}

/*
 * Store a tuple the relation has room for, index it under its hash, and add
 * it to every secondary index.
 *
 * Nothing here can fail, which is what lets the columns and every index entry
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
	size_t position;
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

	for (position = 0; position < relation->index_count; position++)
		relic_index_add(&relation->indexes[position], relation, id);
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

	hash = tuple_hash(relation, columns);
	if (relation_find_hashed(relation, columns, hash) != RELIC_ID_NONE)
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
 * The identifier of the tuple holding these columns, or RELIC_ID_NONE when
 * the relation does not hold it.  A caller that must know not only whether
 * the tuple is present but whether it lies in some range of identifiers --
 * a view of a member being evaluated -- compares the identifier.  As with an
 * insertion, `columns` may be anything for a relation of arity zero.
 */
relic_tuple_id_t
relic_relation_find(const relic_relation_t *relation,
    const relic_dom_t *columns)
{
	return (relation_find_hashed(relation, columns,
	    tuple_hash(relation, columns)));
}

/*
 * Whether the relation already holds this tuple.
 */
bool
relic_relation_contains(const relic_relation_t *relation,
    const relic_dom_t *columns)
{
	return (relic_relation_find(relation, columns) != RELIC_ID_NONE);
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
 * Whether `key_mask` names a nonempty proper subset of this relation's
 * columns, which is what a secondary index can be keyed on.  Every column is
 * the whole tuple, which the full-tuple index already answers.
 */
static bool
key_mask_fits(const relic_relation_t *relation, uint32_t key_mask)
{
	uint32_t columns;

	columns = relation->arity >= 32 ? UINT32_MAX :
	    (UINT32_C(1) << relation->arity) - 1;

	return (key_mask != 0 && key_mask != columns &&
	    (key_mask & ~columns) == 0);
}

/*
 * Make room in an index for `tuples` tuples and one more key, under the
 * memory limit, keeping the storage total in step as relation_make_room()
 * does.
 *
 * `index` need not be one the relation has published: it is measured as a
 * part of the storage total in its own right, which is what lets an index be
 * charged for while it is being built and before anything can reach it.
 */
static int
index_make_room(relic_t *ctx, const relic_relation_t *relation,
    relic_index_t *index, size_t tuples)
{
	size_t bytes_before;
	int status;

	bytes_before = relic_index_bytes(index);

	if (relic_account_exceeds(&ctx->account, bytes_before,
	    relic_index_bytes_after_reserve(index, tuples))) {
		report_limit(ctx, "memory", ctx->account.limit, "indexing",
		    relation);
		return (-1);
	}

	status = relic_index_reserve(index, relation, tuples);
	ctx->account.store_bytes += relic_index_bytes(index) - bytes_before;
	if (status != 0)
		relic_diag_no_memory(ctx);

	return (status);
}

/*
 * Fill an empty, unpublished index with every tuple the relation holds, in
 * identifier order, which is what puts each chain in that order.  Returns 0,
 * or -1 with a diagnostic reported, leaving the index holding whatever it had
 * reached and the storage total counting it.
 *
 * Room is made one tuple at a time rather than for the whole relation at
 * once.  The limit is then applied at each growth exactly as it is when the
 * relation grows by insertion, so an index that fits is admitted however it
 * came to be built.
 */
static int
index_build(relic_t *ctx, const relic_relation_t *relation,
    relic_index_t *index)
{
	relic_tuple_id_t id;

	for (id = RELIC_TUPLE_FIRST; id <= relation->count; id++) {
		if (index_make_room(ctx, relation, index, id) != 0)
			return (-1);

		relic_index_add(index, relation, id);
	}

	return (0);
}

/*
 * The position in relation->indexes of the index keyed on the columns of
 * `key_mask`, building it now if the relation has none.  Returns 0, or -1
 * with a diagnostic reported: memory or the memory limit refused the build,
 * or the mask is not a nonempty proper subset of the relation's columns,
 * which is a mistake in the caller.
 *
 * A build that fails is never published.  It is built outside the relation,
 * so no lookup can reach a partial index and miss the tuples it had not yet
 * reached, and on failure its bytes leave the storage total and its storage
 * is released: a refused build leaves the relation as it was, apart from
 * header capacity that is not accounted.  The header is grown before the
 * build begins so that publishing cannot fail after it succeeds.
 */
int
relic_relation_index(relic_t *ctx, relic_relation_t *relation,
    uint32_t key_mask, uint32_t *positionp)
{
	relic_index_t built;
	relic_index_t *indexes;
	size_t position;
	char described[RELIC_PRED_TEXT_MAX];

	if (!key_mask_fits(relation, key_mask)) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "internal error: an index of %s was asked for on columns "
		    "it cannot be keyed on", relic_relation_describe(ctx,
		    relation, described, sizeof(described)));
		return (-1);
	}

	for (position = 0; position < relation->index_count; position++) {
		if (relation->indexes[position].key_mask == key_mask) {
			*positionp = (uint32_t)position;
			return (0);
		}
	}

	indexes = relic_grow_array(relation->indexes,
	    &relation->index_capacity, relation->index_count + 1,
	    sizeof(*relation->indexes));
	if (indexes == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	relation->indexes = indexes;

	relic_index_init(&built, key_mask);
	if (index_build(ctx, relation, &built) != 0) {
		ctx->account.store_bytes -= relic_index_bytes(&built);
		relic_index_empty(&built);
		return (-1);
	}

	position = relation->index_count;
	relation->indexes[position] = built;
	relation->index_count++;
	*positionp = (uint32_t)position;

	return (0);
}

typedef struct relic_column_rehash relic_column_rehash_t;

/*
 * What the scratch table of count_distinct() needs to recompute the hash of
 * a value it holds: the table holds the identifier of the first tuple seen
 * with each value, and the value is that tuple's entry in `column`.
 */
struct relic_column_rehash {
	const relic_relation_t	*relation;
	uint16_t		 column;
};

static uint32_t
column_rehash(uint32_t id, void *ctx)
{
	const relic_column_rehash_t *rehash;

	rehash = ctx;

	return (relic_hash_word(rehash->relation->hash_seed,
	    tuple_at(rehash->relation, id)[rehash->column]));
}

/*
 * Count the distinct values of one column by keeping, in a scratch
 * identifier table, the first tuple seen with each value.  Returns 0, or -1
 * with a diagnostic reported when memory ran out.
 *
 * The table is released before returning and is not relation storage, so it
 * is not charged against the memory limit; it holds at most one slot per
 * tuple of a relation the limit has already admitted.
 */
static int
count_distinct(relic_t *ctx, const relic_relation_t *relation,
    uint16_t column, size_t *distinctp)
{
	relic_column_rehash_t rehash;
	relic_idtab_t seen;
	relic_idprobe_t probe;
	relic_tuple_id_t id;
	relic_tuple_id_t held;
	relic_dom_t value;
	uint32_t hash;
	size_t distinct;
	bool found;

	rehash.relation = relation;
	rehash.column = column;
	relic_idtab_init(&seen);
	distinct = 0;

	for (id = RELIC_TUPLE_FIRST; id <= relation->count; id++) {
		value = tuple_at(relation, id)[column];
		hash = relic_hash_word(relation->hash_seed, value);

		found = false;
		relic_idprobe_open(&probe, &seen, hash);
		while (!found && relic_idprobe_next(&probe, &held))
			found = tuple_at(relation, held)[column] == value;
		if (found)
			continue;

		if (relic_idtab_reserve(&seen, column_rehash, &rehash) != 0) {
			relic_idtab_reset(&seen);
			relic_diag_no_memory(ctx);
			return (-1);
		}
		relic_idtab_place(&seen, hash, id);
		distinct++;
	}

	relic_idtab_reset(&seen);
	*distinctp = distinct;

	return (0);
}

/*
 * The number of distinct values `column` holds across the relation's tuples,
 * counted the first time it is asked for at the relation's current size and
 * then answered from relation->stats.  Returns 0, or -1 with a diagnostic
 * reported: memory ran out, or the relation has no such column, which is a
 * mistake in the caller.
 */
int
relic_relation_distinct(relic_t *ctx, relic_relation_t *relation,
    uint16_t column, size_t *distinctp)
{
	relic_relation_stats_t *stats;
	size_t distinct;
	char described[RELIC_PRED_TEXT_MAX];

	if (column >= relation->arity) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "internal error: the statistics of %s were asked for a "
		    "column it does not have", relic_relation_describe(ctx,
		    relation, described, sizeof(described)));
		return (-1);
	}

	stats = &relation->stats;
	if (stats->counted_at != relation->count) {
		stats->known = 0;
		stats->counted_at = relation->count;
	}

	if ((stats->known & (UINT32_C(1) << column)) == 0) {
		if (count_distinct(ctx, relation, column, &distinct) != 0)
			return (-1);

		stats->distinct[column] = (uint32_t)distinct;
		stats->known |= UINT32_C(1) << column;
	}

	*distinctp = stats->distinct[column];

	return (0);
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

	relation = relic_relation_create(ctx, pred, 0,
	    relic_pred_of(ctx, pred)->arity);
	if (relation == NULL)
		return (NULL);

	table->entries[pred - 1] = relation;

	return (relation);
}

/*
 * Empty the relation of every predicate, releasing its storage.  The
 * relations stay where they are, so a pointer to one still names its
 * predicate's relation, which now holds nothing.
 *
 * Identifiers restart from the first afterwards, so a range taken before the
 * discard names nothing, or other tuples, after it, and every index is gone,
 * so an index position taken before it names nothing either.  The caller must
 * hold neither across it, which the evaluator satisfies by discarding only
 * before an evaluation begins: every range and every plan lives in an
 * evaluation's state and dies with it.
 *
 * Each relation is cleared, so each total loses exactly what the relations
 * of the table held and keeps what any relation outside it holds.  A query's
 * answer is outside it, and is empty whenever anything but its own answer
 * can run, because the context is busy from the moment the answer is filled
 * until it is emptied again.  Both totals are therefore zero afterwards, and
 * that is asserted rather than assigned: a total set to zero would be right
 * only while that argument held, and one that is asserted fails a test the
 * day it stops holding.
 */
void
relic_relation_discard_all(relic_t *ctx)
{
	relic_reltab_t *table;
	size_t position;

	table = &ctx->relations;

	for (position = 0; position < table->count; position++) {
		if (table->entries[position] != NULL)
			relic_relation_clear(ctx, table->entries[position]);
	}

	assert(table->tuple_count == 0 && ctx->account.store_bytes == 0);
}

/*
 * Write what a relation is called into `buffer` and return it: its
 * predicate's name, or `$q` and the serial number of the query whose answer
 * it is.  A name longer than the buffer is shortened as a message shortens
 * one.  No identifier the lexer accepts contains `$`, so an answer's name
 * cannot be mistaken for a predicate a program writes, and it is built here
 * rather than interned anywhere.
 */
const char *
relic_relation_name(const relic_t *ctx, const relic_relation_t *relation,
    char *buffer, size_t size)
{
	const relic_pred_t *pred;
	const char *name;

	if (relation->pred == RELIC_ID_NONE) {
		snprintf(buffer, size, "$q%zu", relation->serial);
		return (buffer);
	}

	name = NULL;
	pred = relic_pred_of(ctx, relation->pred);
	if (pred != NULL)
		name = relic_symbol_bytes(ctx, pred->name, NULL);

	return (relic_diag_name(name != NULL ? name : "?", buffer, size));
}

/*
 * Write "name/arity" into `buffer`, which must hold RELIC_PRED_TEXT_MAX
 * bytes, and return it, for a message about a relation.  A predicate's
 * relation is described exactly as relic_pred_describe() describes the
 * predicate.
 */
const char *
relic_relation_describe(const relic_t *ctx, const relic_relation_t *relation,
    char *buffer, size_t size)
{
	char name[RELIC_NAME_TEXT_MAX];

	snprintf(buffer, size, "%s/%u",
	    relic_relation_name(ctx, relation, name, sizeof(name)),
	    (unsigned int)relation->arity);

	return (buffer);
}

size_t
relic_relation_total_bytes(const relic_t *ctx)
{
	return (ctx->account.store_bytes);
}

size_t
relic_relation_total_tuples(const relic_t *ctx)
{
	return (ctx->relations.tuple_count);
}
