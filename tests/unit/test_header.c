/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * The public header's own contract.
 *
 * <relic.h> is included before anything else and the translation unit is
 * compiled with only the include directory on its search path, so it fails to
 * build if the header ever starts depending on config.h, on a
 * project-internal header, or on the caller having included something first.
 * The standard headers that follow serve the test's own code and cannot mask
 * a dependency, because the header has already been read by then.  Building
 * as strict C11 keeps the header free of dialect extensions that a consumer
 * compiling conformingly would reject.
 *
 * The runtime part checks that the header and the library agree about the
 * version, which they can stop doing whenever the library is upgraded
 * underneath a program that was compiled against an older header.
 */

#include <relic.h>

#include <stddef.h>

_Static_assert(RELIC_VERSION_NUMBER ==
    (RELIC_VERSION_MAJOR * 10000) + (RELIC_VERSION_MINOR * 100) +
    RELIC_VERSION_PATCH,
    "RELIC_VERSION_NUMBER must order the version components");

_Static_assert(RELIC_VERSION_MINOR < 100 && RELIC_VERSION_PATCH < 100,
    "the minor and patch fields are two decimal digits each");

/*
 * Compared byte by byte rather than with strcmp(3), to keep the set of
 * headers this test depends on as small as the header it is checking.
 */
static int
same_text(const char *left, const char *right)
{
	size_t i;

	for (i = 0; left[i] != '\0'; i++) {
		if (left[i] != right[i])
			return (0);
	}

	return (right[i] == '\0');
}

int
main(void)
{
	const char *linked_version;

	linked_version = relic_version();
	if (linked_version == NULL)
		return (1);

	if (!same_text(linked_version, RELIC_VERSION_STRING))
		return (1);

	return (0);
}
