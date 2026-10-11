/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_FROZEN_H
#define RELIC_FROZEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <relic.h>

#include "program.h"
#include "util.h"

/*
 * The store as code that must only read it sees it.
 *
 * The executor and the work estimator (lib/cost.c) read the context, its
 * relations and their indexes and must never write them.  They are
 * compiled against this header and not against the ones that define a
 * context, a relation or an index, which refuse to be included beside it
 * (RELIC_FROZEN_ONLY), so here those types are incomplete.  Such code can
 * hold a pointer to one and pass it on, and it cannot name a member of
 * one, to read it or to write it.  What it can do with them is exactly the
 * functions below, each of which takes them const and hands back nothing
 * that could be written through without a cast that discards const, which
 * the compiler's warnings report.
 *
 * A const pointer alone would not do: it covers the members of the
 * structure it points at and not what their pointers reach, and a relation
 * reaches its columns and its indexes through pointers.  An incomplete type
 * covers everything behind it, every array a relation holds and any it
 * comes to hold, so a write to the store through a member of one does not
 * compile.
 *
 * Such code stores to two words outside its own: a key's frontier, which
 * relic_index_first_since() moves inside lib/index.c, and the memory
 * account's total for a round's buffers, which it reaches only through the
 * two functions of lib/account.c below.  The account is incomplete here
 * too, so no other part of it can be written.
 */

typedef struct relic_account relic_account_t;
typedef struct relic_index relic_index_t;
typedef struct relic_relation relic_relation_t;
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
 * The result of comparing two values.
 *
 * Ordering is defined only within a type: a mixed-type pair is unordered
 * rather than erroneous, because predicate arguments are untyped and a column
 * may legitimately hold more than one type.  The evaluator turns
 * RELIC_ORDER_UNORDERED into a filtered-out binding.
 */
enum relic_order {
	RELIC_ORDER_LESS = -1,
	RELIC_ORDER_EQUAL = 0,
	RELIC_ORDER_GREATER = 1,
	RELIC_ORDER_UNORDERED = 2
};

/*
 * lib/relation.c
 *
 * relic_relation_pred() gives the predicate a relation is read as, which is
 * what a semi-naive delta is kept under.  relic_relation_all() gives the
 * range of every tuple the relation holds.  relic_relation_tuple() gives the
 * columns of the tuple an identifier names, or NULL for one the relation has
 * not assigned; the pointer is into the relation's storage and good until
 * the next insertion.  relic_relation_find() gives the identifier of the
 * tuple holding some columns, or RELIC_ID_NONE.
 * relic_relation_index_at() gives the secondary index at a position
 * relic_relation_index() gave, found afresh each time since the array
 * holding it can grow between two lookups.  relic_relation_index_find()
 * gives the position of the index keyed on a mask, if the relation has
 * built one, and builds none.  relic_relation_arity() gives the columns of
 * each of a relation's tuples.
 *
 * relic_relation_screen() looks up a batch of tuples packed at the
 * relation's arity as inserting them would, and inserts none: it gives
 * each tuple's wide hash, which relic_relation_insert_batch_hashed() takes,
 * and whether the relation holds it.  relic_relation_hash() gives one
 * tuple's wide hash alone, the same one.
 */
relic_pred_id_t relic_relation_pred(const relic_relation_t *relation);
uint16_t relic_relation_arity(const relic_relation_t *relation);
relic_tuple_range_t relic_relation_all(const relic_relation_t *relation);
const relic_dom_t *relic_relation_tuple(const relic_relation_t *relation,
	    relic_tuple_id_t id);
relic_tuple_id_t relic_relation_find(const relic_relation_t *relation,
	    const relic_dom_t *columns);
const relic_index_t *relic_relation_index_at(const relic_relation_t *relation,
	    uint32_t position);
bool	 relic_relation_index_find(const relic_relation_t *relation,
	    uint32_t key_mask, uint32_t *positionp);
void	 relic_relation_screen(const relic_relation_t *relation,
	    const relic_dom_t *tuples, size_t count, uint64_t *hashes,
	    bool *held);
uint64_t relic_relation_hash(const relic_relation_t *relation,
	    const relic_dom_t *tuple);

/*
 * Whether two tuples of one arity hold the same values.
 *
 * The columns are compared as identifiers rather than as bytes.  A dense
 * array of uint32_t has no padding, so a byte comparison would agree, but it
 * is not defined for the empty tuple: comparing zero bytes at a null pointer
 * is something the standard does not permit and a sanitizer reports.  The
 * loop is defined at every arity and needs no case for the one that would
 * break the other form.  It is inline because every lookup of a tuple, and
 * every screening of one, compares with it.
 */
static inline bool
relic_tuple_equal(const relic_dom_t *left, const relic_dom_t *right,
    uint16_t arity)
{
	uint16_t column;

	for (column = 0; column < arity; column++) {
		if (left[column] != right[column])
			return (false);
	}

	return (true);
}

/*
 * lib/index.c
 *
 * relic_index_key_mask() gives the columns an index is keyed on, bit c for
 * column c, and relic_index_key_count() the distinct keys it holds over its
 * whole relation: none for an empty relation, and otherwise at least one
 * and no more than the relation's tuples.  relic_index_first() and
 * relic_index_next() walk, in identifier order, the tuples of `relation`
 * that agree with a probe on those columns; the probe is laid out as a
 * tuple, and its other columns are not read.
 * relic_index_first_since() starts such a walk at the first of those tuples
 * whose identifier is at least `since`, from the key's frontier, which it
 * moves forward when `since` is the start relic_index_raise_since() last
 * raised the index to.  `since` must be no later than that start: the
 * frontier stands for every start up to it, and a lookup from a later one
 * would have to walk the chain from its head.
 */
uint32_t relic_index_key_mask(const relic_index_t *index);
size_t	 relic_index_key_count(const relic_index_t *index);
relic_tuple_id_t relic_index_first(const relic_index_t *index,
	    const relic_relation_t *relation, const relic_dom_t *probe);
relic_tuple_id_t relic_index_first_since(const relic_index_t *index,
	    const relic_relation_t *relation, const relic_dom_t *probe,
	    relic_tuple_id_t since);
relic_tuple_id_t relic_index_next(const relic_index_t *index,
	    relic_tuple_id_t id);

/*
 * The longest byte string a symbol or string value holds: its length fits
 * a pool entry's, and the length with a terminator's byte fits a size_t
 * however narrow.  The pool refuses to intern a longer one, and the
 * executor refuses to compute one.  A test build may define it lower, so
 * that both refusals can be reached, and never higher.
 */
#ifndef RELIC_STRING_LENGTH_MAX
#define	RELIC_STRING_LENGTH_MAX	(UINT32_MAX - 1)
#endif

_Static_assert(RELIC_STRING_LENGTH_MAX <= UINT32_MAX - 1,
    "a string's length must fit a pool entry's");

/*
 * lib/value.c
 *
 * relic_value_order() orders two values of one type and calls two of
 * different types, or an identifier the context never assigned, unordered.
 *
 * relic_bytes_order() orders two byte strings as the language orders
 * strings: bytewise, the shorter first when one begins the other.
 *
 * relic_value_integer() gives the integer an identifier names, and false
 * for a symbol, a string or an identifier never assigned.
 * relic_value_bytes() gives the bytes of the symbol or string an
 * identifier names and their length, and NULL for an integer or an
 * identifier never assigned; the bytes never move while the context
 * exists.  relic_value_find_integer() and relic_value_find_string() give
 * the identifier the context has assigned an integer or a string of some
 * bytes, or zero, which names no value, when it has assigned none: the
 * lookup half of interning, which writes nothing.  Bytes the pool holds
 * only as a name or a symbol's spelling are no string the context has
 * assigned.
 */
enum relic_order relic_value_order(const relic_t *ctx, relic_dom_t a,
	    relic_dom_t b);
enum relic_order relic_bytes_order(const char *left, size_t left_length,
	    const char *right, size_t right_length);
bool	 relic_value_integer(const relic_t *ctx, relic_dom_t dom,
	    int64_t *integerp);
const char *relic_value_bytes(const relic_t *ctx, relic_dom_t dom,
	    size_t *lengthp);
relic_dom_t relic_value_find_integer(const relic_t *ctx, int64_t integer);
relic_dom_t relic_value_find_string(const relic_t *ctx, const char *bytes,
	    size_t length);

/*
 * lib/account.c
 *
 * relic_account_charge_round() admits `bytes` more for a round's buffers
 * and counts them, or returns false having counted nothing; unlike an
 * arena's refusal, a round's is not recorded, since nothing reports it.
 * relic_account_release_round() gives back bytes a round counted.
 */
bool	 relic_account_charge_round(relic_account_t *account, size_t bytes);
void	 relic_account_release_round(relic_account_t *account, size_t bytes);

#endif /* RELIC_FROZEN_H */
