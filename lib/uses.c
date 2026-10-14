/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * The scope of a query run's direct grouped reductions (lib/reduce.h,
 * specs/design-group-reduction.md): which sources the run reduces rather
 * than stores, and which rules their keys stand in for.
 *
 * A source may lose its rows only when nothing that runs could read them,
 * and that is proved from an inventory of every use the program, the query
 * and the query's group rules make of every predicate, taken in one pass
 * over their bodies before anything is evaluated.  Each literal that reads
 * a relation is one use of its predicate, of one of three sorts:
 *
 *	a fold		an aggregate literal folding the predicate, on the
 *			literal's key columns
 *	a projection	the one literal of a program rule whose head holds
 *			exactly the literal's variables, each once, with
 *			every other argument `_`: the rule derives the
 *			predicate's distinct keys on those columns
 *	anything else	a positive literal in any other rule or in a query,
 *			and every negated one, which read rows
 *
 * An aggregate's compiled body is not a use of its own: a body folded in
 * place is the fold, and a body compiled into a group rule is read through
 * that rule, which the inventory walks as it does any other.  Counting
 * every rule of the program, not only those the query reaches, can keep a
 * source stored that need not be, and never the reverse.
 *
 * A source is admitted when it has a fold, every fold is on one key, every
 * other use is a projection of that key, and its rows come from one rule of
 * a component of its own that reads none of them, with no facts beside it:
 * the conditions under which that rule's plan, once certified distinct at
 * its turn, gives every row exactly once.  The query must read it
 * completely, which reaching a fold of it makes it do, and nothing may
 * fold it within its own rule.  A projection must be the one rule of a
 * component of its own with no facts, so that the keys are all its
 * predicate holds.  A predicate that is a projection of an admitted source
 * stays stored, filled from that source's keys, rather than being reduced
 * itself.
 *
 * The projections the query reaches are made complete in the rewrite, so
 * that they are evaluated from the program's schedule, where the keys can
 * replace their rules, and the rewrite reads them rather than generating
 * rules that would read the source's rows.
 *
 * The same pass records the run's fold families: for each predicate and
 * each key it is folded on, short of every column, the signatures of all
 * its folds on that key.  A family's summary is built over the stored
 * relation when a component's plans would fold it (lib/summarize.c), so
 * the signatures every later fold needs are known before it is built and
 * a built summary never has to grow.  A fold whose family has a summary
 * reads it; any other fold of a stored relation reads its rows.  The
 * whole program's materialization has a scope of its own for the
 * families, with no query and no source admitted.
 */

typedef struct relic_use_tally relic_use_tally_t;

/*
 * One predicate's uses as the inventory counts them: whether it has folds
 * and projections, the key mask of the first of each, and `blocked` once a
 * use of another sort, or a fold on another key, is met.  `admitted` and
 * `projected` are the selection's marks: an admitted source, and a
 * predicate some admitted source's projection derives.
 */
struct relic_use_tally {
	uint32_t	fold_mask;
	uint32_t	projection_mask;
	bool		folded;
	bool		projected_from;
	bool		blocked;
	bool		admitted;
	bool		projected;
};

static bool	 any_fold(const relic_t *ctx, const relic_query_t *query);
static void	*scope_alloc(relic_t *ctx, relic_reduction_t *reduction,
		    size_t count, size_t size);
static relic_use_tally_t *tally_of(relic_use_tally_t *tallies,
		    size_t count, relic_pred_id_t pred);
static bool	 projects_key(const relic_t *ctx, const relic_rule_t *rule,
		    uint32_t *maskp);
static void	 tally_rule(const relic_t *ctx, relic_use_tally_t *tallies,
		    size_t count, const relic_rule_t *rule, bool program_rule);
static size_t	 own_component(const relic_t *ctx,
		    const relic_magic_t *magic, relic_pred_id_t pred);
static const relic_rule_t *only_rule(const relic_t *ctx, size_t position);
static bool	 has_aggregate(const relic_rule_t *rule);
static bool	 may_reduce(const relic_t *ctx, const relic_magic_t *magic,
		    relic_pred_id_t pred, const relic_use_tally_t *tally);
static bool	 may_project(const relic_t *ctx, const relic_magic_t *magic,
		    relic_pred_id_t pred);
static void	 admit_sources(const relic_t *ctx, const relic_magic_t *magic,
		    relic_use_tally_t *tallies, size_t count);
static int	 record_scope(relic_t *ctx, relic_reduction_t *reduction,
		    relic_use_tally_t *tallies, size_t count);
static void	 permute_keys(const relic_rule_t *rule, uint32_t key_mask,
		    uint16_t *keys);
static int	 promote_projections(relic_reduction_t *reduction,
		    relic_magic_t *magic, relic_t *ctx);
static int	 add_signatures(relic_t *ctx, relic_reduction_t *reduction,
		    const relic_rule_t *rule);
static int	 add_signature(relic_t *ctx, relic_reduction_t *reduction,
		    relic_reduce_signature_t **signaturesp, size_t *countp,
		    bool *countsp, const relic_aggregate_t *aggregate);
static size_t	 signature_of(const relic_reduce_signature_t *signatures,
		    size_t count, const relic_aggregate_t *aggregate);
static bool	 holds_fold(const relic_reduce_signature_t *signatures,
		    size_t count, bool counts, const relic_aggregate_t *aggregate,
		    uint16_t *atp);
static relic_reduced_source_t *source_of(relic_reduction_t *reduction,
		    relic_pred_id_t pred);
static size_t	 count_folds(const relic_rule_t *rule);
static int	 record_families(relic_t *ctx, relic_reduction_t *reduction,
		    const relic_query_t *query);
static int	 add_families(relic_t *ctx, relic_reduction_t *reduction,
		    const relic_rule_t *rule);
static relic_fold_family_t *family_of(relic_reduction_t *reduction,
		    relic_pred_id_t pred, uint32_t key_mask);
static int	 family_fold(relic_t *ctx, relic_reduction_t *reduction,
		    const relic_aggregate_t *aggregate,
		    const relic_summary_t **summaryp, uint16_t *signaturep);

void
relic_reduction_begin(relic_reduction_t *reduction, relic_t *ctx)
{
	bzero(reduction, sizeof(*reduction));
	relic_arena_init_charged(&reduction->arena, &ctx->account);
	if ((ctx->config.flags & RELIC_F_NO_GROUP_REDUCTION) == 0)
		ctx->reduction = reduction;
}

void
relic_reduction_end(relic_reduction_t *reduction, relic_t *ctx)
{
	size_t i;

	for (i = 0; i < reduction->source_count; i++)
		relic_summary_destroy(ctx, reduction->sources[i].summary);
	for (i = 0; i < reduction->family_count; i++)
		relic_summary_destroy(ctx, reduction->families[i].summary);
	relic_arena_reset(&reduction->arena);
	if (ctx->reduction == reduction)
		ctx->reduction = NULL;
	reduction->sources = NULL;
	reduction->projections = NULL;
	reduction->families = NULL;
	reduction->source_of = NULL;
	reduction->projection_of = NULL;
	reduction->family_of = NULL;
	reduction->source_count = 0;
	reduction->projection_count = 0;
	reduction->family_count = 0;
	reduction->predicate_count = 0;
}

/*
 * Record the run's fold families, then, for a query, choose its sources
 * and projections, and make the projections the query reaches complete in
 * `magic`, which relic_magic_find_complete() has just walked.  The whole
 * program's materialization passes neither a query nor `magic`, and
 * admits no source.  Nothing is chosen when the run folds nothing, and a
 * scope that chose nothing is a run without reductions: no failure here
 * leaves anything half chosen, since the scope is published only whole.
 */
int
relic_reduction_select(relic_t *ctx, relic_magic_t *magic,
    const relic_query_t *query)
{
	relic_reduction_t *reduction;
	relic_use_tally_t *tallies;
	const relic_ruletab_t *rules;
	size_t count;
	size_t i;

	reduction = ctx->reduction;
	if (reduction == NULL || !any_fold(ctx, query))
		return (0);

	count = relic_pred_count(ctx);
	reduction->predicate_count = count;
	if (record_families(ctx, reduction, query) != 0)
		return (-1);
	if (magic == NULL)
		return (0);

	tallies = scope_alloc(ctx, reduction, count, sizeof(*tallies));
	if (tallies == NULL)
		return (-1);
	bzero(tallies, count * sizeof(*tallies));

	rules = &ctx->program.rules;
	for (i = 0; i < rules->count; i++)
		tally_rule(ctx, tallies, count, &rules->entries[i], true);
	tally_rule(ctx, tallies, count, &query->rule, false);
	for (i = 0; i < query->group_rule_count; i++)
		tally_rule(ctx, tallies, count, &query->group_rules[i], false);

	admit_sources(ctx, magic, tallies, count);
	if (record_scope(ctx, reduction, tallies, count) != 0)
		return (-1);

	for (i = 0; i < rules->count; i++)
		if (add_signatures(ctx, reduction, &rules->entries[i]) != 0)
			return (-1);
	if (add_signatures(ctx, reduction, &query->rule) != 0)
		return (-1);
	for (i = 0; i < query->group_rule_count; i++)
		if (add_signatures(ctx, reduction,
		    &query->group_rules[i]) != 0)
			return (-1);

	return (promote_projections(reduction, magic, ctx));
}

/*
 * Whether any rule the run could evaluate folds a relation, without which
 * nothing can be reduced, and a program-sized inventory would be memory
 * spent for nothing.
 */
static bool
any_fold(const relic_t *ctx, const relic_query_t *query)
{
	const relic_ruletab_t *rules;
	size_t i;

	rules = &ctx->program.rules;
	for (i = 0; i < rules->count; i++)
		if (has_aggregate(&rules->entries[i]))
			return (true);

	return (query != NULL && has_aggregate(&query->rule));
}

/*
 * Allocate from the scope's arena, reporting exhaustion.
 */
static void *
scope_alloc(relic_t *ctx, relic_reduction_t *reduction, size_t count,
    size_t size)
{
	void *block;

	block = relic_arena_allocarray(&reduction->arena, count, size);
	if (block == NULL)
		relic_diag_no_memory(ctx);

	return (block);
}

/*
 * A predicate's tally, or NULL for one made after the inventory began.
 */
static relic_use_tally_t *
tally_of(relic_use_tally_t *tallies, size_t count, relic_pred_id_t pred)
{
	if (pred == RELIC_ID_NONE || pred - 1 >= count)
		return (NULL);

	return (&tallies[pred - 1]);
}

/*
 * Whether a rule is a key projection of the predicate its one literal
 * reads, with the literal's key columns in *maskp: one positive literal,
 * whose arguments are distinct variables or `_`, under a head that holds
 * every one of those variables once and nothing else.  The head having as
 * many arguments as the literal has variables, each one of them and none
 * twice, is what makes it hold each exactly once.
 */
static bool
projects_key(const relic_t *ctx, const relic_rule_t *rule, uint32_t *maskp)
{
	const relic_atom_t *atom;
	const relic_pred_t *pred;
	uint64_t variables;
	uint64_t headed;
	uint64_t bit;
	uint32_t mask;
	uint16_t column;
	uint16_t named;
	uint16_t slot;
	uint16_t i;

	if (rule->head == RELIC_ID_NONE || rule->body_length != 1 ||
	    rule->body[0].kind != RELIC_LITERAL_POSITIVE)
		return (false);
	atom = &rule->body[0].u.atom;
	pred = relic_pred_of(ctx, atom->pred);
	if (pred == NULL)
		return (false);

	variables = 0;
	mask = 0;
	named = 0;
	for (column = 0; column < pred->arity; column++) {
		if (atom->args[column].kind == RELIC_ARG_WILDCARD)
			continue;
		if (atom->args[column].kind != RELIC_ARG_VARIABLE)
			return (false);
		slot = atom->args[column].u.slot;
		if (slot >= 64)
			return (false);
		bit = (uint64_t)1 << slot;
		if ((variables & bit) != 0)
			return (false);
		variables |= bit;
		mask |= (uint32_t)1 << column;
		named++;
	}

	if (rule->head_arity != named)
		return (false);
	headed = 0;
	for (i = 0; i < rule->head_arity; i++) {
		if (rule->head_args[i].kind != RELIC_ARG_VARIABLE)
			return (false);
		slot = rule->head_args[i].u.slot;
		if (slot >= 64)
			return (false);
		bit = (uint64_t)1 << slot;
		if ((variables & bit) == 0 || (headed & bit) != 0)
			return (false);
		headed |= bit;
	}

	*maskp = mask;
	return (true);
}

/*
 * Count the uses one rule's body makes.  Only a program rule can be a
 * projection: a query's rule names no head, and a query's group rule is
 * evaluated by the query, not from the schedule where keys replace rules.
 */
static void
tally_rule(const relic_t *ctx, relic_use_tally_t *tallies, size_t count,
    const relic_rule_t *rule, bool program_rule)
{
	const relic_aggregate_t *aggregate;
	const relic_literal_t *literal;
	relic_use_tally_t *tally;
	uint32_t mask;
	size_t i;

	for (i = 0; i < rule->body_length; i++) {
		literal = &rule->body[i];
		switch (literal->kind) {
		case RELIC_LITERAL_POSITIVE:
			tally = tally_of(tallies, count, literal->u.atom.pred);
			if (tally == NULL)
				break;
			if (!program_rule || !projects_key(ctx, rule, &mask))
				tally->blocked = true;
			else if (!tally->projected_from) {
				tally->projected_from = true;
				tally->projection_mask = mask;
			} else if (tally->projection_mask != mask)
				tally->blocked = true;
			break;
		case RELIC_LITERAL_NEGATED:
			tally = tally_of(tallies, count, literal->u.atom.pred);
			if (tally != NULL)
				tally->blocked = true;
			break;
		case RELIC_LITERAL_AGGREGATE:
			aggregate = &literal->u.aggregate;
			tally = tally_of(tallies, count, aggregate->source);
			if (tally == NULL)
				break;
			if (!tally->folded) {
				tally->folded = true;
				tally->fold_mask = aggregate->key_mask;
			} else if (tally->fold_mask != aggregate->key_mask)
				tally->blocked = true;
			break;
		case RELIC_LITERAL_COMPARISON:
			break;
		}
	}
}

/*
 * The position in the program's schedule of the component deriving
 * `pred`, when it is one of one predicate and one rule that does not read
 * its own head; SIZE_MAX otherwise, and for a predicate no rule derives.
 */
static size_t
own_component(const relic_t *ctx, const relic_magic_t *magic,
    relic_pred_id_t pred)
{
	const relic_component_t *component;
	size_t position;

	position = relic_magic_component_of(magic, pred);
	if (position == SIZE_MAX)
		return (SIZE_MAX);
	component = &ctx->program.schedule.components[position];
	if (component->member_count != 1 || component->rule_count != 1 ||
	    component->recursive)
		return (SIZE_MAX);

	return (position);
}

static const relic_rule_t *
only_rule(const relic_t *ctx, size_t position)
{
	const relic_schedule_t *schedule;
	size_t index;

	schedule = &ctx->program.schedule;
	index = schedule->rule_indexes[schedule->components[position].first_rule];
	return (&ctx->program.rules.entries[index]);
}

static bool
has_aggregate(const relic_rule_t *rule)
{
	size_t i;

	for (i = 0; i < rule->body_length; i++)
		if (rule->body[i].kind == RELIC_LITERAL_AGGREGATE)
			return (true);

	return (false);
}

/*
 * Whether a predicate the inventory found only folded on one key, and
 * otherwise only projected on that key, may be reduced.  It must be one
 * the program writes or the group predicate of one of its aggregates --
 * not a group predicate -X group-rules asked to keep, nor a base view --
 * derived by its own component's one rule with no facts beside it, and
 * the query must read it completely.  A rule holding an aggregate is not
 * certified, since its group's turn is its own.
 */
static bool
may_reduce(const relic_t *ctx, const relic_magic_t *magic,
    relic_pred_id_t pred, const relic_use_tally_t *tally)
{
	const relic_pred_t *info;
	size_t position;

	if (!tally->folded || tally->blocked)
		return (false);
	if (tally->projected_from && tally->projection_mask != tally->fold_mask)
		return (false);

	info = relic_pred_of(ctx, pred);
	if (info == NULL || info->views != RELIC_ID_NONE)
		return (false);
	if (info->kind == RELIC_PRED_GROUP) {
		if ((ctx->config.flags & RELIC_F_GROUP_RULES) != 0)
			return (false);
	} else if (info->kind != RELIC_PRED_WRITTEN)
		return (false);
	if (relic_base_count(ctx, pred) != 0)
		return (false);

	position = own_component(ctx, magic, pred);
	if (position == SIZE_MAX || !magic->complete_components[position])
		return (false);

	return (!has_aggregate(only_rule(ctx, position)));
}

/*
 * Whether the head of a key projection can be filled from its source's
 * keys: one the program writes, or a group predicate, derived by its own
 * component's one rule with no facts beside it.
 */
static bool
may_project(const relic_t *ctx, const relic_magic_t *magic,
    relic_pred_id_t pred)
{
	const relic_pred_t *info;

	info = relic_pred_of(ctx, pred);
	if (info == NULL || info->views != RELIC_ID_NONE ||
	    info->kind == RELIC_PRED_REWRITTEN)
		return (false);
	if (relic_base_count(ctx, pred) != 0)
		return (false);

	return (own_component(ctx, magic, pred) != SIZE_MAX);
}

/*
 * Mark the sources the run reduces and the projections their keys fill.
 * A source one of whose projections cannot be filled from its keys is not
 * admitted.  A source another admitted source projects into then stays
 * stored, filled from that one's keys: the projection's rule is replaced,
 * so it is not there to be reduced.
 */
static void
admit_sources(const relic_t *ctx, const relic_magic_t *magic,
    relic_use_tally_t *tallies, size_t count)
{
	const relic_ruletab_t *rules;
	const relic_rule_t *rule;
	relic_use_tally_t *tally;
	relic_pred_id_t source;
	uint32_t mask;
	size_t i;

	for (i = 0; i < count; i++)
		tallies[i].admitted = may_reduce(ctx, magic,
		    (relic_pred_id_t)(i + 1), &tallies[i]);

	rules = &ctx->program.rules;
	for (i = 0; i < rules->count; i++) {
		rule = &rules->entries[i];
		if (!projects_key(ctx, rule, &mask))
			continue;
		source = rule->body[0].u.atom.pred;
		tally = tally_of(tallies, count, source);
		if (tally == NULL || !tally->admitted)
			continue;
		if (!may_project(ctx, magic, rule->head))
			tally->admitted = false;
	}

	for (i = 0; i < rules->count; i++) {
		rule = &rules->entries[i];
		if (!projects_key(ctx, rule, &mask))
			continue;
		tally = tally_of(tallies, count, rule->body[0].u.atom.pred);
		if (tally != NULL && tally->admitted)
			tallies[rule->head - 1].projected = true;
	}
	for (i = 0; i < count; i++)
		if (tallies[i].projected)
			tallies[i].admitted = false;
}

/*
 * Record the admitted sources and their projections in the scope, and
 * publish them.
 */
static int
record_scope(relic_t *ctx, relic_reduction_t *reduction,
    relic_use_tally_t *tallies, size_t count)
{
	relic_key_projection_t *projection;
	relic_reduced_source_t *source;
	const relic_ruletab_t *rules;
	const relic_rule_t *rule;
	relic_use_tally_t *tally;
	uint16_t *keys;
	uint32_t mask;
	size_t sources;
	size_t projections;
	size_t i;

	sources = 0;
	for (i = 0; i < count; i++)
		if (tallies[i].admitted)
			sources++;
	if (sources == 0)
		return (0);

	rules = &ctx->program.rules;
	projections = 0;
	for (i = 0; i < rules->count; i++) {
		rule = &rules->entries[i];
		if (!projects_key(ctx, rule, &mask))
			continue;
		tally = tally_of(tallies, count, rule->body[0].u.atom.pred);
		if (tally != NULL && tally->admitted)
			projections++;
	}

	reduction->source_of = scope_alloc(ctx, reduction, count,
	    sizeof(*reduction->source_of));
	reduction->projection_of = scope_alloc(ctx, reduction, count,
	    sizeof(*reduction->projection_of));
	reduction->sources = scope_alloc(ctx, reduction, sources,
	    sizeof(*reduction->sources));
	reduction->projections = scope_alloc(ctx, reduction,
	    projections > 0 ? projections : 1,
	    sizeof(*reduction->projections));
	if (reduction->source_of == NULL || reduction->projection_of == NULL ||
	    reduction->sources == NULL || reduction->projections == NULL)
		return (-1);
	bzero(reduction->source_of, count * sizeof(*reduction->source_of));
	bzero(reduction->projection_of,
	    count * sizeof(*reduction->projection_of));

	for (i = 0; i < count; i++) {
		if (!tallies[i].admitted)
			continue;
		source = &reduction->sources[reduction->source_count];
		bzero(source, sizeof(*source));
		source->pred = (relic_pred_id_t)(i + 1);
		source->key_mask = tallies[i].fold_mask;
		source->state = RELIC_SOURCE_PROMISED;
		reduction->source_count++;
		reduction->source_of[i] = (uint32_t)reduction->source_count;
	}

	for (i = 0; i < rules->count; i++) {
		rule = &rules->entries[i];
		if (!projects_key(ctx, rule, &mask))
			continue;
		source = source_of(reduction, rule->body[0].u.atom.pred);
		if (source == NULL)
			continue;
		keys = scope_alloc(ctx, reduction,
		    rule->head_arity > 0 ? rule->head_arity : 1,
		    sizeof(*keys));
		if (keys == NULL)
			return (-1);
		permute_keys(rule, mask, keys);
		projection = &reduction->projections[
		    reduction->projection_count];
		projection->keys = keys;
		projection->pred = rule->head;
		projection->source = (size_t)(source - reduction->sources);
		reduction->projection_count++;
		reduction->projection_of[rule->head - 1] =
		    (uint32_t)reduction->projection_count;
	}

	return (0);
}

/*
 * Fill `keys` for a projection rule: head column i holds the variable the
 * literal binds at the key column whose position among the key columns,
 * in increasing order, is keys[i].
 */
static void
permute_keys(const relic_rule_t *rule, uint32_t key_mask, uint16_t *keys)
{
	const relic_atom_t *atom;
	uint16_t position;
	uint16_t column;
	uint16_t i;

	atom = &rule->body[0].u.atom;
	for (i = 0; i < rule->head_arity; i++) {
		position = 0;
		for (column = 0; column < 32; column++) {
			if ((key_mask & ((uint32_t)1 << column)) == 0)
				continue;
			if (atom->args[column].u.slot ==
			    rule->head_args[i].u.slot)
				break;
			position++;
		}
		keys[i] = position;
	}
}

/*
 * Make complete in the rewrite every projection the query reaches that it
 * would otherwise read on demand.
 */
static int
promote_projections(relic_reduction_t *reduction, relic_magic_t *magic,
    relic_t *ctx)
{
	size_t *positions;
	size_t position;
	size_t count;
	size_t i;

	if (reduction->projection_count == 0)
		return (0);

	positions = scope_alloc(ctx, reduction, reduction->projection_count,
	    sizeof(*positions));
	if (positions == NULL)
		return (-1);

	count = 0;
	for (i = 0; i < reduction->projection_count; i++) {
		position = relic_magic_component_of(magic,
		    reduction->projections[i].pred);
		if (!magic->complete_components[position] &&
		    relic_magic_reaches(magic, position))
			positions[count++] = position;
	}

	return (relic_magic_complete_also(magic, positions, count));
}

/*
 * Add the signatures one rule's folds of admitted sources need.
 */
static int
add_signatures(relic_t *ctx, relic_reduction_t *reduction,
    const relic_rule_t *rule)
{
	const relic_aggregate_t *aggregate;
	relic_reduced_source_t *source;
	size_t i;

	for (i = 0; i < rule->body_length; i++) {
		if (rule->body[i].kind != RELIC_LITERAL_AGGREGATE)
			continue;
		aggregate = &rule->body[i].u.aggregate;
		source = source_of(reduction, aggregate->source);
		if (source != NULL && add_signature(ctx, reduction,
		    &source->signatures, &source->signature_count,
		    &source->counts, aggregate) != 0)
			return (-1);
	}

	return (0);
}

/*
 * Add a fold's signature to the `*countp` of `*signaturesp`, a source's or
 * a family's, unless an earlier fold's is the same, or for a count set
 * *countsp, a count having no signature.  Either has few folds, so the
 * array grows by copying.
 */
static int
add_signature(relic_t *ctx, relic_reduction_t *reduction,
    relic_reduce_signature_t **signaturesp, size_t *countp, bool *countsp,
    const relic_aggregate_t *aggregate)
{
	relic_reduce_signature_t *signatures;

	if (aggregate->fn == RELIC_AGG_COUNT) {
		*countsp = true;
		return (0);
	}
	if (signature_of(*signaturesp, *countp, aggregate) < *countp)
		return (0);

	signatures = scope_alloc(ctx, reduction, *countp + 1,
	    sizeof(*signatures));
	if (signatures == NULL)
		return (-1);
	if (*countp > 0)
		bcopy(*signaturesp, signatures, *countp * sizeof(*signatures));
	signatures[*countp].fn = aggregate->fn;
	signatures[*countp].column = aggregate->column;
	*signaturesp = signatures;
	(*countp)++;

	return (0);
}

/*
 * The position of a fold's signature among `count` signatures, or `count`
 * when none is its.
 */
static size_t
signature_of(const relic_reduce_signature_t *signatures, size_t count,
    const relic_aggregate_t *aggregate)
{
	size_t i;

	for (i = 0; i < count; i++)
		if (signatures[i].fn == aggregate->fn &&
		    signatures[i].column == aggregate->column)
			return (i);

	return (count);
}

/*
 * Whether the `count` signatures and `counts` of a source or a family hold
 * a fold, with *atp set to its signature's position; a count has none,
 * since it reads its group's row count.
 */
static bool
holds_fold(const relic_reduce_signature_t *signatures, size_t count,
    bool counts, const relic_aggregate_t *aggregate, uint16_t *atp)
{
	size_t at;

	*atp = 0;
	if (aggregate->fn == RELIC_AGG_COUNT)
		return (counts);
	at = signature_of(signatures, count, aggregate);
	if (at == count)
		return (false);
	*atp = (uint16_t)at;

	return (true);
}

static relic_reduced_source_t *
source_of(relic_reduction_t *reduction, relic_pred_id_t pred)
{
	uint32_t at;

	if (reduction->source_of == NULL || pred == RELIC_ID_NONE ||
	    pred - 1 >= reduction->predicate_count)
		return (NULL);
	at = reduction->source_of[pred - 1];

	return (at == 0 ? NULL : &reduction->sources[at - 1]);
}

relic_reduced_source_t *
relic_reduction_source(relic_t *ctx, relic_pred_id_t pred)
{
	if (ctx->reduction == NULL)
		return (NULL);

	return (source_of(ctx->reduction, pred));
}

const relic_key_projection_t *
relic_reduction_projection(const relic_t *ctx, relic_pred_id_t pred)
{
	const relic_reduction_t *reduction;
	uint32_t at;

	reduction = ctx->reduction;
	if (reduction == NULL || reduction->projection_of == NULL ||
	    pred == RELIC_ID_NONE || pred - 1 >= reduction->predicate_count)
		return (NULL);
	at = reduction->projection_of[pred - 1];

	return (at == 0 ? NULL : &reduction->projections[at - 1]);
}

/*
 * Resolve a fold: to its source's summary and the signature's position in
 * it when the source is REDUCED, to its family's when the family has
 * been summarized, and otherwise to its relation's rows, leaving
 * *summaryp NULL.  A fold of a source planned before its source's turn,
 * or one whose key or signature the inventory did not record, is a
 * broken invariant: the inventory saw every fold and the schedule orders
 * every reader after its source.
 */
int
relic_reduction_fold(relic_t *ctx, const relic_aggregate_t *aggregate,
    const relic_summary_t **summaryp, uint16_t *signaturep)
{
	const relic_reduced_source_t *source;
	char name[RELIC_PRED_TEXT_MAX];

	*summaryp = NULL;
	*signaturep = 0;
	if (ctx->reduction == NULL)
		return (0);

	source = source_of(ctx->reduction, aggregate->source);
	if (source == NULL || source->state == RELIC_SOURCE_STORED)
		return (family_fold(ctx, ctx->reduction, aggregate, summaryp,
		    signaturep));

	if (source->state == RELIC_SOURCE_REDUCED &&
	    source->key_mask == aggregate->key_mask &&
	    holds_fold(source->signatures, source->signature_count,
	    source->counts, aggregate, signaturep)) {
		*summaryp = source->summary;
		return (0);
	}

	relic_diag_internal(ctx, &aggregate->where,
	    "a fold of %s does not match its reduction",
	    relic_pred_identify(ctx, aggregate->source, name, sizeof(name)));
	return (-1);
}

/*
 * Resolve a fold of a stored relation to its family's summary when the
 * family has one holding the fold's signature, which a family built from
 * the inventory always does.  The summary describes the relation as it
 * was built from: complete, since a component folds only relations an
 * earlier component finished, and nothing derives into a finished
 * relation for the rest of the run.  A relation that has grown since is a
 * broken invariant, reported rather than answered from the summary.
 */
static int
family_fold(relic_t *ctx, relic_reduction_t *reduction,
    const relic_aggregate_t *aggregate, const relic_summary_t **summaryp,
    uint16_t *signaturep)
{
	const relic_fold_family_t *family;
	const relic_relation_t *relation;
	char name[RELIC_PRED_TEXT_MAX];
	uint16_t at;

	family = family_of(reduction, aggregate->source, aggregate->key_mask);
	if (family == NULL || family->summary == NULL ||
	    !holds_fold(family->signatures, family->signature_count,
	    family->counts, aggregate, &at))
		return (0);

	relation = relic_relation_read(ctx, family->pred);
	if (relation == NULL)
		return (-1);
	if (relic_relation_count(relation) != family->built_at) {
		relic_diag_internal(ctx, &aggregate->where,
		    "%s has grown since its summary was built",
		    relic_pred_identify(ctx, family->pred, name,
		    sizeof(name)));
		return (-1);
	}

	*summaryp = family->summary;
	*signaturep = at;
	return (0);
}

relic_fold_family_t *
relic_reduction_family(relic_t *ctx, const relic_aggregate_t *aggregate)
{
	relic_fold_family_t *family;
	uint16_t at;

	if (ctx->reduction == NULL)
		return (NULL);

	family = family_of(ctx->reduction, aggregate->source,
	    aggregate->key_mask);
	if (family == NULL || family->summary != NULL ||
	    !holds_fold(family->signatures, family->signature_count,
	    family->counts, aggregate, &at))
		return (NULL);

	return (family);
}

/*
 * The aggregate literals of a rule's body.
 */
static size_t
count_folds(const relic_rule_t *rule)
{
	size_t count;
	size_t i;

	count = 0;
	for (i = 0; i < rule->body_length; i++)
		if (rule->body[i].kind == RELIC_LITERAL_AGGREGATE)
			count++;

	return (count);
}

/*
 * Record the families of every fold of the program's rules and, for a
 * query, of its rule and group rules.  There are at most as many families
 * as folds, which is what the array is sized for.
 */
static int
record_families(relic_t *ctx, relic_reduction_t *reduction,
    const relic_query_t *query)
{
	const relic_ruletab_t *rules;
	size_t folds;
	size_t i;

	rules = &ctx->program.rules;
	folds = 0;
	for (i = 0; i < rules->count; i++)
		folds += count_folds(&rules->entries[i]);
	if (query != NULL) {
		folds += count_folds(&query->rule);
		for (i = 0; i < query->group_rule_count; i++)
			folds += count_folds(&query->group_rules[i]);
	}
	if (folds == 0)
		return (0);

	reduction->families = scope_alloc(ctx, reduction, folds,
	    sizeof(*reduction->families));
	reduction->family_of = scope_alloc(ctx, reduction,
	    reduction->predicate_count, sizeof(*reduction->family_of));
	if (reduction->families == NULL || reduction->family_of == NULL)
		return (-1);
	bzero(reduction->family_of,
	    reduction->predicate_count * sizeof(*reduction->family_of));

	for (i = 0; i < rules->count; i++)
		if (add_families(ctx, reduction, &rules->entries[i]) != 0)
			return (-1);
	if (query == NULL)
		return (0);
	if (add_families(ctx, reduction, &query->rule) != 0)
		return (-1);
	for (i = 0; i < query->group_rule_count; i++)
		if (add_families(ctx, reduction, &query->group_rules[i]) != 0)
			return (-1);

	return (0);
}

/*
 * Add each fold of a rule to the family of its predicate and key, made
 * when it is the first fold of them.  A fold on every column reaches at
 * most one row however it is planned, and has no family.
 */
static int
add_families(relic_t *ctx, relic_reduction_t *reduction,
    const relic_rule_t *rule)
{
	const relic_aggregate_t *aggregate;
	relic_fold_family_t *family;
	const relic_pred_t *pred;
	uint32_t *link;
	size_t i;

	for (i = 0; i < rule->body_length; i++) {
		if (rule->body[i].kind != RELIC_LITERAL_AGGREGATE)
			continue;
		aggregate = &rule->body[i].u.aggregate;
		pred = relic_pred_of(ctx, aggregate->source);
		if (pred == NULL ||
		    aggregate->source - 1 >= reduction->predicate_count ||
		    aggregate->key_mask == relic_plan_all_columns(pred->arity))
			continue;

		family = family_of(reduction, aggregate->source,
		    aggregate->key_mask);
		if (family == NULL) {
			link = &reduction->family_of[aggregate->source - 1];
			while (*link != 0)
				link = &reduction->families[*link - 1].next;
			family = &reduction->families[reduction->family_count];
			bzero(family, sizeof(*family));
			family->pred = aggregate->source;
			family->key_mask = aggregate->key_mask;
			family->arity = pred->arity;
			reduction->family_count++;
			*link = (uint32_t)reduction->family_count;
		}
		if (add_signature(ctx, reduction, &family->signatures,
		    &family->signature_count, &family->counts,
		    aggregate) != 0)
			return (-1);
	}

	return (0);
}

/*
 * The family of `pred` on the key columns of `key_mask`, or NULL.
 */
static relic_fold_family_t *
family_of(relic_reduction_t *reduction, relic_pred_id_t pred,
    uint32_t key_mask)
{
	size_t at;

	if (reduction->family_of == NULL || pred == RELIC_ID_NONE ||
	    pred - 1 >= reduction->predicate_count)
		return (NULL);

	for (at = reduction->family_of[pred - 1]; at != 0;
	    at = reduction->families[at - 1].next) {
		if (reduction->families[at - 1].key_mask == key_mask)
			return (&reduction->families[at - 1]);
	}

	return (NULL);
}

