/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the lexer.
 *
 * Three properties matter here and each is checked against something other
 * than the implementation.  What a byte can begin is checked against an
 * expectation written from the language definition rather than from the
 * classification table, so a wrong entry in that table fails rather than
 * being confirmed.  What a token spans is checked at the end of the buffer,
 * because a scanner that reads one byte too far is correct everywhere else.
 * And what a string literal holds is checked byte for byte, including the
 * NUL bytes a C string comparison would stop at.
 */

#include "config.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_CLASSIFY	"a byte begins the token the language says"
#define	SCENARIO_COMMENT	"a comment runs to the end of its line"
#define	SCENARIO_INTEGER	"an integer literal is a magnitude, its sign a token"
#define	SCENARIO_INTEGER_RULE	"a byte range is an integer by the language's rule"
#define	SCENARIO_LOCATION	"a token points at its first byte"
#define	SCENARIO_OPERATOR	"an arithmetic operator is a token of its own"
#define	SCENARIO_REJECT		"a malformed token says what is wrong"
#define	SCENARIO_STRING		"a string literal holds arbitrary bytes"
#define	SCENARIO_TRAILING	"a token ends where the buffer does"

/* More than any single test scans. */
#define	TOKEN_MAX		64

typedef struct scan scan_t;

/*
 * One scan of one input, with the tokens it produced.  Tests build one of
 * these, assert against it, and release it; nothing is shared between them,
 * so a failing test cannot make the next one fail.  `copy` is the input
 * scan_text() made, or NULL.
 */
struct scan {
	relic_t		*ctx;
	char		*copy;
	relic_token_t	 tokens[TOKEN_MAX];
	size_t		 count;
};

static void	 a_rejected_literal_is_consumed_whole(void);
static void	 comments_run_to_the_end_of_a_line(void);
static enum relic_token_kind expected_kind(unsigned char byte);
static void	 integer_literals_cover_the_whole_range(void);
static void	 integer_rule_holds_at_its_boundaries(void);
static void	 integer_rule_is_a_magnitude_and_a_sign(void);
static void	 integer_rule_reads_only_its_range(void);
static void	 malformed_tokens_report_their_reason(void);
static void	 operators_are_tokens_of_their_own(void);
static void	 scan_close(scan_t *scan);
static bool	 scan_open(scan_t *scan, const char *text, size_t length);
static void	 scan_text(scan_t *scan, const char *text,
		    const char *scenario);
static bool	 string_holds(const scan_t *scan, size_t index,
		    const char *bytes, size_t length);
static void	 string_literals_hold_arbitrary_bytes(void);
static void	 tokens_end_where_the_buffer_does(void);
static void	 tokens_point_at_their_first_byte(void);
static void	 every_byte_begins_what_the_language_says(void);
static const char *verdict_name(enum relic_integer_verdict verdict);

/*
 * Scan an input whole: `length` bytes at `text`, which need not be
 * terminated.
 */
static bool
scan_open(scan_t *scan, const char *text, size_t length)
{
	relic_lexer_t lexer;
	bool ok;

	bzero(scan, sizeof(*scan));

	scan->ctx = relic_create();
	if (scan->ctx == NULL)
		return (false);

	relic_lexer_init(&lexer, scan->ctx, "test.dl", text, length);

	ok = true;
	for (;;) {
		relic_token_t token;

		if (relic_lexer_next(&lexer, &token) != 0) {
			ok = false;	/* interning ran out of memory */
			break;
		}

		if (scan->count == TOKEN_MAX) {
			ok = false;
			break;
		}
		scan->tokens[scan->count] = token;
		scan->count++;

		if (token.kind == RELIC_TOKEN_END)
			break;
	}

	relic_lexer_reset(&lexer);

	return (ok);
}

/*
 * Scan a C string, reporting a failure against the caller's scenario rather
 * than against a scenario of this helper's own, so that a failing run names
 * the test that was running.
 *
 * The lexer is given a copy of the string without its terminator, in an
 * allocation of exactly its length, so that under AddressSanitizer a scanner
 * that reads past its input fails whichever test gave it the input, rather
 * than finding a NUL there.
 */
static void
scan_text(scan_t *scan, const char *text, const char *scenario)
{
	char *copy;
	size_t length;

	length = strlen(text);
	copy = malloc(length > 0 ? length : 1);
	if (copy == NULL) {
		bzero(scan, sizeof(*scan));
		check(false, scenario, "the input should be copied");
		return;
	}
	bcopy(text, copy, length);

	if (!scan_open(scan, copy, length))
		check(false, scenario, "the input should fit the scan");
	scan->copy = copy;
}

static void
scan_close(scan_t *scan)
{
	relic_destroy(scan->ctx);
	free(scan->copy);
	bzero(scan, sizeof(*scan));
}

/*
 * Whether the interned bytes of the token at `index` are exactly these.
 */
static bool
string_holds(const scan_t *scan, size_t index, const char *bytes,
    size_t length)
{
	const char *interned;
	size_t interned_length;

	if (index >= scan->count ||
	    scan->tokens[index].kind != RELIC_TOKEN_STRING)
		return (false);

	interned = relic_symbol_bytes(scan->ctx, scan->tokens[index].symbol,
	    &interned_length);
	if (interned == NULL || interned_length != length)
		return (false);

	if (length == 0)
		return (true);

	return (bcmp(interned, bytes, length) == 0);
}

/*
 * What a one-byte input should produce, derived from the language definition
 * rather than from the lexer's classification table.  This is the oracle the
 * sweep below compares against: the two must be written from different
 * descriptions or the comparison proves nothing.
 */
static enum relic_token_kind
expected_kind(unsigned char byte)
{
	if (strchr(" \t\n\v\f\r", byte) != NULL && byte != '\0')
		return (RELIC_TOKEN_END);	/* whitespace, nothing after */
	if (byte == '%')
		return (RELIC_TOKEN_END);	/* a comment to end of input */

	if (byte >= 'a' && byte <= 'z')
		return (RELIC_TOKEN_SYMBOL);
	if ((byte >= 'A' && byte <= 'Z') || byte == '_')
		return (RELIC_TOKEN_VARIABLE);
	if (byte >= '0' && byte <= '9')
		return (RELIC_TOKEN_INTEGER);

	switch (byte) {
	case '(':
		return (RELIC_TOKEN_LPAREN);
	case ')':
		return (RELIC_TOKEN_RPAREN);
	case '{':
		return (RELIC_TOKEN_LBRACE);
	case '}':
		return (RELIC_TOKEN_RBRACE);
	case ',':
		return (RELIC_TOKEN_COMMA);
	case '.':
		return (RELIC_TOKEN_DOT);
	case '?':
		return (RELIC_TOKEN_QUESTION);
	case '=':
	case '<':
	case '>':
		return (RELIC_TOKEN_COMPARE);
	case '+':
	case '-':
	case '*':
	case '/':
		return (RELIC_TOKEN_OPERATOR);
	default:
		break;
	}

	/*
	 * Everything else begins no token: a lone `"` is an unterminated
	 * literal, a lone `:` or `!` is half of an operator, and every other
	 * byte, above 0x7f included, is simply not part of the language.
	 * The remainder operator is the word `mod`, a symbol to the lexer.
	 */
	return (RELIC_TOKEN_ERROR);
}

static void
every_byte_begins_what_the_language_says(void)
{
	unsigned int candidate;

	for (candidate = 0; candidate <= UCHAR_MAX; candidate++) {
		char input[1];
		scan_t scan;
		enum relic_token_kind expected;
		unsigned char byte;

		byte = (unsigned char)candidate;
		expected = expected_kind(byte);
		input[0] = (char)byte;

		if (!scan_open(&scan, input, 1)) {
			checkf(false, SCENARIO_CLASSIFY,
			    "byte 0x%02x should scan", candidate);
			continue;
		}

		checkf(scan.count >= 1 && scan.tokens[0].kind == expected,
		    SCENARIO_CLASSIFY, "byte 0x%02x should begin token kind %d",
		    candidate, expected);

		scan_close(&scan);
	}
}

/*
 * Every token class scanned with nothing after it, which is where a scanner
 * that reads one byte past the token it recognized goes wrong.  A trailing
 * newline would hide all of them.
 */
static void
tokens_end_where_the_buffer_does(void)
{
	static const struct {
		const char		*text;
		enum relic_token_kind	 kind;
		size_t			 count;	/* the end token included */
	} cases[] = {
		{ "edge", RELIC_TOKEN_SYMBOL, 2 },
		{ "Node", RELIC_TOKEN_VARIABLE, 2 },
		{ "_", RELIC_TOKEN_VARIABLE, 2 },
		{ "\"abc\"", RELIC_TOKEN_STRING, 2 },
		{ "120", RELIC_TOKEN_INTEGER, 2 },
		{ "-", RELIC_TOKEN_OPERATOR, 2 },
		{ "+", RELIC_TOKEN_OPERATOR, 2 },
		{ ":-", RELIC_TOKEN_IMPLIES, 2 },
		{ "<=", RELIC_TOKEN_COMPARE, 2 },
		{ ">=", RELIC_TOKEN_COMPARE, 2 },
		{ "!=", RELIC_TOKEN_COMPARE, 2 },
		{ "(", RELIC_TOKEN_LPAREN, 2 },
		{ ")", RELIC_TOKEN_RPAREN, 2 },
		{ "{", RELIC_TOKEN_LBRACE, 2 },
		{ "}", RELIC_TOKEN_RBRACE, 2 },
		{ ",", RELIC_TOKEN_COMMA, 2 },
		{ ".", RELIC_TOKEN_DOT, 2 },
		{ "?", RELIC_TOKEN_QUESTION, 2 },
		{ "% a comment", RELIC_TOKEN_END, 1 }
	};

	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scan_t scan;

		scan_text(&scan, cases[i].text, SCENARIO_TRAILING);

		/*
		 * The expected count is the token and the end that follows
		 * it, so a scanner that read one byte past the token and
		 * produced something more would fail here rather than only
		 * on the input that happened to follow.
		 */
		checkf(scan.count == cases[i].count &&
		    scan.tokens[0].kind == cases[i].kind, SCENARIO_TRAILING,
		    "%s should scan as one token and the end of the input",
		    cases[i].text);

		checkf(scan.count >= 1 &&
		    scan.tokens[scan.count - 1].kind == RELIC_TOKEN_END,
		    SCENARIO_TRAILING, "%s should end at the end of the input",
		    cases[i].text);

		scan_close(&scan);
	}
}

static void
tokens_point_at_their_first_byte(void)
{
	/*
	 * Two lines, with a tab indenting the second.  The tokens are named
	 * rather than counted to, so that an assertion below says which byte
	 * of the input it is about.
	 */
	enum {
		TOKEN_EDGE,
		TOKEN_EDGE_LPAREN,
		TOKEN_EDGE_A,
		TOKEN_EDGE_RPAREN,
		TOKEN_EDGE_DOT,
		TOKEN_PATH,		/* the first token of the second line */
		TOKEN_PATH_LPAREN,
		TOKEN_PATH_X,
		TOKEN_PATH_RPAREN,
		TOKEN_PATH_QUESTION,
		TOKEN_END,
		TOKEN_COUNT
	};

	scan_t scan;

	scan_text(&scan, "edge(a).\n\tpath(X)?\n", SCENARIO_LOCATION);

	check(scan.count == TOKEN_COUNT, SCENARIO_LOCATION,
	    "the two statements should produce ten tokens and an end");
	if (scan.count != TOKEN_COUNT) {
		scan_close(&scan);
		return;
	}

	check(scan.tokens[TOKEN_EDGE].where.line == 1 &&
	    scan.tokens[TOKEN_EDGE].where.column == 1, SCENARIO_LOCATION,
	    "the first symbol should be at line 1 column 1");
	check(scan.tokens[TOKEN_EDGE_LPAREN].where.column == 5,
	    SCENARIO_LOCATION,
	    "the opening parenthesis should be at column 5");

	/*
	 * A tab is one column: the number exists to let a reader find the
	 * byte, not to reproduce the indentation.
	 */
	check(scan.tokens[TOKEN_PATH].where.line == 2 &&
	    scan.tokens[TOKEN_PATH].where.column == 2, SCENARIO_LOCATION,
	    "a tab should advance the column by one, not to a tab stop");

	check(scan.tokens[TOKEN_EDGE].where.file != NULL &&
	    strcmp(scan.tokens[TOKEN_EDGE].where.file, "test.dl") == 0,
	    SCENARIO_LOCATION, "a token should name the source it came from");

	scan_close(&scan);
}

static void
comments_run_to_the_end_of_a_line(void)
{
	scan_t scan;

	scan_text(&scan, "% edge(a).\nedge(b).\n", SCENARIO_COMMENT);

	check(scan.count >= 1 && scan.tokens[0].kind == RELIC_TOKEN_SYMBOL,
	    SCENARIO_COMMENT, "a commented statement should produce no tokens");
	check(scan.count >= 1 && scan.tokens[0].where.line == 2,
	    SCENARIO_COMMENT, "scanning should resume on the line after it");

	scan_close(&scan);

	/*
	 * There are no block comments, so a comment that reaches the end of
	 * the input without a newline is ordinary rather than unterminated.
	 */
	scan_text(&scan, "edge(b).\n% trailing", SCENARIO_COMMENT);

	check(scan.count >= 1 &&
	    scan.tokens[scan.count - 1].kind == RELIC_TOKEN_END,
	    SCENARIO_COMMENT,
	    "a comment at the end of input should end the input, not fail");

	scan_close(&scan);
}

/*
 * A literal is its digits' magnitude, and a `-` before it is an operator
 * token, since the lexer cannot tell a negative literal from a subtraction.
 * The magnitude of INT64_MIN scans, since the parser gives it its sign, and
 * a larger one scans as one past it, which no sign brings into range.
 */
static void
integer_literals_cover_the_whole_range(void)
{
	enum {
		TOKEN_ZERO,
		TOKEN_MINUS,
		TOKEN_ONE,
		TOKEN_INT64_MAX,
		TOKEN_INT64_MIN_MAGNITUDE,
		TOKEN_ONE_PAST,
		TOKEN_FAR_PAST,
		TOKEN_END,
		TOKEN_COUNT
	};

	scan_t scan;

	scan_text(&scan, "0 -1 9223372036854775807 9223372036854775808 "
	    "9223372036854775809 99999999999999999999999", SCENARIO_INTEGER);

	check(scan.count == TOKEN_COUNT, SCENARIO_INTEGER,
	    "six literals, a minus and an end should be scanned");
	if (scan.count != TOKEN_COUNT) {
		scan_close(&scan);
		return;
	}

	check(scan.tokens[TOKEN_ZERO].magnitude == 0, SCENARIO_INTEGER,
	    "zero should scan as zero");
	check(scan.tokens[TOKEN_MINUS].kind == RELIC_TOKEN_OPERATOR &&
	    scan.tokens[TOKEN_MINUS].arith == RELIC_ARITH_SUBTRACT,
	    SCENARIO_INTEGER, "a minus should be an operator token");
	check(scan.tokens[TOKEN_ONE].kind == RELIC_TOKEN_INTEGER &&
	    scan.tokens[TOKEN_ONE].magnitude == 1 &&
	    scan.tokens[TOKEN_ONE].where.column == 4, SCENARIO_INTEGER,
	    "the digits after a minus should be a literal of their own");
	check(scan.tokens[TOKEN_INT64_MAX].magnitude == (uint64_t)INT64_MAX,
	    SCENARIO_INTEGER, "the largest positive integer should scan");
	check(scan.tokens[TOKEN_INT64_MIN_MAGNITUDE].magnitude ==
	    RELIC_MAGNITUDE_MAX, SCENARIO_INTEGER,
	    "the magnitude of the smallest integer should scan");
	check(scan.tokens[TOKEN_ONE_PAST].kind == RELIC_TOKEN_INTEGER &&
	    scan.tokens[TOKEN_ONE_PAST].magnitude == RELIC_MAGNITUDE_MAX + 1,
	    SCENARIO_INTEGER,
	    "a magnitude one past the largest should scan as one past it");
	check(scan.tokens[TOKEN_FAR_PAST].kind == RELIC_TOKEN_INTEGER &&
	    scan.tokens[TOKEN_FAR_PAST].magnitude == RELIC_MAGNITUDE_MAX + 1,
	    SCENARIO_INTEGER,
	    "a magnitude far past the largest should scan as one past it");

	scan_close(&scan);
}

/*
 * The four operator bytes are tokens wherever they stand, and `%` is a
 * comment even directly after an operand, so an expression followed by a
 * comment is the expression.
 */
static void
operators_are_tokens_of_their_own(void)
{
	static const enum relic_arith_op expected[] = {
		RELIC_ARITH_ADD, RELIC_ARITH_SUBTRACT, RELIC_ARITH_MULTIPLY,
		RELIC_ARITH_DIVIDE
	};

	scan_t scan;
	size_t i;

	scan_text(&scan, "X+Y-1*Z/2%3 mod\n", SCENARIO_OPERATOR);

	check(scan.count == 10, SCENARIO_OPERATOR,
	    "five operands, four operators and an end should be scanned");
	if (scan.count != 10) {
		scan_close(&scan);
		return;
	}

	for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
		checkf(scan.tokens[2 * i + 1].kind == RELIC_TOKEN_OPERATOR &&
		    scan.tokens[2 * i + 1].arith == expected[i],
		    SCENARIO_OPERATOR, "operator %zu should be its own token",
		    i);

	check(scan.tokens[8].kind == RELIC_TOKEN_INTEGER &&
	    scan.tokens[8].magnitude == 2 &&
	    scan.tokens[9].kind == RELIC_TOKEN_END, SCENARIO_OPERATOR,
	    "a % after an operand should begin a comment");

	scan_close(&scan);
}

/* The name of a verdict of the integer rule, for a failure message. */
static const char *
verdict_name(enum relic_integer_verdict verdict)
{
	switch (verdict) {
	case RELIC_INTEGER_VALID:
		return ("valid");
	case RELIC_INTEGER_MALFORMED:
		return ("malformed");
	case RELIC_INTEGER_OUT_OF_RANGE:
		return ("out of range");
	}

	return ("unknown");
}

/*
 * The rule the lexer and every other reader of integers share, driven
 * directly over byte ranges.  The expectations are written from the rule as
 * the language states it, `-?[0-9]+` within int64_t, at each of its edges.
 * A range may hold any byte, as a table's field may, so a NUL and a byte
 * above 0x7f are among them; a case's length is its literal's, NUL
 * included.
 */
static void
integer_rule_holds_at_its_boundaries(void)
{
#define	RULE_CASE(text, verdict, value) \
	{ text, sizeof(text) - 1, verdict, value }
	static const struct {
		const char			*text;
		size_t				 length;
		enum relic_integer_verdict	 verdict;
		int64_t				 value;	/* when valid */
	} cases[] = {
		RULE_CASE("0", RELIC_INTEGER_VALID, 0),
		RULE_CASE("-0", RELIC_INTEGER_VALID, 0),
		RULE_CASE("007", RELIC_INTEGER_VALID, 7),
		RULE_CASE("-12", RELIC_INTEGER_VALID, -12),
		RULE_CASE("9223372036854775807", RELIC_INTEGER_VALID,
		    INT64_MAX),
		RULE_CASE("-9223372036854775808", RELIC_INTEGER_VALID,
		    INT64_MIN),
		RULE_CASE("0009223372036854775807", RELIC_INTEGER_VALID,
		    INT64_MAX),
		RULE_CASE("9223372036854775808", RELIC_INTEGER_OUT_OF_RANGE, 0),
		RULE_CASE("-9223372036854775809", RELIC_INTEGER_OUT_OF_RANGE,
		    0),
		RULE_CASE("18446744073709551616", RELIC_INTEGER_OUT_OF_RANGE,
		    0),
		RULE_CASE("99999999999999999999999", RELIC_INTEGER_OUT_OF_RANGE,
		    0),
		RULE_CASE("", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE("-", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE("+1", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE(" 7", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE("7 ", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE("7x", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE("1-", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE("--1", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE("0x10", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE("99999999999999999999x", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE("1\0", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE("\0" "1", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE("7\x80", RELIC_INTEGER_MALFORMED, 0),
		RULE_CASE("\xb7" "7", RELIC_INTEGER_MALFORMED, 0)
	};
#undef	RULE_CASE

	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		enum relic_integer_verdict verdict;
		int64_t value;

		value = 0;
		verdict = relic_lex_integer(cases[i].text, cases[i].length,
		    &value);

		checkf(verdict == cases[i].verdict, SCENARIO_INTEGER_RULE,
		    "case %zu, \"%s\", should be %s, not %s", i, cases[i].text,
		    verdict_name(cases[i].verdict), verdict_name(verdict));
		if (verdict == RELIC_INTEGER_VALID &&
		    cases[i].verdict == verdict)
			checkf(value == cases[i].value, SCENARIO_INTEGER_RULE,
			    "case %zu, \"%s\", should convert to %jd, not %jd",
			    i, cases[i].text, (intmax_t)cases[i].value,
			    (intmax_t)value);
		if (verdict != RELIC_INTEGER_VALID)
			checkf(value == 0, SCENARIO_INTEGER_RULE,
			    "case %zu, \"%s\", should leave the value alone",
			    i, cases[i].text);
	}
}

/*
 * The range is all the rule reads.  A range cut from longer text converts
 * only what it holds, and a range with no terminator after it is read to
 * its end and no further; under AddressSanitizer a read past the array
 * below fails the test.
 */
static void
integer_rule_reads_only_its_range(void)
{
	static const char unterminated[] = { '4', '2' };

	scan_t scan;
	int64_t value;

	value = 0;
	check(relic_lex_integer("1234", 2, &value) == RELIC_INTEGER_VALID &&
	    value == 12, SCENARIO_INTEGER_RULE,
	    "the first two bytes of 1234 should be the integer 12");

	value = 0;
	check(relic_lex_integer("-5x", 2, &value) == RELIC_INTEGER_VALID &&
	    value == -5, SCENARIO_INTEGER_RULE,
	    "a byte past the range should not make it malformed");

	value = 0;
	check(relic_lex_integer(unterminated, sizeof(unterminated), &value) ==
	    RELIC_INTEGER_VALID && value == 42, SCENARIO_INTEGER_RULE,
	    "an unterminated range should convert");

	/* The lexer finds the literal's extent at the end of its input too. */
	if (!scan_open(&scan, unterminated, sizeof(unterminated))) {
		check(false, SCENARIO_INTEGER_RULE,
		    "an unterminated literal should scan");
		return;
	}
	check(scan.count == 2 && scan.tokens[0].kind == RELIC_TOKEN_INTEGER &&
	    scan.tokens[0].magnitude == 42, SCENARIO_INTEGER_RULE,
	    "a literal ending the input with no terminator should be 42");
	scan_close(&scan);
}

/*
 * The two pieces the whole rule is made of, which the lexer and the parser
 * each use one of: a magnitude of digits alone, within the magnitude of
 * INT64_MIN, and a sign that must bring it into range.
 */
static void
integer_rule_is_a_magnitude_and_a_sign(void)
{
	uint64_t magnitude;
	int64_t value;

	magnitude = 0;
	check(relic_lex_magnitude("9223372036854775808", 19, &magnitude) ==
	    RELIC_INTEGER_VALID && magnitude == RELIC_MAGNITUDE_MAX,
	    SCENARIO_INTEGER_RULE,
	    "the magnitude of INT64_MIN should convert as a magnitude");
	check(relic_lex_magnitude("9223372036854775809", 19, &magnitude) ==
	    RELIC_INTEGER_OUT_OF_RANGE, SCENARIO_INTEGER_RULE,
	    "a magnitude past INT64_MIN's should be out of range");
	check(relic_lex_magnitude("-1", 2, &magnitude) ==
	    RELIC_INTEGER_MALFORMED, SCENARIO_INTEGER_RULE,
	    "a magnitude should hold no sign");

	value = 0;
	check(relic_integer_signed(RELIC_MAGNITUDE_MAX, true, &value) ==
	    RELIC_INTEGER_VALID && value == INT64_MIN, SCENARIO_INTEGER_RULE,
	    "the magnitude of INT64_MIN should fit when negative");
	check(relic_integer_signed(RELIC_MAGNITUDE_MAX, false, &value) ==
	    RELIC_INTEGER_OUT_OF_RANGE, SCENARIO_INTEGER_RULE,
	    "the magnitude of INT64_MIN should not fit when positive");
	check(relic_integer_signed(RELIC_MAGNITUDE_MAX + 1, true, &value) ==
	    RELIC_INTEGER_OUT_OF_RANGE, SCENARIO_INTEGER_RULE,
	    "a magnitude past INT64_MIN's should fit no sign");
	check(relic_integer_signed((uint64_t)INT64_MAX, false, &value) ==
	    RELIC_INTEGER_VALID && value == INT64_MAX, SCENARIO_INTEGER_RULE,
	    "INT64_MAX should fit when positive");
}

static void
string_literals_hold_arbitrary_bytes(void)
{
	static const char with_nul[] = { 'a', '\0', 'b' };

	scan_t scan;

	scan_text(&scan, "\"\" \"a\\0b\" \"\\\\\\\"\\n\\t\\r\" \"\\x7f\\xAf\"",
	    SCENARIO_STRING);

	check(scan.count == 5, SCENARIO_STRING,
	    "four literals and an end should be scanned");
	if (scan.count != 5) {
		scan_close(&scan);
		return;
	}

	check(string_holds(&scan, 0, "", 0), SCENARIO_STRING,
	    "an empty literal should hold no bytes");
	check(string_holds(&scan, 1, with_nul, sizeof(with_nul)),
	    SCENARIO_STRING, "an escaped NUL should be a byte of the string");
	check(string_holds(&scan, 2, "\\\"\n\t\r", 5), SCENARIO_STRING,
	    "each named escape should decode to its byte");
	check(string_holds(&scan, 3, "\x7f\xaf", 2), SCENARIO_STRING,
	    "a hexadecimal escape should decode in either case");

	scan_close(&scan);

	/* A byte above 0x7f passes through unexamined, so UTF-8 survives. */
	scan_text(&scan, "\"\xc3\xa9\"", SCENARIO_STRING);

	check(string_holds(&scan, 0, "\xc3\xa9", 2), SCENARIO_STRING,
	    "bytes above 0x7f should pass through a literal unchanged");

	scan_close(&scan);
}

static void
malformed_tokens_report_their_reason(void)
{
	static const struct {
		const char		*text;
		enum relic_lex_error	 error;
	} cases[] = {
		{ "\"abc", RELIC_LEX_UNTERMINATED_STRING },
		{ "\"abc\n\"", RELIC_LEX_UNTERMINATED_STRING },
		{ "\"\\q\"", RELIC_LEX_BAD_ESCAPE },
		{ "\"\\xg0\"", RELIC_LEX_BAD_HEX_ESCAPE },
		{ "\"\\x7\"", RELIC_LEX_BAD_HEX_ESCAPE },
		{ "0x10", RELIC_LEX_INTEGER_TRAILING },
		{ ": ", RELIC_LEX_EXPECTED_IMPLIES },
		{ "! ", RELIC_LEX_EXPECTED_NE },
		{ "$", RELIC_LEX_BAD_BYTE }
	};

	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		scan_t scan;

		scan_text(&scan, cases[i].text, SCENARIO_REJECT);

		checkf(scan.count >= 1 &&
		    scan.tokens[0].kind == RELIC_TOKEN_ERROR &&
		    scan.tokens[0].error == cases[i].error, SCENARIO_REJECT,
		    "%s should be rejected with reason %d", cases[i].text,
		    (int)cases[i].error);

		scan_close(&scan);
	}
}

/*
 * A literal whose contents were rejected is still consumed whole, so that
 * what follows it is read as the next statement rather than as the wreckage
 * of this one.  Stopping at the offending escape would leave the closing
 * quote to open a literal of its own.
 */
static void
a_rejected_literal_is_consumed_whole(void)
{
	scan_t scan;

	scan_text(&scan, "\"\\q\".", SCENARIO_REJECT);

	check(scan.count == 3 && scan.tokens[1].kind == RELIC_TOKEN_DOT,
	    SCENARIO_REJECT,
	    "the token after a rejected literal should be what follows it");

	scan_close(&scan);
}

int
main(void)
{
	every_byte_begins_what_the_language_says();
	tokens_end_where_the_buffer_does();
	tokens_point_at_their_first_byte();
	comments_run_to_the_end_of_a_line();
	integer_literals_cover_the_whole_range();
	integer_rule_holds_at_its_boundaries();
	integer_rule_reads_only_its_range();
	integer_rule_is_a_magnitude_and_a_sign();
	operators_are_tokens_of_their_own();
	string_literals_hold_arbitrary_bytes();
	malformed_tokens_report_their_reason();
	a_rejected_literal_is_consumed_whole();

	return (check_summary("lex"));
}
