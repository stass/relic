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

static bool	 body_is_safe(const random_program_t *program,
		    const random_body_t *body, uint64_t *positive_variables);
static size_t	 literal_arity(const random_program_t *program,
		    const random_literal_t *literal);
static void	 remove_argument(random_term_t *args, size_t column);
static void	 remove_column_in_body(random_body_t *body, size_t predicate,
		    size_t column);
static bool	 term_is_bound(random_term_t term, uint64_t positive_variables);
static void	 write_atom(FILE *out, const random_program_t *program,
		    uint8_t predicate, const random_term_t *args);
static void	 write_body(FILE *out, const random_program_t *program,
		    const random_body_t *body);
static void	 write_rule(FILE *out, const random_program_t *program,
		    const random_rule_t *rule);
static void	 write_term(FILE *out, random_term_t term);

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
 * variable of a head, a negated literal or a comparison occurs in a
 * positive relational literal of the same body, and `_` occurs nowhere
 * else.  A query must also have a body, since its body is all it is.
 */
bool
random_program_is_safe(const random_program_t *program)
{
	for (size_t r = 0; r < program->rule_count; r++) {
		const random_rule_t *rule = &program->rules[r];
		uint8_t arity = program->predicates[rule->head_predicate].arity;
		uint64_t positive_variables;

		if (!body_is_safe(program, &rule->body, &positive_variables))
			return (false);
		for (size_t a = 0; a < arity; a++) {
			if (!term_is_bound(rule->head[a], positive_variables))
				return (false);
		}
	}

	for (size_t q = 0; q < program->query_count; q++) {
		uint64_t positive_variables;

		if (program->queries[q].length == 0)
			return (false);
		if (!body_is_safe(program, &program->queries[q],
		    &positive_variables))
			return (false);
	}

	return (true);
}

/*
 * Whether a body's negated literals and comparisons use only variables its
 * positive literals mention.  Those variables are returned as a bit set,
 * for the caller to check a head against.
 */
static bool
body_is_safe(const random_program_t *program, const random_body_t *body,
    uint64_t *positive_variables)
{
	*positive_variables = 0;
	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];

		if (literal->kind != LITERAL_POSITIVE)
			continue;
		for (size_t a = 0; a < literal_arity(program, literal); a++) {
			if (literal->args[a].kind == TERM_VARIABLE)
				*positive_variables |=
				    UINT64_C(1) << literal->args[a].index;
		}
	}

	for (size_t i = 0; i < body->length; i++) {
		const random_literal_t *literal = &body->literals[i];

		if (literal->kind == LITERAL_POSITIVE)
			continue;
		for (size_t a = 0; a < literal_arity(program, literal); a++) {
			if (!term_is_bound(literal->args[a],
			    *positive_variables))
				return (false);
		}
	}

	return (true);
}

/*
 * How many argument slots of a literal are in use: its predicate's arity for
 * a relational literal, both operands for a comparison.  Slots past that
 * hold whatever they last held and are never read.
 */
static size_t
literal_arity(const random_program_t *program, const random_literal_t *literal)
{
	if (literal->kind == LITERAL_COMPARISON)
		return (2);
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
			write_term(out, literal->args[0]);
			fprintf(out, " %s ",
			    comparator_spellings[literal->comparator]);
			write_term(out, literal->args[1]);
			break;
		}
	}
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
