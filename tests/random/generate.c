/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#include "random.h"

/*
 * The generator: a seed becomes a program that is valid by construction.
 *
 * Two properties are built in rather than checked afterwards.
 *
 * Stratification comes from levels.  Every predicate is given a level when
 * it is created, facts-only predicates level zero.  A rule for a predicate
 * at level h reads positively only predicates at levels up to h, and
 * negatively only those below h.  Following dependencies from a head into
 * its body, the level never rises, and across a negation it strictly falls,
 * so a cycle -- which returns to its starting level -- has no negation in
 * it.  Recursion and mutual recursion come from positive literals at the
 * head's own level.
 *
 * Safety comes from the order of the choices.  A body's positive relational
 * literals are drawn first, and the variables they mention are then the only
 * variables anything later may use: the head, the negated literals and the
 * comparisons.  That is the whole of the safety condition, so no unsafe rule
 * can be drawn.  `_` appears only in positive literals.  An assignment binds
 * a variable no positive literal mentions from what those bind, and is
 * drawn before anything else reads it, so the variables it binds join the
 * safe ones; the text places it anywhere in its body, since the library
 * binds by the closure and not by the written order.
 *
 * Arithmetic is defined and terminating by construction.  A program that
 * computes is drawn as one, and states no integer larger in magnitude than
 * RANDOM_INTEGER_SMALL.  An expression's constants lie within
 * RANDOM_NUMBER_MAX of zero, a divisor is a nonzero constant, and a
 * multiplication has a constant among its two operands, so an expression of
 * the shapes drawn here is at most nine times as large as its largest
 * operand, or a constant.
 *
 * Whether an equality assigns depends on what is bound where it runs, and
 * the Magic rewrite runs a rule's prefix with only the call's bindings, so
 * any equality holding an operator may compute a new value, filter or not.
 * Every literal holding an operator is therefore drawn only in a query or
 * in a rule whose positive literals all read levels below its head's, and
 * at most two in a body.  A computed value then never feeds its own
 * computation: bottom up, a value at level h is one stated or one computed
 * by at most two such literals from values below h; top down, the rewrite
 * carries a call's demand from a head to a body literal of a lower level,
 * and a lower level never calls a higher one.  A value passes through at
 * most six bodies, three levels up and three calls down, each multiplying
 * it by at most 81, so every value stays below 7 * 9^12, and there are
 * finitely many under every strategy.  Deleting a literal or a column, as
 * the shrinker does, keeps both properties.
 *
 * The random stream is splitmix64, which is defined by its arithmetic alone,
 * so a seed names the same program on every platform and C library.
 */

typedef struct random_stream random_stream_t;

/*
 * The generator's state: the random stream, and whether the program being
 * drawn computes, which decides the constants it may state.
 */
struct random_stream {
	uint64_t	state;
	bool		computes;
};

/*
 * The pool every constant is drawn from.  The first entries are the common
 * ones, which facts and rules draw most of their constants from so that joins
 * find matches; the rest are drawn rarely, to put mixed types in a column,
 * the extremes of a 64-bit integer, and bytes that need escaping in front of
 * every comparison and every output path.
 */
const random_constant_t random_constants[] = {
	{ CONSTANT_SYMBOL, "a" },
	{ CONSTANT_SYMBOL, "b" },
	{ CONSTANT_SYMBOL, "c" },
	{ CONSTANT_SYMBOL, "d" },
	{ CONSTANT_INTEGER, "0" },
	{ CONSTANT_INTEGER, "1" },
	{ CONSTANT_STRING, "\"a\"" },
	{ CONSTANT_SYMBOL, "e" },
	{ CONSTANT_INTEGER, "-7" },
	{ CONSTANT_STRING, "\"\"" },
	{ CONSTANT_STRING, "\"x y\"" },
	{ CONSTANT_INTEGER, "9223372036854775807" },
	{ CONSTANT_INTEGER, "-9223372036854775808" },
	{ CONSTANT_STRING, "\"\\t\\x01\\0\"" },
	{ CONSTANT_STRING, "\"\\xc3\\xa9\"" },
	{ CONSTANT_STRING, "\"q\\\"\\\\\"" },
	{ CONSTANT_SYMBOL, "z_Z9" },
};
const size_t random_constant_count =
    sizeof(random_constants) / sizeof(random_constants[0]);

#define	COMMON_CONSTANTS	7

/* What a body is drawn for, which decides the predicates it may read. */
typedef enum body_owner {
	BODY_OF_QUERY,
	BODY_OF_BASE_RULE,	/* a predicate's first rule */
	BODY_OF_RULE
} body_owner_t;

/* The levels a literal may read, relative to its head's. */
typedef enum predicate_range {
	READ_ANY,
	READ_BELOW,
	READ_SAME,
	READ_UP_TO
} predicate_range_t;

static void		 draw_assignment(random_stream_t *stream,
			    random_body_t *body, size_t *operator_literalsp,
			    uint64_t *bound_variables);
static bool		 body_may_compute(const random_program_t *program,
			    const random_body_t *body, body_owner_t owner,
			    uint8_t level);
static uint64_t		 draw(random_stream_t *stream);
static void		 draw_expression(random_stream_t *stream,
			    uint64_t bound_variables,
			    random_expression_t *expression);
static random_term_t	 draw_leaf(random_stream_t *stream,
			    uint64_t bound_variables);
static random_term_t	 draw_number(random_stream_t *stream, bool nonzero);
static void		 fit_operation(random_stream_t *stream,
			    random_term_t *left, random_operator_t operator,
			    random_term_t *right);
static void		 insert_literal(random_stream_t *stream,
			    random_body_t *body, const random_literal_t *literal);
static size_t		 draw_below(random_stream_t *stream, size_t bound);
static void		 draw_bound_terms(random_stream_t *stream,
			    uint64_t positive_variables, random_term_t *terms,
			    size_t count);
static bool		 draw_chance(random_stream_t *stream, size_t in);
static random_term_t	 draw_constant(random_stream_t *stream);
static size_t		 draw_assignment_count(random_stream_t *stream);
static size_t		 draw_filter_count(random_stream_t *stream);
static size_t		 draw_positive_count(random_stream_t *stream,
			    body_owner_t owner);
static random_term_t	 draw_positive_term(random_stream_t *stream,
			    size_t variable_count,
			    uint64_t *positive_variables);
static predicate_range_t draw_positive_range(random_stream_t *stream,
			    body_owner_t owner);
static void		 generate_body(random_stream_t *stream,
			    const random_program_t *program,
			    body_owner_t owner, uint8_t level,
			    random_body_t *body, uint64_t *positive_variables);
static void		 generate_fact(random_stream_t *stream,
			    random_program_t *program, uint8_t predicate);
static void		 generate_predicates(random_stream_t *stream,
			    random_program_t *program,
			    size_t *extensional_count);
static void		 generate_rule(random_stream_t *stream,
			    random_program_t *program, uint8_t predicate,
			    body_owner_t owner);
static uint8_t		 pick_predicate(random_stream_t *stream,
			    const random_program_t *program,
			    predicate_range_t range, uint8_t level);
static void		 shuffle_rules(random_stream_t *stream,
			    random_program_t *program);

void
random_generate(random_program_t *program, uint64_t seed)
{
	random_stream_t stream;
	size_t extensional_count, query_count;

	bzero(program, sizeof(*program));
	stream.state = seed;
	stream.computes = draw_chance(&stream, 2);

	generate_predicates(&stream, program, &extensional_count);

	/*
	 * Extensional predicates hold facts only.  Every other predicate has
	 * rules, and some have a few facts as well, which the Magic rewrite
	 * reads on demand rather than completely.
	 */
	for (size_t p = 0; p < program->predicate_count; p++) {
		size_t fact_count = 0, rule_count = 0;

		/*
		 * An extensional predicate is occasionally empty, which every
		 * rule reading it must survive, but mostly holds enough facts
		 * for joins over it to match.  A zero-arity one holds at most
		 * one tuple however many facts state it, so it gets few.
		 */
		if (p < extensional_count) {
			fact_count = 2 + draw_below(&stream, 12);
			if (program->predicates[p].arity == 0)
				fact_count = 1 + draw_below(&stream, 2);
			if (draw_chance(&stream, 10))
				fact_count = 0;
		} else {
			rule_count = 1 + draw_below(&stream, 3);
			if (draw_chance(&stream, 4))
				fact_count = 1 + draw_below(&stream, 3);
		}

		for (size_t i = 0; i < fact_count; i++)
			generate_fact(&stream, program, (uint8_t)p);
		for (size_t i = 0; i < rule_count; i++)
			generate_rule(&stream, program, (uint8_t)p,
			    i == 0 ? BODY_OF_BASE_RULE : BODY_OF_RULE);
	}

	/*
	 * The textual order of statements decides planner ties and which
	 * definitions come first, so it is drawn too.
	 */
	shuffle_rules(&stream, program);

	query_count = 1 + draw_below(&stream, RANDOM_MAX_QUERIES);
	for (size_t q = 0; q < query_count; q++) {
		uint64_t unused_variables;

		generate_body(&stream, program, BODY_OF_QUERY, 0,
		    &program->queries[q], &unused_variables);
		program->query_count++;
	}
}

/*
 * Choose how many predicates there are, their arities and their levels.  The
 * first extensional_count predicates are extensional, at level zero, and the
 * rest are derived, at levels one to three, so that every derived predicate
 * has a lower level for its base rule to read.
 */
static void
generate_predicates(random_stream_t *stream, random_program_t *program,
    size_t *extensional_count)
{
	/* Arities 1 and 2 dominate real programs; 0 and 4 are edge cases. */
	static const uint8_t arity_weights[RANDOM_MAX_ARITY + 1] = {
		1, 3, 4, 2, 1
	};
	size_t weight_total = 0;

	for (size_t a = 0; a <= RANDOM_MAX_ARITY; a++)
		weight_total += arity_weights[a];

	program->predicate_count = 2 +
	    draw_below(stream, RANDOM_MAX_PREDICATES - 1);
	*extensional_count = 1 +
	    draw_below(stream, program->predicate_count / 2);

	for (size_t p = 0; p < program->predicate_count; p++) {
		random_predicate_t *predicate = &program->predicates[p];
		size_t weight = draw_below(stream, weight_total);
		uint8_t arity = 0;

		while (weight >= arity_weights[arity]) {
			weight -= arity_weights[arity];
			arity++;
		}
		predicate->arity = arity;
		predicate->level = 0;
		if (p >= *extensional_count)
			predicate->level = (uint8_t)(1 + draw_below(stream, 3));
	}
}

static void
generate_fact(random_stream_t *stream, random_program_t *program,
    uint8_t predicate)
{
	random_rule_t *fact;

	if (program->rule_count == RANDOM_MAX_RULES)
		return;
	fact = &program->rules[program->rule_count];

	fact->head_predicate = predicate;
	for (size_t i = 0; i < program->predicates[predicate].arity; i++)
		fact->head[i] = draw_constant(stream);
	fact->body.length = 0;

	program->rule_count++;
}

static void
generate_rule(random_stream_t *stream, random_program_t *program,
    uint8_t predicate, body_owner_t owner)
{
	const random_predicate_t *head = &program->predicates[predicate];
	random_rule_t *rule;
	uint64_t bound_variables;

	if (program->rule_count == RANDOM_MAX_RULES)
		return;
	rule = &program->rules[program->rule_count];

	rule->head_predicate = predicate;
	generate_body(stream, program, owner, head->level, &rule->body,
	    &bound_variables);
	draw_bound_terms(stream, bound_variables, rule->head, head->arity);

	program->rule_count++;
}

/*
 * Draw a body: positive relational literals first, then assignments binding
 * fresh variables from what those bound, then negated literals and
 * comparisons over every variable bound.  The bound variables are returned
 * as a bit set, for the head to draw from.
 *
 * What a body may read depends on what it belongs to.  A query reads any
 * predicate, since nothing depends on a query.  A rule reads what its head's
 * level allows, and a predicate's base rule, its first, reads only levels
 * strictly below its head's, so that every derived predicate has a way to
 * hold tuples that does not go through itself.
 *
 * An occasional rule has no positive literal at all, only ground negations
 * and comparisons, or nothing, which makes it a fact; a query always has
 * one, since a query's body is its whole text.
 */
static void
generate_body(random_stream_t *stream, const random_program_t *program,
    body_owner_t owner, uint8_t level, random_body_t *body,
    uint64_t *bound_variables)
{
	size_t positive_count, negated_count, comparison_count;
	size_t assignment_count;
	size_t variable_count;
	size_t operator_literals;

	body->length = 0;
	*bound_variables = 0;

	/*
	 * The proportions keep most bodies satisfiable: few variables, so
	 * literals share them and joins connect.
	 */
	variable_count = 2 + draw_below(stream, RANDOM_MAX_VARIABLES - 1);
	positive_count = draw_positive_count(stream, owner);
	assignment_count = draw_assignment_count(stream);
	negated_count = draw_filter_count(stream);
	comparison_count = draw_filter_count(stream);

	for (size_t i = 0; i < positive_count; i++) {
		random_literal_t *literal = &body->literals[body->length++];
		predicate_range_t range = draw_positive_range(stream, owner);
		uint8_t arity;

		literal->kind = LITERAL_POSITIVE;
		literal->predicate = pick_predicate(stream, program, range,
		    level);
		arity = program->predicates[literal->predicate].arity;
		for (size_t a = 0; a < arity; a++)
			literal->args[a] = draw_positive_term(stream,
			    variable_count, bound_variables);
	}

	operator_literals = 0;
	if (stream->computes && body_may_compute(program, body, owner, level))
		operator_literals = RANDOM_MAX_OPERATOR_LITERALS;
	for (size_t i = 0; i < assignment_count; i++)
		draw_assignment(stream, body, &operator_literals,
		    bound_variables);

	for (size_t i = 0; i < negated_count; i++) {
		random_literal_t *literal = &body->literals[body->length++];
		predicate_range_t range =
		    owner == BODY_OF_QUERY ? READ_ANY : READ_BELOW;

		literal->kind = LITERAL_NEGATED;
		literal->predicate = pick_predicate(stream, program, range,
		    level);
		draw_bound_terms(stream, *bound_variables, literal->args,
		    program->predicates[literal->predicate].arity);
	}

	for (size_t i = 0; i < comparison_count; i++) {
		random_literal_t *literal = &body->literals[body->length++];
		size_t comparator = draw_below(stream, COMPARE_COUNT);
		random_term_t operands[2];

		literal->kind = LITERAL_COMPARISON;
		literal->comparator = (random_comparator_t)comparator;
		draw_bound_terms(stream, *bound_variables, operands, 2);
		for (size_t side = 0; side < 2; side++) {
			random_expression_t *expression = &literal->sides[side];

			bzero(expression, sizeof(*expression));
			expression->leaves[0] = operands[side];
			if (operator_literals > 0 && draw_chance(stream, 2)) {
				draw_expression(stream, *bound_variables,
				    expression);
				operator_literals--;
			}
		}
	}
}

/*
 * Whether a body may hold a literal with an operator: a query's may, and a
 * rule's may when its positive literals all read levels below its head's,
 * so that nothing it computes can reach it again, bottom up or through the
 * Magic rewrite's demand.
 */
static bool
body_may_compute(const random_program_t *program, const random_body_t *body,
    body_owner_t owner, uint8_t level)
{
	if (owner == BODY_OF_QUERY)
		return (true);

	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];

		if (literal->kind == LITERAL_POSITIVE &&
		    program->predicates[literal->predicate].level >= level)
			return (false);
	}
	return (true);
}

/*
 * Draw an assignment to a variable nothing in the body binds yet, from what
 * is bound, written either way round and placed anywhere in the body, and
 * add the variable to the bound ones.  Its value is mostly an expression
 * with operators while *operator_literalsp allows one more, which it then
 * uses, and otherwise a bound variable or a constant.  A body with no
 * variable left to bind gets none.
 */
static void
draw_assignment(random_stream_t *stream, random_body_t *body,
    size_t *operator_literalsp, uint64_t *bound_variables)
{
	random_literal_t literal;
	uint8_t free_variables[RANDOM_MAX_VARIABLES];
	size_t free_count = 0;
	size_t value_side;
	uint8_t target;

	for (uint8_t v = 0; v < RANDOM_MAX_VARIABLES; v++) {
		if ((*bound_variables & (UINT64_C(1) << v)) == 0)
			free_variables[free_count++] = v;
	}
	if (free_count == 0)
		return;
	target = free_variables[draw_below(stream, free_count)];

	bzero(&literal, sizeof(literal));
	literal.kind = LITERAL_COMPARISON;
	literal.comparator = COMPARE_EQ;
	value_side = draw_below(stream, 2);
	literal.sides[1 - value_side].leaves[0].kind = TERM_VARIABLE;
	literal.sides[1 - value_side].leaves[0].index = target;
	if (*operator_literalsp > 0 && !draw_chance(stream, 4)) {
		draw_expression(stream, *bound_variables,
		    &literal.sides[value_side]);
		(*operator_literalsp)--;
	} else
		literal.sides[value_side].leaves[0] = draw_leaf(stream,
		    *bound_variables);

	assert(body->length < RANDOM_MAX_BODY);
	insert_literal(stream, body, &literal);
	*bound_variables |= UINT64_C(1) << target;
}

/*
 * Draw an expression with one or two operators over bound variables and
 * numbers, fitted so that it is defined whatever integers its variables
 * hold: each operator's operands are fitted where it is drawn.  An operator
 * whose right operand is a group never divides by it, so a division or a
 * remainder there is made an addition.
 */
static void
draw_expression(random_stream_t *stream, uint64_t bound_variables,
    random_expression_t *expression)
{
	random_term_t *leaves = expression->leaves;
	random_operator_t *operators = expression->operators;

	bzero(expression, sizeof(*expression));
	expression->shape = (random_shape_t)(SHAPE_BINARY +
	    draw_below(stream, 3));
	expression->negated = draw_chance(stream, 8);
	for (size_t i = 0; i < 3; i++)
		leaves[i] = draw_leaf(stream, bound_variables);
	for (size_t i = 0; i < 2; i++) {
		size_t operator = draw_below(stream, OPERATOR_COUNT);

		operators[i] = (random_operator_t)operator;
	}

	switch (expression->shape) {
	case SHAPE_LEAF:
		break;
	case SHAPE_BINARY:
		fit_operation(stream, &leaves[0], operators[0], &leaves[1]);
		break;
	case SHAPE_LEFT:
		fit_operation(stream, &leaves[0], operators[0], &leaves[1]);
		fit_operation(stream, NULL, operators[1], &leaves[2]);
		break;
	case SHAPE_RIGHT:
		if (operators[0] == OPERATOR_DIVIDE ||
		    operators[0] == OPERATOR_MOD)
			operators[0] = OPERATOR_ADD;
		fit_operation(stream, &leaves[0], operators[0], NULL);
		fit_operation(stream, &leaves[1], operators[1], &leaves[2]);
		break;
	}
}

/*
 * Make one operation defined and small: a division or a remainder divides
 * by a nonzero number, and a multiplication has a number among its
 * operands.  An operand that is a group is NULL; a division's right operand
 * never is.
 */
static void
fit_operation(random_stream_t *stream, random_term_t *left,
    random_operator_t operator, random_term_t *right)
{
	switch (operator) {
	case OPERATOR_DIVIDE:
	case OPERATOR_MOD:
		*right = draw_number(stream, true);
		break;
	case OPERATOR_MULTIPLY:
		if ((left != NULL && left->kind == TERM_NUMBER) ||
		    (right != NULL && right->kind == TERM_NUMBER))
			break;
		if (right != NULL)
			*right = draw_number(stream, false);
		else
			*left = draw_number(stream, false);
		break;
	case OPERATOR_ADD:
	case OPERATOR_SUBTRACT:
	case OPERATOR_COUNT:
		break;
	}
}

/*
 * An operand: two times in three a bound variable, when there is one, and
 * otherwise a number.
 */
static random_term_t
draw_leaf(random_stream_t *stream, uint64_t bound_variables)
{
	uint8_t bound[RANDOM_MAX_VARIABLES];
	size_t bound_count = 0;
	random_term_t term;

	for (uint8_t v = 0; v < RANDOM_MAX_VARIABLES; v++) {
		if ((bound_variables & (UINT64_C(1) << v)) != 0)
			bound[bound_count++] = v;
	}
	if (bound_count == 0 || draw_chance(stream, 3))
		return (draw_number(stream, false));

	term.kind = TERM_VARIABLE;
	term.index = bound[draw_below(stream, bound_count)];
	return (term);
}

/*
 * A number within RANDOM_NUMBER_MAX of zero, and not zero when `nonzero`.
 */
static random_term_t
draw_number(random_stream_t *stream, bool nonzero)
{
	random_term_t term;
	size_t magnitude;

	term.kind = TERM_NUMBER;
	if (!nonzero) {
		term.index = (uint8_t)draw_below(stream,
		    2 * RANDOM_NUMBER_MAX + 1);
		return (term);
	}

	magnitude = 1 + draw_below(stream, RANDOM_NUMBER_MAX);
	term.index = (uint8_t)(draw_chance(stream, 2) ?
	    RANDOM_NUMBER_MAX + magnitude : RANDOM_NUMBER_MAX - magnitude);
	return (term);
}

/*
 * Insert a literal at a position drawn among the body's, moving the ones
 * after it along.
 */
static void
insert_literal(random_stream_t *stream, random_body_t *body,
    const random_literal_t *literal)
{
	size_t position = draw_below(stream, body->length + 1);

	for (size_t i = body->length; i > position; i--)
		body->literals[i] = body->literals[i - 1];
	body->literals[position] = *literal;
	body->length++;
}

/*
 * How many positive literals a body has: one to three, but a query of one
 * atom most often, and an occasional rule none, which leaves it ground
 * negations and comparisons or nothing, a fact.  A query always has one,
 * since a query's body is its whole text.
 */
static size_t
draw_positive_count(random_stream_t *stream, body_owner_t owner)
{
	size_t count = 1 + draw_below(stream, 3);
	bool usual = !draw_chance(stream, 3);
	bool rare = draw_chance(stream, 10);

	if (owner == BODY_OF_QUERY)
		return (usual ? 1 : count);
	return (rare ? 0 : count);
}

/*
 * How many negated literals, or how many comparisons, a body has: none
 * most often, else one or two, since each one filters what the body
 * derives.
 */
static size_t
draw_filter_count(random_stream_t *stream)
{
	bool present = draw_chance(stream, 4);
	size_t count = 1 + draw_below(stream, 2);

	return (present ? count : 0);
}

/*
 * How many assignments a body has: in a program that computes, one or two
 * half the time, since they are what it computes with, and otherwise as
 * many as a filter.
 */
static size_t
draw_assignment_count(random_stream_t *stream)
{
	if (stream->computes && draw_chance(stream, 2))
		return (1 + draw_below(stream, 2));
	return (draw_filter_count(stream));
}

/*
 * The levels a positive literal may read.  A query reads any predicate,
 * since nothing depends on a query.  A base rule reads strictly below its
 * head.  Any other rule reads its head's own level a third of the time,
 * which is where recursion comes from, and otherwise any level up to it.
 */
static predicate_range_t
draw_positive_range(random_stream_t *stream, body_owner_t owner)
{
	bool recursive = draw_chance(stream, 3);

	switch (owner) {
	case BODY_OF_QUERY:
		return (READ_ANY);
	case BODY_OF_BASE_RULE:
		return (READ_BELOW);
	case BODY_OF_RULE:
		break;
	}
	return (recursive ? READ_SAME : READ_UP_TO);
}

/*
 * An argument of a positive literal: nine times in ten one of the body's
 * variables, which the literal then binds, and otherwise a constant or `_`
 * alike.
 */
static random_term_t
draw_positive_term(random_stream_t *stream, size_t variable_count,
    uint64_t *positive_variables)
{
	random_term_t term;
	size_t choice = draw_below(stream, 20);

	if (choice < 18) {
		term.kind = TERM_VARIABLE;
		term.index = (uint8_t)draw_below(stream, variable_count);
		*positive_variables |= UINT64_C(1) << term.index;
		return (term);
	}
	if (choice < 19)
		return (draw_constant(stream));
	term.kind = TERM_ANONYMOUS;
	term.index = 0;
	return (term);
}

/*
 * A predicate whose level lies in the range asked for, relative to the
 * level of the head being derived.  One always qualifies: extensional
 * predicates sit at level zero and every head derived is above it, and a
 * query reads any level.
 */
static uint8_t
pick_predicate(random_stream_t *stream, const random_program_t *program,
    predicate_range_t range, uint8_t level)
{
	uint8_t candidates[RANDOM_MAX_PREDICATES];
	size_t candidate_count = 0;

	for (size_t p = 0; p < program->predicate_count; p++) {
		uint8_t candidate_level = program->predicates[p].level;
		bool allowed = false;

		switch (range) {
		case READ_ANY:
			allowed = true;
			break;
		case READ_BELOW:
			allowed = candidate_level < level;
			break;
		case READ_SAME:
			allowed = candidate_level == level;
			break;
		case READ_UP_TO:
			allowed = candidate_level <= level;
			break;
		}
		if (allowed)
			candidates[candidate_count++] = (uint8_t)p;
	}

	if (candidate_count == 0)
		errx(1, "no predicate for a literal at level %u", level);
	return (candidates[draw_below(stream, candidate_count)]);
}

/*
 * Draw count terms that are safe wherever they appear: variables some
 * positive literal mentions, or constants.  The variables are taken from a
 * shuffled list of the bound ones, so that terms drawn together are distinct
 * until the list runs out.  A head of distinct variables derives more
 * tuples than one repeating a variable, and a comparison of two distinct
 * variables filters where one of a variable with itself is decided.
 */
static void
draw_bound_terms(random_stream_t *stream, uint64_t positive_variables,
    random_term_t *terms, size_t count)
{
	uint8_t bound[RANDOM_MAX_VARIABLES];
	size_t bound_count = 0;

	for (uint8_t v = 0; v < RANDOM_MAX_VARIABLES; v++) {
		if ((positive_variables & (UINT64_C(1) << v)) != 0)
			bound[bound_count++] = v;
	}
	for (size_t i = bound_count; i > 1; i--) {
		size_t j = draw_below(stream, i);
		uint8_t swap = bound[i - 1];

		bound[i - 1] = bound[j];
		bound[j] = swap;
	}

	for (size_t i = 0; i < count; i++) {
		bool constant = draw_chance(stream, 5);

		if (bound_count == 0 || constant) {
			terms[i] = draw_constant(stream);
			continue;
		}
		terms[i].kind = TERM_VARIABLE;
		terms[i].index = bound[i % bound_count];
	}
}

/*
 * A constant from the pool, mostly a common one.  A program that computes
 * draws a number half the time, so that its expressions mostly find
 * integers to compute with, and draws again rather than state a large
 * integer.
 */
static random_term_t
draw_constant(random_stream_t *stream)
{
	random_term_t term;

	if (stream->computes && draw_chance(stream, 2))
		return (draw_number(stream, false));

	term.kind = TERM_CONSTANT;
	do {
		if (draw_chance(stream, 8))
			term.index = (uint8_t)draw_below(stream,
			    random_constant_count);
		else
			term.index = (uint8_t)draw_below(stream,
			    COMMON_CONSTANTS);
	} while (stream->computes && !random_constant_is_small(term.index));
	return (term);
}

static void
shuffle_rules(random_stream_t *stream, random_program_t *program)
{
	for (size_t i = program->rule_count; i > 1; i--) {
		size_t j = draw_below(stream, i);
		random_rule_t swap = program->rules[i - 1];

		program->rules[i - 1] = program->rules[j];
		program->rules[j] = swap;
	}
}

/* splitmix64: one addition and a finalizer per draw. */
static uint64_t
draw(random_stream_t *stream)
{
	uint64_t z;

	stream->state += UINT64_C(0x9e3779b97f4a7c15);
	z = stream->state;
	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	return (z ^ (z >> 31));
}

/*
 * A draw in [0, bound).  The modulo bias is below one part in 2^59 for every
 * bound used here, which a test generator can ignore.
 */
static size_t
draw_below(random_stream_t *stream, size_t bound)
{
	if (bound <= 1)
		return (0);
	return ((size_t)(draw(stream) % bound));
}

/* True one time in `in`. */
static bool
draw_chance(random_stream_t *stream, size_t in)
{
	return (draw_below(stream, in) == 0);
}
