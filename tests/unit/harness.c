/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdarg.h>
#include <stdio.h>

#include "harness.h"

static unsigned int failures;

void
check(bool condition, const char *scenario, const char *expectation)
{
	if (condition)
		return;

	fprintf(stderr, "FAIL: %s: %s\n", scenario, expectation);
	failures++;
}

void
checkf(bool condition, const char *scenario, const char *format, ...)
{
	va_list arguments;

	if (condition)
		return;

	fprintf(stderr, "FAIL: %s: ", scenario);

	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);

	fputc('\n', stderr);
	failures++;
}

int
check_summary(const char *suite)
{
	if (failures == 0)
		return (0);

	fprintf(stderr, "%s: %u check(s) failed\n", suite, failures);

	return (1);
}
