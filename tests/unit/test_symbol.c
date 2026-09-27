/*
 * Copyright (c) 2026 Stanislav Sedov
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

#define	SCENARIO_EMBEDDED	"a NUL byte is part of the key"
#define	SCENARIO_EMPTY		"the empty string is an ordinary symbol"
#define	SCENARIO_HASH		"a symbol keeps the hash it was placed by"
#define	SCENARIO_IDENTITY	"equal bytes intern to one identifier"
#define	SCENARIO_LONG		"a symbol too long to record is refused"
#define	SCENARIO_LOOKUP		"an identifier names the bytes it was given"
#define	SCENARIO_MANY		"many distinct symbols stay distinct"
#define	SCENARIO_PACKED		"a symbol's bytes follow the previous one's"
#define	SCENARIO_UNKNOWN	"an unassigned identifier names nothing"

static void	 a_symbol_keeps_the_hash_it_was_placed_by(void);
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

	check(relic_symbol_intern(ctx, "", 0, &empty) == 0, SCENARIO_EMPTY,
	    "interning no bytes should succeed");
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
	    SCENARIO_UNKNOWN, "an identifier past the last should name nothing");
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

int
main(void)
{
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
