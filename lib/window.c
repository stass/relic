/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * A batch import's analysis of a window reads the context, the builder and
 * the base store only as frozen.h presents them, which is what keeps the
 * parts the pool runs at once from writing what another reads.  The
 * headers that define a context, a relation and an index refuse to be
 * included where this is defined.
 */
#define	RELIC_FROZEN_ONLY

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#include "cdefs.h"
#include "window.h"

/*
 * A window a part finds a mistake in, or runs out of memory for, is
 * declined, and the importer reads it again one field at a time, which
 * finds and reports whatever it was.
 */

_Static_assert(RELIC_WINDOW_VALUE_PART_BITS > 0 &&
    RELIC_WINDOW_VALUE_PART_BITS < 32,
    "a value's part is chosen by the top bits of its 32-bit hash");

static void	 copy_dictionaries(relic_window_t *window);
static int	 decode_field(const relic_t *ctx, enum relic_column column,
		    const relic_table_event_t *event,
		    relic_window_t *window, relic_window_piece_t *piece,
		    size_t *decodedp, relic_dom_t *tokenp);
static void	 divide_values(relic_window_t *window);
static bool	 equal_values(const relic_window_t *window,
		    const relic_pending_t *a, const relic_pending_t *b,
		    const char *b_bytes);
static void	 piece_decode(const relic_window_job_t *job,
		    relic_window_piece_t *piece);
static uint32_t	 piece_rehash(uint32_t id, void *ctx);
static void	 piece_reset(relic_window_piece_t *piece);
static int	 piece_value(relic_window_t *window,
		    relic_window_piece_t *piece, const relic_pending_t *value,
		    const char *bytes, uint32_t hash, size_t *decodedp,
		    relic_dom_t *tokenp);
static size_t	 place_dictionaries(relic_window_t *window);
static bool	 screened(const relic_window_t *window, size_t stripe,
		    const relic_dom_t *tuple, uint64_t hash);
static int	 size_screens(relic_window_t *window);
static int	 size_value_slots(relic_window_t *window);
static int	 size_values(relic_window_t *window, size_t count);
static size_t	 slot_aligned(size_t offset);
static size_t	 value_part(uint32_t hash);
static const relic_dom_t *window_tuple(const relic_window_t *window,
		    size_t position);

void
relic_window_init(relic_window_t *window, uint16_t arity)
{
	bzero(window, sizeof(*window));
	window->arity = arity;
}

void
relic_window_clear(relic_window_t *window)
{
	size_t i;

	for (i = 0; i < window->piece_count; i++)
		piece_reset(&window->pieces[i]);

	window->input = NULL;
	window->length = 0;
	window->piece_count = 0;
	window->value_count = 0;
	window->tuple_count = 0;
	window->fresh_count = 0;
	window->ends_input = false;
}

void
relic_window_reset(relic_window_t *window)
{
	relic_window_clear(window);
	free(window->pieces);
	free(window->decoded);
	free(window->values);
	free(window->value_hashes);
	free(window->value_members);
	free(window->value_slots);
	free(window->tuples);
	free(window->hashes);
	free(window->stripes);
	free(window->stripe_list);
	free(window->ranks);
	free(window->screen_memory);
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
	uint8_t *stripes;
	uint32_t *stripe_list;
	uint32_t *ranks;
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

	stripes = relic_grow_array(window->stripes, &window->stripe_capacity,
	    positions, sizeof(*stripes));
	if (stripes == NULL)
		return (-1);
	window->stripes = stripes;

	stripe_list = relic_grow_array(window->stripe_list,
	    &window->stripe_list_capacity, positions, sizeof(*stripe_list));
	if (stripe_list == NULL)
		return (-1);
	window->stripe_list = stripe_list;

	ranks = relic_grow_array(window->ranks, &window->rank_capacity,
	    positions, sizeof(*ranks));
	if (ranks == NULL)
		return (-1);
	window->ranks = ranks;

	return (0);
}

void
relic_window_decode_part(void *argument, size_t piece,
    size_t worker_index RELIC_UNUSED)
{
	const relic_window_job_t *job;

	job = argument;
	piece_decode(job, &job->window->pieces[piece]);
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
static void
piece_decode(const relic_window_job_t *job, relic_window_piece_t *piece)
{
	relic_table_reader_t reader;
	relic_table_event_t event;
	enum relic_table_event_kind kind;
	enum relic_column column;
	relic_window_t *window;
	relic_dom_t *tuple;
	size_t decoded;
	size_t tuples;
	size_t kept;
	bool header;
	int status;

	window = job->window;
	assert(piece->start <= window->length &&
	    piece->length <= window->length - piece->start);

	relic_table_reader_init(&reader, job->plan->format,
	    job->plan->column_count, job->source_name);
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

		column = job->plan->columns[event.position];
		if (column == RELIC_COLUMN_SKIP)
			continue;
		if (kept == window->arity) {
			status = -1;
			break;
		}
		tuple = window->tuples +
		    (piece->first_tuple + tuples) * window->arity;
		status = decode_field(job->ctx, column, &event, window, piece,
		    &decoded, &tuple[kept]);
		kept++;
	}

	relic_table_reader_reset(&reader);

	piece->valid = status == 0 && tuples == piece->tuples &&
	    (kind == RELIC_TABLE_MORE || kind == RELIC_TABLE_END);
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

	bzero(&value, sizeof(value));
	if (column == RELIC_COLUMN_INTEGER) {
		if (relic_lex_integer(event->bytes, event->length,
		    &value.u.integer) != RELIC_INTEGER_VALID)
			return (-1);
		value.kind = RELIC_PENDING_INTEGER;
		return (piece_value(window, piece, &value, NULL,
		    relic_pending_hash(ctx, &value, NULL), decodedp,
		    tokenp));
	}

	if (column == RELIC_COLUMN_SYMBOL &&
	    !relic_lex_is_symbol(event->bytes, event->length))
		return (-1);
	if (event->length > RELIC_STRING_LENGTH_MAX)
		return (-1);

	value.kind = column == RELIC_COLUMN_SYMBOL ? RELIC_PENDING_SYMBOL :
	    RELIC_PENDING_TEXT;
	value.u.text.length = event->length;

	return (piece_value(window, piece, &value, event->bytes,
	    relic_pending_hash(ctx, &value, event->bytes), decodedp, tokenp));
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

bool
relic_window_decoded(const relic_window_t *window)
{
	size_t i;

	for (i = 0; i < window->piece_count; i++) {
		if (!window->pieces[i].valid)
			return (false);
	}

	return (true);
}

/*
 * Gathering is four steps: each piece's dictionary is given its place
 * among the window's values, the dictionaries are copied there in piece
 * order, the values are divided among the value parts, and the tables the
 * parts find the values' firsts in are laid out.  A window whose tuples
 * have columns has at least one value.
 */
int
relic_window_gather(relic_window_t *window)
{
	size_t count;

	count = place_dictionaries(window);
	if (count == 0)
		return (0);
	if (count > RELIC_DOM_INDEX_MASK || size_values(window, count) != 0)
		return (-1);
	window->value_count = count;

	copy_dictionaries(window);
	divide_values(window);

	return (size_value_slots(window));
}

/*
 * Give each piece's dictionary the position its first value takes among
 * the window's, and return how many values the window's pieces hold.
 */
static size_t
place_dictionaries(relic_window_t *window)
{
	size_t count;
	size_t i;

	count = 0;
	for (i = 0; i < window->piece_count; i++) {
		window->pieces[i].first_value = count;
		count += window->pieces[i].value_count;
	}

	return (count);
}

static int
size_values(relic_window_t *window, size_t count)
{
	relic_pending_t *values;
	uint32_t *hashes;
	uint32_t *members;

	values = relic_grow_array(window->values, &window->value_capacity,
	    count, sizeof(*values));
	if (values == NULL)
		return (-1);
	window->values = values;
	hashes = relic_grow_array(window->value_hashes,
	    &window->value_hash_capacity, count, sizeof(*hashes));
	if (hashes == NULL)
		return (-1);
	window->value_hashes = hashes;
	members = relic_grow_array(window->value_members,
	    &window->value_member_capacity, count, sizeof(*members));
	if (members == NULL)
		return (-1);
	window->value_members = members;

	return (0);
}

static void
copy_dictionaries(relic_window_t *window)
{
	const relic_window_piece_t *piece;
	size_t i;
	size_t v;

	for (i = 0; i < window->piece_count; i++) {
		piece = &window->pieces[i];
		for (v = 0; v < piece->value_count; v++) {
			window->values[piece->first_value + v] =
			    piece->values[v].value;
			window->value_hashes[piece->first_value + v] =
			    piece->values[v].hash;
		}
	}
}

/*
 * List the window's values by the part each falls in, those of part p
 * from `value_start[p]`, with a counting sort, so that each part's are in
 * batch order, as relic_pending_find() needs them.
 */
static void
divide_values(relic_window_t *window)
{
	size_t cursor[RELIC_WINDOW_VALUE_PARTS];
	size_t part;
	size_t i;

	bzero(window->value_start, sizeof(window->value_start));
	for (i = 0; i < window->value_count; i++)
		window->value_start[value_part(window->value_hashes[i]) + 1]++;
	for (part = 0; part < RELIC_WINDOW_VALUE_PARTS; part++) {
		window->value_start[part + 1] += window->value_start[part];
		cursor[part] = window->value_start[part];
	}
	for (i = 0; i < window->value_count; i++)
		window->value_members[cursor[value_part(
		    window->value_hashes[i])]++] = (uint32_t)i;
}

/*
 * Lay out each value part's table of its values in `value_slots`, with the
 * slots relic_pending_find() needs for them.  A window's values are few
 * enough that no sum here can overflow: each part's slots are fewer than
 * four times its values.
 */
static int
size_value_slots(relic_window_t *window)
{
	uint32_t *slots;
	size_t members;
	size_t total;
	size_t part;

	total = 0;
	for (part = 0; part < RELIC_WINDOW_VALUE_PARTS; part++) {
		members = window->value_start[part + 1] -
		    window->value_start[part];
		window->value_slot_start[part] = total;
		window->value_slot_count[part] = members > 0 ?
		    relic_pending_slots(members) : 0;
		total += window->value_slot_count[part];
	}

	slots = relic_grow_array(window->value_slots,
	    &window->value_slot_capacity, total, sizeof(*slots));
	if (slots == NULL)
		return (-1);
	window->value_slots = slots;

	return (0);
}

/*
 * The part a value of hash `hash` is found by: its hash's top bits, which
 * leave the low bits that choose its slot in the part's table to spread
 * the part's values.
 */
static size_t
value_part(uint32_t hash)
{
	return ((size_t)(hash >> (32 - RELIC_WINDOW_VALUE_PART_BITS)));
}

void
relic_window_find_part(void *argument, size_t part,
    size_t worker_index RELIC_UNUSED)
{
	const relic_window_job_t *job;
	const relic_window_t *window;
	relic_pending_batch_t batch;
	uint32_t *slots;
	size_t first;
	size_t members;

	job = argument;
	window = job->window;
	first = window->value_start[part];
	members = window->value_start[part + 1] - first;
	if (members == 0)
		return;

	batch.values = window->values;
	batch.hashes = window->value_hashes;
	batch.bytes = window->decoded;
	batch.count = window->value_count;
	slots = window->value_slots + window->value_slot_start[part];
	bzero(slots, window->value_slot_count[part] * sizeof(*slots));
	relic_pending_find(job->ctx, &batch, window->value_members + first,
	    members, slots, window->value_slot_count[part], job->ids);
}

/*
 * A piece's tokens index its own dictionary, which starts among the
 * window's values at the piece's first value.
 */
void
relic_window_resolve_part(void *argument, size_t piece_index,
    size_t worker_index RELIC_UNUSED)
{
	const relic_window_job_t *job;
	const relic_window_piece_t *piece;
	relic_window_t *window;
	relic_dom_t *words;
	uint64_t hash;
	size_t end;
	size_t i;
	size_t w;

	job = argument;
	window = job->window;
	piece = &window->pieces[piece_index];
	words = window->tuples + piece->first_tuple * window->arity;
	for (w = 0; w < piece->tuples * window->arity; w++) {
		assert(words[w] < piece->value_count);
		words[w] = job->ids[piece->first_value + words[w]];
	}

	end = piece->first_tuple + piece->tuples;
	for (i = piece->first_tuple; i < end; i++) {
		hash = relic_tuple_hash(job->tuple_seed,
		    window_tuple(window, i), window->arity);
		window->hashes[i] = hash;
		window->stripes[i] = (uint8_t)relic_hash_stripe(hash);
	}
}

/*
 * List the resolved tuples by stripe and lay out each stripe's screen in
 * `screen_memory`, which its classify part makes it in.
 */
int
relic_window_list(relic_window_t *window)
{
	relic_list_by_stripe(window->stripes, window->tuple_count,
	    window->stripe_list, window->stripe_start);

	return (size_screens(window));
}

/*
 * Each stripe's screen is given room, before its classify part runs, for
 * every tuple of the stripe, as many as the part can place in it, so that
 * the part never grows it: the parts allocate nothing, and place without
 * reserving.  The screens share one block, each at an offset aligned for
 * its slots.
 */
static int
size_screens(relic_window_t *window)
{
	char *memory;
	size_t tuples;
	size_t bytes;
	size_t total;
	size_t s;

	total = 0;
	for (s = 0; s < RELIC_STRIPES; s++) {
		tuples = window->stripe_start[s + 1] - window->stripe_start[s];
		bytes = relic_idtab_bytes_for(true, tuples);
		if (bytes == SIZE_MAX)
			return (-1);
		total = slot_aligned(total);
		window->screen_start[s] = total;
		total = relic_saturating_sum(total, bytes);
	}
	if (total == SIZE_MAX)
		return (-1);

	memory = relic_grow_array(window->screen_memory,
	    &window->screen_capacity, total > 0 ? total : 1, sizeof(*memory));
	if (memory == NULL)
		return (-1);
	window->screen_memory = memory;

	return (0);
}

/*
 * A tuple is new when neither the builder nor the base store holds it and
 * no earlier tuple of the window is it.  Equal tuples share a stripe, so
 * the earlier tuples it could be are the stripe's, which the stripe's
 * screen, holding the new ones found so far, answers for.
 */
void
relic_window_classify_part(void *argument, size_t stripe,
    size_t worker_index RELIC_UNUSED)
{
	const relic_window_job_t *job;
	const relic_dom_t *tuple;
	relic_window_t *window;
	uint64_t hash;
	uint32_t position;
	size_t end;
	size_t i;

	job = argument;
	window = job->window;
	window->fresh_by_stripe[stripe] = 0;
	end = window->stripe_start[stripe + 1];
	if (end == window->stripe_start[stripe])
		return;

	relic_idtab_init_given(&window->screens[stripe], true,
	    end - window->stripe_start[stripe],
	    window->screen_memory + window->screen_start[stripe]);
	for (i = window->stripe_start[stripe]; i < end; i++) {
		position = window->stripe_list[i];
		tuple = window_tuple(window, position);
		hash = window->hashes[position];
		window->ranks[position] = 0;
		if ((job->builder != NULL &&
		    relic_builder_contains_hashed(job->builder, tuple, hash)) ||
		    (job->store != NULL &&
		    relic_relation_contains_hashed(job->store, tuple, hash)) ||
		    screened(window, stripe, tuple, hash))
			continue;

		relic_idtab_place(&window->screens[stripe],
		    relic_hash_low(hash), position + 1);
		window->ranks[position] = 1;
		window->fresh_by_stripe[stripe]++;
	}
}

/*
 * The first offset at or after `offset` aligned for a table's slots, or
 * SIZE_MAX when there is none.
 */
static size_t
slot_aligned(size_t offset)
{
	size_t misalignment;

	misalignment = offset % sizeof(uint32_t);
	if (misalignment == 0)
		return (offset);

	return (relic_saturating_sum(offset, sizeof(uint32_t) - misalignment));
}

static bool
screened(const relic_window_t *window, size_t stripe,
    const relic_dom_t *tuple, uint64_t hash)
{
	relic_idprobe_t probe;
	uint32_t id;

	relic_idprobe_open(&probe, &window->screens[stripe],
	    relic_hash_low(hash));
	while (relic_idprobe_next(&probe, &id)) {
		if (relic_tuple_equal(window_tuple(window, id - 1), tuple,
		    window->arity))
			return (true);
	}

	return (false);
}

/*
 * Number the new tuples in input order, the order they are to be staged
 * in, over the marks the classify parts left.
 */
void
relic_window_rank(relic_window_t *window)
{
	uint32_t rank;
	size_t i;

	rank = 0;
	for (i = 0; i < window->tuple_count; i++) {
		if (window->ranks[i] != 0)
			window->ranks[i] = ++rank;
	}
	window->fresh_count = rank;
}

static const relic_dom_t *
window_tuple(const relic_window_t *window, size_t position)
{
	return (window->tuples + position * window->arity);
}
