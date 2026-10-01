/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdint.h>
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

static void	 account_pool(relic_t *ctx);
static const relic_symbol_t *symbol_at(const relic_symtab_t *table,
		    uint32_t id);
static bool	 symbol_has_bytes(const relic_symtab_t *table, uint32_t id,
		    const void *bytes, size_t length);
static bool	 pool_has_room(const relic_symtab_t *table,
		    const relic_fresh_string_t *fresh, size_t count);
static uint32_t	 symbol_rehash(uint32_t id, void *ctx);

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
	relic_fresh_string_t fresh;
	relic_arena_mark_t copies;
	uint32_t hash;
	uint32_t id;

	table = &ctx->symbols;

	/*
	 * Checked before the bytes are read: the length is the caller's, and
	 * hashing them is the first thing that would read them.
	 */
	if (length > RELIC_STRING_LENGTH_MAX) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "a symbol or string of %zu bytes is longer than the %lu "
		    "a value may hold", length,
		    (unsigned long)RELIC_STRING_LENGTH_MAX);
		return (-1);
	}

	hash = relic_symbol_hash(ctx, bytes, length);
	id = relic_symbol_find(ctx, bytes, length, hash);
	if (id != RELIC_ID_NONE) {
		*idp = id;
		return (0);
	}

	/*
	 * A new symbol is a reservation of one, as a batch's are of many, so
	 * the memory limit admits it and every growth happens before the
	 * symbol is numbered.  Storing first and indexing second would, on a
	 * failed index growth, leave a symbol no lookup could reach, and the
	 * next intern of the same bytes would assign it a second identifier --
	 * breaking, permanently and silently, the one property the evaluator
	 * relies on.
	 */
	fresh.bytes = bytes;
	fresh.copy = NULL;
	fresh.length = length;
	fresh.hash = hash;
	if (relic_symbol_reserve(ctx, &fresh, 1, &copies) != 0) {
		if (!relic_symbol_has_room(ctx, 1))
			relic_diagf(ctx, RELIC_ERROR, NULL,
			    "too many distinct symbols and strings");
		else
			relic_diag_no_memory(ctx);
		return (-1);
	}

	relic_symbol_append(ctx, &fresh, 1);
	*idp = (uint32_t)table->count;

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

uint32_t
relic_symbol_hash(const relic_t *ctx, const void *bytes, size_t length)
{
	return (relic_hash_bytes(ctx->symbols.hash_seed, bytes, length));
}

uint32_t
relic_symbol_find(const relic_t *ctx, const void *bytes, size_t length,
    uint32_t hash)
{
	relic_idprobe_t probe;
	uint32_t id;

	relic_idprobe_open(&probe, &ctx->symbols.index, hash);
	while (relic_idprobe_next(&probe, &id)) {
		if (symbol_has_bytes(&ctx->symbols, id, bytes, length))
			return (id);
	}

	return (RELIC_ID_NONE);
}

size_t
relic_symbol_pool_bytes(const relic_t *ctx)
{
	const relic_symtab_t *table;

	table = &ctx->symbols;

	return (relic_saturating_sum(relic_saturating_sum(
	    relic_saturating_product(table->capacity, sizeof(*table->entries)),
	    relic_idtab_bytes(&table->index)),
	    relic_arena_total_bytes(&table->strings)));
}

/*
 * Set the account's count of the pool to what the pool occupies, after
 * anything that may have grown it.  The count is reconciled from the pool
 * rather than adjusted by each growth, so it cannot drift from it.
 */
static void
account_pool(relic_t *ctx)
{
	ctx->account.pool_bytes = relic_symbol_pool_bytes(ctx);
}

/*
 * Each part of the pool is projected as interning the strings one at a
 * time would grow it: the entries a step at a time, the index for all of
 * them, which reaches what reserving one at a time reaches, and the arena
 * one copy at a time, each with its terminator, as relic_symbol_reserve()
 * makes it.
 */
size_t
relic_symbol_pool_bytes_after(const relic_t *ctx,
    const relic_fresh_string_t *fresh, size_t count)
{
	const relic_symtab_t *table;
	relic_arena_projection_t strings;
	size_t capacity;
	size_t i;

	table = &ctx->symbols;
	if (count == 0)
		return (relic_symbol_pool_bytes(ctx));

	capacity = relic_grow_array_capacity_stepwise(table->capacity,
	    table->count, count, 1);
	if (capacity == 0)
		return (SIZE_MAX);

	relic_arena_project(&table->strings, &strings);
	for (i = 0; i < count; i++)
		relic_arena_project_alloc(&strings, fresh[i].length + 1);

	return (relic_saturating_sum(relic_saturating_sum(
	    relic_saturating_product(capacity, sizeof(*table->entries)),
	    relic_idtab_bytes_after_reserve(&table->index, count)),
	    strings.total_bytes));
}

/*
 * Whether reserving the fresh strings would grow nothing: the entries and
 * the index have room for them, and the string arena places every copy in
 * the room its head chunk has, which a projection of the copies tells
 * exactly, since it decides as the allocations would.
 */
static bool
pool_has_room(const relic_symtab_t *table, const relic_fresh_string_t *fresh,
    size_t count)
{
	relic_arena_projection_t strings;
	size_t i;

	if (count > table->capacity - table->count ||
	    !relic_idtab_has_room(&table->index, count))
		return (false);

	relic_arena_project(&table->strings, &strings);
	for (i = 0; i < count; i++)
		relic_arena_project_alloc(&strings, fresh[i].length + 1);

	return (strings.total_bytes == relic_arena_total_bytes(&table->strings));
}

bool
relic_symbol_has_room(const relic_t *ctx, size_t count)
{
	return (count <= RELIC_SYMBOL_MAX - ctx->symbols.count);
}

/*
 * Clear the copies a reservation made before the arena is rewound past
 * them, so that no string holds a pointer into memory the arena has freed
 * or will hand out again.
 */
static void
forget_copies(relic_fresh_string_t *fresh, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		fresh[i].copy = NULL;
}

/*
 * This is the one place the pool grows, for one new symbol or a batch's, so
 * the memory limit is asked here, before anything grows, for the whole
 * pool as relic_symbol_pool_bytes_after() projects it; a refusal is recorded
 * on the account for the caller's relic_diag_no_memory() to report.  A
 * reservation the pool has room for grows nothing and adds nothing to the
 * total, and most find it, so the limit is asked only about one that grows.
 *
 * The index and the entries are grown first and the copies made after, in
 * the order the strings are numbered, which is the order interning them one
 * at a time would make them in, so the arena ends as it would have.  A copy
 * the arena refuses rewinds it to where the copies began, as giving the
 * reservation back does, so a reservation that fails or is given back
 * leaves no copy behind and the arena placing a retry's copies exactly
 * where an undisturbed reservation would.  The arena's failure is cleared
 * once the copies are rewound: the arena lives as long as the context, and
 * a failure left standing would refuse every symbol after this one.
 */
int
relic_symbol_reserve(relic_t *ctx, relic_fresh_string_t *fresh, size_t count,
    relic_arena_mark_t *copiesp)
{
	relic_symtab_t *table;
	relic_symbol_t *entries;
	size_t i;

	table = &ctx->symbols;
	relic_arena_mark(&table->strings, copiesp);
	if (count == 0)
		return (0);
	if (!relic_symbol_has_room(ctx, count))
		return (-1);
	if (!pool_has_room(table, fresh, count) &&
	    !relic_account_admit(&ctx->account, relic_symbol_pool_bytes(ctx),
	    relic_symbol_pool_bytes_after(ctx, fresh, count)))
		return (-1);

	if (relic_idtab_reserve(&table->index, count, symbol_rehash,
	    table) != 0) {
		account_pool(ctx);
		return (-1);
	}

	entries = relic_grow_array_stepwise(table->entries, &table->capacity,
	    table->count, count, 1, sizeof(*table->entries));
	if (entries == NULL) {
		account_pool(ctx);
		return (-1);
	}
	table->entries = entries;

	for (i = 0; i < count; i++) {
		assert(fresh[i].length <= RELIC_STRING_LENGTH_MAX);
		fresh[i].copy = relic_arena_alloc(&table->strings,
		    fresh[i].length + 1);
		if (fresh[i].copy == NULL) {
			relic_symbol_unreserve(ctx, fresh, i, copiesp);
			relic_arena_clear_failure(&table->strings);
			return (-1);
		}
		if (fresh[i].length > 0)
			bcopy(fresh[i].bytes, fresh[i].copy, fresh[i].length);
		fresh[i].copy[fresh[i].length] = '\0';
	}
	account_pool(ctx);

	return (0);
}

/*
 * Give back the copies the first `count` fresh strings were reserved, the
 * arena rewound to where they began.
 */
void
relic_symbol_unreserve(relic_t *ctx, relic_fresh_string_t *fresh,
    size_t count, const relic_arena_mark_t *copies)
{
	forget_copies(fresh, count);
	relic_arena_rewind(&ctx->symbols.strings, copies);
	account_pool(ctx);
}

void
relic_symbol_append(relic_t *ctx, const relic_fresh_string_t *fresh,
    size_t count)
{
	relic_symtab_t *table;
	relic_symbol_t *entry;
	size_t i;

	table = &ctx->symbols;
	for (i = 0; i < count; i++) {
		assert(table->count < table->capacity);
		entry = &table->entries[table->count];
		entry->bytes = fresh[i].copy;
		entry->length = (uint32_t)fresh[i].length;
		entry->hash = fresh[i].hash;
		table->count++;
		relic_idtab_place(&table->index, fresh[i].hash,
		    (uint32_t)table->count);
	}
}
