/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "compat.h"
#include "strfn.h"

/*
 * The string functions, as strfn.h describes.
 *
 * Every function is total over operands of the kinds its entry names: given
 * strings where it wants strings and integers where it wants integers, it
 * has a value for every input.  Strings are bytes, never characters, and a
 * position is a byte offset counted from zero.
 */

/*
 * The functions, indexed by enum relic_function.  `substr` has two entries,
 * one per arity, under one spelling.
 */
static const relic_function_info_t functions[] = {
	[RELIC_FN_LENGTH] = {
		.spelling = "length",
		.args = { RELIC_KIND_STRING },
		.result = RELIC_KIND_INTEGER,
		.arity = 1
	},
	[RELIC_FN_CAT] = {
		.spelling = "cat",
		.args = { RELIC_KIND_STRING, RELIC_KIND_STRING },
		.result = RELIC_KIND_STRING,
		.arity = 2
	},
	[RELIC_FN_SUBSTR2] = {
		.spelling = "substr",
		.args = { RELIC_KIND_STRING, RELIC_KIND_INTEGER },
		.result = RELIC_KIND_STRING,
		.arity = 2
	},
	[RELIC_FN_SUBSTR3] = {
		.spelling = "substr",
		.args = { RELIC_KIND_STRING, RELIC_KIND_INTEGER,
		    RELIC_KIND_INTEGER },
		.result = RELIC_KIND_STRING,
		.arity = 3
	},
	[RELIC_FN_INDEX] = {
		.spelling = "index",
		.args = { RELIC_KIND_STRING, RELIC_KIND_STRING },
		.result = RELIC_KIND_INTEGER,
		.arity = 2
	},
	[RELIC_FN_MATCH] = {
		.spelling = "match",
		.args = { RELIC_KIND_STRING, RELIC_KIND_STRING },
		.result = RELIC_KIND_INTEGER,
		.arity = 2
	},
	[RELIC_FN_STR] = {
		.spelling = "str",
		.args = { RELIC_KIND_ANY },
		.result = RELIC_KIND_STRING,
		.arity = 1
	}
};

#define	FUNCTION_COUNT	(sizeof(functions) / sizeof(functions[0]))

_Static_assert(FUNCTION_COUNT == RELIC_FN_STR + 1,
    "every function has an entry");

static bool	 spells(const relic_function_info_t *info, const char *name,
		    size_t length);

const relic_function_info_t *
relic_function_info(enum relic_function fn)
{
	return (&functions[fn]);
}

/*
 * Whether `length` bytes are exactly a function's spelling.
 */
static bool
spells(const relic_function_info_t *info, const char *name, size_t length)
{
	size_t i;

	for (i = 0; i < length; i++) {
		if (info->spelling[i] == '\0' || info->spelling[i] != name[i])
			return (false);
	}

	return (info->spelling[length] == '\0');
}

bool
relic_function_arities(const char *name, size_t length, unsigned int *leastp,
    unsigned int *mostp)
{
	unsigned int least;
	unsigned int most;
	size_t i;

	least = RELIC_MAX_FUNCTION_ARITY + 1;
	most = 0;
	for (i = 0; i < FUNCTION_COUNT; i++) {
		if (!spells(&functions[i], name, length))
			continue;
		if (functions[i].arity < least)
			least = functions[i].arity;
		if (functions[i].arity > most)
			most = functions[i].arity;
	}

	if (most == 0)
		return (false);

	*leastp = least;
	*mostp = most;

	return (true);
}

bool
relic_function_find(const char *name, size_t length, size_t argc,
    enum relic_function *fnp)
{
	size_t i;

	for (i = 0; i < FUNCTION_COUNT; i++) {
		if (functions[i].arity == argc &&
		    spells(&functions[i], name, length)) {
			*fnp = (enum relic_function)i;
			return (true);
		}
	}

	return (false);
}

/*
 * The positions kept are those of the interval [start, start + count), or
 * [start, infinity) when uncounted, that lie in [0, length), read over the
 * integers.  The intersection is found without forming start + count or
 * -start, either of which int64_t may not hold:
 *
 *	a start at or past the length, or a count at most zero, keeps
 *	nothing;
 *	a negative start keeps nothing when start <= -count, where -count is
 *	defined because count is positive; otherwise the interval is cut to
 *	begin at zero, and count + start, a sum of a positive and a negative
 *	integer, is how many of its positions remain;
 *	from the start so fixed, at most length - start bytes remain in the
 *	string, and the count bounds them when there is one.
 *
 * A length is at most RELIC_STRING_LENGTH_MAX, so it and every offset into
 * the string are held exactly by int64_t.
 */
void
relic_substr_range(size_t length, int64_t start, int64_t count, bool counted,
    size_t *offsetp, size_t *keptp)
{
	int64_t whole;
	int64_t kept;

	*offsetp = 0;
	*keptp = 0;
	whole = (int64_t)length;

	if (start >= whole || (counted && count <= 0))
		return;

	if (start < 0) {
		if (counted) {
			if (start <= -count)
				return;
			count += start;
		}
		start = 0;
	}

	kept = whole - start;
	if (counted && count < kept)
		kept = count;

	*offsetp = (size_t)start;
	*keptp = (size_t)kept;
}

/*
 * The empty needle occurs at every position, so its least is zero, and a
 * needle longer than the haystack occurs at none; both are answered before
 * memmem(3) is asked, so that neither depends on what a C library makes of
 * them.
 */
int64_t
relic_index_of(const char *haystack, size_t haystack_length,
    const char *needle, size_t needle_length)
{
	const char *found;

	if (needle_length == 0)
		return (0);
	if (needle_length > haystack_length)
		return (-1);

	found = memmem(haystack, haystack_length, needle, needle_length);
	if (found == NULL)
		return (-1);

	return ((int64_t)(found - haystack));
}

/*
 * The digits come least significant first, so they are formed from the end
 * of a buffer of their own backwards and then copied to the front of
 * `text`.  The magnitude is taken in uint64_t, where negating INT64_MIN is
 * exact, since no int64_t holds its magnitude.
 */
size_t
relic_integer_text(int64_t integer, char text[static RELIC_INTEGER_TEXT_MAX])
{
	char digits[RELIC_INTEGER_TEXT_MAX];
	uint64_t magnitude;
	size_t start;

	if (integer < 0)
		magnitude = 0 - (uint64_t)integer;
	else
		magnitude = (uint64_t)integer;

	start = sizeof(digits);
	do {
		start--;
		digits[start] = (char)('0' + magnitude % 10);
		magnitude /= 10;
	} while (magnitude > 0);

	if (integer < 0) {
		start--;
		digits[start] = '-';
	}

	bcopy(&digits[start], text, sizeof(digits) - start);

	return (sizeof(digits) - start);
}

/*
 * Each length is at most RELIC_STRING_LENGTH_MAX, which is below
 * SIZE_MAX / 2 on every size_t this builds on, so comparing one with what
 * the other leaves of the maximum decides the sum without forming it.
 */
bool
relic_cat_fits(size_t left, size_t right, size_t *lengthp)
{
	if (left > RELIC_STRING_LENGTH_MAX ||
	    right > RELIC_STRING_LENGTH_MAX - left)
		return (false);

	*lengthp = left + right;

	return (true);
}
