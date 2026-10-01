/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <string.h>
#include <strings.h>

#include "compat.h"

/*
 * Copy src into a dst of dstsize bytes, always terminating the result.
 *
 * The return value is the length of src, which is the length the copy would
 * have needed rather than the length it achieved: a caller detects truncation
 * by testing the result against dstsize, and no caller has to inspect dst to
 * find out what happened.  A dstsize of zero writes nothing at all, so dst
 * may be unwritable in that case and is not required to be a string.
 */
size_t
strlcpy(char *dst, const char *src, size_t dstsize)
{
	size_t copylen;
	size_t srclen;

	srclen = strlen(src);

	if (dstsize == 0)
		return (srclen);

	copylen = srclen;
	if (copylen > dstsize - 1)
		copylen = dstsize - 1;

	bcopy(src, dst, copylen);
	dst[copylen] = '\0';

	return (srclen);
}
