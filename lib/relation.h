/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_RELATION_H
#define RELIC_RELATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

typedef struct relic_index relic_index_t;
typedef struct relic_index_key relic_index_key_t;
typedef struct relic_relation relic_relation_t;
typedef struct relic_relation_stats relic_relation_stats_t;
typedef struct relic_reltab relic_reltab_t;
typedef struct relic_tuple_range relic_tuple_range_t;

/*
 * A tuple, identified by its position in its relation.
 *
 * Identifiers are assigned in insertion order starting at one, so a relation
 * holds at most 2^32 - 2 tuples and identifier zero names no tuple, following
 * the convention of RELIC_ID_NONE.  The typedef says, by convention rather
 * than by anything the compiler enforces, that this is neither a domain
 * identifier nor a predicate identifier.
 *
 * Unlike a domain identifier, a tuple identifier's numeric order is
 * meaningful: it is insertion order, and that is exactly what lets the tuples
 * derived in one semi-naive iteration be named by a range rather than copied
 * into a relation of their own.
 */
typedef uint32_t relic_tuple_id_t;

#define	RELIC_TUPLE_FIRST	1u

/*
 * The most tuples a relation holds, and so the most a delta or any range of
 * one does.  The store checks it rather than assuming it: an identifier that
 * wrapped would silently make one tuple into another.
 */
#define	RELIC_TUPLE_MAX		(UINT32_MAX - 1u)

/*
 * A half-open range of tuple identifiers, which is how a relation is
 * iterated and how a semi-naive delta is represented.
 *
 * A range is a value and holds no pointer into the relation, so inserting
 * into the relation cannot invalidate one that is being walked; a consumer
 * reads each tuple through relic_relation_tuple() in the ordinary way.
 */
struct relic_tuple_range {
	relic_tuple_id_t	first;	/* inclusive */
	relic_tuple_id_t	end;	/* exclusive */
};

/*
 * One distinct key of a secondary index: the first and the last tuple that
 * hold it, and its frontier.  The tuples between first and last are threaded
 * through the index's `next` array in identifier order, so a key's tuples
 * are walked from `first` and a new one is linked after `last`.
 *
 * The frontier is a tuple of the chain from which a lookup of a semi-naive
 * delta starts walking: every chain tuple before it has an identifier below
 * the index's `latest_since`, the largest delta start it has been asked
 * for.  Deltas only move forward, so the frontier does too, and positioning
 * a lookup at the start of a delta costs, over a whole fixed point, one pass
 * along each chain.  A new key's frontier is its first tuple, and a frontier
 * is never RELIC_ID_NONE, so tuples linked later stay reachable from it.
 */
struct relic_index_key {
	relic_tuple_id_t	first;
	relic_tuple_id_t	last;
	relic_tuple_id_t	frontier;
};

/*
 * A secondary index: for every combination of values the key columns hold,
 * the tuples that hold it.
 *
 * `key_mask` has bit c set for each key column c.  The key of a tuple is its
 * key columns' values in ascending column order.
 *
 * Each distinct key is assigned a key identifier, from one, in the order keys
 * are first seen.  `keys` maps the hash of a key to the key identifiers that
 * share it, and `entries[k - 1]` records key k's first and last tuple.
 * `next[id - 1]` is the tuple after `id` with the same key, or RELIC_ID_NONE
 * when `id` is the last.  A key's tuples are therefore a chain through
 * `next`, and because a tuple is linked at the end of its chain when it is
 * inserted, every chain is in increasing identifier order.
 *
 * The chains are what keep a skewed key cheap.  Storing every tuple identifier
 * in the table directly would put a key's thousand tuples in a thousand
 * consecutive slots, and every other key hashing into that run would walk it.
 * Here the table holds one slot per distinct key however many tuples share
 * it.
 *
 * No key is stored: comparing against key k reads the key columns of
 * entries[k - 1].first, which the relation already holds.  All three arrays
 * are relation and index storage, accounted against the memory limit.
 *
 * `latest_since` is the largest delta start a lookup has been positioned
 * at, which is what the frontiers are valid for; it is cleared when the
 * index is emptied.
 */
struct relic_index {
	relic_idtab_t		 keys;
	relic_index_key_t	*entries;	/* indexed by key identifier - 1 */
	relic_tuple_id_t	*next;		/* indexed by tuple identifier - 1 */
	size_t			 key_count;
	size_t			 entry_capacity;
	size_t			 next_capacity;
	relic_tuple_id_t	 latest_since;
	uint32_t		 key_mask;
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
 * `index` maps the hash of a whole tuple to the identifiers that share it,
 * which is what answers membership.  It stores identifiers and no keys: the
 * key is the tuple, and the tuple is already in `columns`.
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
	relic_idtab_t		 index;
	relic_index_t		*indexes;
	size_t			 count;			/* tuples */
	size_t			 column_capacity;	/* domain identifiers */
	size_t			 index_count;
	size_t			 index_capacity;
	relic_relation_stats_t	 stats;
	uint64_t		 hash_seed;	/* the context's */
	relic_pred_id_t		 pred;		/* RELIC_ID_NONE for an answer */
	size_t			 serial;	/* the query's, for an answer */
	uint16_t		 arity;
};

/*
 * The relations of the program's predicates, indexed by predicate
 * identifier minus one.  A query's answer relation belongs to the query and
 * has no slot here.
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
 *
 * `tuple_count` is the total the configurable tuple limit is enforced
 * against, summed over every relation of the context, whether it is in this
 * table or is a query's answer.  The bytes the same relations hold are the
 * store's part of the context's memory account, `store_bytes`: the capacity
 * of every relation's columns and canonical index slots and of every array
 * of its secondary indexes, including one being built -- what is actually
 * held, not the smaller number a tuple count implies.  It counts neither
 * this array nor the relations it points at, nor a relation's array of index
 * headers, which are sized by the program rather than by the data.
 */
struct relic_reltab {
	relic_relation_t	**entries;	/* null until created */
	size_t			  count;
	size_t			  capacity;
	size_t			  tuple_count;
};

#endif /* RELIC_RELATION_H */
