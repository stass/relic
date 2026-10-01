/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_TEST_HARNESS_H
#define RELIC_TEST_HARNESS_H

#include <stdbool.h>

/*
 * Report a printf-style call's arguments against its format string.  The
 * harness is not part of the library and cannot include lib/internal.h, so
 * the attribute is spelled out here, as is the one below.
 */
#if defined(__GNUC__)
#define	RELIC_TEST_PRINTFLIKE(fmt, args) \
	__attribute__((__format__(__printf__, fmt, args)))
#else
#define	RELIC_TEST_PRINTFLIKE(fmt, args)
#endif

/*
 * Mark a parameter a callback's signature requires and its body does not
 * read, for a test that includes only the public header.
 */
#if defined(__GNUC__)
#define	RELIC_TEST_UNUSED	__attribute__((__unused__))
#else
#define	RELIC_TEST_UNUSED
#endif

/*
 * The whole of the unit test harness.
 *
 * A test states a scenario and what should have happened, and the failure
 * line names both, so a failing run says what was being tested and what it
 * expected without anybody opening a debugger.  Every check runs: a test
 * binary reports all of its failures in one go rather than stopping at the
 * first, because the second failure is often what explains the first.
 *
 * assert(3) is deliberately not used.  An ordinary optimized build defines
 * NDEBUG, which would compile every assertion away and leave a test that
 * passes without testing anything.
 */
void	check(bool condition, const char *scenario, const char *expectation);

/*
 * The same, for a check that stands for many cases.  A test that sweeps a
 * few thousand inputs must still say which input failed, so it records the
 * first offender and names it here; an expectation that read "every symbol
 * should name its bytes" and nothing more would send the reader to a
 * debugger to find out which symbol did not.
 */
void	checkf(bool condition, const char *scenario, const char *format, ...)
	    RELIC_TEST_PRINTFLIKE(3, 4);

/*
 * Report the result of the suite and return the status the test program
 * should exit with: zero when every check passed.
 */
int	check_summary(const char *suite);

#endif /* RELIC_TEST_HARNESS_H */
