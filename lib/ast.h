/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_AST_H
#define RELIC_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <relic.h>

#include "queue.h"
#include "util.h"

/*
 * The parser's output.
 *
 * These types are a transcription of the language's grammar and deliberately
 * nothing more: no predicate table, no variable numbering, no reordering.
 * What the parser accepts is a syntactic question, and its answer should
 * still be recognizable as the text that was read.  Turning this into a
 * program -- resolving predicates, numbering variables, checking safety -- is
 * a separate pass over a finished tree.
 *
 * Every node is allocated from the program arena and lives as long as the
 * loaded program.  Nothing here is freed individually.
 */

/*
 * At most 32 arguments in an atom.
 *
 * The limit is structural rather than configurable: an index key is a 32-bit
 * column bitmask, so a 33rd column could not be named by one.  The parser
 * enforces it because that is where the argument is written, and because the
 * bound is what lets an atom's terms be collected in a fixed buffer with no
 * allocation and no failure path.
 */
#define	RELIC_MAX_ARITY	32

/*
 * How deeply an expression may nest, counted two ways, each a bound on a
 * stack.
 *
 * The descent of the text: the parentheses, unary minuses and calls the
 * parser is inside when it reads a token, a call counting once at its `(`.
 * That is what bounds the parser's own recursion, and it is counted in the
 * text rather than in the tree because `((((X))))` builds one node however
 * many parentheses it has.
 *
 * The height of the tree, a leaf counting one and every operator, negation
 * and call one more than its tallest operand or argument.  That is what
 * bounds every walk over a tree, and the stack an expression is evaluated
 * on.
 *
 * The limit is structural rather than configurable, as arity's is: a program
 * the parser accepts can then be walked and run with no stack check.  No
 * person writes an expression this deep, and one a program generates is
 * split across assignments, as a person would split it.
 */
#define	RELIC_MAX_EXPRESSION_DEPTH	32

typedef struct relic_ast_aggregate relic_ast_aggregate_t;
typedef struct relic_ast_atom relic_ast_atom_t;
typedef struct relic_ast_call relic_ast_call_t;
typedef struct relic_ast_comparison relic_ast_comparison_t;
typedef struct relic_ast_expr relic_ast_expr_t;
typedef struct relic_ast_literal relic_ast_literal_t;
typedef struct relic_ast_program relic_ast_program_t;
typedef struct relic_ast_stmt relic_ast_stmt_t;
typedef struct relic_ast_stmt_list relic_ast_stmt_list_t;
typedef struct relic_ast_term relic_ast_term_t;
typedef struct relic_ast_variable relic_ast_variable_t;

/*
 * The six built-in comparisons.  A comparison is a filter, except for an
 * equality one side of which is a variable nothing before it binds: that one
 * binds the variable to the other side's value, and is an assignment.
 */
enum relic_compare_op {
	RELIC_CMP_EQ,
	RELIC_CMP_NE,
	RELIC_CMP_LT,
	RELIC_CMP_LE,
	RELIC_CMP_GT,
	RELIC_CMP_GE
};

/*
 * The binary arithmetic operators, over 64-bit integers.  Negation has one
 * operand and is a kind of node of its own rather than an operator.
 */
enum relic_arith_op {
	RELIC_ARITH_ADD,
	RELIC_ARITH_SUBTRACT,
	RELIC_ARITH_MULTIPLY,
	RELIC_ARITH_DIVIDE,
	RELIC_ARITH_REMAINDER
};

/*
 * The string functions, each at one arity: `substr` is two functions of one
 * name, as p/2 and p/3 are two predicates.  lib/strfn.c holds what each
 * takes and returns, and every consumer of a call reads it there.
 */
enum relic_function {
	RELIC_FN_LENGTH,	/* string -> integer */
	RELIC_FN_CAT,		/* string, string -> string */
	RELIC_FN_SUBSTR2,	/* string, integer -> string */
	RELIC_FN_SUBSTR3,	/* string, integer, integer -> string */
	RELIC_FN_INDEX,		/* string, string -> integer */
	RELIC_FN_MATCH,		/* string, pattern -> integer */
	RELIC_FN_STR		/* any -> string */
};

/*
 * The most arguments a function takes.  A fixed arity is what bounds the
 * stack an expression is evaluated on, since a call holds a cell for each
 * earlier argument while a later one is evaluated.
 */
#define	RELIC_MAX_FUNCTION_ARITY	3

/*
 * The four aggregate functions.  COUNT folds a group into its number of
 * rows; SUM, MIN and MAX fold one column of it, the aggregated variable.
 */
enum relic_aggregate_fn {
	RELIC_AGG_COUNT,
	RELIC_AGG_SUM,
	RELIC_AGG_MIN,
	RELIC_AGG_MAX
};

enum relic_ast_term_kind {
	RELIC_TERM_CONSTANT,
	RELIC_TERM_VARIABLE
};

/*
 * A variable occurrence.
 *
 * `name` is the interned spelling, which diagnostics quote and which variable
 * numbering groups occurrences by.  `anonymous` marks a bare `_`, and every
 * occurrence of one denotes a *distinct* variable, so numbering must not
 * group them by name however identical those names are.  The distinction is
 * recorded once, here, rather than rediscovered by a string comparison
 * wherever variables are examined.
 *
 * A name beginning with `_` that is longer than one character, such as
 * `_Unused`, is an ordinary named variable.
 */
struct relic_ast_variable {
	uint32_t	name;
	bool		anonymous;
};

/*
 * A term: a constant or a variable.
 *
 * A constant is already interned, so it holds the same domain identifier that
 * a tuple column will hold and nothing downstream re-derives it.
 */
struct relic_ast_term {
	relic_location_t		 where;
	union {
		relic_dom_t		 constant;
		relic_ast_variable_t	 variable;
	}				 u;
	enum relic_ast_term_kind	 kind;	/* which arm of u is live */
};

/*
 * A predicate applied to arguments, as written.  `where` points at the
 * predicate name, which is what a diagnostic about the predicate should
 * underline.  `terms` holds `arity` of them and is NULL when the arity is
 * zero, which is an ordinary relation holding no columns rather than a
 * special case.
 */
struct relic_ast_atom {
	relic_ast_term_t	*terms;
	relic_location_t	 where;
	uint32_t		 name;
	uint16_t		 arity;
};

enum relic_ast_expr_kind {
	RELIC_AST_EXPR_TERM,		/* a constant or a variable */
	RELIC_AST_EXPR_NEGATE,		/* -operand */
	RELIC_AST_EXPR_BINARY,		/* left op right */
	RELIC_AST_EXPR_CALL		/* name(args) */
};

/*
 * A call as written: a name followed by arguments in parentheses, where an
 * operand may stand.
 *
 * The parser builds a call node for any name before it knows whether the
 * name is a function, because a call inside an atom's arguments makes the
 * atom an error whatever the name is, and the atom's error is the one to
 * report.  So `name` and `argc` are what was written, and `fn` is meaningful
 * only when `resolved` says the name and the argument count are a
 * function's; the parser refuses every expression holding a call that is
 * not, so every call a finished tree holds is resolved.  `args` holds the
 * first `argc` arguments, at most RELIC_MAX_FUNCTION_ARITY of them: an
 * argument past that makes the call no function's, and is parsed and
 * dropped.  `open` is the call's `(`.
 */
struct relic_ast_call {
	relic_ast_expr_t	*args[RELIC_MAX_FUNCTION_ARITY];
	size_t			 argc;
	relic_location_t	 open;
	uint32_t		 name;
	enum relic_function	 fn;
	bool			 resolved;
};

/*
 * An expression as written: a tree whose leaves are terms and whose inner
 * nodes are negations, binary operators and calls, shaped by precedence and
 * by the parentheses the writer gave it.  A term alone is an expression of
 * one node.
 *
 * `where` is the term's location for a leaf, the operator's for a negation
 * or a binary node, and the name's for a call, which is what a message about
 * the node points at.  `height` is one for a leaf and one more than the
 * tallest operand or argument otherwise, never more than
 * RELIC_MAX_EXPRESSION_DEPTH.
 */
struct relic_ast_expr {
	union {
		relic_ast_term_t	 term;		/* TERM */
		relic_ast_expr_t	*operand;	/* NEGATE */
		struct {
			relic_ast_expr_t	*left;
			relic_ast_expr_t	*right;
			enum relic_arith_op	 op;
		}			 binary;	/* BINARY */
		relic_ast_call_t	 call;		/* CALL */
	}				 u;
	relic_location_t		 where;
	enum relic_ast_expr_kind	 kind;	/* which arm of u is live */
	uint8_t				 height;
};

/*
 * A comparison of two expressions, each of which may be a single term.
 */
struct relic_ast_comparison {
	relic_ast_expr_t	*left;
	relic_ast_expr_t	*right;
	enum relic_compare_op	 op;
};

enum relic_ast_literal_kind {
	RELIC_LITERAL_POSITIVE,
	RELIC_LITERAL_NEGATED,
	RELIC_LITERAL_COMPARISON,
	RELIC_LITERAL_AGGREGATE
};

/*
 * An aggregate literal as written: `result = fn(column) { body }`, or
 * `result = count { body }`, whose function has no column.
 *
 * `result` and `column` are variable terms, `column` meaningful for every
 * function but COUNT.  `body` holds `body_length` literals, at least one,
 * none of them an aggregate: the parser refuses an aggregate inside another
 * before it enters the inner body.  `where` is the function name, which is
 * what a message about the aggregate points at.
 */
struct relic_ast_aggregate {
	relic_ast_literal_t	*body;
	size_t			 body_length;
	relic_ast_term_t	 result;
	relic_ast_term_t	 column;	/* not COUNT */
	relic_location_t	 where;
	enum relic_aggregate_fn	 fn;
};

/*
 * One conjunct of a body, or the head of a rule.  `u.atom` is meaningful for
 * a positive or negated literal, `u.comparison` for a comparison and
 * `u.aggregate` for an aggregate.
 */
struct relic_ast_literal {
	union {
		relic_ast_atom_t	 atom;
		relic_ast_comparison_t	 comparison;
		relic_ast_aggregate_t	 aggregate;
	}				 u;
	relic_location_t		 where;
	enum relic_ast_literal_kind	 kind;	/* which arm of u is live */
};

enum relic_ast_stmt_kind {
	RELIC_STMT_RULE,
	RELIC_STMT_QUERY
};

/*
 * A statement: a rule, or a query.
 *
 * A fact is a rule whose body has no literals.  There is no separate fact
 * kind and no `is_fact` flag, because either would be a second way to state
 * what `body_length` already states, and two representations of one fact
 * eventually disagree.  `edge(a, b).` is a rule with an empty body, which is
 * also exactly what it means; a load then moves each such ground statement
 * into its predicate's base store, and keeps the rest.
 *
 * `head` is meaningful for a rule; a query has only a body.  `where` points
 * at the beginning of the statement.
 */
struct relic_ast_stmt {
	TAILQ_ENTRY(relic_ast_stmt)	 link;
	relic_ast_literal_t		*body;
	size_t				 body_length;
	relic_ast_atom_t		 head;
	relic_location_t		 where;
	enum relic_ast_stmt_kind	 kind;
};

TAILQ_HEAD(relic_ast_stmt_list, relic_ast_stmt);

/*
 * Every statement loaded into the context, in the order it was read.
 *
 * A list rather than an array because the statements are arena allocated and
 * an arena cannot resize a block: a growable array would leave every
 * superseded generation of itself behind.  The terms of an atom and the
 * literals of a body are arrays, because their lengths are known by the time
 * they are copied into the arena.
 *
 * `query_count` is how many of the statements are queries, which is what a
 * caller enumerating the program's queries needs before anything has been
 * compiled.
 */
struct relic_ast_program {
	relic_ast_stmt_list_t	statements;
	size_t			count;
	size_t			query_count;
};

#endif /* RELIC_AST_H */
