/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_CDEFS_H
#define RELIC_CDEFS_H

/*
 * Annotations that ask the compiler for a check or tell it an intent, each
 * empty for a compiler that does not understand it.  They sit apart from
 * internal.h so that a module that depends on nothing else in the library,
 * such as the regular expression matcher, can use them without it.
 */

/*
 * Mark a parameter the implementation does not use, or a local or a static
 * function read only by an assertion, which NDEBUG leaves unread.  A function
 * whose shape is fixed by a callback type will have some; writing the attribute
 * is clearer than a (void) cast in the body, which reads like a statement that
 * does something.
 */
#if defined(__GNUC__)
#define	RELIC_UNUSED	__attribute__((__unused__))
#else
#define	RELIC_UNUSED
#endif

/*
 * Ask the compiler to check a printf-style call against its format string.
 * The diagnostic interface is the one place in the library where a format
 * string and its arguments are separated by a call boundary.
 */
#if defined(__GNUC__)
#define	RELIC_PRINTFLIKE(fmt, args) \
	__attribute__((__format__(__printf__, fmt, args)))
#else
#define	RELIC_PRINTFLIKE(fmt, args)
#endif

/*
 * Ask the compiler to reject a call whose result is ignored, for a function
 * whose result says whether its caller may go on.
 */
#if defined(__GNUC__)
#define	RELIC_MUST_CHECK	__attribute__((__warn_unused_result__))
#else
#define	RELIC_MUST_CHECK
#endif

#endif /* RELIC_CDEFS_H */
