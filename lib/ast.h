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

typedef struct relic_ast_atom relic_ast_atom_t;
typedef struct relic_ast_comparison relic_ast_comparison_t;
typedef struct relic_ast_literal relic_ast_literal_t;
typedef struct relic_ast_program relic_ast_program_t;
typedef struct relic_ast_stmt relic_ast_stmt_t;
typedef struct relic_ast_stmt_list relic_ast_stmt_list_t;
typedef struct relic_ast_term relic_ast_term_t;
typedef struct relic_ast_variable relic_ast_variable_t;

/*
 * The six built-in comparisons.  They are filters: a comparison neither
 * generates a value nor binds a variable.
 */
enum relic_compare_op {
	RELIC_CMP_EQ,
	RELIC_CMP_NE,
	RELIC_CMP_LT,
	RELIC_CMP_LE,
	RELIC_CMP_GT,
	RELIC_CMP_GE
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

struct relic_ast_comparison {
	relic_ast_term_t	left;
	relic_ast_term_t	right;
	enum relic_compare_op	op;
};

enum relic_ast_literal_kind {
	RELIC_LITERAL_POSITIVE,
	RELIC_LITERAL_NEGATED,
	RELIC_LITERAL_COMPARISON
};

/*
 * One conjunct of a body, or the head of a rule.  `u.atom` is meaningful for
 * a positive or negated literal and `u.comparison` for a comparison.
 */
struct relic_ast_literal {
	union {
		relic_ast_atom_t	 atom;
		relic_ast_comparison_t	 comparison;
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
 * also exactly what it means.
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
