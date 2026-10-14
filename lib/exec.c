/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * The executor sees the store only as frozen.h presents it, which is what
 * keeps it from writing what it reads.  The headers that define a context, a
 * relation and an index refuse to be included where this is defined,
 * wherever in the file the include is.
 */
#define	RELIC_FROZEN_ONLY

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cdefs.h"
#include "exec.h"
#include "fold.h"

/*
 * The executor: running one plan, an array of operations ending in EMIT.
 *
 * The join a plan describes is a nest of loops, one per generator, and it is
 * run with one loop and an explicit level rather than a C call per level, so
 * a rule of any length uses the same stack as a rule of one literal.
 *
 * Descending means the operation at the current level produced a binding and
 * the next one should start afresh against it.  Backtracking means an
 * operation has nothing more, so the one before it is asked for its next
 * binding.  EMIT adds the head tuple to the scratch's batch and then reports
 * that there is nothing below it, so the level never passes the end of the
 * plan and the loop needs no case for having got there.
 *
 * When the EMIT fills the batch the loop stops and returns, recording in the
 * scratch the level and the resume flag it would have gone on with, so that
 * the next call continues the join exactly where it left off.  Everything
 * the join depends on -- the frame, the cursors, the views -- is in the
 * scratch or in relations nobody writes while the plan runs, so stopping
 * and continuing changes nothing it derives.
 *
 * An expression is a postfix program run on a stack of cells, each holding a
 * domain identifier, a computed integer or a computed string.  An operator
 * or a function reads a cell as the type it wants, decoding an identifier
 * through the integer table or the pool, and a cell of another type has no
 * value of that type: the run has no value, and the operation running it
 * rejects the binding.  A result that is not a value -- an integer that
 * does not fit, a division by zero, a string too long -- ends the run with
 * an error instead, recorded in the scratch, and the plan ends as though
 * every generator were exhausted.
 *
 * A computed string is kept in the scratch's text stack, and a cell names
 * it by offset and length.  Each operation builds the strings its programs
 * compute above the string of the last text slot bound before it, so a
 * string a slot names is overwritten only when the operation that bound the
 * slot runs again and binds it anew (plan.h).  Nothing is saved on descent
 * or restored on backtracking, for the reason nothing in the frame is.
 *
 * The context and every relation are read-only here, as frozen.h makes
 * them, and only the scratch is written.  The views of a plan are resolved
 * when it starts, and no view reaches past the end its relation had then,
 * so whatever the caller does with a batch when the plan pauses -- insert
 * it into the relation the plan reads, even -- lies past every view and
 * changes nothing the plan reads.
 */

typedef struct relic_cell relic_cell_t;
typedef struct relic_fold relic_fold_t;

/*
 * One cell of the stack a postfix program runs on: a domain identifier
 * pushed from a constant or a domain slot, an integer pushed from a number
 * slot or computed, or a string pushed from a text slot or computed, named
 * in the text stack.
 */
enum relic_cell_kind {
	RELIC_CELL_DOM,
	RELIC_CELL_NUMBER,
	RELIC_CELL_TEXT
};

struct relic_cell {
	union {
		relic_dom_t	dom;
		int64_t		number;
		relic_text_t	text;
	}			u;
	enum relic_cell_kind	kind;
};

/*
 * One AGGREGATE's fold of a group's rows: the rows it has folded, the
 * function's state over them (fold.h), and for a min or a max the extreme
 * so far, its identifier and the view of its value the state compares
 * with, meaningful once `rows` is not zero.
 */
struct relic_fold {
	relic_fold_state_t	state;
	relic_value_view_t	extreme_view;
	uint64_t		rows;
	relic_dom_t		extreme;
};

/*
 * How a postfix program's run ended: with a value, with none because an
 * operand was not of the type its operator or function wants, or with a
 * failure recorded.
 */
enum relic_run_end {
	RELIC_RUN_VALUE,
	RELIC_RUN_NO_VALUE,
	RELIC_RUN_FAILED
};

/*
 * What an EMIT did with the head tuple: wrote it into the batch, found the
 * batch's pending bytes too full for its strings, so that it must be asked
 * again once the batch is taken, or recorded a failure to make room.
 */
enum relic_emit_end {
	RELIC_EMIT_WRITTEN,
	RELIC_EMIT_FULL,
	RELIC_EMIT_FAILED
};

static bool	 buffer_make_room(relic_scratch_t *scratch,
		    relic_text_buffer_t *buffer, size_t more,
		    const relic_location_t *where);
static void	 build_tuple(const relic_scratch_t *scratch,
		    const relic_op_atom_t *atom, relic_dom_t *tuple);
static enum relic_run_end call_cat(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_location_t *where,
		    const relic_cell_t *args, relic_cell_t *resultp);
static enum relic_run_end call_function(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_location_t *where,
		    enum relic_function fn, const relic_pattern_t *pattern,
		    const relic_cell_t *args, relic_cell_t *resultp);
static enum relic_run_end call_match(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_location_t *where,
		    const relic_pattern_t *compiled, const relic_cell_t *args,
		    relic_cell_t *resultp);
static enum relic_run_end call_str(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_location_t *where,
		    relic_cell_t value, relic_cell_t *resultp);
static enum relic_run_end call_substr(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_location_t *where,
		    const relic_cell_t *args, bool counted,
		    relic_cell_t *resultp);
static bool	 cell_integer(const relic_exec_env_t *env, relic_cell_t cell,
		    int64_t *integerp);
static bool	 cell_is_kind(const relic_exec_env_t *env, relic_cell_t cell,
		    enum relic_value_kind kind);
static bool	 cell_string(const relic_scratch_t *scratch,
		    const relic_exec_env_t *env, relic_cell_t cell,
		    const char **bytesp, size_t *lengthp);
static relic_dom_t cell_type(relic_cell_t cell);
static bool	 charge_growth(relic_scratch_t *scratch, size_t old_bytes,
		    size_t new_bytes, const relic_location_t *where);
static void	 charge_unresumable_read(relic_scratch_t *scratch);
static bool	 compile_pattern(relic_scratch_t *scratch,
		    const relic_location_t *where, const char *bytes,
		    size_t length, relic_pattern_t *patternp);
static enum relic_order cells_order(const relic_scratch_t *scratch,
		    const relic_exec_env_t *env, relic_cell_t left,
		    relic_cell_t right);
static void	 count_visits(relic_scratch_t *scratch);
static relic_cell_t dom_cell(relic_dom_t dom);
static enum relic_exec_status end_run(relic_scratch_t *scratch);
static relic_dom_t emit_column(relic_scratch_t *scratch,
		    const relic_plan_term_t *term);
static enum relic_emit_end emit_head(relic_scratch_t *scratch,
		    const relic_op_emit_t *emit);
static size_t	 emit_pending_bytes(const relic_scratch_t *scratch,
		    const relic_op_emit_t *emit);
static void	 fold_group(relic_scratch_t *scratch,
		    const relic_exec_env_t *env,
		    const relic_op_aggregate_t *aggregate,
		    const relic_tuple_range_t *view, relic_fold_t *fold);
static bool	 fold_row(const relic_exec_env_t *env,
		    const relic_op_aggregate_t *aggregate, relic_fold_t *fold,
		    const relic_dom_t *tuple);
static bool	 fold_value(relic_scratch_t *scratch,
		    const relic_exec_env_t *env,
		    const relic_op_aggregate_t *aggregate,
		    const relic_fold_t *fold, relic_cell_t *valuep);
static void	 gather_fold_key(const relic_scratch_t *scratch,
		    const relic_op_aggregate_t *aggregate, relic_dom_t *probe);
static size_t	 grown_capacity(size_t capacity, size_t needed, size_t unit);
static bool	 key_matches(const relic_op_aggregate_t *aggregate,
		    const relic_dom_t *probe, const relic_dom_t *tuple);
static const relic_index_t *lookup_index(const relic_op_atom_t *atom);
static relic_tuple_id_t lookup_start(const relic_scratch_t *scratch,
		    const relic_op_atom_t *atom, const relic_index_t *index,
		    relic_tuple_id_t first);
static relic_cell_t number_cell(int64_t number);
static const char *operator_spelling(const relic_step_t *step);
static bool	 order_satisfies(enum relic_compare_op compare,
		    enum relic_order order);
static size_t	 peak(size_t held, size_t capacity);
static bool	 probe_index(relic_scratch_t *scratch,
		    const relic_op_atom_t *atom,
		    const relic_tuple_range_t *view);
static bool	 probe_scan(relic_scratch_t *scratch,
		    const relic_op_atom_t *atom,
		    const relic_tuple_range_t *view);
static bool	 program_make_room(relic_scratch_t *scratch, size_t count,
		    const relic_location_t *where);
static size_t	 program_room_bytes(size_t capacity);
static void	 record_aggregate_note(relic_scratch_t *scratch,
		    const relic_exec_env_t *env,
		    const relic_op_aggregate_t *aggregate,
		    enum relic_note_kind kind);
static void	 record_error(relic_scratch_t *scratch,
		    const relic_location_t *where,
		    enum relic_exec_failure failure);
static void	 record_pattern_error(relic_scratch_t *scratch,
		    const relic_location_t *where,
		    enum relic_regex_failure pattern);
static void	 record_operand_note(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_location_t *where,
		    const char *spelling, relic_cell_t operand);
static void	 record_ordering_note(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_location_t *where,
		    relic_cell_t left, relic_cell_t right);
static void	 reach_driver(relic_scratch_t *scratch, bool sliced);
static void	 resolve_views(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_plan_t *plan);
static enum relic_exec_status run_join(relic_scratch_t *scratch);
static enum relic_run_end run_postfix(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_postfix_t *postfix,
		    const relic_location_t *where, relic_cell_t *valuep);
static enum relic_exec_status run_tests(relic_scratch_t *scratch);
static void	 start_run(relic_scratch_t *scratch,
		    const relic_exec_env_t *env, const relic_plan_t *plan,
		    bool prefix_only, const relic_tuple_range_t *slice);
static bool	 step(relic_scratch_t *scratch, const relic_exec_env_t *env,
		    const relic_op_t *op, relic_op_state_t *state, bool resume);
static bool	 step_aggregate(relic_scratch_t *scratch,
		    const relic_exec_env_t *env,
		    const relic_op_aggregate_t *aggregate,
		    const relic_tuple_range_t *view);
static bool	 step_assign(relic_scratch_t *scratch,
		    const relic_exec_env_t *env,
		    const relic_op_assign_t *assign, relic_op_state_t *state);
static bool	 step_filter(relic_scratch_t *scratch,
		    const relic_exec_env_t *env,
		    const relic_op_filter_t *filter);
static bool	 step_lookup(relic_scratch_t *scratch,
		    const relic_op_atom_t *atom, relic_op_state_t *state,
		    bool resume);
static bool	 step_member(const relic_scratch_t *scratch,
		    const relic_op_atom_t *atom,
		    const relic_tuple_range_t *view);
static bool	 step_probe(relic_scratch_t *scratch,
		    const relic_op_atom_t *atom,
		    const relic_tuple_range_t *view);
static bool	 step_scan(relic_scratch_t *scratch,
		    const relic_op_atom_t *atom, relic_op_state_t *state,
		    bool resume);
static relic_dom_t term_value(const relic_scratch_t *scratch,
		    const relic_plan_term_t *term);
static bool	 test_holds(relic_scratch_t *scratch, const relic_op_t *op,
		    const relic_op_state_t *state);
static size_t	 text_base_top(const relic_scratch_t *scratch,
		    size_t text_base);
static relic_cell_t text_cell(size_t offset, size_t length);
static relic_cell_t text_push(relic_scratch_t *scratch, size_t length);
static uint64_t	 tuples_read(const relic_scratch_t *scratch);
static bool	 tuple_matches(relic_scratch_t *scratch,
		    const relic_op_atom_t *atom, const relic_dom_t *tuple);
static bool	 work_make_room(relic_scratch_t *scratch, size_t count,
		    const relic_location_t *where);
static size_t	 work_space_bytes(size_t capacity);
static enum relic_exec_status yield_after_emit(relic_scratch_t *scratch,
		    size_t level);
static enum relic_exec_status yield_before_emit(relic_scratch_t *scratch,
		    size_t level);

void
relic_scratch_init(relic_scratch_t *scratch, relic_op_state_t *states,
    size_t op_room, relic_dom_t *batch, relic_pending_t *pending,
    uint16_t arity_room)
{
	bzero(scratch, sizeof(*scratch));
	scratch->states = states;
	scratch->op_room = op_room;
	scratch->batch = batch;
	scratch->pending = pending;
	scratch->arity_room = arity_room;
	scratch->finished = true;
}

/*
 * The two buffers are made together and freed together, so a scratch holds
 * both or neither.  The match space begins empty, since most rounds match
 * nothing, and is freed with them.
 */
int
relic_scratch_begin_texts(relic_scratch_t *scratch, relic_account_t *account,
    enum relic_exec_failure *failurep)
{
	const size_t bytes = RELIC_TEXT_STACK_INITIAL + RELIC_PENDING_BYTES;

	assert(scratch->text_stack.bytes == NULL);
	assert(scratch->match.insts == NULL && scratch->match.work.words == NULL);

	*failurep = RELIC_FAIL_LIMIT;
	if (!relic_account_charge_round(account, bytes))
		return (-1);

	*failurep = RELIC_FAIL_NO_MEMORY;

	scratch->text_stack.bytes = malloc(RELIC_TEXT_STACK_INITIAL);
	scratch->pending_bytes.bytes = malloc(RELIC_PENDING_BYTES);
	if (scratch->text_stack.bytes == NULL ||
	    scratch->pending_bytes.bytes == NULL) {
		free(scratch->text_stack.bytes);
		free(scratch->pending_bytes.bytes);
		scratch->text_stack.bytes = NULL;
		scratch->pending_bytes.bytes = NULL;
		relic_account_release_round(account, bytes);
		return (-1);
	}

	scratch->account = account;
	scratch->text_stack.used = 0;
	scratch->text_stack.capacity = RELIC_TEXT_STACK_INITIAL;
	scratch->pending_bytes.used = 0;
	scratch->pending_bytes.capacity = RELIC_PENDING_BYTES;

	return (0);
}

void
relic_scratch_end_texts(relic_scratch_t *scratch)
{
	if (scratch->text_stack.bytes == NULL)
		return;

	relic_account_release_round(scratch->account,
	    scratch->text_stack.capacity + scratch->pending_bytes.capacity +
	    program_room_bytes(scratch->match.capacity) +
	    work_space_bytes(scratch->match.work.capacity));
	free(scratch->text_stack.bytes);
	free(scratch->pending_bytes.bytes);
	free(scratch->match.insts);
	free(scratch->match.work.words);
	bzero(&scratch->text_stack, sizeof(scratch->text_stack));
	bzero(&scratch->pending_bytes, sizeof(scratch->pending_bytes));
	bzero(&scratch->match, sizeof(scratch->match));
	scratch->account = NULL;
}

static size_t
peak(size_t held, size_t capacity)
{
	return (capacity > held ? capacity : held);
}

void
relic_text_capacity_raise(relic_text_capacity_t *peaks,
    const relic_scratch_t *scratch)
{
	peaks->text_stack = peak(peaks->text_stack,
	    scratch->text_stack.capacity);
	peaks->pending_bytes = peak(peaks->pending_bytes,
	    scratch->pending_bytes.capacity);
	peaks->program = peak(peaks->program,
	    program_room_bytes(scratch->match.capacity));
	peaks->work = peak(peaks->work,
	    work_space_bytes(scratch->match.work.capacity));
}

size_t
relic_text_capacity_bytes(const relic_text_capacity_t *peaks)
{
	return (peaks->text_stack + peaks->pending_bytes + peaks->program +
	    peaks->work);
}

/*
 * The bytes of room for a program of `capacity` instructions, and of a
 * work space for one.  Neither product can wrap for a capacity the space
 * has, since each was checked when the space grew to it.
 */
static size_t
program_room_bytes(size_t capacity)
{
	return (capacity * sizeof(relic_regex_inst_t));
}

static size_t
work_space_bytes(size_t capacity)
{
	return (capacity * RELIC_REGEX_WORK_WORDS * sizeof(uint32_t));
}

/*
 * Charge the growth of a round's buffer from `old_bytes` to `new_bytes`, or
 * record at `where` that the limit refused it.
 */
static bool
charge_growth(relic_scratch_t *scratch, size_t old_bytes, size_t new_bytes,
    const relic_location_t *where)
{
	if (!relic_account_charge_round(scratch->account,
	    new_bytes - old_bytes)) {
		record_error(scratch, where, RELIC_FAIL_LIMIT);
		return (false);
	}

	return (true);
}

/*
 * The capacity a match space part that has `capacity` instructions' room
 * grows to for `needed`, with `unit` bytes per instruction, or zero when
 * its bytes could not be counted.  It doubles until it fits, as the text
 * buffers do (buffer_make_room()).
 */
static size_t
grown_capacity(size_t capacity, size_t needed, size_t unit)
{
	size_t grown;

	grown = relic_grow_capacity_doubling(capacity, needed);
	if (grown > SIZE_MAX / unit)
		return (0);

	return (grown);
}

/*
 * Make room in the match space for a program of `count` instructions to be
 * compiled into, charged to the round.  What the room held is not kept,
 * since every program compiled into it is run once and discarded.  Returns
 * true, or false with the refusal recorded at `where` and the space as it
 * was.
 */
static bool
program_make_room(relic_scratch_t *scratch, size_t count,
    const relic_location_t *where)
{
	relic_match_space_t *space;
	relic_regex_inst_t *insts;
	size_t capacity;

	space = &scratch->match;
	if (count <= space->capacity)
		return (true);

	capacity = grown_capacity(space->capacity, count,
	    sizeof(relic_regex_inst_t));
	if (capacity == 0) {
		record_error(scratch, where, RELIC_FAIL_NO_MEMORY);
		return (false);
	}
	if (!charge_growth(scratch, program_room_bytes(space->capacity),
	    program_room_bytes(capacity), where))
		return (false);

	insts = malloc(program_room_bytes(capacity));
	if (insts == NULL) {
		relic_account_release_round(scratch->account,
		    program_room_bytes(capacity) -
		    program_room_bytes(space->capacity));
		record_error(scratch, where, RELIC_FAIL_NO_MEMORY);
		return (false);
	}

	free(space->insts);
	space->insts = insts;
	space->capacity = capacity;

	return (true);
}

/*
 * Make the matcher's work space serve a program of `count` instructions,
 * as program_make_room() makes room for a program.  A grown space starts
 * zeroed, with its stamp at zero, which is the state the matcher asks a new
 * work space to be in: a mark left over from a smaller space would say an
 * instruction is on a list it is not on.
 */
static bool
work_make_room(relic_scratch_t *scratch, size_t count,
    const relic_location_t *where)
{
	relic_regex_work_t *work;
	uint32_t *words;
	size_t capacity;

	work = &scratch->match.work;
	if (count <= work->capacity)
		return (true);

	capacity = grown_capacity(work->capacity, count,
	    RELIC_REGEX_WORK_WORDS * sizeof(uint32_t));
	if (capacity == 0) {
		record_error(scratch, where, RELIC_FAIL_NO_MEMORY);
		return (false);
	}
	if (!charge_growth(scratch, work_space_bytes(work->capacity),
	    work_space_bytes(capacity), where))
		return (false);

	words = calloc(capacity * RELIC_REGEX_WORK_WORDS, sizeof(uint32_t));
	if (words == NULL) {
		relic_account_release_round(scratch->account,
		    work_space_bytes(capacity) -
		    work_space_bytes(work->capacity));
		record_error(scratch, where, RELIC_FAIL_NO_MEMORY);
		return (false);
	}

	free(work->words);
	work->words = words;
	work->capacity = capacity;
	work->stamp = 0;

	return (true);
}

/*
 * Make room in `buffer` for `more` bytes past those in use, doubling it
 * until they fit, the growth charged to the round.  Returns true, or false
 * having recorded at `where` that the limit or the allocator refused the
 * growth, the buffer as it was.  A growth moves the buffer, so a caller
 * reads its bytes through offsets and takes pointers into it only after it
 * has made the room it needs.
 *
 * Doubling makes a buffer's capacity a function of the largest room it was
 * ever asked for.  The tasks of a parallel round together ask for every
 * room the serial run asks for, and for pending bytes they may ask for
 * more: a string the serial run interned earlier in the round is no longer
 * pending there, while a worker, reading the frozen pool, still carries its
 * bytes.  So the largest capacity any worker's buffer reaches is at least
 * the capacity the serial run's would have, which is what the merge's
 * admission counts (lib/merge.c); a merge declined on the excess is run
 * again serially, which the limit then judges exactly.
 */
static bool
buffer_make_room(relic_scratch_t *scratch, relic_text_buffer_t *buffer,
    size_t more, const relic_location_t *where)
{
	size_t capacity;
	char *bytes;

	if (more <= buffer->capacity - buffer->used)
		return (true);

	capacity = more > SIZE_MAX - buffer->used ? 0 :
	    relic_grow_capacity_doubling(buffer->capacity, buffer->used + more);
	if (capacity == 0) {
		record_error(scratch, where, RELIC_FAIL_NO_MEMORY);
		return (false);
	}

	if (!relic_account_charge_round(scratch->account,
	    capacity - buffer->capacity)) {
		record_error(scratch, where, RELIC_FAIL_LIMIT);
		return (false);
	}

	bytes = realloc(buffer->bytes, capacity);
	if (bytes == NULL) {
		relic_account_release_round(scratch->account,
		    capacity - buffer->capacity);
		record_error(scratch, where, RELIC_FAIL_NO_MEMORY);
		return (false);
	}

	buffer->bytes = bytes;
	buffer->capacity = capacity;

	return (true);
}

/*
 * The value a reading term stands for: its constant, or what its slot holds.
 */
static relic_dom_t
term_value(const relic_scratch_t *scratch, const relic_plan_term_t *term)
{
	if (term->action == RELIC_PLAN_CONSTANT)
		return (term->u.constant);

	return (scratch->frame[term->u.slot]);
}

/*
 * Whether a tuple matches a generator's terms, binding the slots it binds.
 *
 * Columns are visited left to right, so a slot bound by one column is in the
 * frame when a later column of the same tuple reads it; that is how edge(X,
 * X) finds only loops.  A tuple that fails part way may leave slots it bound
 * in the frame.  Nothing reads them: the operations after this one run only
 * after a match, and the next match overwrites them first.
 */
static bool
tuple_matches(relic_scratch_t *scratch, const relic_op_atom_t *atom,
    const relic_dom_t *tuple)
{
	const relic_plan_term_t *term;
	uint16_t column;

	for (column = 0; column < atom->arity; column++) {
		term = &atom->terms[column];

		switch (term->action) {
		case RELIC_PLAN_CONSTANT:
		case RELIC_PLAN_BOUND:
		case RELIC_PLAN_NUMBER:
		case RELIC_PLAN_TEXT:
			if (tuple[column] != term_value(scratch, term))
				return (false);
			break;
		case RELIC_PLAN_BIND:
			scratch->frame[term->u.slot] = tuple[column];
			break;
		case RELIC_PLAN_ANY:
			break;
		}
	}

	return (true);
}

/*
 * The tuple a test or an EMIT reads: every term's value, in column order.
 * `tuple` has room for the largest arity there is.
 */
static void
build_tuple(const relic_scratch_t *scratch, const relic_op_atom_t *atom,
    relic_dom_t *tuple)
{
	uint16_t column;

	for (column = 0; column < atom->arity; column++)
		tuple[column] = term_value(scratch, &atom->terms[column]);
}

/*
 * Whether a comparison holds between two values that `order` orders.  Two
 * values of different types are unordered, which satisfies no ordering
 * comparison and makes them unequal.  The binding is filtered out rather
 * than reported as an error, because a column may legitimately hold more
 * than one type.
 */
static bool
order_satisfies(enum relic_compare_op compare, enum relic_order order)
{
	switch (compare) {
	case RELIC_CMP_EQ:
		return (order == RELIC_ORDER_EQUAL);
	case RELIC_CMP_NE:
		return (order != RELIC_ORDER_EQUAL);
	case RELIC_CMP_LT:
		return (order == RELIC_ORDER_LESS);
	case RELIC_CMP_LE:
		return (order == RELIC_ORDER_LESS ||
		    order == RELIC_ORDER_EQUAL);
	case RELIC_CMP_GT:
		return (order == RELIC_ORDER_GREATER);
	case RELIC_CMP_GE:
		return (order == RELIC_ORDER_GREATER ||
		    order == RELIC_ORDER_EQUAL);
	}

	return (false);
}

static relic_cell_t
dom_cell(relic_dom_t dom)
{
	relic_cell_t cell;

	bzero(&cell, sizeof(cell));
	cell.u.dom = dom;
	cell.kind = RELIC_CELL_DOM;

	return (cell);
}

static relic_cell_t
number_cell(int64_t number)
{
	relic_cell_t cell;

	bzero(&cell, sizeof(cell));
	cell.u.number = number;
	cell.kind = RELIC_CELL_NUMBER;

	return (cell);
}

static relic_cell_t
text_cell(size_t offset, size_t length)
{
	relic_cell_t cell;

	bzero(&cell, sizeof(cell));
	cell.u.text.offset = offset;
	cell.u.text.length = length;
	cell.kind = RELIC_CELL_TEXT;

	return (cell);
}

/*
 * A cell naming the `length` bytes past those in use in the text stack,
 * which the caller has made room for and filled, and which are then in use.
 */
static relic_cell_t
text_push(relic_scratch_t *scratch, size_t length)
{
	relic_text_buffer_t *stack;
	relic_cell_t cell;

	stack = &scratch->text_stack;
	assert(length <= stack->capacity - stack->used);

	cell = text_cell(stack->used, length);
	stack->used += length;

	return (cell);
}

/*
 * Where the strings an operation computes begin in the text stack: past
 * the string of the ASSIGN `text_base` names, or at the stack's start
 * (plan.h).
 */
static size_t
text_base_top(const relic_scratch_t *scratch, size_t text_base)
{
	if (text_base == 0)
		return (0);

	return (scratch->states[text_base - 1].text_top);
}

/*
 * The integer a cell holds, when it holds one: its own, or the one its
 * identifier names.  A symbol or a string has none.
 */
static bool
cell_integer(const relic_exec_env_t *env, relic_cell_t cell, int64_t *integerp)
{
	switch (cell.kind) {
	case RELIC_CELL_NUMBER:
		*integerp = cell.u.number;
		return (true);
	case RELIC_CELL_DOM:
		return (relic_value_integer(env->ctx, cell.u.dom, integerp));
	case RELIC_CELL_TEXT:
		break;
	}

	return (false);
}

/*
 * The bytes a cell holds as a string, when it holds one: a computed
 * string's in the text stack, or those of the string an identifier names.
 * A symbol and an integer have none.  A pointer into the text stack is good
 * until the stack next grows.
 */
static bool
cell_string(const relic_scratch_t *scratch, const relic_exec_env_t *env,
    relic_cell_t cell, const char **bytesp, size_t *lengthp)
{
	switch (cell.kind) {
	case RELIC_CELL_TEXT:
		*bytesp = scratch->text_stack.bytes + cell.u.text.offset;
		*lengthp = cell.u.text.length;
		return (true);
	case RELIC_CELL_DOM:
		if ((cell.u.dom & RELIC_DOM_TYPE_MASK) != RELIC_DOM_STRING)
			return (false);
		*bytesp = relic_value_bytes(env->ctx, cell.u.dom, lengthp);
		return (*bytesp != NULL);
	case RELIC_CELL_NUMBER:
		break;
	}

	return (false);
}

/*
 * Whether a cell holds a value of the kind a function's argument wants.
 */
static bool
cell_is_kind(const relic_exec_env_t *env, relic_cell_t cell,
    enum relic_value_kind kind)
{
	int64_t integer;

	switch (kind) {
	case RELIC_KIND_ANY:
		return (true);
	case RELIC_KIND_INTEGER:
		return (cell_integer(env, cell, &integer));
	case RELIC_KIND_STRING:
		if (cell.kind == RELIC_CELL_TEXT)
			return (true);
		return (cell.kind == RELIC_CELL_DOM &&
		    (cell.u.dom & RELIC_DOM_TYPE_MASK) == RELIC_DOM_STRING &&
		    relic_value_bytes(env->ctx, cell.u.dom, NULL) != NULL);
	}

	return (false);
}

/*
 * The type of a cell's value, as a domain identifier's type bits.
 */
static relic_dom_t
cell_type(relic_cell_t cell)
{
	switch (cell.kind) {
	case RELIC_CELL_NUMBER:
		return (RELIC_DOM_INTEGER);
	case RELIC_CELL_TEXT:
		return (RELIC_DOM_STRING);
	case RELIC_CELL_DOM:
		break;
	}

	return (cell.u.dom & RELIC_DOM_TYPE_MASK);
}

/*
 * How two cells' values order, by the rules of comparison: two identifiers
 * as their values order; two strings, computed or named, by their bytes;
 * two integers, computed or named, numerically; and values of different
 * types not at all.  A computed value is compared by what it is, never by
 * interning it, which would write the context.
 */
static enum relic_order
cells_order(const relic_scratch_t *scratch, const relic_exec_env_t *env,
    relic_cell_t left, relic_cell_t right)
{
	const char *left_bytes;
	const char *right_bytes;
	size_t left_length;
	size_t right_length;
	int64_t left_integer;
	int64_t right_integer;

	if (left.kind == RELIC_CELL_DOM && right.kind == RELIC_CELL_DOM)
		return (relic_value_order(env->ctx, left.u.dom, right.u.dom));

	if (left.kind == RELIC_CELL_TEXT || right.kind == RELIC_CELL_TEXT) {
		if (!cell_string(scratch, env, left, &left_bytes,
		    &left_length) ||
		    !cell_string(scratch, env, right, &right_bytes,
		    &right_length))
			return (RELIC_ORDER_UNORDERED);
		return (relic_bytes_order(left_bytes, left_length, right_bytes,
		    right_length));
	}

	if (!cell_integer(env, left, &left_integer) ||
	    !cell_integer(env, right, &right_integer))
		return (RELIC_ORDER_UNORDERED);

	if (left_integer < right_integer)
		return (RELIC_ORDER_LESS);
	if (left_integer > right_integer)
		return (RELIC_ORDER_GREATER);

	return (RELIC_ORDER_EQUAL);
}

/*
 * Record, when the environment asks and nothing is recorded yet, that an
 * ordering comparison at `where` was false because its values' types
 * differ.
 */
static void
record_ordering_note(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_location_t *where, relic_cell_t left, relic_cell_t right)
{
	relic_mixed_note_t *note;

	note = &scratch->note;
	if (!env->note_mixed || note->where != NULL)
		return;

	bzero(note, sizeof(*note));
	note->where = where;
	note->kind = RELIC_NOTE_ORDERING;
	note->left_type = cell_type(left);
	note->right_type = cell_type(right);
}

/*
 * Record, when the environment asks and nothing is recorded yet, that the
 * operator or function spelled `spelling` at `where` was given an operand
 * that is not of the type it wants.
 */
static void
record_operand_note(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_location_t *where, const char *spelling, relic_cell_t operand)
{
	relic_mixed_note_t *note;

	note = &scratch->note;
	if (!env->note_mixed || note->where != NULL)
		return;

	bzero(note, sizeof(*note));
	note->where = where;
	note->kind = RELIC_NOTE_OPERAND;
	note->operator = spelling;
	note->left_type = cell_type(operand);
}

/*
 * Record, when the environment asks and nothing is recorded yet, that an
 * aggregate had no value because of the types its group's rows hold.
 */
static void
record_aggregate_note(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_aggregate_t *aggregate, enum relic_note_kind kind)
{
	relic_mixed_note_t *note;

	note = &scratch->note;
	if (!env->note_mixed || note->where != NULL)
		return;

	bzero(note, sizeof(*note));
	note->where = &aggregate->where;
	note->kind = kind;
	note->aggregate = aggregate->aggregate;
}

/*
 * Record the first failure of the running plan, at the literal `where`, or
 * at none.
 */
static void
record_error(relic_scratch_t *scratch, const relic_location_t *where,
    enum relic_exec_failure failure)
{
	if (scratch->error.failed)
		return;

	scratch->error.where = where;
	scratch->error.failure = failure;
	scratch->error.failed = true;
}

/*
 * Record that a pattern met at `where` was not a regular expression, and
 * why.
 */
static void
record_pattern_error(relic_scratch_t *scratch, const relic_location_t *where,
    enum relic_regex_failure pattern)
{
	if (scratch->error.failed)
		return;

	record_error(scratch, where, RELIC_FAIL_PATTERN);
	scratch->error.pattern = pattern;
}

/*
 * How an arithmetic step's operator is written.
 */
static const char *
operator_spelling(const relic_step_t *step)
{
	if (step->kind == RELIC_STEP_NEGATE)
		return ("-");

	switch (step->u.op) {
	case RELIC_ARITH_ADD:
		return ("+");
	case RELIC_ARITH_SUBTRACT:
		return ("-");
	case RELIC_ARITH_MULTIPLY:
		return ("*");
	case RELIC_ARITH_DIVIDE:
		return ("/");
	case RELIC_ARITH_REMAINDER:
		break;
	}

	return ("mod");
}

/*
 * `cat`: the bytes of the first string followed by those of the second,
 * built past the text stack's top.  A result longer than a value can hold
 * is the failure it is before a byte of it is copied.  The room is made
 * before either operand's bytes are found, since making it may move the
 * stack they are in.
 */
static enum relic_run_end
call_cat(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_location_t *where, const relic_cell_t *args,
    relic_cell_t *resultp)
{
	const char *left;
	const char *right;
	size_t left_length;
	size_t right_length;
	size_t length;
	char *top;

	if (!cell_string(scratch, env, args[0], &left, &left_length) ||
	    !cell_string(scratch, env, args[1], &right, &right_length))
		return (RELIC_RUN_NO_VALUE);

	if (!relic_cat_fits(left_length, right_length, &length)) {
		record_error(scratch, where, RELIC_FAIL_TOO_LONG);
		return (RELIC_RUN_FAILED);
	}
	if (!buffer_make_room(scratch, &scratch->text_stack, length, where))
		return (RELIC_RUN_FAILED);

	(void)cell_string(scratch, env, args[0], &left, &left_length);
	(void)cell_string(scratch, env, args[1], &right, &right_length);
	top = scratch->text_stack.bytes + scratch->text_stack.used;
	if (left_length > 0)
		bcopy(left, top, left_length);
	if (right_length > 0)
		bcopy(right, top + left_length, right_length);

	*resultp = text_push(scratch, length);

	return (RELIC_RUN_VALUE);
}

/*
 * `substr`: the bytes of a string that relic_substr_range() keeps.  A
 * computed string's are named where they already are, since the stack
 * holds them until this operation is done; a named string's are copied past
 * the stack's top, where every computed string is.
 */
static enum relic_run_end
call_substr(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_location_t *where, const relic_cell_t *args, bool counted,
    relic_cell_t *resultp)
{
	const char *bytes;
	size_t length;
	size_t offset;
	size_t kept;
	int64_t start;
	int64_t count;

	count = 0;
	if (!cell_string(scratch, env, args[0], &bytes, &length) ||
	    !cell_integer(env, args[1], &start) ||
	    (counted && !cell_integer(env, args[2], &count)))
		return (RELIC_RUN_NO_VALUE);

	relic_substr_range(length, start, count, counted, &offset, &kept);

	if (args[0].kind == RELIC_CELL_TEXT) {
		*resultp = text_cell(args[0].u.text.offset + offset, kept);
		return (RELIC_RUN_VALUE);
	}

	if (!buffer_make_room(scratch, &scratch->text_stack, kept, where))
		return (RELIC_RUN_FAILED);
	if (kept > 0)
		bcopy(bytes + offset, scratch->text_stack.bytes +
		    scratch->text_stack.used, kept);
	*resultp = text_push(scratch, kept);

	return (RELIC_RUN_VALUE);
}

/*
 * `str`: a computed string is itself; an integer, computed or named, is
 * its decimal text; a named symbol or string is its bytes, copied past the
 * stack's top.
 */
static enum relic_run_end
call_str(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_location_t *where, relic_cell_t value, relic_cell_t *resultp)
{
	const char *bytes;
	size_t length;
	int64_t integer;

	if (value.kind == RELIC_CELL_TEXT) {
		*resultp = value;
		return (RELIC_RUN_VALUE);
	}

	if (cell_integer(env, value, &integer)) {
		if (!buffer_make_room(scratch, &scratch->text_stack,
		    RELIC_INTEGER_TEXT_MAX, where))
			return (RELIC_RUN_FAILED);
		length = relic_integer_text(integer, scratch->text_stack.bytes +
		    scratch->text_stack.used);
		*resultp = text_push(scratch, length);
		return (RELIC_RUN_VALUE);
	}

	bytes = relic_value_bytes(env->ctx, value.u.dom, &length);
	assert(bytes != NULL);
	if (!buffer_make_room(scratch, &scratch->text_stack, length, where))
		return (RELIC_RUN_FAILED);
	if (length > 0)
		bcopy(bytes, scratch->text_stack.bytes +
		    scratch->text_stack.used, length);
	*resultp = text_push(scratch, length);

	return (RELIC_RUN_VALUE);
}

/*
 * `match`: where the leftmost match of the pattern begins in the string, or
 * -1 when none does.  A pattern the rule compiled is run as it is; one that
 * arrives as a value is compiled into the match space first, and fails the
 * evaluation when it is not a regular expression.  Growing the match space
 * never moves the text stack, so the string's bytes stay where they are.
 */
static enum relic_run_end
call_match(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_location_t *where, const relic_pattern_t *compiled,
    const relic_cell_t *args, relic_cell_t *resultp)
{
	relic_pattern_t pattern;
	const char *source;
	const char *text;
	size_t source_length;
	size_t text_length;

	if (!cell_string(scratch, env, args[0], &text, &text_length) ||
	    !cell_string(scratch, env, args[1], &source, &source_length))
		return (RELIC_RUN_NO_VALUE);

	if (compiled != NULL)
		pattern = *compiled;
	else if (!compile_pattern(scratch, where, source, source_length,
	    &pattern))
		return (RELIC_RUN_FAILED);

	if (!work_make_room(scratch, pattern.count, where))
		return (RELIC_RUN_FAILED);

	*resultp = number_cell(relic_regex_match(pattern.insts, pattern.count,
	    &scratch->match.work, text, text_length));

	return (RELIC_RUN_VALUE);
}

/*
 * Compile the `length` bytes of a pattern into the match space's program
 * room, setting *patternp to the program there.  Returns true, or false
 * with the failure recorded: the pattern not a regular expression, or the
 * room not to be had.  A pattern too long for its program's instructions to
 * be numbered would need more room than any allocator gives, and is refused
 * as the allocator refusing it.
 */
static bool
compile_pattern(relic_scratch_t *scratch, const relic_location_t *where,
    const char *bytes, size_t length, relic_pattern_t *patternp)
{
	relic_regex_error_t error;
	size_t capacity;

	capacity = relic_regex_capacity(length);
	if (capacity == 0) {
		record_error(scratch, where, RELIC_FAIL_NO_MEMORY);
		return (false);
	}
	if (!program_make_room(scratch, capacity, where))
		return (false);

	patternp->insts = scratch->match.insts;
	patternp->count = relic_regex_compile(bytes, length,
	    scratch->match.insts, &error);
	if (patternp->count == 0) {
		record_pattern_error(scratch, where, error.failure);
		return (false);
	}

	return (true);
}

/*
 * Call `fn` on `args`, one cell per argument, leaving the result in
 * *resultp; `pattern` is a match's pattern compiled with the rule, or NULL.
 * An argument not of the kind the function wants gives the call no value,
 * with the note recorded, before anything is computed.
 */
static enum relic_run_end
call_function(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_location_t *where, enum relic_function fn,
    const relic_pattern_t *pattern, const relic_cell_t *args,
    relic_cell_t *resultp)
{
	const relic_function_info_t *info;
	const char *haystack;
	const char *needle;
	size_t haystack_length;
	size_t needle_length;
	uint8_t i;

	info = relic_function_info(fn);
	for (i = 0; i < info->arity; i++) {
		if (!cell_is_kind(env, args[i], info->args[i])) {
			record_operand_note(scratch, env, where,
			    info->spelling, args[i]);
			return (RELIC_RUN_NO_VALUE);
		}
	}

	switch (fn) {
	case RELIC_FN_LENGTH:
		if (!cell_string(scratch, env, args[0], &haystack,
		    &haystack_length))
			return (RELIC_RUN_NO_VALUE);
		*resultp = number_cell((int64_t)haystack_length);
		return (RELIC_RUN_VALUE);
	case RELIC_FN_INDEX:
		if (!cell_string(scratch, env, args[0], &haystack,
		    &haystack_length) ||
		    !cell_string(scratch, env, args[1], &needle,
		    &needle_length))
			return (RELIC_RUN_NO_VALUE);
		*resultp = number_cell(relic_index_of(haystack,
		    haystack_length, needle, needle_length));
		return (RELIC_RUN_VALUE);
	case RELIC_FN_CAT:
		return (call_cat(scratch, env, where, args, resultp));
	case RELIC_FN_SUBSTR2:
		return (call_substr(scratch, env, where, args, false,
		    resultp));
	case RELIC_FN_SUBSTR3:
		return (call_substr(scratch, env, where, args, true, resultp));
	case RELIC_FN_STR:
		return (call_str(scratch, env, where, args[0], resultp));
	case RELIC_FN_MATCH:
		return (call_match(scratch, env, where, pattern, args,
		    resultp));
	}

	assert(!"a call of no function");

	return (RELIC_RUN_NO_VALUE);
}

/*
 * Run a postfix program of a literal written at `where`, leaving its value
 * in *valuep.  Each step pushes a cell or pops its operands and pushes its
 * result, and the planner sized the program so that the stack never holds
 * more than RELIC_POSTFIX_DEPTH_MAX cells.  An operand that is not of the
 * type its operator or function wants ends the run with no value, and a
 * result that is not a value ends it with the failure recorded.
 */
static enum relic_run_end
run_postfix(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_postfix_t *postfix, const relic_location_t *where,
    relic_cell_t *valuep)
{
	relic_cell_t stack[RELIC_POSTFIX_DEPTH_MAX];
	const relic_function_info_t *info;
	enum relic_arith_status status;
	enum relic_run_end end;
	const relic_step_t *step;
	relic_cell_t result;
	int64_t right;
	int64_t left;
	int64_t number;
	size_t held;
	size_t i;

	assert(postfix->depth <= RELIC_POSTFIX_DEPTH_MAX);
	held = 0;

	for (i = 0; i < postfix->step_count; i++) {
		step = &postfix->steps[i];

		switch (step->kind) {
		case RELIC_STEP_CONSTANT:
			assert(held < postfix->depth);
			stack[held++] = dom_cell(step->u.constant);
			continue;
		case RELIC_STEP_DOMAIN_SLOT:
			assert(held < postfix->depth);
			stack[held++] = dom_cell(scratch->frame[step->u.slot]);
			continue;
		case RELIC_STEP_NUMBER_SLOT:
			assert(held < postfix->depth);
			stack[held++] = number_cell(
			    scratch->numbers[step->u.slot]);
			continue;
		case RELIC_STEP_TEXT_SLOT:
			assert(held < postfix->depth);
			stack[held++] = text_cell(
			    scratch->texts[step->u.slot].offset,
			    scratch->texts[step->u.slot].length);
			continue;
		case RELIC_STEP_CALL:
			info = relic_function_info(step->u.call.fn);
			assert(held >= info->arity);
			end = call_function(scratch, env, where,
			    step->u.call.fn, step->u.call.pattern,
			    &stack[held - info->arity], &result);
			if (end != RELIC_RUN_VALUE)
				return (end);
			held -= info->arity - 1;
			stack[held - 1] = result;
			continue;
		case RELIC_STEP_NEGATE:
			assert(held >= 1);
			if (!cell_integer(env, stack[held - 1], &right)) {
				record_operand_note(scratch, env, where,
				    operator_spelling(step), stack[held - 1]);
				return (RELIC_RUN_NO_VALUE);
			}
			status = relic_arith_negate(right, &number);
			break;
		case RELIC_STEP_BINARY:
			assert(held >= 2);
			if (!cell_integer(env, stack[held - 2], &left)) {
				record_operand_note(scratch, env, where,
				    operator_spelling(step), stack[held - 2]);
				return (RELIC_RUN_NO_VALUE);
			}
			if (!cell_integer(env, stack[held - 1], &right)) {
				record_operand_note(scratch, env, where,
				    operator_spelling(step), stack[held - 1]);
				return (RELIC_RUN_NO_VALUE);
			}
			held--;
			status = relic_arith_binary(step->u.op, left, right,
			    &number);
			break;
		}

		if (status != RELIC_ARITH_DEFINED) {
			record_error(scratch, where,
			    status == RELIC_ARITH_ZERO_DIVISOR ?
			    RELIC_FAIL_ZERO_DIVISOR : RELIC_FAIL_OVERFLOW);
			return (RELIC_RUN_FAILED);
		}
		stack[held - 1] = number_cell(number);
	}

	assert(held == 1);
	*valuep = stack[0];

	return (RELIC_RUN_VALUE);
}

/*
 * The identifiers an operation's view covers now: the whole relation, or a
 * range derived from the delta of the member being read, as plan.h sets out.
 */
relic_tuple_range_t
relic_exec_view(const relic_exec_env_t *env, const relic_op_atom_t *atom)
{
	relic_tuple_range_t delta;
	relic_tuple_range_t range;

	if (atom->view == RELIC_VIEW_ALL)
		return (relic_relation_all(atom->relation));

	assert(env->deltas != NULL);
	delta = env->deltas[relic_relation_pred(atom->relation) - 1];
	range.first = RELIC_TUPLE_FIRST;
	range.end = delta.end;

	switch (atom->view) {
	case RELIC_VIEW_OLD:
		range.end = delta.first;
		break;
	case RELIC_VIEW_DELTA:
		range.first = delta.first;
		break;
	case RELIC_VIEW_ALL:
	case RELIC_VIEW_CURRENT:
		break;
	}

	return (range);
}

/*
 * Resolve the view of every operation of `plan` that reads a relation,
 * before any of them runs.  Nothing has been inserted yet, so every range
 * ends at or before its relation's end as the plan starts, and the tuples
 * the plan derives, whenever its batches are inserted, lie past it: what a
 * plan reads cannot depend on when its derivations are written.
 */
static void
resolve_views(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_plan_t *plan)
{
	const relic_op_t *op;
	size_t level;

	for (level = 0; level < plan->op_count; level++) {
		op = &plan->ops[level];
		switch (op->kind) {
		case RELIC_OP_SCAN:
		case RELIC_OP_LOOKUP:
		case RELIC_OP_MEMBER:
		case RELIC_OP_EXISTS:
		case RELIC_OP_ANTI_MEMBER:
			scratch->states[level].view = relic_exec_view(env,
			    &op->u.atom);
			break;
		case RELIC_OP_AGGREGATE:
			if (op->u.aggregate.access == RELIC_FOLD_SUMMARY)
				break;
			scratch->states[level].view = relic_relation_all(
			    op->u.aggregate.relation);
			break;
		case RELIC_OP_FILTER:
		case RELIC_OP_ASSIGN:
		case RELIC_OP_EMIT:
			break;
		}
	}
}

/*
 * Count a tuple against the reads the call may still make, for a step that
 * has no cursor to resume from: what it has computed so far cannot be kept
 * between calls.  Once the reads are spent the step asks the call to
 * pause, but finishes all the same, and the call pauses when the step
 * returns, having read at most one such step's walk past its reads.  A
 * fold is such a step, its walk its group's chain through an index or the
 * whole relation when it has no key or no index, and so is a probe that
 * scans its view.
 */
static void
charge_unresumable_read(relic_scratch_t *scratch)
{
	if (scratch->reads_left == 0) {
		scratch->paused = true;
		return;
	}
	scratch->reads_left--;
}

/*
 * A SCAN: find the next tuple of its view that matches, starting from the
 * beginning of the view when not resuming.  The tuple is fetched afresh by
 * identifier each time rather than held, because the caller may insert into
 * the relation, and move its storage, between two calls.  Each tuple the
 * scan rejects is one of the reads the call may make -- one it accepts is
 * paid for by the turns that follow -- and a scan that has used them up
 * before finding a match pauses, its cursor past the last tuple it
 * rejected, where the next call resumes it.  The count is kept in a local
 * while the scan runs and given back as it returns.
 */
static bool
step_scan(relic_scratch_t *scratch, const relic_op_atom_t *atom,
    relic_op_state_t *state, bool resume)
{
	relic_tuple_range_t *cursor;
	const relic_dom_t *tuple;
	relic_tuple_id_t id;
	uint64_t reads_left;
	bool matched;

	cursor = &state->cursor;
	if (!resume)
		*cursor = state->view;

	matched = false;
	reads_left = scratch->reads_left;
	while (cursor->first < cursor->end) {
		id = cursor->first;
		cursor->first++;
		scratch->counters.tuples_scanned++;

		tuple = relic_relation_tuple(atom->relation, id);
		if (tuple_matches(scratch, atom, tuple)) {
			matched = true;
			break;
		}

		if (reads_left == 0) {
			scratch->paused = true;
			break;
		}
		reads_left--;
	}
	scratch->reads_left = reads_left;

	return (matched);
}

/*
 * A LOOKUP: find the next tuple on the chain of the key its bound columns
 * hold that matches, starting when not resuming at the first chain tuple in
 * its view (lookup_start()) and stopping at the end of the view.
 *
 * Every tuple on the chain already agrees with the key columns; it is matched
 * against every term all the same, which binds the other columns and checks
 * a variable written twice among them, so a lookup accepts exactly the
 * tuples a scan with the same terms would.  A lookup counts what it rejects
 * and pauses as a scan does, its cursor on the chain.
 */
static bool
step_lookup(relic_scratch_t *scratch, const relic_op_atom_t *atom,
    relic_op_state_t *state, bool resume)
{
	relic_tuple_range_t *cursor;
	const relic_index_t *index;
	const relic_dom_t *tuple;
	relic_tuple_id_t id;
	uint64_t reads_left;
	bool matched;

	cursor = &state->cursor;
	index = lookup_index(atom);

	if (!resume) {
		cursor->first = lookup_start(scratch, atom, index,
		    state->view.first);
		cursor->end = state->view.end;
	}

	matched = false;
	reads_left = scratch->reads_left;
	while (cursor->first != RELIC_ID_NONE && cursor->first < cursor->end) {
		id = cursor->first;
		cursor->first = relic_index_next(index, id);
		scratch->counters.tuples_looked_up++;

		tuple = relic_relation_tuple(atom->relation, id);
		if (tuple_matches(scratch, atom, tuple)) {
			matched = true;
			break;
		}

		if (reads_left == 0) {
			scratch->paused = true;
			break;
		}
		reads_left--;
	}
	scratch->reads_left = reads_left;

	return (matched);
}

/*
 * The index a LOOKUP reads, found by position each time, since the array
 * holding it can grow while the plan exists.
 */
static const relic_index_t *
lookup_index(const relic_op_atom_t *atom)
{
	const relic_index_t *index;

	index = relic_relation_index_at(atom->relation, atom->index_position);
	assert(relic_index_key_mask(index) == atom->key_mask);

	return (index);
}

/*
 * Where a LOOKUP's walk of its view, which begins at `first`, starts: the
 * head of the chain of the key its bound columns hold for every view but a
 * delta, and for a delta the first chain tuple at or past its start, which
 * the index finds from the key's frontier.  The probe holds each key
 * column's value in that column, the key columns being those of the mask
 * the lookup was planned with, which is its index's; the index reads no
 * other column, and the rest are left unset.
 */
static relic_tuple_id_t
lookup_start(const relic_scratch_t *scratch, const relic_op_atom_t *atom,
    const relic_index_t *index, relic_tuple_id_t first)
{
	relic_dom_t probe[RELIC_MAX_ARITY];
	uint16_t column;

	for (column = 0; column < atom->arity; column++) {
		if ((atom->key_mask & (UINT32_C(1) << column)) != 0)
			probe[column] = term_value(scratch,
			    &atom->terms[column]);
	}

	if (atom->view == RELIC_VIEW_DELTA)
		return (relic_index_first_since(index, atom->relation, probe,
		    first));

	return (relic_index_first(index, atom->relation, probe));
}

/*
 * A MEMBER, and an ANTI_MEMBER's probe of every column: whether the relation
 * holds the tuple the terms name, at an identifier inside the operation's
 * view.
 */
static bool
step_member(const relic_scratch_t *scratch, const relic_op_atom_t *atom,
    const relic_tuple_range_t *view)
{
	relic_dom_t tuple[RELIC_MAX_ARITY];
	relic_tuple_id_t id;

	build_tuple(scratch, atom, tuple);

	id = relic_relation_find(atom->relation, tuple);
	if (id == RELIC_ID_NONE)
		return (false);

	return (id >= view->first && id < view->end);
}

/*
 * The question an EXISTS asks and an ANTI_MEMBER inverts: whether the
 * relation holds, inside the operation's view, a tuple that agrees with the
 * terms on every column they read, asked in the shape the planner chose for
 * the key.
 */
static bool
step_probe(relic_scratch_t *scratch, const relic_op_atom_t *atom,
    const relic_tuple_range_t *view)
{
	switch (relic_plan_probe_shape(atom->key_mask, atom->arity)) {
	case RELIC_PROBE_MEMBER:
		return (step_member(scratch, atom, view));
	case RELIC_PROBE_INDEX:
		return (probe_index(scratch, atom, view));
	case RELIC_PROBE_SCAN:
		break;
	}

	return (probe_scan(scratch, atom, view));
}

/*
 * A probe through the secondary index keyed on the columns the atom reads:
 * whether the walk a LOOKUP of the same key through the same view would
 * make finds a first tuple.  It begins where that walk begins, at the head
 * of the key's chain for every view but a delta, and for a delta at the
 * first chain tuple at or past its start, which the index finds from the
 * key's frontier.  A chain lists its tuples in increasing identifier order,
 * so the tuple the walk begins at is the lowest of the key's tuples it can
 * see, and some tuple of the key lies in the view exactly when that one is
 * below the view's end.  The view's start never excludes it: the other
 * views begin at the relation's first identifier.
 */
static bool
probe_index(relic_scratch_t *scratch, const relic_op_atom_t *atom,
    const relic_tuple_range_t *view)
{
	const relic_index_t *index;
	relic_dom_t probe[RELIC_MAX_ARITY];
	relic_tuple_id_t id;
	uint16_t column;

	index = relic_relation_index_at(atom->relation, atom->index_position);
	assert(relic_index_key_mask(index) == atom->key_mask);

	for (column = 0; column < atom->arity; column++) {
		if ((atom->key_mask & (UINT32_C(1) << column)) != 0)
			probe[column] = term_value(scratch,
			    &atom->terms[column]);
	}

	if (atom->view == RELIC_VIEW_DELTA)
		id = relic_index_first_since(index, atom->relation, probe,
		    view->first);
	else
		id = relic_index_first(index, atom->relation, probe);
	if (id == RELIC_ID_NONE || id >= view->end)
		return (false);

	assert(id >= view->first);
	scratch->counters.tuples_looked_up++;

	return (true);
}

/*
 * A probe by scanning the view for the first tuple the terms match, `_`
 * matching anything: the whole question when the atom names no column,
 * which the first tuple of a nonempty view answers, and the question of a
 * key that names some under RELIC_F_NO_INDEX.  A test has no cursor to
 * resume from, so the scan charges its reads as a fold does and finishes
 * before the call may pause.
 */
static bool
probe_scan(relic_scratch_t *scratch, const relic_op_atom_t *atom,
    const relic_tuple_range_t *view)
{
	relic_tuple_id_t id;

	for (id = view->first; id < view->end; id++) {
		charge_unresumable_read(scratch);
		scratch->counters.tuples_scanned++;
		if (tuple_matches(scratch, atom,
		    relic_relation_tuple(atom->relation, id)))
			return (true);
	}

	return (false);
}


/*
 * A FILTER: whether its comparison holds between the values of its two
 * postfix programs.  A side with no value makes the comparison false, so
 * the right side is not run once the left has none: a failure the right
 * side would meet is not met, as it is not when an earlier filter drops the
 * binding.
 *
 * Two identifiers are equal exactly when they are one identifier, since
 * every value is interned, so an equality of identifiers compares them
 * without decoding.  Otherwise the values are ordered, and when the
 * environment asks for them, a comparison that fails because it ordered
 * values of different types is recorded in the scratch for the caller to
 * report, when it is the first since the caller took the record.  The
 * strings the programs computed are dead once the comparison is made.
 */
static bool
step_filter(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_filter_t *filter)
{
	enum relic_order order;
	relic_cell_t left;
	relic_cell_t right;

	scratch->text_stack.used = text_base_top(scratch, filter->text_base);

	if (run_postfix(scratch, env, &filter->left, &filter->where,
	    &left) != RELIC_RUN_VALUE)
		return (false);

	if (run_postfix(scratch, env, &filter->right, &filter->where,
	    &right) != RELIC_RUN_VALUE)
		return (false);

	if (left.kind == RELIC_CELL_DOM && right.kind == RELIC_CELL_DOM &&
	    (filter->compare == RELIC_CMP_EQ ||
	    filter->compare == RELIC_CMP_NE))
		return ((left.u.dom == right.u.dom) ==
		    (filter->compare == RELIC_CMP_EQ));

	order = cells_order(scratch, env, left, right);
	if (order_satisfies(filter->compare, order))
		return (true);

	if (order == RELIC_ORDER_UNORDERED &&
	    filter->compare != RELIC_CMP_EQ && filter->compare != RELIC_CMP_NE)
		record_ordering_note(scratch, env, &filter->where, left, right);

	return (false);
}

/*
 * Bind text slot `slot` to the computed string `text`, which an ASSIGN
 * whose strings begin at `base` in the text stack computed, and record in
 * `state` the top the operations after it build above.
 *
 * A string computed by this ASSIGN lies at or past the base, and is moved
 * down to it, so that the temporaries above it are given back.  One that
 * lies wholly below the base -- another text slot's string, or a part of
 * one -- is kept where it is: it lies below every top an operation after
 * this one builds above, and when the operation that computed it runs again
 * this one runs again before anything reads the slot.  Either way the frame
 * entry is the string's identifier, when the context has assigned one.
 */
static void
bind_text(relic_scratch_t *scratch, const relic_exec_env_t *env,
    uint16_t slot, relic_text_t text, size_t base, relic_op_state_t *state)
{
	relic_text_buffer_t *stack;

	stack = &scratch->text_stack;

	if (text.offset + text.length <= base) {
		state->text_top = base;
	} else {
		assert(text.offset >= base);
		if (text.offset != base)
			memmove(stack->bytes + base, stack->bytes + text.offset,
			    text.length);
		text.offset = base;
		state->text_top = base + text.length;
	}
	stack->used = state->text_top;

	scratch->texts[slot] = text;
	scratch->frame[slot] = relic_value_find_string(env->ctx,
	    stack->bytes + text.offset, text.length);
}

/*
 * An ASSIGN: bind its slot to the value of its program, by the slot's
 * kind: an identifier into a domain slot, an integer into a number slot,
 * or a string into a text slot, the frame entry of either of the last two
 * being the value's identifier when the context has assigned one and zero
 * when it has not.  A program with no value binds nothing and rejects the
 * binding.
 */
static bool
step_assign(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_assign_t *assign, relic_op_state_t *state)
{
	relic_cell_t value;
	size_t base;

	base = text_base_top(scratch, assign->text_base);
	scratch->text_stack.used = base;

	if (run_postfix(scratch, env, &assign->value, &assign->where,
	    &value) != RELIC_RUN_VALUE)
		return (false);

	switch (assign->kind) {
	case RELIC_SLOT_DOMAIN:
		assert(value.kind == RELIC_CELL_DOM);
		scratch->frame[assign->slot] = value.u.dom;
		break;
	case RELIC_SLOT_NUMBER:
		assert(value.kind == RELIC_CELL_NUMBER);
		scratch->numbers[assign->slot] = value.u.number;
		scratch->frame[assign->slot] = relic_value_find_integer(
		    env->ctx, value.u.number);
		break;
	case RELIC_SLOT_TEXT:
		assert(value.kind == RELIC_CELL_TEXT);
		bind_text(scratch, env, assign->slot, value.u.text, base,
		    state);
		break;
	}

	return (true);
}

/*
 * Lay an AGGREGATE's key out as a tuple of the relation it folds, which is
 * how an index is probed: each key column holds the value of the key slot
 * paired with it, the slots being listed in increasing column order, and
 * the other columns are left unwritten, since nothing reads them.  A key
 * slot that is a number slot holds the integer's identifier, or zero when
 * the value table lacks the integer, which no column holds, as no relation
 * holds such an integer.
 */
static void
gather_fold_key(const relic_scratch_t *scratch,
    const relic_op_aggregate_t *aggregate, relic_dom_t *probe)
{
	uint16_t column;
	uint16_t key;

	key = 0;
	for (column = 0; column < aggregate->arity; column++) {
		if ((aggregate->key_mask & (UINT32_C(1) << column)) == 0)
			continue;
		probe[column] = scratch->frame[aggregate->key_slots[key]];
		key++;
	}
}

/*
 * Whether a row of the relation an AGGREGATE folds holds the key `probe`
 * lays out: whether it agrees with the probe on every key column.
 */
static bool
key_matches(const relic_op_aggregate_t *aggregate, const relic_dom_t *probe,
    const relic_dom_t *tuple)
{
	uint16_t column;

	for (column = 0; column < aggregate->arity; column++) {
		if ((aggregate->key_mask & (UINT32_C(1) << column)) != 0 &&
		    tuple[column] != probe[column])
			return (false);
	}

	return (true);
}

/*
 * Fold one row into the fold.  Returns false when the row leaves the group
 * outside the function's domain -- a sum's row that is not an integer, or
 * a min's or a max's row of another type than the first -- and the group
 * then has no value whatever its other rows hold, so the walk stops there.
 * A count reads no column: its group may have none, and then a tuple holds
 * no value.  Every column of a stored tuple holds a value the context
 * assigned, so a view of it is always had.
 */
static bool
fold_row(const relic_exec_env_t *env, const relic_op_aggregate_t *aggregate,
    relic_fold_t *fold, const relic_dom_t *tuple)
{
	relic_value_view_t value;
	const relic_value_view_t *read;
	relic_dom_t dom;
	bool known RELIC_UNUSED;

	dom = 0;
	read = NULL;
	if (aggregate->fn != RELIC_AGG_COUNT) {
		dom = tuple[aggregate->column];
		known = relic_value_view(env->ctx, dom, &value);
		assert(known);
		read = &value;
	}

	switch (relic_fold_add(aggregate->fn, &fold->state, fold->rows,
	    &fold->extreme_view, read)) {
	case RELIC_FOLD_INVALIDATED:
		return (false);
	case RELIC_FOLD_REPLACED:
		fold->extreme = dom;
		fold->extreme_view = value;
		break;
	case RELIC_FOLD_KEPT:
	case RELIC_FOLD_TIED:
		break;
	}

	fold->rows++;

	return (true);
}

/*
 * Fold every row of the group the key slots name, reached as the
 * operation's access says, within `view`, stopping at a row that leaves
 * the group with no value.  The walk holds no tuple past the row it folds,
 * and the relation is complete, so nothing inserts into it while the walk
 * runs.  A fold cannot pause part way, since its running value lives in
 * its caller's frame, so it charges the rows it reads to the call's reads
 * and goes on, and the call pauses once its step returns.
 *
 * A fold of a summary reads no row: the group's count and accumulator
 * were completed when the source was reduced, and a key no row holds
 * leaves the fold empty, as a walk that matches nothing does.  Nor does
 * an unkeyed count, whose group is the whole view: its count is the
 * view's length.
 */
static void
fold_group(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_aggregate_t *aggregate, const relic_tuple_range_t *view,
    relic_fold_t *fold)
{
	const relic_index_t *index;
	const relic_dom_t *tuple;
	relic_dom_t key[RELIC_MAX_ARITY];
	relic_tuple_id_t id;
	size_t group;
	uint16_t k;

	if (aggregate->access == RELIC_FOLD_SUMMARY) {
		for (k = 0; k < aggregate->key_count; k++)
			key[k] = scratch->frame[aggregate->key_slots[k]];
		scratch->counters.summary_probes++;
		group = relic_summary_find(aggregate->summary, key);
		if (group == RELIC_SUMMARY_NONE)
			return;
		if (aggregate->fn != RELIC_AGG_COUNT)
			fold->state = *relic_summary_state(aggregate->summary,
			    group, aggregate->signature, &fold->extreme);
		fold->rows = relic_summary_rows(aggregate->summary, group);
		return;
	}

	gather_fold_key(scratch, aggregate, key);

	switch (aggregate->access) {
	case RELIC_FOLD_SCAN:
		for (id = view->first; id < view->end; id++) {
			charge_unresumable_read(scratch);
			scratch->counters.tuples_scanned++;
			tuple = relic_relation_tuple(aggregate->relation, id);
			if (key_matches(aggregate, key, tuple) &&
			    !fold_row(env, aggregate, fold, tuple))
				return;
		}
		return;
	case RELIC_FOLD_LOOKUP:
		index = relic_relation_index_at(aggregate->relation,
		    aggregate->index_position);
		assert(relic_index_key_mask(index) == aggregate->key_mask);
		for (id = relic_index_first(index, aggregate->relation, key);
		    id != RELIC_ID_NONE && id < view->end;
		    id = relic_index_next(index, id)) {
			charge_unresumable_read(scratch);
			scratch->counters.tuples_looked_up++;
			tuple = relic_relation_tuple(aggregate->relation, id);
			if (!fold_row(env, aggregate, fold, tuple))
				return;
		}
		return;
	case RELIC_FOLD_PROBE:
		id = relic_relation_find(aggregate->relation, key);
		if (id == RELIC_ID_NONE || id < view->first || id >= view->end)
			return;
		scratch->counters.tuples_looked_up++;
		tuple = relic_relation_tuple(aggregate->relation, id);
		(void)fold_row(env, aggregate, fold, tuple);
		return;
	case RELIC_FOLD_CARDINALITY:
		fold->rows = view->end > view->first ?
		    view->end - view->first : 0;
		return;
	case RELIC_FOLD_SUMMARY:
		break;
	}
}

/*
 * The value a completed fold gives, as relic_fold_outcome() decides it:
 * the row count for a count, the total for a sum, and the extreme for a
 * min or a max, whose empty group has no value and draws no note.  A group
 * left with no value by its rows' types records the aggregate's note, and
 * a total that does not fit int64_t records the overflow, which ends the
 * plan.
 */
static bool
fold_value(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_aggregate_t *aggregate, const relic_fold_t *fold,
    relic_cell_t *valuep)
{
	int64_t integer;

	switch (relic_fold_outcome(aggregate->fn, &fold->state, fold->rows,
	    &integer)) {
	case RELIC_FOLD_VALUE:
		break;
	case RELIC_FOLD_EMPTY:
		return (false);
	case RELIC_FOLD_MIXED:
		record_aggregate_note(scratch, env, aggregate,
		    aggregate->fn == RELIC_AGG_SUM ? RELIC_NOTE_SUM_TYPE :
		    RELIC_NOTE_EXTREME_TYPES);
		return (false);
	case RELIC_FOLD_OVERFLOW:
		record_error(scratch, &aggregate->where, RELIC_FAIL_OVERFLOW);
		return (false);
	}

	if (aggregate->fn == RELIC_AGG_MIN || aggregate->fn == RELIC_AGG_MAX)
		*valuep = dom_cell(fold->extreme);
	else
		*valuep = number_cell(integer);

	return (true);
}

/*
 * An AGGREGATE: fold the group its key names and bind the result slot to
 * the value, an integer into a number slot, whose frame entry is then the
 * integer's identifier or zero, and an identifier into a domain slot.  When
 * the slot was bound before, the fold's value is compared with the slot's
 * value instead, each read as the cell it is, so that a count of two equals
 * a number slot holding two whether or not the value table holds the
 * integer, and a min over strings equals a text slot holding the same
 * bytes.  A fold with no value rejects the binding.
 */
static bool
step_aggregate(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_aggregate_t *aggregate, const relic_tuple_range_t *view)
{
	relic_fold_t fold;
	relic_cell_t bound;
	relic_cell_t value;
	uint16_t slot;

	bzero(&fold, sizeof(fold));
	relic_fold_init(&fold.state);
	fold_group(scratch, env, aggregate, view, &fold);
	if (!fold_value(scratch, env, aggregate, &fold, &value))
		return (false);

	slot = aggregate->slot;
	if (!aggregate->binds) {
		bound = dom_cell(scratch->frame[slot]);
		switch (aggregate->kind) {
		case RELIC_SLOT_NUMBER:
			bound = number_cell(scratch->numbers[slot]);
			break;
		case RELIC_SLOT_TEXT:
			bound = text_cell(scratch->texts[slot].offset,
			    scratch->texts[slot].length);
			break;
		case RELIC_SLOT_DOMAIN:
			break;
		}
		return (cells_order(scratch, env, value, bound) ==
		    RELIC_ORDER_EQUAL);
	}

	if (aggregate->kind == RELIC_SLOT_DOMAIN) {
		assert(value.kind == RELIC_CELL_DOM);
		scratch->frame[slot] = value.u.dom;
		return (true);
	}

	assert(aggregate->kind == RELIC_SLOT_NUMBER &&
	    value.kind == RELIC_CELL_NUMBER);
	scratch->numbers[slot] = value.u.number;
	scratch->frame[slot] = relic_value_find_integer(env->ctx,
	    value.u.number);

	return (true);
}

/*
 * The bytes the pending strings of the head tuple an EMIT is about to
 * write take: those of its text slots whose strings the context has not
 * assigned.
 */
static size_t
emit_pending_bytes(const relic_scratch_t *scratch, const relic_op_emit_t *emit)
{
	const relic_plan_term_t *term;
	uint16_t column;
	size_t bytes;

	bytes = 0;
	for (column = 0; column < emit->arity; column++) {
		term = &emit->terms[column];
		if (term->action == RELIC_PLAN_TEXT &&
		    scratch->frame[term->u.slot] == 0)
			bytes = relic_saturating_sum(bytes,
			    scratch->texts[term->u.slot].length);
	}

	return (bytes);
}

/*
 * The value an EMIT writes for one column.  A number or text slot whose
 * value the context has assigned writes its identifier.  One whose value
 * it has not appends the value to the scratch's pending values -- a
 * string's bytes to the pending bytes, which emit_head() has made room for
 * -- and writes a pending marker, the value's position there counting from
 * one, which has no type bits and so is no identifier.
 */
static relic_dom_t
emit_column(relic_scratch_t *scratch, const relic_plan_term_t *term)
{
	relic_text_buffer_t *bytes;
	relic_pending_t *pending;
	relic_text_t text;

	if ((term->action != RELIC_PLAN_NUMBER &&
	    term->action != RELIC_PLAN_TEXT) ||
	    scratch->frame[term->u.slot] != 0)
		return (term_value(scratch, term));

	assert(scratch->pending != NULL && scratch->pending_count <
	    (size_t)RELIC_INSERT_BATCH * scratch->arity_room);
	pending = &scratch->pending[scratch->pending_count];
	scratch->pending_count++;

	if (term->action == RELIC_PLAN_NUMBER) {
		pending->kind = RELIC_PENDING_INTEGER;
		pending->u.integer = scratch->numbers[term->u.slot];
		return ((relic_dom_t)scratch->pending_count);
	}

	bytes = &scratch->pending_bytes;
	text = scratch->texts[term->u.slot];
	assert(text.length <= bytes->capacity - bytes->used);
	if (text.length > 0)
		bcopy(scratch->text_stack.bytes + text.offset,
		    bytes->bytes + bytes->used, text.length);

	pending->kind = RELIC_PENDING_TEXT;
	pending->u.text.offset = bytes->used;
	pending->u.text.length = text.length;
	bytes->used += text.length;

	return ((relic_dom_t)scratch->pending_count);
}

/*
 * An EMIT: append the head tuple to the batch.  The caller empties a batch
 * of RELIC_INSERT_BATCH tuples before the executor runs again, so there is
 * always room for the tuple's columns.  Its pending strings are measured
 * before anything is written: when they do not fit what the pending bytes
 * have left, a batch holding tuples is full, and is taken before the EMIT
 * is asked again with the same bindings, and an empty batch's pending bytes
 * grow to hold them, so that a tuple is written once and whole.
 */
static enum relic_emit_end
emit_head(relic_scratch_t *scratch, const relic_op_emit_t *emit)
{
	relic_text_buffer_t *bytes;
	relic_dom_t *tuple;
	uint16_t arity;
	uint16_t column;
	size_t needed;

	arity = emit->arity;
	assert(arity <= scratch->arity_room);
	assert(scratch->batch_count < RELIC_INSERT_BATCH);

	bytes = &scratch->pending_bytes;
	needed = emit_pending_bytes(scratch, emit);
	if (needed > bytes->capacity - bytes->used) {
		if (scratch->batch_count > 0)
			return (RELIC_EMIT_FULL);
		if (!buffer_make_room(scratch, bytes, needed, NULL))
			return (RELIC_EMIT_FAILED);
	}

	tuple = &scratch->batch[scratch->batch_count * arity];
	for (column = 0; column < arity; column++)
		tuple[column] = emit_column(scratch, &emit->terms[column]);
	scratch->batch_count++;

	return (RELIC_EMIT_WRITTEN);
}

/*
 * Ask one generator or test of the running plan for a match: a fresh one,
 * or the next one when resuming.
 *
 * A test, an ASSIGN and an AGGREGATE succeed at most once, so resuming one
 * always finds nothing more.  An EXISTS is its probe, and lets the binding
 * through once when some tuple agrees with it on the columns it reads,
 * however many do.  An ANTI_MEMBER is its probe inverted: the binding
 * survives when no tuple does.  The EMIT is not asked here: the run loop
 * appends its tuple, and treats it as never matching, so nothing below it
 * is ever entered and nothing backtracks into it.
 */
static bool
step(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_op_t *op, relic_op_state_t *state, bool resume)
{
	switch (op->kind) {
	case RELIC_OP_SCAN:
		return (step_scan(scratch, &op->u.atom, state, resume));
	case RELIC_OP_LOOKUP:
		return (step_lookup(scratch, &op->u.atom, state, resume));
	case RELIC_OP_MEMBER:
		if (resume)
			return (false);
		return (step_member(scratch, &op->u.atom, &state->view));
	case RELIC_OP_EXISTS:
		if (resume)
			return (false);
		return (step_probe(scratch, &op->u.atom, &state->view));
	case RELIC_OP_ANTI_MEMBER:
		if (resume)
			return (false);
		return (!step_probe(scratch, &op->u.atom, &state->view));
	case RELIC_OP_FILTER:
		if (resume)
			return (false);
		return (step_filter(scratch, env, &op->u.filter));
	case RELIC_OP_ASSIGN:
		if (resume)
			return (false);
		return (step_assign(scratch, env, &op->u.assign, state));
	case RELIC_OP_AGGREGATE:
		if (resume)
			return (false);
		return (step_aggregate(scratch, env, &op->u.aggregate,
		    &state->view));
	case RELIC_OP_EMIT:
		break;
	}

	return (false);
}

/*
 * The tuples the scratch has counted reading since its counters were last
 * taken.
 */
static uint64_t
tuples_read(const relic_scratch_t *scratch)
{
	return (scratch->counters.tuples_scanned +
	    scratch->counters.tuples_looked_up);
}

void
relic_exec_start(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_plan_t *plan)
{
	start_run(scratch, env, plan, false, NULL);
}

void
relic_exec_start_prefix(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_plan_t *plan)
{
	assert(relic_plan_driver(plan) != RELIC_PLAN_NO_DRIVER);

	start_run(scratch, env, plan, true, NULL);
}

/*
 * A run of the prefix is a run of the plan that ends as it reaches the
 * driver (reach_driver()), so a test is asked exactly as a whole run asks
 * it, and a test that rejects finishes the run without reaching the
 * driver (run_tests()).
 */
enum relic_prefix_status
relic_exec_run_prefix(relic_scratch_t *scratch)
{
	assert(scratch->prefix_only);

	if (relic_exec_run(scratch) == RELIC_EXEC_PAUSED)
		return (RELIC_PREFIX_PAUSED);
	if (scratch->error.failed)
		return (RELIC_PREFIX_FAILED);
	if (scratch->reached)
		return (RELIC_PREFIX_REACHED);

	return (RELIC_PREFIX_REJECTED);
}

/*
 * The run starts at the driver, its cursor the slice (reach_driver()), so
 * its first step reads the slice's first tuple; nothing above it runs, and
 * its view was resolved as every other operation's was, which the slice
 * must lie inside and then becomes.  The caller cut the slice from that
 * view (relic_slice_cut()), and a debug build asserts its range; a
 * LOOKUP's slice start is a tuple of its chain, which the caller found by
 * walking the chain.
 */
void
relic_exec_start_slice(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_plan_t *plan, relic_tuple_range_t slice)
{
	assert(relic_plan_driver(plan) != RELIC_PLAN_NO_DRIVER);

	start_run(scratch, env, plan, false, &slice);
	reach_driver(scratch, true);
}

/*
 * Ready the scratch to run `plan` from its start, whole or, when
 * `prefix_only`, as far as its driver, with the driver's view the `slice`
 * of it when one is given.  The driver's cursor is left for
 * reach_driver() to open.
 */
static void
start_run(relic_scratch_t *scratch, const relic_exec_env_t *env,
    const relic_plan_t *plan, bool prefix_only,
    const relic_tuple_range_t *slice)
{
	relic_op_state_t *state;
	size_t driver;

	assert(plan->op_count <= scratch->op_room);
	assert(scratch->note.where == NULL && !scratch->error.failed);

	scratch->plan = plan;
	scratch->env = env;
	resolve_views(scratch, env, plan);

	scratch->batch_count = 0;
	scratch->pending_count = 0;
	scratch->pending_bytes.used = 0;
	scratch->text_stack.used = 0;
	scratch->level = 0;
	scratch->resume = false;
	scratch->paused = false;
	scratch->finished = false;

	driver = relic_plan_driver(plan);
	scratch->driver = driver;
	bzero(&scratch->driver_reads, sizeof(scratch->driver_reads));
	scratch->reached = false;
	scratch->prefix_only = prefix_only;
	scratch->base = 0;
	if (driver == RELIC_PLAN_NO_DRIVER) {
		assert(slice == NULL);
		return;
	}

	state = &scratch->states[driver];
	if (slice != NULL) {
		assert(state->view.first <= slice->first &&
		    slice->first < slice->end && slice->end <= state->view.end);
		state->view = *slice;
	}
}

/*
 * The join has reached the driver: it goes on from there and never
 * backtracks into the tests above it, which have nothing more.
 *
 * A run of the prefix alone is finished here.  Any other opens the
 * driver's cursor here, and so never before its prefix has held, at the
 * start of its walk of its view.  For a SCAN, and for a `sliced` view,
 * whose first identifier its caller found on the chain, that is the view's
 * first identifier.  For a LOOKUP's whole view it is the first tuple of its
 * key's chain in the view (lookup_start()).
 *
 * The driver is then asked to resume from the cursor, which reads the
 * tuple there first.  The join never asks the operation at `base` afresh,
 * so the cursor walks the view once, and the driver's visits are counted
 * from where it opened, `driver_from` (count_visits()).
 */
static void
reach_driver(relic_scratch_t *scratch, bool sliced)
{
	const relic_op_atom_t *atom;
	relic_op_state_t *state;
	size_t driver;

	driver = scratch->driver;
	scratch->reached = true;
	scratch->base = driver;
	scratch->level = driver;
	if (scratch->prefix_only) {
		scratch->finished = true;
		return;
	}

	state = &scratch->states[driver];
	atom = &scratch->plan->ops[driver].u.atom;
	state->cursor = state->view;
	if (scratch->plan->ops[driver].kind == RELIC_OP_LOOKUP && !sliced)
		state->cursor.first = lookup_start(scratch, atom,
		    lookup_index(atom), state->view.first);
	scratch->driver_from = state->cursor.first;
	scratch->resume = true;
}

/*
 * Finish the run: the plan has derived everything it will.
 */
static enum relic_exec_status
end_run(relic_scratch_t *scratch)
{
	scratch->finished = true;

	return (RELIC_EXEC_FINISHED);
}

/*
 * Count the tuples the driver has read, matched or not, since they were
 * last counted: those its cursor has moved past from `driver_from`, which
 * then moves up to the cursor.  For a SCAN they are the identifiers
 * between.  For a LOOKUP they are the chain tuples between, found by
 * following the chain's links again, which reads no tuple and follows no
 * more links than the call's own steps did.  The walk always meets the
 * cursor, the end of the view bounding it only in defence, since the
 * driver reads nothing past the view.  They are counted here rather than
 * as the driver steps so that no lookup's step pays for telling the driver
 * from the lookups after it, and as every call returns so that
 * `driver_reads` holds the run's visits whenever its caller looks, however
 * the run comes to end.
 */
static void
count_visits(relic_scratch_t *scratch)
{
	const relic_op_state_t *state;
	const relic_index_t *index;
	const relic_op_t *op;
	relic_tuple_id_t id;

	assert(scratch->reached && !scratch->prefix_only);
	op = &scratch->plan->ops[scratch->driver];
	state = &scratch->states[scratch->driver];
	if (op->kind == RELIC_OP_SCAN) {
		scratch->driver_reads.visits +=
		    state->cursor.first - scratch->driver_from;
	} else {
		assert(op->kind == RELIC_OP_LOOKUP);
		index = lookup_index(&op->u.atom);
		id = scratch->driver_from;
		while (id != RELIC_ID_NONE && id < state->view.end &&
		    id != state->cursor.first) {
			scratch->driver_reads.visits++;
			id = relic_index_next(index, id);
		}
	}
	scratch->driver_from = state->cursor.first;
}

/*
 * Record where the join goes on from once the EMIT at `level` has filled
 * the batch, which is where it would have gone had the batch had room: back
 * to the operation before the EMIT, for its next match.  An EMIT at the
 * lowest level the join runs at, which only the first operation of a plan
 * with no driver can be, has no operation to go back to, and the plan is
 * finished.  No rule is planned that way, since every body has a literal,
 * but the executor does not rely on it.
 */
static enum relic_exec_status
yield_after_emit(relic_scratch_t *scratch, size_t level)
{
	if (level == scratch->base)
		return (end_run(scratch));

	scratch->level = level - 1;
	scratch->resume = true;

	return (RELIC_EXEC_PAUSED);
}

/*
 * Record that the EMIT at `level` found the batch too full for its tuple,
 * which it has not written: the join goes on by asking it again, with the
 * same bindings, once the batch is taken.
 */
static enum relic_exec_status
yield_before_emit(relic_scratch_t *scratch, size_t level)
{
	scratch->level = level;
	scratch->resume = false;

	return (RELIC_EXEC_PAUSED);
}

/*
 * Record that a quantum of work is done with the join about to ask the
 * operation at `level`, afresh or for its next match as `resume` says.
 * That pair is all the loop of run_join() carries from one turn to
 * the next -- the frame and every operation's cursor are already in the
 * scratch -- so the join goes on from it exactly as it would have.
 */
static enum relic_exec_status
yield_after_quantum(relic_scratch_t *scratch, size_t level, bool resume)
{
	scratch->level = level;
	scratch->resume = resume;

	return (RELIC_EXEC_PAUSED);
}

/*
 * Run the plan from where it last stopped until the batch is full, a
 * quantum of work is done, or the plan is finished.  A plan with a driver
 * runs its prefix first (run_tests()), and the join from the driver on
 * once every test has held; any other plan is all join (run_join()).
 *
 * A quantum is counted two ways, each up to RELIC_EXEC_QUANTUM, so that
 * neither a join of many cheap bindings nor one whose scans reject relation
 * after relation keeps the caller waiting long.  The loop counts its
 * backtracks: every turn either descends, which it can do only as many
 * times as the plan has operations, or ends in one, so the backtracks bound
 * the turns.  The scans and lookups count the tuples they reject, in
 * `reads_left`, and one that runs out mid-way stops inside its step and
 * sets `paused`.  A step with no cursor to resume from charges the tuples
 * it reads to the same count through charge_unresumable_read() and sets
 * `paused` once it has spent them, but finishes its walk.  Every other step
 * does a bounded amount of work.  The run pauses after a step that set
 * `paused`: at the next operation when the step matched, which only a step
 * with no cursor can have, and otherwise at the step's own operation, to
 * be asked for its next match; a test of the prefix that rejects, which
 * has no next match, finishes the run instead (run_tests()).  So a call
 * overruns its reads by at most one such step's walk.  A call backtracks
 * or reads at least once before pausing, so every call makes progress.
 *
 * A batch left full by the caller is reported again rather than appended
 * to: the EMIT writes a tuple only into room the batch has, so the caller's
 * neglect costs it a turn and cannot write past the batch.  An operation
 * that records a failure ends the run there, and the plan is finished.
 */
enum relic_exec_status
relic_exec_run(relic_scratch_t *scratch)
{
	enum relic_exec_status status;

	if (scratch->finished)
		return (RELIC_EXEC_FINISHED);
	if (scratch->batch_count == RELIC_INSERT_BATCH)
		return (RELIC_EXEC_PAUSED);

	scratch->reads_left = RELIC_EXEC_QUANTUM;
	if (!scratch->reached && scratch->driver != RELIC_PLAN_NO_DRIVER)
		status = run_tests(scratch);
	else
		status = run_join(scratch);
	if (scratch->reached && !scratch->prefix_only)
		count_visits(scratch);

	return (status);
}

/*
 * Ask the tests of the prefix, from `level` to the driver, each afresh and
 * in order, as the join would descend through them, and count what they
 * read as the prefix's.  Every test binds nothing and holds at most once,
 * so one that rejects ends the run, as backtracking through the tests above
 * it, which have nothing more, would, and in the same call even when it
 * spent the call's reads.  A test that spends them and holds pauses the
 * run at the next one, the driver included, so that the driver's cursor,
 * whose opening may seek a delta, is opened in a call with its reads
 * whole.  Once every test has held, the join has reached the driver
 * (reach_driver()) and goes on from it in the same call.  So a whole run
 * reaches its driver here alone, and the prefix's reads are what this
 * function reads, which each call counts for itself, the counters being
 * the caller's to take between calls.
 */
static enum relic_exec_status
run_tests(relic_scratch_t *scratch)
{
	const relic_op_t *op;
	uint64_t before;
	bool held;
	bool paused;

	before = tuples_read(scratch);
	held = true;
	paused = false;
	while (held && !paused && scratch->level < scratch->driver) {
		op = &scratch->plan->ops[scratch->level];
		held = test_holds(scratch, op,
		    &scratch->states[scratch->level]);
		if (scratch->error.failed)
			held = false;
		scratch->level++;
		paused = scratch->paused;
		scratch->paused = false;
	}
	scratch->driver_reads.prefix += tuples_read(scratch) - before;

	if (!held)
		return (end_run(scratch));
	if (paused)
		return (RELIC_EXEC_PAUSED);
	reach_driver(scratch, false);
	if (scratch->finished)
		return (RELIC_EXEC_FINISHED);

	return (run_join(scratch));
}

/*
 * Whether a test of the prefix holds, asked afresh, as step() would ask
 * it.  It is asked here rather than through step() so that the join's
 * loop stays step()'s one caller, where the compiler expands it in place.
 */
static bool
test_holds(relic_scratch_t *scratch, const relic_op_t *op,
    const relic_op_state_t *state)
{
	switch (op->kind) {
	case RELIC_OP_MEMBER:
		return (step_member(scratch, &op->u.atom, &state->view));
	case RELIC_OP_EXISTS:
		return (step_probe(scratch, &op->u.atom, &state->view));
	case RELIC_OP_ANTI_MEMBER:
		return (!step_probe(scratch, &op->u.atom, &state->view));
	case RELIC_OP_FILTER:
		return (step_filter(scratch, scratch->env, &op->u.filter));
	case RELIC_OP_SCAN:
	case RELIC_OP_LOOKUP:
	case RELIC_OP_ASSIGN:
	case RELIC_OP_AGGREGATE:
	case RELIC_OP_EMIT:
		break;
	}
	assert(!"a prefix holds only tests");

	return (false);
}

/*
 * Run the join from where it last stopped until the batch is full, a
 * quantum of work is done, or the operation at the lowest level it runs
 * at, `base`, has nothing more: the driver for a plan with one, whose
 * prefix has held, and the first operation for any other.  `level` is the
 * operation being asked for a match, and `resume` says whether it is being
 * asked for its next one or started afresh.  Every yield records a level
 * at or above `base` and leaves every cursor where it was, and every
 * operation the join asks afresh is above the one it last matched, so a
 * driver, which reach_driver() leaves to resume, goes on from its own
 * cursor however often the run returns.
 */
static enum relic_exec_status
run_join(relic_scratch_t *scratch)
{
	const relic_exec_env_t *env;
	const relic_plan_t *plan;
	const relic_op_t *op;
	uint64_t backtracks_left;
	size_t level;
	size_t base;
	bool matched;
	bool resume;

	plan = scratch->plan;
	env = scratch->env;
	base = scratch->base;

	level = scratch->level;
	resume = scratch->resume;
	backtracks_left = RELIC_EXEC_QUANTUM;

	for (;;) {
		op = &plan->ops[level];

		if (op->kind == RELIC_OP_EMIT) {
			switch (emit_head(scratch, &op->u.emit)) {
			case RELIC_EMIT_WRITTEN:
				break;
			case RELIC_EMIT_FULL:
				return (yield_before_emit(scratch, level));
			case RELIC_EMIT_FAILED:
				return (end_run(scratch));
			}
			if (scratch->batch_count == RELIC_INSERT_BATCH)
				return (yield_after_emit(scratch, level));
			matched = false;
		} else {
			matched = step(scratch, env, op,
			    &scratch->states[level], resume);
			if (scratch->error.failed)
				break;
			if (scratch->paused) {
				scratch->paused = false;
				if (matched)
					return (yield_after_quantum(scratch,
					    level + 1, false));
				return (yield_after_quantum(scratch, level,
				    true));
			}
		}

		if (matched) {
			level++;
			resume = false;
			continue;
		}

		if (level == base)
			break;

		level--;
		resume = true;

		backtracks_left--;
		if (backtracks_left == 0)
			return (yield_after_quantum(scratch, level, resume));
	}

	return (end_run(scratch));
}
