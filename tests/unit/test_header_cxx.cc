/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * The public header is includable from C++.
 *
 * A C header reaches a C++ consumer only if its declarations are wrapped in
 * extern "C" and its macros expand to something C++ accepts.  Neither is
 * visible in a C build, so this translation unit exists to compile: it
 * includes <relic.h> before anything else, with only the include directory on
 * its search path, and calls the entry point so that the linkage the wrapper
 * promises is exercised rather than merely declared.
 */

#include <relic.h>

int
main()
{
	const char *linked_version = relic_version();

	return (linked_version == nullptr ? 1 : 0);
}
