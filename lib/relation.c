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
 * The store's tables hold pointers to the predicates' relations rather than
 * the relations, and a query holds a pointer to its answer's, which is what
 * lets a plan or an evaluation resolve a relation once and hold it.
 *
 * A predicate has two relations at most: its base store, holding the facts
 * the program states for it, and its derived relation, which exists once a
 * rule derives it.  Two functions hand them out, one per role.  The relation
 * a rule writes is the derived relation, and asking for it for a predicate
 * no rule derives is refused, so nothing but a load's commit writes a base
 * store.  The relation an operation reads is the derived relation of a
 * derived predicate, whose facts evaluation seeds it with, and otherwise the
 * base store, which is then the whole relation.  Below the two funnels
 * nothing asks which kind a relation is, except to say what it is called.
 */

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
static void	 prefetch_first_candidate(const relic_relation_t *relation,
		    uint32_t hash);
static void	 report_limit(relic_t *ctx, const char *limit,
		    size_t configured, const char *activity,
		    const char *described);
static bool	 relation_columns_after_reserve(
		    const relic_relation_t *relation, size_t added,
		    size_t *columnsp);
static void	 relation_empty(relic_relation_t *relation);
static relic_tuple_id_t relation_find_hashed(
		    const relic_relation_t *relation,
		    const relic_dom_t *columns, uint32_t hash);
static int	 relation_insert_hashed(relic_t *ctx,
		    relic_relation_t *relation, const relic_dom_t *columns,
		    uint32_t hash, bool *addedp);
static int	 relation_grow(relic_t *ctx, relic_relation_t *relation,
		    size_t added);
static void	 relation_init(relic_relation_t *relation, const relic_t *ctx,
		    relic_pred_id_t pred, size_t serial, uint16_t arity);
static void	 relation_drop_indexes(relic_t *ctx,
		    relic_relation_t *relation);
static int	 reltab_cover(relic_t *ctx, relic_reltab_t *table,
		    relic_pred_id_t pred);
static relic_relation_t *reltab_find(const relic_reltab_t *table,
		    relic_pred_id_t pred);
static relic_relation_t *reltab_relation(relic_t *ctx,
		    relic_reltab_t *table, relic_pred_id_t pred);
static void	 reltab_reset(relic_reltab_t *table);
static size_t	 base_bytes(const relic_t *ctx) RELIC_UNUSED;
static int	 bases_fit(relic_t *ctx, const relic_base_growth_t *growths,
		    size_t count);
static int	 refuse_load(relic_t *ctx, const char *key, size_t limit,
		    relic_pred_id_t pred);
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
relic_store_init(relic_store_t *store)
{
	bzero(store, sizeof(*store));
}

void
relic_store_reset(relic_store_t *store)
{
	reltab_reset(&store->derived);
	reltab_reset(&store->bases);
	bzero(store, sizeof(*store));
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
	relic_free_array(relation->columns);
	relation->columns = NULL;
	relation->column_capacity = 0;
	relation->count = 0;

	for (position = 0; position < relation->index_count; position++)
		relic_index_free(&relation->indexes[position]);
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

	relation_init(relation, ctx, pred, serial, arity);

	return (relation);
}

/*
 * Make `relation` an empty relation of `arity` columns, holding nothing
 * that needs releasing.
 */
static void
relation_init(relic_relation_t *relation, const relic_t *ctx,
    relic_pred_id_t pred, size_t serial, uint16_t arity)
{
	bzero(relation, sizeof(*relation));
	relic_idtab_init(&relation->index, false);
	relation->hash_seed = ctx->hash_seed;
	relation->pred = pred;
	relation->serial = serial;
	relation->arity = arity;
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
	ctx->store.tuple_count -= relation->count;
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
	relic_free_array(relation->indexes);
	free(relation);
}

static void
reltab_reset(relic_reltab_t *table)
{
	size_t position;

	for (position = 0; position < table->count; position++)
		relic_relation_free(table->entries[position]);

	relic_free_array(table->entries);
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
 * The same, for `added` more tuples than the relation holds now, or false
 * when those tuples would pass the identifier space.  Both the reservation
 * and the projection that precedes it ask this question, and writing the
 * addition and its bound in one place is what keeps them asking the same
 * one.  A relation's count never exceeds RELIC_TUPLE_MAX, so the bound's
 * subtraction cannot wrap.
 */
static bool
relation_columns_after_reserve(const relic_relation_t *relation,
    size_t added, size_t *columnsp)
{
	if (added > RELIC_TUPLE_MAX - relation->count)
		return (false);

	return (columns_holding(relation->count + added, relation->arity,
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
 * The bytes a relation would hold once relic_relation_reserve() had made
 * room for `added` more tuples.
 *
 * Both growth policies are asked what they would do rather than measured
 * after they have done it, which is what lets the memory limit refuse an
 * allocation instead of reporting one that has already happened.
 *
 * A growth that cannot happen at all -- past the identifier space, or a
 * column count or a byte count that would overflow a size_t -- is reported
 * as no growth, which is the convention relic_idtab_bytes_after_reserve()
 * follows for the same condition.  The limit then admits the tuples and the
 * reservation refuses them, and that is the better division of labour: a
 * relation too large to grow has not exceeded a limit anybody configured,
 * and the reservation names the real obstacle where a limit diagnostic
 * would name the wrong one.
 */
size_t
relic_relation_bytes_after_reserve(const relic_relation_t *relation,
    size_t added)
{
	size_t bytes;
	size_t canonical_bytes;
	size_t column_bytes;
	size_t grown_capacity;
	size_t needed_columns;
	size_t position;
	size_t secondary_bytes;

	if (!relation_columns_after_reserve(relation, added, &needed_columns))
		return (relic_relation_bytes(relation));

	canonical_bytes = relic_idtab_bytes_after_reserve(&relation->index,
	    added);

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
		    &relation->indexes[position], relation->count + added,
		    added);
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
	if (!relic_limit_set(ctx->config.max_tuples))
		return (false);

	return (ctx->store.tuple_count >= ctx->config.max_tuples);
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
	char described[RELIC_PRED_TEXT_MAX];

	if (exceeds_tuple_limit(ctx)) {
		report_limit(ctx, "tuples", ctx->config.max_tuples, "deriving",
		    relic_relation_describe(ctx, relation, described,
		    sizeof(described)));
		return (-1);
	}

	if (relic_account_exceeds(&ctx->account, relic_relation_bytes(relation),
	    relic_relation_bytes_after_reserve(relation, 1))) {
		report_limit(ctx, "memory", ctx->account.limit, "deriving",
		    relic_relation_describe(ctx, relation, described,
		    sizeof(described)));
		return (-1);
	}

	return (0);
}

/*
 * Report a configured limit refusing something, in the form every limit
 * shares: the limit, what it was set to, and what was being done to which
 * relation, as "name/arity" -- deriving into it, indexing it, or loading
 * facts into it.
 */
static void
report_limit(relic_t *ctx, const char *limit, size_t configured,
    const char *activity, const char *described)
{
	relic_diagf(ctx, RELIC_ERROR, NULL,
	    "limit exceeded: %s=%zu while %s %s", limit, configured, activity,
	    described);
}

/*
 * Make room for `added` more tuples: slots in the canonical index, their
 * columns, and room in every secondary index.
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
relation_grow(relic_t *ctx, relic_relation_t *relation, size_t added)
{
	relic_dom_t *columns;
	size_t needed_columns;
	size_t position;
	char described[RELIC_PRED_TEXT_MAX];

	/*
	 * A relation too large to grow at all is one no limit describes: the
	 * tuples would pass the identifier space, or its columns would no
	 * longer be countable.
	 */
	if (!relation_columns_after_reserve(relation, added, &needed_columns)) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "relation %s is too large to grow",
		    relic_relation_describe(ctx, relation, described,
		    sizeof(described)));
		return (-1);
	}

	if (relic_idtab_reserve(&relation->index, added, tuple_rehash,
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
		    relation->count + added, added) != 0) {
			relic_diag_no_memory(ctx);
			return (-1);
		}
	}

	return (0);
}

/*
 * Make room for `added` more tuples and keep the storage total in step.
 *
 * The total is reconciled from what the relation holds before and after, so
 * it stays exact whether the reservation succeeded, failed part way through,
 * or grew nothing at all.  A total maintained by adding at the successful
 * sites and subtracting at the failing ones would be a list of places to
 * remember, and the list would be wrong the first time a step was added.
 */
int
relic_relation_reserve(relic_t *ctx, relic_relation_t *relation,
    size_t added)
{
	size_t bytes_before;
	int status;

	assert(added <= 1 || relation->index_count == 0);

	bytes_before = relic_relation_bytes(relation);
	status = relation_grow(ctx, relation, added);
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
	return (relation_insert_hashed(ctx, relation, columns,
	    tuple_hash(relation, columns), addedp));
}

/*
 * Insert a tuple whose hash, tuple_hash(), the caller has computed: the one
 * insertion both relic_relation_insert() and a batch perform, so the two
 * cannot disagree about what a duplicate is.
 */
static int
relation_insert_hashed(relic_t *ctx, relic_relation_t *relation,
    const relic_dom_t *columns, uint32_t hash, bool *addedp)
{
	*addedp = false;

	if (relation_find_hashed(relation, columns, hash) != RELIC_ID_NONE)
		return (0);

	if (enforce_limits(ctx, relation) != 0)
		return (-1);

	if (relic_relation_reserve(ctx, relation, 1) != 0)
		return (-1);

	tuple_append(relation, columns, hash);
	ctx->store.tuple_count++;
	*addedp = true;

	return (0);
}

/*
 * Insert `count` tuples, packed at the relation's arity, in order, as
 * relic_relation_insert() would one at a time, recording in added[i]
 * whether tuple i was added and in *completedp how many insertions
 * completed.  Returns 0 with every one completed, or -1 with a diagnostic
 * reported for the insertion that failed, the ones before it completed and
 * it and the rest reported not added.  `tuples` is never null, even for a
 * relation of arity zero, whose tuples occupy no words of it.
 *
 * The batch goes in chunks of RELIC_INSERT_BATCH tuples, each in three
 * passes, so that the misses of the duplicate checks overlap rather than
 * follow one another: hash every tuple and prefetch the slot its hash
 * selects; read each such slot, now arriving, and prefetch the tuple it
 * names, which for a duplicate is usually the tuple that confirms it; then
 * insert each in turn by the hash the first pass computed.  An insertion
 * may grow the canonical index, after which the lines prefetched for the
 * old slot array go unused: a prefetch is a hint, and nothing reads what it
 * fetched but the probes, which read through the table as it stands.
 */
int
relic_relation_insert_batch(relic_t *ctx, relic_relation_t *relation,
    const relic_dom_t *tuples, size_t count, bool *added,
    size_t *completedp)
{
	uint32_t hashes[RELIC_INSERT_BATCH];
	const relic_dom_t *chunk;
	size_t chunk_count;
	size_t first;
	size_t i;

	assert(tuples != NULL);

	bzero(added, count * sizeof(*added));
	*completedp = 0;

	for (first = 0; first < count; first += chunk_count) {
		chunk = tuples + first * relation->arity;
		chunk_count = count - first < RELIC_INSERT_BATCH ?
		    count - first : RELIC_INSERT_BATCH;

		for (i = 0; i < chunk_count; i++) {
			hashes[i] = tuple_hash(relation,
			    chunk + i * relation->arity);
			relic_idtab_prefetch(&relation->index, hashes[i]);
		}

		for (i = 0; i < chunk_count; i++)
			prefetch_first_candidate(relation, hashes[i]);

		for (i = 0; i < chunk_count; i++) {
			if (relation_insert_hashed(ctx, relation,
			    chunk + i * relation->arity, hashes[i],
			    &added[first + i]) != 0)
				return (-1);
			(*completedp)++;
		}
	}

	return (0);
}

/*
 * Prefetch the tuple named by the slot `hash` selects, if the slot is
 * occupied.  That tuple is the first a probe for the hash compares, and for
 * a duplicate at the canonical index's load usually the one it finds.  A
 * relation of arity zero has no columns to fetch.
 */
static void
prefetch_first_candidate(const relic_relation_t *relation, uint32_t hash)
{
	relic_tuple_id_t id;

	if (relation->arity == 0)
		return;

	id = relic_idtab_peek(&relation->index, hash);
	if (id != RELIC_ID_NONE)
		RELIC_PREFETCH(tuple_at(relation, id));
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
 * memory limit, keeping the storage total in step as relic_relation_reserve()
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
	char described[RELIC_PRED_TEXT_MAX];

	bytes_before = relic_index_bytes(index);

	if (relic_account_exceeds(&ctx->account, bytes_before,
	    relic_index_bytes_after_reserve(index, tuples, 1))) {
		report_limit(ctx, "memory", ctx->account.limit, "indexing",
		    relic_relation_describe(ctx, relation, described,
		    sizeof(described)));
		return (-1);
	}

	status = relic_index_reserve(index, relation, tuples, 1);
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
		relic_index_free(&built);
		return (-1);
	}

	position = relation->index_count;
	relation->indexes[position] = built;
	relation->index_count++;
	*positionp = (uint32_t)position;
	ctx->store.holds_derived = true;

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
	relic_idtab_init(&seen, true);
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

		if (relic_idtab_reserve(&seen, 1, column_rehash,
		    &rehash) != 0) {
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
 * Extend a table so that it has a slot for every predicate up to `pred`.
 *
 * Only slots are added, and a new one is null: the relation behind it is
 * created when one is asked for.  A predicate that never holds a tuple
 * therefore costs one pointer, and passing over it here commits nothing.
 */
static int
reltab_cover(relic_t *ctx, relic_reltab_t *table, relic_pred_id_t pred)
{
	relic_relation_t **entries;
	size_t position;

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
 * A predicate's relation in a table, or NULL when none has been created.
 */
static relic_relation_t *
reltab_find(const relic_reltab_t *table, relic_pred_id_t pred)
{
	if (pred == RELIC_ID_NONE || pred > table->count)
		return (NULL);

	return (table->entries[pred - 1]);
}

/*
 * A predicate's relation in a table, created empty if this is the first
 * time one has been asked for.  Returns NULL with a diagnostic already
 * reported when it cannot be created, or when the identifier names no
 * predicate, which is a mistake in the caller rather than in the program
 * being run.
 *
 * The relation is owned by the context and lives as long as it does, so the
 * returned pointer stays valid however many relations are created
 * afterwards.  A caller may hold as many of them as it likes.
 */
static relic_relation_t *
reltab_relation(relic_t *ctx, relic_reltab_t *table, relic_pred_id_t pred)
{
	relic_relation_t *relation;

	if (relic_pred_of(ctx, pred) == NULL) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "no relation for predicate %lu, which this context has "
		    "not interned", (unsigned long)pred);
		return (NULL);
	}

	if (pred > table->count && reltab_cover(ctx, table, pred) != 0)
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
 * The relation a rule deriving `pred` writes: its derived relation.  Asking
 * for a predicate no rule derives is an internal error rather than a
 * relation created on the spot, so no rule can write a base store and no
 * mistake can give a predicate a second relation.
 */
relic_relation_t *
relic_relation_written(relic_t *ctx, relic_pred_id_t pred)
{
	char described[RELIC_PRED_TEXT_MAX];

	if (!relic_pred_is_derived(ctx, pred)) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "internal error: a rule writes %s, which no rule derives",
		    relic_pred_describe(ctx, pred, described,
		    sizeof(described)));
		return (NULL);
	}

	ctx->store.holds_derived = true;

	return (reltab_relation(ctx, &ctx->store.derived, pred));
}

/*
 * The relation an operation reading `pred` reads: the derived relation of
 * a derived predicate, and otherwise the base store of the predicate the
 * read names -- its own, or, for a base view, the one it views.  A base
 * store is created empty on first use, so a predicate nothing states or
 * derives reads as empty.
 */
relic_relation_t *
relic_relation_read(relic_t *ctx, relic_pred_id_t pred)
{
	const relic_pred_t *entry;

	entry = relic_pred_of(ctx, pred);
	if (entry != NULL && entry->derived)
		return (reltab_relation(ctx, &ctx->store.derived, pred));

	if (entry != NULL && entry->views != RELIC_ID_NONE)
		pred = entry->views;

	return (reltab_relation(ctx, &ctx->store.bases, pred));
}

/*
 * A predicate's base store, or NULL when nothing has created it.
 */
const relic_relation_t *
relic_base_store(const relic_t *ctx, relic_pred_id_t pred)
{
	return (reltab_find(&ctx->store.bases, pred));
}

/*
 * The number of facts a predicate's base store holds.
 */
size_t
relic_base_count(const relic_t *ctx, relic_pred_id_t pred)
{
	const relic_relation_t *store;

	store = relic_base_store(ctx, pred);

	return (store == NULL ? 0 : store->count);
}

/*
 * The number of facts every base store holds.  Base stores only grow, so
 * the total only grows.
 */
size_t
relic_base_total(const relic_t *ctx)
{
	const relic_reltab_t *table;
	size_t total;
	size_t position;

	table = &ctx->store.bases;
	total = 0;

	for (position = 0; position < table->count; position++) {
		if (table->entries[position] != NULL)
			total += table->entries[position]->count;
	}

	return (total);
}

/*
 * The bytes every base store holds.
 */
static size_t
base_bytes(const relic_t *ctx)
{
	const relic_reltab_t *table;
	size_t bytes;
	size_t position;

	table = &ctx->store.bases;
	bytes = 0;

	for (position = 0; position < table->count; position++) {
		if (table->entries[position] != NULL)
			bytes += relic_relation_bytes(table->entries[position]);
	}

	return (bytes);
}

/*
 * Release a relation's secondary indexes, giving back what they counted
 * against the storage total, and keep its tuples and canonical index.
 */
static void
relation_drop_indexes(relic_t *ctx, relic_relation_t *relation)
{
	size_t position;

	for (position = 0; position < relation->index_count; position++) {
		ctx->account.store_bytes -=
		    relic_index_bytes(&relation->indexes[position]);
		relic_index_free(&relation->indexes[position]);
	}
	relation->index_count = 0;
}

/*
 * Discard everything evaluation derived: empty every derived relation and
 * drop the base stores' secondary indexes, keeping the facts the base
 * stores hold.  The relations stay where they are, so a pointer to one
 * still names the same relation.  A store holding nothing derived has
 * nothing to discard, and the walk is skipped.
 *
 * Identifiers of a derived relation restart from the first afterwards, so a
 * range taken before the discard names nothing, or other tuples, after it,
 * and every secondary index is gone, so an index position taken before it
 * names nothing either.  The caller must hold neither across it, which the
 * evaluator satisfies by discarding only before an evaluation begins: every
 * range and every plan lives in an evaluation's state and dies with it.
 *
 * Each relation is emptied through the totals, so each total loses exactly
 * what it held.  A query's answer is outside the store, and is empty
 * whenever anything but its own answer can run, because the context is busy
 * from the moment the answer is filled until it is emptied again.  Both
 * totals therefore count the base stores alone afterwards, and that is
 * asserted rather than assigned: a total set would be right only while that
 * argument held, and one that is asserted fails a test the day it stops
 * holding.
 */
void
relic_relation_discard_all(relic_t *ctx)
{
	relic_store_t *store;
	size_t position;

	store = &ctx->store;
	if (!store->holds_derived)
		return;

	for (position = 0; position < store->derived.count; position++) {
		if (store->derived.entries[position] != NULL)
			relic_relation_clear(ctx,
			    store->derived.entries[position]);
	}

	for (position = 0; position < store->bases.count; position++) {
		if (store->bases.entries[position] != NULL)
			relation_drop_indexes(ctx,
			    store->bases.entries[position]);
	}

	store->holds_derived = false;

	assert(store->tuple_count == relic_base_total(ctx) &&
	    ctx->account.store_bytes == base_bytes(ctx));
}

/*
 * Check that the base stores can grow as `growths` asks, under both
 * configured limits, before any of them grows.  Returns 0, or -1 having
 * reported the refusal against the predicate of the first growth.
 *
 * A store not yet created is projected as an empty relation of its
 * predicate's arity, which is what it will be when it is created.  Sums that
 * a size_t cannot hold cannot fit any limit either, and are refused as a
 * store too large to grow.
 */
static int
bases_fit(relic_t *ctx, const relic_base_growth_t *growths, size_t count)
{
	const relic_relation_t *store;
	relic_relation_t absent;
	size_t added_total;
	size_t bytes_after;
	size_t bytes_before;
	size_t projected;
	size_t i;
	char described[RELIC_PRED_TEXT_MAX];

	added_total = 0;
	bytes_before = 0;
	bytes_after = 0;

	for (i = 0; i < count; i++) {
		store = relic_base_store(ctx, growths[i].pred);
		if (store == NULL) {
			relation_init(&absent, ctx, growths[i].pred, 0,
			    relic_pred_of(ctx, growths[i].pred)->arity);
			store = &absent;
		}

		projected = relic_relation_bytes_after_reserve(store,
		    growths[i].added);
		if (growths[i].added > RELIC_TUPLE_MAX - store->count ||
		    growths[i].added > SIZE_MAX - added_total ||
		    projected > SIZE_MAX - bytes_after) {
			relic_diagf(ctx, RELIC_ERROR, NULL,
			    "relation %s is too large to grow",
			    relic_pred_describe(ctx, growths[i].pred,
			    described, sizeof(described)));
			return (-1);
		}

		added_total += growths[i].added;
		bytes_before += relic_relation_bytes(store);
		bytes_after += projected;
	}

	if (relic_limit_set(ctx->config.max_tuples) &&
	    (ctx->store.tuple_count > ctx->config.max_tuples ||
	    added_total > ctx->config.max_tuples - ctx->store.tuple_count))
		return (refuse_load(ctx, "tuples", ctx->config.max_tuples,
		    growths[0].pred));

	if (relic_account_exceeds(&ctx->account, bytes_before, bytes_after))
		return (refuse_load(ctx, "memory", ctx->account.limit,
		    growths[0].pred));

	return (0);
}

/*
 * Report that the limit `key`, of `limit`, refuses a load whose first
 * growing store is `pred`'s, and return -1.
 */
static int
refuse_load(relic_t *ctx, const char *key, size_t limit, relic_pred_id_t pred)
{
	char described[RELIC_PRED_TEXT_MAX];

	report_limit(ctx, key, limit, "loading",
	    relic_pred_describe(ctx, pred, described, sizeof(described)));

	return (-1);
}

/*
 * Make room in the base stores for the facts a load's commit is about to
 * append: `added` more tuples for each predicate of `growths`, which holds
 * at least one growth, each of at least one tuple.  Returns 0, or -1 having
 * reported the refusal.
 *
 * Both limits are checked once, over every store together, before any
 * store is created or grown, so a refusal by either leaves every store as it
 * was.  Only a failed allocation among the growths that follow can leave a
 * store with more capacity than it holds, accounted, as a relation that
 * failed to grow keeps what it obtained.  A base store has no secondary
 * index when a commit reserves, since the commit discards what was derived
 * before reserving.
 */
int
relic_bases_reserve(relic_t *ctx, const relic_base_growth_t *growths,
    size_t count)
{
	relic_relation_t *store;
	size_t i;

	assert(count > 0);

	if (bases_fit(ctx, growths, count) != 0)
		return (-1);

	for (i = 0; i < count; i++) {
		store = reltab_relation(ctx, &ctx->store.bases,
		    growths[i].pred);
		if (store == NULL)
			return (-1);

		if (relic_relation_reserve(ctx, store, growths[i].added) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Append a fact the base store `pred` lacks, into room relic_bases_reserve()
 * made: store it and place it in the canonical index, looking nothing up.
 * Nothing here can fail.
 */
void
relic_base_append(relic_t *ctx, relic_pred_id_t pred,
    const relic_dom_t *columns)
{
	relic_relation_t *store;

	store = reltab_find(&ctx->store.bases, pred);
	assert(store != NULL && store->index_count == 0);
	assert(relic_relation_bytes_after_reserve(store, 1) ==
	    relic_relation_bytes(store));
	assert(relation_find_hashed(store, columns,
	    tuple_hash(store, columns)) == RELIC_ID_NONE);

	tuple_append(store, columns, tuple_hash(store, columns));
	ctx->store.tuple_count++;
}

/*
 * Write what a relation is called into `buffer` and return it: its
 * predicate's name, or `$q` and the serial number of the query whose answer
 * it is.  The base store of a derived predicate p is read only through its
 * base view, and is called by the view's name, p$base, since p names the
 * derived relation.  A name longer than the buffer is shortened as a message
 * shortens one.  No identifier the lexer accepts contains `$`, so neither
 * name can be mistaken for a predicate a program writes, and both are built
 * here rather than looked up.
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
	if (name == NULL)
		name = "?";

	if (pred == NULL || !pred->derived ||
	    relation != relic_base_store(ctx, relation->pred))
		return (relic_diag_name(name, buffer, size));

	if ((size_t)snprintf(buffer, size, "%s$base", name) >= size)
		relic_diag_mark_cut(buffer, size);

	return (buffer);
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
	return (ctx->store.tuple_count);
}

/*
 * Two limits bound what relations hold: the tuple count, which
 * enforce_limits() and the load path apply, and the memory account, which
 * they, the building of an index and every charged arena consult.  Each is
 * asked whether it is set by the relic_limit_set() its checks ask first.
 */
bool
relic_limits_bound_relations(const relic_t *ctx)
{
	return (relic_limit_set(ctx->config.max_tuples) ||
	    relic_limit_set(ctx->account.limit));
}
