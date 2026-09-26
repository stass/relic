/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * The lexer.
 *
 * Source text is a sequence of bytes and this module says what those bytes
 * are: a name, a string literal, a number, a punctuation mark, or nothing at
 * all.  It interns the bytes of every name and every string literal, and it
 * reports nothing.  A byte sequence that is not a token comes back as
 * RELIC_TOKEN_ERROR carrying the reason, because the parser is the one that
 * knows whether it is still listening: after the first mistake in a
 * statement it is discarding wreckage and wants silence, and a lexer that
 * printed for itself would have to be told so.
 *
 * Classification is byte-oriented and locale-independent.  isalpha(3)
 * answers a question about the process locale, and the answer would decide
 * whether a program parses; relic's answer must be the same in every locale
 * and on every platform, so the table below is the only classifier used.
 */

#define	LEX_SPACE	0x01u
#define	LEX_DIGIT	0x02u
#define	LEX_HEX		0x04u
#define	LEX_SYM_START	0x08u	/* a lowercase letter: begins a symbol */
#define	LEX_VAR_START	0x10u	/* uppercase or _: begins a variable */
#define	LEX_IDENT	0x20u	/* continues either */

/*
 * Short spellings, so that the table below reads as a table rather than as
 * a column of expressions.  They exist for it alone and are undefined after
 * it.
 */
#define	SP	LEX_SPACE
#define	DG	(LEX_DIGIT | LEX_HEX | LEX_IDENT)
#define	LO	(LEX_SYM_START | LEX_IDENT)
#define	LX	(LEX_HEX | LEX_SYM_START | LEX_IDENT)
#define	UP	(LEX_VAR_START | LEX_IDENT)
#define	UX	(LEX_HEX | LEX_VAR_START | LEX_IDENT)

/*
 * Every byte not named here belongs to no class, which makes it whitespace
 * to nothing, part of no identifier and the start of no number.  That
 * includes every byte above 0x7f: identifiers are ASCII, so a byte outside
 * it appears only inside a string literal, where it is data and is not
 * classified at all.
 */
static const unsigned char byte_class[256] = {
	['\t'] = SP, ['\n'] = SP, ['\v'] = SP, ['\f'] = SP, ['\r'] = SP,
	[' '] = SP,

	['0'] = DG, ['1'] = DG, ['2'] = DG, ['3'] = DG, ['4'] = DG,
	['5'] = DG, ['6'] = DG, ['7'] = DG, ['8'] = DG, ['9'] = DG,

	['a'] = LX, ['b'] = LX, ['c'] = LX, ['d'] = LX, ['e'] = LX,
	['f'] = LX, ['g'] = LO, ['h'] = LO, ['i'] = LO, ['j'] = LO,
	['k'] = LO, ['l'] = LO, ['m'] = LO, ['n'] = LO, ['o'] = LO,
	['p'] = LO, ['q'] = LO, ['r'] = LO, ['s'] = LO, ['t'] = LO,
	['u'] = LO, ['v'] = LO, ['w'] = LO, ['x'] = LO, ['y'] = LO,
	['z'] = LO,

	['A'] = UX, ['B'] = UX, ['C'] = UX, ['D'] = UX, ['E'] = UX,
	['F'] = UX, ['G'] = UP, ['H'] = UP, ['I'] = UP, ['J'] = UP,
	['K'] = UP, ['L'] = UP, ['M'] = UP, ['N'] = UP, ['O'] = UP,
	['P'] = UP, ['Q'] = UP, ['R'] = UP, ['S'] = UP, ['T'] = UP,
	['U'] = UP, ['V'] = UP, ['W'] = UP, ['X'] = UP, ['Y'] = UP,
	['Z'] = UP,

	['_'] = UP
};

#undef	SP
#undef	DG
#undef	LO
#undef	LX
#undef	UP
#undef	UX

static void	 advance_byte(relic_lexer_t *lexer);
static bool	 decoded_append(relic_lexer_t *lexer, unsigned char byte);
static void	 fail_token(relic_token_t *token, enum relic_lex_error error);
static bool	 in_class(unsigned char byte, unsigned int classes);
static bool	 more_input(const relic_lexer_t *lexer);
static unsigned char peek(const relic_lexer_t *lexer, size_t ahead);
static int	 scan_identifier(relic_lexer_t *lexer, relic_token_t *token,
		    enum relic_token_kind kind);
static int	 scan_integer(relic_lexer_t *lexer, relic_token_t *token);
static int	 scan_punctuation(relic_lexer_t *lexer, relic_token_t *token);
static int	 scan_string(relic_lexer_t *lexer, relic_token_t *token);
static int	 scan_string_escape(relic_lexer_t *lexer, relic_token_t *token,
		    bool *decoded);
static int	 scan_token(relic_lexer_t *lexer, relic_token_t *token);
static void	 skip_ignorable(relic_lexer_t *lexer);
static void	 skip_rest_of_string(relic_lexer_t *lexer);
static unsigned int hex_digit_value(unsigned char byte);
static bool	 two_byte_operator(relic_lexer_t *lexer, relic_token_t *token,
		    unsigned char expected, enum relic_token_kind kind,
		    enum relic_lex_error error);

static bool
in_class(unsigned char byte, unsigned int classes)
{
	return ((byte_class[byte] & classes) != 0);
}

static bool
more_input(const relic_lexer_t *lexer)
{
	return (lexer->offset < lexer->length);
}

/*
 * The byte `ahead` positions past the scan position, or NUL once the input
 * runs out.  Returning a byte that belongs to no class is what lets every
 * scanning loop test the class alone and still stop at the end of the input.
 */
static unsigned char
peek(const relic_lexer_t *lexer, size_t ahead)
{
	if (lexer->length - lexer->offset <= ahead)
		return ('\0');

	return ((unsigned char)lexer->text[lexer->offset + ahead]);
}

/*
 * Consume one byte and move the recorded position with it.  Every byte the
 * lexer consumes passes through here, so line and column are maintained in
 * one place.  Columns count bytes: a tab is one column, because the number
 * exists to let a reader find the byte rather than to align a caret under it.
 */
static void
advance_byte(relic_lexer_t *lexer)
{
	unsigned char byte;

	if (!more_input(lexer))
		return;

	byte = (unsigned char)lexer->text[lexer->offset];
	lexer->offset++;

	if (byte == '\n') {
		lexer->where.line++;
		lexer->where.column = 1;
	} else {
		lexer->where.column++;
	}
}

void
relic_lexer_init(relic_lexer_t *lexer, relic_t *ctx, const char *source_name,
    const char *text, size_t length)
{
	bzero(lexer, sizeof(*lexer));

	lexer->ctx = ctx;
	lexer->text = text;
	lexer->length = length;
	lexer->where.file = source_name;
	lexer->where.line = 1;
	lexer->where.column = 1;
}

void
relic_lexer_reset(relic_lexer_t *lexer)
{
	free(lexer->decoded);
	bzero(lexer, sizeof(*lexer));
}

/*
 * Skip whatever separates tokens: whitespace, and a comment from its `%` to
 * the end of its line.
 *
 * The newline that ends a comment is left for the whitespace pass to
 * consume, so that it still advances the line counter.  A comment that runs
 * to the end of the input without one is not an error: a file whose last
 * line is a comment is ordinary, and there are no block comments for
 * anything to be unterminated.
 */
static void
skip_ignorable(relic_lexer_t *lexer)
{
	for (;;) {
		if (in_class(peek(lexer, 0), LEX_SPACE)) {
			advance_byte(lexer);
			continue;
		}

		if (peek(lexer, 0) != '%')
			return;

		while (more_input(lexer) && peek(lexer, 0) != '\n')
			advance_byte(lexer);
	}
}

bool
relic_lex_is_symbol(const char *bytes, size_t length)
{
	size_t i;

	if (length == 0 || !in_class((unsigned char)bytes[0], LEX_SYM_START))
		return (false);

	for (i = 1; i < length; i++) {
		if (!in_class((unsigned char)bytes[i], LEX_IDENT))
			return (false);
	}

	return (true);
}

bool
relic_lex_is_predicate_name(const char *bytes, size_t length)
{
	return (relic_lex_is_symbol(bytes, length) &&
	    !(length == 3 && bcmp(bytes, "not", 3) == 0));
}

int
relic_lexer_next(relic_lexer_t *lexer, relic_token_t *token)
{
	size_t start;
	int status;

	skip_ignorable(lexer);

	bzero(token, sizeof(*token));
	token->kind = RELIC_TOKEN_END;
	token->error = RELIC_LEX_OK;
	token->where = lexer->where;

	start = lexer->offset;
	status = scan_token(lexer, token);

	/*
	 * The span a diagnostic quotes is recorded here rather than by each
	 * scanner, so that it is the bytes actually consumed whichever path
	 * produced the token.
	 */
	token->text = lexer->text + start;
	token->text_length = lexer->offset - start;

	return (status);
}

/*
 * Recognize one token from the byte the scan position holds.  The dispatch
 * is on the byte's class rather than on its value wherever a class exists,
 * so that the classification table is the one description of what a byte can
 * begin.
 */
static int
scan_token(relic_lexer_t *lexer, relic_token_t *token)
{
	unsigned char byte;

	if (!more_input(lexer))
		return (0);		/* RELIC_TOKEN_END, already set */

	byte = peek(lexer, 0);

	if (in_class(byte, LEX_SYM_START))
		return (scan_identifier(lexer, token, RELIC_TOKEN_SYMBOL));

	if (in_class(byte, LEX_VAR_START))
		return (scan_identifier(lexer, token, RELIC_TOKEN_VARIABLE));

	/*
	 * A `-` immediately before a digit is part of the literal rather than
	 * an operator, which is unambiguous because the language has no
	 * arithmetic for it to be an operator in.  It is also what makes
	 * INT64_MIN writable: its magnitude has no positive counterpart.
	 */
	if (in_class(byte, LEX_DIGIT) ||
	    (byte == '-' && in_class(peek(lexer, 1), LEX_DIGIT)))
		return (scan_integer(lexer, token));

	if (byte == '"')
		return (scan_string(lexer, token));

	return (scan_punctuation(lexer, token));
}

/*
 * Mark the token as not being a token at all.  The scan position is left
 * where the error was found; the parser resumes from there.
 */
static void
fail_token(relic_token_t *token, enum relic_lex_error error)
{
	token->kind = RELIC_TOKEN_ERROR;
	token->error = error;
}

static int
scan_identifier(relic_lexer_t *lexer, relic_token_t *token,
    enum relic_token_kind kind)
{
	size_t start;

	start = lexer->offset;
	while (in_class(peek(lexer, 0), LEX_IDENT))
		advance_byte(lexer);

	if (relic_symbol_intern(lexer->ctx, lexer->text + start,
	    lexer->offset - start, &token->symbol) != 0)
		return (-1);

	token->kind = kind;

	return (0);
}

/*
 * Convert `length` bytes to an integer by the language's integer rule: the
 * whole range is `-?[0-9]+` and its value fits int64_t.  Nothing past the
 * range is read, so the range need not be terminated.
 *
 * The digits are accumulated as a magnitude in uint64_t, which holds the
 * magnitude of INT64_MIN as well as that of INT64_MAX, and each step checks
 * that one more digit keeps it within the sign's bound before taking it.
 * The check is `magnitude * 10 + digit > bound` rearranged so that nothing
 * can wrap: `bound - digit` cannot, as the bound is at least INT64_MAX, and
 * since the magnitude is a whole number, exceeding the rounded-down quotient
 * of `(bound - digit) / 10` is exactly exceeding the exact one.
 * Every byte is examined even after the value has left the range, so that a
 * range such as `99999999999999999999x` is refused as no integer at all
 * rather than as one out of range: what the bytes are is decided before
 * what they are worth.
 */
enum relic_integer_verdict
relic_lex_integer(const char *bytes, size_t length, int64_t *valuep)
{
	uint64_t bound;
	uint64_t magnitude;
	size_t first_digit;
	size_t i;
	bool negative;
	bool out_of_range;

	negative = length > 0 && bytes[0] == '-';
	first_digit = negative ? 1 : 0;
	if (first_digit == length)
		return (RELIC_INTEGER_MALFORMED);

	bound = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
	magnitude = 0;
	out_of_range = false;

	for (i = first_digit; i < length; i++) {
		unsigned char byte;
		unsigned int digit;

		byte = (unsigned char)bytes[i];
		if (!in_class(byte, LEX_DIGIT))
			return (RELIC_INTEGER_MALFORMED);
		digit = (unsigned int)(byte - '0');

		if (out_of_range || magnitude > (bound - digit) / 10u) {
			out_of_range = true;
			continue;
		}
		magnitude = magnitude * 10u + digit;
	}

	if (out_of_range)
		return (RELIC_INTEGER_OUT_OF_RANGE);

	/*
	 * The magnitude of INT64_MIN has no positive int64_t counterpart, so
	 * it is the one value not formed by negating a positive one.
	 */
	if (negative && magnitude == (uint64_t)INT64_MAX + 1u)
		*valuep = INT64_MIN;
	else if (negative)
		*valuep = -(int64_t)magnitude;
	else
		*valuep = (int64_t)magnitude;

	return (RELIC_INTEGER_VALID);
}

/*
 * Scan a decimal integer literal into int64_t.  The literal's extent is
 * found first -- an optional `-` and the digits after it -- and the range is
 * then converted by the rule relic_lex_integer() states, which an import
 * holds a table's integer fields to as well.  Within that extent the only
 * way to fail the rule is to leave the range.
 */
static int
scan_integer(relic_lexer_t *lexer, relic_token_t *token)
{
	enum relic_integer_verdict verdict;
	const char *literal;
	size_t start;
	int64_t value;

	start = lexer->offset;
	literal = lexer->text + start;

	if (peek(lexer, 0) == '-')
		advance_byte(lexer);
	while (in_class(peek(lexer, 0), LEX_DIGIT))
		advance_byte(lexer);

	verdict = relic_lex_integer(literal, lexer->offset - start, &value);
	assert(verdict != RELIC_INTEGER_MALFORMED);
	if (verdict != RELIC_INTEGER_VALID) {
		fail_token(token, RELIC_LEX_INTEGER_RANGE);
		return (0);
	}

	/*
	 * An identifier byte immediately after the digits ends the literal in
	 * the middle of something the writer meant as one word.  Reporting it
	 * here is what turns `0x10` into a complaint about the literal rather
	 * than into the integer 0 followed by the symbol x10 and a puzzling
	 * message about a missing comma further on.
	 */
	if (in_class(peek(lexer, 0), LEX_IDENT)) {
		token->where = lexer->where;
		token->byte = peek(lexer, 0);
		fail_token(token, RELIC_LEX_INTEGER_TRAILING);
		return (0);
	}

	token->kind = RELIC_TOKEN_INTEGER;
	token->integer = value;

	return (0);
}

/*
 * The value of a hexadecimal digit.  The caller must have classified the
 * byte as one; anything else falls through to the last case and produces a
 * number that means nothing.
 */
static unsigned int
hex_digit_value(unsigned char byte)
{
	if (byte >= '0' && byte <= '9')
		return ((unsigned int)(byte - '0'));
	if (byte >= 'a' && byte <= 'f')
		return ((unsigned int)(byte - 'a') + 10u);

	return ((unsigned int)(byte - 'A') + 10u);
}

/*
 * Append one decoded byte to the scratch buffer a string literal is built
 * in.  Returns false with the failure reported when it cannot grow.
 */
static bool
decoded_append(relic_lexer_t *lexer, unsigned char byte)
{
	char *bytes;

	bytes = relic_grow_array(lexer->decoded, &lexer->decoded_capacity,
	    lexer->decoded_length + 1, sizeof(*lexer->decoded));
	if (bytes == NULL) {
		relic_diag_no_memory(lexer->ctx);
		return (false);
	}
	lexer->decoded = bytes;

	lexer->decoded[lexer->decoded_length] = (char)byte;
	lexer->decoded_length++;

	return (true);
}

/*
 * Decode the escape whose backslash has just been consumed, appending its
 * byte to the scratch buffer.
 *
 * Returns -1 when the scratch buffer could not grow, and otherwise 0 with
 * *decoded saying whether an escape was recognized.  A rejected escape has
 * already filled in the token; an unknown one is an error rather than a
 * silent literal character, because the permissive reading cannot be taken
 * back: once "\q" means q, defining \q later changes the meaning of a
 * program that already parses.
 */
static int
scan_string_escape(relic_lexer_t *lexer, relic_token_t *token, bool *decoded)
{
	relic_location_t escape_where;
	unsigned char escape;
	unsigned char value;

	*decoded = false;

	if (!more_input(lexer)) {
		fail_token(token, RELIC_LEX_UNTERMINATED_STRING);
		return (0);
	}

	escape_where = lexer->where;
	escape = peek(lexer, 0);
	advance_byte(lexer);

	switch (escape) {
	case '\\':
		value = '\\';
		break;
	case '"':
		value = '"';
		break;
	case '0':
		value = '\0';
		break;
	case 'n':
		value = '\n';
		break;
	case 'r':
		value = '\r';
		break;
	case 't':
		value = '\t';
		break;
	case 'x':
		if (!in_class(peek(lexer, 0), LEX_HEX) ||
		    !in_class(peek(lexer, 1), LEX_HEX)) {
			token->where = escape_where;
			fail_token(token, RELIC_LEX_BAD_HEX_ESCAPE);
			return (0);
		}
		value = (unsigned char)(hex_digit_value(peek(lexer, 0)) * 16u +
		    hex_digit_value(peek(lexer, 1)));
		advance_byte(lexer);
		advance_byte(lexer);
		break;
	default:
		token->where = escape_where;
		token->byte = escape;
		fail_token(token, RELIC_LEX_BAD_ESCAPE);
		return (0);
	}

	if (!decoded_append(lexer, value))
		return (-1);

	*decoded = true;

	return (0);
}

/*
 * Scan a string literal, whose opening quote the scan position holds.
 *
 * A literal holds arbitrary bytes: those above 0x7f pass through
 * unexamined, so UTF-8 survives without the lexer knowing what UTF-8 is, and
 * a NUL written as an escape is part of the string, which is why the symbol
 * pool records a length beside its bytes.
 *
 * A raw newline ends the literal as unterminated rather than continuing it.
 * A missing closing quote is a common typo, and allowing the newline would
 * let one of them swallow the rest of the file and report its failure
 * hundreds of lines from the mistake.  A newline inside a string is written
 * \n.
 */
static int
scan_string(relic_lexer_t *lexer, relic_token_t *token)
{
	advance_byte(lexer);			/* the opening quote */
	lexer->decoded_length = 0;

	for (;;) {
		unsigned char byte;
		bool decoded;

		if (!more_input(lexer) || peek(lexer, 0) == '\n') {
			fail_token(token, RELIC_LEX_UNTERMINATED_STRING);
			return (0);
		}

		byte = peek(lexer, 0);
		advance_byte(lexer);

		if (byte == '"')
			break;

		if (byte != '\\') {
			if (!decoded_append(lexer, byte))
				return (-1);
			continue;
		}

		if (scan_string_escape(lexer, token, &decoded) != 0)
			return (-1);

		/*
		 * The token already says what was wrong with the escape.
		 * The rest of the literal is consumed anyway, so that the
		 * error spans the whole literal and the parser resumes
		 * after it: resuming inside one would read its remaining
		 * bytes as tokens and its closing quote as the start of
		 * another string, which is how one mistyped escape turns
		 * into a complaint about the next statement.
		 */
		if (!decoded) {
			skip_rest_of_string(lexer);
			return (0);
		}
	}

	if (relic_symbol_intern(lexer->ctx, lexer->decoded,
	    lexer->decoded_length, &token->symbol) != 0)
		return (-1);

	token->kind = RELIC_TOKEN_STRING;

	return (0);
}

/*
 * Consume the remainder of a string literal whose contents have already been
 * rejected.  Nothing is decoded; only the two-byte escapes are recognized,
 * because those are the ones that could hide the closing quote.
 */
static void
skip_rest_of_string(relic_lexer_t *lexer)
{
	while (more_input(lexer) && peek(lexer, 0) != '\n') {
		unsigned char byte;

		byte = peek(lexer, 0);
		advance_byte(lexer);

		if (byte == '"')
			return;

		if (byte == '\\' && peek(lexer, 0) != '\n')
			advance_byte(lexer);
	}
}

/*
 * Recognize an operator whose first byte has been consumed and which is
 * complete only with `expected` after it.  `:` and `!` begin nothing else,
 * so a missing second byte is reported as the operator the writer was part
 * way through rather than as an unexpected character.
 *
 * Returns whether the operator was completed, so that a caller with more to
 * fill in does not have to rediscover that from the token.
 */
static bool
two_byte_operator(relic_lexer_t *lexer, relic_token_t *token,
    unsigned char expected, enum relic_token_kind kind,
    enum relic_lex_error error)
{
	if (peek(lexer, 0) != expected) {
		fail_token(token, error);
		return (false);
	}
	advance_byte(lexer);

	token->kind = kind;

	return (true);
}

/*
 * Recognize a punctuation mark or an operator, or report that the byte
 * begins no token at all.
 */
static int
scan_punctuation(relic_lexer_t *lexer, relic_token_t *token)
{
	unsigned char byte;

	byte = peek(lexer, 0);
	advance_byte(lexer);

	switch (byte) {
	case '(':
		token->kind = RELIC_TOKEN_LPAREN;
		break;
	case ')':
		token->kind = RELIC_TOKEN_RPAREN;
		break;
	case ',':
		token->kind = RELIC_TOKEN_COMMA;
		break;
	case '.':
		token->kind = RELIC_TOKEN_DOT;
		break;
	case '?':
		token->kind = RELIC_TOKEN_QUESTION;
		break;
	case ':':
		two_byte_operator(lexer, token, '-', RELIC_TOKEN_IMPLIES,
		    RELIC_LEX_EXPECTED_IMPLIES);
		break;
	case '!':
		if (two_byte_operator(lexer, token, '=', RELIC_TOKEN_COMPARE,
		    RELIC_LEX_EXPECTED_NE))
			token->compare = RELIC_CMP_NE;
		break;
	case '=':
		token->kind = RELIC_TOKEN_COMPARE;
		token->compare = RELIC_CMP_EQ;
		break;
	case '<':
		token->kind = RELIC_TOKEN_COMPARE;
		token->compare = RELIC_CMP_LT;
		if (peek(lexer, 0) == '=') {
			advance_byte(lexer);
			token->compare = RELIC_CMP_LE;
		}
		break;
	case '>':
		token->kind = RELIC_TOKEN_COMPARE;
		token->compare = RELIC_CMP_GT;
		if (peek(lexer, 0) == '=') {
			advance_byte(lexer);
			token->compare = RELIC_CMP_GE;
		}
		break;
	default:
		token->byte = byte;
		fail_token(token, RELIC_LEX_BAD_BYTE);
		break;
	}

	return (0);
}
