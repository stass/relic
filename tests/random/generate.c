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
 * can be drawn.  `_` appears only in positive literals.
 *
 * The random stream is splitmix64, which is defined by its arithmetic alone,
 * so a seed names the same program on every platform and C library.
 */

typedef struct random_stream random_stream_t;

struct random_stream {
	uint64_t	state;
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

static uint64_t		 draw(random_stream_t *stream);
static size_t		 draw_below(random_stream_t *stream, size_t bound);
static void		 draw_bound_terms(random_stream_t *stream,
			    uint64_t positive_variables, random_term_t *terms,
			    size_t count);
static bool		 draw_chance(random_stream_t *stream, size_t in);
static random_term_t	 draw_constant(random_stream_t *stream);
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
	uint64_t positive_variables;

	if (program->rule_count == RANDOM_MAX_RULES)
		return;
	rule = &program->rules[program->rule_count];

	rule->head_predicate = predicate;
	generate_body(stream, program, owner, head->level, &rule->body,
	    &positive_variables);
	draw_bound_terms(stream, positive_variables, rule->head, head->arity);

	program->rule_count++;
}

/*
 * Draw a body: positive relational literals first, then negated literals and
 * comparisons over the variables those bound.  The variables the positive
 * literals mention are returned as a bit set, for the head to draw from.
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
    uint64_t *positive_variables)
{
	size_t positive_count, negated_count, comparison_count;
	size_t variable_count;

	body->length = 0;
	*positive_variables = 0;

	/*
	 * The proportions keep most bodies satisfiable: few variables, so
	 * literals share them and joins connect.
	 */
	variable_count = 2 + draw_below(stream, RANDOM_MAX_VARIABLES - 1);
	positive_count = draw_positive_count(stream, owner);
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
			    variable_count, positive_variables);
	}

	for (size_t i = 0; i < negated_count; i++) {
		random_literal_t *literal = &body->literals[body->length++];
		predicate_range_t range =
		    owner == BODY_OF_QUERY ? READ_ANY : READ_BELOW;

		literal->kind = LITERAL_NEGATED;
		literal->predicate = pick_predicate(stream, program, range,
		    level);
		draw_bound_terms(stream, *positive_variables, literal->args,
		    program->predicates[literal->predicate].arity);
	}

	for (size_t i = 0; i < comparison_count; i++) {
		random_literal_t *literal = &body->literals[body->length++];
		size_t comparator = draw_below(stream, COMPARE_COUNT);

		literal->kind = LITERAL_COMPARISON;
		literal->comparator = (random_comparator_t)comparator;
		draw_bound_terms(stream, *positive_variables, literal->args, 2);
	}
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

static random_term_t
draw_constant(random_stream_t *stream)
{
	random_term_t term;

	term.kind = TERM_CONSTANT;
	if (draw_chance(stream, 8))
		term.index = (uint8_t)draw_below(stream, random_constant_count);
	else
		term.index = (uint8_t)draw_below(stream, COMMON_CONSTANTS);
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
