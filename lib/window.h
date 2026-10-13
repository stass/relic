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

#include <relic.h>

#include "exec.h"
#include "frozen.h"
#include "table.h"
#include "util.h"

/*
 * Batch import: a window of a table's input, cut at a record boundary,
 * staged all at once or not at all.
 *
 * lib/import.c frames a window into pieces of whole records, with a
 * framer, and then analyzes it in steps, each a job of parts that the
 * context's pool runs at once (round.h), with a step of its own between
 * two jobs, while the calling thread frames the next window between and
 * instead of its own parts of the jobs that only read:
 *
 *	decode		a piece to a part: a dictionary of the distinct
 *			values its kept fields hold, and a token per field
 *	gather		the dictionaries copied, in piece order, and
 *			their values divided among the value parts by hash
 *	find		a value part to a part: each value found in the
 *			context or among the values before it
 *			(relic_pending_find())
 *	number		every value promised its identifier, in order
 *			(relic_pending_number())
 *	resolve		a piece to a part: every token turned into the
 *			identifier promised its value, and every tuple
 *			hashed
 *	list		the tuples listed by canonical stripe
 *	classify	a stripe to a part: the tuples new to the
 *			predicate's builder, its base store and the window
 *	rank		the new tuples numbered, in input order
 *
 * The importer then proves the window admissible, reserves, interns the
 * values and fills the builder a stripe to a part; or it declines, and
 * reads the window again one field at a time.
 *
 * This file and lib/window.c are compiled against frozen.h alone
 * (RELIC_FROZEN_ONLY): the parts read the context, the builder and the
 * store through the const functions it declares, and write nothing but
 * the window, each part its own piece, its own values' identifiers or its
 * own stripe's tuples, so that no two parts write one memory location.
 * None of them interns a value, stages a tuple or reports anything.
 */

typedef struct relic_window_piece relic_window_piece_t;
typedef struct relic_import_plan relic_import_plan_t;
typedef struct relic_window_value relic_window_value_t;
typedef struct relic_window relic_window_t;
typedef struct relic_window_job relic_window_job_t;
typedef struct relic_framer relic_framer_t;

/*
 * The values a window holds are divided among this many parts for finding
 * them, by the top bits of their hashes, so that equal values meet in one
 * part.  The count is a constant rather than the number of threads, so
 * which part finds a value is the same whatever the number of threads;
 * what each finds does not depend on it either way.
 */
#define	RELIC_WINDOW_VALUE_PART_BITS	6
#define	RELIC_WINDOW_VALUE_PARTS ((size_t)1 << RELIC_WINDOW_VALUE_PART_BITS)

/*
 * What an import reads, copied from the caller's description once it has
 * been checked.  The description is borrowed, and caller code runs while a
 * table is read -- the stream's read function, the diagnostic callback --
 * so nothing is read from it after the copy, and a description changed
 * behind the import's back cannot make it keep more columns than it
 * checked.  `columns` and `predicate` are owned, `predicate` the name the
 * caller gave, `predicate_length` bytes long; `arity` is the number of
 * columns not skipped.  `field_max`, the input limit as relic_input_max()
 * gives it, is the bound every reader of the table is made with, so the
 * scalar importer and a window's decoders refuse the same field.
 */
struct relic_import_plan {
	enum relic_column	*columns;
	char			*predicate;
	size_t			 predicate_length;
	size_t			 column_count;
	size_t			 arity;
	size_t			 field_max;
	enum relic_format	 format;
	bool			 header;
};

/*
 * A value of a piece's dictionary: an integer, or a symbol's or a string's
 * bytes in the window's decoded bytes, with the hash the value layer looks
 * it up by, relic_pending_hash().
 */
struct relic_window_value {
	relic_pending_t		 value;
	uint64_t		 hash;
};

/*
 * A piece of a window: whole records, `length` bytes from offset `start`,
 * the first at `where`.  `header` says its first record is the table's
 * header, which yields no tuple, and `last` that the input ends with it.
 * Its `tuples` tuples are the window's from `first_tuple`.  A framer
 * frames these.
 *
 * Decoding fills the rest.  `values` lists the distinct values its kept
 * fields hold, `value_count` of them, in the order they first occur, and
 * `seen` finds one by its hash.  A symbol and a string of the same bytes
 * are two values.  Their bytes are in the window's decoded bytes, within
 * the piece's own `length` from `start`: a field decodes to no more bytes
 * than it is written in.  The piece's tuples hold, for each kept field,
 * the position of its value in `values`.  `valid` says every field held a
 * value its column accepts and nothing ran out of memory.  Gathering sets
 * `first_value`, where the piece's values begin among the window's.
 */
struct relic_window_piece {
	relic_window_value_t	*values;
	relic_idtab_t		 seen;
	size_t			 value_count;
	size_t			 value_capacity;
	size_t			 first_value;
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
 * offsets its raw bytes have.  `values` and `value_hashes` gather the
 * pieces' dictionaries, in piece order, `value_count` of them, for the
 * value layer to number; `value_members` lists them by value part, those
 * of part p from `value_start[p]` up to `value_start[p + 1]` in increasing
 * order, and the part's table of them is `value_slot_count[p]` of
 * `value_slots` from `value_slot_start[p]`.
 *
 * `tuples` holds every tuple of the window at `arity` columns, by position
 * in the input, and `hashes` and `stripes` each one's wide hash and the
 * stripe of the canonical index it chooses once resolved.  `stripe_list`
 * lists the tuples by stripe, as relic_list_by_stripe() lists them from
 * `stripe_start`, and stripe s's classify part screens its tuples new to
 * the builder and the store by hash, in a table of their positions plus
 * one whose slots lie in `screen_memory` from `screen_start[s]`.
 * `ranks` holds one for each new tuple and zero for every other once the
 * classify parts have marked them, and relic_window_rank() then gives each
 * new tuple its place among the window's, from one, in input order; there
 * are `fresh_count` new tuples, `fresh_by_stripe[s]` of them in stripe s.
 *
 * Every array here is kept from one window to the next, and released by
 * relic_window_reset(); the pieces' dictionaries are not.
 */
struct relic_window {
	const char		*input;
	relic_window_piece_t	*pieces;
	char			*decoded;
	relic_pending_t		*values;
	uint64_t		*value_hashes;
	uint32_t		*value_members;
	uint32_t		*value_slots;
	relic_dom_t		*tuples;
	uint64_t		*hashes;
	uint8_t			*stripes;
	uint32_t		*stripe_list;
	uint32_t		*ranks;
	char			*screen_memory;
	size_t			 value_start[RELIC_WINDOW_VALUE_PARTS + 1];
	size_t			 value_slot_start[RELIC_WINDOW_VALUE_PARTS];
	size_t			 value_slot_count[RELIC_WINDOW_VALUE_PARTS];
	size_t			 stripe_start[RELIC_STRIPES + 1];
	size_t			 screen_start[RELIC_STRIPES];
	size_t			 fresh_by_stripe[RELIC_STRIPES];
	relic_location_t	 end;
	size_t			 length;
	size_t			 piece_count;
	size_t			 piece_capacity;
	size_t			 decoded_capacity;
	size_t			 value_count;
	size_t			 value_capacity;
	size_t			 value_hash_capacity;
	size_t			 value_member_capacity;
	size_t			 value_slot_capacity;
	size_t			 tuple_count;
	size_t			 tuple_capacity;
	size_t			 hash_capacity;
	size_t			 stripe_capacity;
	size_t			 stripe_list_capacity;
	size_t			 rank_capacity;
	size_t			 screen_capacity;
	size_t			 fresh_count;
	uint16_t		 arity;
	bool			 ends_input;
};

/*
 * How a framer stands: framing nothing, before it is begun or once the
 * window has taken what it framed; framing a window; having framed it
 * into at least one whole record; or having left it unframed, which
 * declines the window.
 */
enum relic_framing {
	RELIC_FRAMING_NONE,
	RELIC_FRAMING,
	RELIC_FRAMED,
	RELIC_UNFRAMED
};

/*
 * A window being framed, by `reader`, which reads the table's structure
 * alone, begun at the window's first byte.  The window is the whole
 * records within `horizon` bytes of that byte, or before the end of input
 * when that comes first, cut into pieces of at least `piece_bytes` but
 * the last.  `buffered` of its bytes are in memory, and `ended` says the
 * stream ended successfully after them; `fed` of them have been fed the
 * reader.
 *
 * The rest is the window's manifest as far as it is framed, which the
 * window takes once it is: `piece_count` pieces in `pieces`, holding
 * `tuple_count` tuples and the window's first `length` bytes, the end of
 * the last record framed, whose next byte is at `end`.  `ends_input` says
 * the table ends with them.  `header` says the next record is the
 * table's header, and `open` that the last piece takes it.
 */
struct relic_framer {
	relic_table_reader_t	 reader;
	relic_window_piece_t	*pieces;
	relic_location_t	 end;
	size_t			 horizon;
	size_t			 piece_bytes;
	size_t			 buffered;
	size_t			 fed;
	size_t			 length;
	size_t			 piece_count;
	size_t			 piece_capacity;
	size_t			 tuple_count;
	enum relic_framing	 state;
	bool			 ended;
	bool			 header;
	bool			 open;
	bool			 ends_input;
};

/*
 * What the parts of a window's jobs read, and where they write: the
 * context, the import's plan and source name, the window, and the
 * predicate's builder and base store, either of which may be NULL.  `ids`
 * is the value layer's map of the window's values to identifiers, which
 * the find parts write and the resolve parts read, and `tuple_seed` the
 * seed every relation hashes tuples under, copied from the context, whose
 * own frozen.h does not present.
 */
struct relic_window_job {
	const relic_t			*ctx;
	const relic_import_plan_t	*plan;
	const char			*source_name;
	relic_window_t			*window;
	const relic_builder_t		*builder;
	const relic_relation_t		*store;
	relic_dom_t			*ids;
	uint64_t			 tuple_seed;
};

/*
 * relic_window_init() makes an empty window of tuples of `arity` columns.
 * relic_window_clear() forgets the window it holds, keeping the window's
 * arrays for the next and releasing the pieces' dictionaries, which are
 * sized by what each piece held, and relic_window_reset() releases
 * everything.
 *
 * The importer frames a window with a framer, and once it is framed gives
 * the window its manifest with relic_window_take_frame(), and sizes the
 * window's arrays for its length and its tuples with relic_window_size().
 * Then come the steps above, the parts
 * of each job run by relic_window_decode_part(), relic_window_find_part(),
 * relic_window_resolve_part() and relic_window_classify_part(), and the
 * steps between them by relic_window_decoded(), which says whether every
 * piece decoded, relic_window_gather(), relic_window_list() and
 * relic_window_rank().  Every function that returns an int returns 0, or
 * -1 having reported nothing, when memory ran out or a count is more than
 * the window can hold.
 */
void	 relic_window_init(relic_window_t *window, uint16_t arity);
void	 relic_window_clear(relic_window_t *window);
void	 relic_window_reset(relic_window_t *window);
void	 relic_window_take_frame(relic_window_t *window,
	    relic_framer_t *framer);
int	 relic_window_size(relic_window_t *window);
bool	 relic_window_decoded(const relic_window_t *window);
int	 relic_window_gather(relic_window_t *window);
int	 relic_window_list(relic_window_t *window);
void	 relic_window_rank(relic_window_t *window);
void	 relic_window_decode_part(void *job, size_t piece,
	    size_t worker_index);
void	 relic_window_find_part(void *job, size_t part, size_t worker_index);
void	 relic_window_resolve_part(void *job, size_t piece,
	    size_t worker_index);
void	 relic_window_classify_part(void *job, size_t stripe,
	    size_t worker_index);

/*
 * relic_framer_init() makes a framer of windows whose horizon is `horizon`
 * bytes from their first and whose pieces end at the first record end at
 * least `piece_bytes` after their start; it frames nothing until begun.
 * relic_framer_reset() releases everything it holds, leaving it as
 * relic_framer_init() made it.
 *
 * relic_framer_begin() begins framing a window of the table `plan`
 * describes, whose first byte is at `where`, which begins a record, and
 * whose first record is the table's header when `header` is set.  The
 * window's first `buffered` bytes are in memory, all it will be given,
 * and `ended` says the stream ended successfully after them.  The bytes
 * before the horizon are fed as the table's last only when the stream
 * ended strictly before the horizon, so a window's records do not depend
 * on whether its end was found at the horizon or later.  `source_name` is
 * borrowed for as long as the framing lasts.  Whatever the framer had
 * framed is forgotten.
 *
 * relic_framer_step() feeds the reader at most `most` more of those
 * bytes, the window's first now at `bytes`, and records the records they
 * complete.  Returns whether the framing goes on, which it no longer does
 * once every byte held before the horizon has been fed or the reader has
 * met the end of input, a mistake or a want of memory; the window is
 * then framed when it holds a whole record, and unframed otherwise.  The
 * reader holds none of the bytes between steps, so they may have moved
 * from one step to the next.  A step of a framer not framing does nothing
 * and returns false.  Pieces are cut by their bytes alone, so how the
 * window was stepped decides nothing about them either.
 */
void	 relic_framer_init(relic_framer_t *framer, size_t horizon,
	    size_t piece_bytes);
void	 relic_framer_reset(relic_framer_t *framer);
void	 relic_framer_begin(relic_framer_t *framer,
	    const relic_import_plan_t *plan, const char *source_name,
	    const relic_location_t *where, bool header, size_t buffered,
	    bool ended);
bool	 relic_framer_step(relic_framer_t *framer, const char *bytes,
	    size_t most);

/* Whether a field's bytes are a value of its column's type, or why not. */
enum relic_field_verdict {
	RELIC_FIELD_VALID,
	RELIC_FIELD_NOT_SYMBOL,
	RELIC_FIELD_NOT_INTEGER,
	RELIC_FIELD_OUT_OF_RANGE,
	RELIC_FIELD_TOO_LONG
};

/*
 * relic_judge_field() is the verdict on a field's `length` bytes under its
 * kept column's type, which the importer's field consumer turns into its
 * diagnostic and a decoding part into an undecoded piece, so that both
 * refuse the same fields: a symbol column's must be one symbol, an integer
 * column's one integer in range, and a symbol's or a string's no longer
 * than a value may be.  A symbol's grammar is judged before its length, so
 * a field that is no symbol is refused as that however long it is.  A
 * valid integer is stored in *integerp.  `column` must be kept.
 */
enum relic_field_verdict relic_judge_field(enum relic_column column,
	    const char *bytes, size_t length, int64_t *integerp);

#endif /* RELIC_WINDOW_H */
