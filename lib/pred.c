/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
 * A name is not a value, so it is not interned in the symbol pool: the
 * table keeps its own copy in the name store, and compares the bytes of a
 * candidate with those asked for.  A predicate is interned once per atom
 * a statement writes or per fact a load stages, never per tuple an
 * evaluation derives, so the comparison costs nothing that matters.
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
static uint32_t	 pred_hash(uint64_t seed, const char *name, size_t length,
		    uint16_t arity);
static uint32_t	 pred_rehash(uint32_t id, void *ctx);
static int	 pred_store(relic_t *ctx, const char *name, size_t length,
		    uint16_t arity, uint32_t hash,
		    const relic_location_t *where, relic_pred_id_t *idp);
static relic_pred_t *pred_entry(relic_t *ctx, relic_pred_id_t id);
static const char *describe_aggregate_group(const relic_t *ctx,
		    const relic_pred_t *group, char *buffer, size_t size);
static const char *keep_name(relic_t *ctx, const char *bytes,
		    size_t length);
static const char *keep_source_name(relic_t *ctx, const char *name);
static uint32_t	 source_rehash(uint32_t position, void *ctx);
static const char *write_pred(const relic_t *ctx, relic_pred_id_t id,
		    size_t name_max, char *buffer, size_t size);

void
relic_predtab_init(relic_predtab_t *table, uint64_t hash_seed)
{
	bzero(table, sizeof(*table));
	relic_idtab_init(&table->index, false);
	relic_arena_init(&table->names, 1);
	relic_idtab_init(&table->source_index, true);
	table->hash_seed = hash_seed;
}

void
relic_predtab_reset(relic_predtab_t *table)
{
	relic_idtab_reset(&table->index);
	relic_idtab_reset(&table->source_index);
	relic_arena_reset(&table->names);
	free(table->entries);
	free(table->sources);
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
 * The hash of a (name, arity) pair: the hash of the name's bytes and the
 * arity, hashed together as two words, so that p/1 and p/2 land apart
 * although their names hash alike.
 */
static uint32_t
pred_hash(uint64_t seed, const char *name, size_t length, uint16_t arity)
{
	uint32_t key[2];

	key[0] = relic_hash_bytes(seed, name, length);
	key[1] = arity;

	return (relic_hash_words(seed, key, 2));
}

/*
 * The hash of a predicate already interned, for the identifier table to use
 * when it grows: the one it was placed by, kept in the entry.
 */
static uint32_t
pred_rehash(uint32_t id, void *ctx)
{
	const relic_predtab_t *table;

	table = ctx;

	return (pred_at(table, id)->hash);
}

/*
 * Copy `length` bytes into the name store, terminated, and return the copy,
 * or NULL when the memory limit or the allocator refused it, for the caller
 * to report through relic_diag_no_memory(), which names the limit when the
 * limit refused.
 *
 * The store is charged as the symbol pool is: a copy that would grow it is
 * admitted first, against the whole total, and the account is then set to
 * what the store holds, so the count cannot drift from it.  The store lives
 * as long as the context, so an allocator's refusal is cleared: left
 * standing, it would refuse every name after this one.
 */
static const char *
keep_name(relic_t *ctx, const char *bytes, size_t length)
{
	relic_arena_projection_t projected;
	relic_arena_t *names;
	size_t held_bytes;
	char *copy;

	names = &ctx->program.predicates.names;
	held_bytes = relic_arena_total_bytes(names);

	relic_arena_project(names, &projected);
	relic_arena_project_alloc(&projected, length + 1);
	if (projected.total_bytes != held_bytes &&
	    !relic_account_admit(&ctx->account, held_bytes,
	    projected.total_bytes))
		return (NULL);

	copy = relic_arena_alloc(names, length + 1);
	ctx->account.name_bytes = relic_arena_total_bytes(names);
	if (copy == NULL) {
		relic_arena_clear_failure(names);
		return (NULL);
	}
	if (length > 0)
		bcopy(bytes, copy, length);
	copy[length] = '\0';

	return (copy);
}

/*
 * Append a predicate to the table and return the identifier assigned to it.
 *
 * The array is grown and the name copied before the count advances, so a
 * failure part way through leaves a table that is smaller than it might have
 * been but is entirely consistent: the entry a caller could reach is one
 * that was fully written.
 */
static int
pred_store(relic_t *ctx, const char *name, size_t length, uint16_t arity,
    uint32_t hash, const relic_location_t *where, relic_pred_id_t *idp)
{
	relic_predtab_t *table;
	relic_pred_t *entries;
	relic_pred_t *stored;
	const char *kept;

	table = &ctx->program.predicates;

	if (table->count >= RELIC_PRED_MAX) {
		relic_diag_error(ctx, RELIC_ERROR_LIMIT, where,
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

	kept = keep_name(ctx, name, length);
	if (kept == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	stored = &table->entries[table->count];
	bzero(stored, sizeof(*stored));
	stored->name.bytes = kept;
	stored->name.length = (uint32_t)length;
	stored->hash = hash;
	stored->views = RELIC_ID_NONE;
	stored->stands_for = RELIC_ID_NONE;
	stored->kind = RELIC_PRED_WRITTEN;
	stored->arity = arity;

	table->count++;
	*idp = (relic_pred_id_t)table->count;

	return (0);
}

/*
 * Intern a predicate, the `length` bytes of `name` at `arity`, storing its
 * identifier in *idp.  Returns 0, or -1 with a diagnostic already reported.
 * The bytes are the caller's and are copied when the predicate is new.
 *
 * A failure is a resource the library could not obtain -- memory, or the
 * last of four billion predicate identifiers -- or an arity or a name past
 * the limits below.  The first two say nothing about the statement being
 * compiled, so a caller that could not create a predicate has learned
 * nothing about it.  The others cannot reach a caller compiling a
 * statement, because a statement with a wider atom never parses, and a name
 * is an identifier the text spelled or a generated one built from it.
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
relic_pred_intern(relic_t *ctx, const char *name, size_t length,
    uint16_t arity, const relic_location_t *where, relic_pred_id_t *idp)
{
	relic_predtab_t *table;
	relic_idprobe_t probe;
	const relic_pred_t *candidate;
	relic_spelling_t asked;
	uint32_t hash;
	uint32_t id;

	if (arity > RELIC_MAX_ARITY) {
		relic_diag_internal(ctx, where,
		    "a predicate of %u arguments was interned, more than the "
		    "%d any may have", (unsigned int)arity, RELIC_MAX_ARITY);
		return (-1);
	}
	if (length > RELIC_STRING_LENGTH_MAX) {
		relic_diag_error(ctx, RELIC_ERROR_REJECTED, where,
		    "a predicate name of %zu bytes is longer than the %lu a "
		    "name may hold", length,
		    (unsigned long)RELIC_STRING_LENGTH_MAX);
		return (-1);
	}

	table = &ctx->program.predicates;
	hash = pred_hash(table->hash_seed, name, length, arity);
	asked.bytes = name;
	asked.length = (uint32_t)length;

	relic_idprobe_open(&probe, &table->index, hash);
	while (relic_idprobe_next(&probe, &id)) {
		candidate = pred_at(table, id);
		if (candidate->arity == arity &&
		    relic_spelling_equal(&candidate->name, &asked)) {
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

	if (pred_store(ctx, name, length, arity, hash, where, &id) != 0)
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
 * Record that the Magic rewrite generated this predicate, as part of
 * `stands_for`, or of the query when `stands_for` is RELIC_ID_NONE: a
 * rule's head names it, and a diagnostic names what it is part of in its
 * place.
 */
void
relic_pred_name_generated(relic_t *ctx, relic_pred_id_t id,
    relic_pred_id_t stands_for)
{
	relic_pred_t *pred;

	pred = pred_entry(ctx, id);
	if (pred == NULL)
		return;

	pred->head_named = true;
	pred->kind = RELIC_PRED_REWRITTEN;
	pred->stands_for = stands_for;
}

/*
 * Record that this predicate is the group predicate of an aggregate in a
 * rule deriving `holder`, or in a query when `holder` is RELIC_ID_NONE: the
 * group rule's head names it, and a diagnostic names it as that aggregate.
 */
void
relic_pred_name_group(relic_t *ctx, relic_pred_id_t id,
    relic_pred_id_t holder)
{
	relic_pred_t *pred;

	pred = pred_entry(ctx, id);
	if (pred == NULL)
		return;

	pred->head_named = true;
	pred->kind = RELIC_PRED_GROUP;
	pred->stands_for = holder;
}

/*
 * Record that a rule with this predicate as its head has been appended to a
 * rule table.  Only relic_ruletab_place() calls this, and a query's
 * compilation when it publishes the group rules the query holds.
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
 * Count one more of the program's rules deriving this predicate, which a
 * program statement's compilation calls for each rule it appends.
 */
void
relic_pred_count_program_rule(relic_t *ctx, relic_pred_id_t id)
{
	relic_pred_t *pred;

	pred = pred_entry(ctx, id);
	if (pred != NULL)
		pred->program_rules++;
}

/*
 * Record that an atom at `where` reads this predicate.  The first such atom
 * is the one a warning points at.  Returns 0, or -1 having reported that
 * memory ran out.
 *
 * This is the one place a location crosses from a statement into the
 * context, and the statement may be a query's, whose source name lives in
 * the query's arena and dies with it.  So the source name is copied into the
 * name store, once per distinct name, before the reference is recorded: a
 * copy refused for memory fails the statement as any resource does and
 * records nothing, so that the statement compiled again records the
 * reference whole.
 */
int
relic_pred_reference(relic_t *ctx, relic_pred_id_t id,
    const relic_location_t *where)
{
	relic_pred_t *pred;
	const char *file;

	pred = pred_entry(ctx, id);
	if (pred == NULL || pred->referenced)
		return (0);

	file = NULL;
	if (where->file != NULL) {
		file = keep_source_name(ctx, where->file);
		if (file == NULL) {
			relic_diag_no_memory(ctx);
			return (-1);
		}
	}

	pred->referenced = true;
	pred->referenced_at = *where;
	pred->referenced_at.file = file;

	return (0);
}

/*
 * The hash of a source name already copied, for its index to use when it
 * grows.  Source names are few and copied once each, so the hash is
 * computed again rather than kept.
 */
static uint32_t
source_rehash(uint32_t position, void *ctx)
{
	const relic_predtab_t *table;
	const char *name;

	table = ctx;
	name = table->sources[position - 1];

	return (relic_hash_bytes(table->hash_seed, name, strlen(name)));
}

/*
 * The copy of a source name in the name store, made the first time the
 * name is asked for and found afterwards, or NULL when memory ran out.
 */
static const char *
keep_source_name(relic_t *ctx, const char *name)
{
	relic_predtab_t *table;
	relic_idprobe_t probe;
	const char **sources;
	const char *kept;
	size_t length;
	uint32_t position;
	uint32_t hash;

	table = &ctx->program.predicates;
	length = strlen(name);
	hash = relic_hash_bytes(table->hash_seed, name, length);

	relic_idprobe_open(&probe, &table->source_index, hash);
	while (relic_idprobe_next(&probe, &position)) {
		kept = table->sources[position - 1];
		if (strlen(kept) == length && bcmp(kept, name, length) == 0)
			return (kept);
	}

	if (table->source_count >= UINT32_MAX - 1u)
		return (NULL);
	if (relic_idtab_reserve(&table->source_index, 1, source_rehash,
	    table) != 0)
		return (NULL);
	sources = relic_grow_array(table->sources, &table->source_capacity,
	    table->source_count + 1, sizeof(*table->sources));
	if (sources == NULL)
		return (NULL);
	table->sources = sources;

	kept = keep_name(ctx, name, length);
	if (kept == NULL)
		return (NULL);

	table->sources[table->source_count] = kept;
	table->source_count++;
	relic_idtab_place(&table->source_index, hash,
	    (uint32_t)table->source_count);

	return (kept);
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
 *
 * A predicate is described in the terms of what the reader of the message
 * wrote and can change.  One the Magic rewrite generated is described as
 * the program's predicate it is part of, or as the query, and the group
 * predicate of an aggregate as that aggregate: "an aggregate of p/1", or
 * "an aggregate of the query".  The predicate a rewritten one is part of
 * is one the rewrite did not generate, and is described as any other is,
 * so that the rewrite adorning a group predicate would leave its adorned
 * form described as the aggregate.  The explanation of a plan names
 * generated predicates by their own names, through relic_relation_name().
 */
const char *
relic_pred_describe(const relic_t *ctx, relic_pred_id_t id, char *buffer,
    size_t size)
{
	const relic_pred_t *pred;

	pred = relic_pred_of(ctx, id);
	if (pred != NULL && pred->kind == RELIC_PRED_REWRITTEN) {
		if (pred->stands_for == RELIC_ID_NONE) {
			strlcpy(buffer, RELIC_QUERY_DESCRIPTION, size);
			return (buffer);
		}
		id = pred->stands_for;
		pred = relic_pred_of(ctx, id);
	}

	if (pred != NULL && pred->kind == RELIC_PRED_GROUP)
		return (describe_aggregate_group(ctx, pred, buffer, size));

	return (relic_pred_identify(ctx, id, buffer, size));
}

/*
 * Describe the group predicate of an aggregate as that aggregate, within
 * the RELIC_PRED_TEXT_MAX bytes any description takes.  The name of the
 * rule's predicate is given the room the prefix and the widest arity leave
 * it, and is shortened to it, so that the arity is never what is cut.
 */
#define	AGGREGATE_PREFIX	"an aggregate of "
#define	AGGREGATE_NAME_MAX						\
	(RELIC_PRED_TEXT_MAX - (sizeof(AGGREGATE_PREFIX) - 1) -		\
	(sizeof("/65535") - 1))

static const char *
describe_aggregate_group(const relic_t *ctx, const relic_pred_t *group,
    char *buffer, size_t size)
{
	char holder[RELIC_PRED_TEXT_MAX];

	if (group->stands_for == RELIC_ID_NONE) {
		strlcpy(buffer, AGGREGATE_PREFIX RELIC_QUERY_DESCRIPTION, size);
		return (buffer);
	}

	snprintf(buffer, size, AGGREGATE_PREFIX "%s", write_pred(ctx,
	    group->stands_for, AGGREGATE_NAME_MAX, holder, sizeof(holder)));

	return (buffer);
}

/*
 * Write "name/arity" of the predicate itself into `buffer` and return it, a
 * generated predicate by its own name, for an internal error, where what a
 * maintainer needs is the predicate the code reached.
 */
const char *
relic_pred_identify(const relic_t *ctx, relic_pred_id_t id, char *buffer,
    size_t size)
{
	return (write_pred(ctx, id, RELIC_NAME_TEXT_MAX, buffer, size));
}

/*
 * Write "name/arity" into `buffer` and return it, the name shortened to fit
 * `name_max` bytes, its terminator included.
 */
static const char *
write_pred(const relic_t *ctx, relic_pred_id_t id, size_t name_max,
    char *buffer, size_t size)
{
	const relic_pred_t *pred;
	char shortened[RELIC_NAME_TEXT_MAX];

	assert(name_max <= sizeof(shortened));

	pred = relic_pred_of(ctx, id);
	if (pred == NULL) {
		snprintf(buffer, size, "?");
		return (buffer);
	}

	snprintf(buffer, size, "%s/%u",
	    relic_diag_name(pred->name.bytes, shortened, name_max),
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

		relic_diag_warning(ctx, &pred->referenced_at,
		    "predicate %s is referenced but never defined",
		    relic_pred_describe(ctx,
		    (relic_pred_id_t)(position + 1), described,
		    sizeof(described)));
	}
}
