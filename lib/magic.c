/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
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
 * Not every predicate is worth adorning.  One derived by nothing but facts is
 * read as it is, since adorning it would copy every fact into a guarded rule.
 * One the query reaches through negation is materialized in full, so that
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
 *	every literal placed before the call.
 *
 * An adornment with no bound column has no magic predicate and its rules no
 * guard, since there is no bound value to record.  The query is rewritten the
 * same way, with nothing bound and no guard, and the magic rules its body
 * generates are the seeds: for path(a, X)? the fact magic$path$bf(a).  The
 * query's head names no predicate, and nothing here looks it up: a head's
 * arity is read from the rule, and a limit reached while generating a rule is
 * attributed to the predicate called, never to the rule making the call.
 *
 * The rewrite terminates because adornments are memoized on the predicate and
 * the bound columns, and a predicate of arity n has at most 2^n of them.  The
 * array of adornments is the worklist: generating one adornment's rules
 * appends the adornments its calls need, and the loop walking the array
 * reaches each of them in turn, once.
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
 * predicates it generates and in rules for the rules.
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

typedef struct relic_demand relic_demand_t;
typedef struct relic_draft relic_draft_t;
typedef struct relic_renumbering relic_renumbering_t;

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
 * rules have not been visited yet.  `queue` holds the components marked
 * complete, in the order they were marked, and those before `queue_head` have
 * had their rules followed to what they depend on.  Each component is
 * pending at most once and queued at most once, which bounds both.
 */
struct relic_demand {
	relic_magic_t		*magic;
	bool			*handled;
	size_t			*pending;
	size_t			*queue;
	size_t			 pending_count;
	size_t			 queue_count;
	size_t			 queue_head;
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
static int	 adorn_rule(relic_magic_t *magic, const relic_rule_t *rule,
		    const relic_adornment_t *adornment);
static int	 adorn_rules(relic_magic_t *magic,
		    const relic_adornment_t *adornment);
static uint32_t	 adornment_hash(const relic_magic_t *magic,
		    relic_pred_id_t source, uint32_t bound);
static int	 adornment_of_call(relic_magic_t *magic,
		    const relic_literal_t *call, uint64_t bound,
		    relic_adornment_t *adornmentp);
static int	 append_rule(relic_magic_t *magic, const relic_rule_t *rule,
		    uint32_t hash);
static bool	 args_equal(const relic_arg_t *a, const relic_arg_t *b,
		    uint16_t count);
static relic_arg_t *bound_args(relic_magic_t *magic, const relic_arg_t *args,
		    uint16_t arity, uint32_t columns);
static int	 canonicalize(relic_magic_t *magic, const relic_draft_t *draft,
		    relic_rule_t *rule);
static void	 close_complete(relic_demand_t *demand);
static uint16_t	 column_count(uint32_t columns);
static const relic_component_t *component_at(const relic_magic_t *magic,
		    size_t position);
static bool	 component_is_extensional(const relic_magic_t *magic,
		    size_t position);
static size_t	 component_of(const relic_magic_t *magic,
		    relic_pred_id_t pred);
static const relic_rule_t *component_rule(const relic_magic_t *magic,
		    const relic_component_t *component, size_t i);
static int	 confirm_pred_limit(relic_magic_t *magic,
		    relic_pred_id_t source, size_t generated);
static int	 confirm_rule_limit(relic_magic_t *magic,
		    relic_pred_id_t attributed);
static relic_arg_t *copy_args(relic_magic_t *magic,
		    relic_renumbering_t *renumbering, const relic_arg_t *args,
		    uint16_t count);
static int	 copy_literal(relic_magic_t *magic,
		    relic_renumbering_t *renumbering,
		    const relic_literal_t *source, relic_literal_t *copy);
static int	 demand_begin(relic_demand_t *demand, relic_magic_t *magic);
static int	 exceed(relic_magic_t *magic,
		    const relic_magic_limit_t *limit, const char *key,
		    const char *what, relic_pred_id_t source);
static int	 find_adornment(relic_magic_t *magic, relic_pred_id_t source,
		    uint32_t bound, relic_adornment_t *adornmentp);
static bool	 find_rule(const relic_magic_t *magic, const relic_rule_t *rule,
		    uint32_t hash);
static int	 guard_literal(relic_magic_t *magic, const relic_rule_t *rule,
		    const relic_adornment_t *head, relic_literal_t *guard);
static uint32_t	 hash_arg(const relic_magic_t *magic, uint32_t hash,
		    const relic_arg_t *arg);
static uint32_t	 hash_step(const relic_magic_t *magic, uint32_t hash,
		    uint32_t word);
static uint64_t	 head_bound_slots(const relic_rule_t *rule,
		    const relic_adornment_t *head);
static int	 index_rules_by_head(relic_magic_t *magic);
static int	 intern_generated(relic_magic_t *magic, const char *name,
		    size_t length, uint16_t arity,
		    const relic_location_t *where, relic_pred_id_t *idp);
static relic_magic_limit_t limit_or_budget(size_t configured,
		    size_t program_size);
static bool	 literals_equal(const relic_magic_t *magic,
		    const relic_literal_t *a, const relic_literal_t *b);
static void	*magic_alloc(relic_magic_t *magic, size_t count, size_t size);
static int	 map_components(relic_magic_t *magic);
static void	 mark_complete(relic_demand_t *demand, size_t position);
static int	 name_adornment(relic_magic_t *magic, relic_pred_id_t source,
		    uint32_t bound, relic_adornment_t *adornment);
static bool	 negates_on_demand(const relic_magic_t *magic,
		    const relic_literal_t *literal);
static uint64_t	 positive_slots(const relic_magic_t *magic,
		    const relic_literal_t *literal);
static uint16_t	 pred_arity(const relic_magic_t *magic, relic_pred_id_t pred);
static bool	 pred_is_complete(const relic_magic_t *magic,
		    relic_pred_id_t pred);
static int	 publish_rule(relic_magic_t *magic, const relic_draft_t *draft);
static bool	 reads_on_demand(const relic_magic_t *magic,
		    const relic_literal_t *literal);
static int	 record_adornment(relic_magic_t *magic,
		    relic_pred_id_t source, uint32_t bound, uint32_t hash,
		    relic_adornment_t *adornmentp);
static int	 record_call(relic_magic_t *magic, const relic_rule_t *rule,
		    const relic_literal_t *body, size_t position,
		    const relic_adornment_t *adornment);
static uint32_t	 rehash_adornment(uint32_t id, void *ctx);
static uint32_t	 rehash_rule(uint32_t id, void *ctx);
static void	 renumber_arg(relic_renumbering_t *renumbering,
		    const relic_arg_t *source, relic_arg_t *copy);
static void	 report_limit(relic_magic_t *magic, const char *limit,
		    size_t configured, relic_pred_id_t source);
static int	 rewrite_body(relic_magic_t *magic, const relic_rule_t *rule,
		    const relic_adornment_t *head, relic_draft_t *draft);
static int	 rewrite_query(relic_magic_t *magic,
		    const relic_rule_t *query);
static bool	 rule_derives_nothing(const relic_rule_t *rule);
static uint32_t	 rule_hash(const relic_magic_t *magic,
		    const relic_rule_t *rule);
static bool	 rules_equal(const relic_magic_t *magic, const relic_rule_t *a,
		    const relic_rule_t *b);
static void	 visit_body(relic_demand_t *demand, const relic_rule_t *rule);
static void	 visit_demanded(relic_demand_t *demand, size_t position);

void
relic_magic_init(relic_magic_t *magic, relic_t *ctx)
{
	bzero(magic, sizeof(*magic));
	magic->ctx = ctx;
	relic_arena_init_charged(&magic->arena, &ctx->account);
	relic_idtab_init(&magic->adornment_index);
	relic_idtab_init(&magic->rule_index);
	magic->hash_seed = ctx->hash_seed;
}

void
relic_magic_reset(relic_magic_t *magic)
{
	relic_t *ctx;

	ctx = magic->ctx;

	free(magic->adornments);
	free(magic->rules.entries);
	relic_idtab_reset(&magic->adornment_index);
	relic_idtab_reset(&magic->rule_index);
	relic_arena_reset(&magic->arena);

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
 * Whether every rule of a component is a fact, which makes its predicate
 * extensional: loading the facts computes the whole relation.
 */
static bool
component_is_extensional(const relic_magic_t *magic, size_t position)
{
	const relic_component_t *component;
	size_t i;

	component = component_at(magic, position);

	for (i = 0; i < component->rule_count; i++) {
		if (component_rule(magic, component, i)->body_length != 0)
			return (false);
	}

	return (true);
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
	demand->queue = magic_alloc(magic, components, sizeof(*demand->queue));
	if (demand->queue == NULL)
		return (-1);

	bzero(magic->complete_components, components *
	    sizeof(*magic->complete_components));
	bzero(demand->handled, components * sizeof(*demand->handled));

	return (0);
}

/*
 * Mark a component complete, queueing it so that what it depends on is
 * marked too.
 */
static void
mark_complete(relic_demand_t *demand, size_t position)
{
	if (demand->magic->complete_components[position])
		return;

	demand->magic->complete_components[position] = true;
	demand->queue[demand->queue_count] = position;
	demand->queue_count++;
}

/*
 * Examine the relational literals of a body the query reaches.  A negated
 * literal's predicate is complete.  A positive literal's is complete when it
 * is extensional, and otherwise read on demand, and the rules of its
 * component are visited in turn -- once, however many literals call it or
 * its fellow members.
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
		if (literal->kind == RELIC_LITERAL_COMPARISON)
			continue;

		pred = literal->u.atom.pred;
		position = component_of(magic, pred);
		if (position == RELIC_NO_COMPONENT)
			continue;

		if (literal->kind == RELIC_LITERAL_NEGATED) {
			mark_complete(demand, position);
			continue;
		}

		if (demand->handled[position])
			continue;
		demand->handled[position] = true;

		if (magic->complete_components[position])
			continue;

		if (component_is_extensional(magic, position)) {
			mark_complete(demand, position);
			continue;
		}

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
 * Mark complete everything a complete component depends on, positively or
 * negatively: a relation is complete only if what it is derived from is.
 * Each component is queued when first marked and its rules followed once.
 */
static void
close_complete(relic_demand_t *demand)
{
	const relic_magic_t *magic;
	const relic_component_t *component;
	const relic_literal_t *literal;
	const relic_rule_t *rule;
	size_t target;
	size_t i;
	size_t j;

	magic = demand->magic;

	while (demand->queue_head < demand->queue_count) {
		component = component_at(magic,
		    demand->queue[demand->queue_head]);
		demand->queue_head++;

		for (i = 0; i < component->rule_count; i++) {
			rule = component_rule(magic, component, i);

			for (j = 0; j < rule->body_length; j++) {
				literal = &rule->body[j];
				if (literal->kind == RELIC_LITERAL_COMPARISON)
					continue;

				target = component_of(magic,
				    literal->u.atom.pred);
				if (target != RELIC_NO_COMPONENT)
					mark_complete(demand, target);
			}
		}
	}
}

/*
 * Find what the query reads completely: walk from its body through every
 * predicate read on demand, collecting the complete components, then close
 * that set over what its members depend on.  A predicate is complete when its
 * component is, or when no rule derives it.
 *
 * The walk may visit the rules of a predicate that the closure later finds
 * complete.  That adds nothing wrong: whatever it collects beyond such a
 * predicate is something the predicate depends on, and complete already.
 */
int
relic_magic_find_complete(relic_magic_t *magic, const relic_rule_t *query)
{
	relic_demand_t demand;
	size_t position;
	size_t p;

	magic->predicate_count = relic_pred_count(magic->ctx);

	if (map_components(magic) != 0)
		return (-1);

	if (index_rules_by_head(magic) != 0)
		return (-1);

	if (demand_begin(&demand, magic) != 0)
		return (-1);

	visit_body(&demand, query);
	while (demand.pending_count > 0) {
		demand.pending_count--;
		position = demand.pending[demand.pending_count];
		visit_demanded(&demand, position);
	}

	close_complete(&demand);

	for (p = 0; p < magic->predicate_count; p++) {
		position = magic->component_of[p];
		magic->incomplete[p] = position != RELIC_NO_COMPONENT &&
		    !magic->complete_components[position];
	}

	return (0);
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
 * A configured limit, or, when none is configured, the budget for a program
 * of `program_size`: large enough for every rewrite that specializes the
 * program rather than multiplies it, and small enough that the rewritten
 * program never has more than a constant multiple of the program's
 * predicates and rules, however many adornments the program's arities allow.
 * It counts rules, not their length: each adorned copy of a rule is as long
 * as the rule and costs as much to order, so a long rule reached through
 * many adornments makes a rewrite much larger than its count suggests.
 */
static relic_magic_limit_t
limit_or_budget(size_t configured, size_t program_size)
{
	relic_magic_limit_t limit;

	limit.configured = configured != 0;
	limit.most = configured;
	if (limit.configured)
		return (limit);

	if (program_size > (SIZE_MAX - RELIC_MAGIC_BUDGET_FLOOR) /
	    RELIC_MAGIC_BUDGET_FACTOR)
		limit.most = SIZE_MAX;
	else
		limit.most = RELIC_MAGIC_BUDGET_FLOOR +
		    RELIC_MAGIC_BUDGET_FACTOR * program_size;

	return (limit);
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

	relic_pred_define(magic->ctx, *idp);

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
	relic_location_t where;
	const char *name;
	char *text;
	size_t name_length;
	size_t length;
	uint16_t arity;
	uint16_t column;

	entry = relic_pred_of(magic->ctx, source);
	name = relic_symbol_bytes(magic->ctx, entry->name, &name_length);
	arity = entry->arity;
	where = entry->where;

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
	    length - MAGIC_PREFIX_LENGTH, arity, &where,
	    &adornment->adorned) != 0)
		return (-1);

	if (bound == 0)
		return (0);

	return (intern_generated(magic, text, length, column_count(bound),
	    &where, &adornment->magic));
}

static uint32_t
adornment_hash(const relic_magic_t *magic, relic_pred_id_t source,
    uint32_t bound)
{
	uint32_t key[2];

	key[0] = source;
	key[1] = bound;

	return (relic_hash_words(magic->hash_seed, key, 2));
}

static uint32_t
rehash_adornment(uint32_t id, void *ctx)
{
	const relic_adornment_t *adornment;
	const relic_magic_t *magic;

	magic = ctx;
	adornment = &magic->adornments[id - 1];

	return (adornment_hash(magic, adornment->source, adornment->bound));
}

/*
 * Record a new adornment at the end of the worklist, naming its predicates.
 * Room in the array and in the index is made before anything is interned, so
 * that running out of memory leaves no half-recorded adornment.
 */
static int
record_adornment(relic_magic_t *magic, relic_pred_id_t source, uint32_t bound,
    uint32_t hash, relic_adornment_t *adornmentp)
{
	relic_adornment_t *adornments;
	size_t generated;

	generated = bound == 0 ? 1 : 2;
	if (confirm_pred_limit(magic, source, generated) != 0)
		return (-1);

	if (magic->adornment_count >= UINT32_MAX - 1) {
		relic_diagf(magic->ctx, RELIC_ERROR, NULL,
		    "a query's Magic rewrite generated more adornments than it "
		    "can index");
		return (-1);
	}

	adornments = relic_grow_array(magic->adornments,
	    &magic->adornment_capacity, magic->adornment_count + 1,
	    sizeof(*magic->adornments));
	if (adornments == NULL) {
		relic_diag_no_memory(magic->ctx);
		return (-1);
	}
	magic->adornments = adornments;

	if (relic_idtab_reserve(&magic->adornment_index, rehash_adornment,
	    magic) != 0) {
		relic_diag_no_memory(magic->ctx);
		return (-1);
	}

	if (name_adornment(magic, source, bound, adornmentp) != 0)
		return (-1);

	magic->adornments[magic->adornment_count] = *adornmentp;
	magic->adornment_count++;
	magic->generated_preds += generated;
	relic_idtab_place(&magic->adornment_index, hash,
	    (uint32_t)magic->adornment_count);

	return (0);
}

/*
 * The adornment for calling `source` with the columns of `bound` bound,
 * recording it if this is the first call to ask for it.
 */
static int
find_adornment(relic_magic_t *magic, relic_pred_id_t source, uint32_t bound,
    relic_adornment_t *adornmentp)
{
	const relic_adornment_t *candidate;
	relic_idprobe_t probe;
	uint32_t hash;
	uint32_t id;

	hash = adornment_hash(magic, source, bound);

	relic_idprobe_open(&probe, &magic->adornment_index, hash);
	while (relic_idprobe_next(&probe, &id)) {
		candidate = &magic->adornments[id - 1];
		if (candidate->source == source && candidate->bound == bound) {
			*adornmentp = *candidate;
			return (0);
		}
	}

	return (record_adornment(magic, source, bound, hash, adornmentp));
}

/*
 * Give a source slot its slot in the generated rule, assigning the next one
 * the first time the slot occurs.
 */
static void
renumber_arg(relic_renumbering_t *renumbering, const relic_arg_t *source,
    relic_arg_t *copy)
{
	uint16_t slot;

	*copy = *source;
	if (source->kind != RELIC_ARG_VARIABLE)
		return;

	slot = source->u.slot;
	if (renumbering->renamed[slot] == RELIC_NO_SLOT) {
		renumbering->renamed[slot] = renumbering->count;
		renumbering->original[renumbering->count] = slot;
		renumbering->occurrences[renumbering->count] = 0;
		renumbering->count++;
	}

	copy->u.slot = renumbering->renamed[slot];
	renumbering->occurrences[copy->u.slot]++;
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

static int
copy_literal(relic_magic_t *magic, relic_renumbering_t *renumbering,
    const relic_literal_t *source, relic_literal_t *copy)
{
	uint16_t arity;

	*copy = *source;

	if (source->kind == RELIC_LITERAL_COMPARISON) {
		renumber_arg(renumbering, &source->u.comparison.left,
		    &copy->u.comparison.left);
		renumber_arg(renumbering, &source->u.comparison.right,
		    &copy->u.comparison.right);
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

static bool
literals_equal(const relic_magic_t *magic, const relic_literal_t *a,
    const relic_literal_t *b)
{
	if (a->kind != b->kind)
		return (false);

	if (a->kind == RELIC_LITERAL_COMPARISON)
		return (a->u.comparison.op == b->u.comparison.op &&
		    args_equal(&a->u.comparison.left, &b->u.comparison.left,
		    1) &&
		    args_equal(&a->u.comparison.right, &b->u.comparison.right,
		    1));

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

		if (literal->kind == RELIC_LITERAL_COMPARISON) {
			hash = hash_step(magic, hash,
			    (uint32_t)literal->u.comparison.op);
			hash = hash_arg(magic, hash,
			    &literal->u.comparison.left);
			hash = hash_arg(magic, hash,
			    &literal->u.comparison.right);
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
	relic_rule_t *entries;

	if (magic->rules.count >= UINT32_MAX - 1) {
		relic_diagf(magic->ctx, RELIC_ERROR, NULL,
		    "a query's Magic rewrite generated more rules than it can "
		    "index");
		return (-1);
	}

	entries = relic_grow_array(magic->rules.entries,
	    &magic->rules.capacity, magic->rules.count + 1,
	    sizeof(*magic->rules.entries));
	if (entries == NULL) {
		relic_diag_no_memory(magic->ctx);
		return (-1);
	}
	magic->rules.entries = entries;

	if (relic_idtab_reserve(&magic->rule_index, rehash_rule, magic) != 0) {
		relic_diag_no_memory(magic->ctx);
		return (-1);
	}

	assert(rule->head != RELIC_ID_NONE);
	magic->rules.entries[magic->rules.count] = *rule;
	magic->rules.count++;
	relic_idtab_place(&magic->rule_index, hash,
	    (uint32_t)magic->rules.count);

	return (0);
}

/*
 * Add a generated rule, renumbered, unless it derives nothing or is a
 * duplicate of one already generated.  Returns 0, or -1 with a diagnostic
 * reported: memory, or the limit on generated rules, which counts only the
 * rules kept.
 */
static int
publish_rule(relic_magic_t *magic, const relic_draft_t *draft)
{
	relic_rule_t rule;
	uint32_t hash;

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
 * The slots a literal binds once placed: every variable of a positive one.
 * A test binds nothing.
 */
static uint64_t
positive_slots(const relic_magic_t *magic, const relic_literal_t *literal)
{
	const relic_arg_t *args;
	uint64_t slots;
	uint16_t arity;
	uint16_t i;

	if (literal->kind != RELIC_LITERAL_POSITIVE)
		return (0);

	args = literal->u.atom.args;
	arity = pred_arity(magic, literal->u.atom.pred);

	slots = 0;
	for (i = 0; i < arity; i++) {
		if (args[i].kind == RELIC_ARG_VARIABLE)
			slots |= UINT64_C(1) << args[i].u.slot;
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
 * Whether a body literal negates a predicate the query does not read
 * completely.  None may: a negated literal is kept as written, so its
 * relation must be whole before the rewrite runs, and nothing evaluates a
 * predicate read on demand except through its adornments.  The walk that
 * finds the complete predicates marks every negated one it reaches, and the
 * rewrite reaches only what that walk did, so this never holds; if the two
 * ever disagreed the negation would read an empty relation and succeed,
 * silently, and no later check -- not stratification, which sees no cycle
 * -- would notice.
 */
static bool
negates_on_demand(const relic_magic_t *magic, const relic_literal_t *literal)
{
	return (literal->kind == RELIC_LITERAL_NEGATED &&
	    !pred_is_complete(magic, literal->u.atom.pred));
}

/*
 * The adornment a call asks for: its predicate with bound the columns that
 * hold a constant or a slot in `bound`, recorded if no earlier call asked
 * for it.
 */
static int
adornment_of_call(relic_magic_t *magic, const relic_literal_t *call,
    uint64_t bound, relic_adornment_t *adornmentp)
{
	uint32_t columns;
	uint16_t arity;

	arity = pred_arity(magic, call->u.atom.pred);
	columns = relic_plan_bound_columns(&call->u.atom, arity, bound);

	return (find_adornment(magic, call->u.atom.pred, columns, adornmentp));
}

/*
 * Generate the magic rule recording the call at body[position], made with
 * `adornment`: the call's bound arguments, derived from every literal before
 * it in the body.  The body is in the order the rule is evaluated in, guard
 * first, so what comes before the call is exactly what has bound its
 * arguments.  An adornment with nothing bound has no magic predicate and
 * records nothing.
 */
static int
record_call(relic_magic_t *magic, const relic_rule_t *rule,
    const relic_literal_t *body, size_t position,
    const relic_adornment_t *adornment)
{
	relic_draft_t draft;

	if (adornment->magic == RELIC_ID_NONE)
		return (0);

	bzero(&draft, sizeof(draft));
	draft.source = rule;
	draft.head = adornment->magic;
	draft.head_arity = column_count(adornment->bound);
	draft.head_args = bound_args(magic, body[position].u.atom.args,
	    pred_arity(magic, adornment->source), adornment->bound);
	if (draft.head_args == NULL)
		return (-1);
	draft.body = body;
	draft.body_length = position;
	draft.attributed = adornment->source;

	return (publish_rule(magic, &draft));
}

/*
 * Draft the body of `rule` rewritten for a call with the columns of `head`
 * bound, generating the magic rules of the calls it makes.
 *
 * The body is put in the order the planner would evaluate it with the head's
 * bound variables already known -- guard first, when the call binds anything
 * -- and walked in that order, keeping the set of slots bound so far.  Each
 * call on a predicate read on demand is renamed to the adornment it asks
 * for, after the magic rule recording it is generated from the literals
 * before it.  The draft has the source rule's head arguments and head arity;
 * its caller names the head.
 */
static int
rewrite_body(relic_magic_t *magic, const relic_rule_t *rule,
    const relic_adornment_t *head, relic_draft_t *draft)
{
	relic_adornment_t called;
	relic_literal_t *body;
	size_t *order;
	size_t length;
	size_t i;
	uint64_t bound;

	order = magic_alloc(magic, rule->body_length, sizeof(*order));
	if (order == NULL)
		return (-1);
	body = magic_alloc(magic, rule->body_length + 1, sizeof(*body));
	if (body == NULL)
		return (-1);

	bound = head_bound_slots(rule, head);
	if (relic_plan_order(magic->ctx, &magic->arena, rule, bound,
	    magic->incomplete, order) != 0)
		return (-1);

	length = 0;
	if (head->magic != RELIC_ID_NONE) {
		if (guard_literal(magic, rule, head, &body[0]) != 0)
			return (-1);
		length++;
	}

	for (i = 0; i < rule->body_length; i++) {
		body[length] = rule->body[order[i]];

		if (negates_on_demand(magic, &body[length])) {
			relic_diagf(magic->ctx, RELIC_ERROR, &rule->where,
			    "internal error: a query's Magic rewrite negates "
			    "a predicate it does not read completely");
			return (-1);
		}

		if (reads_on_demand(magic, &body[length])) {
			if (adornment_of_call(magic, &body[length], bound,
			    &called) != 0)
				return (-1);
			if (record_call(magic, rule, body, length,
			    &called) != 0)
				return (-1);
			body[length].u.atom.pred = called.adorned;
		}

		bound |= positive_slots(magic, &body[length]);
		length++;
	}

	bzero(draft, sizeof(*draft));
	draft->source = rule;
	draft->head_args = rule->head_args;
	draft->head_arity = rule->head_arity;
	draft->body = body;
	draft->body_length = length;

	return (0);
}

/*
 * Generate the adorned rule of one rule deriving an adornment's source: the
 * rewritten body over the adorned head.
 */
static int
adorn_rule(relic_magic_t *magic, const relic_rule_t *rule,
    const relic_adornment_t *adornment)
{
	relic_draft_t draft;

	if (rewrite_body(magic, rule, adornment, &draft) != 0)
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

	if (rewrite_body(magic, query, &unbound, &draft) != 0)
		return (-1);

	draft.head = query->head;

	return (canonicalize(magic, &draft, &magic->query));
}

/*
 * Generate the rules of one adornment: every rule deriving its source
 * predicate, rewritten for that call.
 */
static int
adorn_rules(relic_magic_t *magic, const relic_adornment_t *adornment)
{
	const relic_ruletab_t *rules;
	size_t first;
	size_t end;
	size_t i;

	rules = &magic->ctx->program.rules;
	first = magic->first_rule_of[adornment->source - 1];
	end = magic->first_rule_of[adornment->source];

	for (i = first; i < end; i++) {
		if (adorn_rule(magic, &rules->entries[magic->rules_by_head[i]],
		    adornment) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Rewrite a query whose complete predicates relic_magic_find_complete() has
 * found and the caller has evaluated: the query first, which records the
 * adornments its body calls for and generates the seeds, then the rules of
 * every adornment recorded, in the order recorded.
 *
 * Only predicates read on demand are adorned, and every one of them is
 * derived by some rule -- one no rule derives is complete -- so each
 * adornment's source has rules to generate from.
 *
 * An adornment is copied out of the array before its rules are generated,
 * because generating them may record more and move the array.
 */
int
relic_magic_rewrite(relic_magic_t *magic, const relic_rule_t *query)
{
	relic_adornment_t adornment;
	size_t i;

	magic->pred_limit = limit_or_budget(
	    magic->ctx->config.max_magic_preds,
	    magic->ctx->program.schedule.member_count);
	magic->rule_limit = limit_or_budget(
	    magic->ctx->config.max_magic_rules,
	    magic->ctx->program.rules.count);

	if (rewrite_query(magic, query) != 0)
		return (-1);
	magic->seed_count = magic->rules.count;

	for (i = 0; i < magic->adornment_count; i++) {
		adornment = magic->adornments[i];
		if (adorn_rules(magic, &adornment) != 0)
			return (-1);
	}

	return (0);
}
