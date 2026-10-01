/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the computations the string functions come to
 * (lib/strfn.c).
 *
 * substr is checked at every combination of a start and a count drawn from
 * both extremes of int64_t and the positions around the string's ends,
 * against a reference that decides each position by the definition: kept
 * when it is at or past the start and, with a count, fewer than the count
 * past it, read over the integers.  index is checked at the cases the
 * definition singles out and against a byte-by-byte search over every pair
 * of short strings of a small alphabet that holds a NUL and a byte above
 * 0x7f.  The decimal text of an integer is checked against printf at both
 * extremes, and the concatenation's length at the length limit and one byte
 * past it.  Under UndefinedBehaviorSanitizer none of it may execute a signed
 * overflow.
 */

#include "config.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "strfn.h"

#define	SCENARIO_SUBSTR		"substr keeps the positions its interval holds"
#define	SCENARIO_INDEX		"index finds the least occurrence"
#define	SCENARIO_TEXT		"an integer's text is the decimal printf gives"
#define	SCENARIO_CAT		"a concatenation fits exactly up to the limit"

/*
 * The strings index is searched over: every string of up to SEARCH_LENGTH
 * bytes of the alphabet's bytes, as haystack and as needle.
 */
#define	SEARCH_LENGTH	5
#define	ALPHABET_SIZE	3

static const char alphabet[ALPHABET_SIZE] = { 'a', '\0', '\xe9' };

static bool	 kept_by_definition(int64_t position, int64_t start,
		    int64_t count, bool counted);
static void	 reference_range(size_t length, int64_t start, int64_t count,
		    bool counted, size_t *offsetp, size_t *keptp);
static int64_t	 reference_index(const char *haystack, size_t haystack_length,
		    const char *needle, size_t needle_length);
static size_t	 nth_string(size_t ordinal, char text[static SEARCH_LENGTH]);
static void	 substr_keeps_what_its_interval_holds(void);
static void	 index_finds_the_least_occurrence(void);
static void	 index_agrees_with_a_byte_search(void);
static void	 an_integers_text_is_its_decimal(void);
static void	 a_concatenation_fits_up_to_the_limit(void);

/*
 * Whether `substr` keeps `position`: it is at or past `start`, and, when
 * the call has a count, fewer than `count` positions past it.  The distance
 * from the start is taken in uint64_t, where it is exact for any two
 * int64_t values the first of which is not the greater, so the comparison
 * is over the integers and not over what int64_t holds.
 */
static bool
kept_by_definition(int64_t position, int64_t start, int64_t count,
    bool counted)
{
	uint64_t distance;

	if (position < start)
		return (false);
	if (!counted)
		return (true);
	if (count <= 0)
		return (false);

	distance = (uint64_t)position - (uint64_t)start;

	return (distance < (uint64_t)count);
}

/*
 * The offset and count of the bytes `substr` keeps, found by asking of each
 * position of the string whether it is kept.  The kept positions of an
 * interval are consecutive, so the first and the number kept describe them;
 * a call keeping none gives zero for both, as the function does.
 */
static void
reference_range(size_t length, int64_t start, int64_t count, bool counted,
    size_t *offsetp, size_t *keptp)
{
	size_t position;

	*offsetp = 0;
	*keptp = 0;
	for (position = 0; position < length; position++) {
		if (!kept_by_definition((int64_t)position, start, count,
		    counted))
			continue;
		if (*keptp == 0)
			*offsetp = position;
		(*keptp)++;
	}
}

/*
 * Every start and count drawn from the extremes and the positions around a
 * string's ends, over strings of several lengths, with a count and
 * without.
 */
static void
substr_keeps_what_its_interval_holds(void)
{
	static const size_t lengths[] = { 0, 1, 3, 10 };
	int64_t values[12];
	size_t expected_offset;
	size_t expected_kept;
	size_t offset;
	size_t kept;
	size_t length;
	size_t l;
	size_t s;
	size_t c;
	int64_t whole;
	bool counted;
	int with;

	for (l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
		length = lengths[l];
		whole = (int64_t)length;
		values[0] = INT64_MIN;
		values[1] = INT64_MIN + 1;
		values[2] = -whole - 1;
		values[3] = -2;
		values[4] = -1;
		values[5] = 0;
		values[6] = 1;
		values[7] = whole - 1;
		values[8] = whole;
		values[9] = whole + 1;
		values[10] = INT64_MAX - 1;
		values[11] = INT64_MAX;

		for (with = 0; with < 2; with++) {
			counted = with == 1;
			for (s = 0; s < 12; s++) {
				for (c = 0; c < 12; c++) {
					relic_substr_range(length, values[s],
					    values[c], counted, &offset, &kept);
					reference_range(length, values[s],
					    values[c], counted,
					    &expected_offset, &expected_kept);
					checkf(offset == expected_offset &&
					    kept == expected_kept,
					    SCENARIO_SUBSTR, "of %zu bytes "
					    "from %" PRId64 " %s %" PRId64
					    " should keep %zu at %zu, not %zu "
					    "at %zu", length, values[s],
					    counted ? "for" : "ignoring",
					    values[c], expected_kept,
					    expected_offset, kept, offset);
				}
			}
		}
	}
}

/*
 * The least position at which `needle` occurs in `haystack`, found by
 * comparing it at every position in turn.
 */
static int64_t
reference_index(const char *haystack, size_t haystack_length,
    const char *needle, size_t needle_length)
{
	size_t position;

	if (needle_length > haystack_length)
		return (-1);

	for (position = 0; position + needle_length <= haystack_length;
	    position++) {
		if (memcmp(&haystack[position], needle, needle_length) == 0)
			return ((int64_t)position);
	}

	return (-1);
}

/*
 * The cases the definition names: the empty needle, which occurs at 0 of
 * every haystack, the empty one among them; a needle longer than the
 * haystack; a needle at the start and at the end; and a needle overlapping
 * itself, found where its second attempt begins.
 */
static void
index_finds_the_least_occurrence(void)
{
	static const struct {
		const char	*haystack;
		size_t		 haystack_length;
		const char	*needle;
		size_t		 needle_length;
		int64_t		 expected;
	} cases[] = {
		{ "", 0, "", 0, 0 },
		{ "abc", 3, "", 0, 0 },
		{ "ab", 2, "abc", 3, -1 },
		{ "", 0, "a", 1, -1 },
		{ "abc", 3, "ab", 2, 0 },
		{ "abc", 3, "bc", 2, 1 },
		{ "abc", 3, "abc", 3, 0 },
		{ "aaab", 4, "aab", 3, 1 },
		{ "abababc", 7, "ababc", 5, 2 },
		{ "a\0b\0c", 5, "\0c", 2, 3 },
		{ "caf\xc3\xa9", 5, "\xa9", 1, 4 },
		{ "a\nb", 3, "\n", 1, 1 },
		{ "abc", 3, "d", 1, -1 }
	};
	int64_t found;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		found = relic_index_of(cases[i].haystack,
		    cases[i].haystack_length, cases[i].needle,
		    cases[i].needle_length);
		checkf(found == cases[i].expected, SCENARIO_INDEX,
		    "case %zu should give %" PRId64 ", not %" PRId64, i,
		    cases[i].expected, found);
	}
}

/*
 * The string numbered `ordinal` among all strings of the alphabet, shortest
 * first, written into `text`, and its length.
 */
static size_t
nth_string(size_t ordinal, char text[static SEARCH_LENGTH])
{
	size_t length;
	size_t count;
	size_t i;

	length = 0;
	count = 1;
	while (ordinal >= count) {
		ordinal -= count;
		length++;
		count *= ALPHABET_SIZE;
	}

	for (i = 0; i < length; i++) {
		text[i] = alphabet[ordinal % ALPHABET_SIZE];
		ordinal /= ALPHABET_SIZE;
	}

	return (length);
}

/*
 * Every pair of strings of the alphabet up to SEARCH_LENGTH bytes, each as
 * haystack and as needle, gives the position a byte-by-byte search does.
 */
static void
index_agrees_with_a_byte_search(void)
{
	char haystack[SEARCH_LENGTH];
	char needle[SEARCH_LENGTH];
	size_t haystack_length;
	size_t needle_length;
	size_t strings;
	size_t count;
	size_t length;
	size_t h;
	size_t n;
	int64_t expected;
	int64_t found;
	bool agreed;

	strings = 0;
	count = 1;
	for (length = 0; length <= SEARCH_LENGTH; length++) {
		strings += count;
		count *= ALPHABET_SIZE;
	}

	agreed = true;
	for (h = 0; h < strings && agreed; h++) {
		haystack_length = nth_string(h, haystack);
		for (n = 0; n < strings && agreed; n++) {
			needle_length = nth_string(n, needle);
			expected = reference_index(haystack, haystack_length,
			    needle, needle_length);
			found = relic_index_of(haystack, haystack_length,
			    needle, needle_length);
			agreed = found == expected;
			checkf(agreed, SCENARIO_INDEX, "haystack %zu and "
			    "needle %zu should give %" PRId64 ", not %" PRId64,
			    h, n, expected, found);
		}
	}
}

/*
 * The text is printf's decimal, at both extremes and between them, written
 * at the start of the buffer with its length returned.
 */
static void
an_integers_text_is_its_decimal(void)
{
	static const int64_t integers[] = {
		INT64_MIN, INT64_MIN + 1, -1000000007, -10, -9, -1, 0, 1, 9,
		10, 42, 1000000007, INT64_MAX - 1, INT64_MAX
	};
	char expected[RELIC_INTEGER_TEXT_MAX + 1];
	char text[RELIC_INTEGER_TEXT_MAX];
	size_t length;
	size_t i;

	for (i = 0; i < sizeof(integers) / sizeof(integers[0]); i++) {
		snprintf(expected, sizeof(expected), "%" PRId64, integers[i]);
		length = relic_integer_text(integers[i], text);
		checkf(length == strlen(expected) &&
		    memcmp(text, expected, length) == 0, SCENARIO_TEXT,
		    "%s should be written as itself, in %zu bytes", expected,
		    strlen(expected));
	}
}

/*
 * Two lengths whose sum is the longest a value holds have a concatenation,
 * of exactly that length; one byte more has none, and neither does a sum
 * that size_t cannot hold.
 */
static void
a_concatenation_fits_up_to_the_limit(void)
{
	size_t length;

	length = 0;
	check(relic_cat_fits(0, 0, &length) && length == 0, SCENARIO_CAT,
	    "two empty strings make an empty one");
	check(relic_cat_fits(RELIC_STRING_LENGTH_MAX - 5, 5, &length) &&
	    length == RELIC_STRING_LENGTH_MAX, SCENARIO_CAT,
	    "a concatenation of the longest length fits");
	check(relic_cat_fits(0, RELIC_STRING_LENGTH_MAX, &length) &&
	    length == RELIC_STRING_LENGTH_MAX, SCENARIO_CAT,
	    "the longest string after an empty one fits");
	check(!relic_cat_fits(RELIC_STRING_LENGTH_MAX - 5, 6, &length),
	    SCENARIO_CAT, "one byte past the longest length does not fit");
	check(!relic_cat_fits(RELIC_STRING_LENGTH_MAX, 1, &length),
	    SCENARIO_CAT, "one byte after the longest string does not fit");
	check(!relic_cat_fits(SIZE_MAX, 1, &length) &&
	    !relic_cat_fits(1, SIZE_MAX, &length) &&
	    !relic_cat_fits(SIZE_MAX, SIZE_MAX, &length), SCENARIO_CAT,
	    "a sum size_t cannot hold does not wrap into one that fits");
}

int
main(void)
{
	substr_keeps_what_its_interval_holds();
	index_finds_the_least_occurrence();
	index_agrees_with_a_byte_search();
	an_integers_text_is_its_decimal();
	a_concatenation_fits_up_to_the_limit();

	return (check_summary("strfn"));
}
