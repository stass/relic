/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_RELATION_H
#define RELIC_RELATION_H

/*
 * This header defines what code compiled against frozen.h alone must not
 * see, and such code defines RELIC_FROZEN_ONLY to have it refused.
 */
#ifdef RELIC_FROZEN_ONLY
#error "code that only reads the store must see it only through frozen.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atomic.h"
#include "frozen.h"
#include "program.h"
#include "util.h"

/*
 * The relation store: where a predicate's tuples live, and a query's answer.
 *
 * A relation is a set of tuples of one arity, held as a flat array of domain
 * identifiers with the arity as the stride and a hash index over the whole
 * tuple.  The set semantics are the point: inserting a tuple the relation
 * already holds changes nothing and says so, which is what fixed-point
 * detection is built on.
 *
 * Nothing in this layer inspects a value.  A column is an opaque 32-bit
 * identifier the caller has already interned, so membership, hashing and
 * duplicate elimination are integer work over a dense array.
 */

typedef struct relic_append relic_append_t;
typedef struct relic_append_keys relic_append_keys_t;
typedef struct relic_base_action relic_base_action_t;
typedef struct relic_builder_batch relic_builder_batch_t;
typedef struct relic_builder_projection relic_builder_projection_t;
typedef struct relic_index_key relic_index_key_t;
typedef struct relic_index_pending relic_index_pending_t;
typedef struct relic_index_stripe relic_index_stripe_t;
typedef struct relic_new_keys relic_new_keys_t;
typedef struct relic_payload relic_payload_t;
typedef struct relic_relation_stats relic_relation_stats_t;
typedef struct relic_reltab relic_reltab_t;
typedef struct relic_store relic_store_t;
typedef struct relic_stripe_fill relic_stripe_fill_t;
typedef struct relic_tuple_growth relic_tuple_growth_t;

/*
 * One distinct key of a secondary index: the first and the last tuple that
 * hold it.  The tuples between first and last are threaded through the
 * index's `next` array in identifier order, so a key's tuples are walked
 * from `first` and a new one is linked after `last`.
 */
struct relic_index_key {
	relic_tuple_id_t	first;
	relic_tuple_id_t	last;
};

/*
 * The keys of a secondary index whose wide hashes choose one stripe
 * (relic_hash_stripe()).  Each stripe numbers its keys from one, in the
 * order they are first seen, on its own: a key is named by its stripe and
 * its identifier within the stripe.
 *
 * `keys` maps the low half of a key's hash to the identifiers of the keys
 * that share it, `entries[k - 1]` records key k's first and last tuple,
 * `frontiers[k - 1]` its frontier, and `key_values[(k - 1) * key_length ..)`
 * holds key k's values, copied from the tuple that introduced it.
 */
struct relic_index_stripe {
	relic_idtab_t		 keys;
	relic_index_key_t	*entries;	/* by key identifier - 1 */
	relic_atomic_u32_t	*frontiers;	/* by key identifier - 1 */
	relic_dom_t		*key_values;	/* by key, key_length each */
	size_t			 key_count;
	size_t			 entry_capacity;
	size_t			 frontier_capacity;
	size_t			 key_value_capacity;	/* in keys */
};

/*
 * One index's part of the insertion the relation store is making, from the
 * moment it classifies the tuple until it links it in: `tuple`, the
 * identifier the tuple will take; `hash`, the wide hash of its key; and
 * `key_id`, the key's identifier within the stripe the hash chooses, or
 * RELIC_ID_NONE for a key the index does not hold yet, which the insertion
 * creates.
 *
 * The reservation between classifying and linking may move a stripe's
 * arrays and rehash its table, but cannot change which keys the index holds
 * or their identifiers.  So the record holds identifiers and a hash, never
 * a position, and stays true until the tuple is linked.
 */
struct relic_index_pending {
	uint64_t		hash;
	uint32_t		key_id;
	relic_tuple_id_t	tuple;
};

/*
 * A secondary index: for every combination of values the key columns hold,
 * the tuples that hold it.
 *
 * `key_mask` has bit c set for each key column c, and `key_column` lists
 * those columns in ascending order, `key_length` of them.  The key of a
 * tuple is its key columns' values in that order.
 *
 * Each distinct key lives in the stripe of `stripes` its hash chooses.
 * `next[id - 1]` is the tuple after `id` with the same key, or
 * RELIC_ID_NONE when `id` is the last.  A key's tuples are therefore a chain
 * through `next`, and because a tuple is linked at the end of its chain when
 * it is inserted, every chain is in increasing identifier order.  `next` is
 * one array for every stripe, by tuple: the slot of a chain's last tuple
 * belongs to that chain, and a new tuple's slot to the new tuple, so tuples
 * of keys in different stripes can be linked at once.
 *
 * The chains are what keep a skewed key cheap.  Storing every tuple identifier
 * in the table directly would put a key's thousand tuples in a thousand
 * consecutive slots, and every other key hashing into that run would walk it.
 * Here the table holds one slot per distinct key however many tuples share
 * it.
 *
 * A key's values are stored beside its chain although its first tuple holds
 * them too.  A probe compares against every key whose slot it visits and
 * whose tag matches, and reading `key_values`, where one key's values follow
 * another's, costs a cache line that neighbouring keys share, where reading
 * the tuple is a random access into the relation.  A key costs `key_length`
 * values more, one to a few columns.  Every table and array is relation and
 * index storage, accounted against the memory limit.
 *
 * A key's frontier is a tuple of its chain from which a lookup of a
 * semi-naive delta starts walking: every chain tuple before it has an
 * identifier below `latest_since`, the largest delta start the index has
 * been raised to, and a new index starts it at RELIC_ID_NONE, below every
 * start.  Deltas only move forward, so the frontiers do too, and positioning
 * a lookup at the start of a delta costs, over a whole fixed point, one pass
 * along each chain.  A new key's frontier is its first tuple, and a frontier
 * is never RELIC_ID_NONE, so tuples linked later stay reachable from it.
 *
 * The frontiers are an array of their own, reached through a pointer, so
 * that relic_index_first_since(), handed the index read-only, can still move
 * them: a lookup of a delta is the one reader that writes.  Keeping them
 * apart from `entries` also keeps the line a lookup reads a key's first
 * tuple from free of the words lookups store to.  The workers of a round
 * look up one delta at once, so each frontier is an atomic word, loaded and
 * stored relaxed: every lookup of one key in one round walks toward the
 * same start and stores the same stop, so no store needs ordering against
 * another, and a stale load only walks further.
 *
 * `pending` is the relation store's, for the one tuple it is inserting:
 * what it found of the tuple's key here, between classifying the tuple and
 * linking it in.  Every step after the classification reads the tuple from
 * it, so none can be handed a different one.
 */
struct relic_index {
	relic_index_stripe_t	 stripes[RELIC_STRIPES];
	relic_tuple_id_t	*next;		/* by tuple identifier - 1 */
	size_t			 next_capacity;
	relic_index_pending_t	 pending;
	relic_tuple_id_t	 latest_since;
	uint32_t		 key_mask;
	uint16_t		 key_length;
	uint16_t		 key_column[RELIC_MAX_ARITY];
};

/*
 * The number of distinct values in each column, computed when the planner
 * asks for one and cached here.
 *
 * `distinct[c]` is meaningful for the columns whose bit is set in `known`,
 * and only while the relation holds `counted_at` tuples.  A relation is
 * append-only until it is cleared, so one holding as many tuples as it did
 * when a count was taken holds the same tuples and the count is still true:
 * the tuple count is the cache's version, and no insertion has to mark
 * anything stale.  Clearing returns a relation to no tuples, possibly to be
 * refilled to the same number with different ones, so clearing it clears
 * `known`.
 */
struct relic_relation_stats {
	uint32_t	distinct[RELIC_MAX_ARITY];
	size_t		counted_at;
	uint32_t	known;
};

/*
 * The tuples of a set and the canonical index over them: the storage a
 * relation holds, and the storage a load's fact builder holds until the
 * commit hands it to an empty base store, which then holds it as its own.
 * A payload has one owner at a time, and handing it over moves it whole,
 * so no tuple array or index is ever copied to change hands.
 *
 * `columns` holds `count` tuples of the owner's arity of identifiers each,
 * end to end, so the tuple with identifier `id` begins at
 * `(id - 1) * arity`.  Capacity is counted in columns rather than in tuples
 * so that one growth policy serves every arity including zero, where a
 * tuple occupies no columns at all.
 *
 * `index` is the canonical index, which answers membership: striped, each
 * stripe maps the low half of the wide hash of a whole tuple to the
 * identifiers of the tuples in that stripe that share it.  It stores
 * identifiers and no keys: the key is the tuple, and the tuple is already in
 * `columns`.  A relation's stripes are untagged, since a membership test
 * that finds a candidate usually finds the tuple.  A builder's are tagged,
 * since a load's tests mostly miss, and a store adopting the payload drops
 * the tags, leaving every entry where it is.
 */
struct relic_payload {
	relic_dom_t		*columns;
	relic_idtab_t		 index[RELIC_STRIPES];
	size_t			 count;			/* tuples */
	size_t			 column_capacity;	/* domain identifiers */
};

/*
 * A set of tuples: one predicate's, or one query's answer.
 *
 * `payload` holds its tuples and its canonical index.
 *
 * `hash_seed` is the context's, copied when the relation is created.  Every
 * hash computed over the relation's tuples is keyed by it: the canonical
 * index's, each secondary index's, and the scratch table's that counts a
 * column's distinct values.
 *
 * `indexes` holds the secondary indexes plans have asked for, at most one per
 * key mask, in the order they were built.  An index is only ever appended,
 * so its position is fixed until the relation is cleared or the next discard
 * and is how a plan names it; its address is not, because the array grows.
 * Clearing a relation drops every index along with the tuples, and a discard
 * drops a base store's indexes and frees a derived relation with its own.
 *
 * `pred` is the predicate whose tuples these are, or RELIC_ID_NONE for a
 * query's answer, which belongs to the query object rather than to any
 * predicate; `serial` is then the serial number of the query, and the
 * relation is called `$q` and that number.  What a relation is called is
 * read from these two and nowhere else.
 *
 * `arity` is fixed when the relation is created: a predicate's arity, or a
 * query's number of output columns.  A predicate's cannot drift, because the
 * arity is half of the predicate's identity: foo/1 and foo/2 are two
 * predicates, so nothing can change the arity of one.
 *
 * None of this is arena memory.  It outlives individual queries, and the
 * tuple and index storage is what is large enough to be worth accounting
 * against a limit.
 */
struct relic_relation {
	relic_payload_t		 payload;
	relic_index_t		*indexes;
	size_t			 index_count;
	size_t			 index_capacity;
	relic_relation_stats_t	 stats;
	uint64_t		 hash_seed;	/* the context's */
	relic_pred_id_t		 pred;	/* RELIC_ID_NONE for an answer */
	size_t			 serial;	/* the query's, for an answer */
	uint16_t		 arity;
};

/*
 * Relations of the program's predicates, indexed by predicate identifier
 * minus one.  A query's answer relation belongs to the query and has no
 * slot here.
 *
 * The table is a prefix of the predicate table, extended to cover a predicate
 * the first time a relation for it is asked for.  A slot is null until then,
 * so a predicate nothing has stored a tuple for costs one pointer.
 *
 * It holds pointers to relations rather than the relations themselves, and
 * that indirection is the whole reason the type is shaped this way.  A
 * relation is created once and then handed out repeatedly: a plan resolves
 * the relation of each body literal, an evaluation holds the head's relation
 * across a rule, and both outlive the creation of other relations.  Were the
 * relations stored inline, extending this array would move every one of them
 * and each of those holders would have to know not to keep a pointer -- a
 * rule obeyed at a growing list of call sites, which is only ever as good as
 * its last entry.  Allocating each relation on its own means growth moves
 * pointers and never relations, so a relation's address is fixed for as long
 * as the relation exists and no caller has anything to remember.
 */
struct relic_reltab {
	relic_relation_t	**entries;	/* null until created */
	size_t			  count;
	size_t			  capacity;
};

/*
 * The relation store: a table of derived relations, which evaluation writes
 * and a discard frees, and a table of base stores, which hold the facts a
 * program states and which only a load's commit writes.  A predicate has
 * one of each at most, created when first asked for.
 *
 * `tuple_count` is the total the configurable tuple limit is enforced
 * against, summed over every relation of the context: derived relations,
 * base stores and a query's answer alike.  The bytes the same relations hold
 * are the store's part of the context's memory account, `store_bytes`: the
 * capacity of every relation's columns and canonical index slots and of
 * every array of its secondary indexes, including one being built -- what
 * is actually held, not the smaller number a tuple count implies.  It counts
 * neither the tables nor the relations they point at, nor a relation's
 * array of index headers, which are sized by the program rather than by the
 * data.
 *
 * `base_tuple_count` is how many of those tuples the base stores hold.  A
 * base store gains tuples only through relic_bases_apply(), which counts
 * them, and loses none until the store is reset, so the count is kept
 * where the tuples arrive rather than summed over every store when asked.
 *
 * `holds_derived` is set whenever the store hands out a derived relation or
 * builds a secondary index, the only two things a discard removes, and
 * cleared by the discard once it has removed them all.  Nothing derived
 * exists while it is clear, so a discard with it clear has nothing to do.
 */
struct relic_store {
	relic_reltab_t	derived;
	relic_reltab_t	bases;
	size_t		tuple_count;
	size_t		base_tuple_count;
	bool		holds_derived;
};

/*
 * How many tuples a relation is about to gain, `by_stripe[s]` of them in
 * stripe s of its canonical index.  The total is their sum,
 * relic_tuple_growth_total(), and is not kept beside them, so that the
 * room made for the columns and the room made in the stripes cannot
 * disagree.
 */
struct relic_tuple_growth {
	size_t	by_stripe[RELIC_STRIPES];
};

/*
 * A load's private set of one predicate's new facts: the distinct tuples
 * its load staged that the predicate `pred`'s base store lacks, in the
 * order the load first staged them, held in the payload a relation of the
 * predicate would hold them in.  `hash_seed` is the context's and `arity`
 * the predicate's, so the builder's canonical index places and finds a
 * tuple exactly as the base store's does.
 *
 * Nothing registers a builder in the store or counts it in either total:
 * it is the load's staging, which the limits bound through the tuples they
 * admit to it.  Only the base commit (relic_bases_apply()) publishes its
 * tuples, by giving an empty base store its whole payload or by appending
 * its tuples to a populated one.
 */
struct relic_builder {
	relic_payload_t		 payload;
	uint64_t		 hash_seed;
	relic_pred_id_t		 pred;
	uint16_t		 arity;
};

/*
 * What a builder grows to for a growth, as relic_builder_project() finds
 * it: the capacity of its columns, in identifiers, and of each stripe of
 * its canonical index, in slots.  Each is one a reservation can allocate.
 */
struct relic_builder_projection {
	size_t	column_capacity;
	size_t	stripe_capacity[RELIC_STRIPES];
};

/*
 * Why a builder can or cannot grow as asked: FULL when it would hold more
 * tuples than identifiers number or columns than a size_t counts, and
 * TOO_LARGE when an array it would grow to holds more bytes than a size_t
 * counts, which no allocator could serve.
 */
enum relic_builder_verdict {
	RELIC_BUILDER_FITS,
	RELIC_BUILDER_FULL,
	RELIC_BUILDER_TOO_LARGE
};

/*
 * A batch of tuples at a builder's arity, some of them new to it, which it
 * takes all at once, as a batch import gives it a window's: tuple i's
 * columns are at `tuples` + i * arity and its wide hash is `hashes[i]`.
 * `ranks[i]` is zero for a tuple the builder is not to take, and otherwise
 * its place among those it takes, from one, in the order it takes them,
 * which no two share.  `stripe_list` lists the tuples by the stripe of the
 * canonical index each falls in, as relic_list_by_stripe() does, those of
 * stripe s from `stripe_start[s]` up to `stripe_start[s + 1]`, so that
 * the part filling a stripe reads its own tuples and no other's.  `count`
 * is how many it ranks.
 */
struct relic_builder_batch {
	const relic_dom_t	*tuples;
	const uint64_t		*hashes;
	const uint32_t		*ranks;
	const uint32_t		*stripe_list;
	const size_t		*stripe_start;
	size_t			 count;
};

/*
 * All that the part filling one canonical stripe of a builder with a
 * batch may write: the columns of the batch's tuple ranked r, at `arity`
 * columns a tuple from `columns`, as the builder's tuple `held` + r, after
 * the tuples it held before the batch, and its entry in `index`, the
 * stripe's table of the canonical index.  The room was made before the
 * fill was described; a fill can grow, count or publish nothing.
 */
struct relic_stripe_fill {
	relic_dom_t		*columns;
	relic_idtab_t		*index;
	size_t			 held;
	size_t			 stripe;
	uint16_t		 arity;
};

/*
 * Tuples a relation takes all at once, as a round's merge or a load's
 * commit gives them: none of them in the relation and no two of them
 * equal, in the order they take their identifiers, tuple i becoming
 * `first + i`.
 *
 * `columns`, `hashes` and `stripes` hold each tuple's columns, its wide
 * hash and the stripe of the canonical index that chooses, and `growth`
 * counts the tuples of each stripe.  `stripe_list` lists the tuples'
 * positions by that stripe, those of stripe s from `stripe_start[s]` up to
 * `stripe_start[s + 1]` in increasing order (relic_list_by_stripe()), so
 * that the part placing a stripe reads its own tuples and no other's.
 * `keys` has `key_count` records, one per secondary index of the relation,
 * in the relation's order.  `grown` holds, between the reservation and the
 * part that places a stripe, the stripe's table grown for its new tuples,
 * allocated and not yet filled, and nothing otherwise.
 *
 * Every per-tuple array of the append and of its keys lies in `block`,
 * one allocation laid out once the count is known, but for a load's
 * columns, which it borrows from the builder; `charged` is what the block
 * is charged to the round, nothing for a load's, which is staging.
 * `reserved` is what the relation holds from the reservation on, the
 * grown tables of the append and of its keys with it, which the storage
 * total counts until the append is finished, when the grown tables have
 * replaced the tables they were grown from.
 *
 * A merge sets `count` and `growth`, relic_relation_append_open() lays
 * out the arrays, and the merge's parts fill them.  A load's commit has
 * its arrays laid out and filled by relic_base_action_prepare(), the
 * columns borrowed.  Either way the reservation sets `first`, `reserved`
 * and `grown`, and relic_relation_append_close() releases the block and
 * any grown table left unfilled.  An append of all zeroes holds nothing.
 * The parts of the work on an append that run on several threads at once
 * write no word another part writes: a part that lists the tuples by
 * stripe writes one index's list, and every other part touches one stripe
 * of one index.
 */
struct relic_append {
	void			*block;
	relic_dom_t		*columns;
	uint64_t		*hashes;
	uint8_t			*stripes;
	uint32_t		*stripe_list;
	size_t			 stripe_start[RELIC_STRIPES + 1];
	relic_append_keys_t	*keys;
	relic_idtab_t		 grown[RELIC_STRIPES];
	relic_tuple_growth_t	 growth;
	relic_tuple_id_t	 first;
	size_t			 count;
	size_t			 key_count;
	size_t			 charged;
	size_t			 reserved;
};

/*
 * What an append brings one secondary index: for each tuple its key's wide
 * hash, the stripe of the index that chooses, and the key it joins there --
 * one the index holds, or a new one, the stripe's new keys numbered on from
 * its last in the order the tuples first bring them.  `stripe_list` and
 * `stripe_start` list the tuples' positions by their key's stripe, as an
 * append's own list them by their canonical stripe.  `new_keys` counts
 * each stripe's new keys, and `grown` holds each stripe's key table grown
 * for them, as an append's own `grown` does.
 */
struct relic_append_keys {
	uint64_t	*hashes;
	uint8_t		*stripes;
	uint32_t	*key_ids;
	uint32_t	*stripe_list;
	size_t		 stripe_start[RELIC_STRIPES + 1];
	size_t		 new_keys[RELIC_STRIPES];
	relic_idtab_t	 grown[RELIC_STRIPES];
};

/*
 * What a load's commit does to the base store of a builder's predicate:
 * give it the builder's tuples, at least one.
 *
 * A store holding no tuple adopts the builder's payload whole, `adopt`
 * set; whether its header exists, and what capacity it kept, does not
 * matter.  A store holding tuples has them appended through `append`,
 * which borrows the builder's columns and holds each tuple's wide hash and
 * canonical stripe, the tuples counted and listed by stripe, all prepared
 * before the commit discards anything, so that preparing them can fail
 * while nothing has changed; it has no secondary index's records, since
 * the commit discards those indexes.  An adoption's append is empty.
 *
 * `store` is the base store, set once the reservation has found or made
 * it, and `projected` the bytes the limits admitted it to hold once the
 * action is applied, which a debug build checks it holds then.  `builder`
 * points into the load's staging, which must not move or release it until
 * the action is released.
 */
struct relic_base_action {
	relic_builder_t		*builder;
	relic_relation_t	*store;
	relic_append_t		 append;
	size_t			 projected;
	bool			 adopt;
};

/*
 * The keys new to a stripe of an index that an append's tuples bring,
 * while they are being classified: `table` maps the low half of a new
 * key's hash to its position among the stripe's new keys plus one, and
 * `first[k]` is the tuple of the append that brought new key k, whose
 * columns hold its values.  It is made with room for every tuple of the
 * stripe, which no number of new keys exceeds, so that classifying never
 * allocates.
 */
struct relic_new_keys {
	relic_idtab_t	 table;
	uint32_t	*first;
};

#endif /* RELIC_RELATION_H */
