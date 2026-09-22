/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the value table.
 *
 * Everything the evaluator does to a value it does to a domain identifier, so
 * two properties carry the weight: equal values must intern to one identifier
 * and unequal values must never share one, across all three types and across
 * the boundaries between them.  The symbol foo and the string "foo" share one
 * copy of their bytes and must not share a value.
 *
 * The order functions are tested separately from interning, because they are
 * where the language's type rules live: ordering within a type, unordered
 * across types for the comparison operators, and a total order for sorting
 * output.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_EXTREMES	"the extremes of the integer range round trip"
#define	SCENARIO_IDENTITY	"equal values intern to one identifier"
#define	SCENARIO_MALFORMED	"a malformed value is refused"
#define	SCENARIO_ORDER		"the comparison operators order within a type"
#define	SCENARIO_TOTAL		"the sorting order covers every value"
#define	SCENARIO_TYPES		"the three types are distinct domains"

static relic_dom_t	 intern_integer(relic_t *ctx, int64_t value);
static relic_dom_t	 intern_text(relic_t *ctx, enum relic_type type,
			    const char *text);
static void		 a_malformed_value_is_refused(void);
static void		 equal_values_intern_to_one_identifier(void);
static void		 every_integer_round_trips(void);
static void		 the_comparison_order_holds_within_a_type(void);
static void		 the_sorting_order_ranks_types(void);
static void		 the_three_types_stay_distinct(void);

/*
 * Intern and return the identifier, or RELIC_ID_NONE on failure.  A test that
 * is checking ordering should not have to carry a status through every
 * fixture it builds.
 */
static relic_dom_t
intern_text(relic_t *ctx, enum relic_type type, const char *text)
{
	relic_dom_t dom;

	if (relic_value_intern_bytes(ctx, type, text, strlen(text),
	    &dom) != 0)
		return (RELIC_ID_NONE);

	return (dom);
}

static relic_dom_t
intern_integer(relic_t *ctx, int64_t value)
{
	relic_dom_t dom;

	if (relic_value_intern_integer(ctx, value, &dom) != 0)
		return (RELIC_ID_NONE);

	return (dom);
}

static void
equal_values_intern_to_one_identifier(void)
{
	relic_t *ctx;
	const relic_value_t *stored;
	relic_dom_t first;
	relic_dom_t again;
	relic_dom_t other;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_IDENTITY, "a context should be created");
	if (ctx == NULL)
		return;

	first = intern_text(ctx, RELIC_SYMBOL, "path");
	again = intern_text(ctx, RELIC_SYMBOL, "path");
	other = intern_text(ctx, RELIC_SYMBOL, "edge");

	check(first != RELIC_ID_NONE, SCENARIO_IDENTITY,
	    "interning a symbol should succeed");
	check(first == again, SCENARIO_IDENTITY,
	    "the same symbol should return the identifier already assigned");
	check(first != other, SCENARIO_IDENTITY,
	    "a different symbol should receive a different identifier");
	check(relic_value_count(ctx) == 2, SCENARIO_IDENTITY,
	    "only the two distinct values should have been stored");

	first = intern_integer(ctx, 10);
	again = intern_integer(ctx, 10);
	other = intern_integer(ctx, 11);

	check(first == again, SCENARIO_IDENTITY,
	    "the same integer should return one identifier");
	check(first != other, SCENARIO_IDENTITY,
	    "a different integer should receive a different identifier");

	stored = relic_value_of(ctx, first);
	check(stored != NULL, SCENARIO_IDENTITY,
	    "an assigned identifier should name a value");
	check(stored != NULL && stored->type == RELIC_INTEGER,
	    SCENARIO_IDENTITY, "the stored type should be the one interned");
	check(stored != NULL && stored->v.integer == 10, SCENARIO_IDENTITY,
	    "the stored payload should be the one interned");

	check(relic_value_of(ctx, RELIC_ID_NONE) == NULL, SCENARIO_IDENTITY,
	    "identifier zero should name nothing");
	check(relic_value_of(ctx, (relic_dom_t)relic_value_count(ctx) + 1) ==
	    NULL, SCENARIO_IDENTITY,
	    "an identifier past the last should name nothing");

	relic_destroy(ctx);
}

/*
 * The symbol foo, the string "foo" and the integer 10 are three domains that
 * never meet.  The first two are the interesting pair: they share a symbol
 * identifier because the bytes are interned once, so if the type did not
 * participate in the value's key they would collapse into one value.
 */
static void
the_three_types_stay_distinct(void)
{
	relic_t *ctx;
	const relic_value_t *as_symbol;
	const relic_value_t *as_string;
	relic_dom_t integer;
	relic_dom_t symbol;
	relic_dom_t string;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_TYPES, "a context should be created");
	if (ctx == NULL)
		return;

	symbol = intern_text(ctx, RELIC_SYMBOL, "foo");
	string = intern_text(ctx, RELIC_STRING, "foo");

	check(symbol != RELIC_ID_NONE && string != RELIC_ID_NONE,
	    SCENARIO_TYPES, "both should intern");
	check(symbol != string, SCENARIO_TYPES,
	    "a symbol and a string with the same spelling are not one value");

	as_symbol = relic_value_of(ctx, symbol);
	as_string = relic_value_of(ctx, string);
	check(as_symbol != NULL && as_string != NULL &&
	    as_symbol->v.id == as_string->v.id, SCENARIO_TYPES,
	    "they should nonetheless share one copy of their bytes");

	integer = intern_integer(ctx, 1);

	check(relic_value_order(ctx, symbol, string) ==
	    RELIC_ORDER_UNORDERED, SCENARIO_TYPES,
	    "a comparison across types should be unordered");
	check(relic_value_order(ctx, symbol, integer) ==
	    RELIC_ORDER_UNORDERED, SCENARIO_TYPES,
	    "and so should one between a symbol and an integer");

	relic_destroy(ctx);
}

/*
 * The whole integer range, including the extremes, which is where a
 * conversion that went through an unsigned type or a smaller one would show
 * up.  INT64_MIN in particular has no positive counterpart, so it is the
 * value most likely to be mishandled.
 */
static void
every_integer_round_trips(void)
{
	static const int64_t values[] = {
		INT64_MIN, INT64_MIN + 1, -2, -1, 0, 1, 2,
		INT64_MAX - 1, INT64_MAX
	};

	enum { VALUE_COUNT = sizeof(values) / sizeof(values[0]) };

	relic_t *ctx;
	relic_dom_t doms[VALUE_COUNT];
	size_t first_uninterned;
	size_t first_unrecovered;
	size_t first_collision;
	size_t i;
	size_t j;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_EXTREMES, "a context should be created");
	if (ctx == NULL)
		return;

	first_uninterned = VALUE_COUNT;
	for (i = 0; i < VALUE_COUNT; i++) {
		doms[i] = intern_integer(ctx, values[i]);
		if (doms[i] == RELIC_ID_NONE && first_uninterned == VALUE_COUNT)
			first_uninterned = i;
	}

	checkf(first_uninterned == VALUE_COUNT, SCENARIO_EXTREMES,
	    "the integer at position %zu should have interned",
	    first_uninterned);

	if (first_uninterned != VALUE_COUNT) {
		relic_destroy(ctx);
		return;
	}

	first_unrecovered = VALUE_COUNT;
	for (i = 0; i < VALUE_COUNT && first_unrecovered == VALUE_COUNT; i++) {
		const relic_value_t *stored;

		stored = relic_value_of(ctx, doms[i]);
		if (stored == NULL || stored->type != RELIC_INTEGER ||
		    stored->v.integer != values[i])
			first_unrecovered = i;
	}

	checkf(first_unrecovered == VALUE_COUNT, SCENARIO_EXTREMES,
	    "the identifier at position %zu should name the integer it was "
	    "given", first_unrecovered);

	first_collision = VALUE_COUNT;
	for (i = 0; i < VALUE_COUNT && first_collision == VALUE_COUNT; i++) {
		for (j = i + 1; j < VALUE_COUNT; j++) {
			if (doms[i] == doms[j])
				first_collision = i;
		}
	}

	checkf(first_collision == VALUE_COUNT, SCENARIO_EXTREMES,
	    "the integer at position %zu should have its own identifier",
	    first_collision);

	relic_destroy(ctx);
}

/*
 * Ordering within each type: numeric for integers, bytewise for symbols and
 * for strings.  Bytewise rather than collation means the order never depends
 * on the environment, and the uppercase letter below is the case that would
 * differ under a locale that folds case.
 */
static void
the_comparison_order_holds_within_a_type(void)
{
	relic_t *ctx;
	relic_dom_t negative;
	relic_dom_t zero;
	relic_dom_t positive;
	relic_dom_t upper_b;
	relic_dom_t lower_a;
	relic_dom_t abc;
	relic_dom_t abcd;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_ORDER, "a context should be created");
	if (ctx == NULL)
		return;

	negative = intern_integer(ctx, INT64_MIN);
	zero = intern_integer(ctx, 0);
	positive = intern_integer(ctx, INT64_MAX);

	check(relic_value_order(ctx, negative, zero) == RELIC_ORDER_LESS,
	    SCENARIO_ORDER, "the smallest integer should sort below zero");
	check(relic_value_order(ctx, positive, zero) == RELIC_ORDER_GREATER,
	    SCENARIO_ORDER, "the largest integer should sort above zero");
	check(relic_value_order(ctx, zero, zero) == RELIC_ORDER_EQUAL,
	    SCENARIO_ORDER, "a value should equal itself");

	upper_b = intern_text(ctx, RELIC_SYMBOL, "B");
	lower_a = intern_text(ctx, RELIC_SYMBOL, "a");

	check(relic_value_order(ctx, upper_b, lower_a) == RELIC_ORDER_LESS,
	    SCENARIO_ORDER,
	    "symbols should order by byte value, not by letter");

	abc = intern_text(ctx, RELIC_STRING, "abc");
	abcd = intern_text(ctx, RELIC_STRING, "abcd");

	check(relic_value_order(ctx, abc, abcd) == RELIC_ORDER_LESS,
	    SCENARIO_ORDER, "a prefix should sort before the longer string");
	check(relic_value_order(ctx, abcd, abc) == RELIC_ORDER_GREATER,
	    SCENARIO_ORDER, "and the comparison should be antisymmetric");

	check(relic_value_order(ctx, abc, RELIC_ID_NONE) ==
	    RELIC_ORDER_UNORDERED, SCENARIO_ORDER,
	    "an identifier naming no value should be unordered");

	relic_destroy(ctx);
}

/*
 * The order used to sort output rows ranks integers before symbols before
 * strings, and agrees with the comparison operators within a type.  It exists
 * so that a column holding more than one type still sorts stably; no program
 * can observe it.
 */
static void
the_sorting_order_ranks_types(void)
{
	relic_t *ctx;
	relic_dom_t integer;
	relic_dom_t smaller;
	relic_dom_t symbol;
	relic_dom_t string;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_TOTAL, "a context should be created");
	if (ctx == NULL)
		return;

	/*
	 * Interned in the reverse of the order they should sort in, so that a
	 * comparison that fell back to the identifier would give the opposite
	 * answer to the one required.
	 */
	string = intern_text(ctx, RELIC_STRING, "aaa");
	symbol = intern_text(ctx, RELIC_SYMBOL, "zzz");
	integer = intern_integer(ctx, 5);

	check(relic_value_total_order(ctx, integer, symbol) < 0,
	    SCENARIO_TOTAL, "an integer should sort before a symbol");
	check(relic_value_total_order(ctx, symbol, string) < 0,
	    SCENARIO_TOTAL, "a symbol should sort before a string");
	check(relic_value_total_order(ctx, string, integer) > 0,
	    SCENARIO_TOTAL, "and the order should be transitive across ranks");
	check(relic_value_total_order(ctx, symbol, symbol) == 0,
	    SCENARIO_TOTAL, "a value should sort equal to itself");

	smaller = intern_integer(ctx, 4);

	check(relic_value_total_order(ctx, smaller, integer) < 0,
	    SCENARIO_TOTAL,
	    "within a type it should agree with the comparison operators");

	relic_destroy(ctx);
}

/*
 * An embedding program builds a relic_value_t itself, so it can present one
 * naming a symbol this context never interned.  That must be refused with a
 * diagnostic rather than stored, because everything downstream reads through
 * the identifier it holds.
 *
 * The other malformed value, one whose type is not an enumerator at all, is
 * guarded against in the library but cannot be tested: constructing it is
 * undefined behaviour in C, and UndefinedBehaviorSanitizer reports the load
 * before the guard is reached.  Passing a valid type where the interface
 * accepts only two of the three covers the reachable half.
 */
static void
a_malformed_value_is_refused(void)
{
	relic_t *ctx;
	relic_value_t value;
	relic_dom_t dom;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_MALFORMED, "a context should be created");
	if (ctx == NULL)
		return;

	bzero(&value, sizeof(value));
	value.type = RELIC_SYMBOL;
	value.v.id = 42;

	check(relic_value_intern(ctx, &value, &dom) != 0, SCENARIO_MALFORMED,
	    "a value naming an unknown symbol should be refused");
	check(relic_error(ctx) != NULL, SCENARIO_MALFORMED,
	    "and the reason should be recoverable");

	check(relic_value_intern_bytes(ctx, RELIC_INTEGER, "x", 1, &dom) != 0,
	    SCENARIO_MALFORMED,
	    "bytes should not be internable as an integer");
	check(relic_value_count(ctx) == 0, SCENARIO_MALFORMED,
	    "no refused value should have been stored");

	relic_destroy(ctx);
}

int
main(void)
{
	a_malformed_value_is_refused();
	equal_values_intern_to_one_identifier();
	every_integer_round_trips();
	the_comparison_order_holds_within_a_type();
	the_sorting_order_ranks_types();
	the_three_types_stay_distinct();

	return (check_summary("value"));
}
