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
 * The relation store: where a predicate's tuples live.
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

typedef struct relic_relation relic_relation_t;
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
 * One predicate's tuples.
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
 * `arity` is copied from the predicate rather than read back through it on
 * every insertion.  It cannot drift, because the arity is half of the
 * predicate's identity: foo/1 and foo/2 are two predicates, so nothing can
 * change the arity of one.
 *
 * Neither allocation is arena memory.  They outlive individual queries and
 * they are the allocations large enough to be worth accounting against a
 * limit.
 */
struct relic_relation {
	relic_dom_t	*columns;
	relic_idtab_t	 index;
	size_t		 count;			/* tuples */
	size_t		 column_capacity;	/* domain identifiers */
	relic_pred_id_t	 pred;
	uint16_t	 arity;
};

/*
 * The relations of a program, indexed by predicate identifier minus one.
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
 * `tuple_count` and `storage_bytes` are the totals the configurable tuple and
 * memory limits are enforced against.  `storage_bytes` counts the capacity of
 * every relation's columns and index slots -- what is actually held, not the
 * smaller number a tuple count implies.  It counts neither this array nor the
 * relations it points at, which are sized by the program rather than by the
 * data.
 */
struct relic_reltab {
	relic_relation_t	**entries;	/* null until created */
	size_t			  count;
	size_t			  capacity;
	size_t			  tuple_count;
	size_t			  storage_bytes;
};

#endif /* RELIC_RELATION_H */
