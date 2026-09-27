/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the identifier ceilings of the symbol pool and the
 * integer table.
 *
 * A domain identifier keeps its type in its top two bits, so each table
 * numbers its entries below them, and an entry numbered past the index mask
 * would run into the type bits and turn one value into another.  Each table
 * refuses the entry that would pass its ceiling.  No test can intern a
 * billion values, so this program compiles the library with both ceilings
 * lowered, and checks what the real ceilings are held to: the entry past
 * the ceiling is refused with a diagnostic, and the table is left as it
 * was, its values interning to and decoding as the identifiers they had.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_INTEGERS	"the integer table stops at its ceiling"
#define	SCENARIO_SYMBOLS	"the symbol pool stops at its ceiling"

#define	SYMBOL_TEXT_MAX		32

static bool	 diagnosed(relic_t *ctx, const char *expected);
static void	 symbol_text(char *text, size_t size, size_t ordinal);
static void	 the_integer_table_stops_at_its_ceiling(void);
static void	 the_symbol_pool_stops_at_its_ceiling(void);

/*
 * Symbols are interned until the pool refuses one.  Exactly RELIC_SYMBOL_MAX
 * are admitted, counting any the context holds already, and the refusal
 * says why.  A string of new bytes is refused too, since strings share the
 * pool, but a string of bytes the pool holds is admitted: it needs no new
 * entry.  The symbols admitted keep their identifiers and their bytes.
 */
static void
the_symbol_pool_stops_at_its_ceiling(void)
{
	char text[SYMBOL_TEXT_MAX];
	relic_t *ctx;
	relic_value_t decoded;
	relic_dom_t first;
	relic_dom_t last;
	relic_dom_t dom;
	size_t admitted;
	size_t held;
	bool decoded_last;
	int status;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_SYMBOLS, "a context should be created");
	if (ctx == NULL)
		return;

	held = relic_symbol_count(ctx);
	first = RELIC_ID_NONE;
	last = RELIC_ID_NONE;
	for (admitted = 0; admitted <= RELIC_SYMBOL_MAX; admitted++) {
		symbol_text(text, sizeof(text), admitted);
		if (relic_value_intern_bytes(ctx, RELIC_SYMBOL, text,
		    strlen(text), &dom) != 0)
			break;
		if (first == RELIC_ID_NONE)
			first = dom;
		last = dom;
	}

	checkf(held + admitted == RELIC_SYMBOL_MAX, SCENARIO_SYMBOLS,
	    "the pool should admit %d symbols, not %zu", RELIC_SYMBOL_MAX,
	    held + admitted);
	check(diagnosed(ctx, "too many distinct symbols and strings"),
	    SCENARIO_SYMBOLS, "the refusal should say the pool is full");

	status = relic_value_intern_bytes(ctx, RELIC_STRING, "new", 3, &dom);
	check(status != 0, SCENARIO_SYMBOLS,
	    "a string of new bytes should be refused by the full pool");

	symbol_text(text, sizeof(text), 0);
	status = relic_value_intern_bytes(ctx, RELIC_STRING, text,
	    strlen(text), &dom);
	check(status == 0 && relic_value_decode(ctx, dom, &decoded) &&
	    decoded.type == RELIC_STRING, SCENARIO_SYMBOLS,
	    "a string of bytes the pool holds should need no new entry");
	status = relic_value_intern_bytes(ctx, RELIC_SYMBOL, text,
	    strlen(text), &dom);
	check(status == 0 && dom == first, SCENARIO_SYMBOLS,
	    "the first symbol should intern to its identifier again");

	symbol_text(text, sizeof(text), admitted - 1);
	decoded_last = relic_value_decode(ctx, last, &decoded);
	check(decoded_last && decoded.type == RELIC_SYMBOL &&
	    strcmp(relic_string(ctx, decoded.v.id, NULL), text) == 0,
	    SCENARIO_SYMBOLS,
	    "the last symbol admitted should decode to its bytes");

	relic_destroy(ctx);
}

/*
 * Integers are interned until the table refuses one.  Exactly
 * RELIC_INTEGER_MAX are admitted, the refusal says why, and the integers
 * admitted keep their identifiers.
 */
static void
the_integer_table_stops_at_its_ceiling(void)
{
	relic_t *ctx;
	relic_value_t decoded;
	relic_dom_t first;
	relic_dom_t last;
	relic_dom_t dom;
	size_t admitted;
	bool decoded_last;
	int status;

	ctx = relic_create();
	check(ctx != NULL, SCENARIO_INTEGERS, "a context should be created");
	if (ctx == NULL)
		return;

	first = RELIC_ID_NONE;
	last = RELIC_ID_NONE;
	for (admitted = 0; admitted <= RELIC_INTEGER_MAX; admitted++) {
		if (relic_value_intern_integer(ctx, -(int64_t)admitted,
		    &dom) != 0)
			break;
		if (first == RELIC_ID_NONE)
			first = dom;
		last = dom;
	}

	checkf(admitted == RELIC_INTEGER_MAX, SCENARIO_INTEGERS,
	    "the table should admit %d integers, not %zu", RELIC_INTEGER_MAX,
	    admitted);
	check(diagnosed(ctx, "too many distinct integers"), SCENARIO_INTEGERS,
	    "the refusal should say the table is full");

	status = relic_value_intern_integer(ctx, 0, &dom);
	check(status == 0 && dom == first, SCENARIO_INTEGERS,
	    "the first integer should intern to its identifier again");
	decoded_last = relic_value_decode(ctx, last, &decoded);
	check(decoded_last && decoded.type == RELIC_INTEGER &&
	    decoded.v.integer == -(int64_t)(admitted - 1), SCENARIO_INTEGERS,
	    "the last integer admitted should decode to its value");

	relic_destroy(ctx);
}

static bool
diagnosed(relic_t *ctx, const char *expected)
{
	const char *reported;

	reported = relic_error(ctx);

	return (reported != NULL && strstr(reported, expected) != NULL);
}

/*
 * The text of the `ordinal`th symbol a test interns.
 */
static void
symbol_text(char *text, size_t size, size_t ordinal)
{
	(void)snprintf(text, size, "s%zu", ordinal);
}

int
main(void)
{
	the_integer_table_stops_at_its_ceiling();
	the_symbol_pool_stops_at_its_ceiling();

	return (check_summary("ceiling"));
}
