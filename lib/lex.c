/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
 * all.  It interns nothing and it reports nothing.  A token carries its
 * bytes, and the parser decides what they are: a predicate's name, a
 * variable's, a function's, or a constant.  A byte sequence that is not a
 * token comes back as
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
static void	 advance_bytes(relic_lexer_t *lexer, size_t count);
static bool	 decoded_append(relic_lexer_t *lexer, unsigned char byte);
static size_t	 digits_length(const relic_lexer_t *lexer, size_t ahead);
static size_t	 exponent_marker_length(const relic_lexer_t *lexer,
		    size_t ahead);
static size_t	 float_form_length(const relic_lexer_t *lexer);
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

/*
 * Consume `count` bytes the caller has already examined through peek().
 */
static void
advance_bytes(relic_lexer_t *lexer, size_t count)
{
	for (; count > 0; count--)
		advance_byte(lexer);
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
	 * A `-` is never part of the literal after it, since `X-1` is a
	 * subtraction: it is an operator token, and the parser, which knows
	 * where a term stands, joins it to the digits of a negative literal.
	 */
	if (in_class(byte, LEX_DIGIT))
		return (scan_integer(lexer, token));

	if (byte == '"')
		return (scan_string(lexer, token));

	return (scan_punctuation(lexer, token));
}

/*
 * Mark the token as not being a token at all.  The parser resumes from the
 * scan position, which the scanner leaves past what it consumed: at the
 * offending byte for most errors, and past the whole literal for a string
 * with a bad escape or a reserved floating-point form, so that nothing
 * inside the literal is read as the next token.
 */
static void
fail_token(relic_token_t *token, enum relic_lex_error error)
{
	token->kind = RELIC_TOKEN_ERROR;
	token->error = error;
}

/*
 * Scan a name, whose spelling is the bytes the token spans.
 */
static int
scan_identifier(relic_lexer_t *lexer, relic_token_t *token,
    enum relic_token_kind kind)
{
	size_t start;

	start = lexer->offset;
	while (in_class(peek(lexer, 0), LEX_IDENT))
		advance_byte(lexer);

	token->bytes = lexer->text + start;
	token->length = lexer->offset - start;
	if (token->length > RELIC_STRING_LENGTH_MAX) {
		fail_token(token, RELIC_LEX_NAME_TOO_LONG);
		return (0);
	}
	token->kind = kind;

	return (0);
}

/*
 * Convert `length` bytes, which must all be digits, to their magnitude.
 *
 * The digits are accumulated in uint64_t, which holds RELIC_MAGNITUDE_MAX,
 * and each step checks that one more digit keeps the magnitude within it
 * before taking it.  The check is `magnitude * 10 + digit > bound`
 * rearranged so that nothing can wrap: `bound - digit` cannot, as the bound
 * is far above nine, and since the magnitude is a whole number, exceeding
 * the rounded-down quotient of `(bound - digit) / 10` is exactly exceeding
 * the exact one.  Every byte is examined even after the value has left the
 * range, so that a range such as `99999999999999999999x` is refused as no
 * integer at all rather than as one out of range: what the bytes are is
 * decided before what they are worth.
 */
enum relic_integer_verdict
relic_lex_magnitude(const char *bytes, size_t length, uint64_t *magnitudep)
{
	uint64_t magnitude;
	size_t i;
	bool out_of_range;

	if (length == 0)
		return (RELIC_INTEGER_MALFORMED);

	magnitude = 0;
	out_of_range = false;

	for (i = 0; i < length; i++) {
		unsigned char byte;
		unsigned int digit;

		byte = (unsigned char)bytes[i];
		if (!in_class(byte, LEX_DIGIT))
			return (RELIC_INTEGER_MALFORMED);
		digit = (unsigned int)(byte - '0');

		if (out_of_range ||
		    magnitude > (RELIC_MAGNITUDE_MAX - digit) / 10u) {
			out_of_range = true;
			continue;
		}
		magnitude = magnitude * 10u + digit;
	}

	if (out_of_range)
		return (RELIC_INTEGER_OUT_OF_RANGE);

	*magnitudep = magnitude;

	return (RELIC_INTEGER_VALID);
}

/*
 * A magnitude with a sign, when the result fits int64_t.  The magnitude of
 * INT64_MIN has no positive int64_t counterpart, so it is the one negative
 * value not formed by negating a positive one.
 */
enum relic_integer_verdict
relic_integer_signed(uint64_t magnitude, bool negative, int64_t *valuep)
{
	if (magnitude > (negative ? RELIC_MAGNITUDE_MAX : (uint64_t)INT64_MAX))
		return (RELIC_INTEGER_OUT_OF_RANGE);

	if (negative && magnitude == RELIC_MAGNITUDE_MAX)
		*valuep = INT64_MIN;
	else if (negative)
		*valuep = -(int64_t)magnitude;
	else
		*valuep = (int64_t)magnitude;

	return (RELIC_INTEGER_VALID);
}

/*
 * The whole rule, `-?[0-9]+` within int64_t, as a table's integer field is
 * held to it: an optional sign, the magnitude of the digits after it, and
 * the range checked with the sign known.
 */
enum relic_integer_verdict
relic_lex_integer(const char *bytes, size_t length, int64_t *valuep)
{
	enum relic_integer_verdict verdict;
	uint64_t magnitude;
	bool negative;

	negative = length > 0 && bytes[0] == '-';
	if (negative) {
		bytes++;
		length--;
	}

	verdict = relic_lex_magnitude(bytes, length, &magnitude);
	if (verdict != RELIC_INTEGER_VALID)
		return (verdict);

	return (relic_integer_signed(magnitude, negative, valuep));
}

/*
 * Scan the digits of an integer literal into its magnitude, or reject them
 * when they begin a floating-point form or run into an identifier byte.  A
 * magnitude no sign brings into range is recorded as RELIC_MAGNITUDE_MAX + 1
 * rather than refused here, because whether a magnitude fits depends on a
 * sign only the parser sees, and the parser reports the range of the literal
 * it makes.
 */
static int
scan_integer(relic_lexer_t *lexer, relic_token_t *token)
{
	enum relic_integer_verdict verdict;
	const char *digits;
	size_t digit_count, float_length;
	uint64_t magnitude;

	digits = lexer->text + lexer->offset;
	digit_count = digits_length(lexer, 0);
	advance_bytes(lexer, digit_count);

	/*
	 * Digits followed by a period and a digit, or by an exponent, are no
	 * integer.  The error points at the number's first digit, where the
	 * token already points, and the whole form is consumed, so that the
	 * period inside `1.5` is not taken for the end of the statement when
	 * the parser resumes after it.
	 */
	float_length = float_form_length(lexer);
	if (float_length > 0) {
		advance_bytes(lexer, float_length);
		fail_token(token, RELIC_LEX_NUMBER_UNSUPPORTED);
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

	verdict = relic_lex_magnitude(digits, digit_count, &magnitude);
	assert(verdict != RELIC_INTEGER_MALFORMED);
	if (verdict == RELIC_INTEGER_OUT_OF_RANGE)
		magnitude = RELIC_MAGNITUDE_MAX + 1u;

	token->kind = RELIC_TOKEN_INTEGER;
	token->magnitude = magnitude;

	return (0);
}

/*
 * The number of digits starting `ahead` bytes past the scan position.
 */
static size_t
digits_length(const relic_lexer_t *lexer, size_t ahead)
{
	size_t length;

	length = 0;
	while (in_class(peek(lexer, ahead + length), LEX_DIGIT))
		length++;

	return (length);
}

/*
 * The length of the exponent marker `ahead` bytes past the scan position --
 * `e` or `E` and an optional sign -- when a digit follows it, and zero when
 * the bytes there do not begin an exponent.  `1e` and `1ex` are therefore
 * not exponents and stay the integer's trailing-byte error.
 */
static size_t
exponent_marker_length(const relic_lexer_t *lexer, size_t ahead)
{
	unsigned char byte;
	size_t length;

	byte = peek(lexer, ahead);
	if (byte != 'e' && byte != 'E')
		return (0);
	length = 1;

	byte = peek(lexer, ahead + length);
	if (byte == '+' || byte == '-')
		length++;

	return (in_class(peek(lexer, ahead + length), LEX_DIGIT) ? length : 0);
}

/*
 * The length of the floating-point form that follows an integer's digits,
 * or zero when none does.  The form is a period and digits with an optional
 * exponent, the spelling a floating-point literal would take, or an exponent
 * alone, which no literal takes but which a writer means as a number.  A
 * period not followed by a digit is the statement's terminator and is no
 * part of the form.  Nothing past the input is read, since peek() answers
 * NUL there and NUL is no digit.
 */
static size_t
float_form_length(const relic_lexer_t *lexer)
{
	size_t length, marker;

	length = 0;
	if (peek(lexer, 0) == '.' && in_class(peek(lexer, 1), LEX_DIGIT))
		length = 1 + digits_length(lexer, 1);

	marker = exponent_marker_length(lexer, length);
	if (marker > 0)
		length += marker + digits_length(lexer, length + marker);

	return (length);
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
 * Scan a string literal, whose opening quote the scan position holds.  The
 * token's bytes are the decoded literal in the scratch buffer, which the
 * next literal overwrites, so they are read before the next token is
 * scanned.
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

	token->bytes = lexer->decoded;
	token->length = lexer->decoded_length;
	if (token->length > RELIC_STRING_LENGTH_MAX) {
		fail_token(token, RELIC_LEX_STRING_TOO_LONG);
		return (0);
	}
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
 *
 * The remainder operator is spelled `mod` and scanned as a symbol, which the
 * parser takes for the operator where an operator may stand.  `%` stays the
 * comment it is wherever a token may begin: were it an operator inside an
 * expression and a comment outside one, the lexer would have to be told by
 * the parser which it was in.
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
	case '{':
		token->kind = RELIC_TOKEN_LBRACE;
		break;
	case '}':
		token->kind = RELIC_TOKEN_RBRACE;
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
	case '+':
		token->kind = RELIC_TOKEN_OPERATOR;
		token->arith = RELIC_ARITH_ADD;
		break;
	case '-':
		token->kind = RELIC_TOKEN_OPERATOR;
		token->arith = RELIC_ARITH_SUBTRACT;
		break;
	case '*':
		token->kind = RELIC_TOKEN_OPERATOR;
		token->arith = RELIC_ARITH_MULTIPLY;
		break;
	case '/':
		token->kind = RELIC_TOKEN_OPERATOR;
		token->arith = RELIC_ARITH_DIVIDE;
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
