/*
 * Copyright (c) 2026 Stanislav Sedov
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

typedef struct relic_index_key relic_index_key_t;
typedef struct relic_index_pending relic_index_pending_t;
typedef struct relic_index_stripe relic_index_stripe_t;
typedef struct relic_relation_stats relic_relation_stats_t;
typedef struct relic_base_growth relic_base_growth_t;
typedef struct relic_reltab relic_reltab_t;
typedef struct relic_store relic_store_t;
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
 * and only while the relation holds `counted_at` tuples.  The store is
 * append-only between discards, so a relation holding as many tuples as it
 * did when a count was taken holds the same tuples and the count is still
 * true: the tuple count is the cache's version, and no insertion has to mark
 * anything stale.  A discard returns a relation to fewer tuples, possibly
 * to be refilled to the same number with different ones, so a discard clears
 * `known`.
 */
struct relic_relation_stats {
	uint32_t	distinct[RELIC_MAX_ARITY];
	size_t		counted_at;
	uint32_t	known;
};

/*
 * A set of tuples: one predicate's, or one query's answer.
 *
 * `columns` holds `count` tuples of `arity` identifiers each, end to end, so
 * the tuple with identifier `id` begins at `(id - 1) * arity`.  Capacity is
 * counted in columns rather than in tuples so that one growth policy serves
 * every arity including zero, where a tuple occupies no columns at all.
 *
 * `index` is the canonical index, which answers membership: striped, each
 * stripe maps the low half of the wide hash of a whole tuple to the
 * identifiers of the tuples in that stripe that share it.  It stores
 * identifiers and no keys: the key is the tuple, and the tuple is already in
 * `columns`.
 *
 * `hash_seed` is the context's, copied when the relation is created.  Every
 * hash computed over the relation's tuples is keyed by it: the canonical
 * index's, each secondary index's, and the scratch table's that counts a
 * column's distinct values.
 *
 * `indexes` holds the secondary indexes plans have asked for, at most one per
 * key mask, in the order they were built.  Between discards an index is only
 * ever appended, so its position is fixed until the next discard and is how
 * a plan names it; its address is not, because the array grows.  A discard
 * drops every index along with the tuples.
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
	relic_dom_t		*columns;
	relic_idtab_t		 index[RELIC_STRIPES];
	relic_index_t		*indexes;
	size_t			 count;			/* tuples */
	size_t			 column_capacity;	/* domain identifiers */
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
 * pointers and never relations, so a relation's address is fixed for the life
 * of the context and no caller has anything to remember.
 */
struct relic_reltab {
	relic_relation_t	**entries;	/* null until created */
	size_t			  count;
	size_t			  capacity;
};

/*
 * The relation store: a table of derived relations, which evaluation writes
 * and a discard empties, and a table of base stores, which hold the facts a
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
 * `holds_derived` is set whenever the store hands out a derived relation to
 * be written or builds a secondary index, the only two things a discard
 * removes, and cleared by the discard.  A discard with it clear has nothing
 * to do.
 */
struct relic_store {
	relic_reltab_t	derived;
	relic_reltab_t	bases;
	size_t		tuple_count;
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
 * How many facts a load's commit adds to one predicate's base store.
 */
struct relic_base_growth {
	relic_pred_id_t		pred;
	relic_tuple_growth_t	added;
};

#endif /* RELIC_RELATION_H */
