/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_H
#define RELIC_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The version relic was compiled against.  Only the three components are
 * written down; the string and the ordered number are derived from them, so
 * a release that bumps one of them cannot leave the others describing the
 * previous version.
 *
 * RELIC_VERSION_NUMBER orders releases so that a consumer can require a
 * minimum version in one comparison:
 *
 *	#if RELIC_VERSION_NUMBER < 10200
 *	#error relic 1.2.0 or later is required
 *	#endif
 *
 * The minor and patch fields are allotted two decimal digits each, so a
 * release must keep them below 100.
 */
#define	RELIC_VERSION_MAJOR	0
#define	RELIC_VERSION_MINOR	1
#define	RELIC_VERSION_PATCH	0

#define	RELIC_VERSION_TEXT_(v)	#v
#define	RELIC_VERSION_TEXT(v)	RELIC_VERSION_TEXT_(v)

#define	RELIC_VERSION_STRING \
	RELIC_VERSION_TEXT(RELIC_VERSION_MAJOR) "." \
	RELIC_VERSION_TEXT(RELIC_VERSION_MINOR) "." \
	RELIC_VERSION_TEXT(RELIC_VERSION_PATCH)

#define	RELIC_VERSION_NUMBER \
	((RELIC_VERSION_MAJOR * 10000) + (RELIC_VERSION_MINOR * 100) + \
	 RELIC_VERSION_PATCH)

/*
 * Mark a symbol as part of the library's public interface.  The library is
 * compiled with hidden visibility where the compiler supports it, so a
 * declaration that omits this would resolve within the library and be
 * unreachable from a program linked against it.  Expanding to nothing on a
 * compiler without visibility attributes is correct: there, nothing was
 * hidden in the first place.
 */
#if defined(__GNUC__) && __GNUC__ >= 4
#define	RELIC_EXPORT	__attribute__((__visibility__("default")))
#else
#define	RELIC_EXPORT
#endif

/*
 * The version of the library the caller is linked against, formatted as
 * "major.minor.patch".  It need not equal RELIC_VERSION_STRING: that macro
 * records the version the caller was compiled against, and a shared library
 * may since have been upgraded underneath it.  The returned string is static
 * and outlives the caller.
 */
RELIC_EXPORT const char *relic_version(void);

#ifdef __cplusplus
}
#endif

#endif /* RELIC_H */
