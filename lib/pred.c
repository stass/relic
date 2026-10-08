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
 * A predicate is identified by (name, arity), so foo/1 and foo/2 are
 * unrelated.  The table is the same open-addressing identifier table the
 * symbol pool and the value table use, with the pair as its key:
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

/*
 * The bytes the table occupies, part by part: the entries and the source
 * names' array at their capacity, the index of each, and the name store.
 * A growth is projected by replacing the parts it grows, so the measure and
 * every projection name the parts in one place.
 */
typedef struct predtab_size predtab_size_t;

struct predtab_size {
	size_t	entries;
	size_t	index;
	size_t	sources;
	size_t	source_index;
	size_t	names;
};

static relic_pred_id_t pred_find(const relic_predtab_t *table,
		    const char *name, size_t length, uint16_t arity,
		    uint32_t hash);
static uint64_t	 written_arities(const relic_t *ctx, const char *name);
static relic_pred_t *pred_at(const relic_predtab_t *table,
		    relic_pred_id_t id);
static uint32_t	 pred_hash(uint64_t seed, const char *name, size_t length,
		    uint16_t arity);
static uint32_t	 pred_rehash(uint32_t id, void *ctx);
static void	 measure_table(const relic_predtab_t *table,
		    predtab_size_t *size);
static size_t	 size_total(const predtab_size_t *size);
static size_t	 names_after_copy(const relic_arena_t *names, size_t length);
static size_t	 bytes_with_predicate(const relic_predtab_t *table,
		    size_t length);
static size_t	 bytes_with_source(const relic_predtab_t *table,
		    size_t length);
static bool	 admit_growth(relic_t *ctx, size_t projected_bytes);
static void	 account_table(relic_t *ctx);
static const char *copy_name(relic_arena_t *names, const char *bytes,
		    size_t length);
static const char *grow_for_predicate(relic_predtab_t *table,
		    const char *name, size_t length);
static const char *grow_for_source(relic_predtab_t *table, const char *name,
		    size_t length);
static const char *room_for_predicate(relic_t *ctx, const char *name,
		    size_t length);
static const char *room_for_source(relic_t *ctx, const char *name,
		    size_t length);
static int	 pred_store(relic_t *ctx, const char *name, size_t length,
		    uint16_t arity, uint32_t hash,
		    const relic_location_t *where, relic_pred_id_t *idp);
static relic_pred_t *pred_entry(relic_t *ctx, relic_pred_id_t id);
static const char *describe_aggregate_group(const relic_t *ctx,
		    const relic_pred_t *group, char *buffer, size_t size);
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

static void
measure_table(const relic_predtab_t *table, predtab_size_t *size)
{
	size->entries = relic_saturating_product(table->capacity,
	    sizeof(*table->entries));
	size->index = relic_idtab_bytes(&table->index);
	size->sources = relic_saturating_product(table->source_capacity,
	    sizeof(*table->sources));
	size->source_index = relic_idtab_bytes(&table->source_index);
	size->names = relic_arena_total_bytes(&table->names);
}

static size_t
size_total(const predtab_size_t *size)
{
	size_t total;

	total = relic_saturating_sum(size->entries, size->index);
	total = relic_saturating_sum(total, size->sources);
	total = relic_saturating_sum(total, size->source_index);

	return (relic_saturating_sum(total, size->names));
}

/*
 * What the predicate table occupies, which the memory account counts as
 * `predtab_bytes`.
 */
size_t
relic_pred_table_bytes(const relic_t *ctx)
{
	predtab_size_t size;

	measure_table(&ctx->program.predicates, &size);

	return (size_total(&size));
}

/*
 * What the name store would occupy once it held a copy of `length` bytes
 * with its terminator, as copy_name() makes it.
 */
static size_t
names_after_copy(const relic_arena_t *names, size_t length)
{
	relic_arena_projection_t projected;

	relic_arena_project(names, &projected);
	relic_arena_project_alloc(&projected, length + 1);

	return (projected.total_bytes);
}

/*
 * What the table would occupy once grow_for_predicate() had made room for a
 * predicate named by `length` bytes: an entry, a slot in the index and the
 * name's copy.  An array that could not grow is projected as more than
 * anything could hold.
 */
static size_t
bytes_with_predicate(const relic_predtab_t *table, size_t length)
{
	predtab_size_t size;
	size_t capacity;

	capacity = relic_grow_array_capacity(table->capacity,
	    table->count + 1);
	if (capacity == 0)
		return (SIZE_MAX);

	measure_table(table, &size);
	size.entries = relic_saturating_product(capacity,
	    sizeof(*table->entries));
	size.index = relic_idtab_bytes_after_reserve(&table->index, 1);
	size.names = names_after_copy(&table->names, length);

	return (size_total(&size));
}

/*
 * What the table would occupy once grow_for_source() had made room for a
 * source name of `length` bytes: a place in the array of source names, a
 * slot in their index and the name's copy.
 */
static size_t
bytes_with_source(const relic_predtab_t *table, size_t length)
{
	predtab_size_t size;
	size_t capacity;

	capacity = relic_grow_array_capacity(table->source_capacity,
	    table->source_count + 1);
	if (capacity == 0)
		return (SIZE_MAX);

	measure_table(table, &size);
	size.sources = relic_saturating_product(capacity,
	    sizeof(*table->sources));
	size.source_index = relic_idtab_bytes_after_reserve(
	    &table->source_index, 1);
	size.names = names_after_copy(&table->names, length);

	return (size_total(&size));
}

/*
 * Whether the memory limit admits the table growing to `projected_bytes`,
 * asked against the whole total.  A growth the table has room for adds
 * nothing and is not asked.  A refusal is recorded on the account, so that
 * the caller's relic_diag_no_memory() reports the limit.
 */
static bool
admit_growth(relic_t *ctx, size_t projected_bytes)
{
	size_t held_bytes;

	held_bytes = relic_pred_table_bytes(ctx);
	if (projected_bytes == held_bytes)
		return (true);

	return (relic_account_admit(&ctx->account, held_bytes,
	    projected_bytes));
}

/*
 * Set the account's count of the table to what the table occupies, after
 * anything that may have grown it.  The count is reconciled from the table
 * rather than adjusted by each growth, so it cannot drift from it.
 */
static void
account_table(relic_t *ctx)
{
	ctx->account.predtab_bytes = relic_pred_table_bytes(ctx);
}

/*
 * Copy `length` bytes into the name store, terminated, and return the copy,
 * or NULL when the allocator refused it.  The store lives as long as the
 * context, so the arena's failure is cleared: left standing, it would
 * refuse every name after this one.
 */
static const char *
copy_name(relic_arena_t *names, const char *bytes, size_t length)
{
	char *copy;

	copy = relic_arena_alloc(names, length + 1);
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
 * Grow the table for one more predicate -- a slot in the index and an entry
 * -- and copy its name into the store, returning the copy, or NULL when the
 * allocator refused a growth.
 *
 * Every growth happens before the predicate is stored, so that a failure
 * leaves room behind and nothing else.  Storing first and indexing second
 * would, on a failed index growth, leave a predicate no lookup could reach,
 * and the next mention of the same pair would assign it a second identifier
 * -- which would make one predicate into two relations holding half the
 * answer each.
 */
static const char *
grow_for_predicate(relic_predtab_t *table, const char *name, size_t length)
{
	relic_pred_t *entries;

	if (relic_idtab_reserve(&table->index, 1, pred_rehash, table) != 0)
		return (NULL);

	entries = relic_grow_array(table->entries, &table->capacity,
	    table->count + 1, sizeof(*table->entries));
	if (entries == NULL)
		return (NULL);
	table->entries = entries;

	return (copy_name(&table->names, name, length));
}

/*
 * Grow the table for one more source name -- a slot in their index and a
 * place in their array -- and copy it into the store, as
 * grow_for_predicate() does for a predicate.
 */
static const char *
grow_for_source(relic_predtab_t *table, const char *name, size_t length)
{
	const char **sources;

	if (relic_idtab_reserve(&table->source_index, 1, source_rehash,
	    table) != 0)
		return (NULL);

	sources = relic_grow_array(table->sources, &table->source_capacity,
	    table->source_count + 1, sizeof(*table->sources));
	if (sources == NULL)
		return (NULL);
	table->sources = sources;

	return (copy_name(&table->names, name, length));
}

/*
 * The two ways the table grows, for a predicate and for a source name, each
 * returning the name's copy, or NULL when the memory limit or the allocator
 * refused, for the caller to report through relic_diag_no_memory().
 *
 * The limit is asked first, for the whole table as the growth would leave
 * it, and the account is set to what the table holds after whatever grew,
 * succeeding or not, so that every entry and index slot is charged as well
 * as every name.
 */
static const char *
room_for_predicate(relic_t *ctx, const char *name, size_t length)
{
	relic_predtab_t *table;
	const char *kept;

	table = &ctx->program.predicates;
	if (!admit_growth(ctx, bytes_with_predicate(table, length)))
		return (NULL);

	kept = grow_for_predicate(table, name, length);
	account_table(ctx);

	return (kept);
}

static const char *
room_for_source(relic_t *ctx, const char *name, size_t length)
{
	relic_predtab_t *table;
	const char *kept;

	table = &ctx->program.predicates;
	if (!admit_growth(ctx, bytes_with_source(table, length)))
		return (NULL);

	kept = grow_for_source(table, name, length);
	account_table(ctx);

	return (kept);
}

/*
 * Append a predicate to the table and return the identifier assigned to it.
 *
 * A failure leaves the table with room it did not use but entirely
 * consistent: the entry a caller could reach is one that was fully written
 * and indexed.
 */
static int
pred_store(relic_t *ctx, const char *name, size_t length, uint16_t arity,
    uint32_t hash, const relic_location_t *where, relic_pred_id_t *idp)
{
	relic_predtab_t *table;
	relic_pred_t *stored;
	const char *kept;

	table = &ctx->program.predicates;

	if (table->count >= RELIC_PRED_MAX) {
		relic_diag_error(ctx, RELIC_ERROR_LIMIT, where,
		    "too many distinct predicates");
		return (-1);
	}

	kept = room_for_predicate(ctx, name, length);
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
	relic_idtab_place(&table->index, hash, (uint32_t)table->count);
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
	relic_pred_id_t found;
	uint32_t hash;

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
	found = pred_find(table, name, length, arity, hash);
	if (found != RELIC_ID_NONE) {
		*idp = found;
		return (0);
	}

	return (pred_store(ctx, name, length, arity, hash, where, idp));
}

/*
 * The identifier of the predicate `length` bytes of `name` at `arity`, whose
 * hash is `hash`, or RELIC_ID_NONE when the table holds none.  Interns
 * nothing.  `length` is at most RELIC_STRING_LENGTH_MAX, as every name the
 * table holds is.
 */
static relic_pred_id_t
pred_find(const relic_predtab_t *table, const char *name, size_t length,
    uint16_t arity, uint32_t hash)
{
	relic_idprobe_t probe;
	const relic_pred_t *candidate;
	relic_spelling_t asked;
	uint32_t id;

	asked.bytes = name;
	asked.length = (uint32_t)length;

	relic_idprobe_open(&probe, &table->index, hash);
	while (relic_idprobe_next(&probe, &id)) {
		candidate = pred_at(table, id);
		if (candidate->arity == arity &&
		    relic_spelling_equal(&candidate->name, &asked))
			return (id);
	}

	return (RELIC_ID_NONE);
}

/*
 * The arities at which the program knows `name`: one probe of the table per
 * arity a predicate may have.  Only a predicate the program wrote counts; a
 * base view, a rewritten predicate and an aggregate's group predicate are
 * the library's own, whatever their names.  A name longer than any the
 * table can hold has no arity.  A predicate a failed load or import interned
 * stays in the table, as every interned predicate does, so its arity counts.
 *
 * The set is a 64-bit word, bit n for arity n, so the arity limit must stay
 * below 64 for every arity to have a bit.
 */
_Static_assert(RELIC_MAX_ARITY < 64,
    "a set of arities is a uint64_t, one bit per arity");

int
relic_predicate_arities(relic_t *ctx, const char *name, uint64_t *arities)
{
	int status;

	if (relic_enter(ctx, NULL, NULL) != 0)
		return (-1);

	if (name == NULL || arities == NULL) {
		relic_diag_error(ctx, RELIC_ERROR_ARGUMENT, NULL,
		    "the arities of a predicate need its name and somewhere "
		    "to put them");
		relic_leave(ctx);
		return (-1);
	}

	status = relic_program_ready(ctx);
	if (status == 0)
		*arities = written_arities(ctx, name);

	relic_leave(ctx);

	return (status);
}

/*
 * The set of arities at which the program wrote a predicate named `name`.
 */
static uint64_t
written_arities(const relic_t *ctx, const char *name)
{
	const relic_predtab_t *table;
	const relic_pred_t *pred;
	uint64_t found;
	size_t length;
	uint16_t arity;

	length = strlen(name);
	if (length > RELIC_STRING_LENGTH_MAX)
		return (0);

	table = &ctx->program.predicates;
	found = 0;
	for (arity = 0; arity <= RELIC_MAX_ARITY; arity++) {
		pred = relic_pred_of(ctx, pred_find(table, name, length, arity,
		    pred_hash(table->hash_seed, name, length, arity)));
		if (pred != NULL && pred->kind == RELIC_PRED_WRITTEN &&
		    pred->views == RELIC_ID_NONE)
			found |= UINT64_C(1) << arity;
	}

	return (found);
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

	kept = room_for_source(ctx, name, length);
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
