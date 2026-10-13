/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"

/*
 * Values interned to domain identifiers.
 *
 * A tuple column holds a relic_dom_t and nothing else, so a join, a hash and
 * a duplicate check are integer operations that never inspect a payload.
 * Only the comparison operators, sorting and output come back here.
 *
 * Every type is interned, integers included.  That is what makes the
 * representation uniform, and it is also why interning lives in this one
 * module rather than in the parser: the parser and relic_fact_add() cannot
 * disagree about a value's canonical form when there is only one path that
 * produces one.
 *
 * An identifier's top two bits are its type.  A symbol or a string is its
 * symbol identifier under that type: the pool gives equal bytes one
 * identifier, so it gives a value one identifier too, and interning one is a
 * probe into the pool and nothing else.  An integer is its position in the
 * integer table, which gives equal integers one position the same way.
 */

/*
 * An integer's position is the index below the type bits, and zero is
 * reserved, so the table holds RELIC_DOM_INDEX_MASK integers.  Checked
 * rather than assumed: a position that ran into the type bits would silently
 * turn one value into another.  A test build may define the ceiling lower,
 * so that the refusal can be reached, and never higher.
 */
#ifndef RELIC_INTEGER_MAX
#define	RELIC_INTEGER_MAX	RELIC_DOM_INDEX_MASK
#endif

_Static_assert(RELIC_INTEGER_MAX <= RELIC_DOM_INDEX_MASK,
    "an integer's position must stay below the type bits");

/*
 * The bits of a symbol's entry in `symbol_uses`: interned as a symbol value,
 * and interned as a string value.
 */
#define	RELIC_USED_AS_SYMBOL	0x1u
#define	RELIC_USED_AS_STRING	0x2u

/* Why bytes the language could not write as a symbol are refused as one. */
#define	RELIC_NOT_A_SYMBOL \
	"a symbol is a lowercase letter followed by letters, digits and " \
	"underscores; other bytes make a string"

static int	 compare_bytes(relic_bytes_of_fn bytes_of, const void *source,
		    uint32_t left, uint32_t right);
static int	 compare_integers(int64_t left, int64_t right);
static const char *context_bytes(const void *source, uint32_t id,
		    size_t *lengthp);
static bool	 value_held(const relic_t *ctx, const relic_value_t *value);
static void	 report_foreign(relic_t *ctx, const relic_value_t *foreign);
static void	 account_value_table(relic_t *ctx);
static void	 append_values(relic_valtab_t *table,
		    const relic_pending_map_t *map);
static int	 cover_symbol_uses(relic_t *ctx, uint32_t symbol);
static bool	 decode_integer(const relic_valtab_t *table, uint32_t position,
		    relic_value_t *value);
static bool	 decode_symbol_value(const relic_valtab_t *table,
		    uint32_t symbol, enum relic_type type,
		    relic_value_t *value);
static relic_dom_t dom_type_bits(enum relic_type type);
static int	 intern_bytes(relic_t *ctx, enum relic_type type,
		    const char *bytes, size_t length, relic_value_t *value);
static int	 intern_pending(relic_t *ctx, relic_pending_map_t *map);
static int	 intern_integer(relic_t *ctx, int64_t integer,
		    relic_dom_t *domp);
static int	 intern_symbol_value(relic_t *ctx, enum relic_type type,
		    uint32_t symbol, relic_dom_t *domp);
static uint32_t	 find_integer(const relic_valtab_t *table, int64_t integer,
		    uint32_t hash);
static size_t	 integer_table_bytes(const relic_valtab_t *table);
static uint32_t	 integer_rehash(uint32_t id, void *ctx);
static relic_dom_t find_assigned(const relic_t *ctx,
		    const relic_pending_batch_t *batch, size_t position);
static bool	 is_marker(relic_dom_t id);
static relic_dom_t marker_of(size_t position);
static size_t	 marker_position(relic_dom_t marker);
static relic_dom_t number_marked(const relic_t *ctx,
		    const relic_pending_batch_t *batch, size_t position,
		    relic_pending_map_t *map);
static relic_dom_t number_new(const relic_t *ctx,
		    const relic_pending_batch_t *batch, size_t position,
		    relic_pending_map_t *map);
static relic_dom_t pending_type_bits(const relic_pending_t *value);
static bool	 pending_equal(const relic_pending_batch_t *batch,
		    size_t a, size_t b);
static bool	 admit_value_table(relic_t *ctx, size_t fresh_integers,
		    uint32_t symbol);
static void	 append_integer(relic_valtab_t *table, int64_t integer,
		    uint32_t hash);
static size_t	 integer_table_bytes_after(const relic_valtab_t *table,
		    size_t fresh_integers);
static bool	 integer_table_has_room(const relic_valtab_t *table,
		    size_t count);
static bool	 integers_can_number(const relic_valtab_t *table,
		    size_t count);
static int	 reserve_integers(relic_t *ctx, size_t count);
static size_t	 value_table_bytes_after(const relic_valtab_t *table,
		    size_t fresh_integers, uint32_t symbol);
static void	 record_symbol_use(relic_valtab_t *table, uint32_t symbol,
		    enum relic_type type);
static uint8_t	 symbol_use_bit(enum relic_type type);
static bool	 symbol_used_as(const relic_valtab_t *table, uint32_t symbol,
		    enum relic_type type);
static size_t	 uses_capacity_covering(const relic_valtab_t *table,
		    uint32_t symbol);
static size_t	 value_table_bytes(const relic_valtab_t *table);
static int	 value_from_bytes(relic_t *ctx, enum relic_type type,
		    const char *bytes, size_t length, relic_value_t *value);
static unsigned int value_type_rank(enum relic_type type);
static bool	 value_well_formed(relic_t *ctx, const relic_value_t *value);

void
relic_valtab_init(relic_valtab_t *table, uint64_t hash_seed)
{
	bzero(table, sizeof(*table));
	relic_idtab_init(&table->index, true);
	table->hash_seed = hash_seed;
}

void
relic_valtab_reset(relic_valtab_t *table)
{
	relic_idtab_reset(&table->index);
	free(table->integers);
	free(table->symbol_uses);
	bzero(table, sizeof(*table));
}

/*
 * The type bits of a domain identifier of `type`.  The switch covers every
 * enumerator and has no default, so a type added later is reported here
 * rather than given the bits of another; a type outside the enumeration,
 * which value_well_formed() has already refused, gets none.
 */
static relic_dom_t
dom_type_bits(enum relic_type type)
{
	switch (type) {
	case RELIC_SYMBOL:
		return (RELIC_DOM_SYMBOL);
	case RELIC_STRING:
		return (RELIC_DOM_STRING);
	case RELIC_INTEGER:
		return (RELIC_DOM_INTEGER);
	}

	return (0);
}

/*
 * The bit of a symbol's `symbol_uses` entry for a value of `type`, which is
 * a symbol or a string.
 */
static uint8_t
symbol_use_bit(enum relic_type type)
{
	return (type == RELIC_SYMBOL ? RELIC_USED_AS_SYMBOL :
	    RELIC_USED_AS_STRING);
}

/*
 * Whether `symbol` has been interned as a value of `type`, which is what
 * makes the identifier of that value one the context assigned.
 */
static bool
symbol_used_as(const relic_valtab_t *table, uint32_t symbol,
    enum relic_type type)
{
	if (symbol == RELIC_ID_NONE || symbol > table->symbol_use_capacity)
		return (false);

	return ((table->symbol_uses[symbol - 1] & symbol_use_bit(type)) != 0);
}

static uint32_t
integer_rehash(uint32_t id, void *ctx)
{
	const relic_valtab_t *table;

	table = ctx;

	return (relic_hash_int64(table->hash_seed, table->integers[id - 1]));
}

/*
 * Whether a value a caller handed in is one this context can intern: a known
 * type, for a symbol or a string an identifier this context assigned, and for
 * a symbol bytes the language could write as one.
 *
 * An embedding program builds relic_value_t itself, so the identifier in one
 * may name a symbol from a different context or no symbol at all.  Catching
 * that here keeps a bad identifier out of every domain identifier, where
 * later stages would read through it.  The same caller could name a string's
 * bytes as a symbol, which would print as text that reads back as something
 * else, so every value interned is held to the lexer's rule here, once.
 */
static bool
value_well_formed(relic_t *ctx, const relic_value_t *value)
{
	const char *bytes;
	size_t length;

	switch (value->type) {
	case RELIC_SYMBOL:
	case RELIC_STRING:
		bytes = relic_symbol_bytes(ctx, value->v.id, &length);
		if (bytes == NULL && (value->v.id & RELIC_RESULT_ID_BIT) != 0) {
			relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
			    "value belongs to a collected result; build it "
			    "from its bytes with relic_value_symbol() or "
			    "relic_value_string()");
			return (false);
		}
		if (bytes == NULL) {
			relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
			    "value refers to symbol %lu, which this context "
			    "has not interned",
			    (unsigned long)value->v.id);
			return (false);
		}
		if (value->type == RELIC_SYMBOL &&
		    !relic_lex_is_symbol(bytes, length)) {
			relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
			    RELIC_NOT_A_SYMBOL);
			return (false);
		}
		return (true);
	case RELIC_INTEGER:
		return (true);
	}

	/*
	 * A type that is not an enumerator can only come from a caller that
	 * built the value itself, and reading it is already undefined, so
	 * this cannot be a guarantee.  It is cheap and it catches the mistake
	 * on the compilers anyone will use, which is worth more than the
	 * three lines it costs.
	 */
	relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
	    "value has unknown type %d", (int)value->type);

	return (false);
}

/*
 * Make the record of uses cover `symbol`, grown as uses_capacity_covering()
 * projects: by the steps covering one more symbol at a time would take, so
 * that the record reaches one capacity however its symbols arrive, one
 * interning at a time or a batch's at once.  The entries a growth adds are
 * cleared, since a symbol whose entry held a stray bit would decode as a
 * value no one made.  Returns 0, or -1 with nothing reported when the memory
 * limit refused the growth or it could not happen, leaving the record as it
 * was.
 */
static int
cover_symbol_uses(relic_t *ctx, uint32_t symbol)
{
	relic_valtab_t *table;
	size_t old_capacity;
	uint8_t *uses;

	table = &ctx->values;
	if (symbol <= table->symbol_use_capacity)
		return (0);

	if (!admit_value_table(ctx, 0, symbol))
		return (-1);

	old_capacity = table->symbol_use_capacity;
	uses = relic_grow_array_stepwise(table->symbol_uses,
	    &table->symbol_use_capacity, old_capacity, symbol - old_capacity,
	    1, sizeof(*uses));
	if (uses == NULL)
		return (-1);

	bzero(uses + old_capacity, table->symbol_use_capacity - old_capacity);
	table->symbol_uses = uses;
	account_value_table(ctx);

	return (0);
}

/*
 * Record that `symbol`, which the record of uses covers, is used as a value
 * of `type`, counting the value the first time.
 */
static void
record_symbol_use(relic_valtab_t *table, uint32_t symbol,
    enum relic_type type)
{
	uint8_t *uses;
	uint8_t bit;

	uses = &table->symbol_uses[symbol - 1];
	bit = symbol_use_bit(type);
	if ((*uses & bit) != 0)
		return;

	*uses |= bit;
	if (type == RELIC_SYMBOL)
		table->symbol_values++;
	else
		table->string_values++;
}

/*
 * The domain identifier of a symbol or string value whose symbol the pool
 * holds.  The identifier is the symbol's under the type's bits and needs
 * nothing stored; the record of the symbol's use is what makes it one the
 * context assigned, and what counts the distinct values of each type.
 */
static int
intern_symbol_value(relic_t *ctx, enum relic_type type, uint32_t symbol,
    relic_dom_t *domp)
{
	if (cover_symbol_uses(ctx, symbol) != 0) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	record_symbol_use(&ctx->values, symbol, type);
	*domp = dom_type_bits(type) | symbol;

	return (0);
}

/*
 * Append an integer of hash `hash` to the integer table, which
 * reserve_integers() has made room for, and index it at the position it now
 * holds.  The entry is written before the count advances, so nothing a
 * caller can reach is half initialized.
 */
static void
append_integer(relic_valtab_t *table, int64_t integer, uint32_t hash)
{
	assert(table->integer_count < table->integer_capacity);

	table->integers[table->integer_count] = integer;
	table->integer_count++;
	relic_idtab_place(&table->index, hash, (uint32_t)table->integer_count);
}

/*
 * The bytes the integer table occupies: its array's capacity and its
 * index's slots and tags.
 */
static size_t
integer_table_bytes(const relic_valtab_t *table)
{
	return (relic_saturating_sum(relic_saturating_product(
	    table->integer_capacity, sizeof(*table->integers)),
	    relic_idtab_bytes(&table->index)));
}

/*
 * The bytes the value table occupies: the integer table, and the record of
 * which symbols are values of each type, a byte per symbol it covers.
 */
static size_t
value_table_bytes(const relic_valtab_t *table)
{
	return (relic_saturating_sum(integer_table_bytes(table),
	    table->symbol_use_capacity * sizeof(*table->symbol_uses)));
}

/*
 * The bytes the integer table would occupy holding `fresh_integers` more:
 * its array at the capacity appending them one at a time would reach, and
 * its index after reserving them, which reaches what reserving one at a
 * time reaches.  SIZE_MAX when no capacity could hold them.
 */
static size_t
integer_table_bytes_after(const relic_valtab_t *table, size_t fresh_integers)
{
	size_t capacity;

	if (fresh_integers == 0)
		return (integer_table_bytes(table));

	capacity = relic_grow_array_capacity_stepwise(table->integer_capacity,
	    table->integer_count, fresh_integers, 1);
	if (capacity == 0)
		return (SIZE_MAX);

	return (relic_saturating_sum(relic_saturating_product(capacity,
	    sizeof(*table->integers)), relic_idtab_bytes_after_reserve(
	    &table->index, fresh_integers)));
}

/*
 * The bytes the value table would occupy holding `fresh_integers` more
 * integers and with its record of uses covering `symbol`.
 */
static size_t
value_table_bytes_after(const relic_valtab_t *table, size_t fresh_integers,
    uint32_t symbol)
{
	return (relic_saturating_sum(
	    integer_table_bytes_after(table, fresh_integers),
	    relic_saturating_product(uses_capacity_covering(table, symbol),
	    sizeof(*table->symbol_uses))));
}

/*
 * Ask the memory limit whether the value table may grow as
 * value_table_bytes_after() projects.  Every growth of the table asks
 * here first, whether it interns one value or a batch's, so the limit
 * bounds the table however its values arrive.  A refusal is recorded on the
 * account, as a charged arena's is, so that the caller's
 * relic_diag_no_memory() reports the limit rather than exhaustion.
 */
static bool
admit_value_table(relic_t *ctx, size_t fresh_integers, uint32_t symbol)
{
	return (relic_account_admit(&ctx->account,
	    value_table_bytes(&ctx->values),
	    value_table_bytes_after(&ctx->values, fresh_integers, symbol)));
}

/*
 * Whether `count` more integers can be numbered, the table then holding no
 * more than a domain identifier's position can name.  Interning one
 * integer and committing a batch's both ask this, and the batch's proof
 * asks it through relic_pending_fits(), so they refuse the same integers.
 */
static bool
integers_can_number(const relic_valtab_t *table, size_t count)
{
	return (table->integer_count <= RELIC_INTEGER_MAX &&
	    count <= RELIC_INTEGER_MAX - table->integer_count);
}

/*
 * Whether the integer table's array and index already have room for `count`
 * more integers, so that reserving them grows nothing.
 */
static bool
integer_table_has_room(const relic_valtab_t *table, size_t count)
{
	return (count <= table->integer_capacity - table->integer_count &&
	    relic_idtab_has_room(&table->index, count));
}

/*
 * Make room in the integer table for `count` more integers: the index first
 * and the array second, each to the capacity interning them one at a time
 * would reach, once the memory limit has admitted both.  Interning one
 * integer reserves one, and a batch reserves its new integers at once, so
 * this is the one place the table grows.  Returns 0, or -1 with nothing
 * reported when the table would hold more integers than a position can
 * name, the limit refused, or the allocator did; the contents are as they
 * were, and whatever grew is counted.
 *
 * Most reservations find the room already there.  Those grow nothing and
 * add nothing to the total, so the limit is not asked about them: the
 * projection it is asked with costs more than the reservation itself.  The
 * growths still run, and each finds its own room, so the check decides
 * only whether the limit is asked, never whether the table grows.
 */
static int
reserve_integers(relic_t *ctx, size_t count)
{
	relic_valtab_t *table;
	int64_t *integers;

	table = &ctx->values;
	if (count == 0)
		return (0);
	if (!integers_can_number(table, count))
		return (-1);
	if (!integer_table_has_room(table, count) &&
	    !admit_value_table(ctx, count, 0))
		return (-1);

	if (relic_idtab_reserve(&table->index, count, integer_rehash,
	    table) != 0) {
		account_value_table(ctx);
		return (-1);
	}

	integers = relic_grow_array_stepwise(table->integers,
	    &table->integer_capacity, table->integer_count, count, 1,
	    sizeof(*table->integers));
	if (integers != NULL)
		table->integers = integers;
	account_value_table(ctx);

	return (integers != NULL ? 0 : -1);
}

/*
 * Set the account's count of the value table to what the table occupies,
 * after anything that may have grown it.  The count is reconciled from the
 * table rather than adjusted by each growth, so it cannot drift from it.
 */
static void
account_value_table(relic_t *ctx)
{
	ctx->account.value_bytes = value_table_bytes(&ctx->values);
}

size_t
relic_value_storage_bytes(const relic_t *ctx)
{
	return (relic_saturating_sum(value_table_bytes(&ctx->values),
	    relic_symbol_pool_bytes(ctx)));
}

/*
 * The position the integer table holds an integer at, found by its hash, or
 * RELIC_ID_NONE when it holds none.
 */
static uint32_t
find_integer(const relic_valtab_t *table, int64_t integer, uint32_t hash)
{
	relic_idprobe_t probe;
	uint32_t id;

	relic_idprobe_open(&probe, &table->index, hash);
	while (relic_idprobe_next(&probe, &id)) {
		if (table->integers[id - 1] == integer)
			return (id);
	}

	return (RELIC_ID_NONE);
}

/*
 * The domain identifier of an integer, found in the integer table or else
 * appended to it.
 */
static int
intern_integer(relic_t *ctx, int64_t integer, relic_dom_t *domp)
{
	relic_valtab_t *table;
	uint32_t hash;
	uint32_t id;

	table = &ctx->values;
	hash = relic_hash_int64(table->hash_seed, integer);

	id = find_integer(table, integer, hash);
	if (id != RELIC_ID_NONE) {
		*domp = RELIC_DOM_INTEGER | id;
		return (0);
	}

	/*
	 * Room is reserved before the integer is stored, so that the only
	 * step that can fail happens while there is still nothing to leave
	 * behind.  Storing first would, on a failed index growth, leave an
	 * integer no lookup could reach, and the next intern of an equal
	 * integer would assign it a second identifier -- which the evaluator
	 * would then treat as a different value.
	 */
	if (reserve_integers(ctx, 1) != 0) {
		if (!integers_can_number(table, 1))
			relic_diag_error(ctx, RELIC_ERROR_LIMIT, NULL,
			    "too many distinct integers");
		else
			relic_diag_no_memory(ctx);
		return (-1);
	}

	append_integer(table, integer, hash);
	*domp = RELIC_DOM_INTEGER | (relic_dom_t)table->integer_count;

	return (0);
}

/*
 * Intern a value, storing its domain identifier in *domp.  Returns 0, or -1
 * with a diagnostic already reported.
 */
int
relic_value_intern(relic_t *ctx, const relic_value_t *value, relic_dom_t *domp)
{
	if (!value_well_formed(ctx, value))
		return (-1);

	if (value->type == RELIC_INTEGER)
		return (intern_integer(ctx, value->v.integer, domp));

	return (intern_symbol_value(ctx, value->type, value->v.id, domp));
}

/*
 * Intern a symbol or a string given as bytes, which is what the lexer has.
 * The bytes are interned in the symbol pool, so the symbol foo and the
 * string "foo" share one copy of the bytes and receive two domain
 * identifiers.
 */
int
relic_value_intern_bytes(relic_t *ctx, enum relic_type type, const void *bytes,
    size_t length, relic_dom_t *domp)
{
	relic_value_t value;
	uint32_t symbol;

	if (type != RELIC_SYMBOL && type != RELIC_STRING) {
		relic_diag_internal(ctx, NULL,
		    "bytes cannot be interned as type %d", (int)type);
		return (-1);
	}

	if (relic_symbol_intern(ctx, bytes, length, &symbol) != 0)
		return (-1);

	value.type = type;
	value.v.id = symbol;

	return (relic_value_intern(ctx, &value, domp));
}

int
relic_value_intern_integer(relic_t *ctx, int64_t integer, relic_dom_t *domp)
{
	return (intern_integer(ctx, integer, domp));
}

/*
 * Intern a constant as written, an integer or a symbol's or a string's
 * bytes.
 */
int
relic_value_intern_constant(relic_t *ctx, const relic_constant_t *constant,
    relic_dom_t *domp)
{
	if (constant->type == RELIC_INTEGER)
		return (intern_integer(ctx, constant->integer, domp));

	return (relic_value_intern_bytes(ctx, constant->type, constant->bytes,
	    constant->length, domp));
}

/*
 * The value a symbol or string identifier names, when the symbol has been
 * interned as a value of `type`.
 */
static bool
decode_symbol_value(const relic_valtab_t *table, uint32_t symbol,
    enum relic_type type, relic_value_t *value)
{
	if (!symbol_used_as(table, symbol, type))
		return (false);

	value->type = type;
	value->v.id = symbol;

	return (true);
}

/*
 * The value an integer identifier names, when its position is one the
 * integer table holds.
 */
static bool
decode_integer(const relic_valtab_t *table, uint32_t position,
    relic_value_t *value)
{
	if (position == RELIC_ID_NONE || position > table->integer_count)
		return (false);

	value->type = RELIC_INTEGER;
	value->v.integer = table->integers[position - 1];

	return (true);
}

/*
 * The value is zeroed before its type decodes into it, so every byte a
 * caller receives is written here -- the padding and the member the type
 * does not use included -- and a failed decode leaves zeros behind.
 */
bool
relic_value_decode(const relic_t *ctx, relic_dom_t dom, relic_value_t *value)
{
	const relic_valtab_t *table;
	uint32_t index;

	table = &ctx->values;
	index = dom & RELIC_DOM_INDEX_MASK;

	bzero(value, sizeof(*value));

	switch (dom & RELIC_DOM_TYPE_MASK) {
	case RELIC_DOM_SYMBOL:
		return (decode_symbol_value(table, index, RELIC_SYMBOL, value));
	case RELIC_DOM_STRING:
		return (decode_symbol_value(table, index, RELIC_STRING, value));
	case RELIC_DOM_INTEGER:
		return (decode_integer(table, index, value));
	}

	return (false);
}

relic_dom_t
relic_value_find_integer(const relic_t *ctx, int64_t integer)
{
	const relic_valtab_t *table;
	uint32_t id;

	table = &ctx->values;
	id = find_integer(table, integer,
	    relic_hash_int64(table->hash_seed, integer));
	if (id == RELIC_ID_NONE)
		return (0);

	return (RELIC_DOM_INTEGER | id);
}

/*
 * The probe is the pool's own lookup, and then the string bit: bytes the
 * pool holds only as a symbol value's spelling, or for a value a caller made
 * and has not interned, have no string identifier until interning one as a
 * string sets the bit.
 */
relic_dom_t
relic_value_find_string(const relic_t *ctx, const char *bytes, size_t length)
{
	uint32_t symbol;

	symbol = relic_symbol_find(ctx, bytes, length,
	    relic_symbol_hash(ctx, bytes, length));
	if (!symbol_used_as(&ctx->values, symbol, RELIC_STRING))
		return (0);

	return (RELIC_DOM_STRING | symbol);
}

const char *
relic_value_bytes(const relic_t *ctx, relic_dom_t dom, size_t *lengthp)
{
	relic_value_t value;

	if ((dom & RELIC_DOM_TYPE_MASK) == RELIC_DOM_INTEGER ||
	    !relic_value_decode(ctx, dom, &value))
		return (NULL);

	return (relic_symbol_bytes(ctx, value.v.id, lengthp));
}

bool
relic_value_integer(const relic_t *ctx, relic_dom_t dom, int64_t *integerp)
{
	relic_value_t value;

	if ((dom & RELIC_DOM_TYPE_MASK) != RELIC_DOM_INTEGER ||
	    !decode_integer(&ctx->values, dom & RELIC_DOM_INDEX_MASK, &value))
		return (false);

	*integerp = value.v.integer;

	return (true);
}

/*
 * The slots relic_pending_find() needs to find `count` values among each
 * other: the least power of two at least twice the count, so its table is
 * at most half full and every probe ends at an empty slot.
 */
size_t
relic_pending_slots(size_t count)
{
	size_t size;

	size = 1;
	while (size < 2 * count)
		size *= 2;

	return (size);
}

int
relic_pending_map_init(relic_pending_map_t *map, relic_arena_t *arena,
    size_t room)
{
	bzero(map, sizeof(*map));
	map->room = room;

	map->ids = relic_arena_allocarray(arena, room, sizeof(*map->ids));
	map->fresh = relic_arena_allocarray(arena, room, sizeof(*map->fresh));
	map->strings = relic_arena_allocarray(arena, room,
	    sizeof(*map->strings));
	map->stripes.list = relic_arena_allocarray(arena, room,
	    sizeof(*map->stripes.list));
	if (map->ids == NULL || map->fresh == NULL || map->strings == NULL ||
	    map->stripes.list == NULL)
		return (-1);

	return (0);
}

/*
 * A map's arrays have exactly the sizes relic_pending_map_bytes() counts, so
 * a caller that charged those bytes charged what the map holds.  The count
 * saturates, where a size_t of 32 bits cannot hold it.
 */
size_t
relic_pending_map_bytes(size_t room)
{
	relic_pending_map_t *map;
	size_t per_entry;

	assert(room <= RELIC_DOM_INDEX_MASK);

	per_entry = sizeof(*map->ids) + sizeof(*map->fresh) +
	    sizeof(*map->strings) + sizeof(*map->stripes.list);

	return (relic_saturating_product(room, per_entry));
}

int
relic_pending_map_open(relic_pending_map_t *map, size_t room)
{
	assert(room > 0 && room <= RELIC_DOM_INDEX_MASK);

	bzero(map, sizeof(*map));
	map->room = room;

	map->ids = calloc(room, sizeof(*map->ids));
	map->fresh = calloc(room, sizeof(*map->fresh));
	map->strings = calloc(room, sizeof(*map->strings));
	map->stripes.list = calloc(room, sizeof(*map->stripes.list));
	if (map->ids == NULL || map->fresh == NULL || map->strings == NULL ||
	    map->stripes.list == NULL) {
		relic_pending_map_close(map);
		return (-1);
	}

	return (0);
}

void
relic_pending_map_close(relic_pending_map_t *map)
{
	free(map->ids);
	free(map->fresh);
	free(map->strings);
	free(map->stripes.list);
	bzero(map, sizeof(*map));
}

int
relic_pending_probe_init(relic_pending_probe_t *probe, relic_arena_t *arena,
    size_t room)
{
	bzero(probe, sizeof(*probe));
	probe->room = room;

	probe->hashes = relic_arena_allocarray(arena, room,
	    sizeof(*probe->hashes));
	probe->slots = relic_arena_allocarray(arena,
	    relic_pending_slots(room), sizeof(*probe->slots));
	if (probe->hashes == NULL || probe->slots == NULL)
		return (-1);

	return (0);
}

/*
 * A probe's arrays have exactly the sizes relic_pending_probe_bytes()
 * counts, as a map's have relic_pending_map_bytes().
 */
size_t
relic_pending_probe_bytes(size_t room)
{
	relic_pending_probe_t *probe;

	assert(room <= RELIC_DOM_INDEX_MASK);

	return (relic_saturating_sum(
	    relic_saturating_product(room, sizeof(*probe->hashes)),
	    relic_saturating_product(relic_pending_slots(room),
	    sizeof(*probe->slots))));
}

int
relic_pending_probe_open(relic_pending_probe_t *probe, size_t room)
{
	assert(room > 0 && room <= RELIC_DOM_INDEX_MASK);

	bzero(probe, sizeof(*probe));
	probe->room = room;

	probe->hashes = calloc(room, sizeof(*probe->hashes));
	probe->slots = calloc(relic_pending_slots(room),
	    sizeof(*probe->slots));
	if (probe->hashes == NULL || probe->slots == NULL) {
		relic_pending_probe_close(probe);
		return (-1);
	}

	return (0);
}

void
relic_pending_probe_close(relic_pending_probe_t *probe)
{
	free(probe->hashes);
	free(probe->slots);
	bzero(probe, sizeof(*probe));
}

uint64_t
relic_pending_hash(const relic_t *ctx, const relic_pending_t *value,
    const char *bytes)
{
	if (value->kind == RELIC_PENDING_INTEGER)
		return (relic_hash_int64(ctx->values.hash_seed,
		    value->u.integer));

	return (relic_symbol_hash(ctx, bytes + value->u.text.offset,
	    value->u.text.length));
}

/*
 * The members are visited in batch order, so the first of several equal
 * values to enter the table is the earliest, and the marker every later
 * one is given names it.  The table holds markers, zero in an empty slot.
 */
void
relic_pending_find(const relic_t *ctx, const relic_pending_batch_t *batch,
    const uint32_t *members, size_t member_count, uint32_t *slots,
    size_t slot_count, relic_dom_t *ids)
{
	size_t position;
	size_t mask;
	size_t slot;
	size_t m;

	assert(batch->count <= RELIC_DOM_INDEX_MASK);
	assert(slot_count >= relic_pending_slots(member_count) &&
	    (slot_count & (slot_count - 1)) == 0);

	mask = slot_count - 1;
	for (m = 0; m < member_count; m++) {
		position = members != NULL ? members[m] : m;
		assert(position < batch->count);
		ids[position] = find_assigned(ctx, batch, position);
		if (ids[position] != RELIC_ID_NONE)
			continue;

		for (slot = relic_hash_low(batch->hashes[position]) & mask;
		    slots[slot] != 0;
		    slot = (slot + 1) & mask) {
			if (pending_equal(batch, marker_position(slots[slot]),
			    position))
				break;
		}
		if (slots[slot] == 0)
			slots[slot] = marker_of(position);
		ids[position] = slots[slot];
	}
}

/*
 * The marker naming the batch's value at `position`, and the position a
 * marker names.  A batch holds no more than RELIC_DOM_INDEX_MASK values,
 * so a marker has no type bits, and no identifier is one.
 */
static relic_dom_t
marker_of(size_t position)
{
	assert(position < RELIC_DOM_INDEX_MASK);

	return ((relic_dom_t)position + 1);
}

static bool
is_marker(relic_dom_t id)
{
	return ((id & RELIC_DOM_TYPE_MASK) == 0);
}

static size_t
marker_position(relic_dom_t marker)
{
	assert(marker != RELIC_ID_NONE && is_marker(marker));

	return ((size_t)marker - 1);
}

/*
 * The identifier the context has assigned the batch's value at `position`,
 * or RELIC_ID_NONE when it has none.  For a symbol or a string that is the
 * pool's identifier for its bytes, under the value's type, whether or not
 * the context has made them a value of that type yet: when it has not,
 * relic_pending_append() makes them one by setting the type's bit.
 */
static relic_dom_t
find_assigned(const relic_t *ctx, const relic_pending_batch_t *batch,
    size_t position)
{
	const relic_pending_t *value;
	uint32_t id;

	value = &batch->values[position];
	if (value->kind == RELIC_PENDING_INTEGER) {
		id = find_integer(&ctx->values, value->u.integer,
		    relic_hash_low(batch->hashes[position]));
		return (id == RELIC_ID_NONE ? RELIC_ID_NONE :
		    RELIC_DOM_INTEGER | id);
	}

	id = relic_symbol_find(ctx, batch->bytes + value->u.text.offset,
	    value->u.text.length, batch->hashes[position]);

	return (id == RELIC_ID_NONE ? RELIC_ID_NONE :
	    pending_type_bits(value) | id);
}

/*
 * Whether the batch's values at positions `a` and `b` would be interned as
 * one: two integers of one value, or two symbols or strings of the same
 * bytes, whatever their types, since the pool holds the bytes once.
 */
static bool
pending_equal(const relic_pending_batch_t *batch, size_t a, size_t b)
{
	const relic_pending_t *left;
	const relic_pending_t *right;

	left = &batch->values[a];
	right = &batch->values[b];
	if ((left->kind == RELIC_PENDING_INTEGER) !=
	    (right->kind == RELIC_PENDING_INTEGER))
		return (false);
	if (left->kind == RELIC_PENDING_INTEGER)
		return (left->u.integer == right->u.integer);

	return (batch->hashes[a] == batch->hashes[b] &&
	    left->u.text.length == right->u.text.length &&
	    (left->u.text.length == 0 ||
	    bcmp(batch->bytes + left->u.text.offset,
	    batch->bytes + right->u.text.offset, left->u.text.length) == 0));
}

static relic_dom_t
pending_type_bits(const relic_pending_t *value)
{
	switch (value->kind) {
	case RELIC_PENDING_INTEGER:
		return (RELIC_DOM_INTEGER);
	case RELIC_PENDING_SYMBOL:
		return (dom_type_bits(RELIC_SYMBOL));
	case RELIC_PENDING_TEXT:
		break;
	}

	return (dom_type_bits(RELIC_STRING));
}

/*
 * The capacity the record of symbol uses reaches when it is made to cover
 * `symbol`, by doubling, or SIZE_MAX when the growth would be refused: the
 * capacity a sequence of growths reaches is then the one the highest symbol
 * they covered gives, however the growths were batched, so the serial
 * evaluator and a merged round hold the same bytes.
 */
static size_t
uses_capacity_covering(const relic_valtab_t *table, uint32_t symbol)
{
	size_t capacity;

	if (symbol <= table->symbol_use_capacity)
		return (table->symbol_use_capacity);

	capacity = relic_grow_array_capacity_stepwise(
	    table->symbol_use_capacity, table->symbol_use_capacity,
	    symbol - table->symbol_use_capacity, 1);

	return (capacity == 0 ? SIZE_MAX : capacity);
}

/*
 * Values are numbered in batch order, so a new value is promised an
 * identifier by its first occurrence, which is the order
 * relic_pending_append() interns them in.  The highest symbol a value is
 * promised is kept because the commit records a use of each, and the
 * record must reach that far.  The projection is of the value table and
 * the pool together, each part at the capacity interning the new values
 * one at a time would reach.
 */
size_t
relic_pending_number(const relic_t *ctx, const relic_pending_batch_t *batch,
    relic_pending_map_t *map)
{
	relic_dom_t id;
	size_t i;

	assert(batch->count <= map->room);

	map->count = batch->count;
	map->fresh_count = 0;
	map->string_count = 0;
	map->highest_use = 0;
	for (i = 0; i < batch->count; i++) {
		if (is_marker(map->ids[i]))
			map->ids[i] = number_marked(ctx, batch, i, map);
		id = map->ids[i];
		if ((id & RELIC_DOM_TYPE_MASK) != RELIC_DOM_INTEGER &&
		    (id & RELIC_DOM_INDEX_MASK) > map->highest_use)
			map->highest_use = id & RELIC_DOM_INDEX_MASK;
	}

	relic_symbol_list(map->strings, map->string_count, &map->stripes);

	return (relic_saturating_sum(value_table_bytes_after(&ctx->values,
	    map->fresh_count, map->highest_use),
	    relic_symbol_pool_bytes_after(ctx, map->strings,
	    map->string_count, &map->stripes)));
}

/*
 * The identifier the batch's value at `position` is promised, given the
 * marker relic_pending_find() left it, every value before it numbered.  A
 * marker naming the value itself marks the first of its value, which is
 * promised a new identifier; a later copy takes the one its first was
 * promised, under its own type.
 */
static relic_dom_t
number_marked(const relic_t *ctx, const relic_pending_batch_t *batch,
    size_t position, relic_pending_map_t *map)
{
	size_t first;

	first = marker_position(map->ids[position]);
	assert(first <= position);
	if (first == position)
		return (number_new(ctx, batch, position, map));

	return ((map->ids[first] & RELIC_DOM_INDEX_MASK) |
	    pending_type_bits(&batch->values[position]));
}

/*
 * Promise the batch's value at `position`, the first of its value, the
 * identifier after the last one promised of its kind: an integer the one
 * after the table's count its place among the new integers gives it, and
 * a symbol or a string the one after the pool's count its place among the
 * new bytes gives it.
 */
static relic_dom_t
number_new(const relic_t *ctx, const relic_pending_batch_t *batch,
    size_t position, relic_pending_map_t *map)
{
	const relic_pending_t *value;
	relic_fresh_string_t *fresh;

	value = &batch->values[position];
	if (value->kind == RELIC_PENDING_INTEGER) {
		map->fresh[map->fresh_count] = value->u.integer;
		map->fresh_count++;
		return (RELIC_DOM_INTEGER |
		    (uint32_t)(ctx->values.integer_count + map->fresh_count));
	}

	fresh = &map->strings[map->string_count];
	fresh->bytes = batch->bytes + value->u.text.offset;
	fresh->copy = NULL;
	fresh->length = value->u.text.length;
	fresh->hash = batch->hashes[position];
	map->string_count++;

	return (pending_type_bits(value) |
	    (uint32_t)(relic_symbol_count(ctx) + map->string_count));
}

/*
 * One part holding every value finds them, and numbering them then gives
 * each its identifier.
 */
size_t
relic_pending_measure(const relic_t *ctx, const relic_pending_t *pending,
    size_t count, const char *bytes, relic_pending_probe_t *probe,
    relic_pending_map_t *map)
{
	relic_pending_batch_t batch;
	size_t slot_count;
	size_t i;

	assert(count <= map->room && count <= probe->room);

	for (i = 0; i < count; i++)
		probe->hashes[i] = relic_pending_hash(ctx, &pending[i], bytes);
	batch.values = pending;
	batch.hashes = probe->hashes;
	batch.bytes = bytes;
	batch.count = count;

	slot_count = relic_pending_slots(count);
	bzero(probe->slots, slot_count * sizeof(*probe->slots));
	relic_pending_find(ctx, &batch, NULL, count, probe->slots, slot_count,
	    map->ids);

	return (relic_pending_number(ctx, &batch, map));
}

void
relic_pending_rewrite(const relic_pending_map_t *map, relic_dom_t *tuples,
    size_t count, uint16_t arity)
{
	size_t column;

	for (column = 0; column < count * arity; column++) {
		if ((tuples[column] & RELIC_DOM_TYPE_MASK) != 0)
			continue;

		assert(tuples[column] >= 1 && tuples[column] <= map->count);
		tuples[column] = map->ids[tuples[column] - 1];
	}
}

/*
 * Every part is grown for all of the map's new values at once, each to the
 * capacity interning them one at a time would reach and through the
 * reservation interning one value uses, so that the memory limit admits
 * each growth and the only step that can fail comes before any value is
 * numbered.  The integer table first, then the record of symbol uses, to
 * cover the highest identifier the map promised, and the pool last, whose
 * copies of the new strings are undone whole when one of them fails.  A
 * growth that fails leaves every part's contents as they were, and whatever
 * it grew counted.
 */
int
relic_pending_reserve(relic_t *ctx, relic_pending_map_t *map)
{
	if (reserve_integers(ctx, map->fresh_count) != 0)
		return (-1);

	if (cover_symbol_uses(ctx, map->highest_use) != 0)
		return (-1);

	return (relic_symbol_reserve(ctx, map->strings, map->string_count,
	    &map->stripes, &map->copies));
}

void
relic_pending_unreserve(relic_t *ctx, relic_pending_map_t *map)
{
	relic_symbol_unreserve(ctx, map->strings, map->string_count,
	    &map->copies);
}

void
relic_pending_append(relic_t *ctx, const relic_pending_map_t *map)
{
	relic_symbol_append(ctx, map->strings, map->string_count,
	    &map->stripes);
	append_values(&ctx->values, map);
}

/*
 * Append the map's new integers to the table, and record the use of every
 * symbol and string it promised, into room reserved for them.
 */
static void
append_values(relic_valtab_t *table, const relic_pending_map_t *map)
{
	relic_dom_t id;
	size_t i;

	for (i = 0; i < map->fresh_count; i++)
		append_integer(table, map->fresh[i],
		    relic_hash_int64(table->hash_seed, map->fresh[i]));

	for (i = 0; i < map->count; i++) {
		id = map->ids[i];
		if ((id & RELIC_DOM_TYPE_MASK) == RELIC_DOM_INTEGER)
			continue;

		assert((id & RELIC_DOM_INDEX_MASK) <=
		    table->symbol_use_capacity);
		record_symbol_use(table, id & RELIC_DOM_INDEX_MASK,
		    (id & RELIC_DOM_TYPE_MASK) == RELIC_DOM_SYMBOL ?
		    RELIC_SYMBOL : RELIC_STRING);
	}
}

/*
 * The checks relic_pending_reserve() makes of the identifiers' room before
 * it grows anything, made without growing anything.
 */
bool
relic_pending_fits(const relic_t *ctx, const relic_pending_map_t *map)
{
	return (integers_can_number(&ctx->values, map->fresh_count) &&
	    relic_symbol_has_room(ctx, map->string_count));
}

/*
 * A commit fails for one of three reasons, which the diagnostic tells
 * apart: the integer table, or the pool, holds as many values as an
 * identifier can name, or the allocator refused a growth.
 */
int
relic_pending_commit(relic_t *ctx, relic_pending_map_t *map)
{
	if (intern_pending(ctx, map) != 0) {
		if (!integers_can_number(&ctx->values, map->fresh_count))
			relic_diag_error(ctx, RELIC_ERROR_LIMIT, NULL,
			    "too many distinct integers");
		else if (!relic_symbol_has_room(ctx, map->string_count))
			relic_diag_error(ctx, RELIC_ERROR_LIMIT, NULL,
			    "too many distinct symbols and strings");
		else
			relic_diag_no_memory(ctx);
		return (-1);
	}

	return (0);
}

/*
 * Intern the map's new values, reporting nothing.  The integer table and
 * the record of uses grow first and the pool last, reserved and appended
 * in one relic_symbol_add(), so that the pool may defer its stripes'
 * growth to the parts placing them; the rest is appended once every
 * growth has succeeded.  Returns 0, or -1 with every part's contents as
 * they were and whatever grew counted.
 */
static int
intern_pending(relic_t *ctx, relic_pending_map_t *map)
{
	if (reserve_integers(ctx, map->fresh_count) != 0)
		return (-1);

	if (cover_symbol_uses(ctx, map->highest_use) != 0)
		return (-1);

	if (relic_symbol_add(ctx, map->strings, map->string_count,
	    &map->stripes) != 0)
		return (-1);

	append_values(&ctx->values, map);

	return (0);
}

size_t
relic_value_count(const relic_t *ctx)
{
	return (ctx->values.symbol_values + ctx->values.string_values +
	    ctx->values.integer_count);
}

size_t
relic_value_count_of(const relic_t *ctx, enum relic_type type)
{
	switch (type) {
	case RELIC_SYMBOL:
		return (ctx->values.symbol_values);
	case RELIC_STRING:
		return (ctx->values.string_values);
	case RELIC_INTEGER:
		return (ctx->values.integer_count);
	}

	return (0);
}

static int
compare_integers(int64_t left, int64_t right)
{
	if (left < right)
		return (-1);

	return (left > right ? 1 : 0);
}

/*
 * Order two interned byte strings, whose bytes `bytes_of` reads from
 * `source`, bytewise, and not by locale collation, so that a result never
 * depends on the environment: lexicographically, by relic_bytes_order(),
 * over strings that may contain NUL bytes.
 *
 * A value in a table always names an interned symbol, because interning
 * rejects one that does not, but a value handed in by a caller may name
 * anything.  An identifier the source never assigned sorts before every
 * symbol, so that the order stays total without a comparator that can fail.
 */
static int
compare_bytes(relic_bytes_of_fn bytes_of, const void *source, uint32_t left,
    uint32_t right)
{
	const char *left_bytes;
	const char *right_bytes;
	size_t left_length;
	size_t right_length;

	left_bytes = bytes_of(source, left, &left_length);
	right_bytes = bytes_of(source, right, &right_length);

	if (left_bytes == NULL || right_bytes == NULL) {
		if (left_bytes == right_bytes)
			return (0);

		return (left_bytes == NULL ? -1 : 1);
	}

	switch (relic_bytes_order(left_bytes, left_length, right_bytes,
	    right_length)) {
	case RELIC_ORDER_LESS:
		return (-1);
	case RELIC_ORDER_GREATER:
		return (1);
	case RELIC_ORDER_EQUAL:
	case RELIC_ORDER_UNORDERED:
		break;
	}

	return (0);
}

/*
 * memcmp gives the ordering over the bytes the two have in common; when
 * those are equal the shorter is the smaller.  No bytes in common are not
 * compared at all, so an empty string needs no object to point at.
 */
enum relic_order
relic_bytes_order(const char *left, size_t left_length, const char *right,
    size_t right_length)
{
	size_t common;
	int order;

	common = left_length < right_length ? left_length : right_length;

	order = common == 0 ? 0 : memcmp(left, right, common);
	if (order != 0)
		return (order < 0 ? RELIC_ORDER_LESS : RELIC_ORDER_GREATER);

	if (left_length == right_length)
		return (RELIC_ORDER_EQUAL);

	return (left_length < right_length ? RELIC_ORDER_LESS :
	    RELIC_ORDER_GREATER);
}

/*
 * The order the comparison operators use.
 *
 * Ordering is defined only within a type, so a mixed-type pair is unordered
 * rather than erroneous: predicate arguments are untyped and a column may
 * legitimately hold both an integer and a symbol.  The evaluator turns
 * RELIC_ORDER_UNORDERED into a binding that is filtered out.
 *
 * Equality is exact and needs no payload at all, since equal values intern to
 * one identifier, but the payloads are compared anyway so that this function
 * is a single definition of the order rather than one that agrees with
 * identifier equality only as long as interning is canonical.
 */
enum relic_order
relic_value_order(const relic_t *ctx, relic_dom_t a, relic_dom_t b)
{
	relic_value_t left;
	relic_value_t right;
	bool left_known;
	bool right_known;
	int order;

	left_known = relic_value_decode(ctx, a, &left);
	right_known = relic_value_decode(ctx, b, &right);
	if (!left_known || !right_known || left.type != right.type)
		return (RELIC_ORDER_UNORDERED);

	if (left.type == RELIC_INTEGER)
		order = compare_integers(left.v.integer, right.v.integer);
	else
		order = compare_bytes(context_bytes, ctx, left.v.id,
		    right.v.id);

	if (order < 0)
		return (RELIC_ORDER_LESS);

	return (order > 0 ? RELIC_ORDER_GREATER : RELIC_ORDER_EQUAL);
}

/*
 * Where a type sits in the total order: integers before symbols before
 * strings.  The enumeration is declared in a different order, because it is
 * an ABI whose numbering says nothing about sorting, so the rank is written
 * out rather than derived from the enumerator's value.
 */
static unsigned int
value_type_rank(enum relic_type type)
{
	switch (type) {
	case RELIC_INTEGER:
		return (0);
	case RELIC_SYMBOL:
		return (1);
	case RELIC_STRING:
		return (2);
	}

	/* Unreachable for a stored value; ranks an unknown type last. */
	return (3);
}

/*
 * A total order over every value, used to sort output rows: by type rank
 * first, then within a type as relic_value_order() orders it.
 *
 * It exists only so that a script can ask for stable output from a column
 * that holds more than one type, and it is not visible to the language: no
 * program can observe that an integer sorts before a symbol.  This is its
 * one definition, over whatever table `bytes_of` reads a symbol's or a
 * string's bytes from: relic_value_total_order() reaches it by domain
 * identifier and relic_value_compare() by public value, both over the
 * context, and relic_result_compare() over a collected result.
 */
int
relic_value_order_totally(const relic_value_t *left,
    const relic_value_t *right, relic_bytes_of_fn bytes_of,
    const void *source)
{
	unsigned int left_rank;
	unsigned int right_rank;

	left_rank = value_type_rank(left->type);
	right_rank = value_type_rank(right->type);
	if (left_rank != right_rank)
		return (left_rank < right_rank ? -1 : 1);

	if (left->type == RELIC_INTEGER)
		return (compare_integers(left->v.integer, right->v.integer));

	return (compare_bytes(bytes_of, source, left->v.id, right->v.id));
}

/*
 * relic_symbol_bytes() as a reader of any source's bytes, the source being
 * the context.
 */
static const char *
context_bytes(const void *source, uint32_t id, size_t *lengthp)
{
	return (relic_symbol_bytes(source, id, lengthp));
}

/*
 * The total order over two domain identifiers.
 *
 * An identifier this context never assigned sorts before every real value.
 * Nothing should present one, and ordering it consistently is cheaper than
 * having a sort comparator that can fail.
 */
int
relic_value_total_order(const relic_t *ctx, relic_dom_t a, relic_dom_t b)
{
	relic_value_t left;
	relic_value_t right;
	bool left_known;
	bool right_known;

	if (a == b)
		return (0);

	left_known = relic_value_decode(ctx, a, &left);
	right_known = relic_value_decode(ctx, b, &right);

	if (!left_known || !right_known) {
		if (left_known == right_known)
			return (0);

		return (left_known ? 1 : -1);
	}

	return (relic_value_order_totally(&left, &right, context_bytes, ctx));
}

/*
 * The body of relic_value_symbol() and relic_value_string(), which differ
 * only in `type`.
 */
static int
value_from_bytes(relic_t *ctx, enum relic_type type, const char *bytes,
    size_t length, relic_value_t *value)
{
	int status;

	if (relic_enter(ctx, NULL, NULL) != 0)
		return (-1);

	status = intern_bytes(ctx, type, bytes, length, value);

	relic_leave(ctx);

	return (status);
}

/*
 * Fill *value with interned bytes of `type`.  The value table is left alone:
 * a value is interned there when it enters the program.
 */
static int
intern_bytes(relic_t *ctx, enum relic_type type, const char *bytes,
    size_t length, relic_value_t *value)
{
	uint32_t symbol;

	if ((bytes == NULL && length > 0) || value == NULL) {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "a value needs its bytes and somewhere to be stored");
		return (-1);
	}

	if (type == RELIC_SYMBOL && !relic_lex_is_symbol(bytes, length)) {
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, NULL,
		    RELIC_NOT_A_SYMBOL);
		return (-1);
	}

	if (relic_symbol_intern(ctx, length > 0 ? bytes : "", length,
	    &symbol) != 0)
		return (-1);

	value->type = type;
	value->v.id = symbol;

	return (0);
}

int
relic_value_symbol(relic_t *ctx, const char *bytes, size_t length,
    relic_value_t *value)
{
	return (value_from_bytes(ctx, RELIC_SYMBOL, bytes, length, value));
}

int
relic_value_string(relic_t *ctx, const char *bytes, size_t length,
    relic_value_t *value)
{
	return (value_from_bytes(ctx, RELIC_STRING, bytes, length, value));
}

/*
 * Whether a caller's value is one the context holds: an integer, or a
 * symbol or string identifier the context assigned.
 */
static bool
value_held(const relic_t *ctx, const relic_value_t *value)
{
	switch (value->type) {
	case RELIC_INTEGER:
		return (true);
	case RELIC_SYMBOL:
	case RELIC_STRING:
		return (relic_symbol_bytes(ctx, value->v.id, NULL) != NULL);
	}

	return (false);
}

/*
 * A comparison cannot fail, so a value the context does not hold compares
 * equal to everything, and is reported: a caller sorting with one hears of
 * it rather than getting an order that quietly means nothing.
 */
int
relic_value_compare(relic_t *ctx, const relic_value_t *a,
    const relic_value_t *b)
{
	const relic_value_t *foreign;

	foreign = !value_held(ctx, a) ? a : !value_held(ctx, b) ? b : NULL;
	if (foreign == NULL)
		return (relic_value_order_totally(a, b, context_bytes, ctx));

	report_foreign(ctx, foreign);

	return (0);
}

/*
 * Report a value compared that the context does not hold, naming a
 * collected result's value as one, since that is the commonest such
 * mistake.
 *
 * The report is an operation of its own: it enters the context, so the
 * diagnostic callback runs with the context busy, as every callback does,
 * and can neither call into the context nor compare its way back here.
 * Caller code the library is running is inside an operation already, whose
 * error the report must not displace, so a comparison made there reports
 * nothing.
 */
static void
report_foreign(relic_t *ctx, const relic_value_t *foreign)
{
	if (ctx->busy || relic_enter(ctx, NULL, NULL) != 0)
		return;

	if (foreign->type != RELIC_INTEGER &&
	    (foreign->v.id & RELIC_RESULT_ID_BIT) != 0)
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "a value compared belongs to a collected result; "
		    "relic_result_compare() orders a result's values");
	else
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "a value compared is not one this context holds");

	relic_leave(ctx);
}
