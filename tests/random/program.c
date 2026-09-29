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

#include "random.h"

/*
 * The program model's operations: printing it as relic source text,
 * checking the safety condition the generator builds in and the shrinker's
 * edits can break, and the edits themselves.
 */

static uint64_t	 assigned_variables(const random_body_t *body,
		    uint64_t bound_variables);
static bool	 body_is_bounded(const random_program_t *program,
		    const random_body_t *body, bool query, uint8_t level);
static bool	 body_is_safe(const random_program_t *program,
		    const random_body_t *body, uint64_t *bound_variables);
static bool	 body_states_small(const random_program_t *program,
		    const random_body_t *body);
static bool	 literal_has_operator(const random_literal_t *literal);
static bool	 program_computes(const random_program_t *program);
static bool	 terms_are_small(const random_term_t *terms, size_t count);
static bool	 expression_is_variable(const random_expression_t *expression,
		    uint8_t *variablep);
static size_t	 expression_leaves(const random_expression_t *expression);
static uint64_t	 expression_variables(
		    const random_expression_t *expression);
static size_t	 literal_arity(const random_program_t *program,
		    const random_literal_t *literal);
static int	 operator_precedence(random_operator_t operator);
static void	 remove_argument(random_term_t *args, size_t column);
static void	 remove_column_in_body(random_body_t *body, size_t predicate,
		    size_t column);
static bool	 term_is_bound(random_term_t term, uint64_t positive_variables);
static void	 write_atom(FILE *out, const random_program_t *program,
		    uint8_t predicate, const random_term_t *args);
static void	 write_body(FILE *out, const random_program_t *program,
		    const random_body_t *body);
static void	 write_expression(FILE *out,
		    const random_expression_t *expression);
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

/*
 * Whether every rule and query satisfies the safety condition: every
 * variable of a head, a negated literal or a comparison is bound, by a
 * positive relational literal of the same body or by an assignment, and `_`
 * occurs nowhere else.  A query must also have a body, since its body is
 * all it is.
 */
bool
random_program_is_safe(const random_program_t *program)
{
	for (size_t r = 0; r < program->rule_count; r++) {
		const random_rule_t *rule = &program->rules[r];
		uint8_t arity = program->predicates[rule->head_predicate].arity;
		uint64_t bound_variables;

		if (!body_is_safe(program, &rule->body, &bound_variables))
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
		if (!body_is_safe(program, &program->queries[q],
		    &bound_variables))
			return (false);
	}

	return (true);
}

/*
 * Whether the program's arithmetic is bounded as the generator builds it: a
 * literal holding an operator stands only in a query or in a rule whose
 * positive literals all read levels below its head's, at most
 * RANDOM_MAX_OPERATOR_LITERALS of them in a body, and a program holding one
 * states no integer beyond RANDOM_INTEGER_SMALL in magnitude.  generate.c
 * explains why such a program is defined and finite under every strategy.
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
 * Whether a body holds no more literals with an operator than a body may,
 * and none unless it is a query's or its positive literals all read below
 * `level`, its head's.
 */
static bool
body_is_bounded(const random_program_t *program, const random_body_t *body,
    bool query, uint8_t level)
{
	size_t operator_literals = 0;
	bool reads_below = true;

	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];

		if (literal_has_operator(literal))
			operator_literals++;
		if (literal->kind == LITERAL_POSITIVE &&
		    program->predicates[literal->predicate].level >= level)
			reads_below = false;
	}

	if (operator_literals > RANDOM_MAX_OPERATOR_LITERALS)
		return (false);
	return (operator_literals == 0 || query || reads_below);
}

/* Whether any literal of the program holds an operator. */
static bool
program_computes(const random_program_t *program)
{
	for (size_t r = 0; r < program->rule_count; r++) {
		const random_body_t *body = &program->rules[r].body;

		for (size_t i = 0; i < body->length; i++) {
			if (literal_has_operator(&body->literals[i]))
				return (true);
		}
	}
	for (size_t q = 0; q < program->query_count; q++) {
		const random_body_t *body = &program->queries[q];

		for (size_t i = 0; i < body->length; i++) {
			if (literal_has_operator(&body->literals[i]))
				return (true);
		}
	}
	return (false);
}

/* Whether a literal is a comparison one of whose sides has an operator. */
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

/* Whether every constant a body states is small. */
static bool
body_states_small(const random_program_t *program, const random_body_t *body)
{
	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];

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
 * Whether a body's negated literals and comparisons use only variables it
 * binds: those its positive literals mention, and those its assignments
 * bind from them.  The bound variables are returned as a bit set, for the
 * caller to check a head against.
 */
static bool
body_is_safe(const random_program_t *program, const random_body_t *body,
    uint64_t *bound_variables)
{
	uint64_t positive_variables = 0;

	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];

		if (literal->kind != LITERAL_POSITIVE)
			continue;
		for (size_t a = 0; a < literal_arity(program, literal); a++) {
			if (literal->args[a].kind == TERM_VARIABLE)
				positive_variables |=
				    UINT64_C(1) << literal->args[a].index;
		}
	}
	*bound_variables = assigned_variables(body, positive_variables);

	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];

		switch (literal->kind) {
		case LITERAL_POSITIVE:
			break;
		case LITERAL_NEGATED:
			for (size_t a = 0; a < literal_arity(program, literal);
			    a++) {
				if (!term_is_bound(literal->args[a],
				    *bound_variables))
					return (false);
			}
			break;
		case LITERAL_COMPARISON:
			if (((expression_variables(&literal->sides[0]) |
			    expression_variables(&literal->sides[1])) &
			    ~*bound_variables) != 0)
				return (false);
			break;
		}
	}

	return (true);
}

/*
 * The variables bound once a body's assignments have run, starting from
 * `bound_variables`: an equality one of whose sides is a variable alone,
 * not yet bound, and whose other side reads only bound variables binds
 * it.  An assignment may read what another binds, in whatever order they
 * are written, so the rule is applied until it binds nothing more.
 */
static uint64_t
assigned_variables(const random_body_t *body, uint64_t bound_variables)
{
	bool bound_more;

	do {
		bound_more = false;
		for (size_t i = 0; i < body->length; i++) {
			const random_literal_t *literal = &body->literals[i];

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
				    (UINT64_C(1) << variable)) != 0 ||
				    (other & ~bound_variables) != 0)
					continue;
				bound_variables |= UINT64_C(1) << variable;
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
	uint64_t variables = 0;

	for (size_t i = 0; i < expression_leaves(expression); i++) {
		if (expression->leaves[i].kind == TERM_VARIABLE)
			variables |= UINT64_C(1) << expression->leaves[i].index;
	}
	return (variables);
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
	}
	return (0);
}

/*
 * How many argument slots of a literal are in use: its predicate's arity for
 * a relational literal, and none for a comparison, whose operands are its
 * sides.  Slots past that hold whatever they last held and are never read.
 */
static size_t
literal_arity(const random_program_t *program, const random_literal_t *literal)
{
	if (literal->kind == LITERAL_COMPARISON)
		return (0);
	return (program->predicates[literal->predicate].arity);
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
		return (true);
	case TERM_VARIABLE:
		return ((positive_variables & (UINT64_C(1) << term.index)) != 0);
	}
	return (false);
}

size_t
random_program_statements(const random_program_t *program)
{
	return (program->rule_count + program->query_count);
}

/*
 * Whether any rule's body holds `_`.  A binding of such a body's variables
 * does not name the tuples that satisfy it, since the column `_` stands in
 * may hold several values under one binding.
 */
bool
random_program_has_anonymous(const random_program_t *program)
{
	for (size_t r = 0; r < program->rule_count; r++) {
		const random_body_t *body = &program->rules[r].body;

		for (size_t i = 0; i < body->length; i++) {
			const random_literal_t *literal = &body->literals[i];

			for (size_t a = 0; a < literal_arity(program, literal);
			    a++) {
				if (literal->args[a].kind == TERM_ANONYMOUS)
					return (true);
			}
		}
	}
	return (false);
}

/*
 * The program's facts and rules, followed not by its queries but by one
 * query per rule with a body, asking for that body alone, in rule order.
 * A query's columns are its named variables, so each one's rows are the
 * distinct bindings of its rule's body variables.  The caller frees the
 * result.
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
	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];

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
		}
	}
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

/*
 * Remove one argument position of a predicate: from its arity, from the
 * head of every rule deriving it, and from every literal reading it, in the
 * rules and the queries alike.
 */
static void
remove_argument(random_term_t *args, size_t column)
{
	for (size_t a = column + 1; a < RANDOM_MAX_ARITY; a++)
		args[a - 1] = args[a];
}

static void
remove_column_in_body(random_body_t *body, size_t predicate, size_t column)
{
	for (size_t i = 0; i < body->length; i++) {
		random_literal_t *literal = &body->literals[i];

		if (literal->kind != LITERAL_COMPARISON &&
		    literal->predicate == predicate)
			remove_argument(literal->args, column);
	}
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
