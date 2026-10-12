/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
 * an evaluation holds its ranges in its own state and discards relations only
 * before it begins, so no range outlives the span it was taken in.  Future
 * incremental insertion or deletion support has to confront that before it
 * does anything else.
 *
 * A relation is created once, in an allocation of its own, and never moves.
 * The store's tables hold pointers to the predicates' relations rather than
 * the relations, and a query holds a pointer to its answer's, which is what
 * lets a plan or an evaluation resolve a relation once and hold it.  A base
 * store lives as long as its context.  A derived relation lives until the
 * next discard frees it, which no plan or evaluation outlives.
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
static const relic_idtab_t *canonical_stripe(
		    const relic_relation_t *relation, uint64_t hash);
static void	 classify_keys(relic_relation_t *relation,
		    const relic_dom_t *columns);
static size_t	 columns_bytes_after(const relic_relation_t *relation,
		    size_t added);
static size_t	 append_grown_bytes(const relic_append_t *append);
static size_t	 append_layout(const relic_relation_t *relation,
		    relic_append_t *append, void *block);
static int	 append_grow(relic_relation_t *relation,
		    relic_append_t *append);
static int	 enforce_limits(relic_t *ctx, const relic_relation_t *relation,
		    uint64_t hash);
static bool	 exceeds_tuple_limit(const relic_t *ctx);
static int	 insert_hashed_chunk(relic_t *ctx, relic_relation_t *relation,
		    const relic_dom_t *chunk, const uint64_t *hashes,
		    size_t count, bool *added, size_t *completedp);
static int	 insertion_grow(relic_t *ctx, relic_relation_t *relation,
		    uint64_t hash);
static size_t	 insertion_parts_bytes(const relic_relation_t *relation,
		    uint64_t hash);
static size_t	 insertion_parts_bytes_after(
		    const relic_relation_t *relation, uint64_t hash);
static int	 insertion_reserve(relic_t *ctx, relic_relation_t *relation,
		    uint64_t hash);
static int	 index_build(relic_t *ctx, const relic_relation_t *relation,
		    relic_index_t *index);
static int	 index_make_room(relic_t *ctx, const relic_relation_t *relation,
		    relic_index_t *index);
static bool	 is_called_by_predicate(const relic_t *ctx,
		    const relic_relation_t *relation);
static bool	 key_mask_fits(const relic_relation_t *relation,
		    uint32_t key_mask);
static void	 place_append_stripe(relic_relation_t *relation,
		    relic_append_t *append, size_t s);
static void	 prefetch_first_candidate(const relic_relation_t *relation,
		    uint64_t hash);
static bool	 relation_columns_after_reserve(
		    const relic_relation_t *relation, size_t added,
		    size_t *columnsp);
static void	 release_grown_tables(relic_t *ctx, relic_idtab_t *grown);
static void	 relation_empty(relic_relation_t *relation);
static relic_tuple_id_t relation_find_hashed(
		    const relic_relation_t *relation,
		    const relic_dom_t *columns, uint64_t hash);
static int	 relation_insert_hashed(relic_t *ctx,
		    relic_relation_t *relation, const relic_dom_t *columns,
		    uint64_t hash, bool *addedp);
static int	 relation_grow(relic_t *ctx, relic_relation_t *relation,
		    const relic_tuple_growth_t *growth);
static bool	 relation_too_large(relic_t *ctx,
		    const relic_relation_t *relation, size_t added,
		    size_t *columnsp);
static void	 relation_init(relic_relation_t *relation, const relic_t *ctx,
		    relic_pred_id_t pred, size_t serial, uint16_t arity);
static void	 relation_drop_indexes(relic_t *ctx,
		    relic_relation_t *relation);
static relic_relation_t *derived_relation(relic_t *ctx, relic_pred_id_t pred);
static int	 reltab_cover(relic_t *ctx, relic_reltab_t *table,
		    relic_pred_id_t pred);
static relic_relation_t *reltab_find(const relic_reltab_t *table,
		    relic_pred_id_t pred);
static relic_relation_t *reltab_relation(relic_t *ctx,
		    relic_reltab_t *table, relic_pred_id_t pred);
static void	 reltab_reset(relic_reltab_t *table);
static void	 adopt_builder(relic_t *ctx, relic_relation_t *store,
		    relic_builder_t *builder);
static void	 append_builder(relic_relation_t *store,
		    const relic_base_action_t *action);
static size_t	 base_bytes(const relic_t *ctx) RELIC_UNUSED;
static int	 bases_fit(relic_t *ctx, const relic_base_action_t *actions,
		    size_t count);
static uint32_t	 builder_rehash(uint32_t id, void *ctx);
static relic_tuple_id_t payload_append(relic_payload_t *payload,
		    uint16_t arity, const relic_dom_t *columns, uint64_t hash);
static size_t	 payload_bytes(const relic_payload_t *payload);
static size_t	 payload_bytes_untagged(const relic_payload_t *payload);
static relic_tuple_id_t payload_find(const relic_payload_t *payload,
		    uint16_t arity, const relic_dom_t *columns, uint64_t hash);
static void	 payload_init(relic_payload_t *payload, bool tagged);
static void	 payload_release(relic_payload_t *payload);
static int	 payload_reserve_one(relic_payload_t *payload, uint64_t hash,
		    size_t needed_columns, relic_rehash_fn rehash,
		    void *rehash_ctx);
static relic_dom_t *payload_tuple(const relic_payload_t *payload,
		    uint16_t arity, relic_tuple_id_t id);
static void	 tuple_append(relic_relation_t *relation,
		    const relic_dom_t *columns, uint64_t hash);
static relic_dom_t *tuple_at(const relic_relation_t *relation,
		    relic_tuple_id_t id);
static uint64_t	 tuple_hash(const relic_relation_t *relation,
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
 * Make `payload` empty, holding nothing that needs releasing, with its
 * canonical stripes tagged or not as `tagged` says.
 */
static void
payload_init(relic_payload_t *payload, bool tagged)
{
	size_t s;

	bzero(payload, sizeof(*payload));
	for (s = 0; s < RELIC_STRIPES; s++)
		relic_idtab_init(&payload->index[s], tagged);
}

/*
 * Release the tuples and canonical index `payload` holds, leaving it empty.
 */
static void
payload_release(relic_payload_t *payload)
{
	size_t s;

	for (s = 0; s < RELIC_STRIPES; s++)
		relic_idtab_reset(&payload->index[s]);
	free(payload->columns);
	payload->columns = NULL;
	payload->column_capacity = 0;
	payload->count = 0;
}

/*
 * The bytes of tuple and index storage a payload holds: its columns and
 * every stripe of its canonical index.
 */
static size_t
payload_bytes(const relic_payload_t *payload)
{
	size_t bytes;
	size_t s;

	bytes = payload->column_capacity * sizeof(*payload->columns);
	for (s = 0; s < RELIC_STRIPES; s++)
		bytes += relic_idtab_bytes(&payload->index[s]);

	return (bytes);
}

/* What payload_bytes() gives once the canonical stripes drop their tags. */
static size_t
payload_bytes_untagged(const relic_payload_t *payload)
{
	size_t bytes;
	size_t s;

	bytes = payload->column_capacity * sizeof(*payload->columns);
	for (s = 0; s < RELIC_STRIPES; s++)
		bytes += relic_idtab_bytes_untagged(&payload->index[s]);

	return (bytes);
}

/*
 * Where the columns of the tuple with identifier `id` begin, in a payload
 * of tuples of `arity` columns.  Identifier zero names no tuple, so the
 * array is indexed one lower; this is the only place that translation is
 * written.  The caller must have established that the identifier is within
 * what the payload has room for.
 *
 * The result is writable even though the payload is not, because tuple
 * storage is what a payload holds rather than what it is, and
 * payload_append() writes a new tuple through this.  Every other caller
 * only reads.
 */
static relic_dom_t *
payload_tuple(const relic_payload_t *payload, uint16_t arity,
    relic_tuple_id_t id)
{
	return (payload->columns + ((size_t)id - 1) * arity);
}

/*
 * The identifier of the tuple of `arity` columns holding `columns`, whose
 * wide hash is `hash`, or RELIC_ID_NONE when the payload does not hold it.
 */
static relic_tuple_id_t
payload_find(const relic_payload_t *payload, uint16_t arity,
    const relic_dom_t *columns, uint64_t hash)
{
	relic_idprobe_t probe;
	relic_tuple_id_t id;

	relic_idprobe_open(&probe, &payload->index[relic_hash_stripe(hash)],
	    relic_hash_low(hash));
	while (relic_idprobe_next(&probe, &id)) {
		if (relic_tuple_equal(payload_tuple(payload, arity, id),
		    columns, arity))
			return (id);
	}

	return (RELIC_ID_NONE);
}

/*
 * Make room in `payload` for one more tuple, of wide hash `hash`: a slot in
 * its canonical stripe, which `rehash` and `rehash_ctx` let grow, and
 * `needed_columns` columns in all.  Every one-tuple growth, an insertion's
 * or a builder's, goes through this, which is what lets an empty store
 * adopt a builder's payload with the capacities its own insertions would
 * have reached.
 * Returns 0, or -1 when memory ran out, having reported nothing; what was
 * grown before the failure stays, and holds no new tuple.
 */
static int
payload_reserve_one(relic_payload_t *payload, uint64_t hash,
    size_t needed_columns, relic_rehash_fn rehash, void *rehash_ctx)
{
	relic_dom_t *columns;

	if (relic_idtab_reserve(&payload->index[relic_hash_stripe(hash)], 1,
	    rehash, rehash_ctx) != 0)
		return (-1);

	columns = relic_grow_array(payload->columns, &payload->column_capacity,
	    needed_columns, sizeof(*payload->columns));
	if (columns == NULL)
		return (-1);
	payload->columns = columns;

	return (0);
}

/*
 * Store a tuple the payload has room for, as the one after its last, and
 * place it in its canonical stripe under its wide hash.  Nothing here can
 * fail.  The columns are written before the count admits them, so a tuple
 * another operation can reach is one that was written in full.  Returns the
 * tuple's identifier.
 */
static relic_tuple_id_t
payload_append(relic_payload_t *payload, uint16_t arity,
    const relic_dom_t *columns, uint64_t hash)
{
	relic_dom_t *destination;
	relic_tuple_id_t id;
	uint16_t column;

	id = (relic_tuple_id_t)(payload->count + 1);
	destination = payload_tuple(payload, arity, id);

	/*
	 * A loop rather than a block copy, for the reason relic_tuple_equal()
	 * gives: `columns` may be null for a tuple of arity zero, and a copy
	 * of zero bytes from a null pointer is not defined.
	 */
	for (column = 0; column < arity; column++)
		destination[column] = columns[column];

	payload->count++;

	relic_idtab_place(&payload->index[relic_hash_stripe(hash)],
	    relic_hash_low(hash), id);

	return (id);
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

	payload_release(&relation->payload);

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
	payload_init(&relation->payload, false);
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
	ctx->store.tuple_count -= relation->payload.count;
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

static void
reltab_reset(relic_reltab_t *table)
{
	size_t position;

	for (position = 0; position < table->count; position++)
		relic_relation_free(table->entries[position]);

	free(table->entries);
	bzero(table, sizeof(*table));
}

static relic_dom_t *
tuple_at(const relic_relation_t *relation, relic_tuple_id_t id)
{
	return (payload_tuple(&relation->payload, relation->arity, id));
}

/*
 * The wide hash of a whole tuple, which is what the canonical index is keyed
 * on, and what a load's staging computes once per fact it stages.  A tuple of
 * no columns has a defined hash, which is what lets a relation of arity zero
 * use this path like any other.
 */
uint64_t
relic_tuple_hash(uint64_t seed, const relic_dom_t *columns, uint16_t arity)
{
	return (relic_hash_words_wide(seed, columns, arity));
}

static uint64_t
tuple_hash(const relic_relation_t *relation, const relic_dom_t *columns)
{
	return (relic_tuple_hash(relation->hash_seed, columns,
	    relation->arity));
}

/*
 * The hash of a tuple already stored, for its stripe of the canonical index
 * to use when it grows: the low half of its wide hash, which is the keyed
 * hash of its columns, computed without the unkeyed half that chose the
 * stripe it is already in.
 */
static uint32_t
tuple_rehash(uint32_t id, void *ctx)
{
	const relic_relation_t *relation;

	relation = ctx;

	return (relic_hash_words(relation->hash_seed, tuple_at(relation, id),
	    relation->arity));
}

/*
 * The stripe of the canonical index a tuple of wide hash `hash` lives in.
 */
static const relic_idtab_t *
canonical_stripe(const relic_relation_t *relation, uint64_t hash)
{
	return (&relation->payload.index[relic_hash_stripe(hash)]);
}

/*
 * The identifier of the tuple holding these columns, or RELIC_ID_NONE when
 * the relation does not hold it.
 */
static relic_tuple_id_t
relation_find_hashed(const relic_relation_t *relation,
    const relic_dom_t *columns, uint64_t hash)
{
	return (payload_find(&relation->payload, relation->arity, columns,
	    hash));
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
	if (added > RELIC_TUPLE_MAX - relation->payload.count)
		return (false);

	return (columns_holding(relation->payload.count + added,
	    relation->arity, columnsp));
}

/*
 * The bytes of tuple and index storage a relation holds: its columns, every
 * stripe of its canonical index and every secondary index.
 */
size_t
relic_relation_bytes(const relic_relation_t *relation)
{
	size_t bytes;
	size_t position;

	bytes = payload_bytes(&relation->payload);
	for (position = 0; position < relation->index_count; position++)
		bytes += relic_index_bytes(&relation->indexes[position]);

	return (bytes);
}

/*
 * The bytes a relation without secondary indexes would hold once
 * relic_relation_reserve() had made room for `growth`: its columns grown
 * once for every tuple, and each stripe of its canonical index for the
 * tuples that fall in it.
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
relic_relation_bytes_after_growth(const relic_relation_t *relation,
    const relic_tuple_growth_t *growth)
{
	size_t bytes;
	size_t s;

	assert(relation->index_count == 0);

	bytes = columns_bytes_after(relation,
	    relic_tuple_growth_total(growth));
	if (bytes == SIZE_MAX)
		return (relic_relation_bytes(relation));

	for (s = 0; s < RELIC_STRIPES; s++)
		bytes = relic_saturating_sum(bytes,
		    relic_idtab_bytes_after_reserve(&relation->payload.index[s],
		    growth->by_stripe[s]));
	if (bytes == SIZE_MAX)
		return (relic_relation_bytes(relation));

	return (bytes);
}

/*
 * The tuples a growth adds in all: the sum of those it adds to each stripe,
 * or SIZE_MAX when that sum is more than a size_t holds, which no relation
 * can take.
 */
size_t
relic_tuple_growth_total(const relic_tuple_growth_t *growth)
{
	size_t total;
	size_t s;

	total = 0;
	for (s = 0; s < RELIC_STRIPES; s++)
		total = relic_saturating_sum(total, growth->by_stripe[s]);

	return (total);
}

/*
 * The bytes a relation's columns would hold once grown for `added` more
 * tuples, or SIZE_MAX when they could not grow that far: past the
 * identifier space, or to a size a size_t does not hold.
 */
static size_t
columns_bytes_after(const relic_relation_t *relation, size_t added)
{
	size_t grown_capacity;
	size_t needed_columns;

	if (!relation_columns_after_reserve(relation, added, &needed_columns))
		return (SIZE_MAX);

	grown_capacity = relic_grow_array_capacity(
	    relation->payload.column_capacity, needed_columns);
	if (grown_capacity == 0)
		return (SIZE_MAX);

	return (relic_saturating_product(grown_capacity,
	    sizeof(*relation->payload.columns)));
}

/*
 * The bytes now held by the parts of a relation that inserting a tuple of
 * wide hash `hash` can grow: its columns, the canonical stripe the hash
 * chooses, and in each secondary index the parts its pending insertion can
 * grow, which classify_keys() has classified.  An insertion measures these,
 * before and after it reserves, rather than the whole relation, which would
 * cost a look at every stripe of every index.
 */
static size_t
insertion_parts_bytes(const relic_relation_t *relation, uint64_t hash)
{
	size_t bytes;
	size_t position;

	bytes = relation->payload.column_capacity *
	    sizeof(*relation->payload.columns) +
	    relic_idtab_bytes(canonical_stripe(relation, hash));

	for (position = 0; position < relation->index_count; position++)
		bytes += relic_index_pending_bytes(
		    &relation->indexes[position]);

	return (bytes);
}

/*
 * The same parts' bytes once insertion_grow() had made room for the tuple.
 * A growth that could not happen at all is reported as no growth, as
 * relic_relation_bytes_after_growth() reports one.
 */
static size_t
insertion_parts_bytes_after(const relic_relation_t *relation, uint64_t hash)
{
	size_t columns_after;
	size_t stripe_after;
	size_t bytes;
	size_t position;

	columns_after = columns_bytes_after(relation, 1);
	if (columns_after == SIZE_MAX)
		return (insertion_parts_bytes(relation, hash));

	stripe_after = relic_idtab_bytes_after_reserve(
	    canonical_stripe(relation, hash), 1);
	bytes = relic_saturating_sum(columns_after, stripe_after);

	for (position = 0; position < relation->index_count; position++)
		bytes = relic_saturating_sum(bytes,
		    relic_index_pending_bytes_after(
		    &relation->indexes[position]));
	if (bytes == SIZE_MAX)
		return (insertion_parts_bytes(relation, hash));

	return (bytes);
}

/*
 * Give an append its arrays, for the `count` tuples its caller set, in one
 * block charged to the round before it is allocated, since only a round's
 * merge makes an append: for each tuple its columns, its hash and its
 * stripe, and for each secondary index of the relation its key's hash,
 * stripe and identifier, and for the canonical index and each secondary
 * one the list of the tuples by stripe.  The records of the secondary
 * indexes are sized by the program and are not charged.  Returns 0, or -1
 * when the limit or the allocator refused; what was obtained is kept, for
 * relic_relation_append_close() to release.
 */
int
relic_relation_append_open(relic_t *ctx, const relic_relation_t *relation,
    relic_append_t *append)
{
	size_t carved;
	size_t bytes;

	if (append->count == 0)
		return (0);

	if (relation->index_count > 0) {
		append->keys = calloc(relation->index_count,
		    sizeof(*append->keys));
		if (append->keys == NULL)
			return (-1);
		append->key_count = relation->index_count;
	}

	bytes = append_layout(relation, append, NULL);
	if (bytes == SIZE_MAX ||
	    !relic_account_charge_round(&ctx->account, bytes))
		return (-1);
	append->block = malloc(bytes);
	if (append->block == NULL) {
		relic_account_release_round(&ctx->account, bytes);
		return (-1);
	}
	append->charged = bytes;

	carved = append_layout(relation, append, append->block);
	assert(carved == bytes);
	(void)carved;

	return (0);
}

/*
 * Lay an append's arrays out from `block`, or only measure them when it is
 * NULL (relic_block_piece()), and return their bytes, SIZE_MAX when they
 * are more than a size counts.  The arrays of the secondary indexes go in
 * the records `append->keys` already holds.
 */
static size_t
append_layout(const relic_relation_t *relation, relic_append_t *append,
    void *block)
{
	relic_append_keys_t *keys;
	size_t count;
	size_t bytes;
	size_t index;
	char *cursor;

	cursor = block;
	bytes = 0;
	count = append->count;

	append->columns = relic_block_piece(&cursor, &bytes,
	    relic_saturating_product(count, relation->arity),
	    sizeof(*append->columns));
	append->hashes = relic_block_piece(&cursor, &bytes, count,
	    sizeof(*append->hashes));
	append->stripes = relic_block_piece(&cursor, &bytes, count,
	    sizeof(*append->stripes));
	append->stripe_list = relic_block_piece(&cursor, &bytes, count,
	    sizeof(*append->stripe_list));

	for (index = 0; index < append->key_count; index++) {
		keys = &append->keys[index];
		keys->hashes = relic_block_piece(&cursor, &bytes, count,
		    sizeof(*keys->hashes));
		keys->stripes = relic_block_piece(&cursor, &bytes, count,
		    sizeof(*keys->stripes));
		keys->key_ids = relic_block_piece(&cursor, &bytes, count,
		    sizeof(*keys->key_ids));
		keys->stripe_list = relic_block_piece(&cursor, &bytes, count,
		    sizeof(*keys->stripe_list));
	}

	return (bytes);
}

/*
 * Release everything an append holds, whether it was applied or not: its
 * arrays and their charge to the round, and every table its reservation
 * grew and no part filled, which the storage total counted from the
 * reservation.  Leaves an append of all zeroes.
 */
void
relic_relation_append_close(relic_t *ctx, relic_append_t *append)
{
	size_t index;

	for (index = 0; index < append->key_count; index++)
		release_grown_tables(ctx, append->keys[index].grown);
	free(append->keys);

	free(append->block);
	release_grown_tables(ctx, append->grown);
	relic_account_release_round(&ctx->account, append->charged);

	bzero(append, sizeof(*append));
}

/*
 * Free the RELIC_STRIPES tables of `grown` that a reservation grew and no
 * part filled, taking their bytes from the storage total that counted them.
 * A filled table has become its stripe's, and `grown` holds nothing there.
 */
static void
release_grown_tables(relic_t *ctx, relic_idtab_t *grown)
{
	size_t s;

	for (s = 0; s < RELIC_STRIPES; s++) {
		ctx->account.store_bytes -= relic_idtab_bytes(&grown[s]);
		relic_idtab_reset(&grown[s]);
	}
}

/*
 * Record the tuple at `position` of an append to the relation -- its
 * columns, its wide hash and the stripe of the canonical index that
 * chooses -- and, for each secondary index, its key's hash and stripe.
 * Writes only the append's entries for `position`, so tuples of one append
 * may be recorded on several threads at once.
 */
void
relic_relation_stage(const relic_relation_t *relation, relic_append_t *append,
    size_t position, const relic_dom_t *columns, uint64_t hash)
{
	relic_dom_t *staged;
	size_t index;
	uint16_t column;

	staged = &append->columns[position * relation->arity];
	for (column = 0; column < relation->arity; column++)
		staged[column] = columns[column];
	append->hashes[position] = hash;
	append->stripes[position] = (uint8_t)relic_hash_stripe(hash);

	for (index = 0; index < relation->index_count; index++)
		relic_index_stage(&relation->indexes[index], relation,
		    &append->keys[index], position, columns);
}

/*
 * The parts an append's tuples are listed by stripe in, once every tuple is
 * staged: part 0 lists them by their stripe of the canonical index, and
 * part i > 0 by their key's stripe of the secondary index at i - 1.  An
 * empty append has none.  A caller runs every part, in any order and on
 * any threads, and no two parts write the same word.
 */
size_t
relic_relation_list_parts(const relic_append_t *append)
{
	if (append->count == 0)
		return (0);

	return (1 + append->key_count);
}

void
relic_relation_list_append(relic_append_t *append, size_t part)
{
	relic_append_keys_t *keys;

	if (part == 0) {
		relic_list_by_stripe(append->stripes, append->count,
		    append->stripe_list, append->stripe_start);
		return;
	}

	keys = &append->keys[part - 1];
	relic_list_by_stripe(keys->stripes, append->count, keys->stripe_list,
	    keys->stripe_start);
}

/*
 * The parts an append's keys are classified in, a stripe of a secondary
 * index each: part p is stripe p % RELIC_STRIPES of the secondary index at
 * p / RELIC_STRIPES.  An empty append has none.  A caller runs every part,
 * in any order and on any threads, and no two parts write the same word.
 */
size_t
relic_relation_classify_parts(const relic_append_t *append)
{
	return (append->key_count * RELIC_STRIPES);
}

/*
 * How many of an append's tuples classify part `part` reads, which is also
 * as many new keys as it can find.  The tuples are listed by stripe.
 */
size_t
relic_relation_classify_tuples(const relic_append_t *append, size_t part)
{
	const relic_append_keys_t *keys;
	size_t s;

	keys = &append->keys[part / RELIC_STRIPES];
	s = part % RELIC_STRIPES;
	assert(keys->stripe_start[RELIC_STRIPES] == append->count);

	return (keys->stripe_start[s + 1] - keys->stripe_start[s]);
}

/*
 * Classify the keys of an append's tuples in part `part`
 * (relic_index_classify_append()), with `new_keys` made with room for
 * relic_relation_classify_tuples() of them.
 */
void
relic_relation_classify_append(const relic_relation_t *relation,
    relic_append_t *append, size_t part, relic_new_keys_t *new_keys)
{
	size_t index;

	index = part / RELIC_STRIPES;
	relic_index_classify_append(&relation->indexes[index], relation,
	    append, &append->keys[index], part % RELIC_STRIPES, new_keys);
}

/*
 * The bytes the relation would hold once it had taken an append whose keys
 * every secondary index has classified: exactly what inserting its tuples
 * one at a time would reach (relic_grow_array_capacity_stepwise()), since
 * the append's tuples are all new.  SIZE_MAX when the relation could not
 * hold them, or some growth could not happen.
 */
size_t
relic_relation_bytes_after_append(const relic_relation_t *relation,
    const relic_append_t *append)
{
	size_t columns;
	size_t bytes;
	size_t index;
	size_t s;

	if (append->count > RELIC_TUPLE_MAX - relation->payload.count)
		return (SIZE_MAX);
	if (append->count == 0)
		return (relic_relation_bytes(relation));

	columns = relic_grow_array_capacity_stepwise(
	    relation->payload.column_capacity,
	    relation->payload.count * relation->arity, append->count,
	    relation->arity);
	if (columns == 0)
		return (SIZE_MAX);

	bytes = relic_saturating_product(columns,
	    sizeof(*relation->payload.columns));
	for (s = 0; s < RELIC_STRIPES; s++)
		bytes = relic_saturating_sum(bytes,
		    relic_idtab_bytes_after_reserve(&relation->payload.index[s],
		    append->growth.by_stripe[s]));

	for (index = 0; index < relation->index_count; index++)
		bytes = relic_saturating_sum(bytes,
		    relic_index_bytes_after_append(&relation->indexes[index],
		    relation->payload.count, append, &append->keys[index]));

	return (bytes);
}

/*
 * The bytes the relation holds while it takes an append, from the
 * reservation until the append is applied: what it holds once it has taken
 * it, and beside that every table the reservation replaces, which stays
 * allocated until the part that fills its replacement frees it, however
 * many stripes grow at once.  An array's growth holds its old generation
 * only while it is copied, one array at a time, as an insertion's growth
 * does, and neither counts it.  SIZE_MAX when the append could not be
 * taken.
 */
size_t
relic_relation_bytes_during_append(const relic_relation_t *relation,
    const relic_append_t *append)
{
	size_t bytes;
	size_t index;
	size_t s;

	bytes = relic_relation_bytes_after_append(relation, append);
	if (append->count == 0)
		return (bytes);

	for (s = 0; s < RELIC_STRIPES; s++)
		bytes = relic_saturating_sum(bytes,
		    relic_idtab_bytes_replaced_by_reserve(
		    &relation->payload.index[s], append->growth.by_stripe[s]));

	for (index = 0; index < relation->index_count; index++)
		bytes = relic_saturating_sum(bytes,
		    relic_index_bytes_replaced_by_append(
		    &relation->indexes[index], &append->keys[index]));

	return (bytes);
}

/*
 * Make room for an append, as relic_relation_bytes_after_append() projects
 * it: the columns and every array of every secondary index grown, and the
 * growth of each stripe of the canonical index and of every key table
 * prepared in the append, allocated and not yet filled.  The storage total
 * counts what the relation then holds, the prepared tables with it, as
 * relic_relation_bytes_during_append() projects it.  Returns 0, or -1 when
 * something could not grow, reporting nothing: the caller gives the tuples
 * to the ordinary insertion instead, which reports whatever refuses it.
 * Whatever a failed reservation obtained is kept and counted, the arrays in
 * the relation and the prepared tables in the append, which
 * relic_relation_append_close() releases.
 */
int
relic_relation_reserve_append(relic_t *ctx, relic_relation_t *relation,
    relic_append_t *append)
{
	size_t bytes_before;
	int status;

	bytes_before = relic_relation_bytes(relation);
	status = append_grow(relation, append);
	append->reserved = relic_saturating_sum(relic_relation_bytes(relation),
	    append_grown_bytes(append));
	ctx->account.store_bytes += append->reserved - bytes_before;

	return (status);
}

/*
 * The growths of relic_relation_reserve_append(), every one that can fail.
 */
static int
append_grow(relic_relation_t *relation, relic_append_t *append)
{
	relic_dom_t *columns;
	size_t index;
	size_t s;

	if (append->count > RELIC_TUPLE_MAX - relation->payload.count)
		return (-1);
	append->first = (relic_tuple_id_t)(relation->payload.count + 1);
	if (append->count == 0)
		return (0);

	columns = relic_grow_array_stepwise(relation->payload.columns,
	    &relation->payload.column_capacity,
	    relation->payload.count * relation->arity, append->count,
	    relation->arity, sizeof(*relation->payload.columns));
	if (columns == NULL)
		return (-1);
	relation->payload.columns = columns;

	for (s = 0; s < RELIC_STRIPES; s++) {
		if (relic_idtab_prepare_growth(&relation->payload.index[s],
		    append->growth.by_stripe[s], &append->grown[s]) != 0)
			return (-1);
	}

	for (index = 0; index < relation->index_count; index++) {
		if (relic_index_reserve_append(&relation->indexes[index],
		    relation->payload.count, append, &append->keys[index]) != 0)
			return (-1);
	}

	return (0);
}

/*
 * The bytes of the tables an append's reservation grew and no part has yet
 * filled, of its canonical stripes and of every key stripe.
 */
static size_t
append_grown_bytes(const relic_append_t *append)
{
	size_t bytes;
	size_t index;
	size_t s;

	bytes = 0;
	for (s = 0; s < RELIC_STRIPES; s++)
		bytes += relic_idtab_bytes(&append->grown[s]);

	for (index = 0; index < append->key_count; index++) {
		for (s = 0; s < RELIC_STRIPES; s++)
			bytes += relic_idtab_bytes(
			    &append->keys[index].grown[s]);
	}

	return (bytes);
}

/*
 * Store an append's columns after the relation's last tuple and count its
 * tuples in, once it has room, before its tuples are placed and linked.
 * Nothing reads the relation while a merge runs, so no lookup sees tuples
 * that the indexes do not yet hold.
 */
void
relic_relation_take_append(relic_t *ctx, relic_relation_t *relation,
    const relic_append_t *append)
{
	size_t words;

	assert(append->first == relation->payload.count + 1);

	words = append->count * relation->arity;
	if (words != 0)
		bcopy(append->columns, tuple_at(relation, append->first),
		    words * sizeof(*relation->payload.columns));

	relation->payload.count += append->count;
	ctx->store.tuple_count += append->count;
}

/*
 * The parts an append is applied in once the relation has taken its
 * columns, a stripe each: part s < RELIC_STRIPES places the tuples of
 * stripe s of the canonical index, and each later run of RELIC_STRIPES
 * parts links the tuples of one secondary index, a key stripe each.  An
 * empty append has none.  A caller runs every part, in any order and on any
 * threads: placing reads nothing linking writes, and no two parts write the
 * same word.
 */
size_t
relic_relation_apply_parts(const relic_append_t *append)
{
	if (append->count == 0)
		return (0);

	return ((1 + append->key_count) * RELIC_STRIPES);
}

void
relic_relation_apply_append(relic_relation_t *relation,
    relic_append_t *append, size_t part)
{
	size_t index;

	if (part < RELIC_STRIPES) {
		place_append_stripe(relation, append, part);
		return;
	}

	index = part / RELIC_STRIPES - 1;
	relic_index_link_append(&relation->indexes[index], relation, append,
	    &append->keys[index], part % RELIC_STRIPES);
}

/*
 * Place the tuples of an append that lie in stripe `s` of the canonical
 * index, in identifier order, the stripe first grown into the table the
 * reservation prepared.  The relation holds the tuples.  Nothing here can
 * fail, and nothing here touches another stripe.
 */
static void
place_append_stripe(relic_relation_t *relation, relic_append_t *append,
    size_t s)
{
	relic_idtab_t *stripe;
	uint32_t position;
	size_t i;

	assert(append->stripe_start[RELIC_STRIPES] == append->count);
	stripe = &relation->payload.index[s];
	relic_idtab_complete_growth(stripe, &append->grown[s], tuple_rehash,
	    relation);

	for (i = append->stripe_start[s]; i < append->stripe_start[s + 1];
	    i++) {
		position = append->stripe_list[i];
		relic_idtab_place(stripe, relic_hash_low(
		    append->hashes[position]),
		    (relic_tuple_id_t)(append->first + position));
	}
}

/*
 * Bring the storage total up to date once every part of an append has been
 * applied: the tables the reservation grew have replaced the tables they
 * were grown from, which are freed.
 */
void
relic_relation_finish_append(relic_t *ctx, const relic_relation_t *relation,
    const relic_append_t *append)
{
	assert(append_grown_bytes(append) == 0);

	ctx->account.store_bytes += relic_relation_bytes(relation) -
	    append->reserved;
}

/*
 * How many more tuples the tuple limit admits, or SIZE_MAX when none is
 * set.
 */
size_t
relic_tuples_admissible(const relic_t *ctx)
{
	size_t limit;

	limit = ctx->config.max_tuples;
	if (!relic_limit_set(limit))
		return (SIZE_MAX);
	if (ctx->store.tuple_count >= limit)
		return (0);

	return (limit - ctx->store.tuple_count);
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
 * Apply the configured limits to one more tuple in this relation, of wide
 * hash `hash`, whose keys classify_keys() has classified.  Returns 0 when
 * they admit it, or -1 having reported which of them refused it.
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
enforce_limits(relic_t *ctx, const relic_relation_t *relation, uint64_t hash)
{
	char described[RELIC_PRED_TEXT_MAX];

	if (exceeds_tuple_limit(ctx)) {
		relic_diag_limit(ctx, "tuples", ctx->config.max_tuples,
		    "deriving", relic_relation_describe(ctx, relation, described,
		    sizeof(described)));
		return (-1);
	}

	if (relic_account_exceeds(&ctx->account,
	    insertion_parts_bytes(relation, hash),
	    insertion_parts_bytes_after(relation, hash))) {
		relic_diag_limit(ctx, "memory", ctx->account.limit, "deriving",
		    relic_relation_describe(ctx, relation, described,
		    sizeof(described)));
		return (-1);
	}

	return (0);
}

int
relic_relation_admit_values(relic_t *ctx, const relic_relation_t *relation,
    size_t projected_bytes)
{
	char described[RELIC_PRED_TEXT_MAX];

	if (!relic_account_exceeds(&ctx->account,
	    relic_value_storage_bytes(ctx), projected_bytes))
		return (0);

	relic_diag_limit(ctx, "memory", ctx->account.limit, "deriving",
	    relic_relation_describe(ctx, relation, described,
	    sizeof(described)));

	return (-1);
}

/*
 * Whether `added` more tuples would take a relation past what it can hold at
 * all: past the identifier space, or to columns no longer countable.  Such a
 * relation is one no limit describes, and is reported as too large to grow.
 * Otherwise sets *columnsp to the columns it would then need.
 */
static bool
relation_too_large(relic_t *ctx, const relic_relation_t *relation,
    size_t added, size_t *columnsp)
{
	char described[RELIC_PRED_TEXT_MAX];

	if (relation_columns_after_reserve(relation, added, columnsp))
		return (false);

	relic_diag_error(ctx, RELIC_ERROR_LIMIT, NULL,
	    "the relation of %s is too large to grow",
	    relic_relation_describe(ctx, relation, described,
	    sizeof(described)));

	return (true);
}

/*
 * Make room in a relation without secondary indexes for `growth`: slots in
 * each stripe of the canonical index for the tuples that fall in it, and
 * their columns.  A relation with secondary indexes is refused as the
 * internal error it is: the tuples appended into the room would have none
 * in the indexes.
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
relation_grow(relic_t *ctx, relic_relation_t *relation,
    const relic_tuple_growth_t *growth)
{
	relic_dom_t *columns;
	size_t needed_columns;
	size_t s;
	char described[RELIC_PRED_TEXT_MAX];

	if (relation->index_count != 0) {
		relic_diag_internal(ctx, NULL,
		    "%s was reserved for many tuples while it has secondary "
		    "indexes", relic_relation_identify(ctx, relation, described,
		    sizeof(described)));
		return (-1);
	}

	if (relation_too_large(ctx, relation, relic_tuple_growth_total(growth),
	    &needed_columns))
		return (-1);

	for (s = 0; s < RELIC_STRIPES; s++) {
		if (relic_idtab_reserve(&relation->payload.index[s],
		    growth->by_stripe[s], tuple_rehash, relation) != 0) {
			relic_diag_no_memory(ctx);
			return (-1);
		}
	}

	columns = relic_grow_array(relation->payload.columns,
	    &relation->payload.column_capacity, needed_columns,
	    sizeof(*relation->payload.columns));
	if (columns == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	relation->payload.columns = columns;

	return (0);
}

/*
 * Make room in a relation without secondary indexes for `growth` and keep
 * the storage total in step.
 *
 * The total is reconciled from what the relation holds before and after, so
 * it stays exact whether the reservation succeeded, failed part way through,
 * or grew nothing at all.  A total maintained by adding at the successful
 * sites and subtracting at the failing ones would be a list of places to
 * remember, and the list would be wrong the first time a step was added.
 */
int
relic_relation_reserve(relic_t *ctx, relic_relation_t *relation,
    const relic_tuple_growth_t *growth)
{
	size_t bytes_before;
	int status;

	bytes_before = relic_relation_bytes(relation);
	status = relation_grow(ctx, relation, growth);
	ctx->account.store_bytes +=
	    relic_relation_bytes(relation) - bytes_before;

	return (status);
}

/*
 * Make room for one tuple of wide hash `hash`, whose keys classify_keys()
 * has classified: a slot in its canonical stripe, its columns, and in every
 * secondary index its link and, when its key is new, the key.  Every step
 * that can fail is here, as relation_grow() explains.
 */
static int
insertion_grow(relic_t *ctx, relic_relation_t *relation, uint64_t hash)
{
	size_t needed_columns;
	size_t position;

	if (relation_too_large(ctx, relation, 1, &needed_columns))
		return (-1);

	if (payload_reserve_one(&relation->payload, hash, needed_columns,
	    tuple_rehash, relation) != 0) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	for (position = 0; position < relation->index_count; position++) {
		if (relic_index_reserve_pending(&relation->indexes[position],
		    relation) != 0) {
			relic_diag_no_memory(ctx);
			return (-1);
		}
	}

	return (0);
}

/*
 * Make room for one tuple and keep the storage total in step, by measuring
 * the parts the insertion can grow before and after, as
 * relic_relation_reserve() measures the whole relation.  A debug build
 * measures the whole relation too, so that a growth of anything the parts
 * leave out stops it rather than drifting the total.
 */
static int
insertion_reserve(relic_t *ctx, relic_relation_t *relation, uint64_t hash)
{
#ifndef NDEBUG
	size_t whole_before;
#endif
	size_t bytes_before;
	size_t grown;
	int status;

#ifndef NDEBUG
	whole_before = relic_relation_bytes(relation);
#endif

	bytes_before = insertion_parts_bytes(relation, hash);
	status = insertion_grow(ctx, relation, hash);
	grown = insertion_parts_bytes(relation, hash) - bytes_before;
	ctx->account.store_bytes += grown;

	assert(relic_relation_bytes(relation) - whole_before == grown);

	return (status);
}

/*
 * Classify the keys of the tuple about to be inserted, as the tuple after
 * the relation's last, in every secondary index, which is what the limits,
 * the reservation and the linking of an insertion all read
 * (relic_index_classify()).
 */
static void
classify_keys(relic_relation_t *relation, const relic_dom_t *columns)
{
	relic_tuple_id_t id;
	size_t position;

	id = (relic_tuple_id_t)(relation->payload.count + 1);
	for (position = 0; position < relation->index_count; position++)
		relic_index_classify(&relation->indexes[position], relation,
		    columns, id);
}

/*
 * Store a tuple the relation has room for, place it in its canonical
 * stripe under its wide hash, and link it into every secondary index by the
 * classification of its keys.
 *
 * Nothing here can fail, which is what lets the columns and every index entry
 * be published together.  The identifier is the one past the last, which is
 * what makes the store append-only: nothing here can name a tuple that
 * already exists.
 */
static void
tuple_append(relic_relation_t *relation, const relic_dom_t *columns,
    uint64_t hash)
{
	relic_tuple_id_t id RELIC_UNUSED;
	size_t position;

	id = payload_append(&relation->payload, relation->arity, columns,
	    hash);

	for (position = 0; position < relation->index_count; position++) {
		assert(relation->indexes[position].pending.tuple == id);
		relic_index_add_pending(&relation->indexes[position],
		    relation);
	}
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
 * Insert a tuple whose wide hash, tuple_hash(), the caller has computed: the
 * one insertion both relic_relation_insert() and a batch perform, so the two
 * cannot disagree about what a duplicate is.  It classifies, checks,
 * reserves and applies, in that order: the tuple and its keys are found or
 * found absent, the limits are asked about exactly the growth that implies,
 * room is made for it, and only then is anything stored.
 */
static int
relation_insert_hashed(relic_t *ctx, relic_relation_t *relation,
    const relic_dom_t *columns, uint64_t hash, bool *addedp)
{
	*addedp = false;

	if (relation_find_hashed(relation, columns, hash) != RELIC_ID_NONE)
		return (0);

	classify_keys(relation, columns);
	if (enforce_limits(ctx, relation, hash) != 0)
		return (-1);

	if (insertion_reserve(ctx, relation, hash) != 0)
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
	uint64_t hashes[RELIC_INSERT_BATCH];
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
			relic_idtab_prefetch(canonical_stripe(relation,
			    hashes[i]), relic_hash_low(hashes[i]));
		}

		if (insert_hashed_chunk(ctx, relation, chunk, hashes,
		    chunk_count, &added[first], completedp) != 0)
			return (-1);
	}

	return (0);
}

/*
 * The same, for tuples whose wide hashes the caller computed under the
 * context's seed, as relic_relation_screen() computes them: the first pass
 * prefetches and hashes nothing.  A debug build checks each hash against
 * the relation's own.
 */
int
relic_relation_insert_batch_hashed(relic_t *ctx, relic_relation_t *relation,
    const relic_dom_t *tuples, const uint64_t *hashes, size_t count,
    bool *added, size_t *completedp)
{
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
			assert(hashes[first + i] == tuple_hash(relation,
			    chunk + i * relation->arity));
			relic_idtab_prefetch(canonical_stripe(relation,
			    hashes[first + i]),
			    relic_hash_low(hashes[first + i]));
		}

		if (insert_hashed_chunk(ctx, relation, chunk, &hashes[first],
		    chunk_count, &added[first], completedp) != 0)
			return (-1);
	}

	return (0);
}

/*
 * The last two passes over a chunk of a batch whose slots the first pass has
 * prefetched: prefetch each tuple the slots name, then insert each in turn,
 * counting the insertions that completed.
 */
static int
insert_hashed_chunk(relic_t *ctx, relic_relation_t *relation,
    const relic_dom_t *chunk, const uint64_t *hashes, size_t count,
    bool *added, size_t *completedp)
{
	size_t i;

	for (i = 0; i < count; i++)
		prefetch_first_candidate(relation, hashes[i]);

	for (i = 0; i < count; i++) {
		if (relation_insert_hashed(ctx, relation,
		    chunk + i * relation->arity, hashes[i], &added[i]) != 0)
			return (-1);
		(*completedp)++;
	}

	return (0);
}

/*
 * Look up `count` tuples, packed at the relation's arity, as an insertion of
 * them would, without inserting any: set hashes[i] to tuple i's wide hash and
 * held[i] to whether the relation holds it.  The passes are those of a
 * batch insertion, so the misses of the lookups overlap as its duplicate
 * checks do, and the hash and the probe are its own, so a tuple found here
 * is exactly one an insertion would have found present.
 */
void
relic_relation_screen(const relic_relation_t *relation,
    const relic_dom_t *tuples, size_t count, uint64_t *hashes, bool *held)
{
	size_t i;

	assert(tuples != NULL);

	for (i = 0; i < count; i++) {
		hashes[i] = tuple_hash(relation, tuples + i * relation->arity);
		relic_idtab_prefetch(canonical_stripe(relation, hashes[i]),
		    relic_hash_low(hashes[i]));
	}

	for (i = 0; i < count; i++)
		prefetch_first_candidate(relation, hashes[i]);

	for (i = 0; i < count; i++)
		held[i] = relation_find_hashed(relation,
		    tuples + i * relation->arity, hashes[i]) != RELIC_ID_NONE;
}

uint64_t
relic_relation_hash(const relic_relation_t *relation, const relic_dom_t *tuple)
{
	return (tuple_hash(relation, tuple));
}

/*
 * Prefetch the tuple named by the slot `hash` selects, if the slot is
 * occupied.  That tuple is the first a probe for the hash compares, and for
 * a duplicate at the canonical index's load usually the one it finds.  A
 * relation of arity zero has no columns to fetch.
 */
static void
prefetch_first_candidate(const relic_relation_t *relation, uint64_t hash)
{
	relic_tuple_id_t id;

	if (relation->arity == 0)
		return;

	id = relic_idtab_peek(canonical_stripe(relation, hash),
	    relic_hash_low(hash));
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
	return (relic_relation_contains_hashed(relation, columns,
	    tuple_hash(relation, columns)));
}

/*
 * Whether the relation holds this tuple, whose wide hash the caller computed
 * under the context's seed, as every relation of the context hashes a
 * tuple.  A debug build checks the hash against the relation's own.
 */
bool
relic_relation_contains_hashed(const relic_relation_t *relation,
    const relic_dom_t *columns, uint64_t hash)
{
	assert(hash == tuple_hash(relation, columns));

	return (relation_find_hashed(relation, columns, hash) != RELIC_ID_NONE);
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
	if (id == RELIC_ID_NONE || id > relation->payload.count)
		return (NULL);

	return (tuple_at(relation, id));
}

size_t
relic_relation_count(const relic_relation_t *relation)
{
	return (relation->payload.count);
}

/*
 * One past the identifier of the last tuple, which is both where iteration
 * stops and the mark a later delta is measured from.
 */
relic_tuple_id_t
relic_relation_end(const relic_relation_t *relation)
{
	return ((relic_tuple_id_t)(relation->payload.count + 1));
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

relic_pred_id_t
relic_relation_pred(const relic_relation_t *relation)
{
	return (relation->pred);
}

uint16_t
relic_relation_arity(const relic_relation_t *relation)
{
	return (relation->arity);
}

const relic_index_t *
relic_relation_index_at(const relic_relation_t *relation, uint32_t position)
{
	assert(position < relation->index_count);

	return (&relation->indexes[position]);
}

bool
relic_relation_index_find(const relic_relation_t *relation,
    uint32_t key_mask, uint32_t *positionp)
{
	size_t position;

	for (position = 0; position < relation->index_count; position++) {
		if (relation->indexes[position].key_mask == key_mask) {
			*positionp = (uint32_t)position;
			return (true);
		}
	}

	return (false);
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
 * Make room in an index for the tuple its pending insertion classified, and
 * for the tuple's key when it is new, under the memory limit, keeping the
 * storage total in step as relic_relation_reserve() does, by the parts of
 * the index the insertion can grow.
 *
 * `index` need not be one the relation has published: it is measured as a
 * part of the storage total in its own right, which is what lets an index be
 * charged for while it is being built and before anything can reach it.
 */
static int
index_make_room(relic_t *ctx, const relic_relation_t *relation,
    relic_index_t *index)
{
#ifndef NDEBUG
	size_t whole_before;
#endif
	size_t bytes_before;
	size_t grown;
	int status;
	char described[RELIC_PRED_TEXT_MAX];

	bytes_before = relic_index_pending_bytes(index);

	if (relic_account_exceeds(&ctx->account, bytes_before,
	    relic_index_pending_bytes_after(index))) {
		relic_diag_limit(ctx, "memory", ctx->account.limit, "indexing",
		    relic_relation_describe(ctx, relation, described,
		    sizeof(described)));
		return (-1);
	}

#ifndef NDEBUG
	whole_before = relic_index_bytes(index);
#endif

	status = relic_index_reserve_pending(index, relation);
	grown = relic_index_pending_bytes(index) - bytes_before;
	ctx->account.store_bytes += grown;

	assert(relic_index_bytes(index) - whole_before == grown);
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
 * Each tuple is added as an insertion adds it: classified, room made, and
 * linked.  The limit is then applied at each growth exactly as it is when
 * the relation grows by insertion, so an index that fits is admitted however
 * it came to be built.
 */
static int
index_build(relic_t *ctx, const relic_relation_t *relation,
    relic_index_t *index)
{
	relic_tuple_id_t id;

	for (id = RELIC_TUPLE_FIRST; id <= relation->payload.count; id++) {
		relic_index_classify(index, relation, tuple_at(relation, id),
		    id);
		if (index_make_room(ctx, relation, index) != 0)
			return (-1);

		relic_index_add_pending(index, relation);
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
		relic_diag_internal(ctx, NULL,
		    "an index of %s was asked for on columns it cannot be "
		    "keyed on", relic_relation_identify(ctx, relation,
		    described, sizeof(described)));
		return (-1);
	}

	if (relic_relation_index_find(relation, key_mask, positionp))
		return (0);

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

	for (id = RELIC_TUPLE_FIRST; id <= relation->payload.count; id++) {
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
		relic_diag_internal(ctx, NULL,
		    "the statistics of %s were asked for a column it does not "
		    "have", relic_relation_identify(ctx, relation, described,
		    sizeof(described)));
		return (-1);
	}

	stats = &relation->stats;
	if (stats->counted_at != relation->payload.count) {
		stats->known = 0;
		stats->counted_at = relation->payload.count;
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
 * The relation is owned by the table, so the returned pointer stays valid
 * however many relations are created afterwards, until the table frees it:
 * a base store with its context, a derived relation at the next discard.
 */
static relic_relation_t *
reltab_relation(relic_t *ctx, relic_reltab_t *table, relic_pred_id_t pred)
{
	relic_relation_t *relation;

	if (relic_pred_of(ctx, pred) == NULL) {
		relic_diag_internal(ctx, NULL,
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
 * A derived predicate's relation, created empty if the last discard freed
 * it or none was ever asked for.  This is the only way to the table of
 * derived relations, and it marks the store as holding something derived,
 * so no derived relation exists while the mark is clear.
 */
static relic_relation_t *
derived_relation(relic_t *ctx, relic_pred_id_t pred)
{
	ctx->store.holds_derived = true;

	return (reltab_relation(ctx, &ctx->store.derived, pred));
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
		relic_diag_internal(ctx, NULL,
		    "a rule writes %s, which no rule derives",
		    relic_pred_identify(ctx, pred, described,
		    sizeof(described)));
		return (NULL);
	}

	return (derived_relation(ctx, pred));
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
		return (derived_relation(ctx, pred));

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

	return (store == NULL ? 0 : store->payload.count);
}

/*
 * The number of facts every base store holds.  Base stores only grow, so
 * the total only grows.
 */
size_t
relic_base_total(const relic_t *ctx)
{
	return (ctx->store.base_tuple_count);
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
 * Discard everything evaluation derived: free every derived relation and
 * drop the base stores' secondary indexes, keeping the facts the base
 * stores hold.  A derived relation asked for afterwards is created empty.
 * The mark is clear only while the store holds neither a derived relation
 * nor a secondary index, since both are created only where it is set and a
 * discard removes both, so a discard with the mark clear has nothing to do
 * and skips the walk.
 *
 * A pointer to a derived relation, a range of its identifiers and the
 * position of any secondary index taken before the discard all name nothing
 * after it.  The caller must hold none of them across it, which the
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
	relic_relation_t *relation;
	relic_store_t *store;
	size_t position;

	store = &ctx->store;
	if (!store->holds_derived) {
		assert(store->derived.count == 0);
		return;
	}

	for (position = 0; position < store->derived.count; position++) {
		relation = store->derived.entries[position];
		if (relation != NULL)
			relic_relation_clear(ctx, relation);
	}
	reltab_reset(&store->derived);

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
 * Builders.  A builder is a payload with no relation around it: no
 * secondary index, no statistics, no place in the store and no share of
 * either total, so building one is invisible to everything but its owner.
 * Its canonical index is the one a relation of its arity would build, so
 * that an empty base store can take the payload as it stands, except that
 * its stripes are tagged: a load mostly looks up tuples it has not seen,
 * and a tag rejects most other tuples in a probe's way without reading
 * their columns.  Adopting the payload drops the tags.
 */

/*
 * Make `builder` an empty builder of facts of `pred`, a predicate `ctx`
 * has interned, hashed as every relation of `ctx` hashes them.
 */
void
relic_builder_init(relic_builder_t *builder, const relic_t *ctx,
    relic_pred_id_t pred)
{
	bzero(builder, sizeof(*builder));
	payload_init(&builder->payload, true);
	builder->hash_seed = ctx->hash_seed;
	builder->pred = pred;
	builder->arity = relic_pred_of(ctx, pred)->arity;
}

/*
 * Release the tuples a builder holds, leaving it empty: all of them if the
 * commit never took them, and nothing if an adoption did.
 */
void
relic_builder_reset(relic_builder_t *builder)
{
	payload_release(&builder->payload);
}

size_t
relic_builder_count(const relic_builder_t *builder)
{
	return (builder->payload.count);
}

/*
 * The columns of builder tuple `position`, counted from zero in the order
 * the tuples were added.  For a builder of arity zero the result is never
 * read.
 */
const relic_dom_t *
relic_builder_tuple(const relic_builder_t *builder, size_t position)
{
	assert(position < builder->payload.count);

	return (payload_tuple(&builder->payload, builder->arity,
	    (relic_tuple_id_t)(position + 1)));
}

/*
 * Whether a builder holds `columns`, whose wide hash under the builder's
 * seed is `hash`.  A debug build checks the hash.
 */
bool
relic_builder_contains_hashed(const relic_builder_t *builder,
    const relic_dom_t *columns, uint64_t hash)
{
	assert(hash == relic_tuple_hash(builder->hash_seed, columns,
	    builder->arity));

	return (payload_find(&builder->payload, builder->arity, columns,
	    hash) != RELIC_ID_NONE);
}

/*
 * The hash of builder tuple `id` for its canonical stripe to use when it
 * grows, which is what tuple_rehash() gives a relation's.
 */
static uint32_t
builder_rehash(uint32_t id, void *ctx)
{
	const relic_builder_t *builder;

	builder = ctx;

	return (relic_hash_words(builder->hash_seed,
	    payload_tuple(&builder->payload, builder->arity, id),
	    builder->arity));
}

/*
 * Add `columns`, of wide hash `hash`, which the builder lacks, as its last
 * tuple, growing its columns and canonical stripe as an insertion into a
 * relation grows them.  Room is made before the tuple is stored, so a
 * failure leaves the builder holding what it held.  Returns 0, or -1 having
 * reported that the builder is full or that memory ran out.
 */
int
relic_builder_add_hashed(relic_t *ctx, relic_builder_t *builder,
    const relic_dom_t *columns, uint64_t hash)
{
	relic_payload_t *payload;
	size_t needed_columns;
	char described[RELIC_PRED_TEXT_MAX];

	payload = &builder->payload;
	assert(!relic_builder_contains_hashed(builder, columns, hash));

	if (payload->count >= RELIC_TUPLE_MAX ||
	    !columns_holding(payload->count + 1, builder->arity,
	    &needed_columns)) {
		relic_diag_error(ctx, RELIC_ERROR_LIMIT, NULL,
		    "too many facts of %s in one load",
		    relic_pred_describe(ctx, builder->pred, described,
		    sizeof(described)));
		return (-1);
	}

	if (payload_reserve_one(payload, hash, needed_columns, builder_rehash,
	    builder) != 0) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	(void)payload_append(payload, builder->arity, columns, hash);

	return (0);
}

/*
 * Make room in `builder` for the tuples `growth` counts by stripe, all of
 * them ones it lacks, reaching the capacities adding them one at a time
 * would: each stripe reserved for its own count reaches what reserving one
 * at a time reaches, and the columns grow stepwise as payload_reserve_one()
 * grows them.  Returns 0, or -1 having reported nothing, when the builder
 * would hold more than it can or memory ran out; what grew stays, and
 * holds no new tuple.
 */
int
relic_builder_reserve(relic_builder_t *builder,
    const relic_tuple_growth_t *growth)
{
	relic_payload_t *payload;
	relic_dom_t *columns;
	size_t needed_columns;
	size_t added;
	size_t s;

	payload = &builder->payload;
	added = relic_tuple_growth_total(growth);
	if (added == 0)
		return (0);
	if (added > RELIC_TUPLE_MAX - payload->count ||
	    !columns_holding(payload->count + added, builder->arity,
	    &needed_columns))
		return (-1);

	for (s = 0; s < RELIC_STRIPES; s++) {
		if (growth->by_stripe[s] > 0 &&
		    relic_idtab_reserve(&payload->index[s],
		    growth->by_stripe[s], builder_rehash, builder) != 0)
			return (-1);
	}

	columns = relic_grow_array_stepwise(payload->columns,
	    &payload->column_capacity, payload->count * builder->arity,
	    added, builder->arity, sizeof(*payload->columns));
	if (columns == NULL)
		return (-1);
	payload->columns = columns;
	assert(payload->column_capacity >= needed_columns);

	return (0);
}

/*
 * Describe, once relic_builder_reserve() has made room for a batch, where
 * the part filling each canonical stripe of the builder writes the batch's
 * tuples: fills[s] for stripe s.  The builder must not change until every
 * stripe is filled and relic_builder_admit() has counted them.
 */
void
relic_builder_stripe_fills(relic_builder_t *builder,
    relic_stripe_fill_t fills[RELIC_STRIPES])
{
	relic_payload_t *payload;
	size_t s;

	payload = &builder->payload;
	for (s = 0; s < RELIC_STRIPES; s++) {
		fills[s].columns = payload->columns;
		fills[s].index = &payload->index[s];
		fills[s].held = payload->count;
		fills[s].stripe = s;
		fills[s].arity = builder->arity;
	}
}

/*
 * Write each tuple of `batch` that falls in the fill's stripe and that the
 * batch ranks at the place its rank gives it after the builder's last
 * tuple, and place it in the stripe, in the order the tuples are listed,
 * which is the order adding them one at a time would place them in.
 * Nothing here can fail.
 */
void
relic_stripe_fill(const relic_stripe_fill_t *fill,
    const relic_builder_batch_t *batch)
{
	const relic_dom_t *source;
	relic_dom_t *destination;
	relic_tuple_id_t id;
	uint32_t position;
	uint16_t column;
	size_t i;

	for (i = batch->stripe_start[fill->stripe];
	    i < batch->stripe_start[fill->stripe + 1]; i++) {
		position = batch->stripe_list[i];
		if (batch->ranks[position] == 0)
			continue;

		source = batch->tuples + (size_t)position * fill->arity;
		assert(relic_hash_stripe(batch->hashes[position]) ==
		    fill->stripe);
		assert(batch->ranks[position] <= RELIC_TUPLE_MAX - fill->held);
		id = (relic_tuple_id_t)(fill->held + batch->ranks[position]);
		destination = fill->columns + ((size_t)id - 1) * fill->arity;
		for (column = 0; column < fill->arity; column++)
			destination[column] = source[column];
		relic_idtab_place(fill->index,
		    relic_hash_low(batch->hashes[position]), id);
	}
}

/*
 * Count the tuples `batch` ranks, with which every stripe of the builder
 * was filled, as its own.
 */
void
relic_builder_admit(relic_builder_t *builder,
    const relic_builder_batch_t *batch)
{
	assert(batch->count <= RELIC_TUPLE_MAX - builder->payload.count);

	builder->payload.count += batch->count;
}

/*
 * The base commit.  A load gives every base store its facts in one step,
 * the builders of its predicates described by a manifest of actions, one
 * per store that gains a tuple: relic_base_action_prepare() describes
 * each before anything changes, relic_bases_reserve() makes room for all
 * of them or for none, and relic_bases_apply() publishes all of them.
 */

/*
 * Describe what the commit does to the base store of `builder`'s predicate
 * with the builder's tuples, of which there is at least one, as `action`:
 * an adoption when the store holds no tuple, and otherwise an append, whose
 * hashes and stripe counts are computed here.  Returns 0, or -1 when memory
 * ran out, having reported nothing.  The action is released with
 * relic_base_action_release() whatever this returns.
 */
int
relic_base_action_prepare(const relic_t *ctx, relic_base_action_t *action,
    relic_builder_t *builder)
{
	uint64_t hash;
	size_t i;

	assert(relic_builder_count(builder) > 0);

	bzero(action, sizeof(*action));
	action->builder = builder;
	action->adopt = relic_base_count(ctx, builder->pred) == 0;
	if (action->adopt)
		return (0);

	action->hashes = reallocarray(NULL, relic_builder_count(builder),
	    sizeof(*action->hashes));
	if (action->hashes == NULL)
		return (-1);

	for (i = 0; i < relic_builder_count(builder); i++) {
		hash = relic_tuple_hash(builder->hash_seed,
		    relic_builder_tuple(builder, i), builder->arity);
		action->hashes[i] = hash;
		action->added.by_stripe[relic_hash_stripe(hash)]++;
	}

	return (0);
}

void
relic_base_action_release(relic_base_action_t *action)
{
	free(action->hashes);
	action->hashes = NULL;
}

/*
 * Check that the base stores can take what `actions` gives them, under
 * both configured limits, before any of them changes.  Returns 0, or -1
 * having reported the refusal: a limit's against the predicate of the
 * first action, and a store too large to grow against its own.
 *
 * An adoption's store, absent or empty, is projected as holding the
 * builder's payload, without the tags adopting it drops, in place of
 * whatever empty capacity it kept; an append's as its relation grown by
 * the tuples it gains.  Sums that a
 * size_t cannot hold cannot fit any limit either, and are refused as a
 * store too large to grow.
 */
static int
bases_fit(relic_t *ctx, const relic_base_action_t *actions, size_t count)
{
	const relic_relation_t *store;
	size_t added;
	size_t added_total;
	size_t bytes_after;
	size_t bytes_before;
	size_t held_bytes;
	size_t held_tuples;
	size_t projected;
	size_t i;
	char described[RELIC_PRED_TEXT_MAX];

	added_total = 0;
	bytes_before = 0;
	bytes_after = 0;

	for (i = 0; i < count; i++) {
		store = relic_base_store(ctx, actions[i].builder->pred);
		held_bytes = store == NULL ? 0 : relic_relation_bytes(store);
		held_tuples = store == NULL ? 0 : relic_relation_count(store);
		if (actions[i].adopt) {
			assert(store == NULL || store->payload.count == 0);
			added = relic_builder_count(actions[i].builder);
			projected = payload_bytes_untagged(
			    &actions[i].builder->payload);
		} else {
			assert(store != NULL && store->payload.count != 0);
			added = relic_tuple_growth_total(&actions[i].added);
			projected = relic_relation_bytes_after_growth(store,
			    &actions[i].added);
		}

		if (added > RELIC_TUPLE_MAX - held_tuples ||
		    added > SIZE_MAX - added_total ||
		    held_bytes > SIZE_MAX - bytes_before ||
		    projected > SIZE_MAX - bytes_after) {
			relic_diag_error(ctx, RELIC_ERROR_LIMIT, NULL,
			    "the relation of %s is too large to grow",
			    relic_pred_describe(ctx, actions[i].builder->pred,
			    described, sizeof(described)));
			return (-1);
		}

		added_total += added;
		bytes_before += held_bytes;
		bytes_after += projected;
	}

	if (relic_limit_set(ctx->config.max_tuples) &&
	    (ctx->store.tuple_count > ctx->config.max_tuples ||
	    added_total > ctx->config.max_tuples - ctx->store.tuple_count))
		return (relic_bases_refuse(ctx, "tuples",
		    ctx->config.max_tuples, actions[0].builder->pred));

	if (relic_account_exceeds(&ctx->account, bytes_before, bytes_after))
		return (relic_bases_refuse(ctx, "memory", ctx->account.limit,
		    actions[0].builder->pred));

	return (0);
}

/*
 * Report that the limit `key`, of `limit`, refuses a load whose first
 * growing store is `pred`'s, and return -1.
 */
int
relic_bases_refuse(relic_t *ctx, const char *key, size_t limit,
    relic_pred_id_t pred)
{
	char described[RELIC_PRED_TEXT_MAX];

	relic_diag_limit(ctx, key, limit, "loading",
	    relic_pred_describe(ctx, pred, described, sizeof(described)));

	return (-1);
}

/*
 * Make every base store of `actions`, which holds at least one action,
 * ready to take its builder's tuples.  Returns 0, or -1 having reported the
 * refusal.  The caller has discarded what was derived, so no base store has
 * a secondary index.
 *
 * Both limits are checked once, over every store together, before any
 * store changes, so a refusal by either leaves every store as it was.  Then
 * every adoption's existing store gives back the empty capacity the
 * builder's payload is to replace, before any append reserves, so that the
 * credit the limit check counted on is real by the time anything spends
 * it.  Then each absent store is created and each append's grown.  A failed
 * allocation among those can leave stores empty, created, or with capacity
 * they do not use, all accounted, but no store gains a tuple.
 */
int
relic_bases_reserve(relic_t *ctx, const relic_base_action_t *actions,
    size_t count)
{
	relic_relation_t *store;
	size_t i;

	assert(count > 0);

	if (bases_fit(ctx, actions, count) != 0)
		return (-1);

	for (i = 0; i < count; i++) {
		store = reltab_find(&ctx->store.bases,
		    actions[i].builder->pred);
		if (actions[i].adopt && store != NULL)
			relic_relation_clear(ctx, store);
	}

	for (i = 0; i < count; i++) {
		store = reltab_relation(ctx, &ctx->store.bases,
		    actions[i].builder->pred);
		if (store == NULL)
			return (-1);

		if (!actions[i].adopt &&
		    relic_relation_reserve(ctx, store, &actions[i].added) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Give every base store of `actions` its builder's tuples, into what
 * relic_bases_reserve() made ready.  Nothing here can fail, so either every
 * store gains its tuples or, the reserve having failed, none does.
 *
 * An adoption moves the builder's payload into its empty store, through
 * adopt_builder(), and an append copies the builder's tuples after the
 * store's own, through append_builder().
 */
void
relic_bases_apply(relic_t *ctx, const relic_base_action_t *actions,
    size_t count)
{
	relic_builder_t *builder;
	relic_relation_t *store;
	size_t added;
	size_t i;

	for (i = 0; i < count; i++) {
		builder = actions[i].builder;
		store = reltab_find(&ctx->store.bases, builder->pred);
		assert(store != NULL && store->index_count == 0);
		assert(store->arity == builder->arity &&
		    store->hash_seed == builder->hash_seed);

		added = relic_builder_count(builder);
		if (actions[i].adopt)
			adopt_builder(ctx, store, builder);
		else {
			assert(added ==
			    relic_tuple_growth_total(&actions[i].added));
			append_builder(store, &actions[i]);
		}

		ctx->store.tuple_count += added;
		ctx->store.base_tuple_count += added;
	}
}

/*
 * Move `builder`'s payload whole into `store`, which holds nothing, and
 * leave the builder empty.  The tuples keep their identifiers, and the
 * canonical index, built under the store's seed and arity, is the store's
 * as it stands once its tags are dropped, which the store's lookups, that
 * mostly find what they look for, have no use for.
 */
static void
adopt_builder(relic_t *ctx, relic_relation_t *store, relic_builder_t *builder)
{
	size_t s;

	assert(store->payload.count == 0 &&
	    payload_bytes(&store->payload) == 0);

	for (s = 0; s < RELIC_STRIPES; s++)
		relic_idtab_drop_tags(&builder->payload.index[s]);
	store->payload = builder->payload;
	payload_init(&builder->payload, true);
	ctx->account.store_bytes += payload_bytes(&store->payload);
}

/*
 * Copy each tuple of an append's builder, in builder order, to the position
 * after `store`'s last, and place it under the hash `action` prepared,
 * looking nothing up.  A debug build checks what a release build trusts:
 * the room, the hashes, and that the store lacks every tuple.
 */
static void
append_builder(relic_relation_t *store, const relic_base_action_t *action)
{
	const relic_dom_t *columns;
	uint64_t hash;
	size_t t;

	for (t = 0; t < relic_builder_count(action->builder); t++) {
		columns = relic_builder_tuple(action->builder, t);
		hash = action->hashes[t];
		assert(insertion_parts_bytes_after(store, hash) ==
		    insertion_parts_bytes(store, hash));
		assert(hash == tuple_hash(store, columns));
		assert(relation_find_hashed(store, columns, hash) ==
		    RELIC_ID_NONE);
		tuple_append(store, columns, hash);
	}
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

	name = "?";
	pred = relic_pred_of(ctx, relation->pred);
	if (pred != NULL)
		name = pred->name.bytes;

	if (is_called_by_predicate(ctx, relation))
		return (relic_diag_name(name, buffer, size));

	if ((size_t)snprintf(buffer, size, "%s$base", name) >= size)
		relic_diag_mark_cut(buffer, size);

	return (buffer);
}

/*
 * Whether a relation of a predicate is called by the predicate's name:
 * every one but the base store of a derived predicate, called by its base
 * view's name.  A query's answer has no predicate, and both callers name it
 * before they ask.
 */
static bool
is_called_by_predicate(const relic_t *ctx, const relic_relation_t *relation)
{
	const relic_pred_t *pred;

	assert(relation->pred != RELIC_ID_NONE);

	pred = relic_pred_of(ctx, relation->pred);

	return (pred == NULL || !pred->derived ||
	    relation != relic_base_store(ctx, relation->pred));
}

/*
 * Write what a message about a relation calls it into `buffer`, which must
 * hold RELIC_PRED_TEXT_MAX bytes, and return it.  A relation called by its
 * predicate's name is described exactly as relic_pred_describe() describes
 * the predicate, so the relation of a generated predicate is described in
 * the terms of the program.  A query's answer is described as the query,
 * as the relations the Magic rewrite generates for the query are, and a
 * base view's store by its own name.
 */
const char *
relic_relation_describe(const relic_t *ctx, const relic_relation_t *relation,
    char *buffer, size_t size)
{
	if (relation->pred == RELIC_ID_NONE) {
		strlcpy(buffer, RELIC_QUERY_DESCRIPTION, size);
		return (buffer);
	}

	if (is_called_by_predicate(ctx, relation))
		return (relic_pred_describe(ctx, relation->pred, buffer, size));

	return (relic_relation_identify(ctx, relation, buffer, size));
}

/*
 * Write "name/arity" of the relation itself into `buffer`, which must hold
 * RELIC_PRED_TEXT_MAX bytes, and return it: a generated predicate's relation
 * by its own name, for an internal error, where what a maintainer needs is
 * the relation the code reached.
 */
const char *
relic_relation_identify(const relic_t *ctx, const relic_relation_t *relation,
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
