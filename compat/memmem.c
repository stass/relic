/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "compat.h"

/*
 * The first occurrence of the `little_length` bytes at `little` among the
 * `big_length` bytes at `big`, or NULL when there is none.  The empty needle
 * is found at the start of every haystack, as FreeBSD and glibc find it;
 * macOS finds it nowhere, so no caller relies on either answer.
 *
 * Each position at which the needle could still fit is compared in turn, so
 * the search takes at worst the product of the two lengths, as the C
 * libraries' own worst case does, and reads no byte outside either range.
 */
void *
memmem(const void *big, size_t big_length, const void *little,
    size_t little_length)
{
	const unsigned char *haystack;
	size_t last;
	size_t i;

	haystack = big;
	if (little_length == 0)
		return ((void *)(uintptr_t)haystack);
	if (little_length > big_length)
		return (NULL);

	last = big_length - little_length;
	for (i = 0; i <= last; i++) {
		if (bcmp(haystack + i, little, little_length) == 0)
			return ((void *)(uintptr_t)(haystack + i));
	}

	return (NULL);
}
