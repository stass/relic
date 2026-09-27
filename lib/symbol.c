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
 * Byte strings interned to 32-bit identifiers.
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
 * Identifiers are 32-bit and zero is reserved to mean "none", so the table
 * holds one fewer than a uint32_t can count.  A program that interned four
 * billion distinct strings would have exhausted memory long before, but the
 * ceiling is checked rather than assumed: the alternative is an identifier
 * that wraps to an existing one and silently merges two symbols.
 */
#define	RELIC_SYMBOL_MAX	(UINT32_MAX - 1u)

static const relic_symbol_t *symbol_at(const relic_symtab_t *table,
		    uint32_t id);
static bool	 symbol_has_bytes(const relic_symtab_t *table, uint32_t id,
		    const void *bytes, size_t length);
static uint32_t	 symbol_rehash(uint32_t id, void *ctx);
static int	 symbol_store(relic_t *ctx, const void *bytes, size_t length,
		    uint32_t *idp);

void
relic_symtab_init(relic_symtab_t *table, uint64_t hash_seed)
{
	bzero(table, sizeof(*table));
	relic_idtab_init(&table->index, true);
	table->hash_seed = hash_seed;
}

void
relic_symtab_reset(relic_symtab_t *table)
{
	/*
	 * The symbol bytes are not freed here.  They belong to the context
	 * arena, which releases them when it is reset.
	 */
	relic_idtab_reset(&table->index);
	free(table->entries);
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

	return (relic_hash_bytes(table->hash_seed, symbol->bytes,
	    symbol->length));
}

/*
 * Copy bytes into the context arena, append them to the symbol array and
 * return the identifier assigned to them.
 *
 * The array is grown and the copy is made before the count advances, so a
 * failure part way through leaves a table that is smaller than it might have
 * been but is entirely consistent: the entry a caller could reach is one that
 * was fully written.
 */
static int
symbol_store(relic_t *ctx, const void *bytes, size_t length, uint32_t *idp)
{
	relic_symtab_t *table;
	relic_symbol_t *entries;
	char *copy;

	table = &ctx->symbols;

	if (table->count >= RELIC_SYMBOL_MAX) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "too many distinct symbols and strings");
		return (-1);
	}

	/*
	 * The terminator below needs one byte beyond the length, and a length
	 * of SIZE_MAX would wrap that sum to nothing.  No such buffer can
	 * exist, but the arena would answer the wrapped request with a block
	 * of one alignment unit and the copy would run off the end of it.
	 */
	if (length == SIZE_MAX) {
		relic_diag_no_memory(ctx);
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
	 */
	copy = relic_context_alloc(ctx, length + 1);
	if (copy == NULL)
		return (-1);
	bcopy(bytes, copy, length);
	copy[length] = '\0';

	table->entries[table->count].bytes = copy;
	table->entries[table->count].length = length;
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

	if (symbol_store(ctx, bytes, length, &id) != 0)
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
