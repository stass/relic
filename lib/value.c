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
 */

/*
 * Domain identifiers are 32-bit and zero means "none", so a context holds at
 * most this many distinct values.  Checked rather than assumed: an identifier
 * that wrapped would silently merge two unrelated values.
 */
#define	RELIC_VALUE_MAX		(UINT32_MAX - 1u)

/* Why bytes the language could not write as a symbol are refused as one. */
#define	RELIC_NOT_A_SYMBOL \
	"a symbol is a lowercase letter followed by letters, digits and " \
	"underscores; other bytes make a string"

static int	 compare_bytes(const relic_t *ctx, uint32_t left,
		    uint32_t right);
static int	 compare_integers(int64_t left, int64_t right);
static int	 compare_totally(const relic_t *ctx, const relic_value_t *left,
		    const relic_value_t *right);
static int	 intern_bytes(relic_t *ctx, enum relic_type type,
		    const char *bytes, size_t length, relic_value_t *value);
static const relic_value_t *value_at(const relic_valtab_t *table,
		    relic_dom_t dom);
static bool	 value_equal(const relic_value_t *left,
		    const relic_value_t *right);
static int	 value_from_bytes(relic_t *ctx, enum relic_type type,
		    const char *bytes, size_t length, relic_value_t *value);
static uint32_t	 value_hash(const relic_value_t *value);
static relic_value_t value_normalized(const relic_value_t *value);
static uint32_t	 value_rehash(uint32_t dom, void *ctx);
static int	 value_store(relic_t *ctx, const relic_value_t *value,
		    relic_dom_t *domp);
static unsigned int value_type_rank(enum relic_type type);
static bool	 value_well_formed(relic_t *ctx, const relic_value_t *value);

void
relic_valtab_init(relic_valtab_t *table)
{
	bzero(table, sizeof(*table));
	relic_idtab_init(&table->index);
}

void
relic_valtab_reset(relic_valtab_t *table)
{
	relic_idtab_reset(&table->index);
	free(table->entries);
	bzero(table, sizeof(*table));
}

/*
 * The value a domain identifier names.  Identifier zero is reserved, so the
 * array is indexed one lower; this is the only place that translation is
 * written.  The caller must have established that the identifier was
 * assigned.
 */
static const relic_value_t *
value_at(const relic_valtab_t *table, relic_dom_t dom)
{
	return (&table->entries[dom - 1]);
}

/*
 * Hash a value by its fields rather than by its bytes.
 *
 * relic_value_t contains a union and padding, so hashing its representation
 * would depend on bytes that no assignment defines and two equal values could
 * hash differently.  Reading the type and the live union member makes the
 * hash a function of the value itself.  The type participates so that the
 * symbol foo and the string "foo", which share one symbol identifier, are as
 * likely to land in different slots as any two unrelated keys.
 */
static uint32_t
value_hash(const relic_value_t *value)
{
	uint32_t payload;

	/*
	 * The switch covers every enumerator and has no default, so a type
	 * added later is reported here rather than silently hashing every
	 * value of it into one bucket.  The initialization is what a type
	 * outside the enumeration would fall back on; such a value cannot
	 * come from this table, and reaching the switch with one is already
	 * undefined, but a defined payload is better than an indeterminate
	 * one.
	 */
	payload = 0;

	switch (value->type) {
	case RELIC_SYMBOL:
	case RELIC_STRING:
		payload = relic_hash_word(value->v.id);
		break;
	case RELIC_INTEGER:
		payload = relic_hash_int64(value->v.integer);
		break;
	}

	return (relic_hash_word(payload ^ (uint32_t)value->type));
}

/*
 * Whether two values are the same value.  Different types are never equal,
 * which is the language rule: the symbol foo and the string "foo" are
 * distinct.
 */
static bool
value_equal(const relic_value_t *left, const relic_value_t *right)
{
	if (left->type != right->type)
		return (false);

	if (left->type == RELIC_INTEGER)
		return (left->v.integer == right->v.integer);

	return (left->v.id == right->v.id);
}

static uint32_t
value_rehash(uint32_t dom, void *ctx)
{
	const relic_valtab_t *table;

	table = ctx;

	return (value_hash(value_at(table, dom)));
}

/*
 * Whether a value a caller handed in is one this context can intern: a known
 * type, for a symbol or a string an identifier this context assigned, and for
 * a symbol bytes the language could write as one.
 *
 * An embedding program builds relic_value_t itself, so the identifier in one
 * may name a symbol from a different context or no symbol at all.  Catching
 * that here keeps a bad identifier out of the value table, where later stages
 * would read through it.  The same caller could name a string's bytes as a
 * symbol, which would print as text that reads back as something else, so
 * every value reaching the table is held to the lexer's rule here, once.
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
 * A value with one definite representation: the type, the union member that
 * type selects, and zero everywhere else.
 *
 * relic_value_t has padding between its type and its union, and a union half
 * the caller need not have written.  A caller that filled in only the fields
 * the interface documents leaves those bytes holding whatever its stack held,
 * and relic_value_of() hands whatever is stored straight back.  Normalizing
 * here, in the one place a value enters the table, makes the property a fact
 * about the table rather than a habit of the callers that happen to zero
 * their locals first.
 */
static relic_value_t
value_normalized(const relic_value_t *value)
{
	relic_value_t normalized;

	bzero(&normalized, sizeof(normalized));
	normalized.type = value->type;

	switch (value->type) {
	case RELIC_SYMBOL:
	case RELIC_STRING:
		normalized.v.id = value->v.id;
		break;
	case RELIC_INTEGER:
		normalized.v.integer = value->v.integer;
		break;
	}

	return (normalized);
}

/*
 * Append a value to the value array and return the domain identifier
 * assigned to it.  The entry is written in full before the count advances,
 * so nothing a caller can reach is half initialized.
 */
static int
value_store(relic_t *ctx, const relic_value_t *value, relic_dom_t *domp)
{
	relic_valtab_t *table;
	relic_value_t *entries;
	relic_value_t stored;

	table = &ctx->values;

	if (table->count >= RELIC_VALUE_MAX) {
		relic_diagf(ctx, RELIC_ERROR, NULL, "too many distinct values");
		return (-1);
	}

	/*
	 * The value is copied out before the array is grown, so a caller may
	 * pass a pointer into the table itself -- what relic_value_of()
	 * returns, say -- without the reallocation freeing it underneath the
	 * copy.
	 */
	stored = value_normalized(value);

	entries = relic_grow_array(table->entries, &table->capacity,
	    table->count + 1, sizeof(*table->entries));
	if (entries == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	table->entries = entries;

	table->entries[table->count] = stored;
	table->count++;

	*domp = (relic_dom_t)table->count;

	return (0);
}

/*
 * Intern a value, storing its domain identifier in *domp.  Returns 0, or -1
 * with a diagnostic already reported.
 */
int
relic_value_intern(relic_t *ctx, const relic_value_t *value, relic_dom_t *domp)
{
	relic_valtab_t *table;
	relic_idprobe_t probe;
	uint32_t hash;
	uint32_t dom;

	if (!value_well_formed(ctx, value))
		return (-1);

	table = &ctx->values;
	hash = value_hash(value);

	relic_idprobe_open(&probe, &table->index, hash);
	while (relic_idprobe_next(&probe, &dom)) {
		if (value_equal(value_at(table, dom), value)) {
			*domp = dom;
			return (0);
		}
	}

	/*
	 * Room in the index is taken before the value is stored, so that the
	 * only step that can fail happens while there is still nothing to
	 * leave behind.  Storing first would, on a failed index growth, leave
	 * a value no lookup could reach, and the next intern of an equal
	 * value would assign it a second identifier -- which the evaluator
	 * would then treat as a different value.
	 */
	if (relic_idtab_reserve(&table->index, value_rehash, table) != 0) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	if (value_store(ctx, value, &dom) != 0)
		return (-1);

	relic_idtab_place(&table->index, hash, dom);
	*domp = dom;

	return (0);
}

/*
 * Intern a symbol or a string given as bytes, which is what the lexer has.
 * The bytes are interned in the symbol pool and the resulting value in the
 * value table, so the symbol foo and the string "foo" share one copy of the
 * bytes and receive two domain identifiers.
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
	relic_value_t value;

	value.type = RELIC_INTEGER;
	value.v.integer = integer;

	return (relic_value_intern(ctx, &value, domp));
}

const relic_value_t *
relic_value_of(const relic_t *ctx, relic_dom_t dom)
{
	if (dom == RELIC_ID_NONE || dom > ctx->values.count)
		return (NULL);

	return (value_at(&ctx->values, dom));
}

size_t
relic_value_count(const relic_t *ctx)
{
	return (ctx->values.count);
}

/*
 * How many of the interned values have the given type.  The table is walked
 * rather than counted on insertion, because only statistics ask and they ask
 * rarely.
 */
size_t
relic_value_count_of(const relic_t *ctx, enum relic_type type)
{
	size_t count;
	size_t i;

	count = 0;
	for (i = 0; i < ctx->values.count; i++) {
		if (ctx->values.entries[i].type == type)
			count++;
	}

	return (count);
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
	const relic_value_t *left;
	const relic_value_t *right;
	int order;

	left = relic_value_of(ctx, a);
	right = relic_value_of(ctx, b);

	if (left == NULL || right == NULL || left->type != right->type)
		return (RELIC_ORDER_UNORDERED);

	if (left->type == RELIC_INTEGER)
		order = compare_integers(left->v.integer, right->v.integer);
	else
		order = compare_bytes(ctx, left->v.id, right->v.id);

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
	const relic_value_t *left;
	const relic_value_t *right;

	if (a == b)
		return (0);

	left = relic_value_of(ctx, a);
	right = relic_value_of(ctx, b);

	if (left == NULL || right == NULL) {
		if (left == NULL && right == NULL)
			return (0);

		return (left == NULL ? -1 : 1);
	}

	return (compare_totally(ctx, left, right));
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
