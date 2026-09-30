/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"

/*
 * The Magic Sets rewrite of a query.
 *
 * A query that binds some of its arguments needs only part of what the
 * program derives.  The rewrite specializes the program to the query: each
 * predicate the query reaches is replaced by adorned copies, one for each
 * pattern of bound arguments it is called with, and each copy's rules are
 * guarded by a magic relation holding the bound values it is actually called
 * with.  The magic relations are derived by rules of their own, which pass
 * bindings from a rule's head, through the literals evaluated before a call,
 * to the call: sideways, in the order the planner would evaluate the body.
 * What comes out is ordinary rules, which the ordinary engine evaluates.
 *
 * Not every predicate is worth adorning.  One no rule derives is read from
 * its base store as it is, which holds it whole.  One the query reaches
 * through negation is materialized in full, so that
 * absence means absence, and so is everything a fully materialized relation
 * depends on.  Those are the predicates the query reads completely.
 * relic_magic_find_complete() finds them before anything is evaluated, the
 * caller evaluates them, and relic_magic_rewrite() then orders every body with
 * the planner measuring them.  Every other predicate the query reaches is read
 * through its adornments.
 *
 * For a predicate p called with some columns bound -- written p$bf when its
 * first of two is -- and for each rule deriving p, with its body ordered from
 * the variables of the head's bound columns:
 *
 *	the adorned rule is the rule over p$bf, guarded by magic$p$bf over the
 *	head's bound arguments, with every call on a predicate read on demand
 *	renamed to the adornment that call needs;
 *
 *	each such call, on q with some columns bound, generates a magic rule:
 *	magic$q$... over the call's bound arguments, derived from the guard and
 *	every literal placed before the call;
 *
 *	before every such call after the first, the literals placed so far are
 *	cut into a supplementary predicate, sup$p$bf$<rule>$<call>, over the
 *	variables they bind that the rest of the rule reads, derived once by a
 *	rule of its own; from there the magic rules and the adorned rule read
 *	those literals through it.  Written out in full before every call, the
 *	prefixes of a body of L calls would hold the square of L in literals.
 *
 * A derived predicate with facts gives each of its adornments one more
 * rule, first, reading the facts through its base view, p$base, a predicate
 * of p's arity that reads p's base store: for p$bf,
 *
 *	p$bf(X1, X2) :- magic$p$bf(X1), p$base(X1, X2).
 *
 * The base view is complete, so the adornment reads only the facts its
 * demand reaches, looking them up by the bound columns, and the rewrite's
 * size does not depend on how many facts there are.  Base views are
 * interned before the predicate table is measured, so that each is inside
 * the snapshot of what is complete.
 *
 * An adornment with no bound column has no magic predicate and its rules no
 * guard, since there is no bound value to record.  The query is rewritten the
 * same way, with nothing bound and no guard, and the magic rules its body
 * generates are the seeds: for path(a, X)? the fact magic$path$bf(a).  The
 * query's head names no predicate, and nothing here looks it up: a head's
 * arity is read from the rule, and a limit reached while generating a rule is
 * attributed to the predicate called, never to the rule making the call.
 *
 * A free adornment subsumes the others of its predicate.  Once the query
 * reaches p with no column bound, p$ff holds every tuple of p, and a call
 * binding columns of p reads p$ff through a lookup on them rather than an
 * adornment of its own, which would hold a subset of p$ff computed beside it.
 * So the rewrite runs in two stages.  The first finds the adornments the
 * query reaches, reading through the free adornment every call on a
 * predicate called free anywhere, and drafts nothing; the second drafts the
 * rules of those it keeps.  When none of them binds a column, a rewrite
 * would derive exactly the relations of the predicates the query reaches,
 * so there is no second stage: the query reads those predicates completely,
 * and runs as written over the program's own relations.
 *
 * The rewrite terminates because adornments are memoized on the predicate and
 * the bound columns, and a predicate of arity n has at most 2^n of them.  The
 * kept adornments are the worklist: reading one adornment's rules keeps the
 * adornments its calls need, and the loop over the kept set reaches each of
 * them in turn, once.
 *
 * Generated rules are renumbered: their variables take slots in order of first
 * occurrence, head then body.  That keeps every slot of a rule bound by one of
 * its positive literals, which a magic rule would otherwise break by leaving
 * out variables of its source rule, and it gives every rule a canonical form,
 * so that rules equal up to the names of their variables compare equal and a
 * duplicate is dropped by a plain structural comparison.  A rule whose head is
 * one of its own positive literals can derive nothing new and is dropped too.
 */

/*
 * The budget of a rewrite no limit was configured for: the floor plus the
 * factor times the size of the program, in derived predicates for the
 * predicates it generates, in rules for the rules, and in the size of body
 * literals -- one each, and one more per node of an expression -- for the
 * literals of those rules.
 */
#define	RELIC_MAGIC_BUDGET_FLOOR	256
#define	RELIC_MAGIC_BUDGET_FACTOR	16

/* No component derives the predicate; no slot has been assigned yet. */
#define	RELIC_NO_COMPONENT	SIZE_MAX
#define	RELIC_NO_SLOT		UINT16_MAX

/*
 * Every magic predicate is named after its adorned predicate with this in
 * front, so one buffer holds both names: the magic name, and from past the
 * prefix the adorned one.
 */
#define	MAGIC_PREFIX		"magic$"
#define	MAGIC_PREFIX_LENGTH	(sizeof(MAGIC_PREFIX) - 1)

/*
 * A supplementary predicate is named sup$, the adorned predicate's name --
 * $q0 for the query, whose head names no predicate -- and $ and a decimal
 * ordinal twice.  ORDINALS_MAX leaves room for the two as 64-bit sizes of
 * at most 20 digits each; building the name checks that they fit.
 */
#define	SUPPLEMENTARY_PREFIX		"sup$"
#define	SUPPLEMENTARY_PREFIX_LENGTH	(sizeof(SUPPLEMENTARY_PREFIX) - 1)
#define	QUERY_ADORNED_NAME		"$q0"
#define	QUERY_ADORNED_NAME_LENGTH	(sizeof(QUERY_ADORNED_NAME) - 1)
#define	ORDINALS_MAX			(2 * (1 + 20))

typedef struct relic_closure relic_closure_t;
typedef struct relic_demand relic_demand_t;
typedef struct relic_draft relic_draft_t;
typedef struct relic_prefix relic_prefix_t;
typedef struct relic_renumbering relic_renumbering_t;

/*
 * Components being marked complete and closed over what they depend on.
 * `queue` holds those marked, in the order they were marked, and those
 * before `head` have had their rules followed to what they depend on.  A
 * component is queued at most once, the first time it is marked, which
 * bounds the queue by the schedule.
 */
struct relic_closure {
	relic_magic_t	*magic;
	size_t		*queue;
	size_t		 count;
	size_t		 head;
};

/*
 * The walk that finds what a query reads completely.
 *
 * The walk is over components of the program's schedule rather than over
 * predicates.  Every dependency inside a component is positive, since a
 * negative one would make the program unstratified, so reading one member
 * on demand reaches every other, and visiting all of a component's rules the
 * first time any member is read visits nothing the walk would not reach
 * anyway.  It keeps the walk linear in the rules: finding the rules of one
 * predicate would mean searching its component, which on a recursive
 * component of many predicates repeats that search for every one of them.
 *
 * `handled` marks the components a positive literal has already been
 * examined for, and `pending` holds those found to be read on demand whose
 * rules have not been visited yet.  `complete` collects the components the
 * walk finds complete.  Each component is pending at most once, which bounds
 * `pending`.
 */
struct relic_demand {
	relic_magic_t		*magic;
	bool			*handled;
	size_t			*pending;
	size_t			 pending_count;
	relic_closure_t		 complete;
};

/*
 * A generated rule being written, in the slots of the rule it comes from,
 * whose variable table `source` supplies.  Its arrays are borrowed, and
 * publishing it copies them renumbered.  `head_arity` is the number of
 * `head_args`: the source rule's for an adorned rule or the rewritten query,
 * the number of bound columns for a magic rule.  `attributed` is the
 * predicate of the program the rule belongs to -- the one adorned, or the one
 * whose calls a magic rule records -- which a limit diagnostic names.
 */
struct relic_draft {
	const relic_rule_t	*source;
	const relic_arg_t	*head_args;
	const relic_literal_t	*body;
	size_t			 body_length;
	relic_pred_id_t		 head;
	uint16_t		 head_arity;
	relic_pred_id_t		 attributed;
};

/*
 * The prefix of a body being rewritten: the literals evaluated so far as the
 * generated rules write them, `length` of them in `body`, which has room for
 * the whole body and a guard.  `rule` is the rule being rewritten, the
 * `rule_ordinal`th of those deriving its predicate, and `adornment` the one
 * it is rewritten for, a pseudo-adornment for the query; a cut names its
 * supplementary predicate from them.
 */
struct relic_prefix {
	const relic_rule_t	*rule;
	const relic_adornment_t	*adornment;
	size_t			 rule_ordinal;
	relic_literal_t		*body;
	size_t			 length;
};

/*
 * The slots of a rule being renumbered.  `renamed[s]` is the slot the source
 * rule's slot s takes, or RELIC_NO_SLOT until it has occurred; `original[n]`
 * is the source slot that slot n came from, and `occurrences[n]` how often it
 * occurs.  `count` slots have been assigned.
 */
struct relic_renumbering {
	uint32_t	occurrences[RELIC_MAX_VARIABLES];
	uint16_t	renamed[RELIC_MAX_VARIABLES];
	uint16_t	original[RELIC_MAX_VARIABLES];
	uint16_t	count;
};

static void	 abandon_over_budget(relic_magic_t *magic, const char *what,
		    size_t budget, relic_pred_id_t source);
static int	 add_adornment(relic_magic_t *magic,
		    relic_adornment_set_t *set, relic_pred_id_t source,
		    uint32_t bound, uint32_t hash);
static int	 adorn_rule(relic_magic_t *magic, const relic_rule_t *rule,
		    size_t rule_ordinal, const relic_adornment_t *adornment);
static int	 adorn_rules(relic_magic_t *magic,
		    const relic_adornment_t *adornment);
static uint32_t	 adornment_hash(const relic_adornment_set_t *set,
		    relic_pred_id_t source, uint32_t bound);
static int	 adornment_of_call(relic_magic_t *magic,
		    const relic_literal_t *call, uint32_t columns,
		    relic_adornment_t *adornmentp);
static void	 adornment_set_clear(relic_adornment_set_t *set);
static void	 adornment_set_init(relic_adornment_set_t *set,
		    uint64_t hash_seed);
static void	 adornment_set_release(relic_adornment_set_t *set);
static bool	 aggregates_equal(const relic_aggregate_t *a,
		    const relic_aggregate_t *b);
static int	 append_rule(relic_magic_t *magic, const relic_rule_t *rule,
		    uint32_t hash);
static uint64_t	 arg_slot(const relic_arg_t *arg);
static bool	 args_equal(const relic_arg_t *a, const relic_arg_t *b,
		    uint16_t count);
static int	 base_view_of(relic_magic_t *magic, relic_pred_id_t source,
		    relic_pred_id_t *viewp);
static int	 base_view_rule(relic_magic_t *magic, relic_pred_id_t source,
		    const relic_location_t *where, relic_rule_t *rulep);
static void	 begin_search(relic_magic_t *magic);
static int	 begin_searching(relic_magic_t *magic);
static bool	 binds_some_column(const relic_magic_t *magic);
static relic_arg_t *bound_args(relic_magic_t *magic, const relic_arg_t *args,
		    uint16_t arity, uint32_t columns);
static size_t	 budget_for(size_t program_size);
static uint32_t	 call_columns(const relic_magic_t *magic,
		    const relic_literal_t *call, uint64_t bound);
static int	 canonicalize(relic_magic_t *magic, const relic_draft_t *draft,
		    relic_rule_t *rule);
static int	 charge_literals(relic_magic_t *magic, size_t *spent,
		    size_t size, relic_pred_id_t attributed);
static int	 charge_rule_read(relic_magic_t *magic,
		    const relic_rule_t *rule, relic_pred_id_t source);
static void	 close_complete(relic_closure_t *closure);
static int	 closure_begin(relic_closure_t *closure, relic_magic_t *magic);
static uint16_t	 column_count(uint32_t columns);
static const relic_component_t *component_at(const relic_magic_t *magic,
		    size_t position);
static size_t	 component_of(const relic_magic_t *magic,
		    relic_pred_id_t pred);
static const relic_rule_t *component_rule(const relic_magic_t *magic,
		    const relic_component_t *component, size_t i);
static int	 confirm_pred_indexed(relic_magic_t *magic,
		    relic_pred_id_t source);
static int	 confirm_pred_limit(relic_magic_t *magic,
		    relic_pred_id_t source, size_t generated);
static int	 confirm_rule_limit(relic_magic_t *magic,
		    relic_pred_id_t attributed);
static relic_arg_t *copy_args(relic_magic_t *magic,
		    relic_renumbering_t *renumbering, const relic_arg_t *args,
		    uint16_t count);
static relic_expr_t *copy_expr(relic_magic_t *magic,
		    relic_renumbering_t *renumbering,
		    const relic_expr_t *source);
static int	 copy_aggregate(relic_magic_t *magic,
		    relic_renumbering_t *renumbering,
		    const relic_aggregate_t *source, relic_aggregate_t *copy);
static int	 copy_literal(relic_magic_t *magic,
		    relic_renumbering_t *renumbering,
		    const relic_literal_t *source, relic_literal_t *copy);
static int	 cut_prefix(relic_magic_t *magic, relic_prefix_t *prefix,
		    size_t call_ordinal, uint64_t live,
		    relic_pred_id_t called_pred);
static int	 demand_begin(relic_demand_t *demand, relic_magic_t *magic);
static int	 exceed(relic_magic_t *magic,
		    const relic_magic_limit_t *limit, const char *key,
		    const char *what, relic_pred_id_t source);
static const relic_adornment_t *find_adornment(
		    const relic_adornment_set_t *set, relic_pred_id_t source,
		    uint32_t bound, uint32_t hash);
static bool	 find_rule(const relic_magic_t *magic, const relic_rule_t *rule,
		    uint32_t hash);
static int	 guard_literal(relic_magic_t *magic, const relic_rule_t *rule,
		    const relic_adornment_t *head, relic_literal_t *guard);
static uint32_t	 hash_aggregate(const relic_magic_t *magic, uint32_t hash,
		    const relic_aggregate_t *aggregate);
static uint32_t	 hash_arg(const relic_magic_t *magic, uint32_t hash,
		    const relic_arg_t *arg);
static uint32_t	 hash_expr(const relic_magic_t *magic, uint32_t hash,
		    const relic_expr_t *expr);
static uint32_t	 hash_step(const relic_magic_t *magic, uint32_t hash,
		    uint32_t word);
static uint64_t	 head_bound_slots(const relic_rule_t *rule,
		    const relic_adornment_t *head);
static int	 index_rules_by_head(relic_magic_t *magic);
static int	 intern_base_views(relic_magic_t *magic);
static int	 intern_generated(relic_magic_t *magic, const char *name,
		    size_t length, uint16_t arity,
		    const relic_location_t *where, relic_pred_id_t *idp);
static int	 intern_supplementary(relic_magic_t *magic,
		    const relic_prefix_t *prefix, size_t call_ordinal,
		    uint16_t arity, relic_pred_id_t *idp);
static int	 keep_adornment(relic_magic_t *magic, relic_pred_id_t source,
		    uint32_t bound);
static bool	 kept_bound_adornment_of_free(const relic_magic_t *magic);
static uint64_t	*later_slots(relic_magic_t *magic, const relic_rule_t *rule,
		    const size_t *order);
static relic_magic_limit_t limit_or_budget(size_t configured_limit,
		    bool sized,
		    size_t program_size);
static uint64_t	 literal_slots(const relic_magic_t *magic,
		    const relic_literal_t *literal);
static bool	 literals_equal(const relic_magic_t *magic,
		    const relic_literal_t *a, const relic_literal_t *b);
static void	*magic_alloc(relic_magic_t *magic, size_t count, size_t size);
static int	 map_components(relic_magic_t *magic);
static void	 mark_body_complete(relic_closure_t *closure,
		    const relic_rule_t *rule);
static void	 mark_complete(relic_closure_t *closure, size_t position);
static int	 name_adornment(relic_magic_t *magic, relic_pred_id_t source,
		    uint32_t bound, relic_adornment_t *adornment);
static int	 name_adornments(relic_magic_t *magic);
static void	 note_incomplete(relic_magic_t *magic);
static int	 note_subsumed(relic_magic_t *magic, relic_pred_id_t source,
		    uint32_t bound);
static uint16_t	 pred_arity(const relic_magic_t *magic, relic_pred_id_t pred);
static bool	 pred_is_complete(const relic_magic_t *magic,
		    relic_pred_id_t pred);
static size_t	 program_size(const relic_ruletab_t *rules);
static int	 publish_rule(relic_magic_t *magic, const relic_draft_t *draft);
static int	 reach_adornment(relic_magic_t *magic,
		    relic_pred_id_t source, uint32_t bound);
static int	 reach_calls(relic_magic_t *magic, const relic_rule_t *rule,
		    uint64_t bound);
static int	 reach_calls_in_order(relic_magic_t *magic,
		    const relic_rule_t *rule, uint64_t bound);
static int	 reach_rules_of(relic_magic_t *magic,
		    const relic_adornment_t *adornment);
static int	 read_reached_completely(relic_magic_t *magic,
		    const relic_rule_t *query);
static bool	 reads_whole_on_demand(const relic_magic_t *magic,
		    const relic_literal_t *literal);
static bool	 reads_on_demand(const relic_magic_t *magic,
		    const relic_literal_t *literal);
static int	 record_call(relic_magic_t *magic, const relic_rule_t *rule,
		    const relic_literal_t *prefix, size_t length,
		    const relic_literal_t *call,
		    const relic_adornment_t *adornment);
static uint32_t	 rehash_adornment(uint32_t id, void *ctx);
static uint32_t	 rehash_rule(uint32_t id, void *ctx);
static void	 renumber_arg(relic_renumbering_t *renumbering,
		    const relic_arg_t *source, relic_arg_t *copy);
static uint16_t	 renumber_slot(relic_renumbering_t *renumbering,
		    uint16_t slot);
static void	 report_limit(relic_magic_t *magic, const char *limit,
		    size_t configured, relic_pred_id_t source);
static int	 rewrite_body(relic_magic_t *magic, const relic_rule_t *rule,
		    const relic_adornment_t *head, size_t rule_ordinal,
		    relic_draft_t *draft);
static int	 rewrite_query(relic_magic_t *magic,
		    const relic_rule_t *query);
static bool	 rule_derives_nothing(const relic_rule_t *rule);
static uint32_t	 rule_hash(const relic_magic_t *magic,
		    const relic_rule_t *rule);
static bool	 rules_equal(const relic_magic_t *magic, const relic_rule_t *a,
		    const relic_rule_t *b);
static int	 search_adornments(relic_magic_t *magic,
		    const relic_rule_t *query);
static int	 search_once(relic_magic_t *magic, const relic_rule_t *query);
static uint16_t	 slot_count(uint64_t slots);
static int	 supplementary_literal(relic_magic_t *magic,
		    const relic_prefix_t *prefix, size_t call_ordinal,
		    uint64_t live, relic_literal_t *literal);
static void	 visit_body(relic_demand_t *demand, const relic_rule_t *rule);
static void	 visit_demanded(relic_demand_t *demand, size_t position);

void
relic_magic_init(relic_magic_t *magic, relic_t *ctx)
{
	bzero(magic, sizeof(*magic));
	magic->ctx = ctx;
	relic_arena_init_charged(&magic->arena, &ctx->account);
	relic_arena_init_charged(&magic->scratch, &ctx->account);
	adornment_set_init(&magic->kept, ctx->hash_seed);
	adornment_set_init(&magic->subsumed, ctx->hash_seed);
	relic_idtab_init(&magic->rule_index, false);
	magic->hash_seed = ctx->hash_seed;
}

void
relic_magic_reset(relic_magic_t *magic)
{
	relic_t *ctx;

	ctx = magic->ctx;

	adornment_set_release(&magic->kept);
	adornment_set_release(&magic->subsumed);
	relic_free_array(magic->rules.entries);
	relic_idtab_reset(&magic->rule_index);
	relic_arena_reset(&magic->arena);
	relic_arena_reset(&magic->scratch);

	relic_magic_init(magic, ctx);
}

/*
 * Allocate from the rewrite's arena, reporting exhaustion.  Every caller
 * gives up at the first refusal, so the report is made once.
 */
static void *
magic_alloc(relic_magic_t *magic, size_t count, size_t size)
{
	void *block;

	block = relic_arena_allocarray(&magic->arena, count, size);
	if (block == NULL)
		relic_diag_no_memory(magic->ctx);

	return (block);
}

static uint16_t
pred_arity(const relic_magic_t *magic, relic_pred_id_t pred)
{
	const relic_pred_t *entry;

	entry = relic_pred_of(magic->ctx, pred);

	return (entry == NULL ? 0 : entry->arity);
}

static uint16_t
column_count(uint32_t columns)
{
	uint16_t count;

	count = 0;
	for (; columns != 0; columns &= columns - 1)
		count++;

	return (count);
}

/*
 * The position in the program's schedule of the component deriving a
 * predicate, or RELIC_NO_COMPONENT when no rule of the program does.  A
 * predicate this rewrite generated was interned after the table was sized
 * and is past its end; one an earlier query's rewrite generated is within it
 * and derived by no rule of the program.  Neither has a component.
 */
static size_t
component_of(const relic_magic_t *magic, relic_pred_id_t pred)
{
	if (pred - 1 >= magic->predicate_count)
		return (RELIC_NO_COMPONENT);

	return (magic->component_of[pred - 1]);
}

/*
 * Whether the query reads a predicate completely.  A predicate this rewrite
 * generated is past the end of the table and never is; one an earlier
 * query's rewrite generated has no rule of the program deriving it, so it is
 * complete and empty, as any predicate no rule derives is.
 */
static bool
pred_is_complete(const relic_magic_t *magic, relic_pred_id_t pred)
{
	return (pred - 1 < magic->predicate_count &&
	    !magic->incomplete[pred - 1]);
}

static const relic_component_t *
component_at(const relic_magic_t *magic, size_t position)
{
	return (&magic->ctx->program.schedule.components[position]);
}

/*
 * The `i`th rule of a component of the program's schedule, in program order.
 */
static const relic_rule_t *
component_rule(const relic_magic_t *magic, const relic_component_t *component,
    size_t i)
{
	const relic_program_t *program;

	program = &magic->ctx->program;

	return (&program->rules.entries[
	    program->schedule.rule_indexes[component->first_rule + i]]);
}

/*
 * Record, for each predicate, the position in the program's schedule of the
 * component deriving it, or RELIC_NO_COMPONENT when no rule does.
 */
static int
map_components(relic_magic_t *magic)
{
	const relic_schedule_t *schedule;
	const relic_component_t *component;
	size_t position;
	size_t m;
	size_t p;

	schedule = &magic->ctx->program.schedule;

	magic->component_of = magic_alloc(magic, magic->predicate_count,
	    sizeof(*magic->component_of));
	if (magic->component_of == NULL)
		return (-1);

	for (p = 0; p < magic->predicate_count; p++)
		magic->component_of[p] = RELIC_NO_COMPONENT;

	for (position = 0; position < schedule->component_count; position++) {
		component = &schedule->components[position];

		for (m = 0; m < component->member_count; m++)
			magic->component_of[schedule->members[
			    component->first_member + m] - 1] = position;
	}

	return (0);
}

/*
 * Group the program's rules by head, so that an adornment finds the rules of
 * its predicate without searching its component for them.  A counting sort
 * over the rules in program order keeps each head's rules in program order.
 */
static int
index_rules_by_head(relic_magic_t *magic)
{
	const relic_ruletab_t *rules;
	size_t *next;
	size_t head;
	size_t p;
	size_t r;

	rules = &magic->ctx->program.rules;

	magic->rules_by_head = magic_alloc(magic, rules->count,
	    sizeof(*magic->rules_by_head));
	if (magic->rules_by_head == NULL)
		return (-1);
	magic->first_rule_of = magic_alloc(magic, magic->predicate_count + 1,
	    sizeof(*magic->first_rule_of));
	if (magic->first_rule_of == NULL)
		return (-1);
	next = magic_alloc(magic, magic->predicate_count, sizeof(*next));
	if (next == NULL)
		return (-1);

	bzero(magic->first_rule_of, (magic->predicate_count + 1) *
	    sizeof(*magic->first_rule_of));
	for (r = 0; r < rules->count; r++)
		magic->first_rule_of[rules->entries[r].head]++;

	for (p = 0; p < magic->predicate_count; p++) {
		magic->first_rule_of[p + 1] += magic->first_rule_of[p];
		next[p] = magic->first_rule_of[p];
	}

	for (r = 0; r < rules->count; r++) {
		head = rules->entries[r].head - 1;
		magic->rules_by_head[next[head]++] = r;
	}

	return (0);
}

/*
 * Room for the walk and for its result, every mark clear.
 */
static int
demand_begin(relic_demand_t *demand, relic_magic_t *magic)
{
	size_t components;

	bzero(demand, sizeof(*demand));
	demand->magic = magic;
	components = magic->ctx->program.schedule.component_count;

	magic->incomplete = magic_alloc(magic, magic->predicate_count,
	    sizeof(*magic->incomplete));
	if (magic->incomplete == NULL)
		return (-1);
	magic->complete_components = magic_alloc(magic, components,
	    sizeof(*magic->complete_components));
	if (magic->complete_components == NULL)
		return (-1);
	demand->handled = magic_alloc(magic, components,
	    sizeof(*demand->handled));
	if (demand->handled == NULL)
		return (-1);
	demand->pending = magic_alloc(magic, components,
	    sizeof(*demand->pending));
	if (demand->pending == NULL)
		return (-1);
	if (closure_begin(&demand->complete, magic) != 0)
		return (-1);

	bzero(magic->complete_components, components *
	    sizeof(*magic->complete_components));
	bzero(demand->handled, components * sizeof(*demand->handled));

	return (0);
}

/*
 * Room for a closure over every component of the schedule, none queued.
 */
static int
closure_begin(relic_closure_t *closure, relic_magic_t *magic)
{
	bzero(closure, sizeof(*closure));
	closure->magic = magic;
	closure->queue = magic_alloc(magic,
	    magic->ctx->program.schedule.component_count,
	    sizeof(*closure->queue));

	return (closure->queue == NULL ? -1 : 0);
}

/*
 * Mark a component complete, queueing it so that what it depends on is
 * marked too.
 */
static void
mark_complete(relic_closure_t *closure, size_t position)
{
	if (closure->magic->complete_components[position])
		return;

	closure->magic->complete_components[position] = true;
	closure->queue[closure->count] = position;
	closure->count++;
}

/*
 * Mark complete the components of every predicate a rule's body reads,
 * whatever it reads them for.
 */
static void
mark_body_complete(relic_closure_t *closure, const relic_rule_t *rule)
{
	relic_pred_id_t pred;
	size_t position;
	size_t i;

	for (i = 0; i < rule->body_length; i++) {
		if (!relic_literal_reads(&rule->body[i], &pred))
			continue;

		position = component_of(closure->magic, pred);
		if (position != RELIC_NO_COMPONENT)
			mark_complete(closure, position);
	}
}

/*
 * Examine the literals of a body the query reaches that read a relation.  A
 * negated literal's predicate is complete, and so is an aggregate's group
 * predicate, with everything the body of its group rule reads: a fold needs
 * the whole group.  A query's group predicate is in no component, since the
 * query runs its group rule itself, so what its body reads is marked
 * directly; a program's is, and its body is reached by closing the complete
 * set as well.  A positive literal's predicate is complete when no rule
 * derives it, and otherwise read on demand, and the rules of its component
 * are visited in turn -- once, however many literals call it or its fellow
 * members.
 *
 * A predicate no rule derives is complete and empty, and there is nothing to
 * evaluate for it.  A predicate already complete needs no visit: whatever its
 * rules reach, it depends on, and closing the complete set will mark it.
 */
static void
visit_body(relic_demand_t *demand, const relic_rule_t *rule)
{
	relic_magic_t *magic;
	const relic_literal_t *literal;
	relic_pred_id_t pred;
	size_t position;
	size_t i;

	magic = demand->magic;

	for (i = 0; i < rule->body_length; i++) {
		literal = &rule->body[i];
		if (!relic_literal_reads(literal, &pred))
			continue;

		if (literal->kind == RELIC_LITERAL_AGGREGATE)
			mark_body_complete(&demand->complete,
			    literal->u.aggregate.group_rule);

		position = component_of(magic, pred);
		if (position == RELIC_NO_COMPONENT)
			continue;

		if (literal->kind != RELIC_LITERAL_POSITIVE) {
			mark_complete(&demand->complete, position);
			continue;
		}

		if (demand->handled[position])
			continue;
		demand->handled[position] = true;

		if (magic->complete_components[position])
			continue;

		demand->pending[demand->pending_count] = position;
		demand->pending_count++;
	}
}

/*
 * Visit the body of every rule of a component read on demand.
 */
static void
visit_demanded(relic_demand_t *demand, size_t position)
{
	const relic_component_t *component;
	size_t i;

	component = component_at(demand->magic, position);

	for (i = 0; i < component->rule_count; i++)
		visit_body(demand, component_rule(demand->magic, component, i));
}

/*
 * Mark complete everything a complete component depends on, positively,
 * negatively or through an aggregate: a relation is complete only if what
 * it is derived from is.
 * Each component is queued when first marked and its rules followed once.
 */
static void
close_complete(relic_closure_t *closure)
{
	const relic_magic_t *magic;
	const relic_component_t *component;
	size_t i;

	magic = closure->magic;

	while (closure->head < closure->count) {
		component = component_at(magic, closure->queue[closure->head]);
		closure->head++;

		for (i = 0; i < component->rule_count; i++)
			mark_body_complete(closure,
			    component_rule(magic, component, i));
	}
}

/*
 * Find what the query reads completely: walk from its body through every
 * predicate read on demand, collecting the complete components, then close
 * that set over what its members depend on.  A predicate is complete when its
 * component is, or when no rule derives it; a base view is derived by none.
 *
 * The walk may visit the rules of a predicate that the closure later finds
 * complete.  That adds nothing wrong: whatever it collects beyond such a
 * predicate is something the predicate depends on, and complete already.
 */
int
relic_magic_find_complete(relic_magic_t *magic, const relic_query_t *query)
{
	relic_demand_t demand;
	size_t position;

	if (intern_base_views(magic) != 0)
		return (-1);

	magic->predicate_count = relic_pred_count(magic->ctx);

	if (map_components(magic) != 0)
		return (-1);

	if (index_rules_by_head(magic) != 0)
		return (-1);

	if (demand_begin(&demand, magic) != 0)
		return (-1);

	visit_body(&demand, &query->rule);
	while (demand.pending_count > 0) {
		demand.pending_count--;
		position = demand.pending[demand.pending_count];
		visit_demanded(&demand, position);
	}

	close_complete(&demand.complete);
	note_incomplete(magic);

	return (0);
}

/*
 * Mark as read on demand every predicate a rule derives whose component is
 * not complete.
 */
static void
note_incomplete(relic_magic_t *magic)
{
	size_t position;
	size_t p;

	for (p = 0; p < magic->predicate_count; p++) {
		position = magic->component_of[p];
		magic->incomplete[p] = position != RELIC_NO_COMPONENT &&
		    !magic->complete_components[position];
	}
}

static void
report_limit(relic_magic_t *magic, const char *limit, size_t configured,
    relic_pred_id_t source)
{
	char described[RELIC_PRED_TEXT_MAX];

	relic_diagf(magic->ctx, RELIC_ERROR, NULL,
	    "limit exceeded: %s=%zu while adorning %s", limit, configured,
	    relic_pred_describe(magic->ctx, source, described,
	    sizeof(described)));
}

/*
 * Abandon the rewrite, which outgrew its budget while adorning `source`:
 * warn, and mark it so that its caller answers the query another way.
 */
static void
abandon_over_budget(relic_magic_t *magic, const char *what, size_t budget,
    relic_pred_id_t source)
{
	char described[RELIC_PRED_TEXT_MAX];

	relic_diagf(magic->ctx, RELIC_WARNING, NULL,
	    "the query's Magic rewrite outgrew its budget of %zu %s while "
	    "adorning %s; answering it from the whole program", budget, what,
	    relic_pred_describe(magic->ctx, source, described,
	    sizeof(described)));

	magic->over_budget = true;
}

/*
 * The budget for a program of `program_size`: large enough for every rewrite
 * that specializes the program rather than multiplies it, and small enough
 * that the rewritten program never has more than a constant multiple of the
 * program's size, however many adornments the program's arities allow.
 * Saturates rather than wrapping.
 */
static size_t
budget_for(size_t program_size)
{
	if (program_size > (SIZE_MAX - RELIC_MAGIC_BUDGET_FLOOR) /
	    RELIC_MAGIC_BUDGET_FACTOR)
		return (SIZE_MAX);

	return (RELIC_MAGIC_BUDGET_FLOOR +
	    RELIC_MAGIC_BUDGET_FACTOR * program_size);
}

/*
 * The bound on the predicates a rewrite generates.  A caller who configures
 * either Magic limit has sized the rewrite, and `sized` says so: the count
 * is then bounded by the limit they set for it, `configured_limit`, or not
 * at all if they left it zero.  A rewrite nobody has sized gets the budget
 * for a program of `program_size`.
 */
static relic_magic_limit_t
limit_or_budget(size_t configured_limit, bool sized, size_t program_size)
{
	relic_magic_limit_t limit;

	limit.configured = sized;
	if (!sized)
		limit.most = budget_for(program_size);
	else if (configured_limit != 0)
		limit.most = configured_limit;
	else
		limit.most = SIZE_MAX;

	return (limit);
}

/*
 * The size of the program's rules, which with the rules over base views the
 * literal budget is proportional to: what one adorned copy of them holds,
 * each rule's body plus one for its guard, a literal counting one and one
 * more per node of its expressions.  A rule over a base view,
 * p(X1, ...) :- p$base(X1, ...), counts two.
 */
static size_t
program_size(const relic_ruletab_t *rules)
{
	const relic_rule_t *rule;
	size_t size;
	size_t r;

	size = 0;
	for (r = 0; r < rules->count; r++) {
		rule = &rules->entries[r];
		size = relic_saturating_sum(size, relic_saturating_sum(1,
		    relic_body_size(rule->body, rule->body_length)));
	}

	return (size);
}

/*
 * Report that `source`'s adornment would take the rewrite past a limit or a
 * budget, in the way each is reported, and return -1.
 */
static int
exceed(relic_magic_t *magic, const relic_magic_limit_t *limit,
    const char *key, const char *what, relic_pred_id_t source)
{
	if (limit->configured)
		report_limit(magic, key, limit->most, source);
	else
		abandon_over_budget(magic, what, limit->most, source);

	return (-1);
}

/*
 * Confirm that `generated` more predicates stay within what the rewrite may
 * generate, reporting the predicate being adorned when they would not.
 */
static int
confirm_pred_limit(relic_magic_t *magic, relic_pred_id_t source,
    size_t generated)
{
	if (magic->generated_preds + generated <= magic->pred_limit.most)
		return (0);

	return (exceed(magic, &magic->pred_limit, "magic-preds",
	    "generated predicates", source));
}

/*
 * Confirm that one more generated rule stays within what the rewrite may
 * generate.
 */
static int
confirm_rule_limit(relic_magic_t *magic, relic_pred_id_t attributed)
{
	if (magic->rules.count < magic->rule_limit.most)
		return (0);

	return (exceed(magic, &magic->rule_limit, "magic-rules",
	    "generated rules", attributed));
}

/*
 * Add literals of `size` to the count `spent` of the literal budget, unless
 * that would take it past the budget.  A literal's size is one and one more
 * per node of its expressions, so a rewrite copying a large expression into
 * many prefixes is charged for the nodes it copies and walks, and the
 * budget bounds that work as it bounds the literals.  Outgrowing it abandons
 * the rewrite:
 * the budget exists only for a rewrite nobody has sized.  The count never
 * passes the budget, since only what this admits is added to it, so the
 * subtraction cannot wrap where the sum could.
 */
static int
charge_literals(relic_magic_t *magic, size_t *spent, size_t size,
    relic_pred_id_t attributed)
{
	assert(*spent <= magic->literal_budget);

	if (size > magic->literal_budget - *spent) {
		abandon_over_budget(magic, "generated literals",
		    magic->literal_budget, attributed);
		return (-1);
	}

	*spent += size;

	return (0);
}

/*
 * Intern a generated predicate's name and the predicate, which is defined
 * from the start: it is the head of a rule the rewrite generates, and the
 * undefined-predicate warning must never name it.
 */
static int
intern_generated(relic_magic_t *magic, const char *name, size_t length,
    uint16_t arity, const relic_location_t *where, relic_pred_id_t *idp)
{
	uint32_t symbol;

	if (relic_symbol_intern(magic->ctx, name, length, &symbol) != 0)
		return (-1);

	if (relic_pred_intern(magic->ctx, symbol, arity, where, idp) != 0)
		return (-1);

	relic_pred_name_head(magic->ctx, *idp);

	return (0);
}

/*
 * Intern the base view of every derived predicate whose base store holds
 * facts, counting them.  Only the predicates interned before this began are
 * looked at, which every base view is among: a base view is derived by
 * nothing, and interning one is what extends the table.
 */
static int
intern_base_views(relic_magic_t *magic)
{
	relic_pred_id_t view;
	size_t count;
	size_t p;

	count = relic_pred_count(magic->ctx);
	magic->base_view_count = 0;

	for (p = 1; p <= count; p++) {
		if (!relic_pred_is_derived(magic->ctx, (relic_pred_id_t)p) ||
		    relic_base_count(magic->ctx, (relic_pred_id_t)p) == 0)
			continue;

		if (base_view_of(magic, (relic_pred_id_t)p, &view) != 0)
			return (-1);
		magic->base_view_count++;
	}

	return (0);
}

/*
 * The base view of `source` in *viewp: `name$base`, of source's arity,
 * reading source's base store.  It is interned the first time it is asked
 * for and found afterwards.  No adornment's name ends in `base`, since an
 * adornment's suffix holds only b and f.
 */
static int
base_view_of(relic_magic_t *magic, relic_pred_id_t source,
    relic_pred_id_t *viewp)
{
	static const char suffix[] = "$base";
	const relic_pred_t *entry;
	const char *name;
	char *text;
	size_t name_length;
	uint32_t symbol;
	uint16_t arity;

	entry = relic_pred_of(magic->ctx, source);
	name = relic_symbol_bytes(magic->ctx, entry->name, &name_length);
	arity = entry->arity;

	text = magic_alloc(magic, name_length + sizeof(suffix) - 1, 1);
	if (text == NULL)
		return (-1);
	bcopy(name, text, name_length);
	bcopy(suffix, text + name_length, sizeof(suffix) - 1);

	if (relic_symbol_intern(magic->ctx, text,
	    name_length + sizeof(suffix) - 1, &symbol) != 0)
		return (-1);

	if (relic_pred_intern(magic->ctx, symbol, arity, NULL, viewp) != 0)
		return (-1);

	relic_pred_make_view(magic->ctx, *viewp, source);

	return (0);
}

/*
 * Build in *rulep the rule reading `source`'s facts through its base view,
 *
 *	source(X1, ..., Xn) :- source$base(X1, ..., Xn).
 *
 * for adorn_rule() to rewrite like any rule of the program, located at
 * `where`, the first rule deriving `source`.  Its variables are named X1 to
 * Xn, for explain to show, and each occurs twice, in the head and in the
 * body, as a checked rule's would.  The base view was interned by
 * intern_base_views() before the predicates were counted, so it is found
 * here, among the predicates the rewrite knows complete.
 */
static int
base_view_rule(relic_magic_t *magic, relic_pred_id_t source,
    const relic_location_t *where, relic_rule_t *rulep)
{
	relic_literal_t *literal;
	relic_arg_t *args;
	relic_variable_t *variables;
	relic_pred_id_t view;
	char name[16];
	uint16_t arity;
	uint16_t column;

	if (base_view_of(magic, source, &view) != 0)
		return (-1);
	assert(view <= magic->predicate_count);

	arity = relic_pred_of(magic->ctx, source)->arity;
	literal = magic_alloc(magic, 1, sizeof(*literal));
	if (literal == NULL)
		return (-1);
	args = NULL;
	variables = NULL;
	if (arity > 0) {
		args = magic_alloc(magic, arity, sizeof(*args));
		variables = magic_alloc(magic, arity, sizeof(*variables));
		if (args == NULL || variables == NULL)
			return (-1);
	}

	for (column = 0; column < arity; column++) {
		bzero(&args[column], sizeof(args[column]));
		args[column].kind = RELIC_ARG_VARIABLE;
		args[column].u.slot = column;

		bzero(&variables[column], sizeof(variables[column]));
		snprintf(name, sizeof(name), "X%u", (unsigned int)column + 1);
		if (relic_symbol_intern(magic->ctx, name, strlen(name),
		    &variables[column].name) != 0)
			return (-1);
		variables[column].where = *where;
		variables[column].occurrences = 2;
	}

	bzero(literal, sizeof(*literal));
	literal->kind = RELIC_LITERAL_POSITIVE;
	literal->where = *where;
	literal->u.atom.args = args;
	literal->u.atom.where = *where;
	literal->u.atom.pred = view;

	bzero(rulep, sizeof(*rulep));
	rulep->head_args = args;
	rulep->body = literal;
	rulep->variables = variables;
	rulep->body_length = 1;
	rulep->origin = RELIC_ORIGIN_QUERY;
	rulep->where = *where;
	rulep->head = source;
	rulep->head_arity = arity;
	rulep->variable_count = arity;

	return (0);
}

/*
 * Name and intern the predicates of an adornment: `name$` followed by a b or
 * an f per column for the adorned predicate, and the same after `magic$` for
 * the magic one, which holds only the bound columns and exists only when
 * there is one.
 *
 * What the source predicate's entry says is copied first, because interning
 * can grow the predicate table and move the entry.
 */
static int
name_adornment(relic_magic_t *magic, relic_pred_id_t source, uint32_t bound,
    relic_adornment_t *adornment)
{
	const relic_pred_t *entry;
	const char *name;
	char *text;
	size_t name_length;
	size_t length;
	uint16_t arity;
	uint16_t column;

	entry = relic_pred_of(magic->ctx, source);
	name = relic_symbol_bytes(magic->ctx, entry->name, &name_length);
	arity = entry->arity;

	length = MAGIC_PREFIX_LENGTH + name_length + 1 + arity;
	text = magic_alloc(magic, length, 1);
	if (text == NULL)
		return (-1);

	bcopy(MAGIC_PREFIX, text, MAGIC_PREFIX_LENGTH);
	bcopy(name, text + MAGIC_PREFIX_LENGTH, name_length);
	text[MAGIC_PREFIX_LENGTH + name_length] = '$';
	for (column = 0; column < arity; column++)
		text[MAGIC_PREFIX_LENGTH + name_length + 1 + column] =
		    (bound & (UINT32_C(1) << column)) != 0 ? 'b' : 'f';

	adornment->source = source;
	adornment->bound = bound;
	adornment->magic = RELIC_ID_NONE;

	if (intern_generated(magic, text + MAGIC_PREFIX_LENGTH,
	    length - MAGIC_PREFIX_LENGTH, arity, NULL,
	    &adornment->adorned) != 0)
		return (-1);

	if (bound == 0)
		return (0);

	return (intern_generated(magic, text, length, column_count(bound),
	    NULL, &adornment->magic));
}

/*
 * An empty set of adornments, hashing under `hash_seed`.
 */
static void
adornment_set_init(relic_adornment_set_t *set, uint64_t hash_seed)
{
	bzero(set, sizeof(*set));
	relic_idtab_init(&set->index, false);
	set->hash_seed = hash_seed;
}

/*
 * Forget every adornment of a set and release its index, keeping the array
 * of entries for the next search.
 */
static void
adornment_set_clear(relic_adornment_set_t *set)
{
	set->count = 0;
	relic_idtab_reset(&set->index);
}

static void
adornment_set_release(relic_adornment_set_t *set)
{
	relic_free_array(set->entries);
	relic_idtab_reset(&set->index);
	adornment_set_init(set, set->hash_seed);
}

static uint32_t
adornment_hash(const relic_adornment_set_t *set, relic_pred_id_t source,
    uint32_t bound)
{
	uint32_t key[2];

	key[0] = source;
	key[1] = bound;

	return (relic_hash_words(set->hash_seed, key, 2));
}

static uint32_t
rehash_adornment(uint32_t id, void *ctx)
{
	const relic_adornment_t *adornment;
	const relic_adornment_set_t *set;

	set = ctx;
	adornment = &set->entries[id - 1];

	return (adornment_hash(set, adornment->source, adornment->bound));
}

/*
 * The adornment of `set` for calling `source` with the columns of `bound`
 * bound, whose hash is `hash`, or NULL when none was recorded.
 */
static const relic_adornment_t *
find_adornment(const relic_adornment_set_t *set, relic_pred_id_t source,
    uint32_t bound, uint32_t hash)
{
	const relic_adornment_t *candidate;
	relic_idprobe_t probe;
	uint32_t id;

	relic_idprobe_open(&probe, &set->index, hash);
	while (relic_idprobe_next(&probe, &id)) {
		candidate = &set->entries[id - 1];
		if (candidate->source == source && candidate->bound == bound)
			return (candidate);
	}

	return (NULL);
}

/*
 * Record an adornment at the end of `set`, with no predicates named yet and
 * the position the search has reached among the adornments it keeps.  Room
 * in the array and in the index is made before anything is recorded, so
 * that running out of memory leaves no half-recorded adornment.
 */
static int
add_adornment(relic_magic_t *magic, relic_adornment_set_t *set,
    relic_pred_id_t source, uint32_t bound, uint32_t hash)
{
	relic_adornment_t *entries;
	relic_adornment_t *recorded;

	if (set->count >= UINT32_MAX - 1) {
		relic_diagf(magic->ctx, RELIC_ERROR, NULL,
		    "a query's Magic rewrite generated more adornments than it "
		    "can index");
		return (-1);
	}

	entries = relic_grow_array(set->entries, &set->capacity,
	    set->count + 1, sizeof(*set->entries));
	if (entries == NULL) {
		relic_diag_no_memory(magic->ctx);
		return (-1);
	}
	set->entries = entries;

	if (relic_idtab_reserve(&set->index, 1, rehash_adornment, set) != 0) {
		relic_diag_no_memory(magic->ctx);
		return (-1);
	}

	recorded = &set->entries[set->count];
	bzero(recorded, sizeof(*recorded));
	recorded->source = source;
	recorded->adorned = RELIC_ID_NONE;
	recorded->magic = RELIC_ID_NONE;
	recorded->bound = bound;
	recorded->kept_before = magic->kept.count;

	set->count++;
	relic_idtab_place(&set->index, hash, (uint32_t)set->count);

	return (0);
}

/*
 * Keep the adornment calling `source` with the columns of `bound` bound,
 * unless the search has kept it already.  A new one counts the predicates
 * naming it will generate -- the adorned one, and the magic one when a
 * column is bound -- against the limit on generated predicates.
 */
static int
keep_adornment(relic_magic_t *magic, relic_pred_id_t source, uint32_t bound)
{
	size_t generated;
	uint32_t hash;

	hash = adornment_hash(&magic->kept, source, bound);
	if (find_adornment(&magic->kept, source, bound, hash) != NULL)
		return (0);

	generated = bound == 0 ? 1 : 2;
	if (confirm_pred_limit(magic, source, generated) != 0)
		return (-1);

	if (add_adornment(magic, &magic->kept, source, bound, hash) != 0)
		return (-1);
	magic->generated_preds += generated;

	return (0);
}

/*
 * Note that a call binding the columns of `bound` reads `source` through its
 * free adornment, unless the search has noted it already.  It generates no
 * predicate and counts against no limit.
 */
static int
note_subsumed(relic_magic_t *magic, relic_pred_id_t source, uint32_t bound)
{
	uint32_t hash;

	hash = adornment_hash(&magic->subsumed, source, bound);
	if (find_adornment(&magic->subsumed, source, bound, hash) != NULL)
		return (0);

	return (add_adornment(magic, &magic->subsumed, source, bound, hash));
}

/*
 * Name and intern the predicates of every adornment the query reaches and
 * keeps, in the order they were recorded.  Their count was confirmed against
 * the limit when they were recorded.
 */
static int
name_adornments(relic_magic_t *magic)
{
	relic_adornment_t *adornment;
	size_t i;

	for (i = 0; i < magic->kept.count; i++) {
		adornment = &magic->kept.entries[i];
		if (name_adornment(magic, adornment->source, adornment->bound,
		    adornment) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Give an occurrence of a source slot its slot in the generated rule,
 * assigning the next one the first time the slot occurs.
 */
static uint16_t
renumber_slot(relic_renumbering_t *renumbering, uint16_t slot)
{
	uint16_t renamed;

	if (renumbering->renamed[slot] == RELIC_NO_SLOT) {
		renumbering->renamed[slot] = renumbering->count;
		renumbering->original[renumbering->count] = slot;
		renumbering->occurrences[renumbering->count] = 0;
		renumbering->count++;
	}

	renamed = renumbering->renamed[slot];
	renumbering->occurrences[renamed]++;

	return (renamed);
}

static void
renumber_arg(relic_renumbering_t *renumbering, const relic_arg_t *source,
    relic_arg_t *copy)
{
	*copy = *source;
	if (source->kind == RELIC_ARG_VARIABLE)
		copy->u.slot = renumber_slot(renumbering, source->u.slot);
}

/*
 * A renumbered copy of an expression tree, its variables renumbered in the
 * order they are written, left operand before right and a call's arguments
 * in order.  Returns NULL when the
 * arena refused, which has been reported.  The tree is no taller than the
 * parser allowed, so the recursion is bounded.
 */
static relic_expr_t *
copy_expr(relic_magic_t *magic, relic_renumbering_t *renumbering,
    const relic_expr_t *source)
{
	relic_expr_t *copy;
	uint8_t i;

	copy = magic_alloc(magic, 1, sizeof(*copy));
	if (copy == NULL)
		return (NULL);
	*copy = *source;

	switch (source->kind) {
	case RELIC_EXPR_CONSTANT:
		break;
	case RELIC_EXPR_VARIABLE:
		copy->u.slot = renumber_slot(renumbering, source->u.slot);
		break;
	case RELIC_EXPR_NEGATE:
		copy->u.operand = copy_expr(magic, renumbering,
		    source->u.operand);
		if (copy->u.operand == NULL)
			return (NULL);
		break;
	case RELIC_EXPR_BINARY:
		copy->u.binary.left = copy_expr(magic, renumbering,
		    source->u.binary.left);
		if (copy->u.binary.left == NULL)
			return (NULL);
		copy->u.binary.right = copy_expr(magic, renumbering,
		    source->u.binary.right);
		if (copy->u.binary.right == NULL)
			return (NULL);
		break;
	case RELIC_EXPR_CALL:
		for (i = 0; i < source->u.call.argc; i++) {
			copy->u.call.args[i] = copy_expr(magic, renumbering,
			    source->u.call.args[i]);
			if (copy->u.call.args[i] == NULL)
				return (NULL);
		}
		break;
	}

	return (copy);
}

/*
 * A renumbered copy of a run of arguments, or NULL for none, as a rule of
 * arity zero holds no head arguments.  A null return for a nonzero count
 * means the arena refused, which has been reported.
 */
static relic_arg_t *
copy_args(relic_magic_t *magic, relic_renumbering_t *renumbering,
    const relic_arg_t *args, uint16_t count)
{
	relic_arg_t *copy;
	uint16_t i;

	if (count == 0)
		return (NULL);

	copy = magic_alloc(magic, count, sizeof(*copy));
	if (copy == NULL)
		return (NULL);

	for (i = 0; i < count; i++)
		renumber_arg(renumbering, &args[i], &copy[i]);

	return (copy);
}

/*
 * A renumbered copy of an aggregate literal: its result slot, then its
 * shared slots, in the rule being generated.  The group predicate and the
 * group rule are the source's, since a group is read completely and never
 * rewritten.
 */
static int
copy_aggregate(relic_magic_t *magic, relic_renumbering_t *renumbering,
    const relic_aggregate_t *source, relic_aggregate_t *copy)
{
	uint16_t *shared;
	uint16_t i;

	*copy = *source;
	copy->result = renumber_slot(renumbering, source->result);

	if (source->shared_count == 0)
		return (0);

	shared = magic_alloc(magic, source->shared_count, sizeof(*shared));
	if (shared == NULL)
		return (-1);
	for (i = 0; i < source->shared_count; i++)
		shared[i] = renumber_slot(renumbering, source->shared[i]);
	copy->shared = shared;

	return (0);
}

static int
copy_literal(relic_magic_t *magic, relic_renumbering_t *renumbering,
    const relic_literal_t *source, relic_literal_t *copy)
{
	uint16_t arity;

	*copy = *source;

	if (source->kind == RELIC_LITERAL_AGGREGATE)
		return (copy_aggregate(magic, renumbering, &source->u.aggregate,
		    &copy->u.aggregate));

	if (source->kind == RELIC_LITERAL_COMPARISON) {
		copy->u.comparison.left = copy_expr(magic, renumbering,
		    source->u.comparison.left);
		if (copy->u.comparison.left == NULL)
			return (-1);
		copy->u.comparison.right = copy_expr(magic, renumbering,
		    source->u.comparison.right);
		if (copy->u.comparison.right == NULL)
			return (-1);
		return (0);
	}

	arity = pred_arity(magic, source->u.atom.pred);
	copy->u.atom.args = copy_args(magic, renumbering, source->u.atom.args,
	    arity);
	if (arity > 0 && copy->u.atom.args == NULL)
		return (-1);

	return (0);
}

/*
 * Copy a draft into a compiled rule whose variables are renumbered in order of
 * first occurrence, head then body, each keeping the name and first location
 * of the variable it came from.  The copy is what the rewrite publishes, and
 * its form is canonical: two drafts equal up to the names of their variables
 * give equal copies.
 */
static int
canonicalize(relic_magic_t *magic, const relic_draft_t *draft,
    relic_rule_t *rule)
{
	relic_renumbering_t renumbering;
	relic_variable_t *variable;
	uint16_t n;
	size_t i;

	bzero(rule, sizeof(*rule));
	for (i = 0; i < RELIC_MAX_VARIABLES; i++)
		renumbering.renamed[i] = RELIC_NO_SLOT;
	renumbering.count = 0;

	rule->head = draft->head;
	rule->head_arity = draft->head_arity;
	rule->where = draft->source->where;
	rule->origin = draft->source->origin;

	rule->head_args = copy_args(magic, &renumbering, draft->head_args,
	    rule->head_arity);
	if (rule->head_arity > 0 && rule->head_args == NULL)
		return (-1);

	rule->body = magic_alloc(magic, draft->body_length,
	    sizeof(*rule->body));
	if (rule->body == NULL)
		return (-1);
	for (i = 0; i < draft->body_length; i++) {
		if (copy_literal(magic, &renumbering, &draft->body[i],
		    &rule->body[i]) != 0)
			return (-1);
	}
	rule->body_length = draft->body_length;

	rule->variables = magic_alloc(magic, renumbering.count,
	    sizeof(*rule->variables));
	if (rule->variables == NULL)
		return (-1);
	for (n = 0; n < renumbering.count; n++) {
		variable = &rule->variables[n];
		*variable = draft->source->variables[renumbering.original[n]];
		variable->occurrences = renumbering.occurrences[n];
	}
	rule->variable_count = renumbering.count;

	return (0);
}

static bool
args_equal(const relic_arg_t *a, const relic_arg_t *b, uint16_t count)
{
	uint16_t i;

	for (i = 0; i < count; i++) {
		if (a[i].kind != b[i].kind)
			return (false);
		if (a[i].kind == RELIC_ARG_CONSTANT &&
		    a[i].u.constant != b[i].u.constant)
			return (false);
		if (a[i].kind == RELIC_ARG_VARIABLE &&
		    a[i].u.slot != b[i].u.slot)
			return (false);
	}

	return (true);
}

/*
 * Whether two aggregate literals of canonical rules are the same literal:
 * the same function of the same column of the same group, binding or
 * comparing the same slot for the same key.
 */
static bool
aggregates_equal(const relic_aggregate_t *a, const relic_aggregate_t *b)
{
	uint16_t i;

	if (a->fn != b->fn || a->group != b->group || a->column != b->column ||
	    a->result != b->result || a->shared_count != b->shared_count)
		return (false);

	for (i = 0; i < a->shared_count; i++) {
		if (a->shared[i] != b->shared[i])
			return (false);
	}

	return (true);
}

static bool
literals_equal(const relic_magic_t *magic, const relic_literal_t *a,
    const relic_literal_t *b)
{
	if (a->kind != b->kind)
		return (false);

	if (a->kind == RELIC_LITERAL_AGGREGATE)
		return (aggregates_equal(&a->u.aggregate, &b->u.aggregate));

	if (a->kind == RELIC_LITERAL_COMPARISON)
		return (a->u.comparison.op == b->u.comparison.op &&
		    relic_expr_equal(a->u.comparison.left,
		    b->u.comparison.left) &&
		    relic_expr_equal(a->u.comparison.right,
		    b->u.comparison.right));

	return (a->u.atom.pred == b->u.atom.pred &&
	    args_equal(a->u.atom.args, b->u.atom.args,
	    pred_arity(magic, a->u.atom.pred)));
}

/*
 * Whether two canonical rules are the same rule.  Their variables are
 * numbered by first occurrence, so equal slots mean the same variable in both
 * and no renaming has to be searched for.
 */
static bool
rules_equal(const relic_magic_t *magic, const relic_rule_t *a,
    const relic_rule_t *b)
{
	size_t i;

	if (a->head != b->head || a->head_arity != b->head_arity ||
	    a->body_length != b->body_length ||
	    !args_equal(a->head_args, b->head_args, a->head_arity))
		return (false);

	for (i = 0; i < a->body_length; i++) {
		if (!literals_equal(magic, &a->body[i], &b->body[i]))
			return (false);
	}

	return (true);
}

/*
 * Whether a rule's head is one of its own positive literals, so that it can
 * only derive a tuple its relation already holds.
 */
static bool
rule_derives_nothing(const relic_rule_t *rule)
{
	const relic_literal_t *literal;
	size_t i;

	for (i = 0; i < rule->body_length; i++) {
		literal = &rule->body[i];

		if (literal->kind == RELIC_LITERAL_POSITIVE &&
		    literal->u.atom.pred == rule->head &&
		    args_equal(literal->u.atom.args, rule->head_args,
		    rule->head_arity))
			return (true);
	}

	return (false);
}

/* Fold one word into a running hash. */
static uint32_t
hash_step(const relic_magic_t *magic, uint32_t hash, uint32_t word)
{
	return (relic_hash_word(magic->hash_seed, hash ^ word));
}

static uint32_t
hash_arg(const relic_magic_t *magic, uint32_t hash, const relic_arg_t *arg)
{
	hash = hash_step(magic, hash, (uint32_t)arg->kind);

	switch (arg->kind) {
	case RELIC_ARG_CONSTANT:
		return (hash_step(magic, hash, arg->u.constant));
	case RELIC_ARG_VARIABLE:
		return (hash_step(magic, hash, arg->u.slot));
	case RELIC_ARG_WILDCARD:
		break;
	}

	return (hash);
}

/*
 * The hash of an expression tree, over everything relic_expr_equal()
 * compares.
 */
static uint32_t
hash_expr(const relic_magic_t *magic, uint32_t hash, const relic_expr_t *expr)
{
	uint8_t i;

	hash = hash_step(magic, hash, (uint32_t)expr->kind);

	switch (expr->kind) {
	case RELIC_EXPR_CONSTANT:
		return (hash_step(magic, hash, expr->u.constant));
	case RELIC_EXPR_VARIABLE:
		return (hash_step(magic, hash, expr->u.slot));
	case RELIC_EXPR_NEGATE:
		return (hash_expr(magic, hash, expr->u.operand));
	case RELIC_EXPR_BINARY:
		hash = hash_step(magic, hash, (uint32_t)expr->u.binary.op);
		hash = hash_expr(magic, hash, expr->u.binary.left);
		return (hash_expr(magic, hash, expr->u.binary.right));
	case RELIC_EXPR_CALL:
		hash = hash_step(magic, hash, (uint32_t)expr->u.call.fn);
		for (i = 0; i < expr->u.call.argc; i++)
			hash = hash_expr(magic, hash, expr->u.call.args[i]);
		return (hash);
	}

	return (hash);
}

/*
 * Mix into `hash` what aggregates_equal() compares.
 */
static uint32_t
hash_aggregate(const relic_magic_t *magic, uint32_t hash,
    const relic_aggregate_t *aggregate)
{
	uint16_t i;

	hash = hash_step(magic, hash, (uint32_t)aggregate->fn);
	hash = hash_step(magic, hash, aggregate->group);
	hash = hash_step(magic, hash, aggregate->column);
	hash = hash_step(magic, hash, aggregate->result);
	for (i = 0; i < aggregate->shared_count; i++)
		hash = hash_step(magic, hash, aggregate->shared[i]);

	return (hash);
}

/*
 * The hash of a canonical rule, over everything rules_equal() compares.
 */
static uint32_t
rule_hash(const relic_magic_t *magic, const relic_rule_t *rule)
{
	const relic_literal_t *literal;
	uint32_t hash;
	uint16_t arity;
	uint16_t j;
	size_t i;

	hash = relic_hash_word(magic->hash_seed, rule->head);
	for (j = 0; j < rule->head_arity; j++)
		hash = hash_arg(magic, hash, &rule->head_args[j]);

	hash = hash_step(magic, hash, (uint32_t)rule->body_length);
	for (i = 0; i < rule->body_length; i++) {
		literal = &rule->body[i];
		hash = hash_step(magic, hash, (uint32_t)literal->kind);

		if (literal->kind == RELIC_LITERAL_AGGREGATE) {
			hash = hash_aggregate(magic, hash,
			    &literal->u.aggregate);
			continue;
		}

		if (literal->kind == RELIC_LITERAL_COMPARISON) {
			hash = hash_step(magic, hash,
			    (uint32_t)literal->u.comparison.op);
			hash = hash_expr(magic, hash,
			    literal->u.comparison.left);
			hash = hash_expr(magic, hash,
			    literal->u.comparison.right);
			continue;
		}

		hash = hash_step(magic, hash, literal->u.atom.pred);
		arity = pred_arity(magic, literal->u.atom.pred);
		for (j = 0; j < arity; j++)
			hash = hash_arg(magic, hash, &literal->u.atom.args[j]);
	}

	return (hash);
}

static uint32_t
rehash_rule(uint32_t id, void *ctx)
{
	const relic_magic_t *magic;

	magic = ctx;

	return (rule_hash(magic, &magic->rules.entries[id - 1]));
}

/*
 * Whether a rule equal to this canonical one has been generated already.
 */
static bool
find_rule(const relic_magic_t *magic, const relic_rule_t *rule, uint32_t hash)
{
	relic_idprobe_t probe;
	uint32_t id;

	relic_idprobe_open(&probe, &magic->rule_index, hash);
	while (relic_idprobe_next(&probe, &id)) {
		if (rules_equal(magic, &magic->rules.entries[id - 1], rule))
			return (true);
	}

	return (false);
}

/*
 * Append a generated rule and index it.  Room in the table and in the index
 * is made before the rule is stored.  A rule's index entry is its position
 * plus one, so the table stops one short of what an identifier can hold.
 * The generated rules are analysed like the program's, indexed by head, so
 * the rewritten query, whose head names no predicate, is kept apart and
 * never appended here.
 */
static int
append_rule(relic_magic_t *magic, const relic_rule_t *rule, uint32_t hash)
{
	if (magic->rules.count >= UINT32_MAX - 1) {
		relic_diagf(magic->ctx, RELIC_ERROR, NULL,
		    "a query's Magic rewrite generated more rules than it can "
		    "index");
		return (-1);
	}

	if (relic_idtab_reserve(&magic->rule_index, 1, rehash_rule,
	    magic) != 0) {
		relic_diag_no_memory(magic->ctx);
		return (-1);
	}

	if (relic_ruletab_append(magic->ctx, &magic->rules, rule) != 0)
		return (-1);

	relic_idtab_place(&magic->rule_index, hash,
	    (uint32_t)magic->rules.count);

	return (0);
}

/*
 * Add a generated rule, renumbered, unless it derives nothing or is a
 * duplicate of one already generated.  Returns 0, or -1 with a diagnostic
 * reported: memory, the budget on literals, or the limit on generated rules.
 *
 * The literal budget is charged on entry, for every draft: whether a rule
 * is kept is known only once it is built, and a long body calling its own
 * predicate drafts copies that are all dropped, each costing the arena its
 * length.  Charging what is kept would never see them.  The rule limit
 * measures the rewritten program, so it counts only the rules kept.
 */
static int
publish_rule(relic_magic_t *magic, const relic_draft_t *draft)
{
	relic_rule_t rule;
	uint32_t hash;

	if (charge_literals(magic, &magic->generated_literals,
	    relic_body_size(draft->body, draft->body_length),
	    draft->attributed) != 0)
		return (-1);

	if (canonicalize(magic, draft, &rule) != 0)
		return (-1);

	if (rule_derives_nothing(&rule))
		return (0);

	hash = rule_hash(magic, &rule);
	if (find_rule(magic, &rule, hash))
		return (0);

	if (confirm_rule_limit(magic, draft->attributed) != 0)
		return (-1);

	return (append_rule(magic, &rule, hash));
}

/*
 * The arguments of the columns in `columns`, in column order: what a magic
 * predicate records about a call.  At least one column is chosen.
 */
static relic_arg_t *
bound_args(relic_magic_t *magic, const relic_arg_t *args, uint16_t arity,
    uint32_t columns)
{
	relic_arg_t *chosen;
	uint16_t count;
	uint16_t column;

	chosen = magic_alloc(magic, column_count(columns), sizeof(*chosen));
	if (chosen == NULL)
		return (NULL);

	count = 0;
	for (column = 0; column < arity; column++) {
		if ((columns & (UINT32_C(1) << column)) == 0)
			continue;

		chosen[count] = args[column];
		count++;
	}

	return (chosen);
}

/*
 * The slots of the variables in the head's bound columns: what a call on the
 * rule's predicate binds before the body starts.  Nothing, for the query.
 */
static uint64_t
head_bound_slots(const relic_rule_t *rule, const relic_adornment_t *head)
{
	const relic_arg_t *arg;
	uint64_t slots;
	uint16_t column;

	slots = 0;
	for (column = 0; column < RELIC_MAX_ARITY; column++) {
		if ((head->bound & (UINT32_C(1) << column)) == 0)
			continue;

		arg = &rule->head_args[column];
		if (arg->kind == RELIC_ARG_VARIABLE)
			slots |= UINT64_C(1) << arg->u.slot;
	}

	return (slots);
}

/*
 * The guard of an adorned rule: its head's magic predicate over the head's
 * bound arguments.
 */
static int
guard_literal(relic_magic_t *magic, const relic_rule_t *rule,
    const relic_adornment_t *head, relic_literal_t *guard)
{
	bzero(guard, sizeof(*guard));
	guard->kind = RELIC_LITERAL_POSITIVE;
	guard->where = rule->where;
	guard->u.atom.where = rule->where;
	guard->u.atom.pred = head->magic;
	guard->u.atom.args = bound_args(magic, rule->head_args,
	    rule->head_arity, head->bound);
	if (guard->u.atom.args == NULL)
		return (-1);

	return (0);
}

/*
 * Whether a body literal calls a predicate read on demand, which the rewrite
 * renames to one of its adornments.
 */
static bool
reads_on_demand(const relic_magic_t *magic, const relic_literal_t *literal)
{
	return (literal->kind == RELIC_LITERAL_POSITIVE &&
	    !pred_is_complete(magic, literal->u.atom.pred));
}

/*
 * Whether a body literal that reads its relation whole -- a negated literal,
 * or an aggregate folding its group -- reads a predicate the query does not
 * read completely.  None may: such a literal is kept as written, so its
 * relation must be whole before the rewrite runs, and nothing evaluates a
 * predicate read on demand except through its adornments.  The walk that
 * finds the complete predicates marks every negated and every group
 * predicate it reaches, and the rewrite reaches only what that walk did, so
 * this never holds; if the two ever disagreed the negation would read an
 * empty relation and succeed, or the fold fold an empty group, silently,
 * and no later check -- not stratification, which sees no cycle -- would
 * notice.
 */
static bool
reads_whole_on_demand(const relic_magic_t *magic,
    const relic_literal_t *literal)
{
	relic_pred_id_t pred;

	if (literal->kind == RELIC_LITERAL_POSITIVE ||
	    !relic_literal_reads(literal, &pred))
		return (false);

	return (!pred_is_complete(magic, pred));
}

/*
 * The columns a call binds: those holding a constant or a slot in `bound`.
 */
static uint32_t
call_columns(const relic_magic_t *magic, const relic_literal_t *call,
    uint64_t bound)
{
	return (relic_plan_bound_columns(&call->u.atom,
	    pred_arity(magic, call->u.atom.pred), bound));
}

/*
 * The adornment a call binding `columns` reads, as the search for the
 * adornments the query reaches found it: the free one when its predicate is
 * read free, and otherwise the one binding those columns.  The draft walks
 * every body in the order that search did, so it asks only for adornments
 * the search recorded, and one it did not find is an internal error.
 */
static int
adornment_of_call(relic_magic_t *magic, const relic_literal_t *call,
    uint32_t columns, relic_adornment_t *adornmentp)
{
	const relic_adornment_t *found;
	relic_pred_id_t source;
	uint32_t adorned_columns;

	source = call->u.atom.pred;
	if (confirm_pred_indexed(magic, source) != 0)
		return (-1);

	adorned_columns = magic->reads_free[source - 1] ? 0 : columns;
	found = find_adornment(&magic->kept, source, adorned_columns,
	    adornment_hash(&magic->kept, source, adorned_columns));
	if (found == NULL) {
		relic_diagf(magic->ctx, RELIC_ERROR, &call->where,
		    "internal error: a query's Magic rewrite drafted a call "
		    "it did not adorn");
		return (-1);
	}

	*adornmentp = *found;
	return (0);
}

/*
 * Generate the magic rule recording `call`, made with `adornment`: the call's
 * bound arguments, derived from the `length` literals of `prefix`, which are
 * those before the call as the generated rules write them.  The body is in
 * the order the rule is evaluated in, guard first, so the prefix is exactly
 * what has bound the call's arguments.  An adornment with nothing bound has
 * no magic predicate and records nothing.
 */
static int
record_call(relic_magic_t *magic, const relic_rule_t *rule,
    const relic_literal_t *prefix, size_t length, const relic_literal_t *call,
    const relic_adornment_t *adornment)
{
	relic_draft_t draft;

	if (adornment->magic == RELIC_ID_NONE)
		return (0);

	bzero(&draft, sizeof(draft));
	draft.source = rule;
	draft.head = adornment->magic;
	draft.head_arity = column_count(adornment->bound);
	draft.head_args = bound_args(magic, call->u.atom.args,
	    pred_arity(magic, adornment->source), adornment->bound);
	if (draft.head_args == NULL)
		return (-1);
	draft.body = prefix;
	draft.body_length = length;
	draft.attributed = adornment->source;

	return (publish_rule(magic, &draft));
}

/* The slot an argument reads, as a mask: none unless it is a variable. */
static uint64_t
arg_slot(const relic_arg_t *arg)
{
	if (arg->kind != RELIC_ARG_VARIABLE)
		return (0);

	return (UINT64_C(1) << arg->u.slot);
}

/*
 * The slots a literal reads: every variable among its arguments, whatever
 * its kind, and an aggregate's key and result.
 */
static uint64_t
literal_slots(const relic_magic_t *magic, const relic_literal_t *literal)
{
	const relic_arg_t *args;
	uint64_t slots;
	uint16_t arity;
	uint16_t i;

	if (literal->kind == RELIC_LITERAL_COMPARISON)
		return (relic_comparison_slots(&literal->u.comparison));

	if (literal->kind == RELIC_LITERAL_AGGREGATE)
		return (relic_aggregate_shared_slots(&literal->u.aggregate) |
		    (UINT64_C(1) << literal->u.aggregate.result));

	args = literal->u.atom.args;
	arity = pred_arity(magic, literal->u.atom.pred);

	slots = 0;
	for (i = 0; i < arity; i++)
		slots |= arg_slot(&args[i]);

	return (slots);
}

/*
 * For each position i of a body in the order `order`, the slots read at or
 * after it -- by the literal at i, by any literal placed later, or by the
 * head -- as `later[i]`, with `later[body_length]` the head's alone.  One
 * pass from the end: each position adds its literal to the one after it.
 */
static uint64_t *
later_slots(relic_magic_t *magic, const relic_rule_t *rule,
    const size_t *order)
{
	uint64_t *later;
	uint16_t column;
	size_t i;

	later = magic_alloc(magic, rule->body_length + 1, sizeof(*later));
	if (later == NULL)
		return (NULL);

	later[rule->body_length] = 0;
	for (column = 0; column < rule->head_arity; column++)
		later[rule->body_length] |= arg_slot(&rule->head_args[column]);

	for (i = rule->body_length; i > 0; i--)
		later[i - 1] = later[i] |
		    literal_slots(magic, &rule->body[order[i - 1]]);

	return (later);
}

static uint16_t
slot_count(uint64_t slots)
{
	uint16_t count;

	count = 0;
	for (; slots != 0; slots &= slots - 1)
		count++;

	return (count);
}

/*
 * Name and intern the supplementary predicate of arity `arity` that the cut
 * before the `call_ordinal`th call binding a column of a prefix's rule makes:
 * sup$<adorned>$<rule>$<call>, the adorned predicate's name, the rule's
 * position among the rules deriving its predicate from 0, and the call's among
 * the rule's calls binding a column from 1.  The query's head names no
 * predicate, and its cuts are named after $q0.  The name depends only on the
 * adornment and the positions, so a later query with the same adornments
 * interns the same names and, where the live variables are as many, finds the
 * same predicates.
 */
static int
intern_supplementary(relic_magic_t *magic, const relic_prefix_t *prefix,
    size_t call_ordinal, uint16_t arity, relic_pred_id_t *idp)
{
	const relic_pred_t *adorned;
	const char *name;
	char *text;
	size_t name_length;
	size_t length;
	size_t room;
	int written;

	name = QUERY_ADORNED_NAME;
	name_length = QUERY_ADORNED_NAME_LENGTH;
	if (prefix->adornment->adorned != RELIC_ID_NONE) {
		adorned = relic_pred_of(magic->ctx,
		    prefix->adornment->adorned);
		name = relic_symbol_bytes(magic->ctx, adorned->name,
		    &name_length);
	}

	room = SUPPLEMENTARY_PREFIX_LENGTH + name_length + ORDINALS_MAX + 1;
	text = magic_alloc(magic, room, 1);
	if (text == NULL)
		return (-1);

	bcopy(SUPPLEMENTARY_PREFIX, text, SUPPLEMENTARY_PREFIX_LENGTH);
	bcopy(name, text + SUPPLEMENTARY_PREFIX_LENGTH, name_length);
	length = SUPPLEMENTARY_PREFIX_LENGTH + name_length;
	written = snprintf(text + length, room - length, "$%zu$%zu",
	    prefix->rule_ordinal, call_ordinal);
	if (written < 0 || (size_t)written >= room - length) {
		relic_diagf(magic->ctx, RELIC_ERROR, NULL,
		    "internal error: a supplementary predicate's name does "
		    "not fit");
		return (-1);
	}
	length += (size_t)written;

	return (intern_generated(magic, text, length, arity,
	    &prefix->rule->where, idp));
}

/*
 * The literal a cut leaves in place of the prefix: the supplementary
 * predicate over the slots of `live`, in slot order.  A cut with no live
 * slot is a predicate of arity zero, and its literal has no arguments.
 */
static int
supplementary_literal(relic_magic_t *magic, const relic_prefix_t *prefix,
    size_t call_ordinal, uint64_t live, relic_literal_t *literal)
{
	relic_arg_t *args;
	uint16_t arity;
	uint16_t slot;
	uint16_t n;

	bzero(literal, sizeof(*literal));
	literal->kind = RELIC_LITERAL_POSITIVE;
	literal->where = prefix->rule->where;
	literal->u.atom.where = prefix->rule->where;

	arity = slot_count(live);
	if (intern_supplementary(magic, prefix, call_ordinal, arity,
	    &literal->u.atom.pred) != 0)
		return (-1);

	args = NULL;
	if (arity > 0) {
		args = magic_alloc(magic, arity, sizeof(*args));
		if (args == NULL)
			return (-1);
	}

	n = 0;
	for (slot = 0; slot < RELIC_MAX_VARIABLES; slot++) {
		if ((live & (UINT64_C(1) << slot)) == 0)
			continue;

		bzero(&args[n], sizeof(args[n]));
		args[n].kind = RELIC_ARG_VARIABLE;
		args[n].u.slot = slot;
		n++;
	}
	literal->u.atom.args = args;

	return (0);
}

/*
 * Cut the prefix before the `call_ordinal`th call binding a column, a call
 * on `called_pred`: derive it once, by one supplementary rule, into a
 * supplementary predicate over its live slots, and continue the prefix from
 * that one literal.  `live` holds the slots the prefix binds that the call,
 * a literal after it or the head reads, so every later literal finds its
 * variables in the cut and the generated bodies stay linear in the rule's
 * calls.
 *
 * A cut whose live slots are more than a predicate may have columns is not
 * made, and the prefix continues, written out in full, to the next cut that
 * fits.
 *
 * The supplementary rule is published as every generated rule is, and so is
 * charged to the budgets.  It is attributed to the predicate adorned, or,
 * for the query, which is no predicate, to `called_pred`, as the query's
 * seeds are.  It is never dropped, because its head is new to this rewrite
 * and absent from its own body: the name is built from the adorned
 * predicate's name, which has one `$` before its b's and f's, or `$q0`,
 * which no adorned name is, and from positions a rewrite visits once, and
 * no name a program writes contains `$`.  Were it ever dropped as equal to
 * a rule already kept, that rule would derive the same relation; the
 * assertion guards the argument rather than correctness.  When a budget
 * abandons the rewrite here, the prefix is left as it was and the predicate
 * stays interned with no rule, as an abandoned adornment's predicates do;
 * the rewrite is discarded whole.
 */
static int
cut_prefix(relic_magic_t *magic, relic_prefix_t *prefix, size_t call_ordinal,
    uint64_t live, relic_pred_id_t called_pred)
{
	relic_literal_t cut;
	relic_draft_t draft;
	size_t rules_before RELIC_UNUSED;

	if (slot_count(live) > RELIC_MAX_ARITY)
		return (0);

	if (supplementary_literal(magic, prefix, call_ordinal, live,
	    &cut) != 0)
		return (-1);

	bzero(&draft, sizeof(draft));
	draft.source = prefix->rule;
	draft.head = cut.u.atom.pred;
	draft.head_args = cut.u.atom.args;
	draft.head_arity = slot_count(live);
	draft.body = prefix->body;
	draft.body_length = prefix->length;
	draft.attributed = prefix->adornment->source != RELIC_ID_NONE ?
	    prefix->adornment->source : called_pred;

	rules_before = magic->rules.count;
	if (publish_rule(magic, &draft) != 0)
		return (-1);
	assert(magic->rules.count == rules_before + 1);

	prefix->body[0] = cut;
	prefix->length = 1;

	return (0);
}

/*
 * Draft the body of `rule`, the `rule_ordinal`th rule deriving its
 * predicate, rewritten for a call with the columns of `head` bound,
 * generating the magic rules of the calls it makes and the supplementary
 * rules of the cuts between them.
 *
 * The body is put in the order the planner would evaluate it with the head's
 * bound variables already known -- guard first, when the call binds anything
 * -- and walked in that order, keeping the prefix, the literals evaluated
 * so far as the generated rules write them, and `bound`, the slots the
 * literals walked so far bind.  Each call on a predicate read on demand is
 * renamed to the adornment it reads, and a demand call, one whose adornment
 * binds a column, has its magic rule generated from the prefix.  Before
 * every call binding a column but the first, the prefix is cut, so that the
 * call's magic rule, every later rule and the draft read it through one
 * literal.
 *
 * Where the prefix is cut depends on the source rule and `bound`, never on
 * the adornment a call reads.  A call the free adornment subsumes binds its
 * columns like any other and has the prefix cut before it, though it reads
 * the free adornment and has no magic rule.  So every rule drafted for an
 * adornment is as long as it would be were each call read through the
 * adornment binding its columns, and subsumption lengthens no rule; it
 * leaves out the magic rules of the calls it redirects.  Were the cut
 * before a subsumed call skipped, the prefix before it would run on into
 * the next rule and, past a later cut too wide to make, into every magic
 * rule after it and the draft.
 *
 * Why a cut loses nothing: it keeps the slots of `bound` that are read at or
 * after the call, `later[i]`.  A slot it drops is in `bound` and absent from
 * the prefix from then on, but no literal from position i onwards reads it,
 * and later[] only shrinks along the body, so no later cut asks for it
 * either.  `bound` keeps meaning what the source rule has bound, which is
 * what an adornment is computed from, and a skipped cut changes nothing.
 * The draft has the source rule's head arguments and head arity; its caller
 * names the head.
 */
static int
rewrite_body(relic_magic_t *magic, const relic_rule_t *rule,
    const relic_adornment_t *head, size_t rule_ordinal, relic_draft_t *draft)
{
	relic_adornment_t called;
	relic_literal_t literal;
	relic_prefix_t prefix;
	uint64_t *later;
	size_t *order;
	size_t binding_calls;
	size_t i;
	uint64_t bound;
	uint64_t live;
	uint32_t columns;

	bzero(&prefix, sizeof(prefix));
	prefix.rule = rule;
	prefix.adornment = head;
	prefix.rule_ordinal = rule_ordinal;
	binding_calls = 0;

	order = magic_alloc(magic, rule->body_length, sizeof(*order));
	if (order == NULL)
		return (-1);
	prefix.body = magic_alloc(magic, rule->body_length + 1,
	    sizeof(*prefix.body));
	if (prefix.body == NULL)
		return (-1);

	bound = head_bound_slots(rule, head);
	if (relic_plan_order(magic->ctx, &magic->arena, rule, bound,
	    magic->incomplete, order) != 0)
		return (-1);
	later = later_slots(magic, rule, order);
	if (later == NULL)
		return (-1);

	if (head->magic != RELIC_ID_NONE) {
		if (guard_literal(magic, rule, head, &prefix.body[0]) != 0)
			return (-1);
		prefix.length++;
	}

	for (i = 0; i < rule->body_length; i++) {
		literal = rule->body[order[i]];

		if (reads_whole_on_demand(magic, &literal)) {
			relic_diagf(magic->ctx, RELIC_ERROR, &rule->where,
			    "internal error: a query's Magic rewrite negates "
			    "or folds a predicate it does not read completely");
			return (-1);
		}

		if (reads_on_demand(magic, &literal)) {
			columns = call_columns(magic, &literal, bound);
			if (columns != 0)
				binding_calls++;

			if (columns != 0 && binding_calls > 1) {
				live = bound & later[i];
				if (cut_prefix(magic, &prefix, binding_calls,
				    live, literal.u.atom.pred) != 0)
					return (-1);
			}

			if (adornment_of_call(magic, &literal, columns,
			    &called) != 0)
				return (-1);
			if (record_call(magic, rule, prefix.body,
			    prefix.length, &literal, &called) != 0)
				return (-1);
			literal.u.atom.pred = called.adorned;
		}

		prefix.body[prefix.length] = literal;
		prefix.length++;
		bound |= relic_plan_literal_binds(magic->ctx, &literal, bound);
	}

	bzero(draft, sizeof(*draft));
	draft->source = rule;
	draft->head_args = rule->head_args;
	draft->head_arity = rule->head_arity;
	draft->body = prefix.body;
	draft->body_length = prefix.length;

	return (0);
}

/*
 * Generate the adorned rule of one rule deriving an adornment's source, the
 * `rule_ordinal`th of them in program order: the rewritten body over the
 * adorned head.
 */
static int
adorn_rule(relic_magic_t *magic, const relic_rule_t *rule,
    size_t rule_ordinal, const relic_adornment_t *adornment)
{
	relic_draft_t draft;

	if (rewrite_body(magic, rule, adornment, rule_ordinal, &draft) != 0)
		return (-1);

	draft.head = adornment->adorned;
	draft.attributed = adornment->source;

	return (publish_rule(magic, &draft));
}

/*
 * Rewrite the query, which is called with nothing bound and has no guard,
 * into the rule kept apart from the generated ones.  The magic rules its
 * calls generate are the seeds.
 *
 * The query's head names no predicate, so it is the source and the adorned
 * predicate of a pseudo-adornment with no bound column and no magic
 * predicate: nothing of it is looked up, and the rewritten rule keeps the
 * query's head and head arity, to run into the same answer relation.
 *
 * The rewritten query is canonicalized here rather than published, so the
 * literal budget does not charge it.  It is one rule as long as the query,
 * which is not part of the program the budget is proportional to: charging
 * it would abandon the rewrite of a long query over a small program.
 */
static int
rewrite_query(relic_magic_t *magic, const relic_rule_t *query)
{
	relic_adornment_t unbound;
	relic_draft_t draft;

	bzero(&unbound, sizeof(unbound));
	unbound.source = query->head;
	unbound.adorned = query->head;
	unbound.magic = RELIC_ID_NONE;
	unbound.bound = 0;

	if (rewrite_body(magic, query, &unbound, 0, &draft) != 0)
		return (-1);

	draft.head = query->head;

	return (canonicalize(magic, &draft, &magic->query));
}

/*
 * Generate the rules of one adornment: the rule over its source's base view
 * first, when the source has facts, then every rule deriving the source, in
 * program order, each rewritten for that call.  The rule over the base view
 * is numbered after the program's rules, and calls nothing on demand, so it
 * cuts no prefix and names no supplementary predicate.
 */
static int
adorn_rules(relic_magic_t *magic, const relic_adornment_t *adornment)
{
	const relic_ruletab_t *rules;
	const relic_location_t *where;
	relic_rule_t over_facts;
	size_t first;
	size_t end;
	size_t i;

	rules = &magic->ctx->program.rules;
	first = magic->first_rule_of[adornment->source - 1];
	end = magic->first_rule_of[adornment->source];

	/*
	 * A predicate adorned is derived, so it has a rule, whose place the
	 * rule over its base view borrows.
	 */
	assert(first < end);
	if (relic_base_count(magic->ctx, adornment->source) > 0) {
		where = &rules->entries[magic->rules_by_head[first]].where;
		if (base_view_rule(magic, adornment->source, where,
		    &over_facts) != 0)
			return (-1);

		if (adorn_rule(magic, &over_facts, end - first,
		    adornment) != 0)
			return (-1);
	}

	for (i = first; i < end; i++) {
		if (adorn_rule(magic, &rules->entries[magic->rules_by_head[i]],
		    i - first, adornment) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Room for the searches that find the adornments the query reaches, no
 * predicate yet read free.
 */
static int
begin_searching(relic_magic_t *magic)
{
	size_t count;

	count = magic->predicate_count;

	magic->reads_free = magic_alloc(magic, count,
	    sizeof(*magic->reads_free));
	if (magic->reads_free == NULL)
		return (-1);
	bzero(magic->reads_free, count * sizeof(*magic->reads_free));

	return (0);
}

/*
 * Begin a search with no adornment recorded and no rule read, keeping the
 * predicates earlier searches read free.  The counts a search is held to
 * are its own, so each search is bounded as a rewrite is.
 */
static void
begin_search(relic_magic_t *magic)
{
	adornment_set_clear(&magic->kept);
	adornment_set_clear(&magic->subsumed);
	magic->generated_preds = 0;
	magic->searched_rules = 0;
	magic->searched_literals = 0;
}

/*
 * Confirm that `source` is one of the predicates that existed when the
 * rewrite began, which every array the rewrite indexes by predicate covers.
 * Every literal of a program rule or of the query was interned before then,
 * so one outside is an internal error, reported rather than read past an
 * array.
 */
static int
confirm_pred_indexed(relic_magic_t *magic, relic_pred_id_t source)
{
	if (source != RELIC_ID_NONE && source <= magic->predicate_count)
		return (0);

	relic_diagf(magic->ctx, RELIC_ERROR, NULL,
	    "internal error: a query's Magic rewrite met a predicate it had "
	    "not indexed");

	return (-1);
}

/*
 * Reach the adornment a call on `source` binding the columns of `bound`
 * reads, applying subsumption.
 *
 * A predicate is read free from the first call that binds none of its
 * columns: that call and every later one read its free adornment, and a
 * later call that binds columns is noted as subsumed by it.  Until then a
 * call binding columns reaches the adornment binding them.
 */
static int
reach_adornment(relic_magic_t *magic, relic_pred_id_t source, uint32_t bound)
{
	bool *reads_free;

	if (confirm_pred_indexed(magic, source) != 0)
		return (-1);
	reads_free = &magic->reads_free[source - 1];

	if (bound == 0)
		*reads_free = true;

	if (!*reads_free)
		return (keep_adornment(magic, source, bound));

	if (bound != 0) {
		if (note_subsumed(magic, source, bound) != 0)
			return (-1);
	}

	return (keep_adornment(magic, source, 0));
}

/*
 * Reach the adornment of each call of `rule` in the order it is drafted in
 * for a call binding the slots of `bound`, each call binding those slots and
 * those of every positive literal before it: the adornments rewrite_body()
 * asks for when it drafts the rule in that order.  The order, and the
 * planner's work in finding it, are taken from the scratch arena.
 */
static int
reach_calls_in_order(relic_magic_t *magic, const relic_rule_t *rule,
    uint64_t bound)
{
	const relic_literal_t *literal;
	size_t *order;
	uint32_t columns;
	size_t i;

	order = relic_arena_allocarray(&magic->scratch, rule->body_length,
	    sizeof(*order));
	if (order == NULL) {
		relic_diag_no_memory(magic->ctx);
		return (-1);
	}

	if (relic_plan_order(magic->ctx, &magic->scratch, rule, bound,
	    magic->incomplete, order) != 0)
		return (-1);

	for (i = 0; i < rule->body_length; i++) {
		literal = &rule->body[order[i]];

		if (reads_on_demand(magic, literal)) {
			columns = call_columns(magic, literal, bound);
			if (reach_adornment(magic, literal->u.atom.pred,
			    columns) != 0)
				return (-1);
		}

		bound |= relic_plan_literal_binds(magic->ctx, literal, bound);
	}

	return (0);
}

/*
 * Reach the adornment of every call `rule` makes on a predicate read on
 * demand, for a call that binds the slots of `bound`.
 *
 * What ordering the rule took from the scratch arena is forgotten once the
 * rule is read: a search reads the rules of every adornment it keeps, and
 * keeps none of what it read them with.
 */
static int
reach_calls(relic_magic_t *magic, const relic_rule_t *rule, uint64_t bound)
{
	int status;

	status = reach_calls_in_order(magic, rule, bound);
	relic_arena_reset(&magic->scratch);

	return (status);
}

/*
 * Charge a search for reading one rule of an adornment of `source`: a rule
 * against the rule limit, and its body against the literal budget.
 *
 * Drafting an adornment drafts every rule of its source, whole or cut into
 * pieces that hold at least its body between them, and the literal budget
 * charges every draft, kept or dropped, so a search reads no more literals
 * than drafting the adornments it keeps is charged.  The rule limit counts
 * only the rules the draft keeps, and the draft drops duplicates and rules
 * that derive nothing, so under a configured rule limit a search can refuse
 * a query whose rewrite would have fit.  Charging the rules read is what
 * bounds the search when the rule limit is the only bound configured.
 */
static int
charge_rule_read(relic_magic_t *magic, const relic_rule_t *rule,
    relic_pred_id_t source)
{
	if (magic->searched_rules >= magic->rule_limit.most)
		return (exceed(magic, &magic->rule_limit, "magic-rules",
		    "generated rules", source));
	magic->searched_rules++;

	return (charge_literals(magic, &magic->searched_literals,
	    relic_body_size(rule->body, rule->body_length), source));
}

/*
 * Reach the adornments the rules deriving an adornment's source call, each
 * rule ordered for the columns the adornment binds.  The rule over the
 * source's base view reads only the base view, which is complete, and calls
 * nothing.
 */
static int
reach_rules_of(relic_magic_t *magic, const relic_adornment_t *adornment)
{
	const relic_ruletab_t *rules;
	const relic_rule_t *rule;
	size_t first;
	size_t end;
	size_t i;

	rules = &magic->ctx->program.rules;
	first = magic->first_rule_of[adornment->source - 1];
	end = magic->first_rule_of[adornment->source];

	for (i = first; i < end; i++) {
		rule = &rules->entries[magic->rules_by_head[i]];

		if (charge_rule_read(magic, rule, adornment->source) != 0)
			return (-1);

		if (reach_calls(magic, rule, head_bound_slots(rule,
		    adornment)) != 0)
			return (-1);
	}

	return (0);
}

/*
 * One search: reach the adornments of the query's calls, then those of the
 * rules of every adornment kept, in the order kept, which appends the
 * adornments their calls reach.  An adornment is copied out of the set
 * before its rules are read, because reading them may keep more and move
 * the set's entries.
 */
static int
search_once(relic_magic_t *magic, const relic_rule_t *query)
{
	relic_adornment_t adornment;
	size_t i;

	begin_search(magic);

	if (reach_calls(magic, query, 0) != 0)
		return (-1);

	for (i = 0; i < magic->kept.count; i++) {
		adornment = magic->kept.entries[i];
		if (reach_rules_of(magic, &adornment) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Whether the search kept an adornment binding columns of a predicate it
 * read free: one some call reached before a later call read the predicate
 * free, and so one reached for nothing.
 */
static bool
kept_bound_adornment_of_free(const relic_magic_t *magic)
{
	const relic_adornment_t *adornment;
	size_t i;

	for (i = 0; i < magic->kept.count; i++) {
		adornment = &magic->kept.entries[i];
		if (adornment->bound != 0 &&
		    magic->reads_free[adornment->source - 1])
			return (true);
	}

	return (false);
}

/*
 * Find the adornments the query reaches, drafting nothing, by one search or
 * two.
 *
 * A predicate the query reaches with no column bound is read through its
 * free adornment by every call, whatever the call binds: the free adornment
 * holds every tuple of the predicate, so a call binding columns finds what
 * it would have found in its own adornment by looking the free one up on
 * those columns, and the free one is computed in full beside it anyway.
 * That call binds the same variables either way, so the adornments of the
 * calls after it do not change, and neither does anything an adornment's
 * rules reach: which adornments a rule's calls reach depends on the
 * adornment it is read for and on which predicates are read free, and on
 * nothing else.
 *
 * The first search reads a predicate free from the first call that reaches
 * it free, so an adornment of it that an earlier call reached binding
 * columns was reached for nothing.  When there is one, the second search
 * begins again with every predicate the first read free read free from the
 * start.  Every call it makes the first made too, by induction over what it
 * reaches: a call reaching a free adornment reaches one the first search
 * reached, and a call binding columns of a predicate the first never read
 * free reaches the adornment the first reached for it.  So every predicate
 * the second search reaches free is already read free, and no adornment it
 * keeps was reached for nothing.  A predicate the first search read free
 * stays free in the second even when only an adornment the second no longer
 * reaches called it free.
 *
 * Each search keeps no more adornments than a rewrite without subsumption
 * would adorn, and is held on its own to the limits and budgets that rewrite
 * was held to, as charge_rule_read() describes.  The query's body is read
 * once in each search and charged to neither.
 */
static int
search_adornments(relic_magic_t *magic, const relic_rule_t *query)
{
	if (begin_searching(magic) != 0)
		return (-1);

	if (search_once(magic, query) != 0)
		return (-1);

	if (!kept_bound_adornment_of_free(magic))
		return (0);

	if (search_once(magic, query) != 0)
		return (-1);
	assert(!kept_bound_adornment_of_free(magic));

	return (0);
}

/*
 * Whether some adornment the query reaches and keeps binds a column.
 */
static bool
binds_some_column(const relic_magic_t *magic)
{
	size_t i;

	for (i = 0; i < magic->kept.count; i++) {
		if (magic->kept.entries[i].bound != 0)
			return (true);
	}

	return (false);
}

/*
 * Read completely every predicate the query reaches, all of them free: mark
 * their components complete with everything those depend on, recording in
 * `completed_late` the components not complete already, and keep the query
 * as written.  A rewrite would derive exactly the relations of those
 * predicates, which the program's own rules derive as well, so no rule is
 * generated and nothing is copied.
 *
 * Every adornment kept is of a predicate read on demand, which a rule
 * derives, so each has a component; one without is an internal error.
 */
static int
read_reached_completely(relic_magic_t *magic, const relic_rule_t *query)
{
	relic_closure_t closure;
	size_t components;
	size_t position;
	size_t i;

	components = magic->ctx->program.schedule.component_count;

	if (closure_begin(&closure, magic) != 0)
		return (-1);
	magic->completed_late = magic_alloc(magic, components,
	    sizeof(*magic->completed_late));
	if (magic->completed_late == NULL)
		return (-1);
	bzero(magic->completed_late, components *
	    sizeof(*magic->completed_late));

	for (i = 0; i < magic->kept.count; i++) {
		position = component_of(magic, magic->kept.entries[i].source);
		if (position == RELIC_NO_COMPONENT) {
			relic_diagf(magic->ctx, RELIC_ERROR, NULL,
			    "internal error: a query's Magic rewrite adorned "
			    "a predicate no rule derives");
			return (-1);
		}
		mark_complete(&closure, position);
	}
	close_complete(&closure);

	for (i = 0; i < closure.count; i++)
		magic->completed_late[closure.queue[i]] = true;
	note_incomplete(magic);

	magic->reads_completely = true;
	magic->query = *query;

	return (0);
}

/*
 * Rewrite a query whose complete predicates relic_magic_find_complete() has
 * found and the caller has evaluated, in two stages.  The first finds the
 * adornments the query reaches, applying subsumption, and drafts nothing.
 * When none of them binds a column the query reads what it reaches
 * completely, and there is no second stage.  Otherwise the second names the
 * adornments kept and drafts their rules: the query first, which generates
 * the seeds, then the rules of every adornment kept, in the order recorded.
 *
 * Only predicates read on demand are adorned, and every one of them is
 * derived by some rule -- one no rule derives is complete -- so each
 * adornment's source has rules to generate from.
 *
 * What the rewrite may generate is bounded in predicates and body literals by
 * budgets in proportion to the program, and in predicates and rules by the
 * Magic limits.  Configuring either limit sizes the rewrite and lifts both
 * budgets, leaving only the limits set.  The first stage counts the predicates
 * of the adornments it keeps; the supplementary predicates the second stage's
 * cuts add are not counted, since each has one rule, which the rule limit and
 * the literal budget count.  The budget on literals holds everything the
 * second stage drafts, kept or dropped, to a constant multiple of the
 * program's size, which counting rules cannot when long rules are copied once
 * per adornment; a literal's size counts the nodes of its expressions too,
 * so a large expression copied into many prefixes is bounded as a long body
 * is.  It bounds the rules kept as well.  Every rule is charged its body
 * before it is kept, and every one but a seed has a body literal, of size at
 * least one.  A seed is the query's, or the magic rule of an adorned rule's
 * first call when constants alone bind it, at most one per adorned rule, whose
 * draft was charged a literal.  So the rules kept number at most twice the
 * literals drafted plus the query's seeds.  The budgets are proportional to
 * the program while the cost of answering without the rewrite is proportional
 * to the data, which is why a caller who sets a limit is not overruled.
 */
int
relic_magic_rewrite(relic_magic_t *magic, const relic_rule_t *query)
{
	const relic_config_t *config;
	const relic_program_t *program;
	size_t i;
	bool sized;

	config = &magic->ctx->config;
	program = &magic->ctx->program;
	sized = config->max_magic_preds != 0 || config->max_magic_rules != 0;

	magic->pred_limit = limit_or_budget(config->max_magic_preds, sized,
	    program->schedule.member_count);
	magic->rule_limit.configured = sized;
	magic->rule_limit.most = config->max_magic_rules != 0 ?
	    config->max_magic_rules : SIZE_MAX;
	magic->literal_budget = sized ? SIZE_MAX :
	    budget_for(program_size(&program->rules) +
	    2 * magic->base_view_count);

	if (search_adornments(magic, query) != 0)
		return (-1);

	if (!binds_some_column(magic))
		return (read_reached_completely(magic, query));

	if (name_adornments(magic) != 0)
		return (-1);

	if (rewrite_query(magic, query) != 0)
		return (-1);
	magic->seed_count = magic->rules.count;

	for (i = 0; i < magic->kept.count; i++) {
		if (adorn_rules(magic, &magic->kept.entries[i]) != 0)
			return (-1);
	}

	return (0);
}
