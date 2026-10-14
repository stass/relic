/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_EXEC_H
#define RELIC_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <relic.h>

#include "arith.h"
#include "frozen.h"
#include "plan.h"
#include "program.h"
#include "strfn.h"
#include "util.h"

/*
 * The executor (lib/exec.c): one plan run, a call at a time, over relations
 * that nothing modifies during a call.
 *
 * Everything the executor changes is in one scratch, which its caller owns
 * and hands it: the frame of slot values, one state per operation of the
 * plan, and the batch of head tuples the EMIT derives.  Everything else it
 * reads -- the context, the deltas, every relation and index -- it reaches
 * as frozen.h describes, through types it cannot see inside, so a write to
 * any of them from the executor does not compile.  The one word it stores
 * to outside its scratch is a key's frontier, which relic_index_first_since()
 * moves.
 *
 * The executor never inserts.  It fills the batch and returns when the batch
 * is full, having saved where it was, and the caller decides where the
 * batch goes -- inserted into the relation the caller names, delivered to a
 * caller as rows, or kept aside -- and empties it before resuming.  So one
 * executor serves every way a plan's derivations are used.  It fails only by
 * recording that it did: an expression whose result is not defined, and a
 * buffer of the scratch that could not grow, record an error in the scratch
 * and end the plan, and the caller reports it when it takes the batch.
 *
 * Between two calls the caller may insert into the relations, the plan's
 * own among them, which appends tuples and may move the storage holding
 * them.  It may not remove, clear or discard anything while a plan is
 * unfinished.  A plan keeps tuple identifiers and places on index chains
 * from one call to the next, and finds each tuple and each index afresh by
 * identifier and position, so an append is invisible to it -- every tuple
 * appended lies past the views it resolved when it started -- and a removal
 * would leave it holding identifiers that name nothing.
 */

/*
 * How many tuples the executor's batch holds, and so how many tuples a plan
 * derives before its caller takes them, and how many
 * relic_relation_insert_batch() prefetches for at a time.  Measured on the
 * closure workloads: 16 overlaps most of the misses of a duplicate check, 64
 * a little more, and 256 no more than 64.
 */
#define	RELIC_INSERT_BATCH	64u

/*
 * How much work relic_exec_run() does before it returns to its caller
 * whether or not its batch is full: as many backtracks of its join, and as
 * many tuples its scans and lookups reject.  It bounds how long a join that
 * derives nothing goes without its caller looking for an interrupt.  A
 * backtrack or a rejected tuple costs a few nanoseconds, so a quantum takes
 * well under a millisecond, while taking a batch, often an empty one, once
 * a quantum costs nothing measurable.
 */
#define	RELIC_EXEC_QUANTUM	16384u

/*
 * The room the scratch's text stack and pending bytes start with in a
 * round, each grown past it only for data that needs more: a text stack of
 * a few pages holds the strings an ordinary rule computes, and the pending
 * bytes of a megabyte a batch of their tuples.
 */
#define	RELIC_TEXT_STACK_INITIAL	4096u
#define	RELIC_PENDING_BYTES		(1024u * 1024u)

typedef struct relic_driver_reads relic_driver_reads_t;
typedef struct relic_exec_counters relic_exec_counters_t;
typedef struct relic_exec_env relic_exec_env_t;
typedef struct relic_exec_error relic_exec_error_t;
typedef struct relic_match_space relic_match_space_t;
typedef struct relic_mixed_note relic_mixed_note_t;
typedef struct relic_op_state relic_op_state_t;
typedef struct relic_pending relic_pending_t;
typedef struct relic_scratch relic_scratch_t;
typedef struct relic_text relic_text_t;
typedef struct relic_text_capacity relic_text_capacity_t;
typedef struct relic_text_buffer relic_text_buffer_t;

/*
 * The capacities, in bytes, of the four buffers of a scratch that the data
 * a plan meets sizes: the text stack, the pending bytes, the room for a
 * pattern's program and the matcher's work space.
 */
struct relic_text_capacity {
	size_t	text_stack;
	size_t	pending_bytes;
	size_t	program;
	size_t	work;
};

/*
 * Bytes in one of the scratch's byte buffers -- a computed string in the
 * text stack, or a pending string in the pending bytes -- named by where
 * they begin and how many there are, since the buffer may move when it
 * grows.
 */
struct relic_text {
	size_t	offset;
	size_t	length;
};

/*
 * A byte buffer of the scratch, `used` of its `capacity` bytes in use.  It
 * belongs to the round it was made for, which charges its capacity to the
 * memory account as a round's buffer and gives the charge back when it
 * frees it.
 */
struct relic_text_buffer {
	char	*bytes;
	size_t	 used;
	size_t	 capacity;
};

/*
 * What a running plan reads besides its relations: the context, for
 * comparing values; the deltas the views of a member are resolved from,
 * indexed by predicate identifier minus one, or NULL for an evaluation that
 * runs no fixed point and whose every view is therefore `all`; and whether
 * to record a comparison that failed for ordering values of different types,
 * which RELIC_F_NOTE_MIXED asks for.
 */
struct relic_exec_env {
	const relic_t			*ctx;
	const relic_tuple_range_t	*deltas;
	bool				 note_mixed;
};

/*
 * The executor's state for one operation of the running plan.  `view` is the
 * range of identifiers the operation's view covers, resolved for every
 * operation that reads a relation when the plan starts.  `cursor` is the
 * range a SCAN has still to visit, copied from the view each time the scan
 * starts.
 *
 * A LOOKUP uses its cursor differently.  `first` is the next tuple on the
 * index chain, or RELIC_ID_NONE when the chain is exhausted, and `end` is the
 * view's end, copied when the lookup starts.  A chain is in identifier
 * order, so the lookup stops at the first tuple at or past `end`, exactly as
 * a scan stops at the end of its range.
 *
 * A test keeps no cursor, and a FILTER, an ASSIGN and the EMIT read no
 * relation and leave the view unset too.  An AGGREGATE reads its group
 * relation through the view `all`, resolved like the others.
 *
 * `text_top` is set by an ASSIGN that binds a text slot, each time it
 * succeeds: the end of its string in the text stack, above which the
 * operations after it build theirs (plan.h).
 */
struct relic_op_state {
	relic_tuple_range_t	view;
	relic_tuple_range_t	cursor;
	size_t			text_top;
};

/*
 * The work the executor has counted since its caller last took the counts:
 * the tuples its scans and its lookups read.  Each is defined where
 * relic_stats_t documents the counter of the same name.
 */
struct relic_exec_counters {
	uint64_t	tuples_scanned;
	uint64_t	tuples_looked_up;
};

/*
 * Add the work one count holds to another.
 */
static inline void
relic_exec_counters_add(relic_exec_counters_t *total,
    const relic_exec_counters_t *more)
{
	total->tuples_scanned += more->tuples_scanned;
	total->tuples_looked_up += more->tuples_looked_up;
}

/*
 * The work plans did, in the one unit a round's estimate and a plan's
 * history are measured in: the tuples they read and the tuples they
 * derived.
 */
static inline uint64_t
relic_exec_work(uint64_t derivations, const relic_exec_counters_t *read)
{
	return (derivations + read->tuples_scanned + read->tuples_looked_up);
}

/*
 * What the running plan has read at its driver (plan.h) and before it since
 * it started, as of the executor's last return: `prefix`, the tuples the
 * tests before the driver read, and `visits`, the tuples the driver read,
 * matched or not.  A plan with no driver counts neither.  Both are reads
 * the scratch's counters count as well, picked out rather than added to
 * them, and a round predicts the plan's next run from them (lib/round.c).
 */
struct relic_driver_reads {
	uint64_t	prefix;
	uint64_t	visits;
};

/*
 * What made a comparison, an assignment or an aggregate false only because
 * of a value's type: an ordering comparison of two values of different
 * types, an operand of an arithmetic operator that is not an integer, a
 * row of a sum that is not an integer, or the rows of a min or a max
 * holding values of more than one type.
 */
enum relic_note_kind {
	RELIC_NOTE_ORDERING,
	RELIC_NOTE_OPERAND,
	RELIC_NOTE_SUM_TYPE,
	RELIC_NOTE_EXTREME_TYPES
};

/*
 * The first comparison, assignment or aggregate of the running plan that was
 * false only because of a value's type, when the environment asks for such,
 * since the caller last took the record.  `where` is the literal's location,
 * NULL when there has been none.  An ordering names the types of the two
 * values it compared, as the type bits of a domain identifier; an operand
 * names its operator's spelling and its own type; an aggregate's note names
 * the aggregate, whose function and aggregated variable it is about, and
 * no type, since which row a fold meets first is not a fact about the
 * group.
 *
 * The executor records and never reports, since reporting calls the
 * caller's diagnostic callback and marks the rule as reported, both of which
 * change the context; the caller reports the record when it takes the
 * batch, which is before any tuple derived after the comparison can have
 * been inserted or delivered.  Only the first is kept, because a rule is
 * reported on at most once and every plan running here is of one rule.
 */
struct relic_mixed_note {
	const relic_location_t	*where;
	const relic_aggregate_t	*aggregate;	/* SUM_TYPE, EXTREME_TYPES */
	const char		*operator;	/* OPERAND */
	relic_dom_t		 left_type;	/* ORDERING, and OPERAND's */
	relic_dom_t		 right_type;	/* ORDERING */
	enum relic_note_kind	 kind;
};

/*
 * Why a binding stopped the plan: an integer result that does not fit, a
 * division by zero, a string result longer than a value can be, a pattern
 * that is not a regular expression, or a buffer of the scratch that could
 * not grow, because the memory limit refused it or because the allocator
 * did.
 */
enum relic_exec_failure {
	RELIC_FAIL_OVERFLOW,
	RELIC_FAIL_ZERO_DIVISOR,
	RELIC_FAIL_TOO_LONG,
	RELIC_FAIL_PATTERN,
	RELIC_FAIL_LIMIT,
	RELIC_FAIL_NO_MEMORY
};

/*
 * The first failure the running plan met, when `failed` says there has
 * been one: `failure` says what it was, `pattern` why a pattern was not a
 * regular expression when that was the failure, and `where` is the
 * location of the literal being evaluated, or NULL for a buffer the EMIT
 * could not grow, which is no literal's.  Recording one ends the plan,
 * which is left finished; the caller reports it when it takes the batch,
 * discards the batch and fails the evaluation.  It is kept apart from the
 * note, which records nothing that fails anything.
 */
struct relic_exec_error {
	const relic_location_t		*where;
	enum relic_exec_failure		 failure;
	enum relic_regex_failure	 pattern;	/* PATTERN */
	bool				 failed;
};

/*
 * What a match needs beside its program: room for the program of a pattern
 * compiled as the call runs, `capacity` instructions, and the matcher's
 * work space.  Both are the round's, as the text stack is, charged when
 * they grow and given back with it.  The program's room keeps nothing from
 * one call to the next; the work space keeps its marks and stamp, which is
 * what lets a match begin without clearing them.
 */
struct relic_match_space {
	relic_regex_inst_t	*insts;
	size_t			 capacity;
	relic_regex_work_t	 work;
};

/*
 * A value an EMIT derived that the context had not assigned an identifier
 * when it was computed: an integer, or a string whose bytes are `text` in
 * the batch's pending bytes.  A batch import's values are pending too, and
 * may also be symbols, whose bytes, `text` as a string's, it has checked
 * are a symbol's.
 */
enum relic_pending_kind {
	RELIC_PENDING_INTEGER,
	RELIC_PENDING_TEXT,
	RELIC_PENDING_SYMBOL
};

struct relic_pending {
	union {
		int64_t		integer;	/* INTEGER */
		relic_text_t	text;		/* TEXT, SYMBOL */
	}			u;
	enum relic_pending_kind	kind;
};

/*
 * Pending values interned together (internal.h): `count` of them, the
 * bytes of each symbol or string at its offset in `bytes`, and the hash of
 * each, relic_pending_hash(), which is what the value table or the pool
 * finds it by: an integer's by its value, a symbol's or a string's by its
 * bytes.  relic_pending_hash() hashes one value alone, reading a symbol's
 * or a string's bytes at `bytes` plus its offset, so that a caller can
 * hash the values before it has made a batch of them.
 *
 * relic_pending_find() is the reading half of numbering a batch's values;
 * relic_pending_number() (internal.h) completes it.  The members it reads
 * are the `member_count` positions in `members`, in increasing order, or
 * every value of the batch when `members` is NULL.  For each it writes to
 * `ids` the identifier the context has assigned the value, or, when it
 * has none, a marker: the position, plus one, of the earliest member equal
 * to it.  A marker has no type bits, so no identifier is one.  A batch of
 * a new symbol a, a again, and a symbol b the context holds is given 1, 1
 * and b's identifier, and numbering then gives both a's one new
 * identifier.
 *
 * Values are equal when they are integers of one value, or symbols or
 * strings of the same bytes, whatever their types.  `slots` is the table of
 * the members met so far: `slot_count` zeroed slots, a power of two at
 * least relic_pending_slots(member_count).
 *
 * It reads the context and writes nothing but `slots` and the members'
 * `ids`.  A batch's values can therefore be divided among parts that run
 * at once, each with slots of its own, provided equal values are members
 * of one part, as dividing them by their hashes makes them.
 */
typedef struct relic_pending_batch relic_pending_batch_t;

struct relic_pending_batch {
	const relic_pending_t	*values;
	const uint32_t		*hashes;
	const char		*bytes;
	size_t			 count;
};

uint32_t relic_pending_hash(const relic_t *ctx, const relic_pending_t *value,
	    const char *bytes);
size_t	 relic_pending_slots(size_t count);
void	 relic_pending_find(const relic_t *ctx,
	    const relic_pending_batch_t *batch, const uint32_t *members,
	    size_t member_count, uint32_t *slots, size_t slot_count,
	    relic_dom_t *ids);

/*
 * Everything the executor writes while it runs one plan.
 *
 * `plan` and `env` are the plan being run and what it reads, set when the
 * plan starts, so that every call that continues it continues the same one.
 * They are meaningful until the plan finishes, after which they may point
 * at what the caller has released, and the next start sets them afresh.
 *
 * `frame` holds one value per slot.  A slot is written by the operation that
 * binds it and read only by operations after that one, so a new binding
 * simply overwrites the old: nothing is saved on descending and nothing
 * restored on backtracking, because no operation reads a slot a later one
 * binds.  A number slot (plan.h) holds its integer in `numbers` beside its
 * frame entry, and a text slot names its bytes in the text stack in
 * `texts`; the frame entry of either is the value's identifier or zero.
 *
 * `text_stack` holds every string the running plan has computed and may
 * still read: the strings of the text slots bound, and above them the
 * temporaries of the operation running (plan.h).
 *
 * `pending` holds the values of the head tuples in the batch that the
 * context had not assigned when they were computed, `pending_count` of
 * them, with room for one per column of the batch, and `pending_bytes` the
 * bytes of those that are strings.  Such a column holds a pending marker,
 * the value's position in `pending` counting from one, with no type bits,
 * which no identifier is; the caller resolves every marker before the
 * batch goes anywhere.
 *
 * The text stack and the pending bytes are the round's (relic_text_buffer_t):
 * a plan that calls a function runs only on a scratch that has them, and
 * one that does not never reads them.  `match` is the round's too, and
 * starts empty.  `account` is what the growth of all three is charged to.
 *
 * `states` has room for the operations of a plan of `op_room` operations,
 * one state each, so that the executor's level indexes a plan's operations
 * and their states alike.
 *
 * `batch` holds `batch_count` head tuples the EMIT has derived and the
 * caller has not yet taken, packed at the arity of the relation the EMIT
 * names, with room for RELIC_INSERT_BATCH tuples of `arity_room` columns.
 *
 * `level` and `resume` are where the join was when the executor paused:
 * the operation to ask next and whether to ask it for its next match
 * rather than a first.
 *
 * A plan with a driver, at `driver`, runs in two phases.  Its prefix's
 * tests are asked first, in order, from `level`; once every one has held,
 * the join has reached the driver, `reached`, and runs from it, `base`,
 * the lowest level it runs at, never backtracking into the tests, which
 * have nothing more.  A plan with no driver is all join, from `base`
 * zero.  The plan is finished once the operation at `base` has nothing
 * more, or a test rejects, and a run of the prefix alone, `prefix_only`,
 * once it reaches the driver.
 *
 * `driver_reads` is taken from the counters and the driver's cursor, not
 * counted by the steps.  The prefix's reads are what the counters read
 * while the tests are asked, which each call adds for itself, so that the
 * caller may take the counters between calls.  The driver reads one tuple
 * of its view, the slice in a slice, for each place its cursor moves past,
 * so its visits are taken from the cursor as every call returns, counting
 * from `driver_from`, where the cursor was at the previous return.
 *
 * `reads_left` is how many more tuples the scans and lookups may reject,
 * and the folds read, in the call under way, and `paused` is set by a step
 * that has spent them: a scan or lookup that stopped, its cursor where the
 * next call resumes, or a fold that read its whole walk regardless.  A
 * batch is full when it holds RELIC_INSERT_BATCH tuples, or when the next
 * tuple's pending strings would not fit its pending bytes, and then the
 * EMIT is asked again.  `finished` is set once the plan has derived
 * everything it will; a scratch no plan has started on is finished too,
 * so that running it derives nothing.
 *
 * `counters`, `note` and `error` are what the plans run on the scratch have
 * counted and met since the caller last took them.  The counters accumulate
 * from one plan to the next until they are taken; the note and the error
 * are taken before another plan starts, since each points into the plan
 * that met it.
 *
 * The scratch belongs to one thread for as long as a plan runs on it.
 */
struct relic_scratch {
	const relic_plan_t	*plan;
	const relic_exec_env_t	*env;
	relic_dom_t		 frame[RELIC_MAX_VARIABLES];
	int64_t			 numbers[RELIC_MAX_VARIABLES];
	relic_text_t		 texts[RELIC_MAX_VARIABLES];
	relic_op_state_t	*states;
	relic_dom_t		*batch;
	relic_pending_t		*pending;
	relic_text_buffer_t	 text_stack;
	relic_text_buffer_t	 pending_bytes;
	relic_match_space_t	 match;
	relic_account_t		*account;
	size_t			 op_room;
	size_t			 batch_count;
	size_t			 pending_count;
	size_t			 level;
	size_t			 base;
	size_t			 driver;
	relic_tuple_id_t	 driver_from;
	uint64_t		 reads_left;
	relic_exec_counters_t	 counters;
	relic_driver_reads_t	 driver_reads;
	relic_mixed_note_t	 note;
	relic_exec_error_t	 error;
	uint16_t		 arity_room;
	bool			 resume;
	bool			 paused;
	bool			 finished;
	bool			 reached;
	bool			 prefix_only;
};

/*
 * Why relic_exec_run() returned: the plan has more to derive and paused,
 * because the batch is full or a quantum of work is done, or the plan has
 * derived everything it will.  Either way the batch may hold tuples, which
 * the caller takes before anything else.
 */
enum relic_exec_status {
	RELIC_EXEC_PAUSED,
	RELIC_EXEC_FINISHED
};

/*
 * Why relic_exec_run_prefix() returned: a quantum of work is done and the
 * prefix has more tests to run, a test rejected the empty binding, so the
 * plan derives nothing, every test held, so the driver is reached, or a
 * test recorded a failure.
 */
enum relic_prefix_status {
	RELIC_PREFIX_PAUSED,
	RELIC_PREFIX_REJECTED,
	RELIC_PREFIX_REACHED,
	RELIC_PREFIX_FAILED
};

/*
 * relic_scratch_init() gives a scratch the storage it runs in: room for the
 * states of a plan of `op_room` operations, for a batch of
 * RELIC_INSERT_BATCH tuples of `arity_room` columns, and for as many pending
 * values as the batch has columns, all owned by the caller.  The scratch
 * starts empty and finished, and holds no round's buffers.
 *
 * relic_scratch_begin_texts() gives the scratch a round's text stack and
 * pending bytes, at their initial sizes, and an empty match space, charged
 * to `account` as a round's buffers, and returns 0, or -1 having given it
 * nothing and set *failurep to say whether the limit or the allocator
 * refused them.  relic_scratch_end_texts() frees all three and gives back
 * their charge, whatever they had grown to, and may be called on a scratch
 * that holds none.
 *
 * relic_text_capacity_raise() raises each capacity `peaks` records, in
 * bytes, to that of the scratch's buffer, and relic_text_capacity_bytes()
 * is what they come to together.  The record starts zeroed.  Since each of
 * the four buffers doubles until it fits, the peak over the scratches of a
 * round's workers is at least what one scratch would have held had it run
 * the whole round: as much for the text stack and the match space, and as
 * much or more for the pending bytes, which carry strings the serial run
 * would have interned earlier in the round.
 *
 * relic_exec_start() readies the scratch to run `plan` from the beginning,
 * over what `env` gives it, resolving the view of every operation that
 * reads a relation, with an empty batch.  The plan must fit the scratch,
 * and `plan` and `env` must stay valid until the plan has finished or is
 * abandoned.  relic_exec_run() then runs it, from where it last returned,
 * until the batch is full, RELIC_EXEC_QUANTUM of work is done or the plan
 * has finished, and says whether it paused or finished; a caller that runs
 * it again without emptying a full batch is told it paused again, and
 * nothing is derived.  Between two calls the relations may be
 * appended to and nothing else, as the executor's description above says.
 *
 * relic_exec_view() gives the range of identifiers an operation's view
 * covers under `env`.
 *
 * A plan with a driver (plan.h) can be run in two parts, the way a round
 * divides it among tasks (round.h).
 * relic_exec_start_prefix() readies the scratch to run only the plan's
 * prefix, the tests before its driver, as relic_exec_start() readies it to
 * run the whole plan, and relic_exec_run_prefix() runs those tests, a
 * quantum at a time, through the steps a whole run takes.  It derives
 * nothing and never enters the driver; it counts what the tests read and
 * records their note or failure as a whole run would, and leaves the
 * scratch finished once it has rejected, reached the driver or failed.
 *
 * relic_exec_start_slice() readies the scratch to run the plan from its
 * driver, as though the prefix had just reached it, with the driver's
 * cursor at `slice`: for a SCAN a range of its view, and for a LOOKUP a
 * tuple on its key's chain inside its view and the fence the lookup stops
 * before, at or before the view's end.  The slice must not be empty.  The
 * driver is then asked for its next match from the cursor, and the plan
 * finishes when the driver has nothing more, never backtracking above it.
 * Every operation below the driver reads only the binding it makes and its
 * own view, so the slice derives exactly what the whole plan derives while
 * the driver's cursor is inside the slice, in the same order, and a driver's
 * visits cut into slices run one after another derive what the whole plan
 * does, the prefix's reads excepted.
 */
void	 relic_scratch_init(relic_scratch_t *scratch, relic_op_state_t *states,
	    size_t op_room, relic_dom_t *batch, relic_pending_t *pending,
	    uint16_t arity_room);
int	 relic_scratch_begin_texts(relic_scratch_t *scratch,
	    relic_account_t *account, enum relic_exec_failure *failurep);
void	 relic_scratch_end_texts(relic_scratch_t *scratch);
void	 relic_text_capacity_raise(relic_text_capacity_t *peaks,
	    const relic_scratch_t *scratch);
size_t	 relic_text_capacity_bytes(const relic_text_capacity_t *peaks);
void	 relic_exec_start(relic_scratch_t *scratch,
	    const relic_exec_env_t *env, const relic_plan_t *plan);
enum relic_exec_status relic_exec_run(relic_scratch_t *scratch);
relic_tuple_range_t relic_exec_view(const relic_exec_env_t *env,
	    const relic_op_atom_t *atom);
void	 relic_exec_start_prefix(relic_scratch_t *scratch,
	    const relic_exec_env_t *env, const relic_plan_t *plan);
enum relic_prefix_status relic_exec_run_prefix(relic_scratch_t *scratch);
void	 relic_exec_start_slice(relic_scratch_t *scratch,
	    const relic_exec_env_t *env, const relic_plan_t *plan,
	    relic_tuple_range_t slice);

#endif /* RELIC_EXEC_H */
