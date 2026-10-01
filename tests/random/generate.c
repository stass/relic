/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
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
 * An aggregate is drawn after the positive literals and before the
 * assignments, and follows the same discipline inside its braces.  Its
 * group body reads only levels strictly below its head's, as a negated
 * literal does, so stratification holds by the argument above.  The group
 * body's positive literals draw from variables of the aggregate's own --
 * fresh, and distinct from every other aggregate's, so that none is shared
 * by accident -- and from a subset of the variables the enclosing body's
 * positive literals bind; a variable of that subset the group body mentions
 * is then shared, bound both outside the braces and inside them, as the
 * library requires, and its negated literals, comparisons and assignment
 * draw only on what its positive literals bind.  The aggregated variable is
 * one of the aggregate's own that a positive literal of the group body
 * mentions.  The result is a variable nothing before it binds, which
 * everything drawn later may read, and which the rule is then made to read
 * in its head or in a comparison; now and then it is a variable a positive
 * literal outside the braces binds instead, and the aggregate compares.
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
 * A group body reads only levels below its head's and is never rewritten,
 * since the Magic rewrite reads a group completely, so an operator inside
 * the braces cannot feed its own computation either; the two operator
 * literals are a statement's, shared by its body and its group bodies, so
 * that a value computed in a group body and passed on by min or max is
 * still multiplied by at most 81 per statement.  An aggregate brings three
 * more kinds of value, each of which then enters arithmetic as a stated
 * integer does.  min and max pass on a value of the group.  A sum is drawn
 * only in a program that computes, so the integers it states are small,
 * and over a group body whose positive literals read facts alone: every
 * summand is a stated integer of at most RANDOM_INTEGER_SMALL in magnitude,
 * and a group has no more rows than the facts of its two positive literals'
 * predicates combine to, 13 * 13, so no total exceeds 7 * 169.  A count is
 * the size of a group, a relation the program derives, and this bound alone
 * is not by construction: a count below 2^24 keeps every value below
 * 2^24 * 9^12 < 2^63, and a relation of 2^24 tuples is far beyond what a
 * generated program derives within the harness's alarm.  Were it reached,
 * the overflow would fail the query, a failure the harness reports, rather
 * than hide a disagreement.
 *
 * A program that computes may also call the string functions, which the
 * model counts as operators: a call is a literal holding one, drawn under
 * the same budget and in the same bodies, so a computed string never feeds
 * its own computation either.  Calls are defined by construction.  A
 * pattern is drawn from a fixed list of valid ones, never from a variable,
 * so no pattern fails to compile.  A string argument is a bound variable
 * or a string of the constant pool, of at most three bytes; `str` of an
 * integer spells at most twenty; and an expression holds at most three
 * leaves, so a call's string is at most three times as long as the longest
 * of its arguments, or twenty bytes.  Through the twelve literals a value
 * passes at most, no string exceeds 20 * 3^12 bytes, some ten megabytes,
 * far below the length limit, and no integer a call gives -- a length or a
 * position -- exceeds that either, below the 2^24 the bound above allows
 * a count.  An argument of the wrong type makes a call no value, which
 * filters the binding under every strategy alike.
 *
 * The random stream is splitmix64, which is defined by its arithmetic alone,
 * so a seed names the same program on every platform and C library.
 */

typedef struct random_stream random_stream_t;

/*
 * The generator's state: the random stream, whether the program being
 * drawn computes, which decides the constants it may state, whether its
 * bodies may hold aggregates, and whether what it computes may be strings.
 */
struct random_stream {
	uint64_t	state;
	bool		computes;
	bool		aggregates;
	bool		strings;
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

/*
 * The patterns, each valid in the dialect of `match`: literal bytes, both
 * anchors, sets and their negation, alternation, repetition, a group, an
 * escaped metacharacter, and the empty pattern, which every string matches.
 * Most match some of the pool's strings, a few none.
 */
const char *const random_patterns[] = {
	"\"a\"",
	"\"^a\"",
	"\"a$\"",
	"\"^$\"",
	"\"\"",
	"\"[a-c]+\"",
	"\"[^a ]\"",
	"\"x|y\"",
	"\"(a|b)*c\"",
	"\".?\"",
	"\"\\\\.\"",
	"\"^-?[0-9]+$\"",
};
const size_t random_pattern_count =
    sizeof(random_patterns) / sizeof(random_patterns[0]);

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
	READ_UP_TO,
	READ_FACTS		/* level zero, whatever the head's */
} predicate_range_t;

/* A rule's own variables, those outside every aggregate. */
#define	RULE_VARIABLES	((UINT64_C(1) << RANDOM_MAX_VARIABLES) - 1)

static bool		 body_may_compute(const random_program_t *program,
			    const random_body_t *body, body_owner_t owner,
			    uint8_t level);
static uint64_t		 draw(random_stream_t *stream);
static void		 draw_aggregate(random_stream_t *stream,
			    const random_program_t *program, body_owner_t owner,
			    uint8_t level, uint64_t positive_variables,
			    size_t *operator_literalsp, random_body_t *body,
			    uint64_t *bound_variables);
static size_t		 draw_aggregate_count(random_stream_t *stream);
static void		 draw_assignment(random_stream_t *stream,
			    random_literal_t *literals, size_t *lengthp,
			    size_t capacity, uint64_t targets,
			    size_t *operator_literalsp,
			    uint64_t *bound_variables);
static void		 draw_comparison(random_stream_t *stream,
			    uint64_t bound_variables,
			    size_t *operator_literalsp,
			    random_literal_t *literal);
static void		 draw_expression(random_stream_t *stream,
			    uint64_t bound_variables,
			    random_expression_t *expression);
static void		 draw_call(random_stream_t *stream,
			    uint64_t bound_variables,
			    random_expression_t *expression);
static random_call_t	 draw_inner_call(random_stream_t *stream,
			    random_call_t outer);
static random_term_t	 draw_argument(random_stream_t *stream,
			    uint64_t bound_variables, char kind);
static random_term_t	 draw_string_constant(random_stream_t *stream);
static random_function_t draw_function(random_stream_t *stream);
static random_term_t	 draw_leaf(random_stream_t *stream,
			    uint64_t bound_variables);
static uint8_t		 draw_member(random_stream_t *stream, uint64_t set);
static void		 draw_negated(random_stream_t *stream,
			    const random_program_t *program,
			    predicate_range_t range, uint8_t level,
			    uint64_t bound_variables,
			    random_literal_t *literal);
static random_term_t	 draw_number(random_stream_t *stream, bool nonzero);
static void		 draw_positive(random_stream_t *stream,
			    const random_program_t *program,
			    predicate_range_t range, uint8_t level,
			    uint64_t variables, random_literal_t *literal,
			    uint64_t *positive_variables);
static uint64_t		 draw_subset(random_stream_t *stream,
			    uint64_t set);
static void		 fit_operation(random_stream_t *stream,
			    random_term_t *left, random_operator_t operator,
			    random_term_t *right);
static void		 insert_literal(random_stream_t *stream,
			    random_literal_t *literals, size_t *lengthp,
			    size_t capacity, const random_literal_t *literal);
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
			    uint64_t variables, uint64_t *positive_variables);
static predicate_range_t draw_positive_range(random_stream_t *stream,
			    body_owner_t owner);
static void		 generate_body(random_stream_t *stream,
			    const random_program_t *program,
			    body_owner_t owner, uint8_t level,
			    random_body_t *body, uint64_t *bound_variables,
			    uint64_t *result_variables);
static void		 generate_fact(random_stream_t *stream,
			    random_program_t *program, uint8_t predicate);
static void		 generate_group(random_stream_t *stream,
			    const random_program_t *program, body_owner_t owner,
			    uint8_t level, bool sums, uint64_t shareable,
			    uint64_t local_variables,
			    size_t *operator_literalsp, random_group_t *group,
			    uint64_t *positive_variables);
static void		 generate_predicates(random_stream_t *stream,
			    random_program_t *program,
			    size_t *extensional_count);
static void		 generate_rule(random_stream_t *stream,
			    random_program_t *program, uint8_t predicate,
			    body_owner_t owner);
static uint64_t		 group_local_variables(size_t group,
			    size_t count);
static size_t		 list_members(uint64_t set, uint8_t *members);
static uint8_t		 pick_predicate(random_stream_t *stream,
			    const random_program_t *program,
			    predicate_range_t range, uint8_t level);
static void		 read_results(random_stream_t *stream,
			    const random_program_t *program,
			    random_rule_t *rule, uint64_t bound_variables,
			    uint64_t result_variables);
static void		 shuffle_rules(random_stream_t *stream,
			    random_program_t *program);
static uint64_t		 variable_bit(uint8_t variable);

void
random_generate(random_program_t *program, uint64_t seed)
{
	random_stream_t stream;
	size_t extensional_count, query_count;

	bzero(program, sizeof(*program));
	stream.state = seed;
	stream.computes = draw_chance(&stream, 2);
	stream.aggregates = draw_chance(&stream, 2);
	stream.strings = stream.computes && draw_chance(&stream, 2);

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
		uint64_t bound_variables, result_variables;

		generate_body(&stream, program, BODY_OF_QUERY, 0,
		    &program->queries[q], &bound_variables, &result_variables);
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
	uint64_t bound_variables, result_variables;

	if (program->rule_count == RANDOM_MAX_RULES)
		return;
	rule = &program->rules[program->rule_count];

	rule->head_predicate = predicate;
	generate_body(stream, program, owner, head->level, &rule->body,
	    &bound_variables, &result_variables);
	draw_bound_terms(stream, bound_variables, rule->head, head->arity);
	read_results(stream, program, rule, bound_variables, result_variables);

	program->rule_count++;
}

/*
 * Make the rule read every result its aggregates bind that nothing else in
 * it reads: in a head column no other result took, or, when the head has
 * no such column, in a comparison added to the body.  A result nothing
 * reads would draw the singleton warning the library gives for a typo, and
 * test only whether the aggregate has a value.
 */
static void
read_results(random_stream_t *stream, const random_program_t *program,
    random_rule_t *rule, uint64_t bound_variables, uint64_t result_variables)
{
	random_body_t *body = &rule->body;
	uint8_t arity = program->predicates[rule->head_predicate].arity;
	uint64_t read_variables = 0, unread;
	uint8_t free_columns[RANDOM_MAX_ARITY];
	size_t free_count = 0;

	for (uint8_t a = 0; a < arity; a++) {
		if (rule->head[a].kind == TERM_VARIABLE)
			read_variables |= variable_bit(rule->head[a].index);
	}
	for (size_t i = 0; i < body->length; i++) {
		if (body->literals[i].kind != LITERAL_AGGREGATE)
			read_variables |= random_literal_variables(program,
			    &body->literals[i]);
	}
	unread = result_variables & ~read_variables;

	for (uint8_t a = 0; a < arity; a++) {
		if (rule->head[a].kind != TERM_VARIABLE ||
		    (result_variables & variable_bit(rule->head[a].index)) == 0)
			free_columns[free_count++] = a;
	}

	while (unread != 0) {
		uint8_t result = draw_member(stream, unread);
		random_literal_t comparison;
		size_t comparator;

		unread &= ~variable_bit(result);
		if (free_count > 0) {
			size_t pick = draw_below(stream, free_count);

			rule->head[free_columns[pick]].kind = TERM_VARIABLE;
			rule->head[free_columns[pick]].index = result;
			free_columns[pick] = free_columns[--free_count];
			continue;
		}

		bzero(&comparison, sizeof(comparison));
		comparison.kind = LITERAL_COMPARISON;
		comparator = draw_below(stream, COMPARE_COUNT);
		comparison.comparator = (random_comparator_t)comparator;
		comparison.sides[0].leaves[0].kind = TERM_VARIABLE;
		comparison.sides[0].leaves[0].index = result;
		comparison.sides[1].leaves[0] = draw_leaf(stream,
		    bound_variables & ~variable_bit(result));
		insert_literal(stream, body->literals, &body->length,
		    RANDOM_MAX_BODY, &comparison);
	}
}

/*
 * Draw a body: positive relational literals first, then aggregates over
 * what those bound, then assignments binding fresh variables from what is
 * bound, then negated literals and comparisons over every variable bound.
 * The bound variables are returned as a bit set, for the head to draw
 * from, and so are the results the aggregates bind, for the head to read.
 *
 * What a body may read depends on what it belongs to.  A query reads any
 * predicate, since nothing depends on a query.  A rule reads what its head's
 * level allows, and a predicate's base rule, its first, reads only levels
 * strictly below its head's, so that every derived predicate has a way to
 * hold tuples that does not go through itself.
 *
 * An occasional rule has no positive literal at all, only aggregates over
 * no shared variable, ground negations and comparisons, or nothing, which
 * makes it a fact; a query always has one, since a query's body is its
 * whole text.
 */
static void
generate_body(random_stream_t *stream, const random_program_t *program,
    body_owner_t owner, uint8_t level, random_body_t *body,
    uint64_t *bound_variables, uint64_t *result_variables)
{
	size_t positive_count, aggregate_count, negated_count, comparison_count;
	size_t assignment_count;
	size_t variable_count;
	size_t operator_literals;
	uint64_t rule_variables, positive_variables;
	predicate_range_t negated_range =
	    owner == BODY_OF_QUERY ? READ_ANY : READ_BELOW;

	body->length = 0;
	body->group_count = 0;
	*bound_variables = 0;

	/*
	 * The proportions keep most bodies satisfiable: few variables, so
	 * literals share them and joins connect.
	 */
	variable_count = 2 + draw_below(stream, RANDOM_MAX_VARIABLES - 1);
	rule_variables = (UINT64_C(1) << variable_count) - 1;
	positive_count = draw_positive_count(stream, owner);
	aggregate_count = draw_aggregate_count(stream);
	assignment_count = draw_assignment_count(stream);
	negated_count = draw_filter_count(stream);
	comparison_count = draw_filter_count(stream);

	for (size_t i = 0; i < positive_count; i++)
		draw_positive(stream, program, draw_positive_range(stream,
		    owner), level, rule_variables,
		    &body->literals[body->length++], bound_variables);
	positive_variables = *bound_variables;

	operator_literals = stream->computes ? RANDOM_MAX_OPERATOR_LITERALS : 0;
	for (size_t i = 0; i < aggregate_count; i++)
		draw_aggregate(stream, program, owner, level,
		    positive_variables, &operator_literals, body,
		    bound_variables);
	*result_variables = *bound_variables & ~positive_variables;

	if (!body_may_compute(program, body, owner, level))
		operator_literals = 0;
	for (size_t i = 0; i < assignment_count; i++)
		draw_assignment(stream, body->literals, &body->length,
		    RANDOM_MAX_BODY, RULE_VARIABLES, &operator_literals,
		    bound_variables);

	for (size_t i = 0; i < negated_count; i++)
		draw_negated(stream, program, negated_range, level,
		    *bound_variables, &body->literals[body->length++]);

	for (size_t i = 0; i < comparison_count; i++)
		draw_comparison(stream, *bound_variables, &operator_literals,
		    &body->literals[body->length++]);
}

/*
 * Draw a positive literal of a predicate in `range`, each argument mostly
 * one of `variables`, and add the variables it mentions to
 * *positive_variables.
 */
static void
draw_positive(random_stream_t *stream, const random_program_t *program,
    predicate_range_t range, uint8_t level, uint64_t variables,
    random_literal_t *literal, uint64_t *positive_variables)
{
	uint8_t arity;

	literal->kind = LITERAL_POSITIVE;
	literal->predicate = pick_predicate(stream, program, range, level);
	arity = program->predicates[literal->predicate].arity;
	for (size_t a = 0; a < arity; a++)
		literal->args[a] = draw_positive_term(stream, variables,
		    positive_variables);
}

/* Draw a negated literal of a predicate in `range`, over bound terms. */
static void
draw_negated(random_stream_t *stream, const random_program_t *program,
    predicate_range_t range, uint8_t level, uint64_t bound_variables,
    random_literal_t *literal)
{
	literal->kind = LITERAL_NEGATED;
	literal->predicate = pick_predicate(stream, program, range, level);
	draw_bound_terms(stream, bound_variables, literal->args,
	    program->predicates[literal->predicate].arity);
}

/*
 * Draw a comparison of two bound terms, each side made an expression with
 * operators half the time while *operator_literalsp allows one more.
 */
static void
draw_comparison(random_stream_t *stream, uint64_t bound_variables,
    size_t *operator_literalsp, random_literal_t *literal)
{
	size_t comparator = draw_below(stream, COMPARE_COUNT);
	random_term_t operands[2];

	literal->kind = LITERAL_COMPARISON;
	literal->comparator = (random_comparator_t)comparator;
	draw_bound_terms(stream, bound_variables, operands, 2);
	for (size_t side = 0; side < 2; side++) {
		random_expression_t *expression = &literal->sides[side];

		bzero(expression, sizeof(*expression));
		expression->leaves[0] = operands[side];
		if (*operator_literalsp > 0 && draw_chance(stream, 2)) {
			draw_expression(stream, bound_variables, expression);
			(*operator_literalsp)--;
		}
	}
}

/*
 * Draw an aggregate over what the body's positive literals bind, insert it
 * anywhere in the body, and add its result to the bound variables.  The
 * result is drawn first, so that the variables the group body may share
 * can leave it out: a result inside its own braces would be shared, and
 * could be bound only by something else.  A body with no variable left for
 * a result and none bound to compare against gets no aggregate.
 */
static void
draw_aggregate(random_stream_t *stream, const random_program_t *program,
    body_owner_t owner, uint8_t level, uint64_t positive_variables,
    size_t *operator_literalsp, random_body_t *body,
    uint64_t *bound_variables)
{
	uint8_t group_index = (uint8_t)body->group_count;
	uint64_t fresh_variables = RULE_VARIABLES & ~*bound_variables;
	uint64_t local_variables, shareable;
	uint64_t group_positive_variables, aggregated_candidates;
	random_literal_t literal;
	bool compares;

	compares = positive_variables != 0 &&
	    (fresh_variables == 0 || draw_chance(stream, 8));
	if (!compares && fresh_variables == 0)
		return;

	bzero(&literal, sizeof(literal));
	literal.kind = LITERAL_AGGREGATE;
	literal.group = group_index;
	literal.function = draw_function(stream);
	literal.result = draw_member(stream,
	    compares ? positive_variables : fresh_variables);

	/*
	 * Two to four variables of its own, fewer making the group body's
	 * literals join on them more often.
	 */
	local_variables = group_local_variables(group_index,
	    2 + draw_below(stream, RANDOM_GROUP_VARIABLES - 1));
	shareable = draw_subset(stream,
	    positive_variables & ~variable_bit(literal.result));
	generate_group(stream, program, owner, level,
	    literal.function == FUNCTION_SUM, shareable, local_variables,
	    operator_literalsp, &body->groups[group_index],
	    &group_positive_variables);

	/*
	 * A group body whose positive literals mention none of the
	 * aggregate's own variables has none to fold, and is counted.
	 */
	aggregated_candidates = group_positive_variables & local_variables;
	if (aggregated_candidates == 0)
		literal.function = FUNCTION_COUNT;
	if (literal.function != FUNCTION_COUNT)
		literal.aggregated = draw_member(stream,
		    aggregated_candidates);

	insert_literal(stream, body->literals, &body->length, RANDOM_MAX_BODY,
	    &literal);
	body->group_count++;
	*bound_variables |= variable_bit(literal.result);
}

/*
 * Draw a group body: one or two positive literals over the shareable
 * variables and the aggregate's own, then perhaps an assignment to one of
 * its own, a negated literal and a comparison, over what the positive
 * literals bind.  It reads levels strictly below its head's, any level in
 * a query, and level zero alone for a sum.  The variables its positive
 * literals mention are returned.
 */
static void
generate_group(random_stream_t *stream, const random_program_t *program,
    body_owner_t owner, uint8_t level, bool sums, uint64_t shareable,
    uint64_t local_variables, size_t *operator_literalsp,
    random_group_t *group, uint64_t *positive_variables)
{
	predicate_range_t range =
	    owner == BODY_OF_QUERY ? READ_ANY : READ_BELOW;
	predicate_range_t positive_range = sums ? READ_FACTS : range;
	size_t positive_count = 1 + draw_below(stream, 2);
	uint64_t bound_variables;

	group->length = 0;
	*positive_variables = 0;

	for (size_t i = 0; i < positive_count; i++)
		draw_positive(stream, program, positive_range, level,
		    shareable | local_variables,
		    &group->literals[group->length++], positive_variables);
	bound_variables = *positive_variables;

	if (draw_chance(stream, 3))
		draw_assignment(stream, group->literals, &group->length,
		    RANDOM_MAX_GROUP_BODY, local_variables, operator_literalsp,
		    &bound_variables);
	if (draw_chance(stream, 4))
		draw_negated(stream, program, range, level, bound_variables,
		    &group->literals[group->length++]);
	if (draw_chance(stream, 4))
		draw_comparison(stream, bound_variables, operator_literalsp,
		    &group->literals[group->length++]);
}

/*
 * The first `count` of the variables reserved for the aggregate whose group
 * body is `group` in its body, which follow the rule's own and the earlier
 * aggregates'.
 */
static uint64_t
group_local_variables(size_t group, size_t count)
{
	size_t first = RANDOM_MAX_VARIABLES + group * RANDOM_GROUP_VARIABLES;

	assert(count <= RANDOM_GROUP_VARIABLES);
	return (((UINT64_C(1) << count) - 1) << first);
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
 * Draw an assignment to one of `targets` nothing binds yet, from what is
 * bound, written either way round and inserted anywhere among the
 * literals, and add the variable to the bound ones.  Its value is mostly
 * an expression with operators while *operator_literalsp allows one more,
 * which it then uses, and otherwise a bound variable or a constant.  With
 * no target left to bind there is no assignment.
 */
static void
draw_assignment(random_stream_t *stream, random_literal_t *literals,
    size_t *lengthp, size_t capacity, uint64_t targets,
    size_t *operator_literalsp, uint64_t *bound_variables)
{
	random_literal_t literal;
	uint64_t free_variables = targets & ~*bound_variables;
	size_t value_side;
	uint8_t target;

	if (free_variables == 0)
		return;
	target = draw_member(stream, free_variables);

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

	insert_literal(stream, literals, lengthp, capacity, &literal);
	*bound_variables |= variable_bit(target);
}

/*
 * Draw an expression with one or two operators over bound variables and
 * numbers, fitted so that it is defined whatever integers its variables
 * hold: each operator's operands are fitted where it is drawn.  An operator
 * whose right operand is a group never divides by it, so a division or a
 * remainder there is made an addition.  In a program that computes
 * strings, half the expressions are calls instead.
 */
static void
draw_expression(random_stream_t *stream, uint64_t bound_variables,
    random_expression_t *expression)
{
	random_term_t *leaves = expression->leaves;
	random_operator_t *operators = expression->operators;

	if (stream->strings && draw_chance(stream, 2)) {
		draw_call(stream, bound_variables, expression);
		return;
	}

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
	case SHAPE_CALL:
	case SHAPE_NESTED_CALL:
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
 * Draw a call of any function, a third of the time with a call as its first
 * argument, each argument drawn for the type the function takes there.
 */
static void
draw_call(random_stream_t *stream, uint64_t bound_variables,
    random_expression_t *expression)
{
	const char *arguments;
	size_t leaf = 0;
	size_t function;

	bzero(expression, sizeof(*expression));
	expression->negated = draw_chance(stream, 8);
	function = draw_below(stream, CALL_KINDS);
	expression->calls[0] = (random_call_t)function;
	arguments = random_calls[function].arguments;

	expression->shape = SHAPE_CALL;
	if (draw_chance(stream, 3)) {
		const char *inner_arguments;

		expression->shape = SHAPE_NESTED_CALL;
		expression->calls[1] = draw_inner_call(stream,
		    expression->calls[0]);
		inner_arguments = random_calls[expression->calls[1]].arguments;
		for (size_t a = 0; inner_arguments[a] != '\0'; a++)
			expression->leaves[leaf++] = draw_argument(stream,
			    bound_variables, inner_arguments[a]);
		arguments++;
	}

	for (size_t a = 0; arguments[a] != '\0'; a++)
		expression->leaves[leaf++] = draw_argument(stream,
		    bound_variables, arguments[a]);
	assert(leaf <= RANDOM_MAX_CALL_ARGUMENTS);
}

/*
 * A call to stand as the first argument of `outer`: one whose value is of
 * the type `outer` takes there, a string unless it takes any value, and
 * whose arguments fit beside the outer call's others among an expression's
 * leaves.  `str` always qualifies.
 */
static random_call_t
draw_inner_call(random_stream_t *stream, random_call_t outer)
{
	const char *outer_arguments = random_calls[outer].arguments;
	size_t room = RANDOM_MAX_CALL_ARGUMENTS - (strlen(outer_arguments) - 1);
	random_call_t candidates[CALL_KINDS];
	size_t candidate_count = 0;

	for (size_t c = 0; c < CALL_KINDS; c++) {
		const random_call_info_t *inner = &random_calls[c];

		if (strlen(inner->arguments) > room)
			continue;
		if (!inner->text && outer_arguments[0] != 'v')
			continue;
		candidates[candidate_count++] = (random_call_t)c;
	}

	assert(candidate_count > 0);
	return (candidates[draw_below(stream, candidate_count)]);
}

/*
 * An argument of the type a signature letter names: for a string, two
 * times in three a bound variable, when there is one, and otherwise a
 * string of the pool; for an integer, what draw_leaf() gives; for a
 * pattern, one of the list; for any value, a bound variable or a constant.
 * A bound variable may hold a value of another type, which makes the call
 * no value.
 */
static random_term_t
draw_argument(random_stream_t *stream, uint64_t bound_variables, char kind)
{
	random_term_t term;

	switch (kind) {
	case 's':
		if (bound_variables == 0 || draw_chance(stream, 3))
			return (draw_string_constant(stream));
		break;
	case 'i':
		return (draw_leaf(stream, bound_variables));
	case 'p':
		term.kind = TERM_PATTERN;
		term.index = (uint8_t)draw_below(stream, random_pattern_count);
		return (term);
	default:
		if (bound_variables == 0 || draw_chance(stream, 3))
			return (draw_constant(stream));
		break;
	}

	term.kind = TERM_VARIABLE;
	term.index = draw_member(stream, bound_variables);
	return (term);
}

/* A string of the constant pool, each alike. */
static random_term_t
draw_string_constant(random_stream_t *stream)
{
	uint8_t strings[sizeof(random_constants) / sizeof(random_constants[0])];
	size_t string_count = 0;
	random_term_t term;

	for (size_t i = 0; i < random_constant_count; i++) {
		if (random_constants[i].type == CONSTANT_STRING)
			strings[string_count++] = (uint8_t)i;
	}

	term.kind = TERM_CONSTANT;
	term.index = strings[draw_below(stream, string_count)];
	return (term);
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
	random_term_t term;

	if (bound_variables == 0 || draw_chance(stream, 3))
		return (draw_number(stream, false));

	term.kind = TERM_VARIABLE;
	term.index = draw_member(stream, bound_variables);
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
 * Insert a literal at a position drawn among the `*lengthp` held, moving the
 * ones after it along.
 */
static void
insert_literal(random_stream_t *stream, random_literal_t *literals,
    size_t *lengthp, size_t capacity, const random_literal_t *literal)
{
	size_t position;

	if (*lengthp >= capacity)
		errx(1, "a body of %zu literals has no room for another",
		    capacity);

	position = draw_below(stream, *lengthp + 1);
	for (size_t i = *lengthp; i > position; i--)
		literals[i] = literals[i - 1];
	literals[position] = *literal;
	(*lengthp)++;
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
 * How many aggregates a body has: in a program with aggregates, one a third
 * of the time and now and then two, and otherwise none.
 */
static size_t
draw_aggregate_count(random_stream_t *stream)
{
	if (!stream->aggregates || !draw_chance(stream, 3))
		return (0);
	return (draw_chance(stream, 4) ? 2 : 1);
}

/*
 * An aggregate's function, each alike, except that only a program that
 * computes sums: a sum is safe only over the small integers such a program
 * states, and a program that does not compute counts instead.
 */
static random_function_t
draw_function(random_stream_t *stream)
{
	size_t function = draw_below(stream, FUNCTION_KINDS);

	if (function == FUNCTION_SUM && !stream->computes)
		function = FUNCTION_COUNT;
	return ((random_function_t)function);
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
 * An argument of a positive literal: nine times in ten one of `variables`,
 * which the literal then binds, and otherwise a constant or `_` alike.
 */
static random_term_t
draw_positive_term(random_stream_t *stream, uint64_t variables,
    uint64_t *positive_variables)
{
	random_term_t term;
	size_t choice = draw_below(stream, 20);

	if (choice < 18) {
		term.kind = TERM_VARIABLE;
		term.index = draw_member(stream, variables);
		*positive_variables |= variable_bit(term.index);
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
		case READ_FACTS:
			allowed = candidate_level == 0;
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
	uint8_t bound[RANDOM_VARIABLE_COUNT];
	size_t bound_count = list_members(positive_variables, bound);

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
 * The variables of a set, in increasing order, and how many there are.
 */
static size_t
list_members(uint64_t set, uint8_t *members)
{
	size_t count = 0;

	for (uint8_t v = 0; v < RANDOM_VARIABLE_COUNT; v++) {
		if ((set & variable_bit(v)) != 0)
			members[count++] = v;
	}
	return (count);
}

/* One variable of a set that is not empty, each alike. */
static uint8_t
draw_member(random_stream_t *stream, uint64_t set)
{
	uint8_t members[RANDOM_VARIABLE_COUNT];
	size_t count = list_members(set, members);

	assert(count > 0);
	return (members[draw_below(stream, count)]);
}

/* Each variable of a set, kept or not alike. */
static uint64_t
draw_subset(random_stream_t *stream, uint64_t set)
{
	uint8_t members[RANDOM_VARIABLE_COUNT];
	size_t count = list_members(set, members);
	uint64_t subset = 0;

	for (size_t i = 0; i < count; i++) {
		if (draw_chance(stream, 2))
			subset |= variable_bit(members[i]);
	}
	return (subset);
}

static uint64_t
variable_bit(uint8_t variable)
{
	return (UINT64_C(1) << variable);
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
