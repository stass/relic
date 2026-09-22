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
 * Hash functions with seeds fixed at compile time.  No seed is randomized,
 * derived from an address, read from the clock or taken from the
 * environment: identical input must produce an identical probe order on
 * every run and on every platform, because the golden test suite compares
 * output byte for byte.
 */
uint32_t relic_hash_bytes(const void *bytes, size_t length);
uint32_t relic_hash_int64(int64_t value);
uint32_t relic_hash_word(uint32_t word);

typedef struct relic_idprobe relic_idprobe_t;
typedef struct relic_idtab relic_idtab_t;

/*
 * Recover the hash of an identifier already in the table.  Only the owner of
 * the keys can do this, so it supplies the function; the table calls it once
 * per entry when it doubles and never on a lookup.
 */
typedef uint32_t (*relic_rehash_fn)(uint32_t id, void *ctx);

/*
 * An open-addressing table from a hash to the identifiers that share it.
 *
 * A slot holds an identifier and nothing else: no key, no key pointer, no
 * cached hash.  That is the whole reason for a purpose-built table rather
 * than a generic container.  The key stays wherever its owner already keeps
 * it, so an entry costs four bytes and the owner stores nothing twice.
 *
 * The table never removes an entry and therefore has no tombstones, which is
 * what keeps linear probing simple.  Nothing in relic un-interns a symbol or
 * a value.  A structure that must delete should be a separate type rather
 * than a flag on this one.
 */
struct relic_idtab {
	uint32_t	*slots;		/* RELIC_ID_NONE marks an empty slot */
	size_t		 capacity;	/* a power of two, zero if empty */
	size_t		 count;
};

/*
 * A walk over the slots a hash may have landed in.  The probe sequence lives
 * here and nowhere else; the comparison that decides whether a candidate
 * identifier is the one being looked for stays at the call site, where a
 * reader can see what makes two keys equal.
 */
struct relic_idprobe {
	const relic_idtab_t	*table;
	size_t			 mask;
	size_t			 slot;
};

void	 relic_idtab_init(relic_idtab_t *table);
void	 relic_idtab_reset(relic_idtab_t *table);

/*
 * Installing an identifier is two operations rather than one, and they are
 * separate so that the fallible half can be done before anything is at
 * stake.
 *
 * relic_idtab_reserve() makes room for one more entry, growing the table if
 * it would otherwise pass three quarters full.  It is the only part that can
 * fail, returning -1 when the table cannot grow.
 *
 * relic_idtab_place() then stores the identifier, and cannot fail.
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
int	 relic_idtab_reserve(relic_idtab_t *table, relic_rehash_fn rehash,
	    void *ctx);
void	 relic_idtab_place(relic_idtab_t *table, uint32_t hash, uint32_t id);

/*
 * Begin walking the candidates for hash.
 */
void	 relic_idprobe_open(relic_idprobe_t *probe, const relic_idtab_t *table,
	    uint32_t hash);

/*
 * Store the next candidate identifier in *idp and return true, or return
 * false once the walk reaches an empty slot, which proves no further slot can
 * hold the key.  The walk always terminates: the table keeps an empty slot by
 * growing before it fills.
 */
bool	 relic_idprobe_next(relic_idprobe_t *probe, uint32_t *idp);

#endif /* RELIC_UTIL_H */
