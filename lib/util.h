/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
 * The top two bits of an identifier say the value's type and the bottom
 * thirty index the table of that type: a symbol or a string is its symbol
 * identifier under RELIC_DOM_SYMBOL or RELIC_DOM_STRING, and an integer its
 * position in the integer table under RELIC_DOM_INTEGER.  Type bits of zero
 * are never assigned, so no value's identifier is RELIC_ID_NONE and a zeroed
 * slot or column holds no value.
 *
 * Within a type identifiers are assigned in order of first appearance, so
 * they are reproducible across runs, but their numeric order records only
 * the order the input mentioned things in.  Nothing may sort by a domain
 * identifier; ordering goes through relic_value_order() or
 * relic_value_total_order().
 *
 * The typedef lives here, beside the identifier conventions it obeys, so
 * that a header describing stored values can use it without reaching for
 * every other internal type.
 */
typedef uint32_t relic_dom_t;

#define	RELIC_DOM_TYPE_SHIFT	30
#define	RELIC_DOM_SYMBOL	(UINT32_C(1) << RELIC_DOM_TYPE_SHIFT)
#define	RELIC_DOM_STRING	(UINT32_C(2) << RELIC_DOM_TYPE_SHIFT)
#define	RELIC_DOM_INTEGER	(UINT32_C(3) << RELIC_DOM_TYPE_SHIFT)
#define	RELIC_DOM_INDEX_MASK	((UINT32_C(1) << RELIC_DOM_TYPE_SHIFT) - 1)
#define	RELIC_DOM_TYPE_MASK	(~RELIC_DOM_INDEX_MASK)

/*
 * Grow an array to hold at least count items of item_size bytes each.
 *
 * Returns items unchanged when *capacity already suffices, and otherwise a
 * reallocated array with *capacity updated: doubled, starting at eight items,
 * and never below count.  Returns NULL when the array cannot be grown,
 * leaving both the caller's array and *capacity untouched, so a failed growth
 * loses nothing that was already stored.
 *
 * items is NULL or an array this function returned, and the array is
 * released by relic_free_array() and by nothing else.  Its storage is not a
 * heap block the caller may pass to free(3) or realloc(3): a header in front
 * of the first item records how the storage was obtained.  An array whose
 * bytes reach RELIC_ARRAY_MAP_BYTES is given a mapping of its own, whose pages
 * go back to the system the moment it is released or outgrown, where a freed
 * heap block of that size may stay resident for the rest of the run.
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
 * The capacity an array holding `held` items with room for `capacity`
 * reaches when relic_grow_array() is asked, `requests` times over, for room
 * for `per_request` more items than the last request -- as one insertion
 * after another grows it -- or zero when a growth on the way would be
 * refused.  `held` must not exceed `capacity`.
 *
 * Growing a step at a time can reach more than growing once for the whole:
 * room for 100 more items from room for 8 is 100 in one growth, and 128 in
 * ninety-two growths of one, which double 8 four times.  A projection of
 * what a sequence of insertions will hold follows the steps.  Only a step
 * that passes the capacity grows it, and each growth at least doubles it,
 * so the answer costs as many iterations as the capacity doubles.
 */
size_t	 relic_grow_array_capacity_stepwise(size_t capacity, size_t held,
	    size_t requests, size_t per_request);

/*
 * The capacity a buffer with room for `capacity` items reaches when it
 * doubles -- from the smallest array worth having, when it has less --
 * until it holds `count`, or zero when a doubling on the way would not be
 * representable.  Unlike relic_grow_array_capacity(), which raises a growth
 * to the request, this depends only on the largest request a buffer is
 * ever asked for: growing for one request and then another reaches what
 * growing once for the larger would.  A buffer sized by the data a plan
 * meets grows by it, so that its capacity after a run is the same however
 * the run's requests were divided among the scratches that made them.
 */
size_t	 relic_grow_capacity_doubling(size_t capacity, size_t count);

/*
 * Grow an array, in one allocation, to the capacity
 * relic_grow_array_capacity_stepwise() gives: the room `requests` growths
 * of `per_request` items after the `held` would reach one after another.
 * An owner that makes many insertions' room at once uses this so that it
 * holds exactly what the same insertions made one at a time would, byte
 * for byte.  `requests` must be at least one.  Returns the array, or NULL
 * when it could not grow, leaving the old array and *capacity as they were.
 */
void	*relic_grow_array_stepwise(void *items, size_t *capacity, size_t held,
	    size_t requests, size_t per_request, size_t item_size);

/*
 * The sum of two sizes, and `count` items of `size` bytes, or SIZE_MAX when
 * the result is more than a size_t holds.  A projection of storage built from
 * them says "more than anything could hold" rather than wrapping to a small
 * number a limit would admit.  Both are inline because every insertion
 * projects its relation's storage with them.
 */
static inline size_t
relic_saturating_sum(size_t a, size_t b)
{
	if (a > SIZE_MAX - b)
		return (SIZE_MAX);

	return (a + b);
}

static inline size_t
relic_saturating_product(size_t count, size_t size)
{
	if (size != 0 && count > SIZE_MAX / size)
		return (SIZE_MAX);

	return (count * size);
}

/*
 * The bytes from which an array is mapped rather than taken from the heap.
 * Below it a mapping would cost a system call and a page of rounding for an
 * array whose residue in the heap is small.  Above it the residue is not
 * small: a heap block outgrown by a doubling array is kept by the allocator
 * for reuse, and a table split into stripes that grow one after another
 * leaves such a block behind for every stripe.  At an eighth of a
 * mebibyte the stripes of a large relation's tables are mapped, so their
 * outgrown arrays are returned rather than kept.  The allocation-failure
 * suite sets it lower, so that the small programs it sweeps grow arrays
 * onto and between mappings too.
 */
#ifndef RELIC_ARRAY_MAP_BYTES
#define	RELIC_ARRAY_MAP_BYTES	((size_t)1 << 17)
#endif

/*
 * Defined when AddressSanitizer instruments the build, which GCC and Clang
 * announce differently.  The header in front of a growable array's first
 * item is then marked unaddressable, as are the bytes of a mapping past the
 * array's last item, so the sanitizer reports a write just outside the array
 * as it would for a plain heap block.
 */
#if defined(__SANITIZE_ADDRESS__)
#define	RELIC_ADDRESS_SANITIZER	1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define	RELIC_ADDRESS_SANITIZER	1
#endif
#endif

/*
 * Release a growable array: one relic_grow_array() returned, or the slots or
 * tags of an identifier table.  NULL is ignored, so an array that was never
 * grown needs no test.
 *
 * The header in front of the first item says how the storage was obtained,
 * and it is sealed against its own address.  A header that fails its seal --
 * overwritten by a stray write before the first item, say -- stops the
 * process with abort(3), since the length a damaged header claims could
 * release memory another array is using.
 */
void	 relic_free_array(void *items);

/*
 * Whether a growable array lives in a mapping of its own rather than in the
 * heap.  Nothing depends on the answer; it lets a test observe which storage
 * the growth chose.  The header is checked as relic_free_array() checks it.
 */
bool	 relic_array_mapped(const void *items);

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

/*
 * The hash of a sequence of words for a key kept in a striped table, 64 bits
 * wide.  Its low 32 bits, relic_hash_low(), are what relic_hash_words()
 * returns, and choose a slot and a tag in an identifier table.  Its high 32
 * bits are an unkeyed hash of the same words, whose top bits choose the
 * stripe, relic_hash_stripe().
 *
 * The stripe is unkeyed so that which stripe a key lives in is a function of
 * the key alone.  Each stripe grows as its own keys need, so what the
 * stripes hold and cost depends on how many keys each receives; chosen by
 * the seed, that would differ between contexts and runs, and so would the
 * storage a program reports and the insertion a memory limit refuses.  An
 * author who crowds keys into one stripe gains nothing a single table would
 * not give: within the stripe the slot and the tag are keyed as always, so
 * no probe cluster can be built, and the stripe is only larger.
 */
uint64_t relic_hash_words_wide(uint64_t seed, const uint32_t *words,
	    size_t count);

/*
 * Striped tables.  A table several threads fill at once without a lock --
 * a relation's canonical index, a secondary index's key table -- is
 * RELIC_STRIPES identifier tables, and a key lives in the one its wide
 * hash's top RELIC_STRIPE_BITS bits choose.  Stripes share nothing, so a
 * thread per stripe can grow and fill them at once.  The count is a
 * constant rather than the number of threads, so what a table holds and
 * what it costs are the same whatever the number of threads.  A test build
 * may set RELIC_STRIPE_BITS to zero, one stripe, to check that nothing
 * depends on there being several.
 */
#ifndef RELIC_STRIPE_BITS
#define	RELIC_STRIPE_BITS	4
#endif
#define	RELIC_STRIPES		((size_t)1 << RELIC_STRIPE_BITS)

/*
 * The stripe is taken from the unkeyed high half of a wide hash, so it may
 * use at most those 32 bits, and a round's merge keeps it in a byte per
 * tuple, so at most eight of them.
 */
_Static_assert(RELIC_STRIPE_BITS >= 0 && RELIC_STRIPE_BITS <= 8,
    "a stripe is chosen by bits of the unkeyed half of a wide hash and "
    "kept in a byte");

/*
 * The stripe a wide hash chooses, and the 32 bits of it an identifier table
 * is keyed by.  The stripe is shifted out in two steps so that one stripe,
 * zero bits, shifts by 63 and then by one rather than by 64, which C leaves
 * undefined.
 */
static inline size_t
relic_hash_stripe(uint64_t hash)
{
	return ((size_t)(hash >> (63 - RELIC_STRIPE_BITS) >> 1));
}

static inline uint32_t
relic_hash_low(uint64_t hash)
{
	return ((uint32_t)hash);
}

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
 * Whether the table already has room for `added` more entries, so that
 * relic_idtab_reserve() would grow nothing: an owner that asks a limit
 * about each growth need not ask about a reservation that makes none.
 */
bool	 relic_idtab_has_room(const relic_idtab_t *table, size_t added);

/*
 * A reservation in two halves, for an owner that allocates while it may
 * still fail and moves the entries later, possibly on another thread and
 * alongside other tables' moves.
 *
 * relic_idtab_prepare_growth() makes `grown` the empty table
 * relic_idtab_reserve() would grow `table` to for `added` more entries,
 * allocated but holding nothing, or leaves it without slots when `table`
 * already has room.  It is the only half that can fail, returning -1 with
 * `grown` holding nothing.  relic_idtab_complete_growth() then moves every
 * entry of `table` into `grown`, when `grown` has slots, and makes the
 * result `table`, leaving `grown` holding nothing; it cannot fail.  Until
 * then `table` is as it was, and a growth that is not to be completed is
 * released with relic_idtab_reset() on `grown`.
 */
int	 relic_idtab_prepare_growth(const relic_idtab_t *table, size_t added,
	    relic_idtab_t *grown);
void	 relic_idtab_complete_growth(relic_idtab_t *table,
	    relic_idtab_t *grown, relic_rehash_fn rehash, void *ctx);

/*
 * The bytes a table's slots and tags occupy now, and the bytes they would
 * occupy once relic_idtab_reserve() had made room for `added` more entries.
 *
 * These exist for the relation store, whose indexes are accounted against the
 * memory limit and must therefore be measured before they are allocated
 * rather than after.  A reservation that could not grow the table reports the
 * size it already has: the growth will fail as an ordinary allocation
 * failure, which is a different answer from exceeding a limit.
 *
 * relic_idtab_bytes_replaced_by_reserve() is the bytes such a growth
 * replaces: the table's own when it would grow, which a growth prepared with
 * relic_idtab_prepare_growth() keeps allocated beside the grown table until
 * it is completed, and none when the table has room already.
 */
size_t	 relic_idtab_bytes(const relic_idtab_t *table);
size_t	 relic_idtab_bytes_after_reserve(const relic_idtab_t *table,
	    size_t added);
size_t	 relic_idtab_bytes_replaced_by_reserve(const relic_idtab_t *table,
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
