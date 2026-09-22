/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_COMPAT_H
#define RELIC_COMPAT_H

/*
 * Declarations for the BSD interfaces whose absence configure detected on
 * this platform.  Each has a replacement under compat/ that is compiled only
 * where the matching HAVE_ macro is undefined, so the rest of the tree calls
 * the interface unconditionally and contains no platform test.
 *
 * A build that defines RELIC_COMPAT_DECLARE_ALL gets every declaration
 * whatever the platform provides.  The test suite uses it to compile all of
 * the replacements everywhere, under private names, so that relic-owned code
 * is read by the compiler and the sanitizers on every platform instead of
 * only on the ones that happen to link it.
 *
 * config.h defines those macros and must already have been included.  It is
 * the first include of every source file in the tree, so this header does not
 * include it; without it every HAVE_ macro would be undefined, every
 * declaration below would be emitted, and a platform that does provide an
 * interface would see a second declaration of it.  The check below turns that
 * into a compile error instead, PACKAGE_NAME being a macro config.h always
 * defines.
 */

#ifndef PACKAGE_NAME
#error "config.h must be included before compat.h"
#endif

#include <stddef.h>

#if defined(RELIC_COMPAT_DECLARE_ALL) || !defined(HAVE_REALLOCARRAY)
void *reallocarray(void *optr, size_t nmemb, size_t size);
#endif

#if defined(RELIC_COMPAT_DECLARE_ALL) || !defined(HAVE_STRLCAT)
size_t strlcat(char *dst, const char *src, size_t dstsize);
#endif

#if defined(RELIC_COMPAT_DECLARE_ALL) || !defined(HAVE_STRLCPY)
size_t strlcpy(char *dst, const char *src, size_t dstsize);
#endif

#if defined(RELIC_COMPAT_DECLARE_ALL) || !defined(HAVE_STRTONUM)
long long strtonum(const char *numstr, long long minval, long long maxval,
    const char **errstrp);
#endif

#endif /* RELIC_COMPAT_H */
