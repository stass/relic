/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_REDUCE_H
#define RELIC_REDUCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <relic.h>

#include "arena.h"
#include "ast.h"
#include "exec.h"
#include "fold.h"
#include "frozen.h"
#include "program.h"

/*
 * Direct grouped reduction (specs/design-group-reduction.md): a query's
 * run that proves a source's rows distinct, and finds them read only by
 * folds of one key and by rules projecting that key, reduces the rows
 * into one state per group as its producer derives them, and stores none.
 *
 * A reduction has three parts with three owners.  The scope (lib/uses.c)
 * is the run's: which sources it reduces and which projections stand in
 * for their rules, chosen before the query's complete part is evaluated,
 * and what each became at its turn.  The builder (lib/reduce.c) is the
 * evaluation's while a source's producer runs: it takes the batches the
 * producer's EMIT fills, and nothing else can read it.  The summary is
 * what the builder becomes once the producer has finished: immutable, it
 * is read by folds through fold.h and by projections through the scope,
 * and lives until the scope ends.
 */

typedef struct relic_key_projection relic_key_projection_t;
typedef struct relic_reduce_batch relic_reduce_batch_t;
typedef struct relic_reduce_signature relic_reduce_signature_t;
typedef struct relic_reduced_source relic_reduced_source_t;
typedef struct relic_reducer relic_reducer_t;
typedef struct relic_reduction relic_reduction_t;

/*
 * One accumulator a source's folds need: the function, and the column it
 * folds, which is zero for a count, so that two counts of one source are
 * one signature.  Folds of the same signature read the same state.
 */
struct relic_reduce_signature {
	enum relic_aggregate_fn	fn;
	uint16_t		column;
};

/*
 * A batch a builder takes, laid out as lib/exec.h lays a producer's out:
 * `count` rows packed at the builder's arity in `rows`, whose pending
 * markers name the `pending_count` values of `pending`, a string's bytes
 * among the `byte_count` of `bytes`.
 */
struct relic_reduce_batch {
	const relic_dom_t	*rows;
	const relic_pending_t	*pending;
	const char		*bytes;
	size_t			 count;
	size_t			 pending_count;
	size_t			 byte_count;
};

/*
 * What a source the scope admitted became.  It is PROMISED until its
 * component's turn, when its producer's plan either passes the
 * distinctness certificate and is REDUCED, its summary then standing in
 * for its rows, or does not, and is evaluated as any other and STORED.
 * Neither way changes again within the run.
 */
enum relic_source_state {
	RELIC_SOURCE_PROMISED,
	RELIC_SOURCE_REDUCED,
	RELIC_SOURCE_STORED
};

/*
 * A source the scope admitted: predicate `pred`, folded on the key columns
 * of `key_mask`, with the `signature_count` signatures every fold over it
 * needs, in order of first use, and its summary once it is REDUCED.
 */
struct relic_reduced_source {
	relic_reduce_signature_t	*signatures;
	relic_summary_t			*summary;
	relic_pred_id_t			 pred;
	uint32_t			 key_mask;
	size_t				 signature_count;
	enum relic_source_state		 state;
};

/*
 * A rule the scope will replace with its source's keys: `pred`'s one rule,
 * whose head holds every key column of source `source`, an index into the
 * scope's sources, once.  Head column i holds key `keys[i]`, the position
 * of its source column among the key columns in increasing column order,
 * so a group's key read as relic_summary_key() gives it is permuted into
 * the head by it.
 */
struct relic_key_projection {
	const uint16_t	*keys;
	relic_pred_id_t	 pred;
	size_t		 source;
};

/*
 * A query run's scope: the `source_count` sources and the
 * `projection_count` projections it admitted, with `source_of` and
 * `projection_of`, indexed by predicate identifier minus one for the
 * `predicate_count` predicates that existed when it admitted them, holding
 * each one's position plus one, or zero.  Everything is in `arena`, which
 * is charged; the summaries are charged as they are built.
 */
struct relic_reduction {
	relic_arena_t		 arena;
	relic_reduced_source_t	*sources;
	relic_key_projection_t	*projections;
	uint32_t		*source_of;
	uint32_t		*projection_of;
	size_t			 source_count;
	size_t			 projection_count;
	size_t			 predicate_count;
};

/*
 * The scope's functions are lib/uses.c's, declared in internal.h beside
 * the Magic rewrite they work with.
 *
 * lib/reduce.c: the builder and the summary.
 *
 * relic_reducer_open() makes a builder for `source`, whose rows have
 * `arity` columns.  relic_reducer_take() reduces the rows of `batch` in
 * order, and sets *takenp to how many it reduced, every one unless it
 * refused the next.  relic_reducer_finish() makes the summary of every row
 * taken: it gives the values the groups retain that the context has not
 * assigned their identifiers, the memory limit asked first as it is asked
 * about values derived into `relation`, the source's.
 * relic_reducer_close() releases a builder, finished or not, and
 * relic_summary_destroy() a summary.  Each fallible function returns 0, or
 * -1 having reported the refusal -- memory, the memory limit, a source of
 * more rows than a relation could hold, which no limit describes, or an
 * interrupt -- after which the builder may only be closed.  A refused
 * finish may leave values interned, as a refused insertion may, but no
 * summary.  Every byte either holds is charged to the context's account
 * while it does, and relic_reducer_peak() is the most bytes the builder
 * and the summary it finishes have held at once.
 */
int	 relic_reducer_open(relic_t *ctx, const relic_reduced_source_t *source,
	    uint16_t arity, relic_reducer_t **reducerp);
int	 relic_reducer_take(relic_t *ctx, relic_reducer_t *reducer,
	    const relic_relation_t *relation, const relic_reduce_batch_t *batch,
	    size_t *takenp);
int	 relic_reducer_finish(relic_t *ctx, relic_reducer_t *reducer,
	    const relic_relation_t *relation, relic_summary_t **summaryp);
size_t	 relic_reducer_peak(const relic_reducer_t *reducer);
void	 relic_reducer_close(relic_t *ctx, relic_reducer_t *reducer);
void	 relic_summary_destroy(relic_t *ctx, relic_summary_t *summary);

#endif /* RELIC_REDUCE_H */
