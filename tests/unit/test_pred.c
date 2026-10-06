/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "harness.h"
#include "internal.h"

#define	SCENARIO_DEFINE		"a predicate is defined by what derives it"
#define	SCENARIO_DESCRIBE	"a predicate describes itself for a message"
#define	SCENARIO_GROWTH		"the table stays exact as it grows"
#define	SCENARIO_IDENTITY	"(name, arity) identifies a predicate"
#define	SCENARIO_WHERE		"a predicate remembers the first atom reading it"
#define	SCENARIO_SOURCES	"a source name is kept once and whole"
#define	SOURCE_NAMES		100
#define	LONG_SOURCE		(64 * 1024)
#define	SCENARIO_ARITY		"no predicate has more than 32 arguments"

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
static void	 a_predicate_remembers_the_first_atom_reading_it(void);
static void	 a_source_name_is_kept_once_and_whole(void);
static void	 a_predicate_is_defined_by_what_derives_it(void);
static void	 the_table_stays_exact_as_it_grows(void);
static void	 no_predicate_has_more_than_32_arguments(void);

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
 */
static bool
intern(relic_t *ctx, const char *name, uint16_t arity, relic_pred_id_t *idp)
{
	relic_location_t where;

	bzero(&where, sizeof(where));
	where.file = "test.dl";
	where.line = arity + 1u;
	where.column = 1;

	return (relic_pred_intern(ctx, name, strlen(name), arity, &where,
	    idp) == 0);
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
 * A predicate is read in many places and remembers the first, because that
 * is where the undefined-predicate warning has to point.  Interning it is
 * not reading it: a predicate a load interned for facts it never committed
 * must never be warned about.
 */
static void
a_predicate_remembers_the_first_atom_reading_it(void)
{
	relic_t *ctx;
	const relic_pred_t *pred;
	relic_location_t first;
	relic_location_t later;
	relic_pred_id_t id;
	relic_pred_id_t again;
	char source[16];

	ctx = context();
	if (ctx == NULL)
		return;

	strlcpy(source, "first.dl", sizeof(source));
	bzero(&first, sizeof(first));
	first.file = source;
	first.line = 3;
	first.column = 7;

	bzero(&later, sizeof(later));
	later.file = "later.dl";
	later.line = 99;
	later.column = 1;

	if (relic_pred_intern(ctx, "edge", 4, 2, &later, &id) != 0 ||
	    relic_pred_intern(ctx, "edge", 4, 2, &later, &again) != 0) {
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

	check(!pred->referenced, SCENARIO_WHERE,
	    "interning a predicate should not mark it read");

	check(relic_pred_reference(ctx, id, &first) == 0 &&
	    relic_pred_reference(ctx, again, &later) == 0, SCENARIO_WHERE,
	    "both readings are recorded");

	checkf(pred->referenced && pred->referenced_at.line == first.line &&
	    pred->referenced_at.column == first.column &&
	    strcmp(pred->referenced_at.file, first.file) == 0, SCENARIO_WHERE,
	    "the second reading moved the location to %s:%u:%u",
	    pred->referenced_at.file, (unsigned int)pred->referenced_at.line,
	    (unsigned int)pred->referenced_at.column);

	/*
	 * The location may come from a query, whose source name dies with
	 * it, so the predicate keeps a copy of the name of its own.
	 */
	strlcpy(source, "overwritten", sizeof(source));
	check(pred->referenced_at.file != source &&
	    strcmp(pred->referenced_at.file, "first.dl") == 0, SCENARIO_WHERE,
	    "the remembered location should keep a copy of its source name");

	relic_destroy(ctx);
}

/*
 * The name store keeps one copy of each source name, however many
 * predicates its text first reads and however far the store's index has
 * grown since.  A copy the memory limit refuses fails the reference and
 * records nothing, so the predicate is not left read from no source: the
 * same reference made again, once the limit admits it, records the name.
 */
static void
a_source_name_is_kept_once_and_whole(void)
{
	char name[32];
	char *long_name;
	relic_location_t where;
	relic_pred_id_t id;
	const relic_pred_t *pred;
	relic_t *ctx;
	size_t round;
	size_t i;
	bool recorded;

	ctx = context();
	if (ctx == NULL)
		return;

	bzero(&where, sizeof(where));
	where.file = name;
	where.line = 1;
	where.column = 1;

	recorded = true;
	for (round = 0; round < 2; round++) {
		for (i = 0; i < SOURCE_NAMES; i++) {
			snprintf(name, sizeof(name), "source%zu.dl", i);
			if (relic_pred_intern(ctx, name, strlen(name),
			    (uint16_t)(round + 1), &where, &id) != 0 ||
			    relic_pred_reference(ctx, id, &where) != 0)
				recorded = false;
		}
	}
	check(recorded, SCENARIO_SOURCES, "every reading is recorded");
	checkf(ctx->program.predicates.source_count == SOURCE_NAMES,
	    SCENARIO_SOURCES, "%d source names should be kept once each, "
	    "and %zu are kept", SOURCE_NAMES,
	    ctx->program.predicates.source_count);

	long_name = malloc(LONG_SOURCE + 1);
	if (long_name == NULL) {
		check(false, SCENARIO_SOURCES, "a long source name is written");
		relic_destroy(ctx);
		return;
	}
	memset(long_name, 'n', LONG_SOURCE);
	long_name[LONG_SOURCE] = '\0';
	where.file = long_name;

	if (relic_pred_intern(ctx, "read", 4, 1, &where, &id) != 0 ||
	    (pred = relic_pred_of(ctx, id)) == NULL) {
		check(false, SCENARIO_SOURCES, "the read predicate interns");
		free(long_name);
		relic_destroy(ctx);
		return;
	}

	ctx->account.limit = ctx->account.name_bytes + 1;
	check(relic_pred_reference(ctx, id, &where) != 0, SCENARIO_SOURCES,
	    "a source name the limit refuses fails the reference");
	check(!pred->referenced, SCENARIO_SOURCES,
	    "a refused reference records nothing");

	ctx->account.limit = 0;
	check(relic_pred_reference(ctx, id, &where) == 0 &&
	    pred->referenced && pred->referenced_at.file != NULL &&
	    strcmp(pred->referenced_at.file, long_name) == 0,
	    SCENARIO_SOURCES, "the reference made again records the name");
	checkf(ctx->account.name_bytes == relic_arena_total_bytes(
	    &ctx->program.predicates.names), SCENARIO_SOURCES,
	    "the account should count the %zu bytes the names hold, not %zu",
	    relic_arena_total_bytes(&ctx->program.predicates.names),
	    ctx->account.name_bytes);

	free(long_name);
	relic_destroy(ctx);
}

/*
 * A predicate is defined when a rule's head names it, when a rule deriving
 * it joins a rule table, when it is a base view, or when its base store
 * holds a fact; one only read is not.
 */
static void
a_predicate_is_defined_by_what_derives_it(void)
{
	relic_t *ctx;
	relic_ruletab_t rules;
	relic_rule_t rule;
	relic_value_t value;
	relic_pred_id_t named;
	relic_pred_id_t appended;
	relic_pred_id_t view;
	relic_pred_id_t referenced;

	ctx = context();
	if (ctx == NULL)
		return;

	if (!intern(ctx, "head", 1, &named) ||
	    !intern(ctx, "derived", 0, &appended) ||
	    !intern(ctx, "view", 1, &view) ||
	    !intern(ctx, "body", 1, &referenced)) {
		check(false, SCENARIO_DEFINE, "four predicates intern");
		relic_destroy(ctx);
		return;
	}

	relic_pred_name_head(ctx, named);
	relic_pred_make_view(ctx, view, named);

	bzero(&rules, sizeof(rules));
	bzero(&rule, sizeof(rule));
	rule.head = appended;
	check(relic_ruletab_append(ctx, &rules, &rule) == 0, SCENARIO_DEFINE,
	    "a rule is appended");
	free(rules.entries);

	check(relic_pred_is_defined(ctx, named) &&
	    !relic_pred_is_derived(ctx, named), SCENARIO_DEFINE,
	    "a predicate a head names is defined, and not derived");
	check(relic_pred_is_defined(ctx, appended) &&
	    relic_pred_is_derived(ctx, appended), SCENARIO_DEFINE,
	    "a predicate a table's rule derives is defined and derived");
	check(relic_pred_is_defined(ctx, view), SCENARIO_DEFINE,
	    "a base view is defined");
	check(!relic_pred_is_defined(ctx, referenced), SCENARIO_DEFINE,
	    "a predicate only read is not defined");

	value.type = RELIC_INTEGER;
	value.v.integer = 1;
	check(relic_fact_add(ctx, "body", 1, &value) == 0 &&
	    relic_pred_is_defined(ctx, referenced), SCENARIO_DEFINE,
	    "a predicate with a fact is defined");

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

int
main(void)
{
	name_and_arity_identify_a_predicate();
	a_predicate_remembers_the_first_atom_reading_it();
	a_source_name_is_kept_once_and_whole();
	a_predicate_is_defined_by_what_derives_it();
	a_predicate_describes_itself_for_a_message();
	the_table_stays_exact_as_it_grows();
	no_predicate_has_more_than_32_arguments();

	return (check_summary("test_pred"));
}
