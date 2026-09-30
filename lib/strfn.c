/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <strings.h>

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
