/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"

/*
 * The parser, and the entry points that load text through it.
 *
 * Recursive descent over the grammar, one token of lookahead, and no
 * generated tables.  The parser owns every diagnostic the front end
 * produces, including the lexer's: the lexer hands back the reason a byte
 * sequence is not a token and this module decides whether anybody hears
 * about it.
 *
 * Error recovery synchronizes on the statement terminators, `.` and `?`.
 * After reporting, the parser discards tokens until it has consumed one of
 * them and resumes there, and it stays silent in between.  The first mistake
 * in a statement is the one worth reading; everything after it is the
 * parser's confusion rather than the writer's.  So a statement produces at
 * most one message, which makes the number of messages the number of broken
 * statements and lets a file with five mistakes report five lines.
 *
 * A load is all or nothing.  Statements are parsed onto a list belonging to
 * the load and spliced into the program only once the whole input has
 * parsed, so a program never contains half of a file.
 */

/*
 * How much of a token's text a diagnostic may quote.  Long text is truncated
 * because a message names one thing and the reader needs enough of it to
 * recognize which; no message the front end produces quotes anything that
 * reaches this.
 */
#define	RELIC_QUOTED_TEXT_MAX	40

/*
 * How much of a stream is read per call.  Only the number of read(2) calls
 * depends on it; the whole stream is read either way.
 */
#define	RELIC_READ_CHUNK	(64u * 1024u)

/* Enough for "'c'" or "0x7f" and a terminator. */
#define	RELIC_BYTE_TEXT_MAX	8

/* What a diagnostic calls input the caller did not name. */
#define	RELIC_UNNAMED_SOURCE	"<input>"

typedef struct relic_parser relic_parser_t;

/*
 * A parse in progress.
 *
 * `arena` is where the tree is built: the program arena for a load, and a
 * query's own arena for query text, which lives only as long as the query.
 *
 * `body` is a scratch array that every statement's literals are collected in
 * and then copied out of, so a body of any length costs one growable array
 * for the whole load rather than one per statement.  `statements` is this
 * parse's own list, which becomes part of the program only if a load
 * succeeds.
 *
 * A parse function returns false only after the load has been made certain
 * to fail.  There are three ways that happens and each means one thing, so
 * that the boundary can reconcile them without asking what else went on:
 *
 *	`reported`	a mistake in the source was reported for the statement
 *			being parsed, and counted.  Cleared when a statement
 *			ends, which is what limits a statement to one message.
 *
 *	`failed`	an allocation was refused *and already reported* by the
 *			module that made it -- interning is the only one --
 *			so parsing stops rather than continuing against a
 *			context that can no longer intern.
 *
 *	the arena	an allocation was refused and nobody reported it.
 *			Every allocation the parser makes marks the arena when
 *			it is refused, whether it came from the arena itself
 *			or from the scratch array a body is collected in, so
 *			no call site has to remember to.  parse_text()
 *			reports it, once, at the boundary.
 *
 * The division between the last two is what keeps one exhaustion to one
 * message: whoever reported it says so by setting `failed`, and the boundary
 * speaks only for the refusals that went unreported.
 */
struct relic_parser {
	relic_t			*ctx;
	relic_arena_t		*arena;
	relic_ast_literal_t	*body;
	size_t			 body_length;
	size_t			 body_capacity;
	relic_ast_stmt_list_t	 statements;
	size_t			 statement_count;
	size_t			 query_count;
	size_t			 error_count;
	relic_lexer_t		 lexer;
	relic_token_t		 token;		/* the current token */
	uint32_t		 negation_symbol;
	uint32_t		 anonymous_symbol;
	bool			 reported;
	bool			 failed;
};

static int	 add_fact(relic_t *ctx, const char *predicate, size_t arity,
		    const relic_value_t *values);
static void	 advance(relic_parser_t *parser);
static bool	 append_statement(relic_parser_t *parser,
		    const relic_ast_atom_t *head,
		    const relic_ast_literal_t *literals, size_t literal_count,
		    const relic_location_t *where);
static void	*parse_alloc(relic_parser_t *parser, size_t count,
		    size_t size);
static bool	 atom_after_name(relic_parser_t *parser,
		    const relic_token_t *name, relic_ast_atom_t *atom);
static bool	 body_append(relic_parser_t *parser,
		    const relic_ast_literal_t *literal);
static bool	 is_ground_fact(const relic_ast_stmt_t *statement);
static int	 stage_facts(relic_staging_t *staging,
		    relic_parser_t *parser);
static bool	 collect_body(relic_parser_t *parser,
		    const relic_ast_literal_t *first);
static const char *describe_byte(unsigned char byte, char *buffer,
		    size_t size);
static const char *describe_token(const relic_token_t *token);
static bool	 expect(relic_parser_t *parser, enum relic_token_kind kind,
		    const char *expected);
static bool	 finish_fact(relic_parser_t *parser,
		    const relic_ast_atom_t *head,
		    const relic_location_t *where);
static bool	 finish_query(relic_parser_t *parser,
		    const relic_ast_literal_t *first,
		    const relic_location_t *where);
static bool	 finish_rule(relic_parser_t *parser,
		    const relic_ast_atom_t *head,
		    const relic_location_t *where);
static bool	 head_is_positive(relic_parser_t *parser,
		    const relic_ast_literal_t *head, const char *what);
static int	 load_text(relic_t *ctx, const char *source_name,
		    const char *text, size_t length);
static int	 load_named_file(relic_t *ctx, const char *path);
static int	 load_stream(relic_t *ctx, const char *source_name,
		    FILE *stream);
static bool	 names_a_predicate(const char *predicate);
static const relic_ast_stmt_t *only_query(relic_parser_t *parser);
static bool	 parse_atom(relic_parser_t *parser, relic_ast_atom_t *atom);
static bool	 parse_comparison(relic_parser_t *parser,
		    relic_ast_literal_t *literal, const relic_ast_term_t *left);
static void	 parse_errorf(relic_parser_t *parser,
		    const relic_location_t *where, const char *format, ...)
		    RELIC_PRINTFLIKE(3, 4);
static bool	 parse_literal(relic_parser_t *parser,
		    relic_ast_literal_t *literal, const char *expected);
static int	 parse_program(relic_parser_t *parser);
static bool	 parse_statement(relic_parser_t *parser);
static bool	 parse_term(relic_parser_t *parser, relic_ast_term_t *term);
static int	 parse_text(relic_parser_t *parser, relic_t *ctx,
		    relic_arena_t *arena, const char *source_name,
		    const char *text, size_t length);
static int	 parser_init(relic_parser_t *parser, relic_t *ctx,
		    relic_arena_t *arena,
		    const char *source_name, const char *text, size_t length);
static void	 parser_reset(relic_parser_t *parser);
static const char *predicate_name(const relic_parser_t *parser,
		    uint32_t symbol);
static int	 quoted_precision(const relic_token_t *token);
static const char *quoted_suffix(const relic_token_t *token);
static int	 read_stream(relic_t *ctx, const char *source_name,
		    FILE *stream, char **textp, size_t *lengthp);
static void	 report_lex_error(relic_parser_t *parser);
static const char *source_name_of(relic_t *ctx, const char *name);
static void	 synchronize(relic_parser_t *parser);
static bool	 term_from_token(relic_parser_t *parser,
		    const relic_token_t *token, relic_ast_term_t *term);
static bool	 token_begins_term(const relic_token_t *token);

void
relic_ast_program_init(relic_ast_program_t *program)
{
	bzero(program, sizeof(*program));
	TAILQ_INIT(&program->statements);
}

/*
 * Report a mistake in the statement being parsed, unless one has already
 * been reported for it.  Every diagnostic the front end produces goes
 * through here, the lexer's included, which is what makes "one message per
 * statement" a property of the code rather than a rule to remember.
 */
static void
parse_errorf(relic_parser_t *parser, const relic_location_t *where,
    const char *format, ...)
{
	va_list arguments;

	if (parser->reported || parser->failed)
		return;

	parser->reported = true;
	parser->error_count++;

	va_start(arguments, format);
	relic_vdiagf(parser->ctx, RELIC_ERROR, where, format, arguments);
	va_end(arguments);
}

/*
 * Describe one byte for a message: as a character when it is printable, and
 * as its value when it is not.  A control byte rendered as itself would tell
 * the reader nothing and would travel to a terminal.
 */
static const char *
describe_byte(unsigned char byte, char *buffer, size_t size)
{
	if (byte >= ' ' && byte <= '~')
		snprintf(buffer, size, "'%c'", (int)byte);
	else
		snprintf(buffer, size, "0x%02x", (unsigned int)byte);

	return (buffer);
}

/*
 * What a token is, in the words a message uses after "found".
 *
 * A class rather than the spelling, so that the set of messages the front
 * end can produce is finite and the golden diagnostic tests compare
 * something stable.  Punctuation is its own spelling because the class and
 * the spelling are the same thing.
 */
static const char *
describe_token(const relic_token_t *token)
{
	switch (token->kind) {
	case RELIC_TOKEN_END:
		return ("end of input");
	case RELIC_TOKEN_SYMBOL:
		return ("a symbol");
	case RELIC_TOKEN_VARIABLE:
		return ("a variable");
	case RELIC_TOKEN_STRING:
		return ("a string literal");
	case RELIC_TOKEN_INTEGER:
		return ("an integer literal");
	case RELIC_TOKEN_COMPARE:
		return ("a comparison operator");
	case RELIC_TOKEN_IMPLIES:
		return ("':-'");
	case RELIC_TOKEN_LPAREN:
		return ("'('");
	case RELIC_TOKEN_RPAREN:
		return ("')'");
	case RELIC_TOKEN_COMMA:
		return ("','");
	case RELIC_TOKEN_DOT:
		return ("'.'");
	case RELIC_TOKEN_QUESTION:
		return ("'?'");
	case RELIC_TOKEN_ERROR:
		return ("invalid input");
	}

	/* Unreachable; a token of no known kind is described as invalid. */
	return ("invalid input");
}

/*
 * How much of a token's text a message may quote, as the precision of a
 * "%.*s" conversion.  The length comes from the input and a precision is an
 * int, so it is clamped rather than cast.
 */
static int
quoted_precision(const relic_token_t *token)
{
	if (token->text_length > RELIC_QUOTED_TEXT_MAX)
		return (RELIC_QUOTED_TEXT_MAX);

	return ((int)token->text_length);
}

/*
 * What follows the quoted text: "..." when quoted_precision() cut it, so a
 * reader knows the message does not hold the whole spelling.
 */
static const char *
quoted_suffix(const relic_token_t *token)
{
	return (token->text_length > RELIC_QUOTED_TEXT_MAX ? "..." : "");
}

/*
 * The spelling of an interned predicate name, for a message about it.  Every
 * byte of it is an identifier byte, so it is safe to quote.
 */
static const char *
predicate_name(const relic_parser_t *parser, uint32_t symbol)
{
	const char *bytes;

	bytes = relic_symbol_bytes(parser->ctx, symbol, NULL);
	if (bytes == NULL)
		return ("?");

	return (bytes);
}

/*
 * Turn the current error token into a message.
 */
static void
report_lex_error(relic_parser_t *parser)
{
	const relic_token_t *token;
	char described[RELIC_BYTE_TEXT_MAX];

	token = &parser->token;

	switch (token->error) {
	case RELIC_LEX_BAD_BYTE:
		parse_errorf(parser, &token->where, "unexpected character %s",
		    describe_byte(token->byte, described, sizeof(described)));
		break;
	case RELIC_LEX_BAD_ESCAPE:
		parse_errorf(parser, &token->where,
		    "unknown escape sequence %s in a string literal",
		    describe_byte(token->byte, described, sizeof(described)));
		break;
	case RELIC_LEX_BAD_HEX_ESCAPE:
		parse_errorf(parser, &token->where,
		    "a hexadecimal escape needs two hexadecimal digits");
		break;
	case RELIC_LEX_EXPECTED_IMPLIES:
		parse_errorf(parser, &token->where, "expected ':-'");
		break;
	case RELIC_LEX_EXPECTED_NE:
		parse_errorf(parser, &token->where, "expected '!='");
		break;
	case RELIC_LEX_INTEGER_RANGE:
		parse_errorf(parser, &token->where,
		    "integer literal out of range: %.*s%s",
		    quoted_precision(token), token->text,
		    quoted_suffix(token));
		break;
	case RELIC_LEX_INTEGER_TRAILING:
		parse_errorf(parser, &token->where,
		    "unexpected character %s after an integer literal",
		    describe_byte(token->byte, described, sizeof(described)));
		break;
	case RELIC_LEX_UNTERMINATED_STRING:
		parse_errorf(parser, &token->where,
		    "unterminated string literal");
		break;
	case RELIC_LEX_OK:
		break;
	}
}

/*
 * Take the next token.  A token the lexer could not make sense of is
 * reported here, so no other part of the parser has to consider the
 * possibility; it simply fails to match whatever it wanted, and the message
 * it would have produced is suppressed because this one came first.
 *
 * Where a statement ends is also decided here, and it has to be.  The token
 * of lookahead that a statement is recognized by is fetched by the very
 * advance that consumes the previous statement's terminator, so a lexical
 * error in the first token of a statement arrives while the statement before
 * it still owns `reported`.  Lifting the silence in parse_statement() would
 * be a step too late: the message would already have been charged to the
 * wrong statement, reported a second time by the generic complaint that
 * follows it when the previous statement was clean, and thrown away
 * altogether when it was not -- leaving "found invalid input" in place of
 * the reason.  Consuming a terminator is the one event that means a new
 * statement is starting, and this is the one place that sees it.
 */
static void
advance(relic_parser_t *parser)
{
	bool ended_statement;

	if (parser->failed)
		return;

	ended_statement = parser->token.kind == RELIC_TOKEN_DOT ||
	    parser->token.kind == RELIC_TOKEN_QUESTION;

	if (relic_lexer_next(&parser->lexer, &parser->token) != 0) {
		parser->failed = true;
		bzero(&parser->token, sizeof(parser->token));
		parser->token.kind = RELIC_TOKEN_END;
		return;
	}

	if (ended_statement)
		parser->reported = false;

	if (parser->token.kind == RELIC_TOKEN_ERROR)
		report_lex_error(parser);
}

/*
 * Consume the current token when it is the expected one, and otherwise
 * report what was wanted and what was there.  Naming both matters: a bare
 * "expected ','" leaves the reader to discover that the real mistake is a
 * closing parenthesis missing two lines earlier.
 */
static bool
expect(relic_parser_t *parser, enum relic_token_kind kind,
    const char *expected)
{
	if (parser->token.kind == kind) {
		advance(parser);
		return (true);
	}

	parse_errorf(parser, &parser->token.where, "expected %s, found %s",
	    expected, describe_token(&parser->token));

	return (false);
}

/*
 * Whether a term may begin with this token.
 *
 * Every kind is listed rather than defaulted, so that a token kind added
 * later is a build failure here instead of quietly beginning nothing.
 */
static bool
token_begins_term(const relic_token_t *token)
{
	switch (token->kind) {
	case RELIC_TOKEN_SYMBOL:
	case RELIC_TOKEN_VARIABLE:
	case RELIC_TOKEN_STRING:
	case RELIC_TOKEN_INTEGER:
		return (true);
	case RELIC_TOKEN_END:
	case RELIC_TOKEN_COMPARE:
	case RELIC_TOKEN_IMPLIES:
	case RELIC_TOKEN_LPAREN:
	case RELIC_TOKEN_RPAREN:
	case RELIC_TOKEN_COMMA:
	case RELIC_TOKEN_DOT:
	case RELIC_TOKEN_QUESTION:
	case RELIC_TOKEN_ERROR:
		return (false);
	}

	/* Unreachable; a token of no known kind begins nothing. */
	return (false);
}

/*
 * Allocate `count` items of `size` bytes for the tree, from the program
 * arena.
 *
 * This is the only way the parser allocates anything the tree keeps, and
 * that is what lets a null return here mean one thing: the arena has been
 * marked failed, and the load boundary will fail the load.  A caller can
 * therefore return without reporting, which is the early exit the arena's
 * sticky flag exists to provide.  A count taken from the input whose product
 * would wrap marks the arena too, so it reaches the boundary by the same path.
 */
static void *
parse_alloc(relic_parser_t *parser, size_t count, size_t size)
{
	return (relic_arena_allocarray(parser->arena, count, size));
}

/*
 * Build a term from the token that spells it, interning a constant as it
 * goes so that the tree holds the same domain identifier a tuple column
 * will.
 *
 * Every occurrence of a bare `_` denotes a distinct variable, and that is
 * recorded as a flag here rather than rediscovered by a name comparison
 * wherever variables are later examined.
 */
static bool
term_from_token(relic_parser_t *parser, const relic_token_t *token,
    relic_ast_term_t *term)
{
	relic_value_t value;

	bzero(term, sizeof(*term));
	term->where = token->where;

	switch (token->kind) {
	case RELIC_TOKEN_VARIABLE:
		term->kind = RELIC_TERM_VARIABLE;
		term->u.variable.name = token->symbol;
		term->u.variable.anonymous =
		    (token->symbol == parser->anonymous_symbol);
		return (true);
	case RELIC_TOKEN_SYMBOL:
		value.type = RELIC_SYMBOL;
		value.v.id = token->symbol;
		break;
	case RELIC_TOKEN_STRING:
		value.type = RELIC_STRING;
		value.v.id = token->symbol;
		break;
	case RELIC_TOKEN_INTEGER:
		value.type = RELIC_INTEGER;
		value.v.integer = token->integer;
		break;
	case RELIC_TOKEN_END:
	case RELIC_TOKEN_COMPARE:
	case RELIC_TOKEN_IMPLIES:
	case RELIC_TOKEN_LPAREN:
	case RELIC_TOKEN_RPAREN:
	case RELIC_TOKEN_COMMA:
	case RELIC_TOKEN_DOT:
	case RELIC_TOKEN_QUESTION:
	case RELIC_TOKEN_ERROR:
		/*
		 * Every caller has established that the token begins a term,
		 * so this is unreachable.  It reports rather than assuming,
		 * because assertions are compiled out of a release build and
		 * a term built from the wrong token would become a wrong
		 * answer instead of a failure.
		 */
		parse_errorf(parser, &token->where, "expected a term, found %s",
		    describe_token(token));
		return (false);
	}

	term->kind = RELIC_TERM_CONSTANT;

	if (relic_value_intern(parser->ctx, &value, &term->u.constant) != 0) {
		parser->failed = true;
		return (false);
	}

	return (true);
}

static bool
parse_term(relic_parser_t *parser, relic_ast_term_t *term)
{
	relic_token_t token;

	if (!token_begins_term(&parser->token)) {
		parse_errorf(parser, &parser->token.where,
		    "expected a term, found %s",
		    describe_token(&parser->token));
		return (false);
	}

	token = parser->token;
	advance(parser);

	return (term_from_token(parser, &token, term));
}

/*
 * Parse the rest of an atom, given the token that named its predicate.
 *
 * A name with no argument list is an atom of arity zero, which is an
 * ordinary relation holding no columns and not a special case anywhere
 * below.  The arguments are collected in a fixed buffer, which the arity
 * limit is what permits, and copied into the arena once their number is
 * known.
 */
static bool
atom_after_name(relic_parser_t *parser, const relic_token_t *name,
    relic_ast_atom_t *atom)
{
	relic_ast_term_t terms[RELIC_MAX_ARITY];
	relic_ast_term_t *stored;
	size_t term_count;

	bzero(atom, sizeof(*atom));
	atom->name = name->symbol;
	atom->where = name->where;

	if (parser->token.kind != RELIC_TOKEN_LPAREN)
		return (true);
	advance(parser);

	/*
	 * An empty argument list is tested for once, before the loop, rather
	 * than at the top of it.  Testing inside would also accept the
	 * trailing comma of `p(a,)`, which must be the error it looks like.
	 */
	term_count = 0;
	if (parser->token.kind != RELIC_TOKEN_RPAREN) {
		for (;;) {
			if (term_count == RELIC_MAX_ARITY) {
				parse_errorf(parser, &parser->token.where,
				    "predicate %s has more than %d arguments",
				    predicate_name(parser, name->symbol),
				    RELIC_MAX_ARITY);
				return (false);
			}

			if (!parse_term(parser, &terms[term_count]))
				return (false);
			term_count++;

			if (parser->token.kind != RELIC_TOKEN_COMMA)
				break;
			advance(parser);
		}
	}

	if (!expect(parser, RELIC_TOKEN_RPAREN, "',' or ')'"))
		return (false);

	if (term_count == 0)
		return (true);

	stored = parse_alloc(parser, term_count, sizeof(*stored));
	if (stored == NULL)
		return (false);
	bcopy(terms, stored, term_count * sizeof(*stored));

	atom->terms = stored;
	atom->arity = (uint16_t)term_count;

	return (true);
}

/*
 * Parse an atom from its predicate name onwards.
 *
 * `not` is refused here, which is what makes "no predicate can be named
 * not" true rather than nearly true.  A predicate of that name can never be
 * *defined*, because the keyword is recognized wherever a literal begins and
 * so `not.` and `not(a).` are both rejected already; without this check the
 * one surviving spelling, `not not`, would build a negated reference to a
 * relation nothing could ever fill, and it would look like an ordinary
 * predicate to every later stage.
 */
static bool
parse_atom(relic_parser_t *parser, relic_ast_atom_t *atom)
{
	relic_token_t name;

	if (parser->token.kind != RELIC_TOKEN_SYMBOL) {
		parse_errorf(parser, &parser->token.where,
		    "expected a predicate name, found %s",
		    describe_token(&parser->token));
		return (false);
	}

	if (parser->token.symbol == parser->negation_symbol) {
		parse_errorf(parser, &parser->token.where,
		    "not is the negation keyword and cannot name a predicate");
		return (false);
	}

	name = parser->token;
	advance(parser);

	return (atom_after_name(parser, &name, atom));
}

/*
 * Parse the operator and right operand of a comparison whose left operand
 * has already been built.  The caller has established that the current token
 * is the operator.
 */
static bool
parse_comparison(relic_parser_t *parser, relic_ast_literal_t *literal,
    const relic_ast_term_t *left)
{
	literal->kind = RELIC_LITERAL_COMPARISON;
	literal->u.comparison.left = *left;
	literal->u.comparison.op = parser->token.compare;
	advance(parser);

	return (parse_term(parser, &literal->u.comparison.right));
}

/*
 * Parse one literal: a positive atom, a negated atom, or a comparison.
 * `expected` names what the caller was looking for, so that the message for
 * a token that can begin no literal reads in the caller's terms.
 */
static bool
parse_literal(relic_parser_t *parser, relic_ast_literal_t *literal,
    const char *expected)
{
	relic_ast_term_t left;
	relic_token_t name;

	bzero(literal, sizeof(*literal));
	literal->where = parser->token.where;

	/*
	 * A literal begins with exactly the tokens a term begins with, which
	 * is not a coincidence worth relying on silently: a predicate name is
	 * spelled as a symbol, and a symbol is also a term, so the two sets
	 * have the same members for different reasons.
	 */
	if (!token_begins_term(&parser->token)) {
		parse_errorf(parser, &parser->token.where,
		    "expected %s, found %s", expected,
		    describe_token(&parser->token));
		return (false);
	}

	/*
	 * `not` introduces a negated literal wherever a literal may begin and
	 * is an ordinary symbol everywhere else, so status(foo, not) records
	 * a fact about the symbol `not` and only a predicate cannot be named
	 * one.
	 */
	if (parser->token.kind == RELIC_TOKEN_SYMBOL &&
	    parser->token.symbol == parser->negation_symbol) {
		advance(parser);
		literal->kind = RELIC_LITERAL_NEGATED;

		return (parse_atom(parser, &literal->u.atom));
	}

	/*
	 * A symbol begins either an atom or a comparison it is the left
	 * operand of, and the token after it decides which.  This is the
	 * grammar's one ambiguity and the only place a decision waits for the
	 * token of lookahead the parser already holds.
	 */
	if (parser->token.kind == RELIC_TOKEN_SYMBOL) {
		name = parser->token;
		advance(parser);

		if (parser->token.kind != RELIC_TOKEN_COMPARE) {
			literal->kind = RELIC_LITERAL_POSITIVE;

			return (atom_after_name(parser, &name,
			    &literal->u.atom));
		}

		if (!term_from_token(parser, &name, &left))
			return (false);

		return (parse_comparison(parser, literal, &left));
	}

	if (!parse_term(parser, &left))
		return (false);

	if (parser->token.kind != RELIC_TOKEN_COMPARE) {
		parse_errorf(parser, &parser->token.where,
		    "expected a comparison operator, found %s",
		    describe_token(&parser->token));
		return (false);
	}

	return (parse_comparison(parser, literal, &left));
}

/*
 * Append a literal to the scratch array the current statement's body is
 * collected in.
 *
 * A failure here is exhaustion like any other and is reported like any
 * other: by marking the tree's arena, leaving the parse boundary the single
 * place that turns an unreported refusal into a message.  Parsing is not
 * stopped, because `failed` means something narrower -- a refusal already
 * reported elsewhere -- and claiming it here would silence the boundary and
 * lose the message altogether.  Nothing is lost by carrying on: the marked
 * arena refuses every later tree allocation without reaching the allocator.
 */
static bool
body_append(relic_parser_t *parser, const relic_ast_literal_t *literal)
{
	relic_ast_literal_t *body;

	body = relic_grow_array(parser->body, &parser->body_capacity,
	    parser->body_length + 1, sizeof(*parser->body));
	if (body == NULL) {
		relic_arena_fail(parser->arena);
		return (false);
	}
	parser->body = body;

	parser->body[parser->body_length] = *literal;
	parser->body_length++;

	return (true);
}

/*
 * Collect a comma-separated body whose first literal has already been
 * parsed, into the scratch array.
 */
static bool
collect_body(relic_parser_t *parser, const relic_ast_literal_t *first)
{
	relic_ast_literal_t literal;

	if (!body_append(parser, first))
		return (false);

	while (parser->token.kind == RELIC_TOKEN_COMMA) {
		advance(parser);

		if (!parse_literal(parser, &literal, "a literal"))
			return (false);
		if (!body_append(parser, &literal))
			return (false);
	}

	return (true);
}

/*
 * Publish a parsed statement onto the load's list, copying its body into the
 * arena.
 *
 * Both halves of the statement arrive as parameters, so the scratch array the
 * literals were collected in stays a detail of the caller rather than a
 * second, invisible argument.  A statement with a head is a rule and one
 * without is a query: the kind is derived from the head rather than passed
 * beside it, so the two cannot come to disagree.
 */
static bool
append_statement(relic_parser_t *parser, const relic_ast_atom_t *head,
    const relic_ast_literal_t *literals, size_t literal_count,
    const relic_location_t *where)
{
	relic_ast_stmt_t *statement;
	relic_ast_literal_t *body;

	statement = parse_alloc(parser, 1, sizeof(*statement));
	if (statement == NULL)
		return (false);

	bzero(statement, sizeof(*statement));
	statement->where = *where;

	if (head != NULL) {
		statement->kind = RELIC_STMT_RULE;
		statement->head = *head;
	} else {
		statement->kind = RELIC_STMT_QUERY;
	}

	if (literal_count > 0) {
		body = parse_alloc(parser, literal_count, sizeof(*body));
		if (body == NULL)
			return (false);
		bcopy(literals, body, literal_count * sizeof(*body));

		statement->body = body;
		statement->body_length = literal_count;
	}

	TAILQ_INSERT_TAIL(&parser->statements, statement, link);
	parser->statement_count++;
	if (statement->kind == RELIC_STMT_QUERY)
		parser->query_count++;

	return (true);
}

/*
 * A fact and a rule head are both written as a literal, because which one is
 * being read is not known until the token after it, but only a positive atom
 * can be either: `not p(X).` asserts nothing and `X = 1.` names no relation.
 * `what` is the form the caller has by now established, so the message names
 * it.
 */
static bool
head_is_positive(relic_parser_t *parser, const relic_ast_literal_t *head,
    const char *what)
{
	if (head->kind == RELIC_LITERAL_POSITIVE)
		return (true);

	parse_errorf(parser, &head->where, "%s must be a positive literal",
	    what);

	return (false);
}

/*
 * The three statement forms, each entered with its head or first literal
 * already parsed.
 *
 * They differ in what they do with the token that told parse_statement which
 * form this is, and the difference is the grammar's rather than a
 * convention's, so each says so.  A fact's `.` and a rule's `:-` are
 * consumed here because nothing else wants them.  A query's is left alone:
 * it is either the `,` that collect_body() must see to keep going, or the
 * `?` that ends the statement and is expected after the body.
 */

/* Entered on the `.`, which ends the statement. */
static bool
finish_fact(relic_parser_t *parser, const relic_ast_atom_t *head,
    const relic_location_t *where)
{
	advance(parser);

	/* A fact is a rule with no body, which is what it means. */
	return (append_statement(parser, head, NULL, 0, where));
}

/* Entered on the `:-`, which separates the head from the body. */
static bool
finish_rule(relic_parser_t *parser, const relic_ast_atom_t *head,
    const relic_location_t *where)
{
	relic_ast_literal_t first;

	advance(parser);

	if (!parse_literal(parser, &first, "a literal"))
		return (false);
	if (!collect_body(parser, &first))
		return (false);
	if (!expect(parser, RELIC_TOKEN_DOT, "',' or '.'"))
		return (false);

	return (append_statement(parser, head, parser->body,
	    parser->body_length, where));
}

/* Entered on the `,` before the next literal, or on the closing `?`. */
static bool
finish_query(relic_parser_t *parser, const relic_ast_literal_t *first,
    const relic_location_t *where)
{
	if (!collect_body(parser, first))
		return (false);
	if (!expect(parser, RELIC_TOKEN_QUESTION, "',' or '?'"))
		return (false);

	return (append_statement(parser, NULL, parser->body,
	    parser->body_length, where));
}

/*
 * Parse one statement.
 *
 * Which of the three forms is being read is not known until the token after
 * the first literal: `:-` makes it a rule, `.` a fact, and `,` or `?` a
 * query whose body has already begun.
 */
static bool
parse_statement(relic_parser_t *parser)
{
	relic_ast_literal_t first;
	relic_location_t where;

	/*
	 * `reported` is not cleared here.  It belongs to the statement the
	 * current token begins, and advance() cleared it when it consumed
	 * the terminator that ended the previous one -- before scanning this
	 * statement's first token, so that a lexical error in it is charged
	 * here and reported once.
	 */
	parser->body_length = 0;
	where = parser->token.where;

	if (!parse_literal(parser, &first, "a fact, rule or query"))
		return (false);

	switch (parser->token.kind) {
	case RELIC_TOKEN_IMPLIES:
		if (!head_is_positive(parser, &first, "a rule head"))
			return (false);

		return (finish_rule(parser, &first.u.atom, &where));
	case RELIC_TOKEN_DOT:
		if (!head_is_positive(parser, &first, "a fact"))
			return (false);

		return (finish_fact(parser, &first.u.atom, &where));
	case RELIC_TOKEN_COMMA:
	case RELIC_TOKEN_QUESTION:
		return (finish_query(parser, &first, &where));
	default:
		parse_errorf(parser, &parser->token.where,
		    "expected ':-', ',', '.' or '?', found %s",
		    describe_token(&parser->token));
		return (false);
	}
}

/*
 * Discard tokens up to and including the next statement terminator, so that
 * one malformed statement costs one statement rather than the rest of the
 * file.
 *
 * Nothing is reported while this runs.  That is usually true already,
 * because the statement being abandoned has just had its message, but not
 * always: a statement abandoned for want of memory has had none, and the
 * wreckage it leaves could otherwise produce a lexical complaint about text
 * nobody is going to look at.  Silence is therefore asked for here rather
 * than inherited.  The count is unaffected, since it advances only when a
 * message is actually emitted, and the silence ends by itself: the advance
 * that consumes the terminator lifts it for the statement that follows.
 */
static void
synchronize(relic_parser_t *parser)
{
	parser->reported = true;

	while (!parser->failed && parser->token.kind != RELIC_TOKEN_END) {
		bool terminator;

		terminator = parser->token.kind == RELIC_TOKEN_DOT ||
		    parser->token.kind == RELIC_TOKEN_QUESTION;
		advance(parser);

		if (terminator)
			return;
	}
}

static int
parse_program(relic_parser_t *parser)
{
	advance(parser);		/* prime the current token */

	while (!parser->failed && parser->token.kind != RELIC_TOKEN_END) {
		if (!parse_statement(parser))
			synchronize(parser);
	}

	return (parser->failed ? -1 : 0);
}

static int
parser_init(relic_parser_t *parser, relic_t *ctx, relic_arena_t *arena,
    const char *source_name, const char *text, size_t length)
{
	bzero(parser, sizeof(*parser));

	parser->ctx = ctx;
	parser->arena = arena;
	TAILQ_INIT(&parser->statements);

	/*
	 * The two spellings the parser recognizes are interned once, so that
	 * every literal and every variable is matched against them by an
	 * integer comparison rather than by comparing bytes.
	 */
	if (relic_symbol_intern(ctx, "not", 3, &parser->negation_symbol) != 0)
		return (-1);
	if (relic_symbol_intern(ctx, "_", 1, &parser->anonymous_symbol) != 0)
		return (-1);

	relic_lexer_init(&parser->lexer, ctx, source_name, text, length);

	return (0);
}

static void
parser_reset(relic_parser_t *parser)
{
	relic_lexer_reset(&parser->lexer);
	free(parser->body);
	bzero(parser, sizeof(*parser));
}

/*
 * The stable name a location may point at for the life of the context.
 *
 * Source names are interned like any other byte string, so the bytes live in
 * the context arena and never move: a location stored in the tree stays
 * printable after the load that produced it has returned, and loading one
 * file twice stores one copy of its name.
 */
static const char *
source_name_of(relic_t *ctx, const char *name)
{
	uint32_t symbol;

	if (name == NULL)
		name = RELIC_UNNAMED_SOURCE;

	if (relic_symbol_intern(ctx, name, strlen(name), &symbol) != 0)
		return (NULL);

	return (relic_symbol_bytes(ctx, symbol, NULL));
}

/*
 * Parse text into a list of statements built in `arena`, leaving them on the
 * parser's own list.  Returns 0 when the whole text parsed, or -1 with every
 * mistake already reported.
 *
 * The three things that can make a parse fail are confirmed separately:
 * parsing itself, the arena's failure flag, and the count of statements that
 * were reported on.  The caller decides what to do with the statements only
 * once all three agree, which is what lets a load be all or nothing.
 *
 * The arena flag is cleared once its failure has been reported.  A parse is
 * the phase that flag belongs to, and leaving it set on the program arena
 * would make one transient exhaustion refuse every later load for the life of
 * the context.
 *
 * The parser is left initialized whether or not the parse succeeded, so the
 * caller releases it with parser_reset() either way.
 */
static int
parse_text(relic_parser_t *parser, relic_t *ctx, relic_arena_t *arena,
    const char *source_name, const char *text, size_t length)
{
	const char *name;
	int status;

	bzero(parser, sizeof(*parser));

	name = source_name_of(ctx, source_name);
	if (name == NULL)
		return (-1);

	if (parser_init(parser, ctx, arena, name, text, length) != 0)
		return (-1);

	status = parse_program(parser);

	/*
	 * A refusal nobody reported is reported here.  One that the interning
	 * layer already reported set `failed` instead, and saying it a second
	 * time would tell the reader nothing about a second thing.
	 */
	if (relic_arena_failed(arena)) {
		if (!parser->failed)
			relic_diag_no_memory(ctx);
		relic_arena_clear_failure(arena);
		status = -1;
	}

	if (parser->error_count > 0)
		status = -1;

	return (status);
}

/*
 * Parse text into the program.
 *
 * A load is all or nothing in what it adds to the program.  When the whole
 * text has parsed, its facts are taken off its statements and committed to
 * their base stores, and only when the commit succeeds are the remaining
 * statements -- rules, queries, and a fact with a variable, which the check
 * rejects -- spliced in.  What a failed load does keep is the arena memory
 * its statements occupied, and the predicates its facts named; an arena has
 * no per-allocation free, and the waste is bounded by the text the caller
 * chose to load.
 */
static int
load_text(relic_t *ctx, const char *source_name, const char *text,
    size_t length)
{
	relic_parser_t parser;
	relic_staging_t staging;
	int status;

	relic_staging_init(&staging, ctx);

	status = parse_text(&parser, ctx, &ctx->program_arena, source_name,
	    text, length);
	if (status == 0)
		status = stage_facts(&staging, &parser);
	if (status == 0)
		status = relic_staging_commit(&staging);

	if (status == 0) {
		TAILQ_CONCAT(&ctx->ast.statements, &parser.statements, link);
		ctx->ast.count += parser.statement_count;
		ctx->ast.query_count += parser.query_count;
	}

	relic_staging_reset(&staging);
	parser_reset(&parser);

	return (status);
}

/*
 * Whether a statement is a fact that can be a tuple: a rule with no body,
 * every argument of its head a constant.  A fact with a variable is not,
 * and stays a statement for the check to reject.
 */
static bool
is_ground_fact(const relic_ast_stmt_t *statement)
{
	uint16_t i;

	if (statement->kind != RELIC_STMT_RULE || statement->body_length != 0)
		return (false);

	for (i = 0; i < statement->head.arity; i++) {
		if (statement->head.terms[i].kind != RELIC_TERM_CONSTANT)
			return (false);
	}

	return (true);
}

/*
 * Take every fact that can be a tuple off the parser's statements and
 * stage it, interning its predicate.  Returns 0, or -1 with the reason
 * reported.
 */
static int
stage_facts(relic_staging_t *staging, relic_parser_t *parser)
{
	relic_ast_stmt_t *statement;
	relic_ast_stmt_t *next;
	relic_dom_t tuple[RELIC_MAX_ARITY];
	relic_pred_id_t pred;
	uint16_t i;

	TAILQ_FOREACH_SAFE(statement, &parser->statements, link, next) {
		if (!is_ground_fact(statement))
			continue;

		if (relic_pred_intern(staging->ctx, statement->head.name,
		    statement->head.arity, &statement->head.where, &pred) != 0)
			return (-1);

		for (i = 0; i < statement->head.arity; i++)
			tuple[i] = statement->head.terms[i].u.constant;

		if (relic_staging_add(staging, pred, tuple) != 0)
			return (-1);

		TAILQ_REMOVE(&parser->statements, statement, link);
		parser->statement_count--;
	}

	return (0);
}

/*
 * The one statement a successful parse of query text produced, or NULL having
 * reported why the text was not exactly one query.
 *
 * Query text is held to the grammar's query form, ending in `?`, rather than
 * being completed when the terminator is missing.  A rule or a fact is
 * refused rather than loaded: preparing a query must not change the program.
 */
static const relic_ast_stmt_t *
only_query(relic_parser_t *parser)
{
	const relic_ast_stmt_t *statement;
	const relic_ast_stmt_t *second;

	statement = TAILQ_FIRST(&parser->statements);
	if (statement == NULL) {
		relic_diagf(parser->ctx, RELIC_ERROR, &parser->token.where,
		    "expected a query, found end of input");
		return (NULL);
	}

	if (statement->kind != RELIC_STMT_QUERY) {
		relic_diagf(parser->ctx, RELIC_ERROR, &statement->where,
		    "expected a query, found a %s",
		    statement->body_length == 0 ? "fact" : "rule");
		return (NULL);
	}

	second = TAILQ_NEXT(statement, link);
	if (second != NULL) {
		relic_diagf(parser->ctx, RELIC_ERROR, &second->where,
		    "expected one query, found another statement after it");
		return (NULL);
	}

	return (statement);
}

/*
 * Parse text that must hold exactly one query, building it in `arena`.
 * Returns 0 with the statement in *statementp, or -1 with every mistake
 * already reported.
 *
 * The statement is not added to the program.  It lives in the caller's arena,
 * detached from any list, for as long as the caller keeps that arena.
 */
int
relic_parse_query(relic_t *ctx, relic_arena_t *arena, const char *source_name,
    const char *text, const relic_ast_stmt_t **statementp)
{
	relic_parser_t parser;
	const relic_ast_stmt_t *statement;
	int status;

	*statementp = NULL;

	status = parse_text(&parser, ctx, arena, source_name, text,
	    strlen(text));

	if (status == 0) {
		statement = only_query(&parser);
		if (statement != NULL)
			*statementp = statement;
		else
			status = -1;
	}

	parser_reset(&parser);

	return (status);
}

/*
 * Read a stream whole.
 *
 * The lexer's input contract asks for a NUL one byte past the text, so a
 * byte beyond what is read is always reserved for it.  A stream is read
 * whole rather than incrementally because the parser needs no more than one
 * byte of lookahead but the tree it produces is as large as the input
 * anyway, so an incremental reader would bound a footprint that is not the
 * one that matters.
 */
static int
read_stream(relic_t *ctx, const char *source_name, FILE *stream, char **textp,
    size_t *lengthp)
{
	char *text;
	size_t capacity;
	size_t length;

	text = NULL;
	capacity = 0;
	length = 0;

	for (;;) {
		char *grown;
		size_t room;
		size_t got;
		int read_errno;

		grown = relic_grow_array(text, &capacity,
		    length + RELIC_READ_CHUNK + 1, sizeof(*text));
		if (grown == NULL) {
			free(text);
			relic_diag_no_memory(ctx);
			return (-1);
		}
		text = grown;

		room = capacity - length - 1;

		/*
		 * Cleared first, because a stream whose error indicator was
		 * already set can return short without touching errno, and
		 * the message would then name whatever an earlier call left
		 * behind.
		 */
		errno = 0;
		got = fread(text + length, 1, room, stream);
		read_errno = errno;
		length += got;

		if (got == room)
			continue;

		if (ferror(stream)) {
			relic_diagf(ctx, RELIC_ERROR, NULL,
			    "cannot read %s: %s", source_name,
			    strerror(read_errno));
			free(text);
			return (-1);
		}

		break;
	}

	text[length] = '\0';

	*textp = text;
	*lengthp = length;

	return (0);
}

int
relic_load_string(relic_t *ctx, const char *source_name, const char *text)
{
	int status;

	if (relic_enter(ctx) != 0)
		return (-1);

	if (text != NULL) {
		status = load_text(ctx, source_name, text, strlen(text));
	} else {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "no program text was given");
		status = -1;
	}

	relic_leave(ctx);

	return (status);
}

int
relic_load_file(relic_t *ctx, const char *source_name, FILE *stream)
{
	int status;

	if (relic_enter(ctx) != 0)
		return (-1);

	status = load_stream(ctx, source_name, stream);

	relic_leave(ctx);

	return (status);
}

/*
 * relic_load_file() inside an operation already entered.  The stream is read
 * whole before any of it is parsed; its read function may be the caller's
 * own code, which finds the context busy like any callback.
 */
static int
load_stream(relic_t *ctx, const char *source_name, FILE *stream)
{
	const char *name;
	char *text;
	size_t length;
	int status;

	if (stream == NULL) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "no input stream was given");
		return (-1);
	}

	name = source_name != NULL ? source_name : RELIC_UNNAMED_SOURCE;

	if (read_stream(ctx, name, stream, &text, &length) != 0)
		return (-1);

	status = load_text(ctx, name, text, length);
	free(text);

	return (status);
}

int
relic_load_path(relic_t *ctx, const char *path)
{
	int status;

	if (relic_enter(ctx) != 0)
		return (-1);

	status = load_named_file(ctx, path);

	relic_leave(ctx);

	return (status);
}

/*
 * Open, load and close the file at `path`, naming it by its path.
 */
static int
load_named_file(relic_t *ctx, const char *path)
{
	FILE *stream;
	int status;

	if (path == NULL) {
		relic_diagf(ctx, RELIC_ERROR, NULL, "no path was given");
		return (-1);
	}

	stream = fopen(path, "r");
	if (stream == NULL) {
		relic_diagf(ctx, RELIC_ERROR, NULL, "cannot open %s: %s", path,
		    strerror(errno));
		return (-1);
	}

	status = load_stream(ctx, path, stream);

	if (fclose(stream) != 0 && status == 0) {
		relic_diagf(ctx, RELIC_ERROR, NULL, "cannot close %s: %s", path,
		    strerror(errno));
		status = -1;
	}

	return (status);
}

/*
 * Whether `predicate` may name a predicate: a symbol by the lexer's rule,
 * other than the negation keyword, which the grammar reads as `not` wherever
 * a predicate name would stand.
 */
static bool
names_a_predicate(const char *predicate)
{
	return (relic_lex_is_symbol(predicate, strlen(predicate)) &&
	    strcmp(predicate, "not") != 0);
}

/*
 * Add a fact to the program as though its text had been loaded.
 *
 * The fact is staged and committed exactly as a load commits its facts, so
 * there is one road into a base store and whatever a loaded fact is, this
 * one is too: in its predicate's base store when the call returns, refused
 * by a limit it does not fit, and discarding what was derived before it if
 * it is new.
 */
int
relic_fact_add(relic_t *ctx, const char *predicate, size_t arity,
    const relic_value_t *values)
{
	int status;

	if (relic_enter(ctx) != 0)
		return (-1);

	status = add_fact(ctx, predicate, arity, values);

	relic_leave(ctx);

	return (status);
}

static int
add_fact(relic_t *ctx, const char *predicate, size_t arity,
    const relic_value_t *values)
{
	relic_staging_t staging;
	relic_dom_t tuple[RELIC_MAX_ARITY];
	relic_pred_id_t pred;
	uint32_t name;
	size_t i;
	int status;

	if (predicate == NULL || (values == NULL && arity > 0)) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "a fact needs a predicate name and a value per argument");
		return (-1);
	}

	if (!names_a_predicate(predicate)) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "a predicate name is a lowercase letter followed by "
		    "letters, digits and underscores, and is not `not`");
		return (-1);
	}

	if (arity > RELIC_MAX_ARITY) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "a predicate may have at most %d arguments",
		    RELIC_MAX_ARITY);
		return (-1);
	}

	if (relic_symbol_intern(ctx, predicate, strlen(predicate), &name) != 0)
		return (-1);

	for (i = 0; i < arity; i++) {
		if (relic_value_intern(ctx, &values[i], &tuple[i]) != 0)
			return (-1);
	}

	if (relic_pred_intern(ctx, name, (uint16_t)arity, NULL, &pred) != 0)
		return (-1);

	relic_staging_init(&staging, ctx);
	status = relic_staging_add(&staging, pred, tuple);
	if (status == 0)
		status = relic_staging_commit(&staging);
	relic_staging_reset(&staging);

	return (status);
}
