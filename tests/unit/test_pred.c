/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the predicate table.
 *
 * The golden language tests exercise it through whole programs, which is how
 * a user reaches it and which says nothing about the table itself.  What
 * this suite examines is the property everything above the table rests on:
 * that (name, arity) identifies a predicate exactly, so two mentions of one
 * predicate return one identifier and two predicates that merely share a
 * name never do.  A table that broke that would not fail; it would answer
 * queries from a relation holding half the tuples.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_DEFINE		"defining a predicate is recorded once"
#define	SCENARIO_DESCRIBE	"a predicate describes itself for a message"
#define	SCENARIO_GROWTH		"the table stays exact as it grows"
#define	SCENARIO_IDENTITY	"(name, arity) identifies a predicate"
#define	SCENARIO_WHERE		"a predicate remembers where it was first seen"
#define	SCENARIO_ARITY		"no predicate has more than 32 arguments"
#define	SCENARIO_ANSWER		"an answer predicate is recognized by its name"

/*
 * How many predicates the growth scenario interns.  Enough to make the
 * identifier table double several times, since a rehash is where an
 * open-addressing table loses entries if its probe sequence is wrong.
 */
#define	GROWTH_COUNT	2000

static relic_t	*context(void);
static bool	 intern(relic_t *ctx, const char *name, uint16_t arity,
		    relic_pred_id_t *idp);
static void	 name_and_arity_identify_a_predicate(void);
static void	 a_predicate_describes_itself_for_a_message(void);
static void	 a_predicate_remembers_where_it_was_first_seen(void);
static void	 defining_a_predicate_is_recorded_once(void);
static void	 the_table_stays_exact_as_it_grows(void);
static void	 no_predicate_has_more_than_32_arguments(void);
static void	 an_answer_predicate_is_recognized(void);

static relic_t *
context(void)
{
	relic_t *ctx;

	ctx = relic_create();
	if (ctx == NULL)
		check(false, "creating a context", "a context is created");

	return (ctx);
}

/*
 * Intern a predicate named by its spelling, at a location naming the call.
 *
 * Every scenario below wants a predicate rather than an interned name and a
 * location, so the two steps live here instead of at each call site.
 */
static bool
intern(relic_t *ctx, const char *name, uint16_t arity, relic_pred_id_t *idp)
{
	relic_location_t where;
	uint32_t symbol;

	if (relic_symbol_intern(ctx, name, strlen(name), &symbol) != 0)
		return (false);

	bzero(&where, sizeof(where));
	where.file = "test.dl";
	where.line = arity + 1u;
	where.column = 1;

	return (relic_pred_intern(ctx, symbol, arity, &where, idp) == 0);
}

static void
name_and_arity_identify_a_predicate(void)
{
	relic_t *ctx;
	relic_pred_id_t foo1;
	relic_pred_id_t foo1_again;
	relic_pred_id_t foo2;
	relic_pred_id_t bar1;

	ctx = context();
	if (ctx == NULL)
		return;

	if (!intern(ctx, "foo", 1, &foo1) ||
	    !intern(ctx, "foo", 1, &foo1_again) ||
	    !intern(ctx, "foo", 2, &foo2) ||
	    !intern(ctx, "bar", 1, &bar1)) {
		check(false, SCENARIO_IDENTITY, "four predicates intern");
		relic_destroy(ctx);
		return;
	}

	checkf(foo1 == foo1_again, SCENARIO_IDENTITY,
	    "two mentions of foo/1 gave %u and %u", (unsigned int)foo1,
	    (unsigned int)foo1_again);

	checkf(foo1 != foo2, SCENARIO_IDENTITY,
	    "foo/1 and foo/2 share identifier %u", (unsigned int)foo1);

	checkf(foo1 != bar1, SCENARIO_IDENTITY,
	    "foo/1 and bar/1 share identifier %u", (unsigned int)foo1);

	checkf(relic_pred_count(ctx) == 3, SCENARIO_IDENTITY,
	    "three distinct predicates were interned as %zu",
	    relic_pred_count(ctx));

	/*
	 * Identifiers are assigned in order of first mention, which is what
	 * makes a run reproducible.
	 */
	checkf(foo1 == 1 && foo2 == 2 && bar1 == 3, SCENARIO_IDENTITY,
	    "first-mention order gave %u, %u, %u", (unsigned int)foo1,
	    (unsigned int)foo2, (unsigned int)bar1);

	relic_destroy(ctx);
}

/*
 * A predicate is mentioned in many places and remembers the first, because
 * that is where the undefined-predicate warning has to point.
 */
static void
a_predicate_remembers_where_it_was_first_seen(void)
{
	relic_t *ctx;
	const relic_pred_t *pred;
	relic_location_t first;
	relic_location_t later;
	relic_pred_id_t id;
	relic_pred_id_t again;
	uint32_t symbol;

	ctx = context();
	if (ctx == NULL)
		return;

	if (relic_symbol_intern(ctx, "edge", 4, &symbol) != 0) {
		check(false, SCENARIO_WHERE, "the name interns");
		relic_destroy(ctx);
		return;
	}

	bzero(&first, sizeof(first));
	first.file = "first.dl";
	first.line = 3;
	first.column = 7;

	bzero(&later, sizeof(later));
	later.file = "later.dl";
	later.line = 99;
	later.column = 1;

	if (relic_pred_intern(ctx, symbol, 2, &first, &id) != 0 ||
	    relic_pred_intern(ctx, symbol, 2, &later, &again) != 0) {
		check(false, SCENARIO_WHERE, "both mentions intern");
		relic_destroy(ctx);
		return;
	}

	pred = relic_pred_of(ctx, id);
	if (pred == NULL) {
		check(false, SCENARIO_WHERE, "the predicate is readable");
		relic_destroy(ctx);
		return;
	}

	checkf(pred->where.line == first.line &&
	    pred->where.column == first.column &&
	    strcmp(pred->where.file, first.file) == 0, SCENARIO_WHERE,
	    "the second mention moved the location to %s:%u:%u",
	    pred->where.file, (unsigned int)pred->where.line,
	    (unsigned int)pred->where.column);

	relic_destroy(ctx);
}

static void
defining_a_predicate_is_recorded_once(void)
{
	relic_t *ctx;
	const relic_pred_t *pred;
	relic_pred_id_t defined;
	relic_pred_id_t referenced;

	ctx = context();
	if (ctx == NULL)
		return;

	if (!intern(ctx, "head", 1, &defined) ||
	    !intern(ctx, "body", 1, &referenced)) {
		check(false, SCENARIO_DEFINE, "two predicates intern");
		relic_destroy(ctx);
		return;
	}

	relic_pred_define(ctx, defined);

	pred = relic_pred_of(ctx, defined);
	check(pred != NULL && pred->defined, SCENARIO_DEFINE,
	    "a predicate something derives into is defined");

	pred = relic_pred_of(ctx, referenced);
	check(pred != NULL && !pred->defined, SCENARIO_DEFINE,
	    "a predicate only referenced is not defined");

	/*
	 * An identifier the context never assigned reads as no predicate
	 * rather than as whatever lies past the array.
	 */
	check(relic_pred_of(ctx, RELIC_ID_NONE) == NULL, SCENARIO_DEFINE,
	    "identifier zero names no predicate");
	check(relic_pred_of(ctx, (relic_pred_id_t)(relic_pred_count(ctx) + 1))
	    == NULL, SCENARIO_DEFINE,
	    "an identifier past the table names no predicate");

	relic_destroy(ctx);
}

static void
a_predicate_describes_itself_for_a_message(void)
{
	relic_t *ctx;
	char described[RELIC_PRED_TEXT_MAX];
	relic_pred_id_t id;

	ctx = context();
	if (ctx == NULL)
		return;

	if (!intern(ctx, "path", 2, &id)) {
		check(false, SCENARIO_DESCRIBE, "the predicate interns");
		relic_destroy(ctx);
		return;
	}

	relic_pred_describe(ctx, id, described, sizeof(described));
	checkf(strcmp(described, "path/2") == 0, SCENARIO_DESCRIBE,
	    "path/2 described itself as \"%s\"", described);

	/*
	 * A message must never be the thing that faults, so describing an
	 * identifier no program could produce still yields a printable
	 * string.
	 */
	relic_pred_describe(ctx, RELIC_ID_NONE, described, sizeof(described));
	checkf(strcmp(described, "?") == 0, SCENARIO_DESCRIBE,
	    "an unassigned identifier described itself as \"%s\"", described);

	relic_destroy(ctx);
}

/*
 * Intern many predicates and read every one of them back.
 *
 * Growth is where an open-addressing table loses entries: the identifier
 * table rehashes every key when it doubles, and a rehash that computed a
 * different hash from the original would leave earlier predicates
 * unreachable.  The next mention of one would then be interned a second
 * time, so the check is that the count is exactly what was asked for and
 * that re-interning finds what is already there.
 */
static void
the_table_stays_exact_as_it_grows(void)
{
	relic_t *ctx;
	relic_pred_id_t assigned[GROWTH_COUNT];
	char name[32];
	unsigned int i;

	ctx = context();
	if (ctx == NULL)
		return;

	for (i = 0; i < GROWTH_COUNT; i++) {
		snprintf(name, sizeof(name), "p%u", i);

		if (!intern(ctx, name, (uint16_t)(i % 33), &assigned[i])) {
			checkf(false, SCENARIO_GROWTH, "predicate %u interns",
			    i);
			relic_destroy(ctx);
			return;
		}
	}

	checkf(relic_pred_count(ctx) == GROWTH_COUNT, SCENARIO_GROWTH,
	    "%d distinct predicates were interned as %zu", GROWTH_COUNT,
	    relic_pred_count(ctx));

	for (i = 0; i < GROWTH_COUNT; i++) {
		relic_pred_id_t found;

		snprintf(name, sizeof(name), "p%u", i);

		if (!intern(ctx, name, (uint16_t)(i % 33), &found)) {
			checkf(false, SCENARIO_GROWTH,
			    "predicate %u is found again", i);
			break;
		}

		if (found != assigned[i]) {
			checkf(false, SCENARIO_GROWTH,
			    "predicate %u was %u and is now %u", i,
			    (unsigned int)assigned[i], (unsigned int)found);
			break;
		}
	}

	checkf(relic_pred_count(ctx) == GROWTH_COUNT, SCENARIO_GROWTH,
	    "re-interning every predicate grew the table to %zu",
	    relic_pred_count(ctx));

	relic_destroy(ctx);
}

/*
 * The evaluator builds a tuple in a fixed array of 32 columns and an index key
 * is a 32-bit column mask, so no predicate may be wider.  The parser refuses
 * such an atom where it is written, but interning is the one path every
 * predicate takes, whatever creates it, so it is refused here too.
 */
static void
no_predicate_has_more_than_32_arguments(void)
{
	relic_t *ctx;
	relic_pred_id_t id;

	ctx = context();
	if (ctx == NULL)
		return;

	check(intern(ctx, "wide", RELIC_MAX_ARITY, &id), SCENARIO_ARITY,
	    "a predicate of 32 arguments interns");
	check(!intern(ctx, "wider", RELIC_MAX_ARITY + 1, &id), SCENARIO_ARITY,
	    "a predicate of 33 arguments is refused");
	checkf(relic_pred_count(ctx) == 1, SCENARIO_ARITY,
	    "the refused predicate should not be in the table, which "
	    "holds %zu", relic_pred_count(ctx));

	relic_destroy(ctx);
}

/*
 * A query's answer predicate is named `$q` and a number, which no identifier
 * the lexer accepts can begin with, so the name is what tells it apart.
 */
static void
an_answer_predicate_is_recognized(void)
{
	relic_t *ctx;
	relic_pred_id_t answer;
	relic_pred_id_t user;

	ctx = context();
	if (ctx == NULL)
		return;

	if (!intern(ctx, "$q0", 1, &answer) || !intern(ctx, "q0", 1, &user)) {
		check(false, SCENARIO_ANSWER, "both predicates intern");
		relic_destroy(ctx);
		return;
	}

	check(relic_pred_is_answer(ctx, answer), SCENARIO_ANSWER,
	    "$q0/1 is an answer predicate");
	check(!relic_pred_is_answer(ctx, user), SCENARIO_ANSWER,
	    "q0/1 is not");

	relic_destroy(ctx);
}

int
main(void)
{
	name_and_arity_identify_a_predicate();
	a_predicate_remembers_where_it_was_first_seen();
	defining_a_predicate_is_recorded_once();
	a_predicate_describes_itself_for_a_message();
	the_table_stays_exact_as_it_grows();
	no_predicate_has_more_than_32_arguments();
	an_answer_predicate_is_recognized();

	return (check_summary("test_pred"));
}
