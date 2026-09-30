/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_STRFN_H
#define RELIC_STRFN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ast.h"

/*
 * The string functions (lib/strfn.c): what each takes and returns, and the
 * pure computations over bytes and integers that evaluating one comes to.
 *
 * The table is the one statement of each function's signature.  The parser
 * checks a constant argument against it, the planner reads a call's result
 * type from it, and the executor reads each argument as the type it names,
 * so no consumer of a call carries its own list of what `substr` takes.
 */

typedef struct relic_function_info relic_function_info_t;

/*
 * What a function takes in one argument position, or gives as its result:
 * a string, an integer, or, for an argument of `str`, any value.  A result
 * is never ANY.
 */
enum relic_value_kind {
	RELIC_KIND_STRING,
	RELIC_KIND_INTEGER,
	RELIC_KIND_ANY
};

/*
 * One function: how it is written, how many arguments it takes, the kind
 * each of them must be, and the kind of its result.
 */
struct relic_function_info {
	const char		*spelling;
	enum relic_value_kind	 args[RELIC_MAX_FUNCTION_ARITY];
	enum relic_value_kind	 result;
	uint8_t			 arity;
};

/*
 * relic_function_info() gives a function's entry.
 *
 * relic_function_arities() says whether `length` bytes spell a function at
 * any arity, and gives the least and the greatest arity of that name:
 * `substr` takes two or three, every other function one number.
 * relic_function_find() gives the function a name and an argument count
 * call, when they call one.  A name is a function only where a call may
 * stand, so neither reserves anything.
 */
const relic_function_info_t *relic_function_info(enum relic_function fn);
bool	 relic_function_arities(const char *name, size_t length,
	    unsigned int *leastp, unsigned int *mostp);
bool	 relic_function_find(const char *name, size_t length, size_t argc,
	    enum relic_function *fnp);

#endif /* RELIC_STRFN_H */
