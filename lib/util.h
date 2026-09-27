/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_UTIL_H
#define RELIC_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Storage primitives shared by the symbol table, the value table and the
 * relation store with its indexes.  Nothing here knows what a value or a
 * tuple is: these are the growable array and the identifier table, and the
 * subsystems above supply the meaning.
 */

/*
 * Identifier zero is never assigned to anything.  Symbol identifiers and
 * domain identifiers both begin at one, which is what lets a zeroed slot in
 * the table below mean "empty" with no occupancy bitmap beside it and no
 * sentinel a caller could collide with.
 */
#define	RELIC_ID_NONE	0u

/*
 * A value, interned.  Every value relic handles -- integers included -- is
 * reduced to one of these, so a tuple column is four bytes whatever it holds
 * and a join compares plain integers without inspecting a payload.
 *
 * Identifiers are assigned in order of first appearance, so they are
 * reproducible across runs, but their numeric order records only the order
 * the input mentioned things in.  Nothing may sort by a domain identifier;
 * ordering goes through relic_value_order() or relic_value_total_order().
 *
 * The typedef lives here, beside the identifier conventions it obeys, so
 * that a header describing stored values can use it without reaching for
 * every other internal type.
 */
typedef uint32_t relic_dom_t;

/*
 * Grow an array to hold at least count items of item_size bytes each.
 *
 * Returns items unchanged when *capacity already suffices, and otherwise a
 * reallocated array with *capacity updated: doubled, starting at eight items,
 * and never below count.  Returns NULL when the array cannot be grown,
 * leaving both the caller's array and *capacity untouched, so a failed growth
 * loses nothing that was already stored.
 *
 * A count of zero is treated as one.  Every caller asks for room for
 * `count + 1` items and so cannot produce a zero, and rounding it up rather
 * than returning the array unchanged keeps a null return meaning failure and
 * nothing else.
 *
 *	entries = relic_grow_array(pool->entries, &pool->capacity,
 *	    pool->count + 1, sizeof(*pool->entries));
 *	if (entries == NULL)
 *		return (-1);
 *	pool->entries = entries;
 */
void	*relic_grow_array(void *items, size_t *capacity, size_t count,
	    size_t item_size);

/*
 * The capacity relic_grow_array() would reach for count items, or zero when
 * it would refuse to grow at all.
 *
 * A caller that must know the size of an array before it asks for it -- one
 * accounting the allocation against a limit, say -- uses this rather than
 * measuring afterwards.  The growth itself is stated in terms of this
 * function, so a prediction and the policy it predicts cannot drift apart.
 *
 * Zero is unambiguous: an array that needs no growth returns the capacity it
 * already has, which is never zero, because a request for no items is
 * rounded up to one exactly as it is in relic_grow_array().
 */
size_t	 relic_grow_array_capacity(size_t capacity, size_t count);

/*
 * Hash functions keyed by a 64-bit seed.
 *
 * Each context draws its seed at random when it is created, and every table
 * owner copies it and passes it to every hash it computes.  The author of a
 * program cannot learn the seed, so cannot compute where a key will land and
 * cannot choose keys that pile into one probe cluster and make every lookup
 * linear.  Nothing a run reports depends on the seed: no table is walked in
 * slot order, so the seed decides where a key sits and how many probes find
 * it, and nothing else.
 *
 * A seed is never zero, which the hashes assert: a zero is an owner that was
 * never given its context's seed.
 *
 * For one seed the hashes are the same on every platform.  Words and integers
 * are folded as values rather than as the bytes they occupy, so a table
 * probes in the same order on a big-endian machine as on a little-endian
 * one.
 */
uint32_t relic_hash_bytes(uint64_t seed, const void *bytes, size_t length);
uint32_t relic_hash_int64(uint64_t seed, int64_t value);
uint32_t relic_hash_word(uint64_t seed, uint32_t word);

/*
 * Hash a sequence of 32-bit words, which is what a tuple is.  A count of zero
 * is a defined key rather than a special case, which is what lets a relation
 * of arity zero use the ordinary code path.
 */
uint32_t relic_hash_words(uint64_t seed, const uint32_t *words, size_t count);

typedef struct relic_idprobe relic_idprobe_t;
typedef struct relic_idtab relic_idtab_t;

/*
 * Recover the hash of an identifier already in the table.  Only the owner of
 * the keys can do this, so it supplies the function; the table calls it once
 * per entry when it doubles and never on a lookup.  It must return exactly
 * the hash the identifier was placed under, all 32 bits of it: the low bits
 * choose the slot and a tagged table keeps the top eight.
 */
typedef uint32_t (*relic_rehash_fn)(uint32_t id, void *ctx);

/*
 * An open-addressing table from a hash to the identifiers that share it.
 *
 * A slot holds an identifier and nothing else: no key, no key pointer, no
 * cached hash.  That is the whole reason for a purpose-built table rather
 * than a generic container.  The key stays wherever its owner already keeps
 * it, so an entry costs four bytes, and five in a table created tagged.
 *
 * A table created tagged keeps one byte more per slot, in `tags` beside the
 * slot array: the top eight bits of the hash the slot's identifier was placed
 * under.  The bottom bits of a hash choose its first slot, so a tag tells
 * apart keys that the slot does not, and a probe passes an occupied slot
 * whose tag differs from its own hash's without handing the identifier to
 * the owner.  A search for an absent key then asks the owner about one
 * mismatching slot in 256 rather than each one it visits, which pays where
 * comparing a candidate reads memory far from the table, such as a symbol's
 * bytes.  A table whose searches mostly succeed reads the matching key
 * either way, and is created untagged, with `tags` NULL.
 *
 * The table never removes an entry and therefore has no tombstones, which is
 * what keeps linear probing simple.  Nothing in relic un-interns a symbol or
 * a value.  A structure that must delete should be a separate type rather
 * than a flag on this one.
 */
struct relic_idtab {
	uint32_t	*slots;		/* RELIC_ID_NONE marks an empty slot */
	uint8_t		*tags;		/* per slot when tagged, else NULL */
	size_t		 capacity;	/* a power of two, zero if empty */
	size_t		 count;
	bool		 tagged;
};

/*
 * A walk over the slots a hash may have landed in.  The probe sequence lives
 * here and nowhere else; the comparison that decides whether a candidate
 * identifier is the one being looked for stays at the call site, where a
 * reader can see what makes two keys equal.  `tag` is the hash's tag, which
 * a tagged table's candidates must carry.
 */
struct relic_idprobe {
	const relic_idtab_t	*table;
	size_t			 mask;
	size_t			 slot;
	uint8_t			 tag;
};

/*
 * relic_idtab_init() makes an empty table, tagged or not for good: the
 * choice survives relic_idtab_reset(), which releases the slots and leaves
 * the empty table of the same kind.
 */
void	 relic_idtab_init(relic_idtab_t *table, bool tagged);
void	 relic_idtab_reset(relic_idtab_t *table);

/*
 * Installing an identifier is two operations rather than one, and they are
 * separate so that the fallible half can be done before anything is at
 * stake.
 *
 * relic_idtab_reserve() makes room for `added` more entries, growing the
 * table once, if they would otherwise take it past three quarters full, to
 * the smallest power of two at least twice its size that holds them.  It is
 * the only part that can fail, returning -1 when the table cannot grow.
 *
 * relic_idtab_place() then stores an identifier, and cannot fail; at most
 * `added` places may follow a reservation for `added` entries.
 *
 * An owner reserves before it creates the thing being indexed, so that a
 * failure happens while nothing has been created yet.  Were the order the
 * other way round, a failed insertion would leave an entry in the owner's
 * array that no lookup could reach, and the next lookup of the same key
 * would create a second one.  For an interning table that is not a lost
 * entry but a broken invariant: two identical keys with two identifiers,
 * which compare unequal ever afterwards.
 *
 * Between a reserve and its place the table must not be touched, which holds
 * because a context is used by one thread and the owner does both in one
 * operation.
 *
 * id must not be RELIC_ID_NONE, which marks an empty slot.  Both tables
 * assign an identifier from the length of the array it indexes, so every
 * identifier is at least one by construction and no caller can produce the
 * reserved value.
 */
int	 relic_idtab_reserve(relic_idtab_t *table, size_t added,
	    relic_rehash_fn rehash, void *ctx);
void	 relic_idtab_place(relic_idtab_t *table, uint32_t hash, uint32_t id);

/*
 * The bytes a table's slots and tags occupy now, and the bytes they would
 * occupy once relic_idtab_reserve() had made room for `added` more entries.
 *
 * These exist for the relation store, whose indexes are accounted against the
 * memory limit and must therefore be measured before they are allocated
 * rather than after.  A reservation that could not grow the table reports the
 * size it already has: the growth will fail as an ordinary allocation
 * failure, which is a different answer from exceeding a limit.
 */
size_t	 relic_idtab_bytes(const relic_idtab_t *table);
size_t	 relic_idtab_bytes_after_reserve(const relic_idtab_t *table,
	    size_t added);

/*
 * A prefetch of the line holding `address`: the compiler's builtin where
 * configure found one, and nothing elsewhere.  A prefetch is a hint that
 * reads nothing a program depends on and cannot fault, so without one the
 * same work is done and only the overlap is lost.
 */
#ifdef HAVE_BUILTIN_PREFETCH
#define	RELIC_PREFETCH(address)	__builtin_prefetch(address)
#else
#define	RELIC_PREFETCH(address)	((void)(address))
#endif

/*
 * Hints for a caller about to probe many hashes, which lets their cache
 * misses overlap.  relic_idtab_prefetch() asks for the slot `hash` selects,
 * and its tag in a tagged table; relic_idtab_peek() gives the identifier in
 * that slot, RELIC_ID_NONE when it is empty, whatever hash it was placed
 * under, so that the caller can ask for the key it names too.  Neither is a
 * lookup, and neither changes anything.  A table with no capacity has no
 * slots, and neither touches one.  Both are inline because a batch calls
 * them once per tuple.
 */
static inline void
relic_idtab_prefetch(const relic_idtab_t *table, uint32_t hash)
{
	size_t slot;

	if (table->capacity == 0)
		return;

	slot = hash & (table->capacity - 1);
	RELIC_PREFETCH(&table->slots[slot]);
	if (table->tags != NULL)
		RELIC_PREFETCH(&table->tags[slot]);
}

static inline uint32_t
relic_idtab_peek(const relic_idtab_t *table, uint32_t hash)
{
	if (table->capacity == 0)
		return (RELIC_ID_NONE);

	return (table->slots[hash & (table->capacity - 1)]);
}

/*
 * Begin walking the candidates for hash.
 */
void	 relic_idprobe_open(relic_idprobe_t *probe, const relic_idtab_t *table,
	    uint32_t hash);

/*
 * Store the next candidate identifier in *idp and return true, or return
 * false once the walk reaches an empty slot, which proves no further slot can
 * hold the key.  In a tagged table a candidate is an identifier whose tag is
 * the probe's; the others are passed over.  The walk always terminates: the
 * table keeps an empty slot by growing before it fills.
 */
bool	 relic_idprobe_next(relic_idprobe_t *probe, uint32_t *idp);

#endif /* RELIC_UTIL_H */
