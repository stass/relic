/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for byte-string interning.
 *
 * Interning has to satisfy two properties exactly, because the evaluator
 * compares identifiers instead of bytes and has no way to recover from either
 * being wrong: equal bytes must always produce the same identifier, and
 * unequal bytes must never produce the same one.  A collision that merged two
 * symbols would not crash; it would quietly change which tuples a relation
 * contains.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_BATCH		"a batch leaves the pool as one at a time does"
#define	SCENARIO_EMBEDDED	"a NUL byte is part of the key"
#define	SCENARIO_EMPTY		"the empty string is an ordinary symbol"
#define	SCENARIO_HASH		"a symbol keeps the hash it was placed by"
#define	SCENARIO_IDENTITY	"equal bytes intern to one identifier"
#define	SCENARIO_LONG		"a symbol too long to record is refused"
#define	SCENARIO_LOOKUP		"an identifier names the bytes it was given"
#define	SCENARIO_MANY		"many distinct symbols stay distinct"
#define	SCENARIO_PACKED		"a symbol's bytes follow the previous one's"
#define	SCENARIO_UNKNOWN	"an unassigned identifier names nothing"

typedef struct batch_case batch_case_t;

static void	 a_batch_leaves_the_pool_as_one_at_a_time(void);
static void	 a_symbol_keeps_the_hash_it_was_placed_by(void);
static void	 batch_alike(const batch_case_t *c);
static void	 a_symbol_too_long_is_refused_unread(void);
static void	 an_unassigned_identifier_has_no_bytes(void);
static void	 embedded_nul_bytes_distinguish_symbols(void);
static void	 equal_bytes_intern_to_one_identifier(void);
static void	 many_symbols_receive_distinct_identifiers(void);
static void	 the_empty_string_interns_like_any_other(void);
static void	 the_pool_packs_its_bytes(void);

static void
equal_bytes_intern_to_one_identifier(void)
{
	relic_t *ctx;
	const char *bytes;
	size_t length;
	uint32_t first;
	uint32_t again;
	uint32_t other;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_IDENTITY, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_symbol_intern(ctx, "path", 4, &first) == 0,
	    SCENARIO_IDENTITY, "interning should succeed");
	check(relic_symbol_intern(ctx, "path", 4, &again) == 0,
	    SCENARIO_IDENTITY, "interning the same bytes should succeed");
	check(relic_symbol_intern(ctx, "edge", 4, &other) == 0,
	    SCENARIO_IDENTITY, "interning other bytes should succeed");

	check(first == again, SCENARIO_IDENTITY,
	    "equal bytes should return the identifier already assigned");
	check(first != other, SCENARIO_IDENTITY,
	    "unequal bytes should receive different identifiers");
	check(first != RELIC_ID_NONE, SCENARIO_IDENTITY,
	    "no symbol should be given the reserved identifier zero");
	check(relic_symbol_count(ctx) == 2, SCENARIO_IDENTITY,
	    "only the two distinct symbols should have been stored");

	/*
	 * A prefix of an interned symbol is a different symbol: the length is
	 * part of the key, not a hint about where the bytes end.
	 */
	check(relic_symbol_intern(ctx, "path", 3, &other) == 0,
	    SCENARIO_IDENTITY, "interning a prefix should succeed");
	check(first != other, SCENARIO_IDENTITY,
	    "a prefix should not be the same symbol as the whole");

	bytes = relic_symbol_bytes(ctx, first, &length);
	check(bytes != NULL && length == 4, SCENARIO_LOOKUP,
	    "the recorded length should be the length interned");
	check(bytes != NULL && bcmp(bytes, "path", 4) == 0, SCENARIO_LOOKUP,
	    "the recorded bytes should be the bytes interned");
	check(bytes != NULL && bytes[4] == '\0', SCENARIO_LOOKUP,
	    "the bytes should be terminated for a caller that wants a string");

	/*
	 * Nothing is asked for but the pointer, which a caller with a C
	 * string in hand would do.
	 */
	check(relic_symbol_bytes(ctx, first, NULL) == bytes, SCENARIO_LOOKUP,
	    "the length is optional");

	relic_destroy(ctx);
}

/*
 * A string literal may contain NUL bytes, so the length decides where a
 * symbol ends.  Two symbols that a C string comparison would call equal must
 * intern separately.
 */
static void
embedded_nul_bytes_distinguish_symbols(void)
{
	static const char with_nul[] = { 'a', '\0', 'b' };
	static const char truncated[] = { 'a' };

	relic_t *ctx;
	const char *bytes;
	size_t length;
	uint32_t embedded;
	uint32_t short_form;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_EMBEDDED, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_symbol_intern(ctx, with_nul, sizeof(with_nul),
	    &embedded) == 0, SCENARIO_EMBEDDED,
	    "interning bytes with a NUL should succeed");
	check(relic_symbol_intern(ctx, truncated, sizeof(truncated),
	    &short_form) == 0, SCENARIO_EMBEDDED,
	    "interning the leading byte alone should succeed");

	check(embedded != short_form, SCENARIO_EMBEDDED,
	    "bytes past a NUL should make a different symbol");

	bytes = relic_symbol_bytes(ctx, embedded, &length);
	check(bytes != NULL && length == sizeof(with_nul), SCENARIO_EMBEDDED,
	    "the full length should be preserved");
	check(bytes != NULL && bcmp(bytes, with_nul, sizeof(with_nul)) == 0,
	    SCENARIO_EMBEDDED, "the bytes should be preserved exactly");

	relic_destroy(ctx);
}

static void
the_empty_string_interns_like_any_other(void)
{
	relic_t *ctx;
	const char *bytes;
	size_t length;
	uint32_t empty;
	uint32_t again;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_EMPTY, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_symbol_intern(ctx, NULL, 0, &empty) == 0, SCENARIO_EMPTY,
	    "interning no bytes from no object should succeed");
	check(relic_symbol_intern(ctx, "", 0, &again) == 0, SCENARIO_EMPTY,
	    "interning no bytes again should succeed");
	check(empty == again, SCENARIO_EMPTY,
	    "the empty string should intern to one identifier");

	bytes = relic_symbol_bytes(ctx, empty, &length);
	check(bytes != NULL && length == 0, SCENARIO_EMPTY,
	    "its recorded length should be zero");
	check(bytes != NULL && bytes[0] == '\0', SCENARIO_EMPTY,
	    "and it should still be terminated");

	relic_destroy(ctx);
}

/*
 * Enough symbols that the identifier table doubles several times, checked
 * both ways: every symbol must still map to its own identifier, and every
 * identifier must still name the bytes it was created from.
 */
static void
many_symbols_receive_distinct_identifiers(void)
{
	enum { SYMBOL_COUNT = 4000 };

	relic_t *ctx;
	uint32_t ids[SYMBOL_COUNT];
	size_t first_uninterned;
	size_t first_misnumbered;
	size_t first_unrecoverable;
	size_t i;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_MANY, "a context should be created");
	if (ctx == NULL)
		return;

	first_uninterned = SYMBOL_COUNT;
	for (i = 0; i < SYMBOL_COUNT; i++) {
		char name[32];
		int written;

		written = snprintf(name, sizeof(name), "symbol_%zu", i);
		if (written < 0 || relic_symbol_intern(ctx, name,
		    (size_t)written, &ids[i]) != 0) {
			first_uninterned = i;
			break;
		}
	}

	checkf(first_uninterned == SYMBOL_COUNT, SCENARIO_MANY,
	    "symbol %zu of %d should have interned", first_uninterned,
	    SYMBOL_COUNT);
	check(relic_symbol_count(ctx) == SYMBOL_COUNT, SCENARIO_MANY,
	    "each should have been stored once");

	if (first_uninterned != SYMBOL_COUNT) {
		relic_destroy(ctx);
		return;
	}

	/*
	 * Identifiers are assigned in order of first appearance, so the nth
	 * symbol interned is identifier n.  Checking that rather than merely
	 * checking for duplicates also pins the reproducibility the golden
	 * tests rest on.
	 */
	first_misnumbered = SYMBOL_COUNT;
	first_unrecoverable = SYMBOL_COUNT;
	for (i = 0; i < SYMBOL_COUNT; i++) {
		char name[32];
		const char *bytes;
		size_t length;
		int written;

		if (ids[i] != (uint32_t)(i + 1) &&
		    first_misnumbered == SYMBOL_COUNT)
			first_misnumbered = i;

		written = snprintf(name, sizeof(name), "symbol_%zu", i);
		bytes = relic_symbol_bytes(ctx, ids[i], &length);
		if ((bytes == NULL || written < 0 ||
		    length != (size_t)written ||
		    bcmp(bytes, name, length) != 0) &&
		    first_unrecoverable == SYMBOL_COUNT)
			first_unrecoverable = i;
	}

	checkf(first_misnumbered == SYMBOL_COUNT, SCENARIO_MANY,
	    "symbol %zu should have been assigned identifier %zu in order of "
	    "first appearance", first_misnumbered, first_misnumbered + 1);
	checkf(first_unrecoverable == SYMBOL_COUNT, SCENARIO_MANY,
	    "identifier for symbol %zu should still name the bytes it was "
	    "given", first_unrecoverable);

	relic_destroy(ctx);
}

static void
an_unassigned_identifier_has_no_bytes(void)
{
	relic_t *ctx;
	uint32_t only;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_UNKNOWN, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_symbol_intern(ctx, "one", 3, &only) == 0,
	    SCENARIO_UNKNOWN, "interning should succeed");

	check(relic_symbol_bytes(ctx, RELIC_ID_NONE, NULL) == NULL,
	    SCENARIO_UNKNOWN, "identifier zero should name nothing");
	check(relic_symbol_bytes(ctx, only + 1, NULL) == NULL,
	    SCENARIO_UNKNOWN,
	    "an identifier past the last should name nothing");
	check(relic_string(ctx, only + 1, NULL) == NULL, SCENARIO_UNKNOWN,
	    "and the public accessor should agree");

	relic_destroy(ctx);
}

/*
 * The pool's table grows by the hash each entry records, reading no bytes,
 * so the recorded hash must be the one the bytes were placed by: a
 * different one would move the symbol where no lookup of its bytes looks.
 */
static void
a_symbol_keeps_the_hash_it_was_placed_by(void)
{
	static const char *const texts[] = { "", "a", "path", "a longer one" };

	enum { TEXT_COUNT = sizeof(texts) / sizeof(texts[0]) };

	const relic_symbol_t *entry;
	relic_t *ctx;
	uint32_t id;
	size_t i;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_HASH, "a context should be created");
	if (ctx == NULL)
		return;

	for (i = 0; i < TEXT_COUNT; i++) {
		if (relic_symbol_intern(ctx, texts[i], strlen(texts[i]),
		    &id) != 0) {
			checkf(false, SCENARIO_HASH, "\"%s\" should intern",
			    texts[i]);
			continue;
		}

		entry = &ctx->symbols.entries[id - 1];
		checkf(entry->hash == relic_hash_bytes(ctx->symbols.hash_seed,
		    texts[i], strlen(texts[i])), SCENARIO_HASH,
		    "\"%s\" should keep the hash of its bytes", texts[i]);
	}

	relic_destroy(ctx);
}

/*
 * An entry records a length of 32 bits, so a longer one is refused, and
 * before a byte is read: the length is the caller's, and a buffer that
 * long need not exist.  The buffer here holds one byte, and reading past
 * it is what AddressSanitizer would report.
 */
static void
a_symbol_too_long_is_refused_unread(void)
{
	static const char one[1] = { 'x' };
	relic_t *ctx;
	uint32_t id;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_LONG, "a context should be created");
	if (ctx == NULL)
		return;

	check(relic_symbol_intern(ctx, one, (size_t)UINT32_MAX, &id) != 0,
	    SCENARIO_LONG, "a symbol of 2^32 - 1 bytes should be refused");
	check(relic_error(ctx) != NULL && strstr(relic_error(ctx),
	    "is longer than") != NULL, SCENARIO_LONG,
	    "and the diagnostic should say it is too long");
	check(relic_symbol_count(ctx) == 0, SCENARIO_LONG,
	    "and nothing should have been interned");

	relic_destroy(ctx);
}

/*
 * The pool's bytes are packed: each symbol's bytes and terminator follow
 * the previous symbol's with no padding, whatever their lengths, while a
 * chunk has room for them.
 */
static void
the_pool_packs_its_bytes(void)
{
	static const char *const texts[] = { "a", "bcd", "efghijk", "", "lm" };

	enum { TEXT_COUNT = sizeof(texts) / sizeof(texts[0]) };

	const char *previous;
	const char *bytes;
	relic_t *ctx;
	uint32_t id;
	size_t i;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_PACKED, "a context should be created");
	if (ctx == NULL)
		return;

	previous = NULL;
	for (i = 0; i < TEXT_COUNT; i++) {
		if (relic_symbol_intern(ctx, texts[i], strlen(texts[i]),
		    &id) != 0) {
			checkf(false, SCENARIO_PACKED, "\"%s\" should intern",
			    texts[i]);
			break;
		}

		bytes = relic_symbol_bytes(ctx, id, NULL);
		checkf(previous == NULL || bytes == previous +
		    strlen(texts[i - 1]) + 1, SCENARIO_PACKED,
		    "\"%s\" should begin just past \"%s\" and its terminator",
		    texts[i], i > 0 ? texts[i - 1] : "");
		previous = bytes;
	}

	relic_destroy(ctx);
}

/*
 * A batch of new strings reserved and appended at once leaves the pool as
 * interning them one at a time does: the same identifiers, every stripe of
 * the index at the same capacity, and the bytes the projection promised.
 * That holds whether the stripes are placed on the calling thread or on a
 * pool, and whether they grow at once or in the parts that place them.  A
 * batch is shared from RELIC_SYMBOL_PARALLEL_COUNT strings, and its growth
 * deferred when it is reserved and appended in one call, as `one_call`
 * asks, and the memory limit admits the tables it replaces; `tight` sets
 * the limit to admit the batch and not them.  The stripes are chosen by an
 * unkeyed hash, so two contexts, whose seeds differ, fill their stripes
 * alike.  Both pools hold BATCH_HELD strings before the batch, so that
 * every stripe has a table to replace, and the batches are large enough
 * that every stripe grows several times.
 */
enum {
	BATCH_FULL = RELIC_SYMBOL_PARALLEL_COUNT + 1024,
	BATCH_HELD = 600,
	NAME_SIZE = 16
};

struct batch_case {
	const char	*how;
	size_t		 count;
	size_t		 threads;
	bool		 one_call;
	bool		 tight;
};

static void
a_batch_leaves_the_pool_as_one_at_a_time(void)
{
	static const batch_case_t cases[] = {
		{ "on the calling thread", BATCH_FULL, 1, true, false },
		{ "on the pool, growing at once", BATCH_FULL, 4, false,
		    false },
		{ "on the pool, growing in its parts", BATCH_FULL, 4, true,
		    false },
		{ "one string short of the pool",
		    RELIC_SYMBOL_PARALLEL_COUNT - 1, 4, true, false },
		{ "on the pool, the limit refusing the parts' growth",
		    BATCH_FULL, 4, true, true },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		batch_alike(&cases[i]);
}

static void
batch_alike(const batch_case_t *c)
{
	static relic_fresh_string_t fresh[BATCH_FULL];
	static char names[BATCH_FULL][NAME_SIZE];
	static uint32_t list[BATCH_FULL];
	relic_fresh_stripes_t stripes;
	relic_arena_mark_t copies;
	relic_account_t *account;
	relic_config_t config;
	relic_t *single;
	relic_t *batch;
	size_t projected;
	size_t first_unequal;
	size_t first_lost;
	size_t s;
	size_t i;
	uint32_t id;
	bool shares;
	int written;
	int added;

	bzero(&config, sizeof(config));
	config.threads = c->threads;
	single = relic_create();
	batch = relic_create_config(&config);
	checkf(single != NULL && batch != NULL, SCENARIO_BATCH,
	    "%s: two contexts should be created", c->how);
	if (single == NULL || batch == NULL)
		goto out;
	if (c->threads > 1 && relic_pool_create(&batch->pool, c->threads,
	    batch->hash_seed) != 0) {
		checkf(false, SCENARIO_BATCH, "%s: a pool should be created",
		    c->how);
		goto out;
	}

	for (i = 0; i < BATCH_HELD; i++) {
		written = snprintf(names[0], NAME_SIZE, "held%zu", i);
		if (written < 0 || relic_symbol_intern(single, names[0],
		    (size_t)written, &id) != 0 || relic_symbol_intern(batch,
		    names[0], (size_t)written, &id) != 0) {
			checkf(false, SCENARIO_BATCH, "%s: \"%s\" should "
			    "intern", c->how, names[0]);
			goto out;
		}
	}

	for (i = 0; i < c->count; i++) {
		written = snprintf(names[i], NAME_SIZE, "s%zu", i);
		if (written < 0 || relic_symbol_intern(single, names[i],
		    (size_t)written, &id) != 0) {
			checkf(false, SCENARIO_BATCH, "%s: \"%s\" should "
			    "intern", c->how, names[i]);
			goto out;
		}
		fresh[i].bytes = names[i];
		fresh[i].copy = NULL;
		fresh[i].length = (size_t)written;
		fresh[i].hash = relic_symbol_hash(batch, names[i],
		    (size_t)written);
	}

	stripes.list = list;
	relic_symbol_list(fresh, c->count, &stripes);
	projected = relic_symbol_pool_bytes_after(batch, fresh, c->count,
	    &stripes);
	if (c->tight) {
		account = &batch->account;
		account->limit = account->store_bytes + account->value_bytes +
		    account->pool_bytes + account->predtab_bytes +
		    account->arena_bytes - relic_symbol_pool_bytes(batch) +
		    projected;
	}
	if (c->one_call)
		added = relic_symbol_add(batch, fresh, c->count, &stripes);
	else {
		added = relic_symbol_reserve(batch, fresh, c->count, &stripes,
		    &copies);
		if (added == 0)
			relic_symbol_append(batch, fresh, c->count, &stripes);
	}
	if (added != 0) {
		checkf(false, SCENARIO_BATCH, "%s: the batch should be "
		    "added", c->how);
		goto out;
	}

	checkf(relic_symbol_pool_bytes(batch) == projected, SCENARIO_BATCH,
	    "%s: the batch should hold the bytes its projection promised",
	    c->how);
	checkf(relic_symbol_pool_bytes(batch) ==
	    relic_symbol_pool_bytes(single), SCENARIO_BATCH,
	    "%s: the batch should hold the bytes one at a time holds",
	    c->how);
	checkf(batch->account.pool_bytes == projected, SCENARIO_BATCH,
	    "%s: the account should count what the pool holds", c->how);
	first_unequal = RELIC_STRIPES;
	for (s = 0; s < RELIC_STRIPES; s++) {
		if (batch->symbols.index[s].capacity !=
		    single->symbols.index[s].capacity ||
		    batch->symbols.index[s].count !=
		    single->symbols.index[s].count ||
		    batch->symbols.grown[s].capacity != 0) {
			first_unequal = s;
			break;
		}
	}
	checkf(first_unequal == RELIC_STRIPES, SCENARIO_BATCH,
	    "%s: stripe %zu should be filled and sized as one at a time "
	    "fills it", c->how, first_unequal);

	first_lost = c->count;
	for (i = 0; i < c->count; i++) {
		if (relic_symbol_find(batch, names[i], fresh[i].length,
		    fresh[i].hash) != (uint32_t)(BATCH_HELD + i + 1)) {
			first_lost = i;
			break;
		}
	}
	checkf(first_lost == c->count, SCENARIO_BATCH,
	    "%s: \"%s\" should be found under identifier %zu", c->how,
	    first_lost < c->count ? names[first_lost] : "",
	    BATCH_HELD + first_lost + 1);

#ifdef RELIC_THREADS
	shares = c->threads > 1 && c->count >= RELIC_SYMBOL_PARALLEL_COUNT;
#else
	shares = false;
#endif
	checkf(batch->counters.symbols_shared == (shares ? 1 : 0),
	    SCENARIO_BATCH, "%s: the batch should %sbe placed on the pool",
	    c->how, shares ? "" : "not ");
	checkf(batch->counters.symbols_deferred ==
	    (shares && c->one_call && !c->tight ? 1 : 0), SCENARIO_BATCH,
	    "%s: the stripes should %sgrow in the parts placing them",
	    c->how, shares && c->one_call && !c->tight ? "" : "not ");

out:
	relic_destroy(single);
	relic_destroy(batch);
}

int
main(void)
{
	a_batch_leaves_the_pool_as_one_at_a_time();
	a_symbol_keeps_the_hash_it_was_placed_by();
	a_symbol_too_long_is_refused_unread();
	an_unassigned_identifier_has_no_bytes();
	embedded_nul_bytes_distinguish_symbols();
	equal_bytes_intern_to_one_identifier();
	many_symbols_receive_distinct_identifiers();
	the_empty_string_interns_like_any_other();
	the_pool_packs_its_bytes();

	return (check_summary("symbol"));
}
