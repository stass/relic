/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the BSD interfaces in compat/.
 *
 * configure compiles a replacement from compat/ only on a platform whose C
 * library lacks the interface, so what these tests exercise is whichever
 * implementation the rest of the tree will call here.  That is the property
 * worth checking: engine code calls strlcpy(3) and strtonum(3) without
 * knowing which one it got, and must see one behaviour on every platform.
 */

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "compat.h"
#include "harness.h"

/*
 * A byte no test writes deliberately, used to fill a destination buffer
 * before a call so that a write past the end of what the call was permitted
 * to touch is visible afterwards.
 */
#define	SENTINEL	'#'

static void	fill_with_sentinel(char *buffer, size_t size);
static bool	tail_is_untouched(const char *buffer, size_t size,
		    size_t writable);

static void	an_empty_source_changes_nothing(void);
static void	getentropy_fills_exactly_the_bytes_asked_for(void);
static void	getentropy_refuses_more_than_it_may_hand_out(void);
static void	memmem_finds_the_first_occurrence(void);
static void	memmem_reads_only_the_ranges_it_is_given(void);
static void	reallocarray_accepts_a_count_of_zero(void);
static void	reallocarray_preserves_contents_when_it_grows(void);
static void	reallocarray_refuses_an_overflowing_product(void);
static void	strlcat_appends_when_the_result_fits(void);
static void	strlcat_fills_the_destination_exactly(void);
static void	strlcat_with_no_room_writes_nothing(void);
static void	strlcat_leaves_an_unterminated_destination_alone(void);
static void	strlcat_truncates_and_reports_the_needed_length(void);
static void	strlcpy_copies_a_string_that_fits(void);
static void	strlcpy_fills_the_destination_exactly(void);
static void	strlcpy_truncates_and_reports_the_needed_length(void);
static void	strlcpy_with_no_room_writes_nothing(void);
static void	strtonum_accepts_a_value_inside_the_interval(void);
static void	strtonum_accepts_leading_space_and_sign(void);
static void	strtonum_accepts_the_bounds_of_the_interval(void);
static void	strtonum_converts_the_extremes_of_long_long(void);
static void	strtonum_rejects_malformed_input(void);
static void	strtonum_rejects_values_outside_the_interval(void);

static void
fill_with_sentinel(char *buffer, size_t size)
{
	memset(buffer, SENTINEL, size);
}

/*
 * Report whether the bytes beyond the first `writable` of `buffer` still hold
 * the sentinel.  `writable` is the number of bytes the call under test was
 * entitled to write, terminator included.
 */
static bool
tail_is_untouched(const char *buffer, size_t size, size_t writable)
{
	size_t i;

	for (i = writable; i < size; i++) {
		if (buffer[i] != SENTINEL)
			return (false);
	}

	return (true);
}

/*
 * Two draws of 32 bytes are equal with probability 2^-256, so equal draws
 * mean the bytes did not come from a random source.  The largest request the
 * interface admits, 256 bytes, must succeed as well, and neither call may
 * write past the bytes it was asked for.
 */
static void
getentropy_fills_exactly_the_bytes_asked_for(void)
{
	const char *scenario = "getentropy fills exactly the bytes asked for";
	char first[40];
	char second[40];
	char largest[264];

	fill_with_sentinel(first, sizeof(first));
	fill_with_sentinel(second, sizeof(second));
	fill_with_sentinel(largest, sizeof(largest));

	check(getentropy(first, 32) == 0 && getentropy(second, 32) == 0,
	    scenario, "succeeds for 32 bytes");
	check(bcmp(first, second, 32) != 0, scenario,
	    "gives different bytes to two requests");
	check(tail_is_untouched(first, sizeof(first), 32), scenario,
	    "writes nothing beyond the bytes asked for");
	check(getentropy(largest, 256) == 0, scenario,
	    "succeeds for the largest request it admits");
	check(tail_is_untouched(largest, sizeof(largest), 256), scenario,
	    "writes nothing beyond the largest request");
}

/*
 * A request beyond 256 bytes is refused whole rather than filled in part,
 * so a caller never mistakes a partial buffer for a seed.
 */
static void
getentropy_refuses_more_than_it_may_hand_out(void)
{
	const char *scenario = "getentropy refuses more than 256 bytes";
	char buffer[257];

	check(getentropy(buffer, sizeof(buffer)) == -1, scenario,
	    "fails for 257 bytes");
}

/*
 * The first occurrence is found wherever it is -- at the start, at the end,
 * overlapping an earlier partial one -- and a needle that occurs nowhere, or
 * is longer than the haystack, is not found.  Bytes are compared as bytes,
 * a NUL and a byte above 0x7f among them.  What an empty needle gives is
 * not asked: the C libraries disagree, and the caller answers it itself.
 */
static void
memmem_finds_the_first_occurrence(void)
{
	const char *scenario = "memmem finds the first occurrence";
	const char haystack[] = "aabaab\0\xff" "ab";
	const size_t length = sizeof(haystack) - 1;

	check(memmem(haystack, length, "aab", 3) == haystack, scenario,
	    "finds a needle at the start");
	check(memmem(haystack, length, "ab", 2) == haystack + 1, scenario,
	    "finds the first of several occurrences");
	check(memmem(haystack, length, "baa", 3) == haystack + 2, scenario,
	    "finds a needle overlapping an earlier partial match");
	check(memmem(haystack, length, "\0\xff" "ab", 4) == haystack + 6,
	    scenario, "finds a needle holding a NUL at the end");
	check(memmem(haystack, length, "abc", 3) == NULL, scenario,
	    "does not find a needle that is absent");
	check(memmem("ab", 2, "abc", 3) == NULL, scenario,
	    "does not find a needle longer than the haystack");
}

/*
 * The lengths bound what is read: a needle that would match only by reading
 * one byte past the haystack's length is not found, and neither is one
 * whose bytes past its own length would differ.  Each range is copied into
 * an allocation of exactly its size, so a read past either is one the
 * sanitizers report.
 */
static void
memmem_reads_only_the_ranges_it_is_given(void)
{
	const char *scenario = "memmem reads only the ranges it is given";
	char *haystack;
	char *needle;

	haystack = malloc(4);
	needle = malloc(2);
	check(haystack != NULL && needle != NULL, scenario,
	    "the ranges should be allocated");
	if (haystack == NULL || needle == NULL) {
		free(haystack);
		free(needle);
		return;
	}

	bcopy("abcd", haystack, 4);
	bcopy("dx", needle, 2);
	check(memmem(haystack, 3, needle, 1) == NULL, scenario,
	    "does not find a byte just past the haystack");
	check(memmem(haystack, 4, needle, 1) == haystack + 3, scenario,
	    "finds the needle's first byte at the haystack's last");
	check(memmem(haystack, 4, needle, 2) == NULL, scenario,
	    "does not find a needle running past the haystack");

	free(haystack);
	free(needle);
}

static void
strlcpy_copies_a_string_that_fits(void)
{
	const char *scenario = "strlcpy copies a string that fits";
	char destination[16];
	size_t needed;

	fill_with_sentinel(destination, sizeof(destination));
	needed = strlcpy(destination, "edge", sizeof(destination));

	check(needed == 4, scenario, "returns the length of the source");
	check(strcmp(destination, "edge") == 0, scenario,
	    "leaves the source in the destination");
	check(tail_is_untouched(destination, sizeof(destination), 5), scenario,
	    "writes nothing beyond the terminator");
}

static void
strlcpy_truncates_and_reports_the_needed_length(void)
{
	const char *scenario = "strlcpy truncates a source that does not fit";
	char destination[4];
	size_t needed;

	fill_with_sentinel(destination, sizeof(destination));
	needed = strlcpy(destination, "reachable", sizeof(destination));

	check(needed == 9, scenario,
	    "returns the length the copy would have needed");
	check(strcmp(destination, "rea") == 0, scenario,
	    "fills the destination and terminates it");
}

static void
strlcpy_with_no_room_writes_nothing(void)
{
	const char *scenario = "strlcpy with a size of zero writes nothing";
	char destination[4];
	size_t needed;

	fill_with_sentinel(destination, sizeof(destination));
	needed = strlcpy(destination, "path", 0);

	check(needed == 4, scenario, "returns the length of the source");
	check(tail_is_untouched(destination, sizeof(destination), 0), scenario,
	    "leaves the destination untouched");
}

/*
 * An exact fit is where an off-by-one lives: one byte fewer and the result is
 * truncated, one byte more and the terminator lands outside the destination.
 */
static void
strlcpy_fills_the_destination_exactly(void)
{
	const char *scenario = "strlcpy fills the destination exactly";
	char destination[8];
	size_t stated_size;
	size_t needed;

	stated_size = 4;
	fill_with_sentinel(destination, sizeof(destination));
	needed = strlcpy(destination, "abc", stated_size);

	check(needed == 3, scenario, "returns the length of the source");
	check(strcmp(destination, "abc") == 0, scenario,
	    "copies the whole source");
	check(tail_is_untouched(destination, sizeof(destination), stated_size),
	    scenario, "writes nothing beyond the stated size");
}

static void
strlcat_fills_the_destination_exactly(void)
{
	const char *scenario = "strlcat fills the destination exactly";
	char destination[12];
	size_t stated_size;
	size_t needed;

	stated_size = 6;
	fill_with_sentinel(destination, sizeof(destination));
	strlcpy(destination, "ab", stated_size);
	needed = strlcat(destination, "cde", stated_size);

	check(needed == 5, scenario, "returns the combined length");
	check(strcmp(destination, "abcde") == 0, scenario,
	    "appends the whole source");
	check(tail_is_untouched(destination, sizeof(destination), stated_size),
	    scenario, "writes nothing beyond the stated size");
}

/*
 * A stated size of zero leaves no room for even a terminator.  It is the
 * input that makes the remaining-space arithmetic wrap if the guard against
 * an unterminated destination is missing, since the scan ends with the
 * measured length equal to the size.
 */
static void
strlcat_with_no_room_writes_nothing(void)
{
	const char *scenario = "strlcat with a size of zero writes nothing";
	char destination[8];
	size_t needed;

	fill_with_sentinel(destination, sizeof(destination));
	destination[0] = '\0';

	needed = strlcat(destination, "x", 0);

	check(needed == 1, scenario, "returns the length of the source");
	check(destination[0] == '\0', scenario,
	    "leaves the destination untouched");
	check(tail_is_untouched(destination, sizeof(destination), 1), scenario,
	    "writes nothing at all");
}

static void
an_empty_source_changes_nothing(void)
{
	const char *scenario = "an empty source changes nothing";
	char destination[8];
	size_t needed;

	fill_with_sentinel(destination, sizeof(destination));
	needed = strlcpy(destination, "", sizeof(destination));

	check(needed == 0, scenario, "strlcpy returns a length of zero");
	check(destination[0] == '\0', scenario,
	    "strlcpy leaves an empty string");

	fill_with_sentinel(destination, sizeof(destination));
	strlcpy(destination, "ab", sizeof(destination));
	needed = strlcat(destination, "", sizeof(destination));

	check(needed == 2, scenario,
	    "strlcat returns the length already present");
	check(strcmp(destination, "ab") == 0, scenario,
	    "strlcat leaves the destination as it was");
}

/*
 * The interval is inclusive at both ends, so a value equal to a bound is
 * accepted rather than rejected; an interval of one value is the case where
 * getting that wrong rejects everything.
 */
static void
strtonum_accepts_the_bounds_of_the_interval(void)
{
	const char *scenario = "strtonum accepts the bounds of the interval";
	const char *errstr;
	long long value;

	errstr = "unset";
	value = strtonum("1", 1, 10, &errstr);
	check(value == 1 && errstr == NULL, scenario, "accepts the minimum");

	errstr = "unset";
	value = strtonum("10", 1, 10, &errstr);
	check(value == 10 && errstr == NULL, scenario, "accepts the maximum");

	errstr = "unset";
	value = strtonum("5", 5, 5, &errstr);
	check(value == 5 && errstr == NULL, scenario,
	    "accepts the only value of a single-value interval");
}

/*
 * The conversion is strtoll(3)'s, so it takes the leading space and the
 * explicit sign that strtoll accepts.  This is behaviour a caller validating
 * an operand relies on either way, so it is pinned rather than left to be
 * discovered.
 */
static void
strtonum_accepts_leading_space_and_sign(void)
{
	const char *scenario = "strtonum accepts leading space and a sign";
	const char *errstr;
	long long value;

	errstr = "unset";
	value = strtonum("  42", 0, 100, &errstr);
	check(value == 42 && errstr == NULL, scenario,
	    "skips leading whitespace");

	errstr = "unset";
	value = strtonum("+42", 0, 100, &errstr);
	check(value == 42 && errstr == NULL, scenario,
	    "accepts an explicit plus sign");

	errstr = "unset";
	value = strtonum("-42", -100, 100, &errstr);
	check(value == -42 && errstr == NULL, scenario,
	    "accepts a negative value");
}

static void
strlcat_appends_when_the_result_fits(void)
{
	const char *scenario = "strlcat appends when the result fits";
	char destination[16];
	size_t needed;

	fill_with_sentinel(destination, sizeof(destination));
	strlcpy(destination, "magic", sizeof(destination));
	needed = strlcat(destination, "$path", sizeof(destination));

	check(needed == 10, scenario, "returns the combined length");
	check(strcmp(destination, "magic$path") == 0, scenario,
	    "appends the source to the destination");
	check(tail_is_untouched(destination, sizeof(destination), 11), scenario,
	    "writes nothing beyond the terminator");
}

static void
strlcat_truncates_and_reports_the_needed_length(void)
{
	const char *scenario = "strlcat truncates a result that does not fit";
	char destination[12];
	size_t stated_size;
	size_t needed;

	/* The buffer runs past the stated size so that a write beyond what
	 * the call was permitted is visible rather than merely out of range.
	 */
	stated_size = 8;
	fill_with_sentinel(destination, sizeof(destination));
	strlcpy(destination, "magic", stated_size);
	needed = strlcat(destination, "$path", stated_size);

	check(needed == 10, scenario,
	    "returns the length the result would have needed");
	check(strcmp(destination, "magic$p") == 0, scenario,
	    "fills the destination and terminates it");
	check(tail_is_untouched(destination, sizeof(destination), stated_size),
	    scenario, "writes nothing beyond the stated size");
}

/*
 * A destination with no terminator inside the size the call is given offers
 * no position to append at.  The interface defines the result as the
 * destination left untouched and a length of that size plus the length of
 * the source, which exceeds the size and so reports truncation to a caller
 * that checks it.  The guard that produces this result is what keeps the
 * remaining-space arithmetic from wrapping and writing past the buffer, so
 * it is worth exercising rather than merely reading.
 */
static void
strlcat_leaves_an_unterminated_destination_alone(void)
{
	const char *scenario =
	    "strlcat leaves an unterminated destination alone";
	char destination[16];
	size_t stated_size;
	size_t needed;

	/*
	 * The buffer is larger than the size passed to the call and does end
	 * in a terminator, so the argument remains a string even though the
	 * first stated_size bytes hold none.  A sanitizer that checks string
	 * arguments over their whole length would otherwise report the
	 * fixture itself as the defect.
	 */
	stated_size = 8;
	fill_with_sentinel(destination, sizeof(destination));
	destination[stated_size] = '\0';

	needed = strlcat(destination, "rule", stated_size);

	check(needed == stated_size + 4, scenario,
	    "returns the stated size plus the source length");
	check(tail_is_untouched(destination, stated_size, 0), scenario,
	    "writes nothing at all");
	check(destination[stated_size] == '\0', scenario,
	    "writes nothing beyond the stated size");
}

static void
reallocarray_preserves_contents_when_it_grows(void)
{
	const char *scenario = "reallocarray preserves contents when it grows";
	uint32_t *identifiers;
	uint32_t *grown;
	size_t i;
	bool preserved;

	identifiers = reallocarray(NULL, 8, sizeof(*identifiers));
	check(identifiers != NULL, scenario, "allocates the initial array");
	if (identifiers == NULL)
		return;

	for (i = 0; i < 8; i++)
		identifiers[i] = (uint32_t)i;

	/*
	 * The result is taken in a second pointer, because a failed resize
	 * leaves the original allocation valid and assigning over it would
	 * lose the only reference to it.
	 */
	grown = reallocarray(identifiers, 64, sizeof(*identifiers));
	check(grown != NULL, scenario, "grows the array");
	if (grown == NULL) {
		free(identifiers);
		return;
	}
	identifiers = grown;

	preserved = true;
	for (i = 0; i < 8; i++) {
		if (identifiers[i] != (uint32_t)i)
			preserved = false;
	}
	check(preserved, scenario, "keeps the values the array already held");

	free(identifiers);
}

static void
reallocarray_refuses_an_overflowing_product(void)
{
	const char *scenario =
	    "reallocarray refuses a product that would overflow";
	volatile size_t count = SIZE_MAX;
	void *result;

	/*
	 * The count is read through a volatile object, because a compiler
	 * that sees the overflowing product at compile time warns about the
	 * call this test makes on purpose.
	 */
	errno = 0;
	result = reallocarray(NULL, count, 2);

	check(result == NULL, scenario, "returns NULL instead of a short array");
	check(errno == ENOMEM, scenario, "reports ENOMEM");
}

/*
 * A count of zero is the one input for which no product can overflow, so the
 * check is skipped.  The call must still behave like a resize rather than
 * failing, since a caller emptying an array reaches this path.
 */
static void
reallocarray_accepts_a_count_of_zero(void)
{
	const char *scenario = "reallocarray accepts a count of zero";
	void *result;

	errno = 0;
	result = reallocarray(NULL, 0, sizeof(uint32_t));

	check(errno != ENOMEM, scenario, "does not report an overflow");
	free(result);
}

static void
strtonum_accepts_a_value_inside_the_interval(void)
{
	const char *scenario = "strtonum accepts a value inside the interval";
	const char *errstr;
	long long value;

	errstr = "unset";
	errno = EDOM;
	value = strtonum("5000000", 1, 100000000, &errstr);

	check(value == 5000000, scenario, "converts the digits");
	check(errstr == NULL, scenario, "reports no error");
	check(errno == EDOM, scenario,
	    "leaves errno as the caller had it on success");

	/* An absent error pointer is permitted and must not be written. */
	value = strtonum("42", 0, 100, NULL);
	check(value == 42, scenario, "converts without an error pointer");
}

static void
strtonum_rejects_values_outside_the_interval(void)
{
	const char *scenario =
	    "strtonum rejects values outside the interval";
	const char *errstr;
	long long value;

	errstr = NULL;
	value = strtonum("0", 1, 10, &errstr);
	check(value == 0, scenario, "returns zero when the value is too small");
	check(errstr != NULL && strcmp(errstr, "too small") == 0, scenario,
	    "names a value below the minimum as too small");
	check(errno == ERANGE, scenario, "reports ERANGE below the minimum");

	errstr = NULL;
	value = strtonum("11", 1, 10, &errstr);
	check(value == 0, scenario, "returns zero when the value is too large");
	check(errstr != NULL && strcmp(errstr, "too large") == 0, scenario,
	    "names a value above the maximum as too large");
	check(errno == ERANGE, scenario, "reports ERANGE above the maximum");

	/* A source that no long long can represent is a range failure too. */
	errstr = NULL;
	value = strtonum("99999999999999999999", 0, LLONG_MAX, &errstr);
	check(value == 0, scenario, "returns zero for an unrepresentable value");
	check(errstr != NULL && strcmp(errstr, "too large") == 0, scenario,
	    "names an unrepresentable value as too large");
}

static void
strtonum_rejects_malformed_input(void)
{
	const char *scenario = "strtonum rejects malformed input";
	const char *errstr;
	long long value;

	errstr = NULL;
	value = strtonum("", 0, 10, &errstr);
	check(value == 0 && errstr != NULL && strcmp(errstr, "invalid") == 0,
	    scenario, "rejects an empty string");
	check(errno == EINVAL, scenario, "reports EINVAL for an empty string");

	errstr = NULL;
	value = strtonum("5x", 0, 10, &errstr);
	check(value == 0 && errstr != NULL && strcmp(errstr, "invalid") == 0,
	    scenario, "rejects trailing characters rather than converting a prefix");

	errstr = NULL;
	value = strtonum("0x10", 0, 100, &errstr);
	check(value == 0 && errstr != NULL && strcmp(errstr, "invalid") == 0,
	    scenario, "rejects a base prefix it does not accept");

	errstr = NULL;
	value = strtonum("7", 10, 1, &errstr);
	check(value == 0 && errstr != NULL && strcmp(errstr, "invalid") == 0,
	    scenario, "rejects an interval whose minimum exceeds its maximum");

	/*
	 * A caller that does not want the message passes no pointer, and the
	 * failure path is where omitting it would otherwise be written to.
	 */
	value = strtonum("not a number", 0, 10, NULL);
	check(value == 0, scenario,
	    "fails without an error pointer and returns zero");
}

/*
 * The parser converts integer literals into int64_t and must be able to name
 * the whole range, LLONG_MIN included.  A conversion that loses the extreme
 * would turn a legal literal into a parse error.
 */
static void
strtonum_converts_the_extremes_of_long_long(void)
{
	const char *scenario = "strtonum converts the extremes of long long";
	const char *errstr;
	long long value;

	errstr = "unset";
	value = strtonum("-9223372036854775808", LLONG_MIN, LLONG_MAX, &errstr);
	check(value == LLONG_MIN, scenario, "converts the smallest long long");
	check(errstr == NULL, scenario, "accepts the smallest long long");

	errstr = "unset";
	value = strtonum("9223372036854775807", LLONG_MIN, LLONG_MAX, &errstr);
	check(value == LLONG_MAX, scenario, "converts the largest long long");
	check(errstr == NULL, scenario, "accepts the largest long long");

	/*
	 * A string naming a value below the type's range saturates at
	 * LLONG_MIN, which is not below a minimum of LLONG_MIN.  The
	 * saturation has to be recognized for its own sake, or the caller is
	 * told the value was too large.
	 */
	errstr = NULL;
	value = strtonum("-99999999999999999999", LLONG_MIN, LLONG_MAX,
	    &errstr);
	check(value == 0, scenario, "returns zero for an unrepresentable value");
	check(errstr != NULL && strcmp(errstr, "too small") == 0, scenario,
	    "names a value below the type's range as too small");
}

int
main(void)
{
	an_empty_source_changes_nothing();
	getentropy_fills_exactly_the_bytes_asked_for();
	getentropy_refuses_more_than_it_may_hand_out();
	memmem_finds_the_first_occurrence();
	memmem_reads_only_the_ranges_it_is_given();
	reallocarray_accepts_a_count_of_zero();
	reallocarray_preserves_contents_when_it_grows();
	reallocarray_refuses_an_overflowing_product();
	strlcat_appends_when_the_result_fits();
	strlcat_fills_the_destination_exactly();
	strlcat_leaves_an_unterminated_destination_alone();
	strlcat_truncates_and_reports_the_needed_length();
	strlcat_with_no_room_writes_nothing();
	strlcpy_copies_a_string_that_fits();
	strlcpy_fills_the_destination_exactly();
	strlcpy_truncates_and_reports_the_needed_length();
	strlcpy_with_no_room_writes_nothing();
	strtonum_accepts_a_value_inside_the_interval();
	strtonum_accepts_leading_space_and_sign();
	strtonum_accepts_the_bounds_of_the_interval();
	strtonum_converts_the_extremes_of_long_long();
	strtonum_rejects_malformed_input();
	strtonum_rejects_values_outside_the_interval();

	return (check_summary("compat"));
}
