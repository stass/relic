/*
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Direct checks of the serial cache's completed-prefix boundary. The cache
 * is private to eval.c; including that implementation keeps test access out
 * of the runtime API. This target links every other library object, while
 * test_eval exercises the shipped evaluator's actual execution lifecycle.
 * Only this translation unit replaces calloc, to refuse the optional bitmap.
 */
#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

static bool refuse_bitmap;
static size_t bitmap_requests;
static size_t validation_visits;

static void	*bitmap_calloc(size_t count, size_t size);
static int	 count_validation(relic_t *, const relic_destination_t *,
		    const relic_plan_t *, size_t, size_t);

#define	calloc	bitmap_calloc
#define relic_destination_begin_round count_validation
#include "eval.c"
#undef calloc
#undef relic_destination_begin_round

#define	SCENARIO	"serial positive evidence preserves insertion"

static relic_relation_t *fixture(relic_evaluation_t *eval,
		    relic_serial_seen_t *seen);
static void	 close_fixture(relic_evaluation_t *eval,
		    relic_serial_seen_t *seen);
static void	 positive_evidence_and_appended_values(void);
static void	 refusal_preserves_original_prefix(bool enabled);
static int	 ignore_row(void *argument, const relic_value_t *row,
		    size_t arity);
static void	 stream_and_memory_decline(void);
static void	 optional_allocation_refusal(void);

static int
count_validation(relic_t *ctx, const relic_destination_t *destination,
    const relic_plan_t *plan, size_t items, size_t targets)
{
	validation_visits++;
	return (relic_destination_begin_round(ctx, destination, plan, items, targets));
}

static void *
bitmap_calloc(size_t count, size_t size)
{
	bitmap_requests++;
	if (refuse_bitmap)
		return (NULL);
	return (calloc(count, size));
}

static relic_relation_t *
fixture(relic_evaluation_t *eval, relic_serial_seen_t *seen)
{
	relic_pred_id_t predicate;
	relic_emit_target_t target;
	relic_relation_t *relation;
	relic_destination_t destination;
	char text[65536];
	size_t length, i, j;
	int written;

	bzero(eval, sizeof(*eval));
	bzero(seen, sizeof(*seen));
	eval->ctx = relic_create();
	if (eval->ctx == NULL)
		goto failed;
	length = 0;
	for (i = 0; i < 64; i++) {
		for (j = 0; j < 64; j++) {
			written = snprintf(text + length, sizeof(text) - length,
			    "seed(%zu,%zu).\n", i, j);
			if (written < 0 || (size_t)written >= sizeof(text) - length)
				goto failed;
			length += (size_t)written;
		}
	}
	written = snprintf(text + length, sizeof(text) - length,
	    "v(shared). v(\"shared\"). p(X,Y) :- seed(X,Y).");
	if (written < 0 || (size_t)written >= sizeof(text) - length)
		goto failed;
	if (relic_load_string(eval->ctx, "seen.dl", text) != 0)
		goto failed;
	if (relic_materialize(eval->ctx) != 0)
		goto failed;
	if (relic_pred_intern(eval->ctx, "p", 1, 2, NULL, &predicate) != 0)
		goto failed;
	bzero(&target, sizeof(target));
	relation = relic_relation_read(eval->ctx, predicate);
	if (relation == NULL)
		goto failed;
	relic_destination_set(&destination, relation);
	target.destination = &destination;
	serial_seen_open(eval, &target, seen);
	if (seen->bits == NULL)
		goto failed;
	check(seen->bytes <= 4096, SCENARIO, "admit bounded positive bitmap");
	check(relic_atomic_size_load(&eval->ctx->account.round_bytes) ==
	    seen->bytes, SCENARIO, "charge bitmap capacity");
	return (target.destination->relation);

failed:
	check(false, SCENARIO, "create a derived grid and open its bitmap");
	close_fixture(eval, seen);
	return (NULL);
}

static void
close_fixture(relic_evaluation_t *eval, relic_serial_seen_t *seen)
{
	if (eval->ctx == NULL)
		return;
	free(seen->bits);
	relic_account_release_round(&eval->ctx->account, seen->bytes);
	check(relic_atomic_size_load(&eval->ctx->account.round_bytes) == 0, SCENARIO,
	    "release bitmap charge");
	relic_destroy(eval->ctx);
}

static void
positive_evidence_and_appended_values(void)
{
	relic_evaluation_t eval;
	relic_serial_seen_t seen;
	relic_relation_t *relation;
	relic_dom_t batch[12], integer, symbol, string, later;
	bool added[6];
	size_t completed, count;

	relation = fixture(&eval, &seen);
	if (relation == NULL)
		return;
	integer = relic_value_find_integer(eval.ctx, 0);
	string = relic_value_find_string(eval.ctx, "shared", 6);
	symbol = RELIC_DOM_SYMBOL | (string & RELIC_DOM_INDEX_MASK);
	count = relic_relation_count(relation);
	batch[0] = symbol;
	batch[1] = integer;
	eval.scratch.batch = batch;
	eval.scratch.batch_count = 1;
	check(serial_seen_insert(&eval, relation, &seen, added, &completed)
	    == 0 && completed == 1 && added[0], SCENARIO, "learn symbol before string");
	check(relic_relation_find(relation, batch) == count + 1, SCENARIO,
	    "symbol pair takes next tuple identifier");
	batch[0] = batch[1] = integer;
	batch[2] = symbol;
	batch[3] = integer;
	batch[4] = string;
	batch[5] = integer;
	batch[6] = symbol;
	batch[7] = integer;
	batch[8] = batch[9] = integer;
	batch[10] = string;
	batch[11] = integer;
	eval.scratch.batch = batch;
	eval.scratch.batch_count = 6;
	check(serial_seen_insert(&eval, relation, &seen, added, &completed)
	    == 0 && completed == 6, SCENARIO, "complete mixed batch");
	check(!added[0] && !added[1] && added[2] && !added[3] &&
	    !added[4] && !added[5], SCENARIO, "typed keys and within-batch duplicates");
	check(relic_relation_count(relation) == count + 2, SCENARIO,
	    "store only two distinct new typed pairs");
	check(relic_relation_find(relation, &batch[4]) == count + 2, SCENARIO,
	    "same-bytes string cannot hit symbol cache and takes next ID");
	check(serial_seen_insert(&eval, relation, &seen, added, &completed)
	    == 0 && completed == 6, SCENARIO, "complete all-hit batch");
	for (count = 0; count < 6; count++)
		check(!added[count], SCENARIO, "hits never report insertion");

	check(relic_value_intern_integer(eval.ctx, 9999, &later) == 0, SCENARIO,
	    "append value after coordinate snapshot");
	check(serial_seen_coordinate(&seen, later) == SIZE_MAX, SCENARIO,
	    "new pool position remains an ordinary miss");
	batch[0] = later;
	batch[1] = integer;
	eval.scratch.batch_count = 1;
	check(serial_seen_insert(&eval, relation, &seen, added, &completed)
	    == 0 && completed == 1 && added[0], SCENARIO, "insert out-of-span row");
	check(serial_seen_insert(&eval, relation, &seen, added, &completed)
	    == 0 && completed == 1 && !added[0], SCENARIO, "find out-of-span duplicate");
	check(relic_value_intern_bytes(eval.ctx, RELIC_STRING,
	    "later", 5, &later) == 0, SCENARIO, "append string after snapshot");
	check(serial_seen_coordinate(&seen, later) == SIZE_MAX, SCENARIO,
	    "new string position remains an ordinary miss");
	batch[0] = later;
	check(serial_seen_insert(&eval, relation, &seen, added, &completed)
	    == 0 && completed == 1 && added[0], SCENARIO, "insert new string pair");
	check(serial_seen_insert(&eval, relation, &seen, added, &completed)
	    == 0 && completed == 1 && !added[0], SCENARIO, "find new string duplicate");
	check(serial_seen_coordinate(&seen, 0) == SIZE_MAX, SCENARIO,
	    "zero domain is outside bitmap");
	close_fixture(&eval, &seen);
}

static void
refusal_preserves_original_prefix(bool enabled)
{
	relic_evaluation_t eval;
	relic_serial_seen_t seen;
	relic_relation_t *relation;
	relic_destination_t destination;
	relic_dom_t batch[10], integer, symbol, string;
	bool added[5];
	size_t completed, held;
	uint64_t before, inserted_before;

	relation = fixture(&eval, &seen);
	if (relation == NULL)
		return;
	integer = relic_value_find_integer(eval.ctx, 0);
	string = relic_value_find_string(eval.ctx, "shared", 6);
	symbol = RELIC_DOM_SYMBOL | (string & RELIC_DOM_INDEX_MASK);
	batch[0] = batch[1] = integer;
	eval.scratch.batch = batch;
	eval.scratch.batch_count = 1;
	check(serial_seen_insert(&eval, relation, &seen, added, &completed)
	    == 0 && completed == 1, SCENARIO, "learn an existing hit");

	if (!enabled) {
		free(seen.bits);
		relic_account_release_round(&eval.ctx->account, seen.bytes);
		bzero(&seen, sizeof(seen));
	}

	/* hit, successful miss, hit, refused miss, hit */
	batch[2] = symbol;
	batch[3] = symbol;
	batch[4] = batch[5] = integer;
	batch[6] = string;
	batch[7] = string;
	batch[8] = batch[9] = integer;
	eval.scratch.batch_count = 5;
	eval.ctx->config.max_tuples = eval.ctx->store.tuple_count + 1;
	held = relic_relation_count(relation);
	before = eval.ctx->counters.derivations;
	inserted_before = eval.ctx->counters.tuples_inserted;
	relic_destination_set(&destination, relation);
	check(insert_batch(&eval, &destination, &seen) == -1, SCENARIO,
	    "refuse second unknown row at tuple limit");
	check(eval.ctx->counters.derivations == before + 3, SCENARIO,
	    "count original prefix, excluding hits after refused row");
	check(relic_relation_count(relation) == held + 1, SCENARIO,
	    "publish only successful prefix");
	check(eval.ctx->counters.tuples_inserted == inserted_before + 1, SCENARIO,
	    "count only successful new insertion");
	check(relic_relation_find(relation, &batch[2]) == held + 1, SCENARIO,
	    "successful prefix keeps next tuple identifier");
	check(relic_relation_find(relation, &batch[6]) == RELIC_ID_NONE, SCENARIO,
	    "refused tuple remains absent");
	check(eval.scratch.batch_count == 0, SCENARIO, "empty refused batch");
	close_fixture(&eval, &seen);
}

static int
ignore_row(void *argument RELIC_UNUSED,
    const relic_value_t *row RELIC_UNUSED, size_t arity RELIC_UNUSED)
{
	return (0);
}

static void
stream_and_memory_decline(void)
{
	relic_evaluation_t eval;
	relic_serial_seen_t seen;
	relic_emit_target_t target;
	relic_relation_t *relation;
	relic_destination_t destination;

	bzero(&target, sizeof(target));
	relation = fixture(&eval, &seen);
	if (relation == NULL)
		return;
	relic_destination_set(&destination, relation);
	target.destination = &destination;
	free(seen.bits);
	relic_account_release_round(&eval.ctx->account, seen.bytes);
	target.rows.callback = ignore_row;
	serial_seen_open(&eval, &target, &seen);
	check(seen.bits == NULL && seen.bytes == 0, SCENARIO,
	    "streamed target allocates no cache");
	target.rows.callback = NULL;
	eval.ctx->account.limit = 1;
	serial_seen_open(&eval, &target, &seen);
	check(seen.bits == NULL && seen.bytes == 0, SCENARIO,
	    "finite memory limit allocates no cache");
	close_fixture(&eval, &seen);
}

static void
optional_allocation_refusal(void)
{
	relic_evaluation_t eval;
	relic_serial_seen_t seen;
	relic_emit_target_t target;
	relic_relation_t *relation;
	relic_destination_t destination;
	relic_dom_t tuple[2];
	size_t before;

	bzero(&target, sizeof(target));
	relation = fixture(&eval, &seen);
	if (relation == NULL)
		return;
	relic_destination_set(&destination, relation);
	target.destination = &destination;
	free(seen.bits);
	relic_account_release_round(&eval.ctx->account, seen.bytes);
	before = bitmap_requests;
	refuse_bitmap = true;
	serial_seen_open(&eval, &target, &seen);
	refuse_bitmap = false;
	check(bitmap_requests == before + 1, SCENARIO,
	    "refuse exactly the optional bitmap allocation");
	check(seen.bits == NULL && seen.bytes == 0 &&
	    relic_atomic_size_load(&eval.ctx->account.round_bytes) == 0,
	    SCENARIO, "allocation refusal leaves no bitmap or charge");
	check(relic_error_class(eval.ctx) == RELIC_ERROR_NONE, SCENARIO,
	    "optional refusal reports no error");
	tuple[0] = relic_value_find_string(eval.ctx, "shared", 6);
	tuple[1] = relic_value_find_integer(eval.ctx, 0);
	eval.scratch.batch = tuple;
	eval.scratch.batch_count = 1;
	check(insert_batch(&eval, target.destination, &seen) == 0, SCENARIO,
	    "ordinary insertion continues after optional refusal");
	check(relic_relation_count(target.destination->relation) == 4097, SCENARIO,
	    "ordinary insertion stores the new tuple");
	close_fixture(&eval, &seen);
}

/* Sparse delta rounds must cost their active items, not every component member. */
static void
round_admission_visits_only_active_items(void)
{
	relic_evaluation_t eval;
	relic_destination_t destinations[128];
	relic_destination_t *targets[128];
	relic_relation_t *relation;
	relic_round_item_t item;
	relic_round_t round;
	relic_rule_t seed;
	size_t i;

	bzero(&eval, sizeof(eval));
	eval.ctx = relic_create();
	if (eval.ctx == NULL) {
		check(false, "sparse round admission", "the context creates");
		return;
	}
	relation = relic_relation_create(eval.ctx, RELIC_ID_NONE, 0, 0);
	if (relation == NULL) {
		check(false, "sparse round admission", "the destination relation creates");
		relic_destroy(eval.ctx);
		return;
	}
	for (i = 0; i < 128; i++) {
		relic_destination_set(&destinations[i], relation);
		targets[i] = &destinations[i];
	}
	bzero(&seed, sizeof(seed));
	bzero(&item, sizeof(item));
	item.rule = &seed;
	bzero(&round, sizeof(round));
	round.items = &item;
	round.item_count = round.item_capacity = 1;
	round.targets = targets;
	round.target_count = 128;
	round.env = &eval.env;
	validation_visits = 0;
	check(run_round(&eval, &round) == 0 && relic_relation_count(relation) == 1,
	    "sparse round admission", "the active seed executes");
	check(validation_visits <= round.item_count, "sparse round admission",
	    "destination validation is bounded by active items, not 128 inactive members");
	relic_relation_clear(eval.ctx, relation);
	relic_relation_free(relation);
	relic_destroy(eval.ctx);
}

int
main(void)
{
	round_admission_visits_only_active_items();
	positive_evidence_and_appended_values();
	refusal_preserves_original_prefix(false);
	refusal_preserves_original_prefix(true);
	stream_and_memory_decline();
	optional_allocation_refusal();
	return (check_summary("test_serial_seen"));
}
