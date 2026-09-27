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
 * Byte strings interned to identifiers.
 *
 * Every predicate name, symbol and string literal in a program becomes an
 * identifier here, and the evaluator compares identifiers rather than bytes.
 * Two calls with equal bytes return the same identifier; two calls with
 * unequal bytes return different ones.  Length is part of the key, so bytes
 * that differ only in an embedded NUL are different symbols.
 *
 * Identifiers are assigned in order of first appearance and are never reused,
 * which is what makes a run reproducible.
 */

/*
 * A symbol identifier is the index of a symbol or string value's domain
 * identifier, below its two type bits, and zero is reserved to mean "none",
 * so the table holds RELIC_DOM_INDEX_MASK symbols.  A program that interned
 * a billion distinct strings would have exhausted memory long before, but
 * the ceiling is checked rather than assumed: the alternative is an
 * identifier that runs into the type bits and silently turns one value into
 * another.  A test build may define the ceiling lower, so that the refusal
 * can be reached, and never higher.
 */
#ifndef RELIC_SYMBOL_MAX
#define	RELIC_SYMBOL_MAX	RELIC_DOM_INDEX_MASK
#endif

_Static_assert(RELIC_SYMBOL_MAX <= RELIC_DOM_INDEX_MASK,
    "a symbol identifier must stay below the type bits");

/*
 * The longest byte string the pool holds: its length fits an entry's, and
 * the length with the terminator's byte fits a size_t however narrow.
 */
#define	RELIC_SYMBOL_LENGTH_MAX	(UINT32_MAX - 1)

static const relic_symbol_t *symbol_at(const relic_symtab_t *table,
		    uint32_t id);
static bool	 symbol_has_bytes(const relic_symtab_t *table, uint32_t id,
		    const void *bytes, size_t length);
static uint32_t	 symbol_rehash(uint32_t id, void *ctx);
static int	 symbol_store(relic_t *ctx, const void *bytes, size_t length,
		    uint32_t hash, uint32_t *idp);

void
relic_symtab_init(relic_symtab_t *table, uint64_t hash_seed)
{
	bzero(table, sizeof(*table));
	relic_idtab_init(&table->index, true);
	relic_arena_init(&table->strings, 1);
	table->hash_seed = hash_seed;
}

void
relic_symtab_reset(relic_symtab_t *table)
{
	relic_idtab_reset(&table->index);
	relic_arena_reset(&table->strings);
	relic_free_array(table->entries);
	bzero(table, sizeof(*table));
}

/*
 * The symbol an identifier names.  Identifier zero is reserved, so the array
 * is indexed one lower; this is the only place that translation is written.
 * The caller must have established that the identifier was assigned.
 */
static const relic_symbol_t *
symbol_at(const relic_symtab_t *table, uint32_t id)
{
	return (&table->entries[id - 1]);
}

/*
 * Whether the symbol with this identifier holds exactly these bytes.
 */
static bool
symbol_has_bytes(const relic_symtab_t *table, uint32_t id, const void *bytes,
    size_t length)
{
	const relic_symbol_t *symbol;

	symbol = symbol_at(table, id);
	if (symbol->length != length)
		return (false);

	/*
	 * Two symbols of no length are equal without either pointer being
	 * examined, which is what lets a caller intern the empty string
	 * without having an object to point at.
	 */
	if (length == 0)
		return (true);

	return (bcmp(symbol->bytes, bytes, length) == 0);
}

/*
 * The hash of a symbol already interned, for the identifier table to use
 * when it grows.
 */
static uint32_t
symbol_rehash(uint32_t id, void *ctx)
{
	const relic_symtab_t *table;
	const relic_symbol_t *symbol;

	table = ctx;
	symbol = symbol_at(table, id);

	return (symbol->hash);
}

/*
 * Copy bytes into the string arena, append them to the symbol array with
 * the hash they were placed by, and return the identifier assigned to them.
 * The length has been checked against RELIC_SYMBOL_LENGTH_MAX, so it fits
 * the entry and the terminator's byte cannot wrap the size.
 *
 * The array is grown and the copy is made before the count advances, so a
 * failure part way through leaves a table that is smaller than it might have
 * been but is entirely consistent: the entry a caller could reach is one that
 * was fully written.
 */
static int
symbol_store(relic_t *ctx, const void *bytes, size_t length, uint32_t hash,
    uint32_t *idp)
{
	relic_symtab_t *table;
	relic_symbol_t *entries;
	relic_symbol_t *entry;
	char *copy;

	table = &ctx->symbols;

	if (table->count >= RELIC_SYMBOL_MAX) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "too many distinct symbols and strings");
		return (-1);
	}

	entries = relic_grow_array(table->entries, &table->capacity,
	    table->count + 1, sizeof(*table->entries));
	if (entries == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	table->entries = entries;

	/*
	 * One byte past the recorded length holds a terminator, so a caller
	 * that has no use for the length can treat the result as a C string.
	 * A refused copy clears the arena's failure: the next symbol may fit
	 * where this one did not.
	 */
	copy = relic_arena_alloc(&table->strings, length + 1);
	if (copy == NULL) {
		relic_arena_clear_failure(&table->strings);
		relic_diag_no_memory(ctx);
		return (-1);
	}
	bcopy(bytes, copy, length);
	copy[length] = '\0';

	entry = &table->entries[table->count];
	entry->bytes = copy;
	entry->length = (uint32_t)length;
	entry->hash = hash;
	table->count++;

	*idp = (uint32_t)table->count;

	return (0);
}

/*
 * Intern bytes, storing the identifier in *idp.  Returns 0, or -1 with a
 * diagnostic already reported.
 *
 * A zero length is legitimate and interns the empty string, and `bytes` is
 * then not examined at all, so a caller with no object to point at may pass
 * NULL for it.
 */
int
relic_symbol_intern(relic_t *ctx, const void *bytes, size_t length,
    uint32_t *idp)
{
	relic_symtab_t *table;
	relic_idprobe_t probe;
	uint32_t hash;
	uint32_t id;

	table = &ctx->symbols;

	/*
	 * Checked before the bytes are read: the length is the caller's, and
	 * hashing them is the first thing that would read them.
	 */
	if (length > RELIC_SYMBOL_LENGTH_MAX) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "a symbol or string of %zu bytes is longer than the %lu "
		    "a value may hold", length,
		    (unsigned long)RELIC_SYMBOL_LENGTH_MAX);
		return (-1);
	}

	hash = relic_hash_bytes(table->hash_seed, bytes, length);

	relic_idprobe_open(&probe, &table->index, hash);
	while (relic_idprobe_next(&probe, &id)) {
		if (symbol_has_bytes(table, id, bytes, length)) {
			*idp = id;
			return (0);
		}
	}

	/*
	 * Room in the index is taken before the symbol is stored, so that the
	 * only step that can fail happens while there is still nothing to
	 * leave behind.  Storing first and indexing second would, on a failed
	 * index growth, leave a symbol no lookup could reach, and the next
	 * intern of the same bytes would assign it a second identifier --
	 * breaking, permanently and silently, the one property the evaluator
	 * relies on.
	 */
	if (relic_idtab_reserve(&table->index, 1, symbol_rehash, table) != 0) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	if (symbol_store(ctx, bytes, length, hash, &id) != 0)
		return (-1);

	relic_idtab_place(&table->index, hash, id);
	*idp = id;

	return (0);
}

/*
 * The bytes of an interned symbol, or NULL for an identifier this context
 * never assigned.  The length, when asked for, excludes the terminator.
 */
const char *
relic_symbol_bytes(const relic_t *ctx, uint32_t id, size_t *lengthp)
{
	const relic_symbol_t *symbol;

	if (id == RELIC_ID_NONE || id > ctx->symbols.count)
		return (NULL);

	symbol = symbol_at(&ctx->symbols, id);
	if (lengthp != NULL)
		*lengthp = symbol->length;

	return (symbol->bytes);
}

size_t
relic_symbol_count(const relic_t *ctx)
{
	return (ctx->symbols.count);
}

const char *
relic_string(relic_t *ctx, uint32_t id, size_t *lenp)
{
	return (relic_symbol_bytes(ctx, id, lenp));
}
