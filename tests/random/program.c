/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "random.h"

/*
 * The program model's operations: printing it as relic source text,
 * checking the safety condition the generator builds in and the shrinker's
 * edits can break, and the edits themselves.
 */

static uint64_t	 assigned_variables(const random_literal_t *literals,
		    size_t length, uint64_t bound_variables);
static uint64_t	 bound_by_closure(const random_body_t *body,
		    const uint64_t *shared, uint64_t bound_variables);
static bool	 body_computes(const random_body_t *body);
static bool	 body_is_bounded(const random_program_t *program,
		    const random_body_t *body, bool query, uint8_t level);
static bool	 body_is_safe(const random_program_t *program,
		    const random_body_t *body, uint64_t head_variables,
		    uint64_t *bound_variables);
static bool	 body_states_small(const random_program_t *program,
		    const random_body_t *body);
static size_t	 count_operator_literals(const random_literal_t *literals,
		    size_t length);
static bool	 group_is_safe(const random_program_t *program,
		    const random_group_t *group,
		    const random_literal_t *aggregate);
static bool	 group_reads_facts(const random_program_t *program,
		    const random_group_t *group);
static uint64_t	 group_variables(const random_program_t *program,
		    const random_group_t *group,
		    const random_literal_t *aggregate);
static bool	 literal_has_operator(const random_literal_t *literal);
static bool	 literals_have_anonymous(const random_program_t *program,
		    const random_literal_t *literals, size_t length);
static bool	 literals_state_small(const random_program_t *program,
		    const random_literal_t *literals, size_t length);
static uint64_t	 positive_variables(const random_program_t *program,
		    const random_literal_t *literals, size_t length);
static bool	 program_computes(const random_program_t *program);
static void	 shared_variables(const random_program_t *program,
		    const random_body_t *body, uint64_t head_variables,
		    uint64_t *shared);
static bool	 terms_are_small(const random_term_t *terms, size_t count);
static uint64_t	 terms_variables(const random_term_t *terms,
		    size_t count);
static bool	 expression_is_variable(const random_expression_t *expression,
		    uint8_t *variablep);
static size_t	 expression_leaves(const random_expression_t *expression);
static uint64_t	 expression_variables(
		    const random_expression_t *expression);
static size_t	 literal_arity(const random_program_t *program,
		    const random_literal_t *literal);
static int	 operator_precedence(random_operator_t operator);
static size_t	 call_arity(random_call_t call);
static void	 remove_argument(random_term_t *args, size_t column);
static void	 remove_column_in_body(random_body_t *body, size_t predicate,
		    size_t column);
static void	 remove_column_in_literals(random_literal_t *literals,
		    size_t length, size_t predicate, size_t column);
static bool	 term_is_bound(random_term_t term, uint64_t positive_variables);
static bool	 terms_are_bound(const random_term_t *terms, size_t count,
		    uint64_t bound_variables);
static bool	 test_is_safe(const random_program_t *program,
		    const random_literal_t *literal, uint64_t bound_variables);
static uint64_t	 variable_bit(uint8_t variable);
static void	 write_aggregate(FILE *out, const random_program_t *program,
		    const random_body_t *body,
		    const random_literal_t *aggregate);
static void	 write_atom(FILE *out, const random_program_t *program,
		    uint8_t predicate, const random_term_t *args);
static void	 write_call(FILE *out, random_call_t call,
		    const random_term_t *leaves, bool nested,
		    random_call_t inner);
static void	 write_body(FILE *out, const random_program_t *program,
		    const random_body_t *body);
static void	 write_expression(FILE *out,
		    const random_expression_t *expression);
static void	 write_literals(FILE *out, const random_program_t *program,
		    const random_body_t *body, const random_literal_t *literals,
		    size_t length);
static void	 write_operation(FILE *out, random_term_t left,
		    random_operator_t operator, random_term_t right);
static void	 write_rule(FILE *out, const random_program_t *program,
		    const random_rule_t *rule);
static void	 write_term(FILE *out, random_term_t term);

static const char *const operator_spellings[OPERATOR_COUNT] = {
	[OPERATOR_ADD] = "+",
	[OPERATOR_SUBTRACT] = "-",
	[OPERATOR_MULTIPLY] = "*",
	[OPERATOR_DIVIDE] = "/",
	[OPERATOR_MOD] = "mod",
};

static const char *const comparator_spellings[COMPARE_COUNT] = {
	[COMPARE_EQ] = "=",
	[COMPARE_NE] = "!=",
	[COMPARE_LT] = "<",
	[COMPARE_LE] = "<=",
	[COMPARE_GT] = ">",
	[COMPARE_GE] = ">=",
};

const random_call_info_t random_calls[CALL_KINDS] = {
	[CALL_LENGTH] = { "length", "s", false },
	[CALL_CAT] = { "cat", "ss", true },
	[CALL_SUBSTR] = { "substr", "si", true },
	[CALL_SUBSTR_COUNT] = { "substr", "sii", true },
	[CALL_INDEX] = { "index", "ss", false },
	[CALL_MATCH] = { "match", "sp", false },
	[CALL_STR] = { "str", "v", true },
};

static const char *const function_spellings[FUNCTION_KINDS] = {
	[FUNCTION_COUNT] = "count",
	[FUNCTION_SUM] = "sum",
	[FUNCTION_MIN] = "min",
	[FUNCTION_MAX] = "max",
};

/*
 * Whether every rule and query satisfies the safety condition: every
 * variable of a head, a negated literal or a comparison is bound, by a
 * positive relational literal of the same body, by an assignment or by an
 * aggregate, and `_` occurs nowhere else.  An aggregate's shared variables
 * and its result must be bound as well, and its group body must be safe as
 * a rule of its own.  A query must also have a body, since its body is all
 * it is.
 */
bool
random_program_is_safe(const random_program_t *program)
{
	for (size_t r = 0; r < program->rule_count; r++) {
		const random_rule_t *rule = &program->rules[r];
		uint8_t arity = program->predicates[rule->head_predicate].arity;
		uint64_t bound_variables;

		if (!body_is_safe(program, &rule->body,
		    terms_variables(rule->head, arity), &bound_variables))
			return (false);
		for (size_t a = 0; a < arity; a++) {
			if (!term_is_bound(rule->head[a], bound_variables))
				return (false);
		}
	}

	for (size_t q = 0; q < program->query_count; q++) {
		uint64_t bound_variables;

		if (program->queries[q].length == 0)
			return (false);
		if (!body_is_safe(program, &program->queries[q], 0,
		    &bound_variables))
			return (false);
	}

	return (true);
}

/*
 * Whether the program's arithmetic is bounded as the generator builds it: a
 * literal holding an operator stands only in a query, in a rule whose
 * positive literals all read levels below its head's, or inside an
 * aggregate's braces, at most RANDOM_MAX_OPERATOR_LITERALS of them in a
 * statement's body and group bodies together; a sum's group body reads
 * facts alone; and a program holding an operator or a sum states no integer
 * beyond RANDOM_INTEGER_SMALL in magnitude.  A call counts as an operator
 * throughout, since it too computes a value the program does not state.
 * generate.c explains why such a program is defined and finite under every
 * strategy.
 */
bool
random_program_is_bounded(const random_program_t *program)
{
	bool computes = program_computes(program);

	for (size_t r = 0; r < program->rule_count; r++) {
		const random_rule_t *rule = &program->rules[r];
		const random_predicate_t *head =
		    &program->predicates[rule->head_predicate];

		if (!body_is_bounded(program, &rule->body, false, head->level))
			return (false);
		if (computes && (!terms_are_small(rule->head, head->arity) ||
		    !body_states_small(program, &rule->body)))
			return (false);
	}

	for (size_t q = 0; q < program->query_count; q++) {
		const random_body_t *query = &program->queries[q];

		if (!body_is_bounded(program, query, true, 0))
			return (false);
		if (computes && !body_states_small(program, query))
			return (false);
	}

	return (true);
}

/*
 * Whether a body and its group bodies hold no more literals with an
 * operator than a statement may; whether the body's own hold none unless it
 * is a query's or its positive literals all read below `level`, its
 * head's; and whether every sum's group body reads facts alone.  A group
 * body reads below its head's level by construction, so the operators in
 * it are allowed wherever the aggregate is.
 */
static bool
body_is_bounded(const random_program_t *program, const random_body_t *body,
    bool query, uint8_t level)
{
	size_t body_operators, operator_literals;
	bool reads_below = true;

	body_operators = count_operator_literals(body->literals, body->length);
	operator_literals = body_operators;

	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];
		const random_group_t *group;

		if (literal->kind == LITERAL_POSITIVE &&
		    program->predicates[literal->predicate].level >= level)
			reads_below = false;
		if (literal->kind != LITERAL_AGGREGATE)
			continue;

		group = &body->groups[literal->group];
		operator_literals += count_operator_literals(group->literals,
		    group->length);
		if (literal->function == FUNCTION_SUM &&
		    !group_reads_facts(program, group))
			return (false);
	}

	if (operator_literals > RANDOM_MAX_OPERATOR_LITERALS)
		return (false);
	return (body_operators == 0 || query || reads_below);
}

/* How many of the literals are comparisons holding an operator. */
static size_t
count_operator_literals(const random_literal_t *literals, size_t length)
{
	size_t count = 0;

	for (size_t i = 0; i < length; i++) {
		if (literal_has_operator(&literals[i]))
			count++;
	}
	return (count);
}

/*
 * Whether every positive literal of a group body reads a level-zero
 * predicate, one that holds facts alone.
 */
static bool
group_reads_facts(const random_program_t *program, const random_group_t *group)
{
	for (size_t i = 0; i < group->length; i++) {
		const random_literal_t *literal = &group->literals[i];

		if (literal->kind == LITERAL_POSITIVE &&
		    program->predicates[literal->predicate].level != 0)
			return (false);
	}
	return (true);
}

/*
 * Whether the program computes integers it does not state: whether any
 * literal, in a body or in a group body, holds an operator, or any
 * aggregate sums.  A count computes an integer too, but one no larger than
 * a relation, whatever integers the program states.
 */
static bool
program_computes(const random_program_t *program)
{
	for (size_t r = 0; r < program->rule_count; r++) {
		if (body_computes(&program->rules[r].body))
			return (true);
	}
	for (size_t q = 0; q < program->query_count; q++) {
		if (body_computes(&program->queries[q]))
			return (true);
	}
	return (false);
}

static bool
body_computes(const random_body_t *body)
{
	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];
		const random_group_t *group;

		if (literal_has_operator(literal))
			return (true);
		if (literal->kind != LITERAL_AGGREGATE)
			continue;

		group = &body->groups[literal->group];
		if (literal->function == FUNCTION_SUM ||
		    count_operator_literals(group->literals, group->length) > 0)
			return (true);
	}
	return (false);
}

/*
 * Whether a literal is a comparison one of whose sides has an operator or a
 * call, either of which computes a value the program does not state.
 */
static bool
literal_has_operator(const random_literal_t *literal)
{
	if (literal->kind != LITERAL_COMPARISON)
		return (false);

	for (size_t side = 0; side < 2; side++) {
		const random_expression_t *expression = &literal->sides[side];

		if (expression->shape != SHAPE_LEAF || expression->negated)
			return (true);
	}
	return (false);
}

/* Whether every constant a body and its group bodies state is small. */
static bool
body_states_small(const random_program_t *program, const random_body_t *body)
{
	if (!literals_state_small(program, body->literals, body->length))
		return (false);

	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];
		const random_group_t *group;

		if (literal->kind != LITERAL_AGGREGATE)
			continue;
		group = &body->groups[literal->group];
		if (!literals_state_small(program, group->literals,
		    group->length))
			return (false);
	}
	return (true);
}

static bool
literals_state_small(const random_program_t *program,
    const random_literal_t *literals, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		const random_literal_t *literal = &literals[i];

		if (literal->kind != LITERAL_COMPARISON) {
			if (!terms_are_small(literal->args,
			    literal_arity(program, literal)))
				return (false);
			continue;
		}
		for (size_t side = 0; side < 2; side++) {
			const random_expression_t *expression =
			    &literal->sides[side];

			if (!terms_are_small(expression->leaves,
			    expression_leaves(expression)))
				return (false);
		}
	}
	return (true);
}

/* Whether no term among `terms` is a large integer of the pool. */
static bool
terms_are_small(const random_term_t *terms, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (terms[i].kind == TERM_CONSTANT &&
		    !random_constant_is_small(terms[i].index))
			return (false);
	}
	return (true);
}

/*
 * Whether a pool constant is no integer beyond RANDOM_INTEGER_SMALL in
 * magnitude.
 */
bool
random_constant_is_small(size_t index)
{
	long long value;

	if (random_constants[index].type != CONSTANT_INTEGER)
		return (true);
	value = strtoll(random_constants[index].spelling, NULL, 10);
	return (value >= -RANDOM_INTEGER_SMALL &&
	    value <= RANDOM_INTEGER_SMALL);
}

/*
 * Whether a body is safe, given the variables its head mentions.  The body
 * binds the variables its positive literals mention, and then, by the
 * closure of bound_by_closure(), those its assignments and aggregates bind
 * from them.  Every negated literal and comparison must use only bound
 * variables, and every aggregate needs its shared variables and its result
 * bound and its group body safe.  The bound variables are returned as a
 * bit set, for the caller to check a head against.
 */
static bool
body_is_safe(const random_program_t *program, const random_body_t *body,
    uint64_t head_variables, uint64_t *bound_variables)
{
	uint64_t shared[RANDOM_MAX_AGGREGATES];

	shared_variables(program, body, head_variables, shared);
	*bound_variables = bound_by_closure(body, shared,
	    positive_variables(program, body->literals, body->length));

	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];
		uint64_t required;

		switch (literal->kind) {
		case LITERAL_POSITIVE:
			break;
		case LITERAL_NEGATED:
		case LITERAL_COMPARISON:
			if (!test_is_safe(program, literal, *bound_variables))
				return (false);
			break;
		case LITERAL_AGGREGATE:
			if (!group_is_safe(program,
			    &body->groups[literal->group], literal))
				return (false);
			required = shared[literal->group] |
			    variable_bit(literal->result);
			if ((required & ~*bound_variables) != 0)
				return (false);
			break;
		}
	}

	return (true);
}

/*
 * Each aggregate's shared variables, indexed by its group: those of its
 * group body that occur anywhere else in the statement -- in the head, in
 * a literal outside every aggregate, as an aggregate's result, or in
 * another group body.  The rest of a group body's variables are local to
 * it.  This is the library's rule, stated by occurrence, so a variable two
 * group bodies mention is shared by both.
 */
static void
shared_variables(const random_program_t *program, const random_body_t *body,
    uint64_t head_variables, uint64_t *shared)
{
	uint64_t outside = head_variables;
	uint64_t inside[RANDOM_MAX_AGGREGATES] = { 0 };

	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];

		outside |= random_literal_variables(program, literal);
		if (literal->kind == LITERAL_AGGREGATE)
			inside[literal->group] = group_variables(program,
			    &body->groups[literal->group], literal);
	}

	for (size_t g = 0; g < RANDOM_MAX_AGGREGATES; g++) {
		uint64_t elsewhere = outside;

		for (size_t other = 0; other < RANDOM_MAX_AGGREGATES; other++) {
			if (other != g)
				elsewhere |= inside[other];
		}
		shared[g] = inside[g] & elsewhere;
	}
}

/*
 * The variables a body binds, starting from `bound_variables`, those its
 * positive literals mention: an assignment binds its variable once the
 * other side's are bound, and an aggregate binds its result once its
 * shared variables are.  Either may read what the other binds, in whatever
 * order they are written, so both rules are applied until they bind
 * nothing more.
 */
static uint64_t
bound_by_closure(const random_body_t *body, const uint64_t *shared,
    uint64_t bound_variables)
{
	uint64_t before;

	do {
		before = bound_variables;
		bound_variables = assigned_variables(body->literals,
		    body->length, bound_variables);
		for (size_t i = 0; i < body->length; i++) {
			const random_literal_t *literal = &body->literals[i];

			if (literal->kind == LITERAL_AGGREGATE &&
			    (shared[literal->group] & ~bound_variables) == 0)
				bound_variables |=
				    variable_bit(literal->result);
		}
	} while (bound_variables != before);

	return (bound_variables);
}

/*
 * Whether an aggregate's group body is safe as the rule the library
 * compiles it to, whose head names every variable of the group body, the
 * aggregated one among them: the body is not empty, holds no aggregate, and
 * binds each of those variables by a positive literal or an assignment
 * inside the braces, and `_` stands only in its positive literals.
 */
static bool
group_is_safe(const random_program_t *program, const random_group_t *group,
    const random_literal_t *aggregate)
{
	uint64_t bound_variables;

	if (group->length == 0)
		return (false);

	bound_variables = assigned_variables(group->literals, group->length,
	    positive_variables(program, group->literals, group->length));
	if ((group_variables(program, group, aggregate) &
	    ~bound_variables) != 0)
		return (false);

	for (size_t i = 0; i < group->length; i++) {
		const random_literal_t *literal = &group->literals[i];

		if (literal->kind == LITERAL_AGGREGATE)
			return (false);
		if (literal->kind != LITERAL_POSITIVE &&
		    !test_is_safe(program, literal, bound_variables))
			return (false);
	}
	return (true);
}

/*
 * The variables of a group body: those its literals mention, and the
 * aggregated variable, which is an occurrence inside the braces.
 */
static uint64_t
group_variables(const random_program_t *program, const random_group_t *group,
    const random_literal_t *aggregate)
{
	uint64_t variables = 0;

	for (size_t i = 0; i < group->length; i++)
		variables |= random_literal_variables(program,
		    &group->literals[i]);
	if (aggregate->function != FUNCTION_COUNT)
		variables |= variable_bit(aggregate->aggregated);
	return (variables);
}

/*
 * The variables a literal mentions where it stands.  An aggregate's result
 * stands in the enclosing body; its group body's variables do not.
 */
uint64_t
random_literal_variables(const random_program_t *program,
    const random_literal_t *literal)
{
	switch (literal->kind) {
	case LITERAL_POSITIVE:
	case LITERAL_NEGATED:
		return (terms_variables(literal->args,
		    literal_arity(program, literal)));
	case LITERAL_COMPARISON:
		return (expression_variables(&literal->sides[0]) |
		    expression_variables(&literal->sides[1]));
	case LITERAL_AGGREGATE:
		return (variable_bit(literal->result));
	}
	return (0);
}

/* The variables the positive literals among `literals` mention. */
static uint64_t
positive_variables(const random_program_t *program,
    const random_literal_t *literals, size_t length)
{
	uint64_t variables = 0;

	for (size_t i = 0; i < length; i++) {
		if (literals[i].kind == LITERAL_POSITIVE)
			variables |= random_literal_variables(program,
			    &literals[i]);
	}
	return (variables);
}

/* The variables among `terms`, as a bit set. */
static uint64_t
terms_variables(const random_term_t *terms, size_t count)
{
	uint64_t variables = 0;

	for (size_t i = 0; i < count; i++) {
		if (terms[i].kind == TERM_VARIABLE)
			variables |= variable_bit(terms[i].index);
	}
	return (variables);
}

static uint64_t
variable_bit(uint8_t variable)
{
	return (UINT64_C(1) << variable);
}

/*
 * The variables bound once the assignments among `literals` have run,
 * starting from `bound_variables`: an equality one of whose sides is a
 * variable alone, not yet bound, and whose other side reads only bound
 * variables binds it.  An assignment may read what another binds, in
 * whatever order they are written, so the rule is applied until it binds
 * nothing more.
 */
static uint64_t
assigned_variables(const random_literal_t *literals, size_t length,
    uint64_t bound_variables)
{
	bool bound_more;

	do {
		bound_more = false;
		for (size_t i = 0; i < length; i++) {
			const random_literal_t *literal = &literals[i];

			if (literal->kind != LITERAL_COMPARISON ||
			    literal->comparator != COMPARE_EQ)
				continue;
			for (size_t side = 0; side < 2; side++) {
				uint64_t other = expression_variables(
				    &literal->sides[1 - side]);
				uint8_t variable;

				if (!expression_is_variable(
				    &literal->sides[side], &variable) ||
				    (bound_variables &
				    variable_bit(variable)) != 0 ||
				    (other & ~bound_variables) != 0)
					continue;
				bound_variables |= variable_bit(variable);
				bound_more = true;
			}
		}
	} while (bound_more);

	return (bound_variables);
}

/*
 * Whether an expression is a variable alone, and which.
 */
static bool
expression_is_variable(const random_expression_t *expression,
    uint8_t *variablep)
{
	if (expression->shape != SHAPE_LEAF || expression->negated ||
	    expression->leaves[0].kind != TERM_VARIABLE)
		return (false);

	*variablep = expression->leaves[0].index;
	return (true);
}

/*
 * The variables an expression reads, as a bit set.
 */
static uint64_t
expression_variables(const random_expression_t *expression)
{
	return (terms_variables(expression->leaves,
	    expression_leaves(expression)));
}

/* How many leaves an expression's shape holds. */
static size_t
expression_leaves(const random_expression_t *expression)
{
	switch (expression->shape) {
	case SHAPE_LEAF:
		return (1);
	case SHAPE_BINARY:
		return (2);
	case SHAPE_LEFT:
	case SHAPE_RIGHT:
		return (3);
	case SHAPE_CALL:
		return (call_arity(expression->calls[0]));
	case SHAPE_NESTED_CALL:
		return (call_arity(expression->calls[0]) - 1 +
		    call_arity(expression->calls[1]));
	}
	return (0);
}

/* How many arguments a function takes. */
static size_t
call_arity(random_call_t call)
{
	return (strlen(random_calls[call].arguments));
}

/*
 * How many argument slots of a literal are in use: its predicate's arity for
 * a relational literal, and none for a comparison, whose operands are its
 * sides, or for an aggregate.  Slots past that hold whatever they last held
 * and are never read.
 */
static size_t
literal_arity(const random_program_t *program, const random_literal_t *literal)
{
	if (literal->kind != LITERAL_POSITIVE &&
	    literal->kind != LITERAL_NEGATED)
		return (0);
	return (program->predicates[literal->predicate].arity);
}

/*
 * Whether a negated literal or a comparison is safe: every term it holds,
 * each argument of the one and each leaf of the other's two sides, is a
 * constant or a bound variable, and none is `_`, which nothing binds.
 */
static bool
test_is_safe(const random_program_t *program, const random_literal_t *literal,
    uint64_t bound_variables)
{
	if (literal->kind == LITERAL_NEGATED)
		return (terms_are_bound(literal->args,
		    literal_arity(program, literal), bound_variables));

	return (terms_are_bound(literal->sides[0].leaves,
	    expression_leaves(&literal->sides[0]), bound_variables) &&
	    terms_are_bound(literal->sides[1].leaves,
	    expression_leaves(&literal->sides[1]), bound_variables));
}

static bool
terms_are_bound(const random_term_t *terms, size_t count,
    uint64_t bound_variables)
{
	for (size_t i = 0; i < count; i++) {
		if (!term_is_bound(terms[i], bound_variables))
			return (false);
	}
	return (true);
}

/* Whether a term is safe outside a positive literal. */
static bool
term_is_bound(random_term_t term, uint64_t positive_variables)
{
	switch (term.kind) {
	case TERM_ANONYMOUS:
		return (false);
	case TERM_CONSTANT:
	case TERM_NUMBER:
	case TERM_PATTERN:
		return (true);
	case TERM_VARIABLE:
		return ((positive_variables & variable_bit(term.index)) != 0);
	}
	return (false);
}

size_t
random_program_statements(const random_program_t *program)
{
	return (program->rule_count + program->query_count);
}

/*
 * Whether any rule's body, or any group body in it, holds `_`.  A binding
 * of such a body's variables does not name the tuples that satisfy it,
 * since the column `_` stands in may hold several values under one
 * binding.
 */
bool
random_program_has_anonymous(const random_program_t *program)
{
	for (size_t r = 0; r < program->rule_count; r++) {
		const random_body_t *body = &program->rules[r].body;

		if (literals_have_anonymous(program, body->literals,
		    body->length))
			return (true);
		for (size_t i = 0; i < body->length; i++) {
			const random_literal_t *literal = &body->literals[i];
			const random_group_t *group;

			if (literal->kind != LITERAL_AGGREGATE)
				continue;
			group = &body->groups[literal->group];
			if (literals_have_anonymous(program, group->literals,
			    group->length))
				return (true);
		}
	}
	return (false);
}

static bool
literals_have_anonymous(const random_program_t *program,
    const random_literal_t *literals, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		const random_literal_t *literal = &literals[i];

		for (size_t a = 0; a < literal_arity(program, literal); a++) {
			if (literal->args[a].kind == TERM_ANONYMOUS)
				return (true);
		}
	}
	return (false);
}

/* How many aggregates a body holds. */
size_t
random_body_aggregates(const random_body_t *body)
{
	size_t count = 0;

	for (size_t i = 0; i < body->length; i++) {
		if (body->literals[i].kind == LITERAL_AGGREGATE)
			count++;
	}
	return (count);
}

/*
 * The program's facts and rules, followed not by its queries but by one
 * query per rule with a body, asking for that body alone, and after it one
 * per aggregate of the body, in written order, asking for its group body
 * alone.  A query's columns are its named variables, so each one's rows are
 * the distinct bindings of its rule's body variables, or of the variables
 * of the group rule the library compiles an aggregate's braces to.  The
 * caller frees the result.
 */
char *
random_bindings_text(const random_program_t *program)
{
	FILE *out;
	char *text = NULL;
	size_t length = 0;

	out = open_memstream(&text, &length);
	if (out == NULL)
		err(1, "open_memstream");

	for (size_t r = 0; r < program->rule_count; r++)
		write_rule(out, program, &program->rules[r]);

	for (size_t r = 0; r < program->rule_count; r++) {
		const random_body_t *body = &program->rules[r].body;

		if (body->length == 0)
			continue;
		write_body(out, program, body);
		fputs("?\n", out);

		for (size_t i = 0; i < body->length; i++) {
			const random_literal_t *literal = &body->literals[i];
			const random_group_t *group;

			if (literal->kind != LITERAL_AGGREGATE)
				continue;
			group = &body->groups[literal->group];
			write_literals(out, program, body, group->literals,
			    group->length);
			fputs("?\n", out);
		}
	}

	if (fclose(out) != 0)
		err(1, "writing a program");

	return (text);
}

/*
 * The program as source text: its facts and rules in order, then its
 * queries.  The caller frees the result.
 */
char *
random_program_text(const random_program_t *program)
{
	FILE *out;
	char *text = NULL;
	size_t length = 0;

	out = open_memstream(&text, &length);
	if (out == NULL)
		err(1, "open_memstream");
	random_program_write(program, out);
	if (fclose(out) != 0)
		err(1, "writing a program");

	return (text);
}

void
random_program_write(const random_program_t *program, FILE *out)
{
	for (size_t r = 0; r < program->rule_count; r++)
		write_rule(out, program, &program->rules[r]);

	for (size_t q = 0; q < program->query_count; q++) {
		write_body(out, program, &program->queries[q]);
		fputs("?\n", out);
	}
}

static void
write_rule(FILE *out, const random_program_t *program,
    const random_rule_t *rule)
{
	write_atom(out, program, rule->head_predicate, rule->head);
	if (rule->body.length > 0) {
		fputs(" :- ", out);
		write_body(out, program, &rule->body);
	}
	fputs(".\n", out);
}

static void
write_body(FILE *out, const random_program_t *program,
    const random_body_t *body)
{
	write_literals(out, program, body, body->literals, body->length);
}

/*
 * Literals of `body`, its own or those of one of its group bodies, joined
 * by commas.  The body is where an aggregate's group body is found.
 */
static void
write_literals(FILE *out, const random_program_t *program,
    const random_body_t *body, const random_literal_t *literals,
    size_t length)
{
	for (size_t i = 0; i < length; i++) {
		const random_literal_t *literal = &literals[i];

		if (i > 0)
			fputs(", ", out);

		switch (literal->kind) {
		case LITERAL_NEGATED:
			fputs("not ", out);
			/* FALLTHROUGH */
		case LITERAL_POSITIVE:
			write_atom(out, program, literal->predicate,
			    literal->args);
			break;
		case LITERAL_COMPARISON:
			write_expression(out, &literal->sides[0]);
			fprintf(out, " %s ",
			    comparator_spellings[literal->comparator]);
			write_expression(out, &literal->sides[1]);
			break;
		case LITERAL_AGGREGATE:
			write_aggregate(out, program, body, literal);
			break;
		}
	}
}

/* `R = count { ... }`, or `R = sum(V) { ... }` and likewise min and max. */
static void
write_aggregate(FILE *out, const random_program_t *program,
    const random_body_t *body, const random_literal_t *aggregate)
{
	const random_group_t *group = &body->groups[aggregate->group];
	random_term_t result = { TERM_VARIABLE, aggregate->result };
	random_term_t aggregated = { TERM_VARIABLE, aggregate->aggregated };

	write_term(out, result);
	fprintf(out, " = %s", function_spellings[aggregate->function]);
	if (aggregate->function != FUNCTION_COUNT) {
		fputc('(', out);
		write_term(out, aggregated);
		fputc(')', out);
	}
	fputs(" { ", out);
	write_literals(out, program, body, group->literals, group->length);
	fputs(" }", out);
}

/*
 * An expression, with the parentheses its shape needs and no others: a
 * group left of an operator binding tighter, and a group right of one
 * binding as tightly or tighter, since the operators associate to the left.
 * A negation is written over a variable alone or over a parenthesized
 * whole, never over a number, whose `-` would fold into it.
 */
static void
write_expression(FILE *out, const random_expression_t *expression)
{
	const random_term_t *leaves = expression->leaves;
	const random_operator_t *operators = expression->operators;
	bool group;

	if (expression->negated) {
		if (expression->shape == SHAPE_LEAF &&
		    leaves[0].kind == TERM_VARIABLE) {
			fputc('-', out);
			write_term(out, leaves[0]);
			return;
		}
		fputs("-(", out);
	}

	switch (expression->shape) {
	case SHAPE_LEAF:
		write_term(out, leaves[0]);
		break;
	case SHAPE_BINARY:
		write_operation(out, leaves[0], operators[0], leaves[1]);
		break;
	case SHAPE_LEFT:
		group = operator_precedence(operators[0]) <
		    operator_precedence(operators[1]);
		if (group)
			fputc('(', out);
		write_operation(out, leaves[0], operators[0], leaves[1]);
		if (group)
			fputc(')', out);
		fprintf(out, " %s ", operator_spellings[operators[1]]);
		write_term(out, leaves[2]);
		break;
	case SHAPE_RIGHT:
		group = operator_precedence(operators[1]) <=
		    operator_precedence(operators[0]);
		write_term(out, leaves[0]);
		fprintf(out, " %s ", operator_spellings[operators[0]]);
		if (group)
			fputc('(', out);
		write_operation(out, leaves[1], operators[1], leaves[2]);
		if (group)
			fputc(')', out);
		break;
	case SHAPE_CALL:
		write_call(out, expression->calls[0], leaves, false,
		    CALL_KINDS);
		break;
	case SHAPE_NESTED_CALL:
		write_call(out, expression->calls[0], leaves, true,
		    expression->calls[1]);
		break;
	}

	if (expression->negated)
		fputc(')', out);
}

static void
write_operation(FILE *out, random_term_t left, random_operator_t operator,
    random_term_t right)
{
	write_term(out, left);
	fprintf(out, " %s ", operator_spellings[operator]);
	write_term(out, right);
}

/* Addition and subtraction bind less tightly than the other three. */
static int
operator_precedence(random_operator_t operator)
{
	switch (operator) {
	case OPERATOR_ADD:
	case OPERATOR_SUBTRACT:
		return (1);
	case OPERATOR_MULTIPLY:
	case OPERATOR_DIVIDE:
	case OPERATOR_MOD:
	case OPERATOR_COUNT:
		break;
	}
	return (2);
}

/*
 * A call, `name(argument, ...)`.  When `nested`, its first argument is the
 * call `inner` over the first leaves, and its other arguments are the
 * leaves after those.
 */
static void
write_call(FILE *out, random_call_t call, const random_term_t *leaves,
    bool nested, random_call_t inner)
{
	size_t arity = call_arity(call);
	size_t first = 0;

	fprintf(out, "%s(", random_calls[call].spelling);
	if (nested) {
		write_call(out, inner, leaves, false, CALL_KINDS);
		first = call_arity(inner);
		arity--;
	}
	for (size_t a = 0; a < arity; a++) {
		if (nested || a > 0)
			fputs(", ", out);
		write_term(out, leaves[first + a]);
	}
	fputc(')', out);
}

/* A zero-arity atom is written as its bare name, as a program would. */
static void
write_atom(FILE *out, const random_program_t *program, uint8_t predicate,
    const random_term_t *args)
{
	uint8_t arity = program->predicates[predicate].arity;

	fprintf(out, "p%u", (unsigned)predicate);
	if (arity == 0)
		return;

	fputc('(', out);
	for (size_t a = 0; a < arity; a++) {
		if (a > 0)
			fputs(", ", out);
		write_term(out, args[a]);
	}
	fputc(')', out);
}

static void
write_term(FILE *out, random_term_t term)
{
	switch (term.kind) {
	case TERM_VARIABLE:
		fputc('A' + term.index, out);
		break;
	case TERM_ANONYMOUS:
		fputc('_', out);
		break;
	case TERM_CONSTANT:
		fputs(random_constants[term.index].spelling, out);
		break;
	case TERM_NUMBER:
		fprintf(out, "%d", (int)term.index - RANDOM_NUMBER_MAX);
		break;
	case TERM_PATTERN:
		fputs(random_patterns[term.index], out);
		break;
	}
}

/*
 * The shrinker's edits.  Each removes one thing and closes the gap, so the
 * rest of the program keeps its order.  None of them adds a dependency, so
 * none can make a stratified program unstratified; removing a literal or a
 * column can make a rule unsafe, which random_program_is_safe() detects.
 */

void
random_remove_query(random_program_t *program, size_t query)
{
	for (size_t q = query + 1; q < program->query_count; q++)
		program->queries[q - 1] = program->queries[q];
	program->query_count--;
}

void
random_remove_rule(random_program_t *program, size_t rule)
{
	for (size_t r = rule + 1; r < program->rule_count; r++)
		program->rules[r - 1] = program->rules[r];
	program->rule_count--;
}

void
random_remove_literal(random_body_t *body, size_t literal)
{
	for (size_t i = literal + 1; i < body->length; i++)
		body->literals[i - 1] = body->literals[i];
	body->length--;
}

void
random_remove_group_literal(random_group_t *group, size_t literal)
{
	for (size_t i = literal + 1; i < group->length; i++)
		group->literals[i - 1] = group->literals[i];
	group->length--;
}

/*
 * Remove one argument position of a predicate: from its arity, from the
 * head of every rule deriving it, and from every literal reading it, in the
 * rules, the queries and their group bodies alike.
 */
static void
remove_argument(random_term_t *args, size_t column)
{
	for (size_t a = column + 1; a < RANDOM_MAX_ARITY; a++)
		args[a - 1] = args[a];
}

static void
remove_column_in_literals(random_literal_t *literals, size_t length,
    size_t predicate, size_t column)
{
	for (size_t i = 0; i < length; i++) {
		random_literal_t *literal = &literals[i];

		if ((literal->kind == LITERAL_POSITIVE ||
		    literal->kind == LITERAL_NEGATED) &&
		    literal->predicate == predicate)
			remove_argument(literal->args, column);
	}
}

static void
remove_column_in_body(random_body_t *body, size_t predicate, size_t column)
{
	remove_column_in_literals(body->literals, body->length, predicate,
	    column);
	for (size_t g = 0; g < body->group_count; g++)
		remove_column_in_literals(body->groups[g].literals,
		    body->groups[g].length, predicate, column);
}

void
random_remove_column(random_program_t *program, size_t predicate,
    size_t column)
{
	program->predicates[predicate].arity--;

	for (size_t r = 0; r < program->rule_count; r++) {
		random_rule_t *rule = &program->rules[r];

		if (rule->head_predicate == predicate)
			remove_argument(rule->head, column);
		remove_column_in_body(&rule->body, predicate, column);
	}

	for (size_t q = 0; q < program->query_count; q++)
		remove_column_in_body(&program->queries[q], predicate, column);
}
