/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"

/*
 * The builder of a direct grouped reduction, and the summary it becomes
 * (lib/reduce.h, specs/design-group-reduction.md).
 *
 * A builder keeps one group per distinct key among the rows it takes, in
 * the order the keys first occur, and in each group the row count and an
 * accumulator per signature (lib/fold.h).  A row's values are identifiers
 * the context assigned or values it has not, which the producer's batch
 * holds pending (lib/exec.h); a key is found by its values, hashed and
 * compared as values whatever the representation, so a pending string and
 * the same string assigned would be one key, and a symbol and a string of
 * the same bytes are two.  Nothing a batch holds outlives it: a key's text
 * is copied into the builder's key bytes once, when its group is made, and
 * a min's or a max's text into a buffer of the extreme's own, which the
 * next extreme reuses, so a group holds its key and its extremes and no
 * storage that grows with its rows.  Every row is counted against the most
 * a relation could hold, so that a group's exact sum cannot wrap however
 * the source's rows were never stored.
 *
 * Finishing gives the values the groups retain -- keys, and the extremes
 * of groups whose extreme has a value -- the identifiers they will keep,
 * interning those the context lacks in the order of their first
 * occurrence among the source's rows, and copies the groups into a
 * summary keyed by identifiers, which is what a fold probes with.  The
 * summary is laid out and charged before any value is interned, and is
 * handed to the caller only once every group is in it, so a refusal at
 * any step leaves no summary, though it may leave values interned, as a
 * refused insertion leaves the values of the batches before it.  Every
 * walk finishing makes -- of the groups, of the retained values while
 * they are sorted and while they are interned -- looks for the interrupt
 * every RELIC_REDUCE_CHUNK steps, so no step of it grows with the input.
 */

/*
 * The most rows one source may reduce: as many as a relation could hold,
 * so that every group's sum has fewer than 2^32 addends and its exact
 * total stays far inside the two words that hold it (lib/arith.h).  A test
 * build lowers it to reach the refusal.
 */
#ifndef RELIC_REDUCE_ROWS_MAX
#define	RELIC_REDUCE_ROWS_MAX	((uint64_t)RELIC_TUPLE_MAX)
#endif

/*
 * The steps a walk finishing makes between looks for the interrupt, and
 * the retained values interned as one batch.
 */
#define	RELIC_REDUCE_CHUNK	4096u

typedef struct relic_extreme relic_extreme_t;
typedef struct relic_groups relic_groups_t;
typedef struct relic_held relic_held_t;
typedef struct relic_retained relic_retained_t;
typedef struct relic_seen relic_seen_t;
typedef struct relic_stored_builder relic_stored_builder_t;

/*
 * A value a builder holds: an identifier the context had assigned, or a
 * value it had not, an integer or a symbol's or a string's `length` bytes,
 * a key's at `offset` in the builder's key bytes and an extreme's at the
 * start of the extreme's buffer.
 */
struct relic_held {
	union {
		relic_dom_t	dom;		/* assigned */
		int64_t		integer;	/* RELIC_INTEGER */
		size_t		offset;		/* a key's text */
	}			u;
	size_t			length;		/* RELIC_SYMBOL, RELIC_STRING */
	enum relic_type		type;
	bool			assigned;
};

/*
 * The extreme of one group's min or max: the value, held as above, the
 * buffer an owned text is copied into, `capacity` bytes the extremes the
 * group holds in turn reuse, and the ordinal of the row that holds it,
 * the first of equal ones.
 */
struct relic_extreme {
	char		*bytes;
	size_t		 capacity;
	relic_held_t	 value;
	uint64_t	 ordinal;
};

/*
 * The groups' arrays, `capacity` groups each, laid out in one block: each
 * group's `key_count` key values, its row count, the ordinal of its first
 * row, the hash of its key, and `signature_count` accumulators and
 * extremes, one per signature; a count's and a sum's extreme is unused.
 */
struct relic_groups {
	relic_held_t		*keys;
	uint64_t		*rows;
	uint64_t		*first;
	uint32_t		*hashes;
	relic_fold_state_t	*states;
	relic_extreme_t		*extremes;
};

/*
 * A row's value as a builder reads it: a view of it, and its identifier,
 * or zero when the context has none.
 */
struct relic_seen {
	relic_value_view_t	view;
	relic_dom_t		dom;
};

/*
 * A value finishing gives an identifier to: the held value, the bytes it
 * is held in, and where it first occurs, the ordinal of the row and the
 * column, which is what orders the values interned.
 */
struct relic_retained {
	relic_held_t	*held;
	const char	*bytes;
	uint64_t	 ordinal;
	uint16_t	 column;
};

/*
 * A builder.  `signatures` are the source's, borrowed from the scope,
 * `key_columns` the source columns of its key in increasing order, and
 * `table` finds a group by its key's hash, holding group positions plus
 * one.  `charged` is what it holds of the account now, `peak` the most it
 * has, and `rows_seen` the rows it has taken.  `next_ordinal` is the
 * occurrence of the next row, which a serial run leaves equal to
 * `rows_seen` and a lane sets to its slice's place in visit order.
 * `budget` is the most it may charge, SIZE_MAX when it is the global
 * builder and has none.  A row that would pass the budget leaves `full`,
 * and `oversized` when the builder still holds no group, which is a row
 * no empty partial can admit.  `failed` is a refusal a limited builder
 * does not report, for the caller to run the same rows serially.
 */
struct relic_reducer {
	const relic_reduce_signature_t	*signatures;
	relic_groups_t			 groups;
	void				*block;
	char				*key_bytes;
	relic_idtab_t			 table;
	size_t				 signature_count;
	size_t				 group_count;
	size_t				 group_capacity;
	size_t				 block_bytes;
	size_t				 key_bytes_used;
	size_t				 key_bytes_capacity;
	size_t				 charged;
	size_t				 peak;
	size_t				 budget;
	uint64_t			 rows_seen;
	uint64_t			 next_ordinal;
	uint64_t			 hash_seed;
	uint16_t			 key_columns[RELIC_MAX_ARITY];
	uint16_t			 key_count;
	uint16_t			 arity;
	bool				 full;
	bool				 oversized;
	bool				 failed;
	bool				 report;
};

/*
 * A summary: `group_count` groups, each's key identifiers, row count, and
 * `signature_count` accumulators and extremes, in the order of the keys'
 * first occurrence, and `table`, which finds a group by its key and is
 * laid out in the same block, sized for exactly the groups it holds.
 * `bytes` is the block's, charged to the account until it is destroyed.
 */
struct relic_summary {
	relic_dom_t		*keys;
	uint64_t		*rows;
	relic_fold_state_t	*states;
	relic_dom_t		*extremes;
	void			*block;
	relic_idtab_t		 table;
	size_t			 bytes;
	size_t			 group_count;
	size_t			 signature_count;
	uint64_t		 hash_seed;
	uint16_t		 key_count;
};

/*
 * The builder of one family's summary over a stored relation
 * (relic_summarize_relation()).  Every value of a stored tuple is an
 * identifier the context assigned, so none of a reducer's pending values
 * arise: a group is found by its key's identifiers, hashed as the summary
 * hashes them, and its key is read from its `first` tuple, which stays in
 * place for the walk, rather than copied.  `capacity` groups are laid out
 * in one block of `block_bytes`, each with its first tuple, row count,
 * and an accumulator and an extreme's identifier per signature, in the
 * order of the keys' first occurrence; `table` finds a group by its key,
 * holding positions plus one.  `charged` is what the builder holds of the
 * account.
 */
struct relic_stored_builder {
	const relic_fold_family_t	*family;
	const relic_relation_t		*relation;
	relic_tuple_id_t		*first;
	uint64_t			*rows;
	relic_fold_state_t		*states;
	relic_dom_t			*extremes;
	void				*block;
	relic_idtab_t			 table;
	size_t				 group_count;
	size_t				 capacity;
	size_t				 block_bytes;
	size_t				 charged;
	uint64_t			 hash_seed;
	uint16_t			 key_columns[RELIC_MAX_ARITY];
	uint16_t			 key_count;
};

static bool	 charge(relic_t *ctx, relic_reducer_t *reducer, size_t bytes);
static void	 release(relic_t *ctx, relic_reducer_t *reducer, size_t bytes);
static void	 report_no_memory(relic_t *ctx, relic_reducer_t *reducer);
static void	 release_groups(relic_t *ctx, relic_reducer_t *reducer);
static void	 see_held(const relic_t *ctx, const char *bytes,
		    const relic_held_t *held, relic_seen_t *seen);
static int	 copy_extreme(relic_t *ctx, relic_reducer_t *reducer,
		    relic_extreme_t *dst, const relic_extreme_t *src);
static size_t	 lay_out_groups(const relic_reducer_t *reducer,
		    size_t capacity, void *memory, relic_groups_t *groups);
static int	 grow_groups(relic_t *ctx, relic_reducer_t *reducer);
static int	 reserve_key_bytes(relic_t *ctx, relic_reducer_t *reducer,
		    size_t length);
static uint32_t	 rehash_group(uint32_t id, void *owner);
static void	 see_value(const relic_t *ctx,
		    const relic_reduce_batch_t *batch, relic_dom_t value,
		    relic_seen_t *seen);
static uint32_t	 key_hash(const relic_reducer_t *reducer,
		    const relic_seen_t *key);
static void	 held_view(const relic_t *ctx, const char *bytes,
		    const relic_held_t *held, relic_value_view_t *view);
static bool	 views_equal(const relic_value_view_t *left,
		    const relic_value_view_t *right);
static bool	 key_matches(const relic_t *ctx,
		    const relic_reducer_t *reducer, size_t group,
		    const relic_seen_t *key);
static size_t	 find_group(const relic_t *ctx,
		    const relic_reducer_t *reducer, uint32_t hash,
		    const relic_seen_t *key);
static int	 add_group(relic_t *ctx, relic_reducer_t *reducer,
		    uint32_t hash, const relic_seen_t *key, size_t *groupp);
static void	 hold(relic_held_t *held, const relic_seen_t *seen,
		    size_t offset);
static int	 retain_extreme(relic_t *ctx, relic_reducer_t *reducer,
		    relic_extreme_t *extreme, const relic_seen_t *seen);
static int	 take_row(relic_t *ctx, relic_reducer_t *reducer,
		    const relic_reduce_batch_t *batch, const relic_dom_t *row);
static void	 refuse_rows(relic_t *ctx, const relic_relation_t *relation);
static int	 poll_walk(relic_t *ctx, const relic_relation_t *relation,
		    size_t step);
static int	 count_retained(relic_t *ctx, const relic_reducer_t *reducer,
		    const relic_relation_t *relation, size_t *countp);
static int	 collect_retained(relic_t *ctx, relic_reducer_t *reducer,
		    const relic_relation_t *relation,
		    relic_retained_t *retained);
static bool	 retained_before(const relic_retained_t *left,
		    const relic_retained_t *right);
static int	 sort_retained(relic_t *ctx, const relic_relation_t *relation,
		    relic_retained_t *retained, relic_retained_t *spare,
		    size_t count, relic_retained_t **sortedp);
static int	 publish(relic_t *ctx, relic_reducer_t *reducer,
		    const relic_relation_t *relation);
static int	 intern_retained(relic_t *ctx, relic_reducer_t *reducer,
		    const relic_relation_t *relation,
		    relic_retained_t *retained, size_t count);
static size_t	 retained_text(const relic_retained_t *retained,
		    size_t count);
static void	 describe_retained(const relic_retained_t *retained,
		    size_t count, relic_pending_t *pending, char *text);
static int	 intern_chunk(relic_t *ctx, relic_reducer_t *reducer,
		    const relic_relation_t *relation,
		    relic_retained_t *retained, size_t count);
static size_t	 lay_out_summary(relic_summary_t *summary, void *memory);
static int	 open_summary(relic_t *ctx, relic_reducer_t *reducer,
		    relic_summary_t **summaryp);
static void	 discard_summary(relic_t *ctx, relic_reducer_t *reducer,
		    relic_summary_t *summary);
static int	 fill_summary(relic_t *ctx, relic_reducer_t *reducer,
		    const relic_relation_t *relation,
		    relic_summary_t *summary);
static void	 stored_open(relic_t *ctx, relic_stored_builder_t *builder,
		    const relic_fold_family_t *family,
		    const relic_relation_t *relation);
static bool	 stored_charge(relic_t *ctx, relic_stored_builder_t *builder,
		    size_t bytes);
static void	 stored_release(relic_t *ctx,
		    relic_stored_builder_t *builder, size_t bytes);
static size_t	 lay_out_stored(relic_stored_builder_t *builder,
		    size_t capacity, void *memory);
static int	 grow_stored(relic_t *ctx, relic_stored_builder_t *builder);
static uint32_t	 rehash_stored(uint32_t id, void *owner);
static size_t	 find_stored(const relic_stored_builder_t *builder,
		    uint32_t hash, const relic_dom_t *key);
static int	 add_stored(relic_t *ctx, relic_stored_builder_t *builder,
		    uint32_t hash, relic_tuple_id_t id, size_t *groupp);
static int	 take_stored(relic_t *ctx, relic_stored_builder_t *builder,
		    relic_tuple_id_t id, const relic_dom_t *tuple);
static int	 finish_stored(relic_t *ctx, relic_stored_builder_t *builder,
		    relic_summary_t **summaryp);
static void	 discard_stored(relic_t *ctx, relic_stored_builder_t *builder,
		    relic_summary_t *summary);
static void	 close_stored(relic_t *ctx, relic_stored_builder_t *builder);

/*
 * Charge `bytes` more for the builder.  An ordinary builder uses the
 * arena account, on the calling thread.  A limited one uses the round
 * account, which workers may charge together, so a wave's partials do
 * not race on the arena total.
 */
static bool
charge(relic_t *ctx, relic_reducer_t *reducer, size_t bytes)
{
	if (reducer->budget != SIZE_MAX &&
	    (reducer->charged >= reducer->budget ||
	    bytes > reducer->budget - reducer->charged)) {
		if (reducer->group_count == 0)
			reducer->oversized = true;
		reducer->full = true;
		return (false);
	}
	if (reducer->budget != SIZE_MAX) {
		if (!relic_account_charge_round(&ctx->account, bytes)) {
			reducer->failed = true;
			return (false);
		}
	} else if (!relic_account_charge(&ctx->account, bytes)) {
		report_no_memory(ctx, reducer);
		return (false);
	}

	reducer->charged += bytes;
	if (reducer->charged > reducer->peak)
		reducer->peak = reducer->charged;

	return (true);
}

/*
 * A limited builder does not report: its caller abandons the parallel
 * attempt and runs the same rows serially, where the refusal is said.
 */
static void
report_no_memory(relic_t *ctx, relic_reducer_t *reducer)
{
	if (reducer->report)
		relic_diag_no_memory(ctx);
	else
		reducer->failed = true;
}

static void
release(relic_t *ctx, relic_reducer_t *reducer, size_t bytes)
{
	assert(bytes <= reducer->charged);
	if (reducer->budget != SIZE_MAX)
		relic_account_release_round(&ctx->account, bytes);
	else
		relic_account_release(&ctx->account, bytes);
	reducer->charged -= bytes;
}

static int	 open_reducer(relic_t *ctx, const relic_reduced_source_t *source,
		    uint16_t arity, bool report, relic_reducer_t **reducerp);

int
relic_reducer_open(relic_t *ctx, const relic_reduced_source_t *source,
    uint16_t arity, relic_reducer_t **reducerp)
{
	return (open_reducer(ctx, source, arity, true, reducerp));
}

static int
open_reducer(relic_t *ctx, const relic_reduced_source_t *source,
    uint16_t arity, bool report, relic_reducer_t **reducerp)
{
	relic_reducer_t *reducer;
	uint16_t column;

	*reducerp = NULL;
	if (!relic_account_charge(&ctx->account, sizeof(*reducer))) {
		if (report)
			relic_diag_no_memory(ctx);
		return (-1);
	}

	reducer = calloc(1, sizeof(*reducer));
	if (reducer == NULL) {
		relic_account_release(&ctx->account, sizeof(*reducer));
		if (report)
			relic_diag_no_memory(ctx);
		return (-1);
	}

	reducer->charged = sizeof(*reducer);
	reducer->peak = reducer->charged;
	reducer->budget = SIZE_MAX;
	reducer->report = report;
	reducer->signatures = source->signatures;
	reducer->signature_count = source->signature_count;
	reducer->hash_seed = ctx->hash_seed;
	reducer->arity = arity;
	for (column = 0; column < arity; column++) {
		if ((source->key_mask & (UINT32_C(1) << column)) != 0)
			reducer->key_columns[reducer->key_count++] = column;
	}
	relic_idtab_init(&reducer->table, true);

	*reducerp = reducer;

	return (0);
}

/*
 * Every extreme's buffer, the key bytes, the table, the groups' block and
 * the builder itself are given back, which is everything it charged.
 */
void
relic_reducer_close(relic_t *ctx, relic_reducer_t *reducer)
{
	if (reducer == NULL)
		return;

	release_groups(ctx, reducer);
	assert(reducer->charged == sizeof(*reducer));
	relic_account_release(&ctx->account, sizeof(*reducer));
	free(reducer);
}

/*
 * Give back the groups, their table, their key bytes and every extreme's
 * buffer, leaving the builder as relic_reducer_open() made it.
 */
static void
release_groups(relic_t *ctx, relic_reducer_t *reducer)
{
	size_t extremes;
	size_t i;

	extremes = reducer->group_count * reducer->signature_count;
	for (i = 0; i < extremes; i++) {
		free(reducer->groups.extremes[i].bytes);
		release(ctx, reducer, reducer->groups.extremes[i].capacity);
	}

	release(ctx, reducer, relic_idtab_bytes(&reducer->table));
	relic_idtab_reset(&reducer->table);
	free(reducer->key_bytes);
	release(ctx, reducer, reducer->key_bytes_capacity);
	free(reducer->block);
	release(ctx, reducer, reducer->block_bytes);

	bzero(&reducer->groups, sizeof(reducer->groups));
	reducer->block = NULL;
	reducer->key_bytes = NULL;
	reducer->group_count = 0;
	reducer->group_capacity = 0;
	reducer->block_bytes = 0;
	reducer->key_bytes_used = 0;
	reducer->key_bytes_capacity = 0;
	reducer->rows_seen = 0;
	reducer->next_ordinal = 0;
	reducer->full = false;
	reducer->oversized = false;
	reducer->failed = false;
}

int
relic_reducer_open_limited(relic_t *ctx, const relic_reduced_source_t *source,
    uint16_t arity, size_t budget, relic_reducer_t **reducerp)
{
	if (open_reducer(ctx, source, arity, false, reducerp) != 0)
		return (-1);
	(*reducerp)->budget = budget;

	return (0);
}

void
relic_reducer_quiet(relic_reducer_t *reducer, bool report)
{
	reducer->report = report;
}

void
relic_reducer_reset(relic_t *ctx, relic_reducer_t *reducer)
{
	release_groups(ctx, reducer);
	relic_idtab_init(&reducer->table, true);
}

void
relic_reducer_clear(relic_t *ctx, relic_reducer_t *reducer)
{
	release_groups(ctx, reducer);
	relic_idtab_init(&reducer->table, true);
}

void
relic_reducer_set_ordinal(relic_reducer_t *reducer, uint64_t ordinal)
{
	reducer->next_ordinal = ordinal;
}

bool
relic_reducer_full(const relic_reducer_t *reducer)
{
	return (reducer->full);
}

bool
relic_reducer_oversized(const relic_reducer_t *reducer)
{
	return (reducer->oversized);
}

bool
relic_reducer_failed(const relic_reducer_t *reducer)
{
	return (reducer->failed);
}

size_t
relic_reducer_peak(const relic_reducer_t *reducer)
{
	return (reducer->peak);
}

uint64_t
relic_reducer_rows(const relic_reducer_t *reducer)
{
	return (reducer->rows_seen);
}

/*
 * Fold `src`'s groups into `dest`, as taking `src`'s rows after `dest`'s
 * would have, the earlier occurrence keeping a group's first row and a
 * tied extreme.  The parts must be of the same signatures and key.  A
 * combined row count past the source ceiling is a refusal dest does not
 * report, so the caller can run the same rows serially.
 */
int
relic_reducer_combine(relic_t *ctx, relic_reducer_t *dest,
    const relic_reducer_t *src)
{
	relic_seen_t key[RELIC_MAX_ARITY];
	relic_value_view_t dest_view;
	relic_value_view_t src_view;
	relic_extreme_t *dst_ex;
	const relic_extreme_t *src_ex;
	relic_fold_state_t *dst_state;
	const relic_held_t *held;
	uint64_t dest_rows;
	uint32_t hash;
	size_t group;
	size_t at;
	size_t g;
	size_t s;
	uint16_t k;

	assert(dest->signature_count == src->signature_count &&
	    dest->key_count == src->key_count && dest->arity == src->arity);

	if (src->rows_seen > RELIC_REDUCE_ROWS_MAX - dest->rows_seen) {
		dest->failed = true;
		return (-1);
	}

	for (g = 0; g < src->group_count; g++) {
		held = &src->groups.keys[g * src->key_count];
		for (k = 0; k < src->key_count; k++)
			see_held(ctx, src->key_bytes, &held[k], &key[k]);
		hash = src->groups.hashes[g];
		group = find_group(ctx, dest, hash, key);
		if (group == SIZE_MAX &&
		    add_group(ctx, dest, hash, key, &group) != 0)
			return (-1);

		dest_rows = dest->groups.rows[group];
		if (dest_rows == 0 ||
		    src->groups.first[g] < dest->groups.first[group])
			dest->groups.first[group] = src->groups.first[g];

		for (s = 0; s < dest->signature_count; s++) {
			at = group * dest->signature_count + s;
			dst_state = &dest->groups.states[at];
			dst_ex = &dest->groups.extremes[at];
			src_ex = &src->groups.extremes[g *
			    src->signature_count + s];
			held_view(ctx, dst_ex->bytes, &dst_ex->value,
			    &dest_view);
			held_view(ctx, src_ex->bytes, &src_ex->value,
			    &src_view);
			switch (relic_fold_combine(dest->signatures[s].fn,
			    dst_state, dest_rows, &dest_view,
			    &src->groups.states[g * src->signature_count + s],
			    src->groups.rows[g], &src_view)) {
			case RELIC_FOLD_REPLACED:
				if (copy_extreme(ctx, dest, dst_ex, src_ex) != 0)
					return (-1);
				break;
			case RELIC_FOLD_TIED:
				if (src_ex->ordinal < dst_ex->ordinal &&
				    copy_extreme(ctx, dest, dst_ex, src_ex) != 0)
					return (-1);
				break;
			case RELIC_FOLD_KEPT:
			case RELIC_FOLD_INVALIDATED:
				break;
			}
		}
		dest->groups.rows[group] = dest_rows + src->groups.rows[g];
	}
	dest->rows_seen += src->rows_seen;

	return (0);
}

/*
 * The value a builder holds, as take_row() sees a row's.
 */
static void
see_held(const relic_t *ctx, const char *bytes, const relic_held_t *held,
    relic_seen_t *seen)
{
	held_view(ctx, bytes, held, &seen->view);
	seen->dom = held->assigned ? held->u.dom : 0;
}

/*
 * Make `src`'s extreme `dst`'s, the text copied into `dst`'s buffer.
 */
static int
copy_extreme(relic_t *ctx, relic_reducer_t *reducer, relic_extreme_t *dst,
    const relic_extreme_t *src)
{
	relic_seen_t seen;

	see_held(ctx, src->bytes, &src->value, &seen);
	if (retain_extreme(ctx, reducer, dst, &seen) != 0)
		return (-1);
	dst->ordinal = src->ordinal;

	return (0);
}

/*
 * Lay the groups' arrays out for `capacity` groups from `memory`, or only
 * measure them when `memory` is NULL (relic_block_piece()), and return
 * their bytes, SIZE_MAX when a size cannot count them.
 */
static size_t
lay_out_groups(const relic_reducer_t *reducer, size_t capacity,
    void *memory, relic_groups_t *groups)
{
	size_t per_signature;
	size_t bytes;
	char *cursor;

	cursor = memory;
	bytes = 0;
	per_signature = relic_saturating_product(capacity,
	    reducer->signature_count);

	groups->keys = relic_block_piece(&cursor, &bytes,
	    relic_saturating_product(capacity, reducer->key_count),
	    sizeof(*groups->keys));
	groups->rows = relic_block_piece(&cursor, &bytes, capacity,
	    sizeof(*groups->rows));
	groups->first = relic_block_piece(&cursor, &bytes, capacity,
	    sizeof(*groups->first));
	groups->hashes = relic_block_piece(&cursor, &bytes, capacity,
	    sizeof(*groups->hashes));
	groups->states = relic_block_piece(&cursor, &bytes, per_signature,
	    sizeof(*groups->states));
	groups->extremes = relic_block_piece(&cursor, &bytes, per_signature,
	    sizeof(*groups->extremes));

	return (bytes);
}

/*
 * Give the groups room for one more, in a new block charged before it is
 * allocated, the old one given back once the groups are copied out of
 * it.
 */
static int
grow_groups(relic_t *ctx, relic_reducer_t *reducer)
{
	relic_groups_t grown;
	relic_groups_t *old;
	size_t capacity;
	size_t per_signature;
	size_t bytes;
	void *block;

	capacity = relic_grow_array_capacity(reducer->group_capacity,
	    reducer->group_count + 1);
	bytes = capacity == 0 ? SIZE_MAX :
	    lay_out_groups(reducer, capacity, NULL, &grown);
	if (bytes == SIZE_MAX) {
		report_no_memory(ctx, reducer);
		return (-1);
	}
	if (!charge(ctx, reducer, bytes))
		return (-1);

	block = malloc(bytes);
	if (block == NULL) {
		release(ctx, reducer, bytes);
		report_no_memory(ctx, reducer);
		return (-1);
	}
	(void)lay_out_groups(reducer, capacity, block, &grown);

	old = &reducer->groups;
	per_signature = reducer->group_count * reducer->signature_count;
	if (reducer->group_count > 0) {
		bcopy(old->keys, grown.keys, reducer->group_count *
		    reducer->key_count * sizeof(*grown.keys));
		bcopy(old->rows, grown.rows, reducer->group_count *
		    sizeof(*grown.rows));
		bcopy(old->first, grown.first, reducer->group_count *
		    sizeof(*grown.first));
		bcopy(old->hashes, grown.hashes, reducer->group_count *
		    sizeof(*grown.hashes));
		bcopy(old->states, grown.states, per_signature *
		    sizeof(*grown.states));
		bcopy(old->extremes, grown.extremes, per_signature *
		    sizeof(*grown.extremes));
	}

	free(reducer->block);
	release(ctx, reducer, reducer->block_bytes);
	reducer->block = block;
	reducer->block_bytes = bytes;
	reducer->groups = grown;
	reducer->group_capacity = capacity;

	return (0);
}

/*
 * Give the key bytes room for `length` more.
 */
static int
reserve_key_bytes(relic_t *ctx, relic_reducer_t *reducer, size_t length)
{
	size_t capacity;
	size_t needed;
	char *bytes;

	if (length <= reducer->key_bytes_capacity - reducer->key_bytes_used)
		return (0);

	needed = relic_saturating_sum(reducer->key_bytes_used, length);
	capacity = needed == SIZE_MAX ? 0 :
	    relic_grow_array_capacity(reducer->key_bytes_capacity, needed);
	if (capacity == 0) {
		report_no_memory(ctx, reducer);
		return (-1);
	}
	if (!charge(ctx, reducer, capacity - reducer->key_bytes_capacity))
		return (-1);

	bytes = realloc(reducer->key_bytes, capacity);
	if (bytes == NULL) {
		release(ctx, reducer, capacity - reducer->key_bytes_capacity);
		report_no_memory(ctx, reducer);
		return (-1);
	}

	reducer->key_bytes = bytes;
	reducer->key_bytes_capacity = capacity;

	return (0);
}

static uint32_t
rehash_group(uint32_t id, void *owner)
{
	const relic_reducer_t *reducer;

	reducer = owner;

	return (reducer->groups.hashes[id - 1]);
}

/*
 * The value a row's column holds: an identifier, which the context
 * assigned, or a pending marker, which names the batch's pending value
 * one less than it.  A pending string or symbol borrows the batch's bytes;
 * an empty one points at an addressable empty string instead, since the
 * batch's bytes may be none.  The executor that filled the batch made
 * every marker name one of its values and every text lie in its bytes.
 */
static void
see_value(const relic_t *ctx, const relic_reduce_batch_t *batch,
    relic_dom_t value, relic_seen_t *seen)
{
	const relic_pending_t *held;
	bool known RELIC_UNUSED;

	bzero(seen, sizeof(*seen));
	if ((value & RELIC_DOM_TYPE_MASK) != 0) {
		seen->dom = value;
		known = relic_value_view(ctx, value, &seen->view);
		assert(known);
		return;
	}

	assert(value >= 1 && value <= batch->pending_count);
	held = &batch->pending[value - 1];
	seen->view.bytes = "";
	switch (held->kind) {
	case RELIC_PENDING_INTEGER:
		seen->view.type = RELIC_INTEGER;
		seen->view.integer = held->u.integer;
		return;
	case RELIC_PENDING_TEXT:
		seen->view.type = RELIC_STRING;
		break;
	case RELIC_PENDING_SYMBOL:
		seen->view.type = RELIC_SYMBOL;
		break;
	}

	assert(held->u.text.offset <= batch->byte_count &&
	    held->u.text.length <= batch->byte_count - held->u.text.offset);
	seen->view.length = held->u.text.length;
	if (held->u.text.length > 0)
		seen->view.bytes = batch->bytes + held->u.text.offset;
}

/*
 * The hash of a key, from each value's type and contents, so that equal
 * values hash alike whether or not the context has assigned them.
 */
static uint32_t
key_hash(const relic_reducer_t *reducer, const relic_seen_t *key)
{
	uint32_t words[2 * RELIC_MAX_ARITY];
	const relic_value_view_t *view;
	uint16_t k;

	for (k = 0; k < reducer->key_count; k++) {
		view = &key[k].view;
		words[2 * k] = (uint32_t)view->type;
		words[2 * k + 1] = view->type == RELIC_INTEGER ?
		    relic_hash_int64(reducer->hash_seed, view->integer) :
		    relic_hash_bytes(reducer->hash_seed, view->bytes,
		    view->length);
	}

	return (relic_hash_words(reducer->hash_seed, words,
	    2 * (size_t)reducer->key_count));
}

/*
 * A view of a held value, whose text, if owned, is at `bytes` plus its
 * offset.
 */
static void
held_view(const relic_t *ctx, const char *bytes, const relic_held_t *held,
    relic_value_view_t *view)
{
	bool known RELIC_UNUSED;

	if (held->assigned) {
		known = relic_value_view(ctx, held->u.dom, view);
		assert(known);
		return;
	}

	bzero(view, sizeof(*view));
	view->type = held->type;
	view->bytes = "";
	if (held->type == RELIC_INTEGER) {
		view->integer = held->u.integer;
		return;
	}

	view->length = held->length;
	if (held->length > 0)
		view->bytes = bytes + held->u.offset;
}

static bool
views_equal(const relic_value_view_t *left, const relic_value_view_t *right)
{
	if (left->type != right->type)
		return (false);

	if (left->type == RELIC_INTEGER)
		return (left->integer == right->integer);

	return (left->length == right->length &&
	    bcmp(left->bytes, right->bytes, left->length) == 0);
}

/*
 * Whether group `group`'s key is `key`: two identifiers are the same value
 * exactly when they are equal, and any other pair is compared as values.
 */
static bool
key_matches(const relic_t *ctx, const relic_reducer_t *reducer,
    size_t group, const relic_seen_t *key)
{
	const relic_held_t *held;
	relic_value_view_t view;
	uint16_t k;

	held = &reducer->groups.keys[group * reducer->key_count];
	for (k = 0; k < reducer->key_count; k++) {
		if (held[k].assigned && key[k].dom != 0) {
			if (held[k].u.dom != key[k].dom)
				return (false);
			continue;
		}

		held_view(ctx, reducer->key_bytes, &held[k], &view);
		if (!views_equal(&view, &key[k].view))
			return (false);
	}

	return (true);
}

/*
 * The group whose key is `key`, of hash `hash`, or SIZE_MAX.
 */
static size_t
find_group(const relic_t *ctx, const relic_reducer_t *reducer,
    uint32_t hash, const relic_seen_t *key)
{
	relic_idprobe_t probe;
	uint32_t id;

	relic_idprobe_open(&probe, &reducer->table, hash);
	while (relic_idprobe_next(&probe, &id)) {
		if (reducer->groups.hashes[id - 1] == hash &&
		    key_matches(ctx, reducer, id - 1, key))
			return (id - 1);
	}

	return (SIZE_MAX);
}

/*
 * Hold a row's value: its identifier, or its contents, a text's bytes at
 * `offset` in whatever holds them.
 */
static void
hold(relic_held_t *held, const relic_seen_t *seen, size_t offset)
{
	bzero(held, sizeof(*held));
	held->type = seen->view.type;
	if (seen->dom != 0) {
		held->assigned = true;
		held->u.dom = seen->dom;
		return;
	}

	if (seen->view.type == RELIC_INTEGER) {
		held->u.integer = seen->view.integer;
		return;
	}

	held->u.offset = offset;
	held->length = seen->view.length;
}

/*
 * Make a group for `key`, of hash `hash`, with no rows yet, every room it
 * needs made before anything is written: the groups', the table's, and
 * the key bytes' for the key's texts the context has not assigned.
 */
static int
add_group(relic_t *ctx, relic_reducer_t *reducer, uint32_t hash,
    const relic_seen_t *key, size_t *groupp)
{
	relic_held_t *held;
	size_t text;
	size_t table_growth;
	size_t group;
	size_t s;
	uint16_t k;

	if (reducer->group_count == reducer->group_capacity &&
	    grow_groups(ctx, reducer) != 0)
		return (-1);

	if (!relic_idtab_has_room(&reducer->table, 1)) {
		size_t old_bytes;

		old_bytes = relic_idtab_bytes(&reducer->table);
		table_growth = relic_idtab_bytes_after_reserve(&reducer->table,
		    1) - old_bytes;
		if (reducer->budget != SIZE_MAX) {
			if (!charge(ctx, reducer,
			    relic_saturating_sum(table_growth, old_bytes)))
				return (-1);
		} else if (!charge(ctx, reducer, table_growth))
			return (-1);
		if (relic_idtab_reserve(&reducer->table, 1, rehash_group,
		    reducer) != 0) {
			release(ctx, reducer, reducer->budget != SIZE_MAX ?
			    relic_saturating_sum(table_growth, old_bytes) :
			    table_growth);
			report_no_memory(ctx, reducer);
			return (-1);
		}
		if (reducer->budget != SIZE_MAX)
			release(ctx, reducer, old_bytes);
	}

	text = 0;
	for (k = 0; k < reducer->key_count; k++) {
		if (key[k].dom == 0 && key[k].view.type != RELIC_INTEGER)
			text = relic_saturating_sum(text, key[k].view.length);
	}
	if (reserve_key_bytes(ctx, reducer, text) != 0)
		return (-1);

	group = reducer->group_count;
	held = &reducer->groups.keys[group * reducer->key_count];
	for (k = 0; k < reducer->key_count; k++) {
		hold(&held[k], &key[k], reducer->key_bytes_used);
		if (key[k].dom == 0 && key[k].view.length > 0) {
			bcopy(key[k].view.bytes, reducer->key_bytes +
			    reducer->key_bytes_used, key[k].view.length);
			reducer->key_bytes_used += key[k].view.length;
		}
	}
	reducer->groups.rows[group] = 0;
	reducer->groups.first[group] = reducer->next_ordinal;
	reducer->groups.hashes[group] = hash;
	for (s = 0; s < reducer->signature_count; s++) {
		relic_fold_init(&reducer->groups.states[
		    group * reducer->signature_count + s]);
		bzero(&reducer->groups.extremes[
		    group * reducer->signature_count + s],
		    sizeof(relic_extreme_t));
	}

	reducer->group_count++;
	relic_idtab_place(&reducer->table, hash, (uint32_t)reducer->group_count);
	*groupp = group;

	return (0);
}

/*
 * Make a row's value its group's extreme, at the row being taken: its
 * identifier, or its contents, an owned text copied into the extreme's
 * buffer, which grows only to the longest text it has held.
 */
static int
retain_extreme(relic_t *ctx, relic_reducer_t *reducer,
    relic_extreme_t *extreme, const relic_seen_t *seen)
{
	size_t capacity;
	char *bytes;

	if (seen->dom == 0 && seen->view.type != RELIC_INTEGER &&
	    seen->view.length > extreme->capacity) {
		capacity = relic_grow_array_capacity(extreme->capacity,
		    seen->view.length);
		if (capacity == 0) {
			report_no_memory(ctx, reducer);
			return (-1);
		}
		if (!charge(ctx, reducer, capacity - extreme->capacity))
			return (-1);
		bytes = realloc(extreme->bytes, capacity);
		if (bytes == NULL) {
			release(ctx, reducer, capacity - extreme->capacity);
			report_no_memory(ctx, reducer);
			return (-1);
		}
		extreme->bytes = bytes;
		extreme->capacity = capacity;
	}

	hold(&extreme->value, seen, 0);
	if (seen->dom == 0 && seen->view.length > 0)
		bcopy(seen->view.bytes, extreme->bytes, seen->view.length);
	extreme->ordinal = reducer->next_ordinal;

	return (0);
}

/*
 * Report that a source has more rows than a relation could hold, as the
 * relation store reports a relation it cannot grow.
 */
static void
refuse_rows(relic_t *ctx, const relic_relation_t *relation)
{
	char described[RELIC_PRED_TEXT_MAX];

	relic_diag_error(ctx, RELIC_ERROR_LIMIT, NULL,
	    "the relation of %s is too large to grow",
	    relic_relation_describe(ctx, relation, described,
	    sizeof(described)));
}

int
relic_reducer_take(relic_t *ctx, relic_reducer_t *reducer,
    const relic_relation_t *relation, const relic_reduce_batch_t *batch,
    size_t *takenp)
{
	size_t i;

	reducer->full = false;
	for (i = 0; i < batch->count; i++) {
		*takenp = i;
		if (reducer->rows_seen >= RELIC_REDUCE_ROWS_MAX) {
			if (reducer->budget == SIZE_MAX)
				refuse_rows(ctx, relation);
			else
				reducer->failed = true;
			return (-1);
		}
		if (take_row(ctx, reducer, batch,
		    &batch->rows[i * reducer->arity]) != 0) {
			if (reducer->full && !reducer->failed)
				return (0);
			return (-1);
		}
	}
	*takenp = batch->count;

	return (0);
}

/*
 * Reduce one row into the group of its key, made when it is the key's
 * first: its row count, and each signature's accumulator, a min's or a
 * max's extreme replaced when the row's value is the new one.
 */
static int
take_row(relic_t *ctx, relic_reducer_t *reducer,
    const relic_reduce_batch_t *batch, const relic_dom_t *row)
{
	relic_seen_t key[RELIC_MAX_ARITY];
	const relic_reduce_signature_t *signature;
	relic_value_view_t extreme_view;
	relic_extreme_t *extreme;
	relic_fold_state_t *state;
	relic_seen_t value;
	uint64_t rows;
	uint32_t hash;
	size_t group;
	size_t at;
	size_t s;
	uint16_t k;

	for (k = 0; k < reducer->key_count; k++)
		see_value(ctx, batch, row[reducer->key_columns[k]], &key[k]);
	hash = key_hash(reducer, key);
	group = find_group(ctx, reducer, hash, key);
	if (group == SIZE_MAX &&
	    add_group(ctx, reducer, hash, key, &group) != 0)
		return (-1);

	rows = reducer->groups.rows[group];
	for (s = 0; s < reducer->signature_count; s++) {
		signature = &reducer->signatures[s];
		at = group * reducer->signature_count + s;
		state = &reducer->groups.states[at];
		extreme = &reducer->groups.extremes[at];
		see_value(ctx, batch, row[signature->column], &value);
		held_view(ctx, extreme->bytes, &extreme->value, &extreme_view);

		switch (relic_fold_add(signature->fn, state, rows,
		    &extreme_view, &value.view)) {
		case RELIC_FOLD_REPLACED:
			if (retain_extreme(ctx, reducer, extreme, &value) != 0) {
				/*
				 * The accumulator has already changed.  A
				 * limited builder cannot leave the row
				 * for another wave: that would add it
				 * twice.  The caller runs the rows
				 * serially instead.
				 */
				if (reducer->budget != SIZE_MAX)
					reducer->failed = true;
				return (-1);
			}
			break;
		case RELIC_FOLD_KEPT:
		case RELIC_FOLD_TIED:
		case RELIC_FOLD_INVALIDATED:
			break;
		}
	}

	reducer->groups.rows[group] = rows + 1;
	reducer->next_ordinal++;
	reducer->rows_seen++;

	return (0);
}

/*
 * Look for the interrupt before step `step` of a walk finishing makes,
 * once every RELIC_REDUCE_CHUNK steps.
 */
static int
poll_walk(relic_t *ctx, const relic_relation_t *relation, size_t step)
{
	if (step == 0 || step % RELIC_REDUCE_CHUNK != 0)
		return (0);

	return (relic_refuse_interrupted(ctx, "deriving", relation));
}

/*
 * Count into *countp the values the groups retain that the context has
 * not assigned: every such key value, and the extreme of every min or max
 * whose group gives it a value.  An extreme the rows' types left with none
 * is never read, and is not retained.
 */
static int
count_retained(relic_t *ctx, const relic_reducer_t *reducer,
    const relic_relation_t *relation, size_t *countp)
{
	const relic_fold_state_t *state;
	const relic_extreme_t *extreme;
	size_t count;
	size_t at;
	size_t g;
	size_t s;
	uint16_t k;

	count = 0;
	for (g = 0; g < reducer->group_count; g++) {
		if (poll_walk(ctx, relation, g) != 0)
			return (-1);
		for (k = 0; k < reducer->key_count; k++) {
			if (!reducer->groups.keys[g * reducer->key_count +
			    k].assigned)
				count++;
		}
		for (s = 0; s < reducer->signature_count; s++) {
			at = g * reducer->signature_count + s;
			state = &reducer->groups.states[at];
			extreme = &reducer->groups.extremes[at];
			if ((reducer->signatures[s].fn == RELIC_AGG_MIN ||
			    reducer->signatures[s].fn == RELIC_AGG_MAX) &&
			    !state->invalid && !extreme->value.assigned)
				count++;
		}
	}
	*countp = count;

	return (0);
}

/*
 * List what count_retained() counts, each with where it first occurs: a
 * key value at its group's first row, an extreme at the row that holds
 * it, in the column each is of.
 */
static int
collect_retained(relic_t *ctx, relic_reducer_t *reducer,
    const relic_relation_t *relation, relic_retained_t *retained)
{
	relic_extreme_t *extreme;
	relic_held_t *held;
	size_t count;
	size_t at;
	size_t g;
	size_t s;
	uint16_t k;

	count = 0;
	for (g = 0; g < reducer->group_count; g++) {
		if (poll_walk(ctx, relation, g) != 0)
			return (-1);
		for (k = 0; k < reducer->key_count; k++) {
			held = &reducer->groups.keys[g * reducer->key_count +
			    k];
			if (held->assigned)
				continue;
			retained[count].held = held;
			retained[count].bytes = reducer->key_bytes;
			retained[count].ordinal = reducer->groups.first[g];
			retained[count].column = reducer->key_columns[k];
			count++;
		}
		for (s = 0; s < reducer->signature_count; s++) {
			at = g * reducer->signature_count + s;
			extreme = &reducer->groups.extremes[at];
			if ((reducer->signatures[s].fn != RELIC_AGG_MIN &&
			    reducer->signatures[s].fn != RELIC_AGG_MAX) ||
			    reducer->groups.states[at].invalid ||
			    extreme->value.assigned)
				continue;
			retained[count].held = &extreme->value;
			retained[count].bytes = extreme->bytes;
			retained[count].ordinal = extreme->ordinal;
			retained[count].column = reducer->signatures[s].column;
			count++;
		}
	}

	return (0);
}

static bool
retained_before(const relic_retained_t *left, const relic_retained_t *right)
{
	if (left->ordinal != right->ordinal)
		return (left->ordinal < right->ordinal);

	return (left->column < right->column);
}

/*
 * Sort the retained values by where they first occur, by a bottom-up merge
 * sort between `retained` and `spare`, each of `count`, stable so that the
 * order of two values of one row and column -- a key value and an extreme
 * of the same value -- is the order they were listed.  *sortedp is
 * whichever array holds the result.  Each pass writes every value once,
 * the interrupt looked for as it does.
 */
static int
sort_retained(relic_t *ctx, const relic_relation_t *relation,
    relic_retained_t *retained, relic_retained_t *spare, size_t count,
    relic_retained_t **sortedp)
{
	relic_retained_t *from;
	relic_retained_t *to;
	relic_retained_t *swap;
	size_t width;
	size_t start;
	size_t middle;
	size_t end;
	size_t left;
	size_t right;
	size_t out;

	from = retained;
	to = spare;
	for (width = 1; width < count; width *= 2) {
		for (start = 0; start < count; start += 2 * width) {
			middle = count - start < width ? count : start + width;
			end = count - middle < width ? count : middle + width;
			left = start;
			right = middle;
			for (out = start; out < end; out++) {
				if (poll_walk(ctx, relation, out) != 0)
					return (-1);
				if (left < middle && (right == end ||
				    !retained_before(&from[right],
				    &from[left])))
					to[out] = from[left++];
				else
					to[out] = from[right++];
			}
		}

		swap = from;
		from = to;
		to = swap;
	}

	*sortedp = from;

	return (0);
}

/*
 * Lay a summary's block out from `memory`, or only measure it when
 * `memory` is NULL, for the groups and signatures it records, and return
 * its bytes, SIZE_MAX when a size cannot count them.  The key table comes
 * last, sized for exactly the groups, and is placed once.
 */
static size_t
lay_out_summary(relic_summary_t *summary, void *memory)
{
	size_t per_signature;
	size_t table_bytes;
	size_t bytes;
	char *cursor;
	void *table;

	cursor = memory;
	bytes = 0;
	per_signature = relic_saturating_product(summary->group_count,
	    summary->signature_count);
	table_bytes = relic_idtab_bytes_for(true, summary->group_count);

	summary->keys = relic_block_piece(&cursor, &bytes,
	    relic_saturating_product(summary->group_count,
	    summary->key_count), sizeof(*summary->keys));
	summary->rows = relic_block_piece(&cursor, &bytes,
	    summary->group_count, sizeof(*summary->rows));
	summary->states = relic_block_piece(&cursor, &bytes, per_signature,
	    sizeof(*summary->states));
	summary->extremes = relic_block_piece(&cursor, &bytes, per_signature,
	    sizeof(*summary->extremes));
	table = relic_block_piece(&cursor, &bytes, table_bytes, 1);
	if (table != NULL && summary->group_count > 0)
		relic_idtab_init_given(&summary->table, true,
		    summary->group_count, table);

	return (table_bytes == SIZE_MAX ? SIZE_MAX : bytes);
}

/*
 * Make an empty summary of the builder's shape, its block allocated and
 * charged to the builder, which holds the charge, and so counts it in
 * its peak, until relic_reducer_finish() hands the summary over.
 */
static int
open_summary(relic_t *ctx, relic_reducer_t *reducer,
    relic_summary_t **summaryp)
{
	relic_summary_t *summary;
	size_t bytes;

	*summaryp = NULL;
	summary = calloc(1, sizeof(*summary));
	if (summary == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	summary->group_count = reducer->group_count;
	summary->signature_count = reducer->signature_count;
	summary->key_count = reducer->key_count;
	summary->hash_seed = reducer->hash_seed;
	relic_idtab_init(&summary->table, true);

	bytes = relic_saturating_sum(lay_out_summary(summary, NULL),
	    sizeof(*summary));
	if (bytes == SIZE_MAX) {
		free(summary);
		relic_diag_no_memory(ctx);
		return (-1);
	}
	if (!charge(ctx, reducer, bytes)) {
		free(summary);
		return (-1);
	}

	summary->block = malloc(bytes - sizeof(*summary));
	if (summary->block == NULL) {
		release(ctx, reducer, bytes);
		free(summary);
		relic_diag_no_memory(ctx);
		return (-1);
	}
	summary->bytes = bytes;
	(void)lay_out_summary(summary, summary->block);

	*summaryp = summary;

	return (0);
}

/*
 * Destroy a summary the builder still holds the charge of, one whose
 * finishing was refused.
 */
static void
discard_summary(relic_t *ctx, relic_reducer_t *reducer,
    relic_summary_t *summary)
{
	free(summary->block);
	release(ctx, reducer, summary->bytes);
	free(summary);
}

void
relic_summary_destroy(relic_t *ctx, relic_summary_t *summary)
{
	if (summary == NULL)
		return;

	free(summary->block);
	relic_account_release(&ctx->account, summary->bytes);
	free(summary);
}

typedef struct relic_group_order relic_group_order_t;

struct relic_group_order {
	uint64_t	first;
	size_t		group;
};

static int
compare_group_order(const void *left, const void *right)
{
	const relic_group_order_t *a = left;
	const relic_group_order_t *b = right;

	if (a->first < b->first)
		return (-1);
	if (a->first > b->first)
		return (1);
	if (a->group < b->group)
		return (-1);
	if (a->group > b->group)
		return (1);

	return (0);
}

/*
 * Copy the builder's groups into the summary in first-occurrence order,
 * every value they retain holding its identifier by now, and place each
 * group in the key table.
 */
static int
fill_summary(relic_t *ctx, relic_reducer_t *reducer,
    const relic_relation_t *relation, relic_summary_t *summary)
{
	relic_group_order_t *order;
	relic_group_order_t next;
	const relic_extreme_t *extreme;
	const relic_held_t *held;
	relic_dom_t *key;
	uint32_t hash;
	size_t at;
	size_t bytes;
	size_t g;
	size_t i;
	size_t j;
	size_t s;
	uint16_t k;

	bytes = relic_saturating_product(reducer->group_count, sizeof(*order));
	order = NULL;
	if (bytes != 0) {
		if (bytes == SIZE_MAX || !charge(ctx, reducer, bytes)) {
			if (bytes == SIZE_MAX)
				relic_diag_no_memory(ctx);
			return (-1);
		}
		order = malloc(bytes);
		if (order == NULL) {
			release(ctx, reducer, bytes);
			relic_diag_no_memory(ctx);
			return (-1);
		}
		for (g = 0; g < reducer->group_count; g++) {
			order[g].first = reducer->groups.first[g];
			order[g].group = g;
		}
		for (i = 1; i < reducer->group_count; i++) {
			if (poll_walk(ctx, relation, i) != 0) {
				free(order);
				release(ctx, reducer, bytes);
				return (-1);
			}
			next = order[i];
			j = i;
			while (j > 0 &&
			    compare_group_order(&order[j - 1], &next) > 0) {
				order[j] = order[j - 1];
				j--;
			}
			order[j] = next;
		}
	}

	for (i = 0; i < reducer->group_count; i++) {
		g = order[i].group;
		if (poll_walk(ctx, relation, i) != 0) {
			free(order);
			release(ctx, reducer, bytes);
			return (-1);
		}
		key = &summary->keys[i * summary->key_count];
		held = &reducer->groups.keys[g * reducer->key_count];
		for (k = 0; k < reducer->key_count; k++) {
			assert(held[k].assigned);
			key[k] = held[k].u.dom;
		}
		summary->rows[i] = reducer->groups.rows[g];

		for (s = 0; s < reducer->signature_count; s++) {
			extreme = &reducer->groups.extremes[
			    g * reducer->signature_count + s];
			at = i * summary->signature_count + s;
			summary->states[at] = reducer->groups.states[
			    g * reducer->signature_count + s];
			summary->extremes[at] = extreme->value.assigned ?
			    extreme->value.u.dom : 0;
		}

		hash = relic_hash_words(summary->hash_seed, key,
		    summary->key_count);
		relic_idtab_place(&summary->table, hash, (uint32_t)(i + 1));
	}

	free(order);
	if (bytes != 0)
		release(ctx, reducer, bytes);

	return (0);
}

/*
 * The summary is charged to the builder from when it is opened, so the
 * builder's peak counts it beside everything finishing holds, and its
 * charge passes to the summary only once it is whole: relic_summary_destroy()
 * gives it back from then.
 */
int
relic_reducer_finish(relic_t *ctx, relic_reducer_t *reducer,
    const relic_relation_t *relation, relic_summary_t **summaryp)
{
	relic_summary_t *summary;

	*summaryp = NULL;
	if (open_summary(ctx, reducer, &summary) != 0)
		return (-1);

	if (publish(ctx, reducer, relation) != 0) {
		discard_summary(ctx, reducer, summary);
		return (-1);
	}
	if (fill_summary(ctx, reducer, relation, summary) != 0) {
		discard_summary(ctx, reducer, summary);
		return (-1);
	}

	reducer->charged -= summary->bytes;
	*summaryp = summary;

	return (0);
}

/*
 * Give every retained value the context has not assigned its identifier,
 * interning them in the order they first occur, each value once however
 * many groups retain it.  The list and its sorting space are charged
 * while they are held.
 */
static int
publish(relic_t *ctx, relic_reducer_t *reducer,
    const relic_relation_t *relation)
{
	relic_retained_t *retained;
	relic_retained_t *sorted;
	size_t count;
	size_t bytes;
	int status;

	if (count_retained(ctx, reducer, relation, &count) != 0)
		return (-1);
	if (count == 0)
		return (0);

	bytes = relic_saturating_product(relic_saturating_product(count, 2),
	    sizeof(*retained));
	if (bytes == SIZE_MAX) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	if (!charge(ctx, reducer, bytes))
		return (-1);
	retained = malloc(bytes);
	if (retained == NULL) {
		release(ctx, reducer, bytes);
		relic_diag_no_memory(ctx);
		return (-1);
	}

	status = collect_retained(ctx, reducer, relation, retained);
	if (status == 0)
		status = sort_retained(ctx, relation, retained,
		    retained + count, count, &sorted);
	if (status == 0)
		status = intern_retained(ctx, reducer, relation, sorted,
		    count);

	free(retained);
	release(ctx, reducer, bytes);

	return (status);
}

/*
 * Intern the `count` retained values, sorted, RELIC_REDUCE_CHUNK at a time,
 * the interrupt looked for before each chunk.  A value an earlier chunk
 * interned a later chunk finds assigned, so each value is interned once,
 * in the order it first occurs, however many groups retain it.
 */
static int
intern_retained(relic_t *ctx, relic_reducer_t *reducer,
    const relic_relation_t *relation, relic_retained_t *retained,
    size_t count)
{
	size_t start;
	size_t chunk;

	for (start = 0; start < count; start += chunk) {
		if (relic_refuse_interrupted(ctx, "deriving", relation) != 0)
			return (-1);
		chunk = count - start < RELIC_REDUCE_CHUNK ? count - start :
		    RELIC_REDUCE_CHUNK;
		if (intern_chunk(ctx, reducer, relation, retained + start,
		    chunk) != 0)
			return (-1);
	}

	return (0);
}

/*
 * The text bytes of `count` retained values, SIZE_MAX when a size cannot
 * count them.
 */
static size_t
retained_text(const relic_retained_t *retained, size_t count)
{
	size_t text;
	size_t i;

	text = 0;
	for (i = 0; i < count; i++) {
		if (retained[i].held->type != RELIC_INTEGER)
			text = relic_saturating_sum(text,
			    retained[i].held->length);
	}

	return (text);
}

/*
 * Describe `count` retained values as a batch's pending values, each
 * text copied into `text`, retained_text() bytes, in order.
 */
static void
describe_retained(const relic_retained_t *retained, size_t count,
    relic_pending_t *pending, char *text)
{
	const relic_held_t *held;
	size_t used;
	size_t i;

	used = 0;
	for (i = 0; i < count; i++) {
		held = retained[i].held;
		bzero(&pending[i], sizeof(pending[i]));
		switch (held->type) {
		case RELIC_INTEGER:
			pending[i].kind = RELIC_PENDING_INTEGER;
			pending[i].u.integer = held->u.integer;
			continue;
		case RELIC_STRING:
			pending[i].kind = RELIC_PENDING_TEXT;
			break;
		case RELIC_SYMBOL:
			pending[i].kind = RELIC_PENDING_SYMBOL;
			break;
		}
		pending[i].u.text.offset = used;
		pending[i].u.text.length = held->length;
		if (held->length > 0)
			bcopy(retained[i].bytes + held->u.offset, text + used,
			    held->length);
		used += held->length;
	}
}

/*
 * Intern `count` retained values, at most RELIC_REDUCE_CHUNK, as one batch
 * of pending values (internal.h), in stages: the batch's storage -- its
 * pending values, their text, the probe and the map -- charged and
 * allocated; the values described into it; measured, which says the
 * identifier each will have and what the value table and the pool will
 * then hold; admitted by the memory limit as values derived into the
 * source's relation would be; and committed.  Only a commit that succeeded
 * gives the held values their identifiers.  Every path gives the storage
 * back.
 */
static int
intern_chunk(relic_t *ctx, relic_reducer_t *reducer,
    const relic_relation_t *relation, relic_retained_t *retained,
    size_t count)
{
	relic_pending_probe_t probe;
	relic_pending_map_t map;
	relic_pending_t *pending;
	size_t projected;
	size_t text;
	size_t room;
	size_t i;
	char *copy;
	int status;

	assert(count > 0 && count <= RELIC_REDUCE_CHUNK);
	text = retained_text(retained, count);
	room = relic_saturating_sum(relic_saturating_sum(
	    count * sizeof(*pending), text),
	    relic_saturating_sum(relic_pending_map_bytes(count),
	    relic_pending_probe_bytes(count)));
	if (room == SIZE_MAX) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	if (!charge(ctx, reducer, room))
		return (-1);

	status = -1;
	pending = malloc(count * sizeof(*pending));
	copy = malloc(text > 0 ? text : 1);
	if (pending == NULL || copy == NULL) {
		relic_diag_no_memory(ctx);
		goto free_batch;
	}
	if (relic_pending_probe_open(&probe, count) != 0) {
		relic_diag_no_memory(ctx);
		goto free_batch;
	}
	if (relic_pending_map_open(&map, count) != 0) {
		relic_pending_probe_close(&probe);
		relic_diag_no_memory(ctx);
		goto free_batch;
	}

	describe_retained(retained, count, pending, copy);
	projected = relic_pending_measure(ctx, pending, count, copy, &probe,
	    &map);
	relic_pending_probe_close(&probe);
	if (projected == SIZE_MAX) {
		relic_diag_no_memory(ctx);
		goto close_map;
	}
	if (relic_relation_admit_values(ctx, relation, projected) != 0)
		goto close_map;
	if (relic_pending_commit(ctx, &map) != 0)
		goto close_map;

	for (i = 0; i < count; i++) {
		retained[i].held->assigned = true;
		retained[i].held->u.dom = map.ids[i];
	}
	status = 0;

close_map:
	relic_pending_map_close(&map);
free_batch:
	free(copy);
	free(pending);
	release(ctx, reducer, room);

	return (status);
}

size_t
relic_summary_groups(const relic_summary_t *summary)
{
	return (summary->group_count);
}

const relic_dom_t *
relic_summary_key(const relic_summary_t *summary, size_t group)
{
	assert(group < summary->group_count);

	return (&summary->keys[group * summary->key_count]);
}

/*
 * A key value of zero, which a key slot holds for a value the context
 * never assigned, is no group's: every group's key holds identifiers.
 */
size_t
relic_summary_find(const relic_summary_t *summary, const relic_dom_t *key)
{
	relic_idprobe_t probe;
	const relic_dom_t *held;
	uint32_t hash;
	uint32_t id;
	uint16_t k;

	if (summary->group_count == 0)
		return (RELIC_SUMMARY_NONE);
	for (k = 0; k < summary->key_count; k++) {
		if (key[k] == 0)
			return (RELIC_SUMMARY_NONE);
	}

	hash = relic_hash_words(summary->hash_seed, key, summary->key_count);
	relic_idprobe_open(&probe, &summary->table, hash);
	while (relic_idprobe_next(&probe, &id)) {
		held = &summary->keys[(size_t)(id - 1) * summary->key_count];
		for (k = 0; k < summary->key_count; k++) {
			if (held[k] != key[k])
				break;
		}
		if (k == summary->key_count)
			return (id - 1);
	}

	return (RELIC_SUMMARY_NONE);
}

uint64_t
relic_summary_rows(const relic_summary_t *summary, size_t group)
{
	assert(group < summary->group_count);

	return (summary->rows[group]);
}

const relic_fold_state_t *
relic_summary_state(const relic_summary_t *summary, size_t group,
    size_t signature, relic_dom_t *extremep)
{
	size_t at;

	assert(group < summary->group_count &&
	    signature < summary->signature_count);
	at = group * summary->signature_count + signature;
	*extremep = summary->extremes[at];

	return (&summary->states[at]);
}

/*
 * The tuples are walked in identifier order, every family's builder taking
 * each run of RELIC_REDUCE_CHUNK of them in turn.  The summaries are made
 * into `summaries` and handed to the families together, so a refusal part
 * way through destroys those already made and leaves every family as it
 * was.
 */
int
relic_summarize_relation(relic_t *ctx, relic_arena_t *arena,
    const relic_relation_t *relation, relic_fold_family_t *const *families,
    size_t count)
{
	relic_stored_builder_t *builders;
	relic_summary_t **summaries;
	const relic_dom_t *tuple;
	relic_tuple_id_t first;
	relic_tuple_id_t last;
	relic_tuple_id_t end;
	relic_tuple_id_t id;
	size_t f;
	int status;

	builders = relic_arena_allocarray(arena, count, sizeof(*builders));
	summaries = relic_arena_allocarray(arena, count, sizeof(*summaries));
	if (builders == NULL || summaries == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	bzero(summaries, count * sizeof(*summaries));
	for (f = 0; f < count; f++) {
		assert(families[f]->summary == NULL &&
		    families[f]->arity == relation->arity);
		stored_open(ctx, &builders[f], families[f], relation);
	}

	status = 0;
	end = relic_relation_end(relation);
	for (first = 1; status == 0 && first < end; first = last) {
		status = relic_refuse_interrupted(ctx, "summarizing",
		    relation);
		last = end - first < RELIC_REDUCE_CHUNK ? end :
		    first + RELIC_REDUCE_CHUNK;
		for (f = 0; status == 0 && f < count; f++) {
			tuple = relic_relation_tuple(relation, first);
			for (id = first; status == 0 && id < last; id++) {
				status = take_stored(ctx, &builders[f], id,
				    tuple);
				tuple += relation->arity;
			}
		}
		if (status == 0)
			ctx->counters.read.tuples_scanned += last - first;
	}

	for (f = 0; status == 0 && f < count; f++)
		status = finish_stored(ctx, &builders[f], &summaries[f]);

	for (f = 0; f < count; f++) {
		close_stored(ctx, &builders[f]);
		if (status != 0) {
			relic_summary_destroy(ctx, summaries[f]);
			continue;
		}
		families[f]->summary = summaries[f];
		families[f]->built_at = relic_relation_count(relation);
	}
	if (status == 0)
		ctx->counters.summaries_built += count;

	return (status);
}

static void
stored_open(relic_t *ctx, relic_stored_builder_t *builder,
    const relic_fold_family_t *family, const relic_relation_t *relation)
{
	uint16_t column;

	bzero(builder, sizeof(*builder));
	builder->family = family;
	builder->relation = relation;
	builder->hash_seed = ctx->hash_seed;
	relic_idtab_init(&builder->table, true);
	for (column = 0; column < family->arity; column++) {
		if ((family->key_mask & (UINT32_C(1) << column)) != 0)
			builder->key_columns[builder->key_count++] = column;
	}
}

/*
 * Charge and give back `bytes` for the builder, as charge() and release()
 * do for a reducer.
 */
static bool
stored_charge(relic_t *ctx, relic_stored_builder_t *builder, size_t bytes)
{
	if (!relic_account_charge(&ctx->account, bytes)) {
		relic_diag_no_memory(ctx);
		return (false);
	}
	builder->charged += bytes;

	return (true);
}

static void
stored_release(relic_t *ctx, relic_stored_builder_t *builder, size_t bytes)
{
	assert(bytes <= builder->charged);
	relic_account_release(&ctx->account, bytes);
	builder->charged -= bytes;
}

/*
 * Lay out the arrays of `capacity` groups in `memory`, or only measure
 * them when it is NULL, and return their bytes, SIZE_MAX when a size
 * cannot count them.
 */
static size_t
lay_out_stored(relic_stored_builder_t *builder, size_t capacity,
    void *memory)
{
	size_t per_signature;
	size_t bytes;
	char *cursor;

	cursor = memory;
	bytes = 0;
	per_signature = relic_saturating_product(capacity,
	    builder->family->signature_count);
	builder->first = relic_block_piece(&cursor, &bytes, capacity,
	    sizeof(*builder->first));
	builder->rows = relic_block_piece(&cursor, &bytes, capacity,
	    sizeof(*builder->rows));
	builder->states = relic_block_piece(&cursor, &bytes, per_signature,
	    sizeof(*builder->states));
	builder->extremes = relic_block_piece(&cursor, &bytes, per_signature,
	    sizeof(*builder->extremes));

	return (bytes);
}

/*
 * Give the groups room for one more, in a new block charged before it is
 * allocated, the old one given back once the groups are copied out of it.
 */
static int
grow_stored(relic_t *ctx, relic_stored_builder_t *builder)
{
	relic_stored_builder_t grown;
	size_t per_signature;
	size_t capacity;
	size_t bytes;

	grown = *builder;
	capacity = relic_grow_array_capacity(builder->capacity,
	    builder->group_count + 1);
	bytes = capacity == 0 ? SIZE_MAX :
	    lay_out_stored(&grown, capacity, NULL);
	if (bytes == SIZE_MAX) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	if (!stored_charge(ctx, builder, bytes))
		return (-1);
	grown.block = malloc(bytes);
	if (grown.block == NULL) {
		stored_release(ctx, builder, bytes);
		relic_diag_no_memory(ctx);
		return (-1);
	}

	(void)lay_out_stored(&grown, capacity, grown.block);
	per_signature = builder->group_count *
	    builder->family->signature_count;
	if (builder->group_count > 0) {
		bcopy(builder->first, grown.first, builder->group_count *
		    sizeof(*grown.first));
		bcopy(builder->rows, grown.rows, builder->group_count *
		    sizeof(*grown.rows));
		bcopy(builder->states, grown.states, per_signature *
		    sizeof(*grown.states));
		bcopy(builder->extremes, grown.extremes, per_signature *
		    sizeof(*grown.extremes));
	}

	free(builder->block);
	stored_release(ctx, builder, builder->block_bytes);
	builder->first = grown.first;
	builder->rows = grown.rows;
	builder->states = grown.states;
	builder->extremes = grown.extremes;
	builder->block = grown.block;
	builder->block_bytes = bytes;
	builder->capacity = capacity;

	return (0);
}

/*
 * The hash of group `id`'s key, position plus one, for the table's growth.
 */
static uint32_t
rehash_stored(uint32_t id, void *owner)
{
	const relic_stored_builder_t *builder = owner;
	const relic_dom_t *tuple;
	relic_dom_t key[RELIC_MAX_ARITY];
	uint16_t k;

	tuple = relic_relation_tuple(builder->relation,
	    builder->first[id - 1]);
	for (k = 0; k < builder->key_count; k++)
		key[k] = tuple[builder->key_columns[k]];

	return (relic_hash_words(builder->hash_seed, key, builder->key_count));
}

/*
 * The position of the group of `key`, whose hash is `hash`, or SIZE_MAX
 * when no tuple taken so far has it.
 */
static size_t
find_stored(const relic_stored_builder_t *builder, uint32_t hash,
    const relic_dom_t *key)
{
	relic_idprobe_t probe;
	const relic_dom_t *tuple;
	uint32_t id;
	uint16_t k;

	relic_idprobe_open(&probe, &builder->table, hash);
	while (relic_idprobe_next(&probe, &id)) {
		tuple = relic_relation_tuple(builder->relation,
		    builder->first[id - 1]);
		for (k = 0; k < builder->key_count; k++) {
			if (tuple[builder->key_columns[k]] != key[k])
				break;
		}
		if (k == builder->key_count)
			return (id - 1);
	}

	return (SIZE_MAX);
}

/*
 * Make the group whose first tuple is `id`, with no rows, its position in
 * *groupp.  The table is reserved before the group is counted, so a
 * refusal leaves no group the table cannot find.
 */
static int
add_stored(relic_t *ctx, relic_stored_builder_t *builder, uint32_t hash,
    relic_tuple_id_t id, size_t *groupp)
{
	size_t table_growth;
	size_t group;
	size_t s;

	if (builder->group_count == builder->capacity &&
	    grow_stored(ctx, builder) != 0)
		return (-1);

	if (!relic_idtab_has_room(&builder->table, 1)) {
		table_growth = relic_idtab_bytes_after_reserve(&builder->table,
		    1) - relic_idtab_bytes(&builder->table);
		if (!stored_charge(ctx, builder, table_growth))
			return (-1);
		if (relic_idtab_reserve(&builder->table, 1, rehash_stored,
		    builder) != 0) {
			stored_release(ctx, builder, table_growth);
			relic_diag_no_memory(ctx);
			return (-1);
		}
	}

	group = builder->group_count++;
	builder->first[group] = id;
	builder->rows[group] = 0;
	for (s = 0; s < builder->family->signature_count; s++) {
		relic_fold_init(&builder->states[group *
		    builder->family->signature_count + s]);
		builder->extremes[group *
		    builder->family->signature_count + s] = 0;
	}
	relic_idtab_place(&builder->table, hash, (uint32_t)(group + 1));
	*groupp = group;

	return (0);
}

/*
 * Fold tuple `id` into its group, as fold_row() in lib/exec.c folds a
 * stored row, an extreme kept as the identifier of the value that
 * replaced it.  A group whose rows leave a signature outside its
 * function's domain keeps counting them, as a reducer's does, since its
 * row count is every count's.
 */
static int
take_stored(relic_t *ctx, relic_stored_builder_t *builder,
    relic_tuple_id_t id, const relic_dom_t *tuple)
{
	const relic_reduce_signature_t *signature;
	relic_value_view_t extreme;
	relic_value_view_t value;
	relic_dom_t key[RELIC_MAX_ARITY];
	uint64_t rows;
	uint32_t hash;
	size_t group;
	size_t at;
	size_t s;
	uint16_t k;
	bool known RELIC_UNUSED;

	for (k = 0; k < builder->key_count; k++)
		key[k] = tuple[builder->key_columns[k]];
	hash = relic_hash_words(builder->hash_seed, key, builder->key_count);
	group = find_stored(builder, hash, key);
	if (group == SIZE_MAX &&
	    add_stored(ctx, builder, hash, id, &group) != 0)
		return (-1);

	rows = builder->rows[group];
	for (s = 0; s < builder->family->signature_count; s++) {
		signature = &builder->family->signatures[s];
		at = group * builder->family->signature_count + s;
		known = relic_value_view(ctx, tuple[signature->column], &value);
		assert(known);
		bzero(&extreme, sizeof(extreme));
		if (builder->extremes[at] != 0) {
			known = relic_value_view(ctx, builder->extremes[at],
			    &extreme);
			assert(known);
		}
		if (relic_fold_add(signature->fn, &builder->states[at], rows,
		    &extreme, &value) == RELIC_FOLD_REPLACED)
			builder->extremes[at] = tuple[signature->column];
	}
	builder->rows[group] = rows + 1;

	return (0);
}

/*
 * Copy the groups into a summary laid out for exactly them, charged to
 * the builder until it is whole and then handed over with its charge.
 */
static int
finish_stored(relic_t *ctx, relic_stored_builder_t *builder,
    relic_summary_t **summaryp)
{
	relic_summary_t *summary;
	const relic_dom_t *tuple;
	relic_dom_t *key;
	size_t signature_count;
	size_t bytes;
	size_t g;
	uint32_t hash;
	uint16_t k;

	*summaryp = NULL;
	signature_count = builder->family->signature_count;
	summary = calloc(1, sizeof(*summary));
	if (summary == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	summary->group_count = builder->group_count;
	summary->signature_count = signature_count;
	summary->key_count = builder->key_count;
	summary->hash_seed = builder->hash_seed;
	relic_idtab_init(&summary->table, true);

	bytes = relic_saturating_sum(lay_out_summary(summary, NULL),
	    sizeof(*summary));
	if (bytes == SIZE_MAX) {
		free(summary);
		relic_diag_no_memory(ctx);
		return (-1);
	}
	if (!stored_charge(ctx, builder, bytes)) {
		free(summary);
		return (-1);
	}
	summary->block = malloc(bytes - sizeof(*summary));
	if (summary->block == NULL) {
		stored_release(ctx, builder, bytes);
		free(summary);
		relic_diag_no_memory(ctx);
		return (-1);
	}
	summary->bytes = bytes;
	(void)lay_out_summary(summary, summary->block);

	if (builder->group_count > 0) {
		bcopy(builder->rows, summary->rows, builder->group_count *
		    sizeof(*summary->rows));
		bcopy(builder->states, summary->states, builder->group_count *
		    signature_count * sizeof(*summary->states));
		bcopy(builder->extremes, summary->extremes,
		    builder->group_count * signature_count *
		    sizeof(*summary->extremes));
	}
	for (g = 0; g < builder->group_count; g++) {
		if (g > 0 && g % RELIC_REDUCE_CHUNK == 0 &&
		    relic_refuse_interrupted(ctx, "summarizing",
		    builder->relation) != 0) {
			discard_stored(ctx, builder, summary);
			return (-1);
		}
		tuple = relic_relation_tuple(builder->relation,
		    builder->first[g]);
		key = &summary->keys[g * summary->key_count];
		for (k = 0; k < summary->key_count; k++)
			key[k] = tuple[builder->key_columns[k]];
		hash = relic_hash_words(summary->hash_seed, key,
		    summary->key_count);
		relic_idtab_place(&summary->table, hash, (uint32_t)(g + 1));
	}

	builder->charged -= bytes;
	*summaryp = summary;

	return (0);
}

/*
 * Destroy a summary the builder still holds the charge of, one whose
 * finishing was refused.
 */
static void
discard_stored(relic_t *ctx, relic_stored_builder_t *builder,
    relic_summary_t *summary)
{
	free(summary->block);
	stored_release(ctx, builder, summary->bytes);
	free(summary);
}

/*
 * Give back everything the builder holds; a summary it finished is the
 * caller's.
 */
static void
close_stored(relic_t *ctx, relic_stored_builder_t *builder)
{
	free(builder->block);
	relic_idtab_reset(&builder->table);
	relic_account_release(&ctx->account, builder->charged);
	builder->charged = 0;
}
