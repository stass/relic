/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_WINDOW_H
#define RELIC_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal.h"

/*
 * Batch import (specs/design-bulk-load.md): a window of a table's input,
 * cut at a record boundary, staged all at once or not at all.
 *
 * lib/import.c frames a window into pieces of whole records, then calls
 * the functions here, which read the window's bytes and the context and
 * write nothing but the window: relic_window_decode_piece() gives each
 * piece a dictionary of the distinct values its kept fields hold and a
 * token per field; relic_window_resolve() turns every token into the
 * identifier the value layer promised its value; relic_window_classify()
 * finds the tuples new to the predicate's builder and base store, and to
 * the window.  The importer then proves the window admissible, reserves,
 * and publishes; or it declines, and reads the window again one field at a
 * time.
 */

typedef struct relic_window_piece relic_window_piece_t;
typedef struct relic_import_plan relic_import_plan_t;
typedef struct relic_window_value relic_window_value_t;
typedef struct relic_window relic_window_t;

/*
 * What an import reads, copied from the caller's description once it has
 * been checked.  The description is borrowed, and caller code runs while a
 * table is read -- the stream's read function, the diagnostic callback --
 * so nothing is read from it after the copy, and a description changed
 * behind the import's back cannot make it keep more columns than it
 * checked.  `columns` and `predicate` are owned, `predicate` the name the
 * caller gave, `predicate_length` bytes long; `arity` is the number of
 * columns not skipped.
 */
struct relic_import_plan {
	enum relic_column	*columns;
	char			*predicate;
	size_t			 predicate_length;
	size_t			 column_count;
	size_t			 arity;
	enum relic_format	 format;
	bool			 header;
};

/*
 * A value of a piece's dictionary: an integer, or a symbol's or a string's
 * bytes in the window's decoded bytes, with the hash the value layer looks
 * it up by.
 */
struct relic_window_value {
	relic_pending_t		 value;
	uint32_t		 hash;
};

/*
 * A piece of a window: whole records, `length` bytes from offset `start`,
 * the first at `where`.  `header` says its first record is the table's
 * header, which yields no tuple, and `last` that the input ends with it.
 * Its `tuples` tuples are the window's from `first_tuple`.  The importer
 * frames these.
 *
 * Decoding fills the rest.  `values` lists the distinct values its kept
 * fields hold, `value_count` of them, in the order they first occur, and
 * `seen` finds one by its hash.  A symbol and a string of the same bytes
 * are two values.  Their bytes are in the window's decoded bytes, within
 * the piece's own `length` from `start`: a field decodes to no more bytes
 * than it is written in.  The piece's tuples hold, for each kept field,
 * the position of its value in `values`.  `valid` says every field held a
 * value its column accepts and nothing ran out of memory.
 */
struct relic_window_piece {
	relic_window_value_t	*values;
	relic_idtab_t		 seen;
	size_t			 value_count;
	size_t			 value_capacity;
	size_t			 start;
	size_t			 length;
	size_t			 first_tuple;
	size_t			 tuples;
	relic_location_t	 where;
	bool			 header;
	bool			 last;
	bool			 valid;
};

/*
 * A window: the `length` bytes of `input`, which it borrows, up to the end
 * of their last whole record, framed into `piece_count` pieces holding
 * `tuple_count` tuples.  `end` is the location of the byte after them, and
 * `ends_input` says the input ends with them.
 *
 * `decoded` holds the pieces' distinct decoded bytes, each piece's at the
 * offsets its raw bytes have.  `tuples` holds every tuple of the window at
 * `arity` columns, by position in the input; `hashes` each one's wide hash
 * once resolved.  `fresh` lists, in input order, the positions of the
 * tuples new to the builder, the base store and every earlier tuple of the
 * window, `fresh_count` of them, `growth` counting them by stripe, and
 * `screen` finds one by its hash.  `values` gathers the pieces'
 * dictionaries, in piece order, for the value layer to number, and `map`
 * is what it numbered them as.
 *
 * The arrays sized by a window's bytes and records are kept from one
 * window to the next; the pieces' dictionaries, the screen and the map
 * belong to one window.
 */
struct relic_window {
	const char		*input;
	relic_window_piece_t	*pieces;
	char			*decoded;
	relic_dom_t		*tuples;
	uint64_t		*hashes;
	uint32_t		*fresh;
	relic_pending_t		*values;
	relic_pending_map_t	 map;
	relic_idtab_t		 screen;
	relic_tuple_growth_t	 growth;
	relic_location_t	 end;
	size_t			 length;
	size_t			 piece_count;
	size_t			 piece_capacity;
	size_t			 decoded_capacity;
	size_t			 tuple_count;
	size_t			 tuple_capacity;
	size_t			 hash_capacity;
	size_t			 fresh_count;
	size_t			 fresh_capacity;
	size_t			 value_count;
	size_t			 value_capacity;
	uint16_t		 arity;
	bool			 ends_input;
	bool			 mapped;
};

/*
 * relic_window_init() makes an empty window of tuples of `arity` columns.
 * relic_window_clear() forgets the window it holds, keeping the arrays
 * sized by a window's bytes and records for the next and releasing the
 * pieces' dictionaries, the screen and the map, and relic_window_reset()
 * releases everything.
 *
 * The importer frames a window by adding each piece with
 * relic_window_add_piece(), which returns it zeroed, or NULL when memory
 * ran out, and then sizes the window's arrays for its length and its
 * tuples with relic_window_size().  relic_window_decode_piece() decodes a
 * piece, which only reads the context.  relic_window_gather() then gathers
 * the pieces' dictionaries into `values`, in piece order, and opens `map`
 * with room for them.  Once the value layer has measured them into the
 * map, relic_window_resolve() turns every tuple's tokens into the
 * identifiers the map promised and hashes it, and relic_window_classify()
 * finds the fresh tuples, given the predicate's builder and base store,
 * either of which may be NULL.  Every function that returns an int returns
 * 0, or -1 having reported nothing.
 */
void	 relic_window_init(relic_window_t *window, uint16_t arity);
void	 relic_window_clear(relic_window_t *window);
void	 relic_window_reset(relic_window_t *window);
relic_window_piece_t *relic_window_add_piece(relic_window_t *window);
int	 relic_window_size(relic_window_t *window);
int	 relic_window_decode_piece(const relic_t *ctx,
	    const relic_import_plan_t *plan, const char *source_name,
	    relic_window_t *window, relic_window_piece_t *piece);
int	 relic_window_gather(relic_window_t *window);
void	 relic_window_resolve(const relic_t *ctx, relic_window_t *window);
int	 relic_window_classify(relic_window_t *window,
	    const relic_builder_t *builder, const relic_relation_t *store);

#endif /* RELIC_WINDOW_H */
