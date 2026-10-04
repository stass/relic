/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "internal.h"

/*
 * A 32-bit Linux system opens a file over 2 GiB only when asked to with
 * O_LARGEFILE, and refuses it otherwise with EOVERFLOW; read(2) through the
 * descriptor then needs nothing more.  Where the flag does not exist, off_t
 * is 64 bits and every file opens without it.
 */
#ifndef O_LARGEFILE
#define	O_LARGEFILE	0
#endif

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
 * the load, and facts into the load's staging; both are added to the program
 * only once the whole input has parsed, so a program never contains half of
 * a file.
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

typedef struct relic_argument_offense relic_argument_offense_t;
typedef struct relic_function_name relic_function_name_t;
typedef struct relic_literal_argument relic_literal_argument_t;
typedef struct relic_literal_arguments relic_literal_arguments_t;
typedef struct relic_load relic_load_t;
typedef struct relic_parser relic_parser_t;

/*
 * Where an argument of a literal that began `name(` stopped being a term,
 * and what an atom's parser says there: `expected <expected>, found
 * <found>`.  It is the message the literal receives when it turns out to be
 * an atom, whose arguments must be terms.
 */
struct relic_argument_offense {
	relic_location_t	 where;
	const char		*expected;
	const char		*found;
};

/*
 * One argument of a literal that began `name(`, read before the parser can
 * know whether the literal is an atom or a call.  An argument that is a
 * term alone is kept as `term`, with `tree` NULL and nothing allocated,
 * since an atom keeps its terms as they are.  One that goes on past a term
 * is built as `tree`, which only a call can hold, and `offense` says where
 * it stopped being a term.
 */
struct relic_literal_argument {
	relic_ast_term_t		 term;		/* when tree is NULL */
	relic_ast_expr_t		*tree;
	relic_argument_offense_t	 offense;	/* when tree is not NULL */
};

/*
 * The `count` arguments of a literal that began `name(`, as many as an atom
 * may have.
 */
struct relic_literal_arguments {
	relic_literal_argument_t	args[RELIC_MAX_ARITY];
	size_t				count;
};

/*
 * An aggregate function's name as written, and the function it names.
 */
struct relic_function_name {
	const char		*spelling;
	enum relic_aggregate_fn	 fn;
};

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
 * `staging` is the load's staging, which a fact whose arguments are all
 * constants is added to instead of becoming a statement.  It is NULL when
 * the text is a query's, where a fact is a mistake: there it is kept as a
 * statement so that it can be refused as the fact it is.
 *
 * A parse function returns false only after the load has been made certain
 * to fail.  There are three ways that happens and each means one thing, so
 * that the boundary can reconcile them without asking what else went on:
 *
 *	`reported`	a mistake in the source was reported for the statement
 *			being parsed, and counted.  Cleared when a statement
 *			ends, which is what limits a statement to one message.
 *
 *	`failed`	an allocation or a limit refused something *and it was
 *			already reported* by the module that refused it --
 *			interning or staging -- so parsing stops rather than
 *			continuing against a context that can no longer
 *			intern or a load that can no longer commit.  Once it
 *			is set advance() returns false, and every caller
 *			returns false at once.  The token of lookahead is
 *			interned before the parser is done with what
 *			precedes it, so a fact or a term can still be in
 *			hand when the refusal comes, and finishing it would
 *			go on working against the refusal.
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
 *
 * `expression_descent` counts the parentheses and unary minuses the parser
 * is inside, which is the depth of its recursion into an expression.  Each
 * production that raises it lowers it again on every way out, so it is zero
 * between statements.
 *
 * `group_body` is the scratch array an aggregate's body is collected in, as
 * `body` is for a statement's, and `in_aggregate` says that one is being
 * read.  An aggregate is refused while another is open, before its body is
 * entered, so one scratch array serves every body and the parser descends
 * into at most one.
 */
struct relic_parser {
	relic_t			*ctx;
	relic_arena_t		*arena;
	relic_ast_literal_t	*body;
	size_t			 body_length;
	size_t			 body_capacity;
	relic_ast_literal_t	*group_body;
	size_t			 group_body_length;
	size_t			 group_body_capacity;
	relic_ast_stmt_list_t	 statements;
	relic_staging_t		*staging;
	size_t			 statement_count;
	size_t			 query_count;
	size_t			 error_count;
	relic_lexer_t		 lexer;
	relic_token_t		 token;		/* the current token */
	uint32_t		 negation_symbol;
	uint32_t		 anonymous_symbol;
	unsigned int		 expression_descent;
	bool			 in_aggregate;
	bool			 reported;
	bool			 failed;
};

/*
 * A load parsed and not yet committed: its statements, on the parser's own
 * list, and its facts, staged.
 */
struct relic_load {
	relic_parser_t	 parser;
	relic_staging_t	 staging;
};

static const char	*describe_added_fact(const char *predicate,
		    size_t arity, char *buffer, size_t size);
static int	 add_fact(relic_t *ctx, const char *predicate, size_t arity,
		    const relic_value_t *values);
static bool	 advance(relic_parser_t *parser) RELIC_MUST_CHECK;
static bool	 append_statement(relic_parser_t *parser,
		    const relic_ast_atom_t *head,
		    const relic_ast_literal_t *literals, size_t literal_count,
		    const relic_location_t *where);
static void	*parse_alloc(relic_parser_t *parser, size_t count,
		    size_t size);
static bool	 atom_after_name(relic_parser_t *parser,
		    const relic_token_t *name, relic_ast_atom_t *atom,
		    relic_ast_term_t scratch[static RELIC_MAX_ARITY]);
static bool	 body_append(relic_parser_t *parser,
		    const relic_ast_literal_t *literal);
static bool	 collect_group_body(relic_parser_t *parser,
		    relic_ast_aggregate_t *aggregate);
static bool	 group_append(relic_parser_t *parser,
		    const relic_ast_literal_t *literal);
static bool	 is_bare_variable(const relic_ast_expr_t *expr);
static bool	 parse_aggregate(relic_parser_t *parser,
		    relic_ast_literal_t *literal, const relic_ast_expr_t *left,
		    enum relic_compare_op op, const relic_token_t *name,
		    enum relic_aggregate_fn fn);
static bool	 parse_aggregated_variable(relic_parser_t *parser,
		    relic_ast_aggregate_t *aggregate);
static bool	 parse_right_operand(relic_parser_t *parser,
		    relic_ast_literal_t *literal);
static bool	 refuse_misplaced_aggregate(relic_parser_t *parser,
		    const relic_token_t *name);
static bool	 token_function(const relic_token_t *token,
		    enum relic_aggregate_fn *fnp);
static bool	 spells_aggregate(const char *bytes, size_t length,
		    enum relic_aggregate_fn *fnp);
static bool	 check_argument(relic_parser_t *parser,
		    enum relic_function fn, size_t position,
		    const relic_ast_expr_t *arg);
static bool	 check_call(relic_parser_t *parser,
		    const relic_ast_expr_t *expr);
static bool	 check_expression(relic_parser_t *parser,
		    const relic_ast_expr_t *expr);
static bool	 check_operand(relic_parser_t *parser,
		    const relic_ast_expr_t *operand, enum relic_arith_op op,
		    bool negation);
static bool	 complete_atom(relic_parser_t *parser,
		    const relic_token_t *name,
		    const relic_literal_arguments_t *arguments,
		    relic_ast_literal_t *literal,
		    relic_ast_term_t scratch[static RELIC_MAX_ARITY]);
static bool	 collect_body(relic_parser_t *parser,
		    const relic_ast_literal_t *first,
		    relic_ast_term_t scratch[static RELIC_MAX_ARITY]);
static bool	 continue_product(relic_parser_t *parser,
		    relic_ast_expr_t *first, relic_ast_expr_t **exprp);
static bool	 continue_sum(relic_parser_t *parser, relic_ast_expr_t *first,
		    relic_ast_expr_t **exprp);
static const char *describe_token(const relic_token_t *token);
static bool	 enter_nesting(relic_parser_t *parser,
		    const relic_location_t *where);
static bool	 expect(relic_parser_t *parser, enum relic_token_kind kind,
		    const char *expected);
static bool	 finish_fact(relic_parser_t *parser,
		    const relic_ast_atom_t *head,
		    const relic_location_t *where);
static bool	 finish_query(relic_parser_t *parser,
		    const relic_ast_literal_t *first,
		    relic_ast_term_t scratch[static RELIC_MAX_ARITY],
		    const relic_location_t *where);
static bool	 finish_rule(relic_parser_t *parser,
		    const relic_ast_atom_t *head,
		    const relic_location_t *where);
static bool	 head_is_positive(relic_parser_t *parser,
		    const relic_ast_literal_t *head, const char *what);
static bool	 integer_term(relic_parser_t *parser,
		    const relic_token_t *digits, const relic_token_t *minus,
		    relic_ast_term_t *term);
static bool	 is_ground(const relic_ast_atom_t *atom);
static bool	 keep_atom(relic_parser_t *parser, relic_ast_atom_t *atom);
static bool	 keep_literal(relic_parser_t *parser,
		    relic_ast_literal_t *literal);
static int	 load_commit(relic_load_t *load);
static int	 load_named_file(relic_t *ctx, const char *path);
static int	 load_parse(relic_load_t *load, relic_t *ctx,
		    const char *source_name, const char *text, size_t length);
static int	 load_read_text(relic_t *ctx, const char *source_name,
		    char *text, size_t length);
static void	 load_reset(relic_load_t *load);
static int	 load_stream(relic_t *ctx, const char *source_name,
		    FILE *stream);
static int	 load_text(relic_t *ctx, const char *source_name,
		    const char *text, size_t length);
static bool	 make_binary(relic_parser_t *parser, enum relic_arith_op op,
		    const relic_location_t *where, relic_ast_expr_t *left,
		    relic_ast_expr_t *right, relic_ast_expr_t **exprp);
static bool	 make_call(relic_parser_t *parser, const relic_token_t *name,
		    const relic_location_t *open, const relic_ast_call_t *call,
		    relic_ast_expr_t **exprp);
static bool	 make_leaf(relic_parser_t *parser, const relic_ast_term_t *term,
		    relic_ast_expr_t **exprp);
static bool	 make_negation(relic_parser_t *parser,
		    const relic_location_t *where, relic_ast_expr_t *operand,
		    relic_ast_expr_t **exprp);
static bool	 node_fits(relic_parser_t *parser, unsigned int height,
		    const relic_location_t *where);
static void	 note_offense(relic_argument_offense_t *offense,
		    const relic_token_t *token, const char *expected);
static const relic_ast_stmt_t *only_query(relic_parser_t *parser);
static const char *operator_spelling(enum relic_arith_op op);
static bool	 parse_after_minus(relic_parser_t *parser,
		    const relic_token_t *minus, relic_ast_expr_t **exprp);
static bool	 parse_atom(relic_parser_t *parser, relic_ast_atom_t *atom,
		    relic_ast_term_t scratch[static RELIC_MAX_ARITY]);
static bool	 parse_atom_or_call(relic_parser_t *parser,
		    const relic_token_t *name, relic_ast_literal_t *literal,
		    relic_ast_term_t scratch[static RELIC_MAX_ARITY]);
static bool	 parse_call(relic_parser_t *parser, const relic_token_t *name,
		    relic_ast_expr_t **exprp);
static bool	 parse_call_arguments(relic_parser_t *parser,
		    relic_ast_call_t *call);
static bool	 parse_call_literal(relic_parser_t *parser,
		    const relic_token_t *name, const relic_location_t *open,
		    const relic_literal_arguments_t *arguments,
		    relic_ast_literal_t *literal);
static bool	 parse_comparison(relic_parser_t *parser,
		    relic_ast_literal_t *literal, relic_ast_expr_t *left);
static bool	 parse_expression(relic_parser_t *parser,
		    relic_ast_expr_t **exprp);
static bool	 parse_inside_parentheses(relic_parser_t *parser,
		    relic_ast_expr_t **exprp);
static bool	 parse_parenthesized(relic_parser_t *parser,
		    relic_ast_expr_t **exprp);
static bool	 parse_primary(relic_parser_t *parser, relic_ast_expr_t **exprp);
static bool	 parse_unary(relic_parser_t *parser, relic_ast_expr_t **exprp);
static void	 parse_errorf(relic_parser_t *parser,
		    const relic_location_t *where, const char *format, ...)
		    RELIC_PRINTFLIKE(3, 4);
static bool	 parse_literal(relic_parser_t *parser,
		    relic_ast_literal_t *literal, const char *expected,
		    relic_ast_term_t scratch[static RELIC_MAX_ARITY]);
static bool	 parse_literal_argument(relic_parser_t *parser,
		    relic_literal_argument_t *argument);
static bool	 parse_literal_arguments(relic_parser_t *parser,
		    const relic_token_t *name,
		    relic_literal_arguments_t *arguments);
static int	 parse_program(relic_parser_t *parser);
static bool	 parse_statement(relic_parser_t *parser);
static bool	 parse_term(relic_parser_t *parser, relic_ast_term_t *term);
static int	 parse_text(relic_parser_t *parser, relic_t *ctx,
		    relic_arena_t *arena, relic_staging_t *staging,
		    const char *source_name, const char *text, size_t length);
static int	 parser_init(relic_parser_t *parser, relic_t *ctx,
		    relic_arena_t *arena, relic_staging_t *staging,
		    const char *source_name, const char *text, size_t length);
static void	 parser_reset(relic_parser_t *parser);
static bool	 refuse_call(relic_parser_t *parser,
		    const relic_ast_expr_t *expr);
static void	 report_arity_limit(relic_parser_t *parser,
		    const relic_token_t *name);
static int	 quoted_precision(const relic_token_t *token);
static const char *quoted_suffix(const relic_token_t *token);
static int	 read_stream(relic_t *ctx, const char *source_name,
		    FILE *stream, char **textp, size_t *lengthp);
static void	 report_lex_error(relic_parser_t *parser);
static bool	 stage_fact(relic_parser_t *parser,
		    const relic_ast_atom_t *fact);
static void	 synchronize(relic_parser_t *parser);
static bool	 term_from_token(relic_parser_t *parser,
		    const relic_token_t *token, relic_ast_term_t *term);
static bool	 token_begins_literal(const relic_token_t *token);
static bool	 token_begins_term(const relic_token_t *token);
static bool	 token_operator(const relic_parser_t *parser,
		    enum relic_arith_op *opp);

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
	relic_diag_verror(parser->ctx, RELIC_ERROR_REJECTED, where, format,
	    arguments);
	va_end(arguments);
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
	case RELIC_TOKEN_OPERATOR:
		switch (token->arith) {
		case RELIC_ARITH_ADD:
			return ("'+'");
		case RELIC_ARITH_SUBTRACT:
			return ("'-'");
		case RELIC_ARITH_MULTIPLY:
			return ("'*'");
		case RELIC_ARITH_DIVIDE:
			return ("'/'");
		case RELIC_ARITH_REMAINDER:
			break;
		}
		return ("an operator");
	case RELIC_TOKEN_COMPARE:
		return ("a comparison operator");
	case RELIC_TOKEN_IMPLIES:
		return ("':-'");
	case RELIC_TOKEN_LPAREN:
		return ("'('");
	case RELIC_TOKEN_RPAREN:
		return ("')'");
	case RELIC_TOKEN_LBRACE:
		return ("'{'");
	case RELIC_TOKEN_RBRACE:
		return ("'}'");
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
 * Report an argument list, at the current token, that is one argument past
 * the widest a predicate may have.  The predicate is named by `name`, cut and
 * marked as relic_diag_name() cuts a name, so that a long one cannot crowd
 * the limit out of the message.
 */
static void
report_arity_limit(relic_parser_t *parser, const relic_token_t *name)
{
	const char *bytes;
	char named[RELIC_NAME_TEXT_MAX];

	bytes = relic_symbol_bytes(parser->ctx, name->symbol, NULL);
	relic_diag_name(bytes != NULL ? bytes : "?", named, sizeof(named));
	parse_errorf(parser, &parser->token.where,
	    "predicate %s has more than %d arguments", named, RELIC_MAX_ARITY);
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
		    relic_diag_byte(token->byte, described, sizeof(described)));
		break;
	case RELIC_LEX_BAD_ESCAPE:
		parse_errorf(parser, &token->where,
		    "unknown escape sequence %s in a string literal",
		    relic_diag_byte(token->byte, described, sizeof(described)));
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
	case RELIC_LEX_INTEGER_TRAILING:
		parse_errorf(parser, &token->where,
		    "unexpected character %s after an integer literal",
		    relic_diag_byte(token->byte, described, sizeof(described)));
		break;
	case RELIC_LEX_NUMBER_UNSUPPORTED:
		parse_errorf(parser, &token->where,
		    "floating-point numbers are not supported");
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
 * Take the next token.  Returns false when the parse has failed, whether
 * the lexer's interning refused the token now or a refusal came earlier, and
 * the caller must then return false without finishing what it was doing;
 * see `failed`.  A token the lexer could not make sense of is not a failure.
 * It is reported here, so no other part of the parser has to consider the
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
static bool
advance(relic_parser_t *parser)
{
	bool ended_statement;

	if (parser->failed)
		return (false);

	ended_statement = parser->token.kind == RELIC_TOKEN_DOT ||
	    parser->token.kind == RELIC_TOKEN_QUESTION;

	if (relic_lexer_next(&parser->lexer, &parser->token) != 0) {
		parser->failed = true;
		bzero(&parser->token, sizeof(parser->token));
		parser->token.kind = RELIC_TOKEN_END;
		return (false);
	}

	if (ended_statement)
		parser->reported = false;

	if (parser->token.kind == RELIC_TOKEN_ERROR)
		report_lex_error(parser);

	return (true);
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
	if (parser->token.kind == kind)
		return (advance(parser));

	parse_errorf(parser, &parser->token.where, "expected %s, found %s",
	    expected, describe_token(&parser->token));

	return (false);
}

/*
 * Whether a term may begin with this token.  A `-` begins the negative
 * integer literal it and the digits after it spell.
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
	case RELIC_TOKEN_OPERATOR:
		return (token->arith == RELIC_ARITH_SUBTRACT);
	case RELIC_TOKEN_END:
	case RELIC_TOKEN_COMPARE:
	case RELIC_TOKEN_IMPLIES:
	case RELIC_TOKEN_LPAREN:
	case RELIC_TOKEN_RPAREN:
	case RELIC_TOKEN_LBRACE:
	case RELIC_TOKEN_RBRACE:
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
 * Whether a literal may begin with this token: whatever begins a term,
 * since a predicate name is spelled as a symbol and a comparison may begin
 * with any term, and a `(` or a `-` that begins a comparison's expression.
 */
static bool
token_begins_literal(const relic_token_t *token)
{
	return (token_begins_term(token) || token->kind == RELIC_TOKEN_LPAREN);
}

/*
 * Whether the current token is a binary operator, and which.  `mod` is a
 * symbol everywhere else: it is the remainder operator only where an
 * operator may stand, after an operand, where a symbol never could without
 * a comma before it.  So nothing is reserved, and a predicate or a constant
 * may still be called `mod`.
 *
 * `mod` is recognized by its spelling rather than interned beforehand, as
 * `not` is: an interned spelling takes a symbol identifier in every program,
 * and every identifier after it would move, and with them the stripes the
 * program's keys fall in and the storage it reports.
 */
static bool
token_operator(const relic_parser_t *parser, enum relic_arith_op *opp)
{
	const relic_token_t *token;

	token = &parser->token;

	if (token->kind == RELIC_TOKEN_OPERATOR) {
		*opp = token->arith;
		return (true);
	}

	if (token->kind == RELIC_TOKEN_SYMBOL && token->text_length == 3 &&
	    bcmp(token->text, "mod", 3) == 0) {
		*opp = RELIC_ARITH_REMAINDER;
		return (true);
	}

	return (false);
}

/*
 * Whether the current token spells an aggregate function, and which.  Like
 * `mod`, the four names are recognized by their spelling and only where the
 * grammar lets a function stand -- after the `=` of an equality, followed by
 * `(` or `{` -- so each remains an ordinary symbol everywhere else: a
 * constant, or the name of a predicate.
 */
static bool
token_function(const relic_token_t *token, enum relic_aggregate_fn *fnp)
{
	if (token->kind != RELIC_TOKEN_SYMBOL)
		return (false);

	return (spells_aggregate(token->text, token->text_length, fnp));
}

/*
 * Whether `length` bytes spell an aggregate function, and which.
 */
static bool
spells_aggregate(const char *bytes, size_t length, enum relic_aggregate_fn *fnp)
{
	static const relic_function_name_t functions[] = {
		{ "count", RELIC_AGG_COUNT },
		{ "sum", RELIC_AGG_SUM },
		{ "min", RELIC_AGG_MIN },
		{ "max", RELIC_AGG_MAX }
	};
	size_t spelled;
	size_t i;

	for (i = 0; i < sizeof(functions) / sizeof(functions[0]); i++) {
		spelled = strlen(functions[i].spelling);
		if (length == spelled &&
		    bcmp(bytes, functions[i].spelling, spelled) == 0) {
			*fnp = functions[i].fn;
			return (true);
		}
	}

	return (false);
}

/*
 * Refuse a function name followed by `(` or `{` where no aggregate may
 * stand, with the form an aggregate does take.  In operand position a
 * symbol is never legally followed by either, so the text can only have
 * been meant as an aggregate.  Always returns false, having reported.
 */
static bool
refuse_misplaced_aggregate(relic_parser_t *parser, const relic_token_t *name)
{
	parse_errorf(parser, &name->where,
	    "an aggregate is written V = count { ... }");

	return (false);
}

/*
 * How an operator is written, as a message names it.
 */
static const char *
operator_spelling(enum relic_arith_op op)
{
	switch (op) {
	case RELIC_ARITH_ADD:
		return ("+");
	case RELIC_ARITH_SUBTRACT:
		return ("-");
	case RELIC_ARITH_MULTIPLY:
		return ("*");
	case RELIC_ARITH_DIVIDE:
		return ("/");
	case RELIC_ARITH_REMAINDER:
		return ("mod");
	}

	return ("?");
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
		return (integer_term(parser, token, NULL, term));
	case RELIC_TOKEN_OPERATOR:
	case RELIC_TOKEN_END:
	case RELIC_TOKEN_COMPARE:
	case RELIC_TOKEN_IMPLIES:
	case RELIC_TOKEN_LPAREN:
	case RELIC_TOKEN_RPAREN:
	case RELIC_TOKEN_LBRACE:
	case RELIC_TOKEN_RBRACE:
	case RELIC_TOKEN_COMMA:
	case RELIC_TOKEN_DOT:
	case RELIC_TOKEN_QUESTION:
	case RELIC_TOKEN_ERROR:
		/*
		 * Every caller has established that the token begins a
		 * term, and has taken the `-` of a negative literal itself,
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

/*
 * Build the integer term that the digits of `digits` spell, negated when
 * `minus` is the `-` written before them and NULL otherwise.  The literal
 * begins at the sign, which is where a message about it points.
 *
 * The range is checked here, with the sign known, rather than by the lexer:
 * the magnitude of INT64_MIN fits only when it is negative, and a lexer that
 * refused it would make INT64_MIN impossible to write.
 */
static bool
integer_term(relic_parser_t *parser, const relic_token_t *digits,
    const relic_token_t *minus, relic_ast_term_t *term)
{
	relic_value_t value;

	bzero(term, sizeof(*term));
	term->where = minus != NULL ? minus->where : digits->where;
	term->kind = RELIC_TERM_CONSTANT;

	value.type = RELIC_INTEGER;
	if (relic_integer_signed(digits->magnitude, minus != NULL,
	    &value.v.integer) != RELIC_INTEGER_VALID) {
		parse_errorf(parser, &term->where,
		    "integer literal out of range: %s%.*s%s",
		    minus != NULL ? "-" : "", quoted_precision(digits),
		    digits->text, quoted_suffix(digits));
		return (false);
	}

	if (relic_value_intern(parser->ctx, &value, &term->u.constant) != 0) {
		parser->failed = true;
		return (false);
	}

	return (true);
}

/*
 * Parse a term: a constant, a variable, or a negative integer literal, whose
 * `-` and digits are two tokens, with or without space between them.
 */
static bool
parse_term(relic_parser_t *parser, relic_ast_term_t *term)
{
	relic_token_t minus;
	relic_token_t token;

	if (!token_begins_term(&parser->token)) {
		parse_errorf(parser, &parser->token.where,
		    "expected a term, found %s",
		    describe_token(&parser->token));
		return (false);
	}

	if (parser->token.kind == RELIC_TOKEN_OPERATOR) {
		minus = parser->token;
		if (!advance(parser))
			return (false);

		if (parser->token.kind != RELIC_TOKEN_INTEGER) {
			parse_errorf(parser, &parser->token.where,
			    "expected an integer literal, found %s",
			    describe_token(&parser->token));
			return (false);
		}

		token = parser->token;
		if (!advance(parser))
			return (false);

		return (integer_term(parser, &token, &minus, term));
	}

	token = parser->token;
	if (!advance(parser))
		return (false);

	return (term_from_token(parser, &token, term));
}

/*
 * Expressions.
 *
 * The grammar, from the loosest binding to the tightest:
 *
 *	expression	:= product (("+" | "-") product)*
 *	product		:= unary (("*" | "/" | "mod") unary)*
 *	unary		:= "-" unary | primary
 *	primary		:= integer | variable | symbol | string
 *			 | "(" expression ")"
 *			 | name "(" [expression ("," expression)*] ")"
 *
 * Binary operators associate to the left, so each level is a loop that
 * folds its operands into the tree as they are read, and only a
 * parenthesis, a call's parentheses and a unary minus recurse.  Those are
 * what expression_descent counts, and the 33rd is refused where it opens,
 * before the parser enters it: that is what bounds the recursion however
 * the text is shaped, including text that never builds a node.  Every node
 * is refused too when it would be taller than RELIC_MAX_EXPRESSION_DEPTH,
 * which bounds every later walk over the tree.
 *
 * A name is a call only where an operand may stand and only when `(`
 * follows it, as `mod` is an operator only after an operand, so no name is
 * reserved: `length(a, 4).` is still a fact.  Whether the name is a
 * function, and the constants in the tree of the right types, is checked
 * once the whole expression is read, by check_expression().
 *
 * A `-` directly before an integer literal is folded into the literal, as it
 * is wherever a term stands, so `X = -1` holds the constant -1 and not the
 * negation of 1, and INT64_MIN can be written.  Folding needs the token
 * after the `-`, so the minus is consumed first and the decision made on
 * what follows it.
 *
 * The leaves of a product and a sum are unary expressions, and a comparison
 * whose left operand is a symbol has consumed that symbol before it knows
 * it is reading an expression, so the two loops also continue from an
 * operand already built.
 */
static bool
parse_expression(relic_parser_t *parser, relic_ast_expr_t **exprp)
{
	relic_ast_expr_t *first;

	if (!parse_unary(parser, &first))
		return (false);

	return (continue_sum(parser, first, exprp));
}

/*
 * A sum whose first unary operand, `first`, has been parsed.
 */
static bool
continue_sum(relic_parser_t *parser, relic_ast_expr_t *first,
    relic_ast_expr_t **exprp)
{
	relic_ast_expr_t *left;
	relic_ast_expr_t *operand;
	relic_ast_expr_t *right;
	relic_location_t where;
	enum relic_arith_op op;

	if (!continue_product(parser, first, &left))
		return (false);

	while (token_operator(parser, &op) &&
	    (op == RELIC_ARITH_ADD || op == RELIC_ARITH_SUBTRACT)) {
		where = parser->token.where;
		if (!advance(parser))
			return (false);

		if (!parse_unary(parser, &operand))
			return (false);
		if (!continue_product(parser, operand, &right))
			return (false);
		if (!make_binary(parser, op, &where, left, right, &left))
			return (false);
	}

	*exprp = left;

	return (true);
}

/*
 * A product whose first unary operand, `first`, has been parsed.
 */
static bool
continue_product(relic_parser_t *parser, relic_ast_expr_t *first,
    relic_ast_expr_t **exprp)
{
	relic_ast_expr_t *left;
	relic_ast_expr_t *right;
	relic_location_t where;
	enum relic_arith_op op;

	left = first;

	while (token_operator(parser, &op) &&
	    (op == RELIC_ARITH_MULTIPLY || op == RELIC_ARITH_DIVIDE ||
	    op == RELIC_ARITH_REMAINDER)) {
		where = parser->token.where;
		if (!advance(parser))
			return (false);

		if (!parse_unary(parser, &right))
			return (false);
		if (!make_binary(parser, op, &where, left, right, &left))
			return (false);
	}

	*exprp = left;

	return (true);
}

/*
 * A unary expression: a negative integer literal, a negation, or a primary.
 */
static bool
parse_unary(relic_parser_t *parser, relic_ast_expr_t **exprp)
{
	relic_token_t minus;

	if (parser->token.kind != RELIC_TOKEN_OPERATOR ||
	    parser->token.arith != RELIC_ARITH_SUBTRACT)
		return (parse_primary(parser, exprp));

	minus = parser->token;
	if (!advance(parser))
		return (false);

	return (parse_after_minus(parser, &minus, exprp));
}

/*
 * The rest of a unary expression whose `-`, `minus`, has been consumed: the
 * negative integer literal it begins, or the negation of a unary
 * expression.
 */
static bool
parse_after_minus(relic_parser_t *parser, const relic_token_t *minus,
    relic_ast_expr_t **exprp)
{
	relic_ast_expr_t *operand;
	relic_ast_term_t term;
	relic_token_t digits;
	bool parsed;

	if (parser->token.kind == RELIC_TOKEN_INTEGER) {
		digits = parser->token;
		if (!advance(parser))
			return (false);
		if (!integer_term(parser, &digits, minus, &term))
			return (false);

		return (make_leaf(parser, &term, exprp));
	}

	if (!enter_nesting(parser, &minus->where))
		return (false);
	parsed = parse_unary(parser, &operand);
	parser->expression_descent--;
	if (!parsed)
		return (false);

	return (make_negation(parser, &minus->where, operand, exprp));
}

/*
 * A primary: a term, a call, or an expression in parentheses.  A
 * parenthesis builds no node of its own, so redundant ones leave the tree
 * as it would be without them.
 *
 * A symbol is a call's name when `(` follows it, which is known only once
 * the symbol is consumed, and the constant it spells otherwise; so it is
 * interned as a value only once it is known to be one.  An aggregate's name
 * followed by `{` is an aggregate written where none may stand.
 */
static bool
parse_primary(relic_parser_t *parser, relic_ast_expr_t **exprp)
{
	enum relic_aggregate_fn fn;
	relic_ast_term_t term;
	relic_token_t name;

	if (parser->token.kind == RELIC_TOKEN_LPAREN)
		return (parse_parenthesized(parser, exprp));

	if (!token_begins_term(&parser->token)) {
		parse_errorf(parser, &parser->token.where,
		    "expected an operand, found %s",
		    describe_token(&parser->token));
		return (false);
	}

	if (parser->token.kind != RELIC_TOKEN_SYMBOL) {
		if (!parse_term(parser, &term))
			return (false);

		return (make_leaf(parser, &term, exprp));
	}

	name = parser->token;
	if (!advance(parser))
		return (false);

	if (parser->token.kind == RELIC_TOKEN_LPAREN)
		return (parse_call(parser, &name, exprp));

	if (parser->token.kind == RELIC_TOKEN_LBRACE &&
	    token_function(&name, &fn))
		return (refuse_misplaced_aggregate(parser, &name));

	if (!term_from_token(parser, &name, &term))
		return (false);

	return (make_leaf(parser, &term, exprp));
}

/*
 * A call, from the `(` after its name, `name`: its arguments, each an
 * expression, and its `)`.  The `(` counts one level of the text's descent,
 * as a parenthesis does.
 */
static bool
parse_call(relic_parser_t *parser, const relic_token_t *name,
    relic_ast_expr_t **exprp)
{
	relic_ast_call_t call;
	relic_location_t open;
	bool parsed;

	open = parser->token.where;
	if (!enter_nesting(parser, &open))
		return (false);
	parsed = parse_call_arguments(parser, &call);
	parser->expression_descent--;
	if (!parsed)
		return (false);

	return (make_call(parser, name, &open, &call, exprp));
}

/*
 * The `(`, the arguments and the `)` of a call, into `call`.  Every argument
 * is parsed, and the first RELIC_MAX_FUNCTION_ARITY are kept: a call with
 * more is no function's, which check_call() reports from the count.
 */
static bool
parse_call_arguments(relic_parser_t *parser, relic_ast_call_t *call)
{
	relic_ast_expr_t *arg;

	bzero(call, sizeof(*call));
	if (!advance(parser))
		return (false);

	/*
	 * An empty argument list is tested for once, before the loop, as an
	 * atom's is, so that `f(a,)` is the error it looks like.
	 */
	if (parser->token.kind != RELIC_TOKEN_RPAREN) {
		for (;;) {
			if (!parse_expression(parser, &arg))
				return (false);
			if (call->argc < RELIC_MAX_FUNCTION_ARITY)
				call->args[call->argc] = arg;
			call->argc++;

			if (parser->token.kind != RELIC_TOKEN_COMMA)
				break;
			if (!advance(parser))
				return (false);
		}
	}

	return (expect(parser, RELIC_TOKEN_RPAREN, "',' or ')'"));
}

static bool
parse_parenthesized(relic_parser_t *parser, relic_ast_expr_t **exprp)
{
	bool parsed;

	if (!enter_nesting(parser, &parser->token.where))
		return (false);
	parsed = parse_inside_parentheses(parser, exprp);
	parser->expression_descent--;

	return (parsed);
}

/*
 * The `(`, the expression and the `)` of a parenthesized expression, whose
 * nesting the caller has counted.
 */
static bool
parse_inside_parentheses(relic_parser_t *parser, relic_ast_expr_t **exprp)
{
	if (!advance(parser))
		return (false);

	if (!parse_expression(parser, exprp))
		return (false);

	return (expect(parser, RELIC_TOKEN_RPAREN, "an operator or ')'"));
}

/*
 * Count one more parenthesis or unary minus the parser is inside, unless
 * that would be more than an expression may nest, which is reported where
 * the production opens.  The caller lowers the count again once the
 * production has been parsed, whether or not it parsed.
 */
static bool
enter_nesting(relic_parser_t *parser, const relic_location_t *where)
{
	if (parser->expression_descent == RELIC_MAX_EXPRESSION_DEPTH) {
		parse_errorf(parser, where,
		    "expression nested more than %d levels deep",
		    RELIC_MAX_EXPRESSION_DEPTH);
		return (false);
	}

	parser->expression_descent++;

	return (true);
}

/*
 * Whether a node of `height` may be built, reporting at its operator when it
 * may not.  One message covers both bounds, since a reader fixes both the
 * same way, by splitting the expression across assignments.
 */
static bool
node_fits(relic_parser_t *parser, unsigned int height,
    const relic_location_t *where)
{
	if (height <= RELIC_MAX_EXPRESSION_DEPTH)
		return (true);

	parse_errorf(parser, where, "expression nested more than %d levels deep",
	    RELIC_MAX_EXPRESSION_DEPTH);

	return (false);
}

/*
 * Check an expression once the whole of it has been read, in the order it
 * is written, so that the first mistake in the text is the one reported: a
 * constant of the wrong type under an operator or in a function's argument,
 * and a call of a name that is no function, or of a function with the
 * wrong number of arguments.
 *
 * These are checked over the finished tree rather than as each node is
 * built because the arguments of a literal that begins `name(` are parsed
 * as expressions before the parser knows whether the literal is an atom or
 * a call: an atom with `X + "a"` for an argument is reported as an atom
 * whose argument is not a term, and only an expression is held to what an
 * expression may hold.  The tree is no taller than the parser allowed, so
 * the recursion is bounded.
 */
static bool
check_expression(relic_parser_t *parser, const relic_ast_expr_t *expr)
{
	const relic_ast_expr_t *left;
	const relic_ast_expr_t *right;
	enum relic_arith_op op;

	switch (expr->kind) {
	case RELIC_AST_EXPR_TERM:
		return (true);
	case RELIC_AST_EXPR_NEGATE:
		if (!check_expression(parser, expr->u.operand))
			return (false);
		return (check_operand(parser, expr->u.operand,
		    RELIC_ARITH_SUBTRACT, true));
	case RELIC_AST_EXPR_BINARY:
		left = expr->u.binary.left;
		right = expr->u.binary.right;
		op = expr->u.binary.op;

		if (!check_expression(parser, left))
			return (false);
		if (!check_operand(parser, left, op, false))
			return (false);
		if (!check_expression(parser, right))
			return (false);
		return (check_operand(parser, right, op, false));
	case RELIC_AST_EXPR_CALL:
		return (check_call(parser, expr));
	}

	return (true);
}

/*
 * Check a call: that it calls a function, and then each argument, the
 * argument's own tree before its type, as they are written.
 */
static bool
check_call(relic_parser_t *parser, const relic_ast_expr_t *expr)
{
	const relic_ast_call_t *call;
	size_t i;

	call = &expr->u.call;
	if (!call->resolved)
		return (refuse_call(parser, expr));

	for (i = 0; i < call->argc; i++) {
		if (!check_expression(parser, call->args[i]))
			return (false);
		if (!check_argument(parser, call->fn, i, call->args[i]))
			return (false);
	}

	return (true);
}

/*
 * Refuse a call that calls no function, at its name, with what is wrong:
 * an aggregate's name, which is an aggregate written where an operand
 * stands; a name no function has; or a function's name with a number of
 * arguments it does not take.  Always returns false, having reported.
 */
static bool
refuse_call(relic_parser_t *parser, const relic_ast_expr_t *expr)
{
	enum relic_aggregate_fn aggregate;
	const relic_ast_call_t *call;
	const char *name;
	unsigned int least;
	unsigned int most;
	size_t length;
	char named[RELIC_NAME_TEXT_MAX];

	call = &expr->u.call;
	name = relic_symbol_bytes(parser->ctx, call->name, &length);
	if (name == NULL) {
		name = "?";
		length = 1;
	}

	if (spells_aggregate(name, length, &aggregate)) {
		parse_errorf(parser, &expr->where,
		    "an aggregate is written V = count { ... }");
		return (false);
	}

	relic_diag_name(name, named, sizeof(named));
	if (!relic_function_arities(name, length, &least, &most)) {
		parse_errorf(parser, &expr->where, "%s is not a function",
		    named);
		return (false);
	}

	if (least == most)
		parse_errorf(parser, &expr->where,
		    "%s takes %u argument%s, not %zu", named, least,
		    least == 1 ? "" : "s", call->argc);
	else
		parse_errorf(parser, &expr->where,
		    "%s takes %u or %u arguments, not %zu", named, least, most,
		    call->argc);

	return (false);
}

/*
 * Whether an argument may stand at `position` of a call of `fn`, reporting
 * where it is written when it may not.  As for an operator's operand, only
 * a constant's type is known here; a variable's is a property of the tuples
 * that bind it, and an operand of the wrong type then makes the comparison
 * false.
 */
static bool
check_argument(relic_parser_t *parser, enum relic_function fn,
    size_t position, const relic_ast_expr_t *arg)
{
	const relic_function_info_t *info;
	const relic_ast_term_t *term;
	enum relic_value_kind wanted;
	const char *bytes;
	relic_dom_t type;
	int64_t integer;
	size_t length;
	char described[RELIC_STRING_TEXT_MAX];
	char named[RELIC_NAME_TEXT_MAX];

	info = relic_function_info(fn);
	wanted = info->args[position];
	if (wanted == RELIC_KIND_ANY || arg->kind != RELIC_AST_EXPR_TERM)
		return (true);

	term = &arg->u.term;
	if (term->kind != RELIC_TERM_CONSTANT)
		return (true);

	type = term->u.constant & RELIC_DOM_TYPE_MASK;
	if ((wanted == RELIC_KIND_STRING && type == RELIC_DOM_STRING) ||
	    (wanted == RELIC_KIND_INTEGER && type == RELIC_DOM_INTEGER))
		return (true);

	switch (type) {
	case RELIC_DOM_INTEGER:
		if (!relic_value_integer(parser->ctx, term->u.constant,
		    &integer))
			integer = 0;
		parse_errorf(parser, &term->where,
		    "integer %" PRId64 " cannot be an argument of %s", integer,
		    info->spelling);
		return (false);
	case RELIC_DOM_STRING:
		bytes = relic_symbol_bytes(parser->ctx,
		    term->u.constant & RELIC_DOM_INDEX_MASK, &length);
		if (bytes == NULL) {
			bytes = "";
			length = 0;
		}
		parse_errorf(parser, &term->where,
		    "string %s cannot be an argument of %s",
		    relic_diag_string(bytes, length, described,
		    sizeof(described)), info->spelling);
		return (false);
	default:
		break;
	}

	bytes = relic_symbol_bytes(parser->ctx,
	    term->u.constant & RELIC_DOM_INDEX_MASK, NULL);
	parse_errorf(parser, &term->where, "symbol %s cannot be an argument of %s",
	    relic_diag_name(bytes != NULL ? bytes : "?", named, sizeof(named)),
	    info->spelling);

	return (false);
}

/*
 * Whether an expression may be an operand of an operator, reporting where it
 * is written when it may not.  A constant's type is a property of its token,
 * so a symbol or a string under an operator is a mistake that can be seen
 * here, and is reported where it is written rather than left to make the
 * comparison false for every binding.  A variable's type is a property of
 * the tuples that bind it, and is not checked.
 */
static bool
check_operand(relic_parser_t *parser, const relic_ast_expr_t *operand,
    enum relic_arith_op op, bool negation)
{
	const relic_ast_term_t *term;
	const char *spelling;
	const char *bytes;
	char named[RELIC_NAME_TEXT_MAX];

	if (operand->kind != RELIC_AST_EXPR_TERM)
		return (true);

	term = &operand->u.term;
	if (term->kind != RELIC_TERM_CONSTANT ||
	    (term->u.constant & RELIC_DOM_TYPE_MASK) == RELIC_DOM_INTEGER)
		return (true);

	spelling = negation ? "-" : operator_spelling(op);

	if ((term->u.constant & RELIC_DOM_TYPE_MASK) == RELIC_DOM_STRING) {
		parse_errorf(parser, &term->where,
		    "a string literal cannot be an operand of %s", spelling);
		return (false);
	}

	bytes = relic_symbol_bytes(parser->ctx,
	    term->u.constant & RELIC_DOM_INDEX_MASK, NULL);
	parse_errorf(parser, &term->where,
	    "symbol %s cannot be an operand of %s",
	    relic_diag_name(bytes != NULL ? bytes : "?", named, sizeof(named)),
	    spelling);

	return (false);
}

/*
 * A leaf of the tree, holding a term.
 */
static bool
make_leaf(relic_parser_t *parser, const relic_ast_term_t *term,
    relic_ast_expr_t **exprp)
{
	relic_ast_expr_t *leaf;

	leaf = parse_alloc(parser, 1, sizeof(*leaf));
	if (leaf == NULL)
		return (false);

	bzero(leaf, sizeof(*leaf));
	leaf->kind = RELIC_AST_EXPR_TERM;
	leaf->where = term->where;
	leaf->u.term = *term;
	leaf->height = 1;

	*exprp = leaf;

	return (true);
}

/*
 * A negation of `operand`, written at `where`.
 */
static bool
make_negation(relic_parser_t *parser, const relic_location_t *where,
    relic_ast_expr_t *operand, relic_ast_expr_t **exprp)
{
	relic_ast_expr_t *node;
	unsigned int height;

	height = (unsigned int)operand->height + 1;
	if (!node_fits(parser, height, where))
		return (false);

	node = parse_alloc(parser, 1, sizeof(*node));
	if (node == NULL)
		return (false);

	bzero(node, sizeof(*node));
	node->kind = RELIC_AST_EXPR_NEGATE;
	node->where = *where;
	node->u.operand = operand;
	node->height = (uint8_t)height;

	*exprp = node;

	return (true);
}

/*
 * `left op right`, the operator written at `where`.
 */
static bool
make_binary(relic_parser_t *parser, enum relic_arith_op op,
    const relic_location_t *where, relic_ast_expr_t *left,
    relic_ast_expr_t *right, relic_ast_expr_t **exprp)
{
	relic_ast_expr_t *node;
	unsigned int height;

	height = (unsigned int)(left->height > right->height ?
	    left->height : right->height) + 1;
	if (!node_fits(parser, height, where))
		return (false);

	node = parse_alloc(parser, 1, sizeof(*node));
	if (node == NULL)
		return (false);

	bzero(node, sizeof(*node));
	node->kind = RELIC_AST_EXPR_BINARY;
	node->where = *where;
	node->u.binary.left = left;
	node->u.binary.right = right;
	node->u.binary.op = op;
	node->height = (uint8_t)height;

	*exprp = node;

	return (true);
}

/*
 * A call of `name`, whose `(` is at `open`, with the arguments `call`
 * holds.  The name and the argument count are resolved to a function here
 * when they are one's; check_call() reports them when they are not.
 */
static bool
make_call(relic_parser_t *parser, const relic_token_t *name,
    const relic_location_t *open, const relic_ast_call_t *call,
    relic_ast_expr_t **exprp)
{
	relic_ast_expr_t *node;
	const char *bytes;
	unsigned int height;
	size_t length;
	size_t kept;
	size_t i;

	kept = call->argc < RELIC_MAX_FUNCTION_ARITY ? call->argc :
	    RELIC_MAX_FUNCTION_ARITY;
	height = 1;
	for (i = 0; i < kept; i++) {
		if ((unsigned int)call->args[i]->height + 1 > height)
			height = (unsigned int)call->args[i]->height + 1;
	}
	if (!node_fits(parser, height, &name->where))
		return (false);

	node = parse_alloc(parser, 1, sizeof(*node));
	if (node == NULL)
		return (false);

	bzero(node, sizeof(*node));
	node->kind = RELIC_AST_EXPR_CALL;
	node->where = name->where;
	node->u.call = *call;
	node->u.call.name = name->symbol;
	node->u.call.open = *open;
	node->height = (uint8_t)height;

	bytes = relic_symbol_bytes(parser->ctx, name->symbol, &length);
	if (bytes != NULL)
		node->u.call.resolved = relic_function_find(bytes, length,
		    call->argc, &node->u.call.fn);

	*exprp = node;

	return (true);
}

/*
 * Parse the rest of an atom, given the token that named its predicate.
 *
 * A name with no argument list is an atom of arity zero, which is an
 * ordinary relation holding no columns and not a special case anywhere
 * below.  The arguments are read into the caller's scratch buffer of
 * RELIC_MAX_ARITY terms, which the arity limit is what permits, and the atom
 * points into it.  Nothing is allocated: whether the atom is kept in the
 * tree, and so copied out by keep_atom(), is decided by what follows it.
 */
static bool
atom_after_name(relic_parser_t *parser, const relic_token_t *name,
    relic_ast_atom_t *atom,
    relic_ast_term_t scratch[static RELIC_MAX_ARITY])
{
	size_t term_count;

	bzero(atom, sizeof(*atom));
	atom->name = name->symbol;
	atom->where = name->where;

	if (parser->token.kind != RELIC_TOKEN_LPAREN)
		return (true);
	if (!advance(parser))
		return (false);

	/*
	 * An empty argument list is tested for once, before the loop, rather
	 * than at the top of it.  Testing inside would also accept the
	 * trailing comma of `p(a,)`, which must be the error it looks like.
	 */
	term_count = 0;
	if (parser->token.kind != RELIC_TOKEN_RPAREN) {
		for (;;) {
			if (term_count == RELIC_MAX_ARITY) {
				report_arity_limit(parser, name);
				return (false);
			}

			if (!parse_term(parser, &scratch[term_count]))
				return (false);
			term_count++;

			if (parser->token.kind != RELIC_TOKEN_COMMA)
				break;
			if (!advance(parser))
				return (false);
		}
	}

	if (!expect(parser, RELIC_TOKEN_RPAREN, "',' or ')'"))
		return (false);

	if (term_count > 0) {
		atom->terms = scratch;
		atom->arity = (uint16_t)term_count;
	}

	return (true);
}

/*
 * Copy an atom's arguments out of the scratch buffer they were read into
 * and into the tree's arena, so that the atom outlives the buffer.  Every
 * atom the tree keeps passes through here, a head in append_statement() and
 * a body's atom in body_append(), and no other way into the tree exists.
 */
static bool
keep_atom(relic_parser_t *parser, relic_ast_atom_t *atom)
{
	relic_ast_term_t *stored;

	if (atom->arity == 0)
		return (true);

	stored = parse_alloc(parser, atom->arity, sizeof(*stored));
	if (stored == NULL)
		return (false);
	bcopy(atom->terms, stored, atom->arity * sizeof(*stored));

	atom->terms = stored;

	return (true);
}

/*
 * Keep a literal's atom, if it has one.  A comparison's expressions and an
 * aggregate's body are built in the tree's arena as they are parsed, so
 * there is nothing of either outside to copy.
 */
static bool
keep_literal(relic_parser_t *parser, relic_ast_literal_t *literal)
{
	switch (literal->kind) {
	case RELIC_LITERAL_POSITIVE:
	case RELIC_LITERAL_NEGATED:
		return (keep_atom(parser, &literal->u.atom));
	case RELIC_LITERAL_COMPARISON:
	case RELIC_LITERAL_AGGREGATE:
		return (true);
	}

	/*
	 * Unreachable, since the switch names every kind.  A literal of no
	 * known kind cannot be kept safely, so the load is failed with a
	 * report rather than an assumption, as term_from_token() fails a
	 * token of no known kind.
	 */
	parse_errorf(parser, &literal->where, "a literal of unknown kind");
	return (false);
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
parse_atom(relic_parser_t *parser, relic_ast_atom_t *atom,
    relic_ast_term_t scratch[static RELIC_MAX_ARITY])
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
	if (!advance(parser))
		return (false);

	return (atom_after_name(parser, &name, atom, scratch));
}

/*
 * Parse the comparator and right operand of a comparison whose left operand
 * has already been built, checking each operand as it is completed.
 */
static bool
parse_comparison(relic_parser_t *parser, relic_ast_literal_t *literal,
    relic_ast_expr_t *left)
{
	if (!check_expression(parser, left))
		return (false);

	if (parser->token.kind != RELIC_TOKEN_COMPARE) {
		parse_errorf(parser, &parser->token.where,
		    "expected a comparison operator, found %s",
		    describe_token(&parser->token));
		return (false);
	}

	literal->kind = RELIC_LITERAL_COMPARISON;
	literal->u.comparison.left = left;
	literal->u.comparison.op = parser->token.compare;
	if (!advance(parser))
		return (false);

	return (parse_right_operand(parser, literal));
}

/*
 * The right operand of a comparison whose left operand and comparator are
 * in `literal`: an expression, or the one place an aggregate may stand.
 *
 * A function name is an aggregate's only when the token after it is `(` or
 * `{`, and the parser must consume the name to see that token.  So the name
 * is taken first; followed by either, the literal becomes the aggregate,
 * and otherwise the name is the symbol it always was, the first operand of
 * the expression.  A symbol in operand position is never legally followed
 * by `(` or `{`, so no program that parsed without aggregates reads
 * differently with them.
 */
static bool
parse_right_operand(relic_parser_t *parser, relic_ast_literal_t *literal)
{
	enum relic_aggregate_fn fn;
	relic_ast_expr_t *leaf;
	relic_ast_term_t term;
	relic_token_t name;

	if (!token_function(&parser->token, &fn)) {
		if (!parse_expression(parser, &literal->u.comparison.right))
			return (false);

		return (check_expression(parser, literal->u.comparison.right));
	}

	name = parser->token;
	if (!advance(parser))
		return (false);

	if (parser->token.kind == RELIC_TOKEN_LPAREN ||
	    parser->token.kind == RELIC_TOKEN_LBRACE)
		return (parse_aggregate(parser, literal,
		    literal->u.comparison.left, literal->u.comparison.op,
		    &name, fn));

	if (!term_from_token(parser, &name, &term))
		return (false);
	if (!make_leaf(parser, &term, &leaf))
		return (false);
	if (!continue_sum(parser, leaf, &literal->u.comparison.right))
		return (false);

	return (check_expression(parser, literal->u.comparison.right));
}

/*
 * Whether an expression is a variable alone, which is what may stand before
 * the `=` of an aggregate.
 */
static bool
is_bare_variable(const relic_ast_expr_t *expr)
{
	return (expr->kind == RELIC_AST_EXPR_TERM &&
	    expr->u.term.kind == RELIC_TERM_VARIABLE);
}

/*
 * Parse an aggregate from the token after its function name, `name`: the
 * aggregated variable in parentheses when the function takes one, then the
 * body in braces.  `left` and `op` are the comparison it was found in, which
 * must be an equality whose left side is a bare variable, the result.
 *
 * An aggregate inside an open one is refused before its body is entered,
 * which is what bounds the parser's descent at one level however the text
 * is shaped: the body is the only production that parses literals from
 * inside a literal.
 */
static bool
parse_aggregate(relic_parser_t *parser, relic_ast_literal_t *literal,
    const relic_ast_expr_t *left, enum relic_compare_op op,
    const relic_token_t *name, enum relic_aggregate_fn fn)
{
	relic_ast_aggregate_t *aggregate;
	relic_location_t where;
	bool collected;

	where = literal->where;

	if (parser->in_aggregate) {
		parse_errorf(parser, &name->where,
		    "an aggregate body cannot hold an aggregate");
		return (false);
	}

	if (op != RELIC_CMP_EQ || !is_bare_variable(left))
		return (refuse_misplaced_aggregate(parser, name));

	bzero(literal, sizeof(*literal));
	literal->where = where;
	literal->kind = RELIC_LITERAL_AGGREGATE;
	aggregate = &literal->u.aggregate;
	aggregate->result = left->u.term;
	aggregate->where = name->where;
	aggregate->fn = fn;

	if (!parse_aggregated_variable(parser, aggregate))
		return (false);

	if (!expect(parser, RELIC_TOKEN_LBRACE, "'{'"))
		return (false);

	parser->in_aggregate = true;
	collected = collect_group_body(parser, aggregate);
	parser->in_aggregate = false;
	if (!collected)
		return (false);

	return (expect(parser, RELIC_TOKEN_RBRACE, "',' or '}'"));
}

/*
 * The aggregated variable of `sum`, `min` or `max`, in parentheses, into
 * aggregate->column; `count` takes none.  The current token is the one after
 * the function name, a `(` or a `{`.
 *
 * The variable names a column of the group, and `_` names none, so it is
 * refused where it is written.
 */
static bool
parse_aggregated_variable(relic_parser_t *parser,
    relic_ast_aggregate_t *aggregate)
{
	const char *spelling;

	spelling = relic_aggregate_spelling(aggregate->fn);

	if (aggregate->fn == RELIC_AGG_COUNT) {
		if (parser->token.kind != RELIC_TOKEN_LPAREN)
			return (true);
		parse_errorf(parser, &parser->token.where,
		    "count takes no variable");
		return (false);
	}

	if (parser->token.kind != RELIC_TOKEN_LPAREN) {
		parse_errorf(parser, &parser->token.where,
		    "%s takes one variable in parentheses", spelling);
		return (false);
	}
	if (!advance(parser))
		return (false);

	if (parser->token.kind != RELIC_TOKEN_VARIABLE) {
		parse_errorf(parser, &parser->token.where,
		    "expected a variable, found %s",
		    describe_token(&parser->token));
		return (false);
	}
	if (parser->token.symbol == parser->anonymous_symbol) {
		parse_errorf(parser, &parser->token.where,
		    "%s takes a named variable, not _", spelling);
		return (false);
	}
	if (!term_from_token(parser, &parser->token, &aggregate->column))
		return (false);
	if (!advance(parser))
		return (false);

	return (expect(parser, RELIC_TOKEN_RPAREN, "')'"));
}

/*
 * Collect an aggregate's comma-separated body, at least one literal, into
 * the group scratch array, and copy it into the tree's arena.  Each atom's
 * arguments are read into a buffer here and kept before the next literal is
 * read, as collect_body() keeps a statement's.
 */
static bool
collect_group_body(relic_parser_t *parser, relic_ast_aggregate_t *aggregate)
{
	relic_ast_term_t scratch[RELIC_MAX_ARITY];
	relic_ast_literal_t literal;
	relic_ast_literal_t *body;

	parser->group_body_length = 0;

	for (;;) {
		if (!parse_literal(parser, &literal, "a literal", scratch))
			return (false);
		if (!group_append(parser, &literal))
			return (false);

		if (parser->token.kind != RELIC_TOKEN_COMMA)
			break;
		if (!advance(parser))
			return (false);
	}

	body = parse_alloc(parser, parser->group_body_length, sizeof(*body));
	if (body == NULL)
		return (false);
	bcopy(parser->group_body, body,
	    parser->group_body_length * sizeof(*body));

	aggregate->body = body;
	aggregate->body_length = parser->group_body_length;

	return (true);
}

/*
 * Append a literal to the group scratch array, keeping its atom's arguments
 * in the tree, with the failure contract of body_append().
 */
static bool
group_append(relic_parser_t *parser, const relic_ast_literal_t *literal)
{
	relic_ast_literal_t *body;
	relic_ast_literal_t kept;

	kept = *literal;
	if (!keep_literal(parser, &kept))
		return (false);

	body = relic_grow_array(parser->group_body,
	    &parser->group_body_capacity, parser->group_body_length + 1,
	    sizeof(*parser->group_body));
	if (body == NULL) {
		relic_arena_fail(parser->arena);
		return (false);
	}
	parser->group_body = body;

	parser->group_body[parser->group_body_length] = kept;
	parser->group_body_length++;

	return (true);
}

/*
 * A literal that begins `name(`: an atom, or a comparison whose left
 * operand begins with a call of `name`.  Which one is decided by the token
 * after the `)`, as a symbol alone is decided by the token after it: an
 * operator or a comparator continues a call, and anything else ends an
 * atom, what may follow a literal being its caller's to check.
 *
 * The arguments are read before that token is seen, so each is read as an
 * expression, and one that is a term alone -- as every argument of an atom
 * must be -- is kept as the term with nothing allocated: a fact is a
 * literal of this shape, and reading a million of them must cost what it
 * did before calls existed.
 *
 * The `(` counts one level of the text's descent, as a call's does.  An
 * atom's arguments are terms, which never descend, so the count constrains
 * only a call.
 */
static bool
parse_atom_or_call(relic_parser_t *parser, const relic_token_t *name,
    relic_ast_literal_t *literal,
    relic_ast_term_t scratch[static RELIC_MAX_ARITY])
{
	relic_literal_arguments_t arguments;
	relic_location_t open;
	enum relic_arith_op op;
	bool parsed;

	open = parser->token.where;
	if (!enter_nesting(parser, &open))
		return (false);
	parsed = parse_literal_arguments(parser, name, &arguments);
	parser->expression_descent--;
	if (!parsed)
		return (false);

	if (parser->token.kind == RELIC_TOKEN_COMPARE ||
	    token_operator(parser, &op))
		return (parse_call_literal(parser, name, &open, &arguments,
		    literal));

	return (complete_atom(parser, name, &arguments, literal, scratch));
}

/*
 * The `(`, the arguments and the `)` of a literal that began `name(`, into
 * `arguments`.
 */
static bool
parse_literal_arguments(relic_parser_t *parser, const relic_token_t *name,
    relic_literal_arguments_t *arguments)
{
	arguments->count = 0;
	if (!advance(parser))
		return (false);

	/*
	 * An empty argument list is tested for once, before the loop, rather
	 * than at the top of it.  Testing inside would also accept the
	 * trailing comma of `p(a,)`, which must be the error it looks like.
	 */
	if (parser->token.kind != RELIC_TOKEN_RPAREN) {
		for (;;) {
			if (arguments->count == RELIC_MAX_ARITY) {
				report_arity_limit(parser, name);
				return (false);
			}

			if (!parse_literal_argument(parser,
			    &arguments->args[arguments->count]))
				return (false);
			arguments->count++;

			if (parser->token.kind != RELIC_TOKEN_COMMA)
				break;
			if (!advance(parser))
				return (false);
		}
	}

	return (expect(parser, RELIC_TOKEN_RPAREN, "',' or ')'"));
}

/*
 * Record where an argument stops being a term: at `token`, which an atom's
 * parser, wanting `expected` there, would have reported.
 */
static void
note_offense(relic_argument_offense_t *offense, const relic_token_t *token,
    const char *expected)
{
	offense->where = token->where;
	offense->expected = expected;
	offense->found = describe_token(token);
}

/*
 * One argument of a literal that began `name(`, into *argument: a term, or
 * an expression that is more than a term with the place it stopped being
 * one.
 *
 * An argument is a term unless an operator follows it, it begins a call,
 * or it is an expression in parentheses or under a unary minus; each of
 * those is where an atom's parser stops, and the offense records the
 * message it gives there.  A token that begins no term gets the message a
 * term's absence always has, as parse_term() gives it.
 */
static bool
parse_literal_argument(relic_parser_t *parser,
    relic_literal_argument_t *argument)
{
	relic_argument_offense_t *offense;
	relic_ast_term_t *term;
	relic_ast_expr_t *first;
	relic_token_t digits;
	relic_token_t token;
	enum relic_arith_op op;

	argument->tree = NULL;
	term = &argument->term;
	offense = &argument->offense;
	token = parser->token;

	if (token.kind == RELIC_TOKEN_SYMBOL) {
		if (!advance(parser))
			return (false);

		if (parser->token.kind == RELIC_TOKEN_LPAREN) {
			note_offense(offense, &parser->token, "',' or ')'");
			if (!parse_call(parser, &token, &first))
				return (false);

			return (continue_sum(parser, first, &argument->tree));
		}

		if (!term_from_token(parser, &token, term))
			return (false);
	} else if (token.kind == RELIC_TOKEN_OPERATOR &&
	    token.arith == RELIC_ARITH_SUBTRACT) {
		if (!advance(parser))
			return (false);

		if (parser->token.kind != RELIC_TOKEN_INTEGER) {
			note_offense(offense, &parser->token,
			    "an integer literal");
			if (!parse_after_minus(parser, &token, &first))
				return (false);

			return (continue_sum(parser, first, &argument->tree));
		}

		digits = parser->token;
		if (!advance(parser))
			return (false);
		if (!integer_term(parser, &digits, &token, term))
			return (false);
	} else if (token.kind == RELIC_TOKEN_LPAREN) {
		note_offense(offense, &token, "a term");

		return (parse_expression(parser, &argument->tree));
	} else if (!parse_term(parser, term)) {
		return (false);
	}

	if (!token_operator(parser, &op))
		return (true);

	note_offense(offense, &parser->token, "',' or ')'");
	if (!make_leaf(parser, term, &first))
		return (false);

	return (continue_sum(parser, first, &argument->tree));
}

/*
 * A literal that began `name(` and is an atom, whose arguments must be
 * terms.  The first that is not is refused with the message an atom's
 * parser gives where it stops being one; otherwise the terms are copied into
 * `scratch`, which the atom points into until it is kept.
 */
static bool
complete_atom(relic_parser_t *parser, const relic_token_t *name,
    const relic_literal_arguments_t *arguments, relic_ast_literal_t *literal,
    relic_ast_term_t scratch[static RELIC_MAX_ARITY])
{
	const relic_argument_offense_t *offense;
	relic_ast_atom_t *atom;
	size_t i;

	for (i = 0; i < arguments->count; i++) {
		if (arguments->args[i].tree == NULL)
			continue;

		offense = &arguments->args[i].offense;
		parse_errorf(parser, &offense->where, "expected %s, found %s",
		    offense->expected, offense->found);
		return (false);
	}

	for (i = 0; i < arguments->count; i++)
		scratch[i] = arguments->args[i].term;

	literal->kind = RELIC_LITERAL_POSITIVE;
	atom = &literal->u.atom;
	atom->name = name->symbol;
	atom->where = name->where;
	if (arguments->count > 0) {
		atom->terms = scratch;
		atom->arity = (uint16_t)arguments->count;
	}

	return (true);
}

/*
 * A literal that began `name(` and is a comparison, whose left operand
 * begins with the call of `name` on `arguments`.  A term is given its leaf
 * now that it is known to be an expression's.  Arguments past the ones a
 * function can take are counted and not kept, which leaves the call
 * unresolved, and check_call() refuses it for its arity.
 */
static bool
parse_call_literal(relic_parser_t *parser, const relic_token_t *name,
    const relic_location_t *open, const relic_literal_arguments_t *arguments,
    relic_ast_literal_t *literal)
{
	const relic_literal_argument_t *argument;
	relic_ast_expr_t *left;
	relic_ast_expr_t *first;
	relic_ast_call_t call;
	size_t i;

	bzero(&call, sizeof(call));
	call.argc = arguments->count;
	for (i = 0; i < arguments->count && i < RELIC_MAX_FUNCTION_ARITY; i++) {
		argument = &arguments->args[i];
		if (argument->tree != NULL)
			call.args[i] = argument->tree;
		else if (!make_leaf(parser, &argument->term, &call.args[i]))
			return (false);
	}

	if (!make_call(parser, name, open, &call, &first))
		return (false);
	if (!continue_sum(parser, first, &left))
		return (false);

	return (parse_comparison(parser, literal, left));
}

/*
 * Parse one literal: a positive atom, a negated atom, a comparison, or an
 * aggregate, which is found as the right side of an equality.
 * `expected` names what the caller was looking for, so that the message for
 * a token that can begin no literal reads in the caller's terms.  An atom's
 * arguments are read into `scratch`, a buffer of RELIC_MAX_ARITY terms that
 * the literal points into until it is kept.
 */
static bool
parse_literal(relic_parser_t *parser, relic_ast_literal_t *literal,
    const char *expected,
    relic_ast_term_t scratch[static RELIC_MAX_ARITY])
{
	enum relic_aggregate_fn fn;
	relic_ast_expr_t *left;
	relic_ast_expr_t *leaf;
	relic_ast_term_t term;
	relic_token_t name;
	enum relic_arith_op op;

	bzero(literal, sizeof(*literal));
	literal->where = parser->token.where;

	if (!token_begins_literal(&parser->token)) {
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
		if (!advance(parser))
			return (false);
		literal->kind = RELIC_LITERAL_NEGATED;

		return (parse_atom(parser, &literal->u.atom, scratch));
	}

	/*
	 * A symbol begins either an atom or a comparison whose left expression
	 * begins with it, and the token after it decides which: a comparator
	 * or an operator makes the symbol an operand, and a `(` leaves the
	 * question to the token after the `)`, parse_atom_or_call()'s.  This
	 * is the grammar's one ambiguity, and each decision waits only for a
	 * token of lookahead.
	 */
	if (parser->token.kind == RELIC_TOKEN_SYMBOL) {
		name = parser->token;
		if (!advance(parser))
			return (false);

		if (parser->token.kind == RELIC_TOKEN_LBRACE &&
		    token_function(&name, &fn))
			return (refuse_misplaced_aggregate(parser, &name));

		if (parser->token.kind == RELIC_TOKEN_LPAREN)
			return (parse_atom_or_call(parser, &name, literal,
			    scratch));

		if (parser->token.kind != RELIC_TOKEN_COMPARE &&
		    !token_operator(parser, &op)) {
			literal->kind = RELIC_LITERAL_POSITIVE;

			return (atom_after_name(parser, &name,
			    &literal->u.atom, scratch));
		}

		if (!term_from_token(parser, &name, &term))
			return (false);
		if (!make_leaf(parser, &term, &leaf))
			return (false);
		if (!continue_sum(parser, leaf, &left))
			return (false);

		return (parse_comparison(parser, literal, left));
	}

	if (!parse_expression(parser, &left))
		return (false);

	return (parse_comparison(parser, literal, left));
}

/*
 * Append a literal to the scratch array the current statement's body is
 * collected in, keeping its atom's arguments in the tree so that the buffer
 * they were read into can be reused for the next literal.
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
	relic_ast_literal_t kept;

	kept = *literal;
	if (!keep_literal(parser, &kept))
		return (false);

	body = relic_grow_array(parser->body, &parser->body_capacity,
	    parser->body_length + 1, sizeof(*parser->body));
	if (body == NULL) {
		relic_arena_fail(parser->arena);
		return (false);
	}
	parser->body = body;

	parser->body[parser->body_length] = kept;
	parser->body_length++;

	return (true);
}

/*
 * Collect a comma-separated body whose first literal has already been
 * parsed, into the body array.  `scratch` is the buffer the first literal's
 * arguments were read into; once that literal is appended the buffer is
 * free, and each later literal is read into it in turn.  So the caller's
 * `first` no longer describes its arguments when this returns: the kept copy
 * in the body does.
 */
static bool
collect_body(relic_parser_t *parser, const relic_ast_literal_t *first,
    relic_ast_term_t scratch[static RELIC_MAX_ARITY])
{
	relic_ast_literal_t literal;

	if (!body_append(parser, first))
		return (false);

	while (parser->token.kind == RELIC_TOKEN_COMMA) {
		if (!advance(parser))
			return (false);

		if (!parse_literal(parser, &literal, "a literal", scratch))
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
		if (!keep_atom(parser, &statement->head))
			return (false);
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

/* Whether every argument of an atom is a constant. */
static bool
is_ground(const relic_ast_atom_t *atom)
{
	uint16_t i;

	for (i = 0; i < atom->arity; i++) {
		if (atom->terms[i].kind != RELIC_TERM_CONSTANT)
			return (false);
	}

	return (true);
}

/*
 * Add a ground fact to the load's staging.  Its predicate is interned now,
 * while the text is parsed, as the lexer interns every symbol it reads, so
 * the predicate stays interned if the load later fails.  relic_pred_intern()
 * and relic_staging_add() each report their own refusal, so either refusing
 * stops the parse as a refusal already reported.
 */
static bool
stage_fact(relic_parser_t *parser, const relic_ast_atom_t *fact)
{
	relic_dom_t tuple[RELIC_MAX_ARITY];
	relic_pred_id_t pred;
	uint16_t i;

	for (i = 0; i < fact->arity; i++)
		tuple[i] = fact->terms[i].u.constant;

	if (relic_pred_intern(parser->ctx, fact->name, fact->arity,
	    &fact->where, &pred) != 0) {
		parser->failed = true;
		return (false);
	}

	if (relic_staging_add(parser->staging, pred, tuple) != 0) {
		parser->failed = true;
		return (false);
	}

	return (true);
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

/*
 * Entered on the `.`, which ends the statement.
 *
 * A fact a load reads whose arguments are all constants is a tuple, and is
 * staged rather than kept.  It is staged before the token after the `.` is
 * read, so a fact the limits refuse is the last thing the load reads: the
 * lexer interns nothing past it.  Any other fact is kept as what it means, a
 * rule with no body: one with a variable, for the check to reject as unsafe
 * with the message an unsafe rule receives, and one in a query's text, for
 * the query parser to refuse as a fact.
 */
static bool
finish_fact(relic_parser_t *parser, const relic_ast_atom_t *head,
    const relic_location_t *where)
{
	if (parser->staging != NULL && is_ground(head)) {
		if (!stage_fact(parser, head))
			return (false);

		return (advance(parser));
	}

	if (!advance(parser))
		return (false);

	return (append_statement(parser, head, NULL, 0, where));
}

/*
 * Entered on the `:-`, which separates the head from the body.  The head's
 * arguments are still in the caller's buffer until the statement is
 * appended, so the body is read into a buffer of its own.
 */
static bool
finish_rule(relic_parser_t *parser, const relic_ast_atom_t *head,
    const relic_location_t *where)
{
	relic_ast_term_t scratch[RELIC_MAX_ARITY];
	relic_ast_literal_t first;

	if (!advance(parser))
		return (false);

	if (!parse_literal(parser, &first, "a literal", scratch))
		return (false);
	if (!collect_body(parser, &first, scratch))
		return (false);
	if (!expect(parser, RELIC_TOKEN_DOT, "',' or '.'"))
		return (false);

	return (append_statement(parser, head, parser->body,
	    parser->body_length, where));
}

/*
 * Entered on the `,` before the next literal, or on the closing `?`.
 * `scratch` is the buffer the first literal was read into.
 */
static bool
finish_query(relic_parser_t *parser, const relic_ast_literal_t *first,
    relic_ast_term_t scratch[static RELIC_MAX_ARITY],
    const relic_location_t *where)
{
	if (!collect_body(parser, first, scratch))
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
 * query whose body has already begun.  So the first literal's arguments are
 * read into a buffer here, and each form keeps them in the tree or not.
 */
static bool
parse_statement(relic_parser_t *parser)
{
	relic_ast_term_t scratch[RELIC_MAX_ARITY];
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

	if (!parse_literal(parser, &first, "a fact, rule or query", scratch))
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
		return (finish_query(parser, &first, scratch, &where));
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

	while (parser->token.kind != RELIC_TOKEN_END) {
		bool terminator;

		terminator = parser->token.kind == RELIC_TOKEN_DOT ||
		    parser->token.kind == RELIC_TOKEN_QUESTION;
		if (!advance(parser))
			return;

		if (terminator)
			return;
	}
}

static int
parse_program(relic_parser_t *parser)
{
	if (!advance(parser))		/* prime the current token */
		return (-1);

	while (!parser->failed && parser->token.kind != RELIC_TOKEN_END) {
		if (!parse_statement(parser))
			synchronize(parser);
	}

	return (parser->failed ? -1 : 0);
}

static int
parser_init(relic_parser_t *parser, relic_t *ctx, relic_arena_t *arena,
    relic_staging_t *staging, const char *source_name, const char *text,
    size_t length)
{
	bzero(parser, sizeof(*parser));

	parser->ctx = ctx;
	parser->arena = arena;
	parser->staging = staging;
	TAILQ_INIT(&parser->statements);

	/*
	 * The two spellings the parser recognizes wherever a literal or a
	 * variable is read are interned once, so that each is matched against
	 * them by an integer comparison rather than by comparing bytes.
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
	relic_free_array(parser->body);
	relic_free_array(parser->group_body);
	bzero(parser, sizeof(*parser));
}

/*
 * The stable name a location may point at for the life of the context.
 *
 * Source names are interned like any other byte string, so the bytes live in
 * the symbol pool's string arena and never move: a location stored in the
 * tree stays printable after the load that produced it has returned, and
 * loading one file twice stores one copy of its name.
 */
const char *
relic_source_name(relic_t *ctx, const char *name)
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
 * parser's own list, and its ground facts into `staging` unless that is
 * NULL.  Returns 0 when the whole text parsed, or -1 with every mistake
 * already reported.
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
    relic_staging_t *staging, const char *source_name, const char *text,
    size_t length)
{
	const char *name;
	int status;

	bzero(parser, sizeof(*parser));

	name = relic_source_name(ctx, source_name);
	if (name == NULL)
		return (-1);

	if (parser_init(parser, ctx, arena, staging, name, text, length) != 0)
		return (-1);

	status = parse_program(parser);

	/*
	 * A refusal nobody reported is reported here, unless interning or
	 * staging has since reported one of its own and set `failed`: the load
	 * already fails with a message, and a second would tell the reader
	 * about the same exhaustion, or about one that no longer matters.
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
 * A load is all or nothing in what it adds to the program.  Its facts are
 * staged as they are parsed, and when the whole text has parsed they are
 * committed to their base stores; only when the commit succeeds are its
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
	relic_load_t load;
	int status;

	status = load_parse(&load, ctx, source_name, text, length);
	if (status == 0)
		status = load_commit(&load);

	load_reset(&load);

	return (status);
}

/*
 * Load text read from a stream, which the load owns and releases once it is
 * parsed, so that the commit, which grows the base stores, runs without it.
 * The parse produced identifiers, interned names and locations naming an
 * interned source, never pointers into the text; the lexer's state still
 * points there, and neither the commit nor the reset reads it.
 */
static int
load_read_text(relic_t *ctx, const char *source_name, char *text,
    size_t length)
{
	relic_load_t load;
	int status;

	status = load_parse(&load, ctx, source_name, text, length);
	relic_free_array(text);
	if (status == 0)
		status = load_commit(&load);

	load_reset(&load);

	return (status);
}

/*
 * Parse text into `load`: its statements onto the load's own list and its
 * facts into the load's staging, neither yet in the program.  Returns 0, or
 * -1 having reported every mistake; the caller resets `load` either way.
 */
static int
load_parse(relic_load_t *load, relic_t *ctx, const char *source_name,
    const char *text, size_t length)
{
	relic_staging_init(&load->staging, ctx);

	return (parse_text(&load->parser, ctx, &ctx->program_arena,
	    &load->staging, source_name, text, length));
}

/*
 * Commit a parsed load's facts to their base stores and, only when that
 * succeeds, splice its statements into the program.
 */
static int
load_commit(relic_load_t *load)
{
	relic_t *ctx;

	ctx = load->parser.ctx;
	if (relic_staging_commit(&load->staging) != 0)
		return (-1);

	TAILQ_CONCAT(&ctx->ast.statements, &load->parser.statements, link);
	ctx->ast.count += load->parser.statement_count;
	ctx->ast.query_count += load->parser.query_count;

	return (0);
}

static void
load_reset(relic_load_t *load)
{
	relic_staging_reset(&load->staging);
	parser_reset(&load->parser);
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
		relic_diag_error(parser->ctx, RELIC_ERROR_REJECTED,
		    &parser->token.where,
		    "expected a query, found end of input");
		return (NULL);
	}

	if (statement->kind != RELIC_STMT_QUERY) {
		relic_diag_error(parser->ctx, RELIC_ERROR_REJECTED,
		    &statement->where, "expected a query, found a %s",
		    statement->body_length == 0 ? "fact" : "rule");
		return (NULL);
	}

	second = TAILQ_NEXT(statement, link);
	if (second != NULL) {
		relic_diag_error(parser->ctx, RELIC_ERROR_REJECTED,
		    &second->where,
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

	/* No staging: a fact in a query's text stays a statement to refuse. */
	status = parse_text(&parser, ctx, arena, NULL, source_name, text,
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
 * A stream is read whole rather than incrementally because the lexer scans
 * one contiguous range, so that every token, however long, is a pointer and
 * a length into the text with no buffer to refill beneath it.  An incremental
 * reader would bound only the text, while the load's staged facts, held until
 * its all-or-nothing commit, grow with the input regardless.
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
		    length + RELIC_READ_CHUNK, sizeof(*text));
		if (grown == NULL) {
			relic_free_array(text);
			relic_diag_no_memory(ctx);
			return (-1);
		}
		text = grown;

		room = capacity - length;

		/*
		 * Cleared first, because a stream whose error indicator was
		 * already set can return short without touching errno, and
		 * the message would then name whatever an earlier call left
		 * behind.  A stream that fails without saying why, such as
		 * one whose read function sets no errno, is reported as an
		 * input error rather than as no error at all.
		 */
		errno = 0;
		got = fread(text + length, 1, room, stream);
		read_errno = errno;
		length += got;

		if (got == room)
			continue;

		if (ferror(stream)) {
			relic_diag_error(ctx, RELIC_ERROR_IO, NULL,
			    "cannot read %s: %s", source_name,
			    strerror(read_errno != 0 ? read_errno : EIO));
			relic_free_array(text);
			return (-1);
		}

		break;
	}

	*textp = text;
	*lengthp = length;

	return (0);
}

int
relic_load_string(relic_t *ctx, const char *source_name, const char *text)
{
	int status;

	if (relic_enter(ctx, "loading",
	    source_name != NULL ? source_name : RELIC_UNNAMED_SOURCE) != 0)
		return (-1);

	if (text != NULL) {
		status = load_text(ctx, source_name, text, strlen(text));
	} else {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
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

	if (relic_enter(ctx, "loading",
	    source_name != NULL ? source_name : RELIC_UNNAMED_SOURCE) != 0)
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

	if (stream == NULL) {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "no input stream was given");
		return (-1);
	}

	name = source_name != NULL ? source_name : RELIC_UNNAMED_SOURCE;

	if (read_stream(ctx, name, stream, &text, &length) != 0)
		return (-1);

	return (load_read_text(ctx, name, text, length));
}

int
relic_load_path(relic_t *ctx, const char *path)
{
	int status;

	if (relic_enter(ctx, "loading",
	    path != NULL ? path : RELIC_UNNAMED_SOURCE) != 0)
		return (-1);

	status = load_named_file(ctx, path);

	relic_leave(ctx);

	return (status);
}

/*
 * The descriptor is opened with O_CLOEXEC and wrapped, rather than opened by
 * fopen(3) with the "e" mode, which POSIX gained only in its 2024 edition.
 * The flag is set as the descriptor is made, so no fork(2) on another thread
 * can come between the open and the marking.
 */
FILE *
relic_open_input(relic_t *ctx, const char *path)
{
	FILE *stream;
	int descriptor;
	int saved_errno;

	descriptor = open(path, O_RDONLY | O_CLOEXEC | O_LARGEFILE);
	if (descriptor == -1) {
		relic_diag_error(ctx, RELIC_ERROR_IO, NULL,
		    "cannot open %s: %s", path, strerror(errno));
		return (NULL);
	}

	stream = fdopen(descriptor, "r");
	if (stream == NULL) {
		saved_errno = errno;
		close(descriptor);
		relic_diag_error(ctx, RELIC_ERROR_IO, NULL,
		    "cannot open %s: %s", path, strerror(saved_errno));
		return (NULL);
	}

	return (stream);
}

int
relic_close_input(relic_t *ctx, const char *path, FILE *stream)
{
	if (fclose(stream) != 0) {
		relic_diag_error(ctx, RELIC_ERROR_IO, NULL,
		    "cannot close %s: %s", path, strerror(errno));
		return (-1);
	}

	return (0);
}

/*
 * Open the file at `path`, read it whole and close it, then load what it
 * held, naming it by its path.  The file is closed before anything is
 * parsed, so a close that fails fails the load with nothing added.
 */
static int
load_named_file(relic_t *ctx, const char *path)
{
	FILE *stream;
	char *text;
	size_t length;

	if (path == NULL) {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "no path was given");
		return (-1);
	}

	stream = relic_open_input(ctx, path);
	if (stream == NULL)
		return (-1);

	if (read_stream(ctx, path, stream, &text, &length) != 0) {
		fclose(stream);
		return (-1);
	}

	if (relic_close_input(ctx, path, stream) != 0) {
		relic_free_array(text);
		return (-1);
	}

	return (load_read_text(ctx, path, text, length));
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
	char described[RELIC_PRED_TEXT_MAX];
	int status;

	if (relic_enter(ctx, "adding", describe_added_fact(predicate, arity,
	    described, sizeof(described))) != 0)
		return (-1);

	status = add_fact(ctx, predicate, arity, values);

	relic_leave(ctx);

	return (status);
}

/*
 * Write the predicate relic_fact_add() is asked to add to as "name/arity"
 * into `buffer`, the name cut as relic_pred_describe() cuts one, and return
 * the buffer.  A missing name, which add_fact() refuses before it interns
 * anything, is written "?".
 */
static const char *
describe_added_fact(const char *predicate, size_t arity, char *buffer,
    size_t size)
{
	char shortened[RELIC_NAME_TEXT_MAX];

	snprintf(buffer, size, "%s/%zu", predicate != NULL ?
	    relic_diag_name(predicate, shortened, sizeof(shortened)) : "?",
	    arity);

	return (buffer);
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
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "a fact needs a predicate name and a value per argument");
		return (-1);
	}

	if (!relic_lex_is_predicate_name(predicate, strlen(predicate))) {
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, NULL,
		    "a predicate name is a lowercase letter followed by "
		    "letters, digits and underscores, and is not `not`");
		return (-1);
	}

	if (arity > RELIC_MAX_ARITY) {
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, NULL,
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
