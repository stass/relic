/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
 * Every symbol and string value a context holds has its bytes interned to an
 * identifier here, and the evaluator compares identifiers rather than bytes.
 * Names of predicates, variables and sources are not values and are kept
 * elsewhere.  Two calls with equal bytes return the same identifier; two
 * calls with unequal bytes return different ones.  Length is part of the
 * key, so bytes that differ only in an embedded NUL are different symbols.
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
 * A batch of new strings being placed a stripe to a part: the strings of
 * `fresh`, listed by `stripes`, the one at position i numbered `first` + i.
 */
typedef struct {
	relic_symtab_t			*table;
	const relic_fresh_string_t	*fresh;
	const relic_fresh_stripes_t	*stripes;
	uint32_t			 first;
} placement_t;

static void	 account_pool(relic_t *ctx);
static void	 append_entry(relic_symtab_t *table, const char *copy,
		    size_t length, uint64_t hash);
static int	 copy_strings(relic_t *ctx, relic_fresh_string_t *fresh,
		    size_t count, const relic_arena_mark_t *copies);
static int	 grow_pool(relic_t *ctx, const relic_fresh_string_t *fresh,
		    size_t count, const relic_fresh_stripes_t *stripes,
		    bool may_defer);
static bool	 holds_prepared(const relic_symtab_t *table);
static int	 intern_growing(relic_t *ctx, const void *bytes,
		    size_t length, uint64_t hash);
static size_t	 index_bytes(const relic_symtab_t *table);
static size_t	 index_bytes_after(const relic_symtab_t *table,
		    const relic_fresh_stripes_t *stripes);
static size_t	 index_bytes_replaced(const relic_symtab_t *table,
		    const relic_fresh_stripes_t *stripes);
static int	 place_in_room(relic_symtab_t *table, const void *bytes,
		    size_t length, uint64_t hash);
static void	 place_part(void *argument, size_t stripe,
		    size_t worker_index);
static bool	 placement_shares(const relic_t *ctx, size_t count);
static bool	 pool_has_room(const relic_symtab_t *table,
		    const relic_fresh_string_t *fresh, size_t count,
		    const relic_fresh_stripes_t *stripes);
static int	 prepare_stripes(relic_symtab_t *table,
		    const relic_fresh_stripes_t *stripes);
static void	 release_prepared(relic_symtab_t *table);
static int	 reserve(relic_t *ctx, relic_fresh_string_t *fresh,
		    size_t count, const relic_fresh_stripes_t *stripes,
		    bool may_defer, relic_arena_mark_t *copiesp);
static int	 reserve_stripes(relic_symtab_t *table,
		    const relic_fresh_stripes_t *stripes);
static bool	 room_for_one(const relic_symtab_t *table, uint64_t hash,
		    size_t length);
static size_t	 stripe_added(const relic_fresh_stripes_t *stripes,
		    size_t stripe);
static const relic_symbol_t *symbol_at(const relic_symtab_t *table,
		    uint32_t id);
static bool	 symbol_has_bytes(const relic_symtab_t *table, uint32_t id,
		    const void *bytes, size_t length);
static uint32_t	 symbol_rehash(uint32_t id, void *ctx);

void
relic_symtab_init(relic_symtab_t *table, uint64_t hash_seed)
{
	size_t s;

	bzero(table, sizeof(*table));
	for (s = 0; s < RELIC_STRIPES; s++) {
		relic_idtab_init(&table->index[s], true);
		relic_idtab_init(&table->grown[s], true);
	}
	relic_arena_init(&table->strings, 1);
	table->hash_seed = hash_seed;
}

void
relic_symtab_reset(relic_symtab_t *table)
{
	size_t s;

	for (s = 0; s < RELIC_STRIPES; s++) {
		relic_idtab_reset(&table->index[s]);
		relic_idtab_reset(&table->grown[s]);
	}
	relic_arena_reset(&table->strings);
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
 * The hash of a symbol already interned, for its stripe of the index to
 * use when it grows: the keyed half it was placed under.
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
	uint64_t hash;
	uint32_t id;
	int status;

	table = &ctx->symbols;

	/*
	 * Checked before the bytes are read: the length is the caller's, and
	 * hashing them is the first thing that would read them.
	 */
	if (length > RELIC_STRING_LENGTH_MAX) {
		relic_symbol_refuse_length(ctx, NULL, length);
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
	 * relies on.  Most new symbols find the room the reservation would
	 * make already there, and are placed in it directly.
	 */
	if (relic_symbol_has_room(ctx, 1) &&
	    room_for_one(table, hash, length))
		status = place_in_room(table, bytes, length, hash);
	else
		status = intern_growing(ctx, bytes, length, hash);
	if (status != 0) {
		if (!relic_symbol_has_room(ctx, 1))
			relic_diag_error(ctx, RELIC_ERROR_LIMIT, NULL,
			    "too many distinct symbols and strings");
		else
			relic_diag_no_memory(ctx);
		return (-1);
	}

	*idp = (uint32_t)table->count;

	return (0);
}

/*
 * Whether the pool holds room for one more string of `length` bytes under
 * `hash` without growing anything: an entry, a slot in its stripe, and its
 * copy in the string arena's head chunk, as pool_has_room() says of a
 * batch.
 */
static bool
room_for_one(const relic_symtab_t *table, uint64_t hash, size_t length)
{
	relic_arena_projection_t strings;

	if (table->count == table->capacity ||
	    !relic_idtab_has_room(&table->index[relic_hash_stripe(hash)], 1))
		return (false);

	relic_arena_project(&table->strings, &strings);
	relic_arena_project_alloc(&strings, length + 1);

	return (strings.total_bytes ==
	    relic_arena_total_bytes(&table->strings));
}

/*
 * Copy, number and place a new string into room the pool already holds
 * for it: a free entry and a slot in its stripe.  Returns 0, or -1 when
 * the arena refused the copy, with no symbol added and the arena's failure
 * cleared, as a failed reservation leaves it.
 */
static int
place_in_room(relic_symtab_t *table, const void *bytes, size_t length,
    uint64_t hash)
{
	char *copy;

	copy = relic_arena_alloc(&table->strings, length + 1);
	if (copy == NULL) {
		relic_arena_clear_failure(&table->strings);
		return (-1);
	}
	if (length > 0)
		bcopy(bytes, copy, length);
	copy[length] = '\0';

	append_entry(table, copy, length, hash);
	relic_idtab_place(&table->index[relic_hash_stripe(hash)],
	    relic_hash_low(hash), (uint32_t)table->count);

	return (0);
}

/*
 * Intern a new string the pool must grow for, as a batch of one.  Returns
 * 0, or -1 having reported nothing, as relic_symbol_add() does.
 */
static int
intern_growing(relic_t *ctx, const void *bytes, size_t length, uint64_t hash)
{
	relic_fresh_stripes_t stripes;
	relic_fresh_string_t fresh;
	uint32_t position;

	fresh.bytes = bytes;
	fresh.copy = NULL;
	fresh.length = length;
	fresh.hash = hash;
	stripes.list = &position;
	relic_symbol_list(&fresh, 1, &stripes);

	return (relic_symbol_add(ctx, &fresh, 1, &stripes));
}

void
relic_symbol_refuse_length(relic_t *ctx, const relic_location_t *where,
    size_t length)
{
	relic_diag_error(ctx, RELIC_ERROR_REJECTED, where,
	    "a symbol or string of %zu bytes is longer than the %lu a value "
	    "may hold", length, (unsigned long)RELIC_STRING_LENGTH_MAX);
}

/*
 * The bytes of an interned symbol, or NULL for an identifier the table
 * never assigned.  The length, when asked for, excludes the terminator.
 */
const char *
relic_symtab_bytes(const relic_symtab_t *table, uint32_t id,
    size_t *lengthp)
{
	const relic_symbol_t *symbol;

	if (id == RELIC_ID_NONE || id > table->count)
		return (NULL);

	symbol = symbol_at(table, id);
	if (lengthp != NULL)
		*lengthp = symbol->length;

	return (symbol->bytes);
}

const char *
relic_symbol_bytes(const relic_t *ctx, uint32_t id, size_t *lengthp)
{
	return (relic_symtab_bytes(&ctx->symbols, id, lengthp));
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

uint64_t
relic_symbol_hash(const relic_t *ctx, const void *bytes, size_t length)
{
	return (relic_hash_bytes_wide(ctx->symbols.hash_seed, bytes, length));
}

uint32_t
relic_symtab_find(const relic_symtab_t *table, const void *bytes,
    size_t length, uint64_t hash)
{
	relic_idprobe_t probe;
	uint32_t id;

	relic_idprobe_open(&probe, &table->index[relic_hash_stripe(hash)],
	    relic_hash_low(hash));
	while (relic_idprobe_next(&probe, &id)) {
		if (symbol_has_bytes(table, id, bytes, length))
			return (id);
	}

	return (RELIC_ID_NONE);
}

uint32_t
relic_symbol_find(const relic_t *ctx, const void *bytes, size_t length,
    uint64_t hash)
{
	return (relic_symtab_find(&ctx->symbols, bytes, length, hash));
}

/*
 * A pool no context owns -- a collected result's -- charges nothing and
 * reports nothing, so its interning is the context's without the account:
 * the index and the entries are grown, then the bytes copied, and only
 * then is the symbol numbered and placed, so a failure at any step leaves
 * every symbol findable and none half-made.  The copy refused by the arena
 * clears the arena's failure, so that a later intern may try again.
 */
int
relic_symtab_intern(relic_symtab_t *table, const void *bytes, size_t length,
    uint32_t *idp)
{
	relic_symbol_t *entries;
	uint64_t hash;
	uint32_t id;

	if (length > RELIC_STRING_LENGTH_MAX)
		return (-1);

	hash = relic_hash_bytes_wide(table->hash_seed, bytes, length);
	id = relic_symtab_find(table, bytes, length, hash);
	if (id != RELIC_ID_NONE) {
		*idp = id;
		return (0);
	}

	if (table->count >= RELIC_SYMBOL_MAX ||
	    relic_idtab_reserve(&table->index[relic_hash_stripe(hash)], 1,
	    symbol_rehash, table) != 0)
		return (-1);

	entries = relic_grow_array_stepwise(table->entries, &table->capacity,
	    table->count, 1, 1, sizeof(*table->entries));
	if (entries == NULL)
		return (-1);
	table->entries = entries;

	if (place_in_room(table, bytes, length, hash) != 0)
		return (-1);
	*idp = (uint32_t)table->count;

	return (0);
}

size_t
relic_symbol_pool_bytes(const relic_t *ctx)
{
	const relic_symtab_t *table;

	table = &ctx->symbols;

	return (relic_saturating_sum(relic_saturating_sum(
	    relic_saturating_product(table->capacity, sizeof(*table->entries)),
	    index_bytes(table)),
	    relic_arena_total_bytes(&table->strings)));
}

/* The index's stripes, and the tables prepared for any to grow into. */
static size_t
index_bytes(const relic_symtab_t *table)
{
	size_t bytes;
	size_t s;

	bytes = 0;
	for (s = 0; s < RELIC_STRIPES; s++)
		bytes = relic_saturating_sum(bytes, relic_saturating_sum(
		    relic_idtab_bytes(&table->index[s]),
		    relic_idtab_bytes(&table->grown[s])));

	return (bytes);
}

/*
 * What the index occupies once each stripe has made room for its strings
 * of `stripes`, or SIZE_MAX when a stripe could not grow to hold them.
 */
static size_t
index_bytes_after(const relic_symtab_t *table,
    const relic_fresh_stripes_t *stripes)
{
	size_t capacity;
	size_t added;
	size_t bytes;
	size_t s;

	bytes = 0;
	for (s = 0; s < RELIC_STRIPES; s++) {
		added = stripe_added(stripes, s);
		if (!relic_idtab_capacity_after_reserve(&table->index[s], added,
		    &capacity))
			return (SIZE_MAX);
		bytes = relic_saturating_sum(bytes,
		    relic_idtab_bytes_after_reserve(&table->index[s], added));
	}

	return (bytes);
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
 * What the stripes that grow for the strings of `stripes` occupy now, which
 * a growth prepared for each holds beside its replacement until the stripe
 * is placed.
 */
static size_t
index_bytes_replaced(const relic_symtab_t *table,
    const relic_fresh_stripes_t *stripes)
{
	size_t bytes;
	size_t s;

	bytes = 0;
	for (s = 0; s < RELIC_STRIPES; s++)
		bytes = relic_saturating_sum(bytes,
		    relic_idtab_bytes_replaced_by_reserve(&table->index[s],
		    stripe_added(stripes, s)));

	return (bytes);
}

/*
 * Count the fresh strings into their stripes, then lay their positions out
 * stripe by stripe, each stripe's in increasing order.
 */
void
relic_symbol_list(const relic_fresh_string_t *fresh, size_t count,
    relic_fresh_stripes_t *stripes)
{
	size_t cursor[RELIC_STRIPES];
	size_t s;
	size_t i;

	assert(count <= UINT32_MAX);

	bzero(stripes->start, sizeof(stripes->start));
	for (i = 0; i < count; i++)
		stripes->start[relic_hash_stripe(fresh[i].hash) + 1]++;
	for (s = 0; s < RELIC_STRIPES; s++) {
		stripes->start[s + 1] += stripes->start[s];
		cursor[s] = stripes->start[s];
	}
	for (i = 0; i < count; i++)
		stripes->list[cursor[relic_hash_stripe(fresh[i].hash)]++] =
		    (uint32_t)i;
}

/* How many of the strings `stripes` lists are placed in `stripe`. */
static size_t
stripe_added(const relic_fresh_stripes_t *stripes, size_t stripe)
{
	return (stripes->start[stripe + 1] - stripes->start[stripe]);
}

/*
 * Each part of the pool is projected as interning the strings one at a
 * time would grow it: the entries a step at a time, each stripe of the
 * index for all of its strings, which reaches what reserving them one at a
 * time reaches, and the arena one copy at a time, each with its
 * terminator, as relic_symbol_reserve() makes it.
 */
size_t
relic_symbol_pool_bytes_after(const relic_t *ctx,
    const relic_fresh_string_t *fresh, size_t count,
    const relic_fresh_stripes_t *stripes)
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
	    index_bytes_after(table, stripes)), strings.total_bytes));
}

/*
 * Whether reserving the fresh strings would grow nothing: the entries and
 * every stripe of the index have room for them, and the string arena places
 * every copy in the room its head chunk has, which a projection of the
 * copies tells exactly, since it decides as the allocations would.
 */
static bool
pool_has_room(const relic_symtab_t *table, const relic_fresh_string_t *fresh,
    size_t count, const relic_fresh_stripes_t *stripes)
{
	relic_arena_projection_t strings;
	size_t s;
	size_t i;

	if (count > table->capacity - table->count)
		return (false);
	for (s = 0; s < RELIC_STRIPES; s++) {
		if (stripe_added(stripes, s) > 0 &&
		    !relic_idtab_has_room(&table->index[s],
		    stripe_added(stripes, s)))
			return (false);
	}

	relic_arena_project(&table->strings, &strings);
	for (i = 0; i < count; i++)
		relic_arena_project_alloc(&strings, fresh[i].length + 1);

	return (strings.total_bytes ==
	    relic_arena_total_bytes(&table->strings));
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
 * The pool grows each stripe of its index at once, so that nothing it
 * holds afterwards depends on what the caller does next.
 */
int
relic_symbol_reserve(relic_t *ctx, relic_fresh_string_t *fresh, size_t count,
    const relic_fresh_stripes_t *stripes, relic_arena_mark_t *copiesp)
{
	return (reserve(ctx, fresh, count, stripes, false, copiesp));
}

/*
 * The batch is reserved and appended in one call, so that the stripes'
 * growth may be deferred to the parts placing them: nothing can reserve,
 * intern or look up between the tables being prepared and the parts that
 * fill them.
 */
int
relic_symbol_add(relic_t *ctx, relic_fresh_string_t *fresh, size_t count,
    const relic_fresh_stripes_t *stripes)
{
	relic_arena_mark_t copies;

	if (reserve(ctx, fresh, count, stripes, true, &copies) != 0)
		return (-1);

	relic_symbol_append(ctx, fresh, count, stripes);

	return (0);
}

/*
 * Make room for the fresh strings and copy their bytes, for
 * relic_symbol_append() to number them; the stripes' growth is deferred to
 * the parts placing them when `may_defer` and grow_pool() allows it.
 * Returns 0, or -1 when the pool cannot take them: it holds as many symbols
 * as an identifier can name, the memory limit refuses its growth, recorded
 * on the account for the caller's relic_diag_no_memory() to report, or an
 * allocation failed.  A failure leaves no copy behind and no table
 * prepared, and whatever grew counted.
 *
 * A reservation the pool has room for grows nothing, and most find it, so
 * the limit is asked only about one that grows, and the account, which
 * nothing else changes, is reconciled only after one.
 */
static int
reserve(relic_t *ctx, relic_fresh_string_t *fresh, size_t count,
    const relic_fresh_stripes_t *stripes, bool may_defer,
    relic_arena_mark_t *copiesp)
{
	relic_symtab_t *table;

	table = &ctx->symbols;
	assert(!holds_prepared(table));
	relic_arena_mark(&table->strings, copiesp);
	if (count == 0)
		return (0);
	if (!relic_symbol_has_room(ctx, count))
		return (-1);

	if (pool_has_room(table, fresh, count, stripes))
		return (copy_strings(ctx, fresh, count, copiesp));

	if (grow_pool(ctx, fresh, count, stripes, may_defer) != 0) {
		account_pool(ctx);
		return (-1);
	}
	if (copy_strings(ctx, fresh, count, copiesp) != 0)
		return (-1);
	account_pool(ctx);

	return (0);
}

/*
 * Grow the pool's entries and its index for the fresh strings once the
 * memory limit admits the pool they make, as relic_symbol_pool_bytes_after()
 * projects it.  Each stripe that must grow does so at once, unless
 * `may_defer`, the placement shares, and the limit admits as well the
 * tables the growth replaces, which a deferred growth holds beside their
 * replacements until the parts placing the stripes complete it: then each
 * stripe's growth is only prepared.  Parts that all run on the calling
 * thread gain nothing from deferring, which would only hold every replaced
 * table at once.  Returns 0, or -1 with no table prepared and whatever grew
 * kept.
 */
static int
grow_pool(relic_t *ctx, const relic_fresh_string_t *fresh, size_t count,
    const relic_fresh_stripes_t *stripes, bool may_defer)
{
	relic_symtab_t *table;
	relic_symbol_t *entries;
	size_t replaced;
	size_t before;
	size_t after;
	bool defer;
	int status;

	table = &ctx->symbols;
	before = relic_symbol_pool_bytes(ctx);
	after = relic_symbol_pool_bytes_after(ctx, fresh, count, stripes);
	if (!relic_account_admit(&ctx->account, before, after))
		return (-1);

	replaced = index_bytes_replaced(table, stripes);
	defer = may_defer && placement_shares(ctx, count) &&
	    !relic_account_exceeds(&ctx->account, before,
	    relic_saturating_sum(after, replaced));
	if (defer)
		status = prepare_stripes(table, stripes);
	else
		status = reserve_stripes(table, stripes);
	if (status != 0)
		return (-1);

	entries = relic_grow_array_stepwise(table->entries, &table->capacity,
	    table->count, count, 1, sizeof(*table->entries));
	if (entries == NULL) {
		release_prepared(table);
		return (-1);
	}
	table->entries = entries;

	return (0);
}

/*
 * Copy each fresh string's bytes, with a terminator, into the string arena,
 * in the order the strings are numbered, which is the order interning them
 * one at a time would copy them in, so the arena ends as it would have.  A
 * copy the arena refuses rewinds it to `copies`, where the copies began, as
 * giving the reservation back does, so the arena places a retry's copies
 * exactly where an undisturbed reservation would.  The arena's failure is
 * cleared once the copies are rewound: the arena lives as long as the
 * context, and a failure left standing would refuse every symbol after.
 */
static int
copy_strings(relic_t *ctx, relic_fresh_string_t *fresh, size_t count,
    const relic_arena_mark_t *copies)
{
	relic_symtab_t *table;
	size_t i;

	table = &ctx->symbols;
	for (i = 0; i < count; i++) {
		assert(fresh[i].length <= RELIC_STRING_LENGTH_MAX);
		fresh[i].copy = relic_arena_alloc(&table->strings,
		    fresh[i].length + 1);
		if (fresh[i].copy == NULL) {
			relic_symbol_unreserve(ctx, fresh, i, copies);
			relic_arena_clear_failure(&table->strings);
			return (-1);
		}
		if (fresh[i].length > 0)
			bcopy(fresh[i].bytes, fresh[i].copy, fresh[i].length);
		fresh[i].copy[fresh[i].length] = '\0';
	}

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
	release_prepared(&ctx->symbols);
	forget_copies(fresh, count);
	relic_arena_rewind(&ctx->symbols.strings, copies);
	account_pool(ctx);
}

/*
 * Grow at once each stripe that must grow for its strings of `stripes`.
 */
static int
reserve_stripes(relic_symtab_t *table, const relic_fresh_stripes_t *stripes)
{
	size_t added;
	size_t s;

	for (s = 0; s < RELIC_STRIPES; s++) {
		added = stripe_added(stripes, s);
		if (added > 0 && relic_idtab_reserve(&table->index[s], added,
		    symbol_rehash, table) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Prepare for each stripe that must grow for its strings of `stripes` the
 * table it grows into, which the part placing the stripe moves it to.  A
 * failure releases every table prepared.
 */
static int
prepare_stripes(relic_symtab_t *table, const relic_fresh_stripes_t *stripes)
{
	size_t added;
	size_t s;

	for (s = 0; s < RELIC_STRIPES; s++) {
		added = stripe_added(stripes, s);
		if (relic_idtab_has_room(&table->index[s], added))
			continue;
		if (relic_idtab_prepare_growth(&table->index[s], added,
		    &table->grown[s]) != 0) {
			release_prepared(table);
			return (-1);
		}
	}

	return (0);
}

/*
 * Whether a stripe has a table prepared to grow into, which only a
 * relic_symbol_add() in progress holds.
 */
static bool
holds_prepared(const relic_symtab_t *table)
{
	size_t s;

	for (s = 0; s < RELIC_STRIPES; s++) {
		if (table->grown[s].capacity > 0)
			return (true);
	}

	return (false);
}

/* Free the tables prepared for stripes to grow into, which none has used. */
static void
release_prepared(relic_symtab_t *table)
{
	size_t s;

	for (s = 0; s < RELIC_STRIPES; s++)
		relic_idtab_reset(&table->grown[s]);
}

/*
 * Whether a batch of `count` new strings is placed on the context's pool,
 * as relic_symbol_append() is documented to decide.
 */
static bool
placement_shares(const relic_t *ctx, size_t count)
{
	if (count < 2 || ctx->pool == NULL ||
	    relic_pool_workers(ctx->pool) < 2)
		return (false);

	return ((ctx->config.flags & RELIC_F_PARALLEL_ALWAYS) != 0 ||
	    count >= RELIC_SYMBOL_PARALLEL_COUNT);
}

/*
 * The entries are appended in the order the strings are numbered, on the
 * calling thread, and then each stripe, a part of its own, places its
 * strings in that order too, so every stripe ends as interning them one at
 * a time would leave it.  The parts read the entries and write their own
 * stripes alone.  Only a deferred growth changes what the pool occupies
 * here, so the account is brought up to date, after one, once every part
 * has freed the table its stripe grew out of.
 */
void
relic_symbol_append(relic_t *ctx, const relic_fresh_string_t *fresh,
    size_t count, const relic_fresh_stripes_t *stripes)
{
	placement_t placement;
	relic_symtab_t *table;
	size_t s;
	size_t i;
	bool deferred;

	table = &ctx->symbols;
	deferred = holds_prepared(table);
	placement.table = table;
	placement.fresh = fresh;
	placement.stripes = stripes;
	placement.first = (uint32_t)table->count + 1;
	for (i = 0; i < count; i++)
		append_entry(table, fresh[i].copy, fresh[i].length,
		    fresh[i].hash);

	if (placement_shares(ctx, count)) {
		if (deferred)
			ctx->counters.symbols_deferred++;
		relic_pool_for(ctx->pool, RELIC_STRIPES, place_part,
		    &placement);
		ctx->counters.symbols_shared++;
	} else {
		for (s = 0; s < RELIC_STRIPES; s++) {
			if (stripe_added(stripes, s) > 0)
				place_part(&placement, s, 0);
		}
	}
	if (deferred)
		account_pool(ctx);
}

/*
 * Number a copied symbol the next identifier, into room the entries were
 * grown to hold.  Its stripe places it after.
 */
static void
append_entry(relic_symtab_t *table, const char *copy, size_t length,
    uint64_t hash)
{
	relic_symbol_t *entry;

	assert(table->count < table->capacity);
	entry = &table->entries[table->count];
	entry->bytes = copy;
	entry->length = (uint32_t)length;
	entry->hash = relic_hash_low(hash);
	table->count++;
}

/*
 * Place in `stripe` of the index the strings the placement lists there,
 * having first moved the stripe into the table prepared for it to grow
 * into, if one was.
 */
static void
place_part(void *argument, size_t stripe, size_t worker_index RELIC_UNUSED)
{
	const placement_t *placement;
	relic_symtab_t *table;
	uint32_t position;
	size_t i;

	placement = argument;
	table = placement->table;
	relic_idtab_complete_growth(&table->index[stripe],
	    &table->grown[stripe], symbol_rehash, table);
	for (i = placement->stripes->start[stripe];
	    i < placement->stripes->start[stripe + 1]; i++) {
		position = placement->stripes->list[i];
		relic_idtab_place(&table->index[stripe],
		    relic_hash_low(placement->fresh[position].hash),
		    placement->first + position);
	}
}
