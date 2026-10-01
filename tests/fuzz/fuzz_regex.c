/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "fuzz.h"
#include "regex.h"

/*
 * The regex target: an input is a pattern and a string, compiled and
 * matched by the matcher `match` runs (lib/regex.c).
 *
 * The first byte is the pattern's length, cut to what the input holds; the
 * pattern follows, and the string is the rest.  Every byte may appear in
 * either, NUL and the metacharacters among them.
 *
 * A pattern must compile into no more instructions than its room, or be
 * refused with a failure found inside the pattern; a compiled program must
 * match the string or not, reporting a start within it.  A crash, a
 * sanitizer report, a hang or a broken bound is the finding.
 */
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	relic_regex_error_t error;
	relic_regex_inst_t *insts;
	relic_regex_work_t work;
	const char *pattern;
	const char *text;
	size_t pattern_length;
	size_t text_length;
	size_t capacity;
	uint32_t count;
	int64_t start;

	if (size == 0)
		return (0);

	pattern_length = data[0];
	if (pattern_length > size - 1)
		pattern_length = size - 1;
	pattern = (const char *)data + 1;
	text = pattern + pattern_length;
	text_length = size - 1 - pattern_length;

	capacity = relic_regex_capacity(pattern_length);
	insts = calloc(capacity, sizeof(*insts));
	work.words = calloc(capacity * RELIC_REGEX_WORK_WORDS,
	    sizeof(*work.words));
	work.capacity = capacity;
	work.stamp = 0;
	if (insts == NULL || work.words == NULL)
		abort();

	count = relic_regex_compile(pattern, pattern_length, insts, &error);
	if (count > capacity)
		abort();

	/*
	 * The empty pattern compiles, so a failure is always found at a byte
	 * of the pattern.
	 */
	if (count == 0) {
		if (error.position >= pattern_length)
			abort();
	} else {
		start = relic_regex_match(insts, count, &work, text,
		    text_length);
		if (start < -1 || start > (int64_t)text_length)
			abort();
	}

	free(insts);
	free(work.words);

	return (0);
}
