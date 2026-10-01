/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "cdefs.h"
#include "regex.h"

/*
 * Regular expressions, as regex.h describes: a recursive-descent compiler
 * to a Thompson program, and Thompson's simulation of the program to find
 * where the leftmost match begins.
 *
 * The compiler emits each construct as it is parsed:
 *
 *	a byte, `.`, a set, `^`, `$`	one BYTE, ANY, SET, BOL or EOL
 *	x*				SPLIT(x, out); x; JUMP back to the SPLIT
 *	x+				x; SPLIT(back to x, out)
 *	x?				SPLIT(x, out); x
 *	x|y				SPLIT(x, y); x; JUMP out; y
 *
 * so no construct emits more than three instructions per byte of the
 * pattern: a byte one, a repetition and its operand at most three for two
 * bytes, a `|` two.  A run of repetitions is one repetition -- `x**` is
 * `x*`, and `x+?` and `x?+` are `x*` too -- so the run of `*`, `+` and `?`
 * after an operand is read whole and emitted once.
 *
 * A SPLIT that goes before its operand is not known to be needed until the
 * operand has been emitted, so it is inserted at the operand's start,
 * moving the operand up by one.  The operand is the last thing emitted, and
 * nothing outside it jumps into it except to its start, which the inserted
 * instruction now holds, so every target the move must correct lies within
 * the moved code and points at or past the start.  An insertion moves at
 * most the operand, and each operand is moved at most once per level of
 * parentheses around it, so compiling costs at most the depth bound times
 * the pattern's length.  The JUMPs that end an alternation's branches wait
 * for the alternation's end, chained through their targets.
 */

/*
 * The target of a JUMP not yet placed that ends the chain of those waiting.
 */
#define	CHAIN_END	UINT32_MAX

/*
 * A pattern being compiled: its bytes, the next to read, the program so
 * far, the depth of parentheses open, and the first failure met.
 */
typedef struct compiler compiler_t;

struct compiler {
	const uint8_t		*pattern;
	size_t			 length;
	size_t			 position;
	relic_regex_inst_t	*insts;
	size_t			 capacity;
	uint32_t		 count;
	unsigned int		 depth;
	relic_regex_error_t	 error;
};

/*
 * One step's threads: each is an instruction and the position its match
 * began at, two words in `words`, `count` of them.
 */
typedef struct thread_list thread_list_t;

struct thread_list {
	uint32_t	*words;
	size_t		 count;
};

/*
 * What a match being run reads and writes besides its lists: the program,
 * the text, the marks saying which instructions a list holds in this step,
 * the stamp this step marks with, and the answer, -1 until some match has
 * been found.
 */
typedef struct matcher matcher_t;

struct matcher {
	const relic_regex_inst_t	*insts;
	const uint8_t			*text;
	size_t				 length;
	uint32_t			*marks;
	uint32_t			 stamp;
	int64_t				 answer;
};

static bool	 at_end(const compiler_t *compiler);
static uint8_t	 peek(const compiler_t *compiler);
static bool	 fail(compiler_t *compiler, enum relic_regex_failure failure,
		    size_t position);
static relic_regex_inst_t *emit(compiler_t *compiler, enum relic_regex_op op);
static relic_regex_inst_t *insert_at(compiler_t *compiler, uint32_t start);
static bool	 compile_alternation(compiler_t *compiler);
static bool	 compile_branch(compiler_t *compiler);
static bool	 compile_piece(compiler_t *compiler);
static bool	 compile_atom(compiler_t *compiler);
static bool	 compile_group(compiler_t *compiler);
static bool	 compile_set(compiler_t *compiler);
static void	 set_add(uint8_t set[static 32], uint8_t low, uint8_t high);
static bool	 set_holds(const uint8_t set[static 32], uint8_t byte);
static uint32_t	 next_stamp(relic_regex_work_t *work);
static void	 add_thread(matcher_t *matcher, thread_list_t *list,
		    uint32_t pc, uint32_t start, size_t position);
static void	 thread_append(matcher_t *matcher, thread_list_t *list,
		    uint32_t pc, uint32_t start);
static bool	 consumes(const relic_regex_inst_t *inst, uint8_t byte);
static void	 add_start(matcher_t *matcher, thread_list_t *list,
		    size_t position);
static bool	 begins_with_byte(const relic_regex_inst_t *first);
static size_t	 next_start(const matcher_t *matcher, size_t position);

size_t
relic_regex_capacity(size_t length)
{
	if (length > (UINT32_MAX - 1) / 3)
		return (0);

	return (3 * length + 1);
}

const char *
relic_regex_message(enum relic_regex_failure failure)
{
	switch (failure) {
	case RELIC_RE_UNMATCHED_OPEN:
		return ("unmatched (");
	case RELIC_RE_UNMATCHED_CLOSE:
		return ("unmatched )");
	case RELIC_RE_UNTERMINATED_SET:
		return ("unterminated [");
	case RELIC_RE_NOTHING_TO_REPEAT:
		return ("nothing to repeat");
	case RELIC_RE_REVERSED_RANGE:
		return ("reversed range");
	case RELIC_RE_TRAILING_BACKSLASH:
		return ("trailing backslash");
	case RELIC_RE_TOO_DEEP:
		break;
	}

	return ("nested more than 32 levels deep");
}

_Static_assert(RELIC_REGEX_DEPTH_MAX == 32,
    "the message names the depth bound");

uint32_t
relic_regex_compile(const char *pattern, size_t length,
    relic_regex_inst_t *insts, relic_regex_error_t *errorp)
{
	compiler_t compiler;

	bzero(&compiler, sizeof(compiler));
	compiler.pattern = (const uint8_t *)pattern;
	compiler.length = length;
	compiler.insts = insts;
	compiler.capacity = relic_regex_capacity(length);
	assert(compiler.capacity > 0);

	if (!compile_alternation(&compiler)) {
		*errorp = compiler.error;
		return (0);
	}

	/*
	 * The alternation stops only at the end or at a `)`, and one at the
	 * top level closes nothing.
	 */
	if (!at_end(&compiler)) {
		(void)fail(&compiler, RELIC_RE_UNMATCHED_CLOSE,
		    compiler.position);
		*errorp = compiler.error;
		return (0);
	}

	(void)emit(&compiler, RELIC_RE_MATCH);

	return (compiler.count);
}

static bool
at_end(const compiler_t *compiler)
{
	return (compiler->position >= compiler->length);
}

/*
 * The next byte of the pattern, which the caller has established exists.
 */
static uint8_t
peek(const compiler_t *compiler)
{
	assert(!at_end(compiler));

	return (compiler->pattern[compiler->position]);
}

static bool
fail(compiler_t *compiler, enum relic_regex_failure failure, size_t position)
{
	compiler->error.failure = failure;
	compiler->error.position = position;

	return (false);
}

/*
 * Append an instruction of `op`, zeroed but for its op, and return it.  The
 * construction above never needs more than the capacity.
 */
static relic_regex_inst_t *
emit(compiler_t *compiler, enum relic_regex_op op)
{
	relic_regex_inst_t *inst;

	assert(compiler->count < compiler->capacity);
	inst = &compiler->insts[compiler->count];
	compiler->count++;

	bzero(inst, sizeof(*inst));
	inst->op = (uint8_t)op;

	return (inst);
}

/*
 * Make room for one instruction at `start` by moving everything emitted
 * from there on up by one, correcting the targets the moved code holds, and
 * return the room, which the caller fills.  Every target the moved code
 * holds points at or past `start`, since it points within the moved code
 * or just past it.
 */
static relic_regex_inst_t *
insert_at(compiler_t *compiler, uint32_t start)
{
	relic_regex_inst_t *inst;
	uint32_t i;

	assert(compiler->count < compiler->capacity);
	memmove(&compiler->insts[start + 1], &compiler->insts[start],
	    (compiler->count - start) * sizeof(*compiler->insts));
	compiler->count++;

	for (i = start + 1; i < compiler->count; i++) {
		inst = &compiler->insts[i];
		if (inst->op != RELIC_RE_SPLIT && inst->op != RELIC_RE_JUMP)
			continue;
		assert(inst->u.to.x >= start && inst->u.to.x != CHAIN_END);
		inst->u.to.x++;
		if (inst->op == RELIC_RE_SPLIT) {
			assert(inst->u.to.y >= start);
			inst->u.to.y++;
		}
	}

	inst = &compiler->insts[start];
	bzero(inst, sizeof(*inst));

	return (inst);
}

/*
 * pattern := branch ("|" branch)*
 *
 * Each `|` puts a SPLIT before the branch just emitted, to it or on to what
 * follows, and a JUMP after it to the alternation's end, which is placed
 * once the last branch has been emitted.  A later SPLIT goes where the
 * earlier one's second target already points, which is the entry to the
 * rest of the alternation it now is.
 */
static bool
compile_alternation(compiler_t *compiler)
{
	relic_regex_inst_t *split;
	relic_regex_inst_t *jump;
	uint32_t branch_start;
	uint32_t waiting;
	uint32_t next;

	waiting = CHAIN_END;
	branch_start = compiler->count;
	if (!compile_branch(compiler))
		return (false);

	while (!at_end(compiler) && peek(compiler) == '|') {
		compiler->position++;

		split = insert_at(compiler, branch_start);
		split->op = RELIC_RE_SPLIT;
		split->u.to.x = branch_start + 1;

		jump = emit(compiler, RELIC_RE_JUMP);
		jump->u.to.x = waiting;
		waiting = compiler->count - 1;

		compiler->insts[branch_start].u.to.y = compiler->count;
		branch_start = compiler->count;
		if (!compile_branch(compiler))
			return (false);
	}

	while (waiting != CHAIN_END) {
		next = compiler->insts[waiting].u.to.x;
		compiler->insts[waiting].u.to.x = compiler->count;
		waiting = next;
	}

	return (true);
}

/*
 * branch := piece*, ending at a `|`, at the `)` that closes a group, or at
 * the pattern's end.  A `)` outside every group closes nothing.
 */
static bool
compile_branch(compiler_t *compiler)
{
	uint8_t byte;

	while (!at_end(compiler)) {
		byte = peek(compiler);
		if (byte == '|')
			break;
		if (byte == ')') {
			if (compiler->depth == 0)
				return (fail(compiler,
				    RELIC_RE_UNMATCHED_CLOSE,
				    compiler->position));
			break;
		}
		if (!compile_piece(compiler))
			return (false);
	}

	return (true);
}

/*
 * piece := atom ("*" | "+" | "?")*
 *
 * The run of repetitions is read whole: any `*`, or both a `+` and a `?`,
 * repeat the atom any number of times; `+` alone at least once; `?` alone
 * at most once.
 */
static bool
compile_piece(compiler_t *compiler)
{
	relic_regex_inst_t *inst;
	uint32_t start;
	uint8_t byte;
	bool star;
	bool plus;
	bool quest;

	byte = peek(compiler);
	if (byte == '*' || byte == '+' || byte == '?')
		return (fail(compiler, RELIC_RE_NOTHING_TO_REPEAT,
		    compiler->position));

	start = compiler->count;
	if (!compile_atom(compiler))
		return (false);

	star = plus = quest = false;
	while (!at_end(compiler)) {
		byte = peek(compiler);
		if (byte == '*')
			star = true;
		else if (byte == '+')
			plus = true;
		else if (byte == '?')
			quest = true;
		else
			break;
		compiler->position++;
	}

	if (star || (plus && quest)) {
		inst = insert_at(compiler, start);
		inst->op = RELIC_RE_SPLIT;
		inst->u.to.x = start + 1;
		inst->u.to.y = compiler->count + 1;
		inst = emit(compiler, RELIC_RE_JUMP);
		inst->u.to.x = start;
	} else if (plus) {
		inst = emit(compiler, RELIC_RE_SPLIT);
		inst->u.to.x = start;
		inst->u.to.y = compiler->count;
	} else if (quest) {
		inst = insert_at(compiler, start);
		inst->op = RELIC_RE_SPLIT;
		inst->u.to.x = start + 1;
		inst->u.to.y = compiler->count;
	}

	return (true);
}

/*
 * atom := byte | "." | "[" set "]" | "(" pattern ")" | "^" | "$" | "\" byte
 *
 * A `]` or a `{` outside a set is a byte like any other, as it is to
 * `grep -E`.
 */
static bool
compile_atom(compiler_t *compiler)
{
	relic_regex_inst_t *inst;
	uint8_t byte;

	byte = peek(compiler);
	compiler->position++;

	switch (byte) {
	case '(':
		return (compile_group(compiler));
	case '[':
		return (compile_set(compiler));
	case '.':
		(void)emit(compiler, RELIC_RE_ANY);
		return (true);
	case '^':
		(void)emit(compiler, RELIC_RE_BOL);
		return (true);
	case '$':
		(void)emit(compiler, RELIC_RE_EOL);
		return (true);
	case '\\':
		if (at_end(compiler))
			return (fail(compiler, RELIC_RE_TRAILING_BACKSLASH,
			    compiler->position - 1));
		byte = peek(compiler);
		compiler->position++;
		break;
	default:
		break;
	}

	inst = emit(compiler, RELIC_RE_BYTE);
	inst->u.byte = byte;

	return (true);
}

/*
 * "(" pattern ")", the `(` already read.  A group emits nothing of its own.
 */
static bool
compile_group(compiler_t *compiler)
{
	size_t open;

	open = compiler->position - 1;
	if (compiler->depth == RELIC_REGEX_DEPTH_MAX)
		return (fail(compiler, RELIC_RE_TOO_DEEP, open));

	compiler->depth++;
	if (!compile_alternation(compiler))
		return (false);
	compiler->depth--;

	if (at_end(compiler))
		return (fail(compiler, RELIC_RE_UNMATCHED_OPEN, open));
	compiler->position++;

	return (true);
}

/*
 * set := ["^"] item+ "]", the `[` already read, where
 *
 *	item := byte | byte "-" byte | "\" byte
 *
 * A `]` first, after any `^`, is a byte rather than the set's end.  An
 * unescaped item followed by `-` and a byte other than `]` or `\` is a
 * range, whatever the item is, so `[]-a]` and `[--0]` are ranges, as they
 * are to `grep -E`; a `-` that cannot be read so, such as the last byte,
 * is a byte.  An escaped byte is always a byte alone, never a range's end.
 */
static bool
compile_set(compiler_t *compiler)
{
	relic_regex_inst_t *inst;
	uint8_t set[32];
	size_t open;
	size_t i;
	uint8_t low;
	uint8_t high;
	bool negated;
	bool first;

	open = compiler->position - 1;
	bzero(set, sizeof(set));

	negated = !at_end(compiler) && peek(compiler) == '^';
	if (negated)
		compiler->position++;

	for (first = true;; first = false) {
		if (at_end(compiler))
			return (fail(compiler, RELIC_RE_UNTERMINATED_SET,
			    open));

		low = peek(compiler);
		if (low == ']' && !first)
			break;
		compiler->position++;

		if (low == '\\') {
			if (at_end(compiler))
				return (fail(compiler,
				    RELIC_RE_TRAILING_BACKSLASH,
				    compiler->position - 1));
			low = peek(compiler);
			compiler->position++;
			set_add(set, low, low);
			continue;
		}

		if (compiler->position + 1 < compiler->length &&
		    peek(compiler) == '-' &&
		    compiler->pattern[compiler->position + 1] != ']' &&
		    compiler->pattern[compiler->position + 1] != '\\') {
			high = compiler->pattern[compiler->position + 1];
			if (high < low)
				return (fail(compiler, RELIC_RE_REVERSED_RANGE,
				    compiler->position - 1));
			compiler->position += 2;
			set_add(set, low, high);
			continue;
		}

		set_add(set, low, low);
	}
	compiler->position++;

	if (negated) {
		for (i = 0; i < sizeof(set); i++)
			set[i] = (uint8_t)~set[i];
	}

	inst = emit(compiler, RELIC_RE_SET);
	bcopy(set, inst->u.set, sizeof(set));

	return (true);
}

/*
 * Add the bytes from `low` to `high`, both included, to a set.
 */
static void
set_add(uint8_t set[static 32], uint8_t low, uint8_t high)
{
	unsigned int byte;

	for (byte = low; byte <= high; byte++)
		set[byte / 8] |= (uint8_t)(1u << (byte % 8));
}

static bool
set_holds(const uint8_t set[static 32], uint8_t byte)
{
	return ((set[byte / 8] & (1u << (byte % 8))) != 0);
}

/*
 * The matcher runs one list of threads per position of the text, the
 * threads at that position ordered by the position their match began at.
 * A step adds, for each thread in order whose instruction consumes the
 * byte at the position, a thread at the next instruction to the next
 * position's list, and then, while no match has been found, a thread
 * beginning at the next position, whose start is the greatest; so the next
 * list is ordered too.  Adding a thread follows its SPLITs, JUMPs and
 * anchors at once, and adds an instruction only if the list does not hold
 * it yet: the thread holding it began no later than one that would add it
 * again, so the instruction is held by the earliest start that reaches it,
 * which is the only one the leftmost match can come from.  A list thus
 * holds each instruction at most once, and a step does work proportional
 * to the program's length.
 *
 * A thread reaching MATCH records its start when it is the least found.
 * Once a match is found no new start is added, and a thread starting at or
 * after the match found is dropped, since it cannot improve on it; the
 * run ends when the text is consumed or no thread remains.
 *
 * A program that begins by consuming a byte of some set can only begin a
 * match where the text holds such a byte, so a start is added only there;
 * and while no thread is alive and no answer found, the search goes
 * straight to the next such position rather than stepping an empty list
 * through the bytes between.  An unanchored search for a pattern that
 * begins with a literal thus spends its steps only where a match can begin.
 *
 * The marks are per instruction, and hold the stamp of the step whose list
 * holds the instruction: a new step takes a new stamp, which empties the
 * marks without clearing them, so a short text run against a long program
 * pays nothing per step for the program's length.  The stamp continues
 * from one match to the next, and the marks are cleared only when it would
 * wrap.
 */
int64_t
relic_regex_match(const relic_regex_inst_t *insts,
    uint32_t count RELIC_UNUSED, relic_regex_work_t *work, const char *text,
    size_t length)
{
	const relic_regex_inst_t *inst;
	thread_list_t current;
	thread_list_t next;
	thread_list_t swap;
	matcher_t matcher;
	size_t position;
	size_t i;
	uint32_t pc;
	uint32_t start;
	uint8_t byte;

	assert(count > 0 && count <= work->capacity);
	assert(length <= UINT32_MAX - 1);

	matcher.insts = insts;
	matcher.text = (const uint8_t *)text;
	matcher.length = length;
	matcher.marks = work->words + 4 * work->capacity;
	matcher.answer = -1;

	current.words = work->words;
	current.count = 0;
	next.words = work->words + 2 * work->capacity;
	next.count = 0;

	matcher.stamp = next_stamp(work);
	add_start(&matcher, &current, 0);

	for (position = 0; position < length; position++) {
		if (current.count == 0) {
			if (matcher.answer >= 0)
				break;
			position = next_start(&matcher, position);
			if (position == length)
				break;
			matcher.stamp = next_stamp(work);
			add_start(&matcher, &current, position);
		}

		matcher.stamp = next_stamp(work);
		next.count = 0;
		byte = matcher.text[position];

		for (i = 0; i < current.count; i++) {
			pc = current.words[2 * i];
			start = current.words[2 * i + 1];
			if (matcher.answer >= 0 && start >= matcher.answer)
				continue;
			inst = &insts[pc];
			if (consumes(inst, byte))
				add_thread(&matcher, &next, pc + 1, start,
				    position + 1);
		}

		if (matcher.answer < 0)
			add_start(&matcher, &next, position + 1);

		swap = current;
		current = next;
		next = swap;
	}

	return (matcher.answer);
}

/*
 * Add to a list the thread of a match beginning at `position`, unless the
 * program begins by consuming a byte the text does not hold there, when the
 * thread would die on its first step and is not worth adding.  A program
 * beginning with any other instruction is started everywhere.
 */
static void
add_start(matcher_t *matcher, thread_list_t *list, size_t position)
{
	const relic_regex_inst_t *first;

	first = &matcher->insts[0];
	if (begins_with_byte(first) && (position == matcher->length ||
	    !consumes(first, matcher->text[position])))
		return;

	add_thread(matcher, list, 0, (uint32_t)position, position);
}

/*
 * The first position at or after `position` where a match can begin: for
 * a program that begins by consuming a byte, where the text holds a byte
 * its first instruction consumes, found by memchr(3) for a single byte, or
 * the text's length when there is none; for any other program, `position`
 * itself.
 */
static size_t
next_start(const matcher_t *matcher, size_t position)
{
	const relic_regex_inst_t *first;
	const uint8_t *found;

	first = &matcher->insts[0];
	if (!begins_with_byte(first))
		return (position);

	if (first->op == RELIC_RE_BYTE) {
		found = memchr(matcher->text + position, first->u.byte,
		    matcher->length - position);
		return (found == NULL ? matcher->length :
		    (size_t)(found - matcher->text));
	}

	while (position < matcher->length &&
	    !consumes(first, matcher->text[position]))
		position++;

	return (position);
}

/*
 * Whether an instruction consumes a byte of a set it names: BYTE or SET.
 * ANY consumes every byte, so nothing is learned from it.
 */
static bool
begins_with_byte(const relic_regex_inst_t *first)
{
	return (first->op == RELIC_RE_BYTE || first->op == RELIC_RE_SET);
}

/*
 * The stamp of a new step.  Zero is the stamp of marks never set, so a
 * stamp that wraps to zero clears the marks and starts again at one.
 */
static uint32_t
next_stamp(relic_regex_work_t *work)
{
	work->stamp++;
	if (work->stamp == 0) {
		bzero(work->words + 4 * work->capacity,
		    work->capacity * sizeof(*work->words));
		work->stamp = 1;
	}

	return (work->stamp);
}

/*
 * Whether a consuming instruction takes this byte.  Any other instruction
 * takes none: a step reads only the instructions that consume, and a
 * thread at any other has already gone on through it.
 */
static bool
consumes(const relic_regex_inst_t *inst, uint8_t byte)
{
	switch (inst->op) {
	case RELIC_RE_BYTE:
		return (inst->u.byte == byte);
	case RELIC_RE_ANY:
		return (true);
	case RELIC_RE_SET:
		return (set_holds(inst->u.set, byte));
	default:
		break;
	}

	return (false);
}

/*
 * Add a thread at `pc` whose match began at `start` to the list of
 * `position`, and every thread it goes on to without consuming a byte.
 * The list itself is the work list: the threads appended here are visited
 * in turn and their successors appended after them, so no stack is needed
 * and every instruction is visited once.  All of them share one start, so
 * the order among them does not matter.
 */
static void
add_thread(matcher_t *matcher, thread_list_t *list, uint32_t pc,
    uint32_t start, size_t position)
{
	const relic_regex_inst_t *inst;
	size_t i;

	i = list->count;
	thread_append(matcher, list, pc, start);

	for (; i < list->count; i++) {
		pc = list->words[2 * i];
		inst = &matcher->insts[pc];

		switch (inst->op) {
		case RELIC_RE_SPLIT:
			thread_append(matcher, list, inst->u.to.x, start);
			thread_append(matcher, list, inst->u.to.y, start);
			break;
		case RELIC_RE_JUMP:
			thread_append(matcher, list, inst->u.to.x, start);
			break;
		case RELIC_RE_BOL:
			if (position == 0)
				thread_append(matcher, list, pc + 1, start);
			break;
		case RELIC_RE_EOL:
			if (position == matcher->length)
				thread_append(matcher, list, pc + 1, start);
			break;
		case RELIC_RE_MATCH:
			if (matcher->answer < 0 || start < matcher->answer)
				matcher->answer = start;
			break;
		default:
			break;
		}
	}
}

/*
 * Append a thread at `pc` to the list unless the list holds `pc` already.
 */
static void
thread_append(matcher_t *matcher, thread_list_t *list, uint32_t pc,
    uint32_t start)
{
	if (matcher->marks[pc] == matcher->stamp)
		return;

	matcher->marks[pc] = matcher->stamp;
	list->words[2 * list->count] = pc;
	list->words[2 * list->count + 1] = start;
	list->count++;
}
