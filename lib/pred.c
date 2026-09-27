/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "internal.h"

/*
 * Predicates interned to identifiers.
 *
 * A predicate is (name, arity), per architecture section 17, so foo/1 and
 * foo/2 are unrelated.  The table is the same open-addressing identifier
 * table the symbol pool and the value table use, with the pair as its key:
 * two mentions of one predicate return one identifier, and every later stage
 * indexes by that identifier rather than by a name and a number.
 *
 * Identifiers are assigned in order of first mention and are never reused.
 * The table never removes an entry, because nothing un-defines a predicate.
 */

/*
 * Identifiers are 32-bit and zero is reserved to mean "none", so the table
 * holds one fewer than a uint32_t can count.  A program with four billion
 * distinct predicates would have exhausted memory long before, but the
 * ceiling is checked rather than assumed: the alternative is an identifier
 * that wraps to an existing one and silently merges two predicates.
 */
#define	RELIC_PRED_MAX	(UINT32_MAX - 1u)

static relic_pred_t *pred_at(const relic_predtab_t *table,
		    relic_pred_id_t id);
static uint32_t	 pred_hash(uint64_t seed, uint32_t name, uint16_t arity);
static uint32_t	 pred_rehash(uint32_t id, void *ctx);
static int	 pred_store(relic_t *ctx, uint32_t name, uint16_t arity,
		    const relic_location_t *where, relic_pred_id_t *idp);
static relic_pred_t *pred_entry(relic_t *ctx, relic_pred_id_t id);

void
relic_predtab_init(relic_predtab_t *table, uint64_t hash_seed)
{
	bzero(table, sizeof(*table));
	relic_idtab_init(&table->index, false);
	table->hash_seed = hash_seed;
}

void
relic_predtab_reset(relic_predtab_t *table)
{
	relic_idtab_reset(&table->index);
	relic_free_array(table->entries);
	bzero(table, sizeof(*table));
}

/*
 * The predicate an identifier names.  Identifier zero is reserved, so the
 * array is indexed one lower; this is the only place that translation is
 * written.  The caller must have established that the identifier was
 * assigned.
 */
static relic_pred_t *
pred_at(const relic_predtab_t *table, relic_pred_id_t id)
{
	return (&table->entries[id - 1]);
}

/*
 * The hash of a (name, arity) pair.
 *
 * The pair is packed into one 64-bit word and hashed by the integer hash the
 * value table already uses, rather than by a hash function of its own.  The
 * packing is exact: an interned name is 32 bits and an arity is at most 32,
 * so no two distinct pairs collide before the hash sees them.
 */
static uint32_t
pred_hash(uint64_t seed, uint32_t name, uint16_t arity)
{
	uint64_t key;

	key = ((uint64_t)name << 16) | (uint64_t)arity;

	return (relic_hash_int64(seed, (int64_t)key));
}

/*
 * The hash of a predicate already interned, for the identifier table to use
 * when it grows.
 */
static uint32_t
pred_rehash(uint32_t id, void *ctx)
{
	const relic_predtab_t *table;
	const relic_pred_t *pred;

	table = ctx;
	pred = pred_at(table, id);

	return (pred_hash(table->hash_seed, pred->name, pred->arity));
}

/*
 * Append a predicate to the table and return the identifier assigned to it.
 *
 * The array is grown before the count advances, so a failure part way
 * through leaves a table that is smaller than it might have been but is
 * entirely consistent: the entry a caller could reach is one that was fully
 * written.
 */
static int
pred_store(relic_t *ctx, uint32_t name, uint16_t arity,
    const relic_location_t *where, relic_pred_id_t *idp)
{
	relic_predtab_t *table;
	relic_pred_t *entries;
	relic_pred_t *stored;

	table = &ctx->program.predicates;

	if (table->count >= RELIC_PRED_MAX) {
		relic_diagf(ctx, RELIC_ERROR, where,
		    "too many distinct predicates");
		return (-1);
	}

	entries = relic_grow_array(table->entries, &table->capacity,
	    table->count + 1, sizeof(*table->entries));
	if (entries == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	table->entries = entries;

	stored = &table->entries[table->count];
	bzero(stored, sizeof(*stored));
	stored->name = name;
	stored->views = RELIC_ID_NONE;
	stored->arity = arity;

	table->count++;
	*idp = (relic_pred_id_t)table->count;

	return (0);
}

/*
 * Intern a predicate, storing its identifier in *idp.  Returns 0, or -1 with
 * a diagnostic already reported.
 *
 * A failure is a resource the library could not obtain -- memory, or the
 * last of four billion predicate identifiers -- or an arity past the limit
 * below.  The first two say nothing about the statement being compiled, so a
 * caller that could not create a predicate has learned nothing about it.  The
 * third cannot reach a caller compiling a statement, because a statement
 * with a wider atom never parses.
 *
 * `where` is what an error names; it is not recorded, since interning a
 * predicate says nothing about whether anything reads it.
 *
 * No predicate may have more than RELIC_MAX_ARITY arguments: an index key is
 * a 32-bit column mask, and the evaluator builds a tuple in a fixed array of
 * that many columns.  The parser and the query compiler refuse a wider atom
 * where it is written, with a message about the text; this is the one path
 * every predicate takes, so the bound holds here for whatever creates one.
 */
int
relic_pred_intern(relic_t *ctx, uint32_t name, uint16_t arity,
    const relic_location_t *where, relic_pred_id_t *idp)
{
	relic_predtab_t *table;
	relic_idprobe_t probe;
	const relic_pred_t *candidate;
	uint32_t hash;
	uint32_t id;

	if (arity > RELIC_MAX_ARITY) {
		relic_diagf(ctx, RELIC_ERROR, where,
		    "a predicate may have at most %d arguments",
		    RELIC_MAX_ARITY);
		return (-1);
	}

	table = &ctx->program.predicates;
	hash = pred_hash(table->hash_seed, name, arity);

	relic_idprobe_open(&probe, &table->index, hash);
	while (relic_idprobe_next(&probe, &id)) {
		candidate = pred_at(table, id);
		if (candidate->name == name && candidate->arity == arity) {
			*idp = id;
			return (0);
		}
	}

	/*
	 * Room in the index is taken before the predicate is stored, so that
	 * the only step that can fail happens while there is still nothing to
	 * leave behind.  Storing first and indexing second would, on a failed
	 * index growth, leave a predicate no lookup could reach, and the next
	 * mention of the same pair would assign it a second identifier --
	 * which would make one predicate into two relations holding half the
	 * answer each.
	 */
	if (relic_idtab_reserve(&table->index, 1, pred_rehash, table) != 0) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	if (pred_store(ctx, name, arity, where, &id) != 0)
		return (-1);

	relic_idtab_place(&table->index, hash, id);
	*idp = id;

	return (0);
}

/*
 * The predicate an identifier names, or NULL for one this context never
 * assigned.
 */
const relic_pred_t *
relic_pred_of(const relic_t *ctx, relic_pred_id_t id)
{
	const relic_predtab_t *table;

	table = &ctx->program.predicates;
	if (id == RELIC_ID_NONE || id > table->count)
		return (NULL);

	return (pred_at(table, id));
}

size_t
relic_pred_count(const relic_t *ctx)
{
	return (ctx->program.predicates.count);
}

/*
 * The writable entry of an identifier, or NULL for one this context never
 * assigned.
 */
static relic_pred_t *
pred_entry(relic_t *ctx, relic_pred_id_t id)
{
	relic_predtab_t *table;

	table = &ctx->program.predicates;
	if (id == RELIC_ID_NONE || id > table->count)
		return (NULL);

	return (pred_at(table, id));
}

/*
 * Record that a rule's head names this predicate.
 */
void
relic_pred_name_head(relic_t *ctx, relic_pred_id_t id)
{
	relic_pred_t *pred;

	pred = pred_entry(ctx, id);
	if (pred != NULL)
		pred->head_named = true;
}

/*
 * Record that a rule with this predicate as its head has been appended to a
 * rule table.  Only relic_ruletab_append() calls this.
 */
void
relic_pred_mark_derived(relic_t *ctx, relic_pred_id_t id)
{
	relic_pred_t *pred;

	pred = pred_entry(ctx, id);
	if (pred != NULL)
		pred->derived = true;
}

/*
 * Record that an atom at `where` reads this predicate.  The first such atom
 * is the one a warning points at.
 */
void
relic_pred_reference(relic_t *ctx, relic_pred_id_t id,
    const relic_location_t *where)
{
	relic_pred_t *pred;

	pred = pred_entry(ctx, id);
	if (pred == NULL || pred->referenced)
		return;

	pred->referenced = true;
	pred->referenced_at = *where;
}

/*
 * Make `view` the base view of `viewed`: a predicate that reads viewed's
 * base store.  A view is made once, when it is first interned.
 */
void
relic_pred_make_view(relic_t *ctx, relic_pred_id_t view,
    relic_pred_id_t viewed)
{
	relic_pred_t *pred;

	pred = pred_entry(ctx, view);
	if (pred != NULL)
		pred->views = viewed;
}

bool
relic_pred_is_derived(const relic_t *ctx, relic_pred_id_t id)
{
	const relic_pred_t *pred;

	pred = relic_pred_of(ctx, id);

	return (pred != NULL && pred->derived);
}

/*
 * Whether something derives into this predicate: a rule's head names it,
 * it is derived, it is a base view, or its base store holds tuples.
 */
bool
relic_pred_is_defined(const relic_t *ctx, relic_pred_id_t id)
{
	const relic_pred_t *pred;

	pred = relic_pred_of(ctx, id);
	if (pred == NULL)
		return (false);

	return (pred->head_named || pred->derived ||
	    pred->views != RELIC_ID_NONE || relic_base_count(ctx, id) > 0);
}

/*
 * Write "name/arity" into `buffer` and return it, for a message about a
 * predicate.
 *
 * Every byte of a predicate name is an identifier byte, so the name is safe
 * to quote without escaping; it is truncated because it can be arbitrarily
 * long and a message names one thing.  An identifier the context never
 * assigned cannot arise from a compiled program, but the accessor is total
 * so that a message can never be the thing that faults.
 */
const char *
relic_pred_describe(const relic_t *ctx, relic_pred_id_t id, char *buffer,
    size_t size)
{
	const relic_pred_t *pred;
	const char *name;
	char shortened[RELIC_NAME_TEXT_MAX];

	pred = relic_pred_of(ctx, id);
	if (pred == NULL) {
		snprintf(buffer, size, "?");
		return (buffer);
	}

	name = relic_symbol_bytes(ctx, pred->name, NULL);
	if (name == NULL)
		name = "?";

	snprintf(buffer, size, "%s/%u",
	    relic_diag_name(name, shortened, sizeof(shortened)),
	    (unsigned int)pred->arity);

	return (buffer);
}

/*
 * Warn about every predicate the program reads and nothing defines.
 *
 * Reported once per predicate, at the first atom that read it: such
 * a predicate is a typo far more often than an intentionally empty relation,
 * and the consequence is silent, because every positive literal over an
 * empty relation fails and every negated one succeeds.
 *
 * The sweep is over the whole table rather than over what a check just
 * compiled, because its answer is not incremental: a predicate referenced by
 * an early load may be defined by a later one, so the sweep must see every
 * definition before it speaks.  Predicates already reported are skipped, so
 * repeating the sweep says nothing twice.
 */
void
relic_pred_report_undefined(relic_t *ctx)
{
	relic_predtab_t *table;
	relic_pred_t *pred;
	char described[RELIC_PRED_TEXT_MAX];
	size_t position;

	table = &ctx->program.predicates;

	for (position = 0; position < table->count; position++) {
		pred = &table->entries[position];

		if (!pred->referenced || pred->reported_undefined ||
		    relic_pred_is_defined(ctx,
		    (relic_pred_id_t)(position + 1)))
			continue;

		pred->reported_undefined = true;

		relic_diagf(ctx, RELIC_WARNING, &pred->referenced_at,
		    "predicate %s is referenced but never defined",
		    relic_pred_describe(ctx,
		    (relic_pred_id_t)(position + 1), described,
		    sizeof(described)));
	}
}
