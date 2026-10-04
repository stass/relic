/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_TEST_RANDOM_H
#define RELIC_TEST_RANDOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * The randomized differential suite.
 *
 * A seed generates a program that is safe and stratified by construction.
 * The program is run under every evaluation strategy the library offers, in
 * a child process, and every strategy must give the default's answers and,
 * where it evaluates the same rules, hold as many tuples.  The semi-naive
 * fixed point must also make every derivation exactly once, which no answer
 * shows, since a duplicate changes none.  A program that fails is shrunk to
 * a small one that fails the same way.
 *
 * The program model below is the generator's own rather than the library's
 * syntax tree, so that a mistake in the library's representation cannot be
 * repeated by the generator and hidden by the agreement of the two.
 */

/*
 * Sizes of the model.  They are far inside the library's structural limits
 * -- arity 32 and 64 variables per rule -- so no limit can refuse a generated
 * program, and small enough that every relation stays small and a program
 * runs in milliseconds under every strategy.  A body holds at most three
 * positive literals, two aggregates, and two each of assignments, negated
 * literals and comparisons, and two more comparisons that read what the
 * aggregates bind.
 */
#define	RANDOM_MAX_PREDICATES	8
#define	RANDOM_MAX_ARITY	4
#define	RANDOM_MAX_VARIABLES	6
#define	RANDOM_MAX_BODY		13
#define	RANDOM_MAX_RULES	96
#define	RANDOM_MAX_QUERIES	4

/*
 * Aggregates.  A body holds at most RANDOM_MAX_AGGREGATES of them, and the
 * body in each one's braces, its group body, at most two positive literals
 * and one each of assignments, negated literals and comparisons.  A rule's
 * own variables are the first RANDOM_MAX_VARIABLES; each aggregate of a body
 * draws its local variables from RANDOM_GROUP_VARIABLES of its own after
 * those, so that no variable is local to two aggregates, which would make
 * it shared between them.  A group body's columns, its shared and local
 * variables, number at most ten, far inside the 32 a predicate may have.
 */
#define	RANDOM_MAX_AGGREGATES	2
#define	RANDOM_MAX_GROUP_BODY	5
#define	RANDOM_GROUP_VARIABLES	4
#define	RANDOM_VARIABLE_COUNT	(RANDOM_MAX_VARIABLES + \
	RANDOM_MAX_AGGREGATES * RANDOM_GROUP_VARIABLES)

/*
 * The integers an expression may be given as constants are those from
 * -RANDOM_NUMBER_MAX to RANDOM_NUMBER_MAX, and a program with expressions
 * states no integer larger in magnitude than RANDOM_INTEGER_SMALL.  With
 * the expression shapes generate.c draws, that keeps every value any
 * generated program can compute inside a 64-bit integer.
 */
#define	RANDOM_NUMBER_MAX	3
#define	RANDOM_INTEGER_SMALL	7

/* The most literals holding an operator one body may have. */
#define	RANDOM_MAX_OPERATOR_LITERALS	2

typedef enum random_term_kind {
	TERM_VARIABLE,		/* index names the variable: A, B, ... */
	TERM_ANONYMOUS,		/* `_`, a distinct variable at every occurrence */
	TERM_CONSTANT,		/* index is a position in the constant pool */
	TERM_NUMBER,		/* index less RANDOM_NUMBER_MAX is an integer */
	TERM_PATTERN		/* index is a position in the pattern list */
} random_term_kind_t;

typedef struct random_term random_term_t;

struct random_term {
	random_term_kind_t	kind;
	uint8_t			index;
};

typedef enum random_literal_kind {
	LITERAL_POSITIVE,
	LITERAL_NEGATED,
	LITERAL_COMPARISON,
	LITERAL_AGGREGATE
} random_literal_kind_t;

/*
 * An aggregate's function.  count takes no variable; sum, min and max fold
 * the aggregated variable over the group.
 */
typedef enum random_function {
	FUNCTION_COUNT,
	FUNCTION_SUM,
	FUNCTION_MIN,
	FUNCTION_MAX,
	FUNCTION_KINDS		/* the number of functions */
} random_function_t;

typedef enum random_operator {
	OPERATOR_ADD,
	OPERATOR_SUBTRACT,
	OPERATOR_MULTIPLY,
	OPERATOR_DIVIDE,
	OPERATOR_MOD,
	OPERATOR_COUNT
} random_operator_t;

/*
 * A string function, one of each arity of `substr`.  random_calls[] holds
 * each one's spelling and what its arguments must be.
 */
typedef enum random_call {
	CALL_LENGTH,
	CALL_CAT,
	CALL_SUBSTR,
	CALL_SUBSTR_COUNT,
	CALL_INDEX,
	CALL_MATCH,
	CALL_STR,
	CALL_KINDS		/* the number of functions */
} random_call_t;

/*
 * A function's spelling and signature: one letter per argument, `s` a
 * string, `i` an integer, `p` a pattern and `v` a value of any type, and
 * whether its value is a string.
 */
typedef struct random_call_info random_call_info_t;

struct random_call_info {
	const char	*spelling;
	const char	*arguments;
	bool		 text;
};

extern const random_call_info_t	random_calls[CALL_KINDS];

/* The arguments a call holds at most, and so an expression's leaves. */
#define	RANDOM_MAX_CALL_ARGUMENTS	3

/*
 * An expression of one, two or three operands, the leaves, in one of six
 * shapes: a leaf alone, `l0 op0 l1`, `(l0 op0 l1) op1 l2` and `l0 op0 (l1
 * op1 l2)`, a call `c0(l0, ...)` and a call whose first argument is a call,
 * `c0(c1(l0, ...), ...)`, the inner call's arguments the first leaves and
 * the outer's others the rest.  Each is negated as a whole when `negated`.
 * A leaf is a variable, a number, a constant of the pool or, as a call's
 * pattern argument, a pattern; the text holds the parentheses the shape
 * needs and no others.
 */
typedef enum random_shape {
	SHAPE_LEAF,
	SHAPE_BINARY,
	SHAPE_LEFT,
	SHAPE_RIGHT,
	SHAPE_CALL,
	SHAPE_NESTED_CALL
} random_shape_t;

typedef struct random_expression random_expression_t;

struct random_expression {
	random_shape_t		shape;
	random_operator_t	operators[2];
	random_call_t		calls[2];	/* the outer call, the inner */
	random_term_t		leaves[RANDOM_MAX_CALL_ARGUMENTS];
	bool			negated;
};

typedef enum random_comparator {
	COMPARE_EQ,
	COMPARE_NE,
	COMPARE_LT,
	COMPARE_LE,
	COMPARE_GT,
	COMPARE_GE,
	COMPARE_COUNT
} random_comparator_t;

/*
 * A body literal.  A relational literal, positive or negated, reads its
 * predicate's arity from the program's predicate table and holds that many
 * arguments.  A comparison holds its two operands as expressions; an
 * equality one of whose sides is a variable alone that nothing else binds
 * is an assignment, and binds it.
 *
 * An aggregate, `result = function { group body }`, names its group body by
 * its index among the enclosing body's groups, which stays fixed however
 * literals move or are removed.  Its result is a variable of the enclosing
 * body, bound by the aggregate once every shared variable is, or already
 * bound, when the aggregate compares instead.  A group body holds no
 * aggregate.
 */
typedef struct random_literal random_literal_t;

struct random_literal {
	random_literal_kind_t	kind;
	random_comparator_t	comparator;	/* comparisons only */
	uint8_t			predicate;	/* relational literals only */
	random_term_t		args[RANDOM_MAX_ARITY];
	random_expression_t	sides[2];	/* comparisons only */
	random_function_t	function;	/* aggregates only */
	uint8_t			group;		/* aggregates only */
	uint8_t			result;		/* aggregates only */
	uint8_t			aggregated;	/* sum, min and max only */
};

/* The literals inside one aggregate's braces. */
typedef struct random_group random_group_t;

struct random_group {
	size_t			length;
	random_literal_t	literals[RANDOM_MAX_GROUP_BODY];
};

/*
 * A body is a rule's or a query's conjunction of literals, and the group
 * bodies of the aggregates among them.  A group no literal names, as one
 * whose aggregate the shrinker removed, is never read.
 */
typedef struct random_body random_body_t;

struct random_body {
	size_t			length;
	random_literal_t	literals[RANDOM_MAX_BODY];
	size_t			group_count;
	random_group_t		groups[RANDOM_MAX_AGGREGATES];
};

/* A rule; a fact is a rule whose body is empty and whose head is ground. */
typedef struct random_rule random_rule_t;

struct random_rule {
	uint8_t			head_predicate;
	random_term_t		head[RANDOM_MAX_ARITY];
	random_body_t		body;
};

/*
 * A predicate is named `p` and its index.  Its level is what makes every
 * generated program stratified: a rule for a predicate at level h reads
 * positively only predicates at levels up to h, and negatively only those
 * below h, so along any cycle of dependencies the level never falls and
 * therefore no cycle runs through a negation.
 */
typedef struct random_predicate random_predicate_t;

struct random_predicate {
	uint8_t		arity;
	uint8_t		level;
};

typedef struct random_program random_program_t;

struct random_program {
	size_t			predicate_count;
	random_predicate_t	predicates[RANDOM_MAX_PREDICATES];
	size_t			rule_count;
	random_rule_t		rules[RANDOM_MAX_RULES];
	size_t			query_count;
	random_body_t		queries[RANDOM_MAX_QUERIES];
};

/*
 * A constant: its type as the library will report it, and the text a
 * program spells it with.
 */
typedef enum random_constant_type {
	CONSTANT_SYMBOL,
	CONSTANT_STRING,
	CONSTANT_INTEGER
} random_constant_type_t;

typedef struct random_constant random_constant_t;

struct random_constant {
	random_constant_type_t	 type;
	const char		*spelling;
};

extern const random_constant_t	random_constants[];
extern const size_t		random_constant_count;

/*
 * The patterns a call of `match` is given, each valid in the dialect, as a
 * program spells it.
 */
extern const char *const	random_patterns[];
extern const size_t		random_pattern_count;

/* generate.c */
void	random_generate(random_program_t *program, uint64_t seed);

/* program.c */
bool	random_program_is_safe(const random_program_t *program);
bool	random_program_is_bounded(const random_program_t *program);
bool	random_constant_is_small(size_t index);
bool	random_program_has_positive_anonymous(
	    const random_program_t *program);
uint64_t random_literal_variables(const random_program_t *program,
	    const random_literal_t *literal);
char	*random_bindings_text(const random_program_t *program);
char	*random_helper_text(const random_program_t *program);
size_t	random_program_statements(const random_program_t *program);
char	*random_program_text(const random_program_t *program);
void	random_program_write(const random_program_t *program, FILE *out);

void	random_remove_query(random_program_t *program, size_t query);
void	random_remove_rule(random_program_t *program, size_t rule);
void	random_remove_literal(random_body_t *body, size_t literal);
void	random_remove_group_literal(random_group_t *group, size_t literal);
size_t	random_body_aggregates(const random_body_t *body);
void	random_remove_column(random_program_t *program, size_t predicate,
	    size_t column);

/*
 * The evaluation strategies every program is run under.  compare.c holds
 * what each one configures; STRATEGY_NONE stands for no strategy, as the
 * reference of one that has none and as the culprit of a failure that
 * happened outside every strategy's run.
 */
typedef enum random_strategy {
	STRATEGY_DEFAULT,
	STRATEGY_NAIVE,
	STRATEGY_NO_INDEX,
	STRATEGY_NO_STREAM,
	STRATEGY_PARALLEL,
	STRATEGY_NO_REORDER,
	STRATEGY_NO_MAGIC,
	STRATEGY_NAIVE_NO_MAGIC,
	STRATEGY_NO_INDEX_NO_MAGIC,
	STRATEGY_NO_REORDER_NO_MAGIC,
	STRATEGY_PARALLEL_NO_MAGIC,
	STRATEGY_MATERIALIZED,
	STRATEGY_COUNT,
	STRATEGY_NONE = STRATEGY_COUNT
} random_strategy_t;

/*
 * The kinds of failure a run can end in.  compare.c finds those up to
 * FAILURE_LAST_VERDICT; main.c, which runs the comparison in a child, finds
 * the rest from how the child ended.  The last compare.c finds is an
 * evaluation error, which a generated program is built never to meet.
 */
typedef enum random_failure_kind {
	FAILURE_NONE,
	FAILURE_REJECTED,	/* the library refused a generated program */
	FAILURE_QUERY,		/* a query failed with no limit configured */
	FAILURE_ANSWERS,	/* a strategy's answer differs from default */
	FAILURE_TOTALS,		/* a strategy's tuple total differs */
	FAILURE_DERIVATIONS,	/* a derivation was made twice, or missed */
	FAILURE_HELPER,		/* the helper rendering answers otherwise */
	FAILURE_EVALUATION,	/* an evaluation met an undefined result */
	FAILURE_LAST_VERDICT = FAILURE_EVALUATION, /* the last of compare.c's */
	FAILURE_CRASH,		/* the child was killed by a signal */
	FAILURE_HANG,		/* the child ran past its alarm */
	FAILURE_ABNORMAL	/* the child exited without its verdict */
} random_failure_kind_t;

/*
 * A failure: its kind, the strategy that failed or was running when the
 * child died, and for a crash the signal and for an abnormal exit the exit
 * status, so that two crashes of different causes are different failures.
 */
typedef struct random_failure random_failure_t;

struct random_failure {
	random_failure_kind_t	kind;
	random_strategy_t	strategy;
	int			code;		/* signal or exit status */
};

/*
 * A fault the self-test injects: one row dropped from every answer that has
 * one, of the naive strategy or of the helper rendering, so that it can show
 * that each comparison finds a disagreement and reports it as its own.
 */
typedef enum random_fault {
	FAULT_NONE,
	FAULT_NAIVE,
	FAULT_HELPER
} random_fault_t;

typedef struct random_options random_options_t;

struct random_options {
	random_fault_t	fault;		/* FAULT_NONE but in the self-test */
	unsigned int	timeout_seconds;
};

/* Called as each strategy's run begins, and with STRATEGY_NONE after all. */
typedef void	random_announce_fn(void *announce_ctx,
		    random_strategy_t strategy);

/* compare.c */
const char	*random_strategy_name(random_strategy_t strategy);
random_failure_t random_compare(const random_program_t *program,
		    const random_options_t *options, FILE *report,
		    random_announce_fn *announce, void *announce_ctx);

/* main.c */
random_failure_t random_run_isolated(const random_program_t *program,
		    const random_options_t *options, bool show_report);
bool		 random_failure_equal(random_failure_t a, random_failure_t b);
const char	*random_failure_name(random_failure_kind_t kind);

/* shrink.c */
void	random_shrink(random_program_t *program, random_failure_t failure,
	    const random_options_t *options);

#endif /* RELIC_TEST_RANDOM_H */
