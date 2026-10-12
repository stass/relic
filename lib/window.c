/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#include "window.h"

/*
 * The parts of a batch import that read a window and the context and
 * write only the window.  None of them interns a value, stages a tuple or
 * reports anything: a window they find a mistake in, or run out of memory
 * for, is declined, and the importer reads it again one field at a time,
 * which finds and reports whatever it was.
 */

static int	 decode_field(const relic_t *ctx, enum relic_column column,
		    const relic_table_event_t *event,
		    relic_window_t *window, relic_window_piece_t *piece,
		    size_t *decodedp, relic_dom_t *tokenp);
static bool	 equal_values(const relic_window_t *window,
		    const relic_pending_t *a, const relic_pending_t *b,
		    const char *b_bytes);
static uint32_t	 piece_rehash(uint32_t id, void *ctx);
static void	 piece_reset(relic_window_piece_t *piece);
static int	 piece_value(relic_window_t *window,
		    relic_window_piece_t *piece, const relic_pending_t *value,
		    const char *bytes, uint32_t hash, size_t *decodedp,
		    relic_dom_t *tokenp);
static uint32_t	 screen_rehash(uint32_t id, void *ctx);
static bool	 screened(const relic_window_t *window,
		    const relic_dom_t *tuple, uint64_t hash);
static const relic_dom_t *window_tuple(const relic_window_t *window,
		    size_t position);

void
relic_window_init(relic_window_t *window, uint16_t arity)
{
	bzero(window, sizeof(*window));
	window->arity = arity;
	relic_idtab_init(&window->screen, true);
}

void
relic_window_clear(relic_window_t *window)
{
	size_t i;

	for (i = 0; i < window->piece_count; i++)
		piece_reset(&window->pieces[i]);
	if (window->mapped)
		relic_pending_map_close(&window->map);
	relic_idtab_reset(&window->screen);

	window->input = NULL;
	window->length = 0;
	window->piece_count = 0;
	window->tuple_count = 0;
	window->fresh_count = 0;
	window->value_count = 0;
	window->ends_input = false;
	window->mapped = false;
	bzero(&window->growth, sizeof(window->growth));
}

void
relic_window_reset(relic_window_t *window)
{
	relic_window_clear(window);
	free(window->pieces);
	free(window->decoded);
	free(window->tuples);
	free(window->hashes);
	free(window->fresh);
	free(window->values);
	bzero(window, sizeof(*window));
}

static void
piece_reset(relic_window_piece_t *piece)
{
	free(piece->values);
	relic_idtab_reset(&piece->seen);
}

relic_window_piece_t *
relic_window_add_piece(relic_window_t *window)
{
	relic_window_piece_t *pieces;
	relic_window_piece_t *piece;

	pieces = relic_grow_array(window->pieces, &window->piece_capacity,
	    window->piece_count + 1, sizeof(*pieces));
	if (pieces == NULL)
		return (NULL);
	window->pieces = pieces;

	piece = &pieces[window->piece_count];
	bzero(piece, sizeof(*piece));
	relic_idtab_init(&piece->seen, true);
	window->piece_count++;

	return (piece);
}

/*
 * Every array holds at least one element, so that a window of no bytes or
 * of tuples of no columns still has an address to offset from.  A tuple's
 * position is kept in 32 bits, which a window's size bounds far below.
 */
int
relic_window_size(relic_window_t *window)
{
	char *decoded;
	relic_dom_t *tuples;
	uint64_t *hashes;
	uint32_t *fresh;
	size_t positions;
	size_t words;

	if (window->tuple_count > UINT32_MAX || (window->arity > 0 &&
	    window->tuple_count > SIZE_MAX / window->arity))
		return (-1);
	words = window->tuple_count * window->arity;
	positions = window->tuple_count > 0 ? window->tuple_count : 1;

	decoded = relic_grow_array(window->decoded, &window->decoded_capacity,
	    window->length > 0 ? window->length : 1, sizeof(*decoded));
	if (decoded == NULL)
		return (-1);
	window->decoded = decoded;

	tuples = relic_grow_array(window->tuples, &window->tuple_capacity,
	    words > 0 ? words : 1, sizeof(*tuples));
	if (tuples == NULL)
		return (-1);
	window->tuples = tuples;

	hashes = relic_grow_array(window->hashes, &window->hash_capacity,
	    positions, sizeof(*hashes));
	if (hashes == NULL)
		return (-1);
	window->hashes = hashes;

	fresh = relic_grow_array(window->fresh, &window->fresh_capacity,
	    positions, sizeof(*fresh));
	if (fresh == NULL)
		return (-1);
	window->fresh = fresh;

	return (0);
}

/*
 * The piece is read by a reader of its own, begun at the piece's first
 * byte with the location that byte has in the table, so it emits the
 * events a reader of the whole table would.  Framing found the piece's
 * records whole and well formed by the same transitions, so the only
 * reasons left to stop are a field its column refuses and memory; the
 * counts are checked rather than trusted all the same, since a tuple
 * beyond the piece's would be written past them.
 */
int
relic_window_decode_piece(const relic_t *ctx, const relic_import_plan_t *plan,
    const char *source_name, relic_window_t *window,
    relic_window_piece_t *piece)
{
	relic_table_reader_t reader;
	relic_table_event_t event;
	enum relic_table_event_kind kind;
	enum relic_column column;
	relic_dom_t *tuple;
	size_t decoded;
	size_t tuples;
	size_t kept;
	bool header;
	int status;

	assert(piece->start <= window->length &&
	    piece->length <= window->length - piece->start);

	relic_table_reader_init(&reader, plan->format, plan->column_count,
	    source_name);
	relic_table_reader_start_at(&reader, &piece->where);
	relic_table_feed(&reader, window->input + piece->start, piece->length,
	    piece->last);

	header = piece->header;
	decoded = 0;
	tuples = 0;
	kept = 0;
	status = 0;
	kind = RELIC_TABLE_MORE;
	while (status == 0) {
		kind = relic_table_next(&reader, &event);
		if (kind != RELIC_TABLE_FIELD && kind != RELIC_TABLE_RECORD)
			break;
		if (header) {
			header = kind != RELIC_TABLE_RECORD;
			continue;
		}
		if (tuples == piece->tuples) {
			status = -1;
			break;
		}
		if (kind == RELIC_TABLE_RECORD) {
			tuples++;
			kept = 0;
			continue;
		}

		column = plan->columns[event.position];
		if (column == RELIC_COLUMN_SKIP)
			continue;
		if (kept == window->arity) {
			status = -1;
			break;
		}
		tuple = window->tuples +
		    (piece->first_tuple + tuples) * window->arity;
		status = decode_field(ctx, column, &event, window, piece,
		    &decoded, &tuple[kept]);
		kept++;
	}

	relic_table_reader_reset(&reader);

	piece->valid = status == 0 && tuples == piece->tuples &&
	    (kind == RELIC_TABLE_MORE || kind == RELIC_TABLE_END);

	return (piece->valid ? 0 : -1);
}

/*
 * Give a field the token of its value under its kept column's type: the
 * checks are the ones interning the field one at a time makes, without
 * the diagnostics.  Returns 0, or -1 when the column refuses the field or
 * memory ran out.
 */
static int
decode_field(const relic_t *ctx, enum relic_column column,
    const relic_table_event_t *event, relic_window_t *window,
    relic_window_piece_t *piece, size_t *decodedp, relic_dom_t *tokenp)
{
	relic_pending_t value;
	uint32_t hash;

	bzero(&value, sizeof(value));
	if (column == RELIC_COLUMN_INTEGER) {
		if (relic_lex_integer(event->bytes, event->length,
		    &value.u.integer) != RELIC_INTEGER_VALID)
			return (-1);
		value.kind = RELIC_PENDING_INTEGER;
		hash = relic_hash_int64(ctx->values.hash_seed,
		    value.u.integer);
		return (piece_value(window, piece, &value, NULL, hash,
		    decodedp, tokenp));
	}

	if (column == RELIC_COLUMN_SYMBOL &&
	    !relic_lex_is_symbol(event->bytes, event->length))
		return (-1);
	if (event->length > RELIC_STRING_LENGTH_MAX)
		return (-1);

	value.kind = column == RELIC_COLUMN_SYMBOL ? RELIC_PENDING_SYMBOL :
	    RELIC_PENDING_TEXT;
	value.u.text.length = event->length;
	hash = relic_symbol_hash(ctx, event->bytes, event->length);

	return (piece_value(window, piece, &value, event->bytes, hash,
	    decodedp, tokenp));
}

/*
 * The token of `value`, whose bytes, when it has any, are at `bytes` and
 * whose hash is `hash`: its position in the piece's dictionary, added
 * there if it is not already.  A new symbol's or string's bytes are copied
 * to the piece's decoded bytes, *decodedp of which are used.  Room is made
 * before anything is added, so a failure leaves the dictionary as it was.
 */
static int
piece_value(relic_window_t *window, relic_window_piece_t *piece,
    const relic_pending_t *value, const char *bytes, uint32_t hash,
    size_t *decodedp, relic_dom_t *tokenp)
{
	relic_window_value_t *values;
	relic_window_value_t *entry;
	relic_idprobe_t probe;
	size_t length;
	uint32_t id;

	relic_idprobe_open(&probe, &piece->seen, hash);
	while (relic_idprobe_next(&probe, &id)) {
		if (equal_values(window, &piece->values[id - 1].value, value,
		    bytes)) {
			*tokenp = id - 1;
			return (0);
		}
	}

	length = value->kind == RELIC_PENDING_INTEGER ? 0 :
	    value->u.text.length;
	if (length > piece->length - *decodedp)
		return (-1);
	if (relic_idtab_reserve(&piece->seen, 1, piece_rehash, piece) != 0)
		return (-1);
	values = relic_grow_array(piece->values, &piece->value_capacity,
	    piece->value_count + 1, sizeof(*values));
	if (values == NULL)
		return (-1);
	piece->values = values;

	entry = &values[piece->value_count];
	entry->value = *value;
	entry->hash = hash;
	if (value->kind != RELIC_PENDING_INTEGER) {
		entry->value.u.text.offset = piece->start + *decodedp;
		if (length > 0)
			bcopy(bytes, window->decoded + piece->start + *decodedp,
			    length);
		*decodedp += length;
	}

	piece->value_count++;
	relic_idtab_place(&piece->seen, hash, (uint32_t)piece->value_count);
	*tokenp = (relic_dom_t)(piece->value_count - 1);

	return (0);
}

/*
 * Whether `a`, a value of a dictionary, is `b`, whose bytes, when it has
 * any, are at `b_bytes`.
 */
static bool
equal_values(const relic_window_t *window, const relic_pending_t *a,
    const relic_pending_t *b, const char *b_bytes)
{
	if (a->kind != b->kind)
		return (false);
	if (a->kind == RELIC_PENDING_INTEGER)
		return (a->u.integer == b->u.integer);

	return (a->u.text.length == b->u.text.length &&
	    (a->u.text.length == 0 || bcmp(window->decoded + a->u.text.offset,
	    b_bytes, a->u.text.length) == 0));
}

static uint32_t
piece_rehash(uint32_t id, void *ctx)
{
	const relic_window_piece_t *piece;

	piece = ctx;

	return (piece->values[id - 1].hash);
}

/*
 * The map is opened with room for the window's values, of which there is
 * always at least one when a tuple has a column.
 */
int
relic_window_gather(relic_window_t *window)
{
	const relic_window_piece_t *piece;
	relic_pending_t *values;
	size_t count;
	size_t i;
	size_t v;

	count = 0;
	for (i = 0; i < window->piece_count; i++)
		count += window->pieces[i].value_count;
	if (count == 0)
		return (0);
	if (count > RELIC_DOM_INDEX_MASK)
		return (-1);

	values = relic_grow_array(window->values, &window->value_capacity,
	    count, sizeof(*values));
	if (values == NULL)
		return (-1);
	window->values = values;
	window->value_count = 0;
	for (i = 0; i < window->piece_count; i++) {
		piece = &window->pieces[i];
		for (v = 0; v < piece->value_count; v++)
			window->values[window->value_count++] =
			    piece->values[v].value;
	}

	if (relic_pending_map_open(&window->map, count) != 0)
		return (-1);
	window->mapped = true;

	return (0);
}

/*
 * A piece's tokens index its own dictionary, which starts in the map
 * where the pieces before it end.
 */
void
relic_window_resolve(const relic_t *ctx, relic_window_t *window)
{
	const relic_window_piece_t *piece;
	relic_dom_t *words;
	size_t first;
	size_t i;
	size_t w;

	first = 0;
	for (i = 0; i < window->piece_count; i++) {
		piece = &window->pieces[i];
		words = window->tuples + piece->first_tuple * window->arity;
		for (w = 0; w < piece->tuples * window->arity; w++) {
			assert(words[w] < piece->value_count);
			words[w] = window->map.ids[first + words[w]];
		}
		first += piece->value_count;
	}

	for (i = 0; i < window->tuple_count; i++)
		window->hashes[i] = relic_tuple_hash(ctx->hash_seed,
		    window_tuple(window, i), window->arity);
}

/*
 * A tuple is fresh when neither the builder nor the base store holds it
 * and no earlier tuple of the window is it, which the screen, holding the
 * fresh tuples found so far, answers.  Fresh tuples are listed in input
 * order, which is the order they are to be staged in.
 */
int
relic_window_classify(relic_window_t *window,
    const relic_builder_t *builder, const relic_relation_t *store)
{
	const relic_dom_t *tuple;
	uint64_t hash;
	size_t i;

	for (i = 0; i < window->tuple_count; i++) {
		tuple = window_tuple(window, i);
		hash = window->hashes[i];
		if ((builder != NULL &&
		    relic_builder_contains_hashed(builder, tuple, hash)) ||
		    (store != NULL &&
		    relic_relation_contains_hashed(store, tuple, hash)) ||
		    screened(window, tuple, hash))
			continue;

		if (relic_idtab_reserve(&window->screen, 1, screen_rehash,
		    window) != 0)
			return (-1);
		window->fresh[window->fresh_count] = (uint32_t)i;
		window->fresh_count++;
		relic_idtab_place(&window->screen, relic_hash_low(hash),
		    (uint32_t)window->fresh_count);
		window->growth.by_stripe[relic_hash_stripe(hash)]++;
	}

	return (0);
}

static bool
screened(const relic_window_t *window, const relic_dom_t *tuple,
    uint64_t hash)
{
	relic_idprobe_t probe;
	uint32_t id;

	relic_idprobe_open(&probe, &window->screen, relic_hash_low(hash));
	while (relic_idprobe_next(&probe, &id)) {
		if (relic_tuple_equal(window_tuple(window,
		    window->fresh[id - 1]), tuple, window->arity))
			return (true);
	}

	return (false);
}

static uint32_t
screen_rehash(uint32_t id, void *ctx)
{
	const relic_window_t *window;

	window = ctx;

	return (relic_hash_low(window->hashes[window->fresh[id - 1]]));
}

static const relic_dom_t *
window_tuple(const relic_window_t *window, size_t position)
{
	return (window->tuples + position * window->arity);
}
