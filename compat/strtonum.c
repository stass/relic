/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include "compat.h"

#define	ERRSTR_INVALID		"invalid"
#define	ERRSTR_TOO_SMALL	"too small"
#define	ERRSTR_TOO_LARGE	"too large"

/*
 * Convert numstr to a long long that lies within [minval, maxval].
 *
 * Failure is reported through *errstrp rather than through the return value,
 * because every return value is a legitimate result and no sentinel is
 * available.  On success *errstrp is NULL and errno is left as the caller had
 * it, so converting a series of strings needs no errno bookkeeping between
 * them.  On failure the return value is 0, *errstrp is one of three words
 * suitable for interpolating into a diagnostic, and errno is EINVAL for a
 * malformed string or ERANGE for a well-formed one outside the interval.
 *
 * The whole string must be consumed.  Trailing characters are a failure and
 * not a partial conversion, which is the property that makes this the right
 * interface for validating a command-line operand: "5x" and "" are rejected
 * where strtoll(3) would accept the first and return zero for the second.
 */
long long
strtonum(const char *numstr, long long minval, long long maxval,
    const char **errstrp)
{
	char *endptr;
	const char *errstr;
	long long value;
	int conversion_errno;
	int failure_errno;
	int saved_errno;

	saved_errno = errno;
	errstr = NULL;
	failure_errno = 0;

	errno = 0;
	value = strtoll(numstr, &endptr, 10);
	conversion_errno = errno;

	/*
	 * An inverted interval admits no value at all, and a string that
	 * strtoll(3) did not consume entirely is not a number, so both are
	 * malformed input rather than a value out of range.
	 */
	if (minval > maxval || endptr == numstr || *endptr != '\0') {
		errstr = ERRSTR_INVALID;
		failure_errno = EINVAL;
	} else if (conversion_errno == ERANGE || value < minval ||
	    value > maxval) {
		/*
		 * strtoll(3) saturates at LLONG_MIN or LLONG_MAX for a string
		 * naming a value the type cannot hold, so the value still
		 * says which end of the interval was missed.  Saturation at
		 * LLONG_MIN has to be recognized explicitly, because a caller
		 * whose minimum is LLONG_MIN would otherwise see a value that
		 * is not below its minimum and be told the value was too
		 * large.
		 */
		if (value < minval ||
		    (conversion_errno == ERANGE && value == LLONG_MIN))
			errstr = ERRSTR_TOO_SMALL;
		else
			errstr = ERRSTR_TOO_LARGE;

		failure_errno = ERANGE;
	}

	if (errstr == NULL) {
		errno = saved_errno;
	} else {
		errno = failure_errno;
		value = 0;
	}

	if (errstrp != NULL)
		*errstrp = errstr;

	return (value);
}
