/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

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

static int	 compare_bytes(const relic_t *ctx, uint32_t left,
		    uint32_t right);
static int	 compare_integers(int64_t left, int64_t right);
static int	 compare_totally(const relic_t *ctx, const relic_value_t *left,
		    const relic_value_t *right);
static int	 cover_symbol_uses(relic_t *ctx, uint32_t symbol);
static bool	 decode_integer(const relic_valtab_t *table, uint32_t position,
		    relic_value_t *value);
static bool	 decode_symbol_value(const relic_valtab_t *table,
		    uint32_t symbol, enum relic_type type,
		    relic_value_t *value);
static relic_dom_t dom_type_bits(enum relic_type type);
static int	 intern_bytes(relic_t *ctx, enum relic_type type,
		    const char *bytes, size_t length, relic_value_t *value);
static int	 intern_integer(relic_t *ctx, int64_t integer,
		    relic_dom_t *domp);
static int	 intern_symbol_value(relic_t *ctx, enum relic_type type,
		    uint32_t symbol, relic_dom_t *domp);
static uint32_t	 integer_rehash(uint32_t id, void *ctx);
static int	 integer_store(relic_t *ctx, int64_t integer, uint32_t *idp);
static void	 record_symbol_use(relic_valtab_t *table, uint32_t symbol,
		    enum relic_type type);
static uint8_t	 symbol_use_bit(enum relic_type type);
static bool	 symbol_used_as(const relic_valtab_t *table, uint32_t symbol,
		    enum relic_type type);
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
	relic_free_array(table->integers);
	relic_free_array(table->symbol_uses);
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
		if (bytes == NULL) {
			relic_diagf(ctx, RELIC_ERROR, NULL,
			    "value refers to symbol %lu, which this context "
			    "has not interned",
			    (unsigned long)value->v.id);
			return (false);
		}
		if (value->type == RELIC_SYMBOL &&
		    !relic_lex_is_symbol(bytes, length)) {
			relic_diagf(ctx, RELIC_ERROR, NULL, RELIC_NOT_A_SYMBOL);
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
	relic_diagf(ctx, RELIC_ERROR, NULL, "value has unknown type %d",
	    (int)value->type);

	return (false);
}

/*
 * Make the record of uses cover `symbol`.  The entries a growth adds are
 * cleared, since a symbol whose entry held a stray bit would decode as a
 * value no one made, and a failed growth leaves every earlier record as it
 * was.
 */
static int
cover_symbol_uses(relic_t *ctx, uint32_t symbol)
{
	relic_valtab_t *table;
	uint8_t *uses;
	size_t old_capacity;

	table = &ctx->values;
	if (symbol <= table->symbol_use_capacity)
		return (0);

	old_capacity = table->symbol_use_capacity;
	uses = relic_grow_array(table->symbol_uses, &table->symbol_use_capacity,
	    symbol, sizeof(*uses));
	if (uses == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	bzero(uses + old_capacity, table->symbol_use_capacity - old_capacity);
	table->symbol_uses = uses;

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
	if (cover_symbol_uses(ctx, symbol) != 0)
		return (-1);

	record_symbol_use(&ctx->values, symbol, type);
	*domp = dom_type_bits(type) | symbol;

	return (0);
}

/*
 * Append an integer to the integer table and return its position.  The
 * entry is written before the count advances, so nothing a caller can reach
 * is half initialized.
 */
static int
integer_store(relic_t *ctx, int64_t integer, uint32_t *idp)
{
	relic_valtab_t *table;
	int64_t *integers;

	table = &ctx->values;

	if (table->integer_count >= RELIC_INTEGER_MAX) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "too many distinct integers");
		return (-1);
	}

	integers = relic_grow_array(table->integers, &table->integer_capacity,
	    table->integer_count + 1, sizeof(*table->integers));
	if (integers == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	table->integers = integers;

	table->integers[table->integer_count] = integer;
	table->integer_count++;

	*idp = (uint32_t)table->integer_count;

	return (0);
}

/*
 * The domain identifier of an integer, found in the integer table or else
 * appended to it.
 */
static int
intern_integer(relic_t *ctx, int64_t integer, relic_dom_t *domp)
{
	relic_valtab_t *table;
	relic_idprobe_t probe;
	uint32_t hash;
	uint32_t id;

	table = &ctx->values;
	hash = relic_hash_int64(table->hash_seed, integer);

	relic_idprobe_open(&probe, &table->index, hash);
	while (relic_idprobe_next(&probe, &id)) {
		if (table->integers[id - 1] == integer) {
			*domp = RELIC_DOM_INTEGER | id;
			return (0);
		}
	}

	/*
	 * Room in the index is taken before the integer is stored, so that
	 * the only step that can fail happens while there is still nothing to
	 * leave behind.  Storing first would, on a failed index growth, leave
	 * an integer no lookup could reach, and the next intern of an equal
	 * integer would assign it a second identifier -- which the evaluator
	 * would then treat as a different value.
	 */
	if (relic_idtab_reserve(&table->index, 1, integer_rehash, table) != 0) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	if (integer_store(ctx, integer, &id) != 0)
		return (-1);

	relic_idtab_place(&table->index, hash, id);
	*domp = RELIC_DOM_INTEGER | id;

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
		relic_diagf(ctx, RELIC_ERROR, NULL,
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
 * Order two interned byte strings bytewise, and not by locale collation, so
 * that a result never depends on the environment.
 *
 * memcmp gives the ordering over the bytes the two have in common; when those
 * are equal the shorter string is the smaller, which is what makes this a
 * lexicographic order over strings that may contain NUL bytes.
 *
 * A value in the table always names an interned symbol, because
 * relic_value_intern() rejects one that does not, but a value handed to
 * relic_value_compare() is the caller's and may name anything.  An identifier
 * this context never assigned sorts before every symbol, so that the order
 * stays total without a comparator that can fail.
 */
static int
compare_bytes(const relic_t *ctx, uint32_t left, uint32_t right)
{
	const char *left_bytes;
	const char *right_bytes;
	size_t left_length;
	size_t right_length;
	size_t common;
	int order;

	left_bytes = relic_symbol_bytes(ctx, left, &left_length);
	right_bytes = relic_symbol_bytes(ctx, right, &right_length);

	if (left_bytes == NULL || right_bytes == NULL) {
		if (left_bytes == right_bytes)
			return (0);

		return (left_bytes == NULL ? -1 : 1);
	}

	common = left_length < right_length ? left_length : right_length;

	order = memcmp(left_bytes, right_bytes, common);
	if (order != 0)
		return (order < 0 ? -1 : 1);

	if (left_length == right_length)
		return (0);

	return (left_length < right_length ? -1 : 1);
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
		order = compare_bytes(ctx, left.v.id, right.v.id);

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
 * one definition; relic_value_total_order() reaches it by domain identifier
 * and relic_value_compare() by public value.
 */
static int
compare_totally(const relic_t *ctx, const relic_value_t *left,
    const relic_value_t *right)
{
	unsigned int left_rank;
	unsigned int right_rank;

	left_rank = value_type_rank(left->type);
	right_rank = value_type_rank(right->type);
	if (left_rank != right_rank)
		return (left_rank < right_rank ? -1 : 1);

	if (left->type == RELIC_INTEGER)
		return (compare_integers(left->v.integer, right->v.integer));

	return (compare_bytes(ctx, left->v.id, right->v.id));
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

	return (compare_totally(ctx, &left, &right));
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

	if (relic_enter(ctx) != 0)
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
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "a value needs its bytes and somewhere to be stored");
		return (-1);
	}

	if (type == RELIC_SYMBOL && !relic_lex_is_symbol(bytes, length)) {
		relic_diagf(ctx, RELIC_ERROR, NULL, RELIC_NOT_A_SYMBOL);
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

int
relic_value_compare(relic_t *ctx, const relic_value_t *a,
    const relic_value_t *b)
{
	return (compare_totally(ctx, a, b));
}
