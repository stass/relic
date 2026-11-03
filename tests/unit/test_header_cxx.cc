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
 * its search path, and calls entry points so that the linkage the wrapper
 * promises is exercised rather than merely declared.
 *
 * It also declares a variable of the error class's type.  The function and
 * the type have names of their own, so the variable needs no elaborated
 * `enum` specifier, which a type sharing the function's name would need in
 * C++.
 */

#include <relic.h>

int
main()
{
	const char *linked_version = relic_version();
	relic_error_class_t error_class;
	relic_t *ctx;

	if (linked_version == nullptr)
		return (1);

	ctx = relic_create();
	if (ctx == nullptr)
		return (1);

	error_class = relic_error_class(ctx);
	if (relic_destroy(ctx) != 0)
		return (1);

	return (error_class == RELIC_ERROR_NONE ? 0 : 1);
}
