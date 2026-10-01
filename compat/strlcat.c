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
 * Append src to the string in a dst of dstsize bytes, always terminating the
 * result.
 *
 * The return value is the length the concatenation would have needed, so a
 * caller detects truncation by testing it against dstsize.
 *
 * A dst holding no terminator within dstsize is not a string.  There is no
 * position to append at and nothing to terminate, so the destination is left
 * exactly as it was and the result length is defined as dstsize plus the
 * length of src, which is greater than dstsize and therefore reports
 * truncation to a caller that checks.
 */
size_t
strlcat(char *dst, const char *src, size_t dstsize)
{
	size_t copylen;
	size_t dstlen;
	size_t srclen;

	for (dstlen = 0; dstlen < dstsize && dst[dstlen] != '\0'; dstlen++)
		;

	srclen = strlen(src);

	if (dstlen == dstsize)
		return (dstsize + srclen);

	copylen = srclen;
	if (copylen > dstsize - dstlen - 1)
		copylen = dstsize - dstlen - 1;

	bcopy(src, dst + dstlen, copylen);
	dst[dstlen + copylen] = '\0';

	return (dstlen + srclen);
}
