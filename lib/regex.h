/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_REGEX_H
#define RELIC_REGEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The regular expressions `match` decides (lib/regex.c): a pattern
 * compiled to a program of instructions, and the program run over a string
 * to find the least position at which a match begins.
 *
 * The dialect is POSIX extended regular expressions without counted
 * repetition, character classes, backreferences or flags: bytes, `.`, sets
 * in brackets, groups, alternation, the three postfix repetitions `*`, `+`
 * and `?`, the anchors `^` and `$`, and a backslash making the next byte
 * literal.  Bytes are never characters: `.` and a negated set match any
 * byte, a line feed and a NUL among them, and `^` and `$` match only at the
 * ends of the string.
 *
 * Both halves are pure and allocate nothing.  A pattern of `length` bytes
 * compiles into at most relic_regex_capacity(length) instructions, which the
 * caller provides, so that a pattern's program can be laid out before it is
 * compiled.  A program is immutable once compiled, and any number of
 * threads may run it at once, each with a work space of its own: the
 * matcher's mutable state is the work space and never the program.
 */

/*
 * The deepest parentheses nest in a pattern, which bounds the compiler's
 * recursion as the parser's bound on an expression's nesting bounds the
 * parser's.
 */
#define	RELIC_REGEX_DEPTH_MAX	32

typedef struct relic_pattern relic_pattern_t;
typedef struct relic_regex_error relic_regex_error_t;
typedef struct relic_regex_inst relic_regex_inst_t;
typedef struct relic_regex_work relic_regex_work_t;

/*
 * What an instruction does.  A thread at BYTE, ANY or SET consumes one byte
 * of the string when the byte is the instruction's, any byte, or in its
 * set, and goes on to the next instruction; SPLIT goes on at both its
 * targets, JUMP at its one; BOL and EOL go on to the next instruction only
 * at the start and at the end of the string; and a thread reaching MATCH
 * has matched.
 */
enum relic_regex_op {
	RELIC_RE_BYTE,
	RELIC_RE_ANY,
	RELIC_RE_SET,
	RELIC_RE_SPLIT,
	RELIC_RE_JUMP,
	RELIC_RE_BOL,
	RELIC_RE_EOL,
	RELIC_RE_MATCH
};

/*
 * One instruction: a byte for BYTE, a bitmap of 256 bytes for SET, bit
 * `b % 8` of byte `b / 8` set when the set holds `b`, and the index of the
 * instruction to go on at for JUMP, or of the two for SPLIT.
 */
struct relic_regex_inst {
	union {
		uint8_t		byte;
		uint8_t		set[32];
		struct {
			uint32_t	x;
			uint32_t	y;
		}		to;
	}		u;
	uint8_t		op;		/* enum relic_regex_op */
};

/*
 * A compiled pattern: the program of `count` instructions at `insts`, which
 * nothing writes once it is compiled.
 */
struct relic_pattern {
	const relic_regex_inst_t	*insts;
	uint32_t			 count;
};

/*
 * Why a pattern is not a regular expression of the dialect, and the offset
 * in the pattern of the byte where the compiler found out: the unmatched
 * parenthesis, the `[` left open, the repetition with nothing before it,
 * the range's first byte, the backslash, or the `(` one level too deep.
 */
enum relic_regex_failure {
	RELIC_RE_UNMATCHED_OPEN,
	RELIC_RE_UNMATCHED_CLOSE,
	RELIC_RE_UNTERMINATED_SET,
	RELIC_RE_NOTHING_TO_REPEAT,
	RELIC_RE_REVERSED_RANGE,
	RELIC_RE_TRAILING_BACKSLASH,
	RELIC_RE_TOO_DEEP
};

struct relic_regex_error {
	enum relic_regex_failure	failure;
	size_t				position;
};

/*
 * The mutable state of a match, owned by whoever runs it: room for a
 * program of `capacity` instructions, two lists of threads of two words
 * each and a mark per instruction, RELIC_REGEX_WORK_WORDS words per
 * instruction in all, and the stamp the marks were last set with.  A work
 * space starts zeroed, and serves any program of at most `capacity`
 * instructions, one match at a time.
 */
#define	RELIC_REGEX_WORK_WORDS	5u

struct relic_regex_work {
	uint32_t	*words;
	size_t		 capacity;
	uint32_t	 stamp;
};

/*
 * relic_regex_capacity() is the most instructions a pattern of `length`
 * bytes compiles into, 3 * length + 1, or zero when that many could not be
 * numbered by an instruction's 32-bit targets, which no pattern a caller
 * could hold compiles into anyway.
 *
 * relic_regex_compile() compiles a pattern into `insts`, which has room for
 * relic_regex_capacity(length) instructions, and returns how many it wrote,
 * or zero with *errorp saying why the pattern is not one.
 *
 * relic_regex_match() runs a program of `count` instructions over `length`
 * bytes of `text`, in a work space with room for it, and returns the least
 * position at which some match begins, or -1 when none does.  It takes time
 * proportional to the text's length times the program's, whatever the
 * pattern, and neither recurses nor allocates.
 *
 * relic_regex_message() is the text a diagnostic gives for a failure.
 */
size_t	 relic_regex_capacity(size_t length);
uint32_t relic_regex_compile(const char *pattern, size_t length,
	    relic_regex_inst_t *insts, relic_regex_error_t *errorp);
int64_t	 relic_regex_match(const relic_regex_inst_t *insts, uint32_t count,
	    relic_regex_work_t *work, const char *text, size_t length);
const char *relic_regex_message(enum relic_regex_failure failure);

#endif /* RELIC_REGEX_H */
