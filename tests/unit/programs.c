/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "harness.h"
#include "programs.h"

typedef struct program_text program_text_t;

/* Program text being written.  `failed` is set when memory ran out. */
struct program_text {
	char	*bytes;
	size_t	 length;
	size_t	 capacity;
	bool	 failed;
};

static void	 append(program_text_t *text, const char *format, ...)
		    RELIC_TEST_PRINTFLIKE(2, 3);
static void	 append_f_padding(program_text_t *text, unsigned int arity);
static void	 append_p_atom(program_text_t *text, unsigned int arity,
		    unsigned int stepped);

/*
 * Append formatted text, growing the buffer to fit it.  A failure is
 * recorded rather than returned, so a program is written as a plain sequence
 * of appends and checked once at the end.
 */
static void
append(program_text_t *text, const char *format, ...)
{
	va_list args;
	char *bytes;
	size_t capacity;
	int length;

	if (text->failed)
		return;

	va_start(args, format);
	length = vsnprintf(NULL, 0, format, args);
	va_end(args);
	if (length < 0) {
		text->failed = true;
		return;
	}

	if (text->length + (size_t)length + 1 > text->capacity) {
		capacity = text->capacity == 0 ? 256 : text->capacity;
		while (text->length + (size_t)length + 1 > capacity)
			capacity *= 2;
		bytes = realloc(text->bytes, capacity);
		if (bytes == NULL) {
			text->failed = true;
			return;
		}
		text->bytes = bytes;
		text->capacity = capacity;
	}

	va_start(args, format);
	(void)vsnprintf(text->bytes + text->length,
	    text->capacity - text->length, format, args);
	va_end(args);
	text->length += (size_t)length;
}

/*
 * p(X1, ..., Xn), with Y in place of X`stepped` unless `stepped` is 0.
 */
static void
append_p_atom(program_text_t *text, unsigned int arity, unsigned int stepped)
{
	unsigned int column;

	append(text, "p(");
	for (column = 1; column <= arity; column++) {
		if (column > 1)
			append(text, ", ");
		if (column == stepped)
			append(text, "Y");
		else
			append(text, "X%u", column);
	}
	append(text, ")");
}

/*
 * The start of an f atom: "f(" and the constants that come before its last
 * two arguments.  There are two more of them than p has columns, so an f
 * literal is bound in more columns than any call on p and the planner
 * places it first in every body.
 */
static void
append_f_padding(program_text_t *text, unsigned int arity)
{
	unsigned int i;

	append(text, "f(");
	for (i = 0; i < arity + 2; i++)
		append(text, "c, ");
}

/*
 * The fact f(c, ..., c); a rule deriving p from f alone, binding every
 * column from the fact; and for each column k a rule stepping Xk through f
 * and calling p with Y in its place.  The planner places f first, which
 * binds Y, so that rule calls p with column k bound as well as every other
 * column its head was called with bound.
 */
char *
widening_program(unsigned int arity)
{
	program_text_t text = { NULL, 0, 0, false };
	unsigned int column;

	append_f_padding(&text, arity);
	append(&text, "c, c).\n");

	append_p_atom(&text, arity, 0);
	append(&text, " :- ");
	for (column = 1; column <= arity; column++) {
		if (column > 1)
			append(&text, ", ");
		append_f_padding(&text, arity);
		append(&text, "X%u, X%u)", column, column);
	}
	append(&text, ".\n");

	for (column = 1; column <= arity; column++) {
		append_p_atom(&text, arity, 0);
		append(&text, " :- ");
		append_f_padding(&text, arity);
		append(&text, "X%u, Y), ", column);
		append_p_atom(&text, arity, column);
		append(&text, ".\n");
	}

	if (text.failed) {
		free(text.bytes);
		return (NULL);
	}

	return (text.bytes);
}
