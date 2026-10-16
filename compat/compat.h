/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_COMPAT_H
#define RELIC_COMPAT_H

/*
 * Declarations for the BSD interfaces this platform's headers do not
 * declare.  Each interface has a replacement under compat/ that is compiled
 * where the C library lacks it, so the rest of the tree calls the interface
 * unconditionally and contains no platform test.  A MemorySanitizer build
 * compiles private strlcat and strlcpy replacements everywhere: sanitizer
 * interception of the C library's copies varies between runtimes. config.h
 * gives them private names before the system headers declare them.
 *
 * An interface is declared here exactly where configure found no system
 * declaration of it, which is a separate question from whether it links, so
 * a declaration never repeats the system's.
 *
 * The test suite also compiles every replacement on every platform, under a
 * private name given by renaming the interface on the command line, so that
 * relic-owned code is read by the compiler and the sanitizers everywhere
 * instead of only on the platforms that happen to link it.  The renaming
 * applies to the system's declaration as much as to this header's, so each
 * replacement is declared under its private name either way.
 *
 * config.h defines the HAVE_DECL_ macros and must already have been
 * included.  It is the first include of every source file in the tree, so
 * this header does not include it; without it every macro would be
 * undefined, which -Wundef reports and the check below turns into a compile
 * error, PACKAGE_NAME being a macro config.h always defines.
 */

#ifndef PACKAGE_NAME
#error "config.h must be included before compat.h"
#endif

#include <stddef.h>

/*
 * The headers that declare these interfaces where a platform does, so that
 * a caller can include this header and nothing else.  getentropy(2) is
 * declared in one of two places depending on the platform.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_SYS_RANDOM_H
#include <sys/random.h>
#endif

#if !HAVE_DECL_GETENTROPY
int getentropy(void *buf, size_t buflen);
#endif

#if !HAVE_DECL_MEMMEM
void *memmem(const void *big, size_t big_length, const void *little,
    size_t little_length);
#endif

#if !HAVE_DECL_REALLOCARRAY
void *reallocarray(void *optr, size_t nmemb, size_t size);
#endif

#if !HAVE_DECL_STRLCAT
size_t strlcat(char *dst, const char *src, size_t dstsize);
#endif

#if !HAVE_DECL_STRLCPY
size_t strlcpy(char *dst, const char *src, size_t dstsize);
#endif

#if !HAVE_DECL_STRTONUM
long long strtonum(const char *numstr, long long minval, long long maxval,
    const char **errstrp);
#endif

#endif /* RELIC_COMPAT_H */
