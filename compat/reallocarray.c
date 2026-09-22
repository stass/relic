/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "compat.h"

/*
 * Resize an allocation to hold nmemb objects of size bytes.
 *
 * The product is what makes this interface worth having: computing it in the
 * caller lets an overflow wrap silently and produce an allocation far smaller
 * than the caller believes it obtained, which is how a bounds check that
 * looks correct ends up writing past the end of a buffer.  An overflowing
 * product here is reported as ENOMEM and no allocation is made or freed.
 */
void *
reallocarray(void *optr, size_t nmemb, size_t size)
{
	if (nmemb != 0 && size > SIZE_MAX / nmemb) {
		errno = ENOMEM;
		return (NULL);
	}

	return (realloc(optr, nmemb * size));
}
