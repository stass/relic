/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Contract tests for the regular expression matcher (lib/regex.c).
 *
 * Each construct of the dialect is matched alone and combined, each of the
 * seven errors is found at its byte, and the leftmost rule is checked where
 * a matcher reporting some other match would differ.  Patterns that make a
 * backtracking matcher take exponential time complete here in time linear
 * in the text.  Random patterns never compile into more instructions than
 * the bound the program's room is sized by.
 *
 * The matcher is compared with a reference over a million random patterns
 * and strings.  The reference parses a pattern by the grammar into a tree
 * of its own and decides a match by the set of positions each subtree can
 * end at from a set of positions it can begin at, a definition that shares
 * nothing with the compiled program or the simulation: the two must agree
 * on whether a pattern is one, on why it is not, and on where the leftmost
 * match begins.
 */

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "harness.h"
#include "regex.h"

#define	SCENARIO_CONSTRUCTS	"each construct matches what it means"
#define	SCENARIO_ERRORS		"a pattern that is not one says why and where"
#define	SCENARIO_LEFTMOST	"a match is the leftmost"
#define	SCENARIO_LINEAR		"no pattern takes more than linear time"
#define	SCENARIO_BOUND		"a program fits the room its pattern gives"
#define	SCENARIO_REFERENCE	"the matcher agrees with the reference"
#define	SCENARIO_STAMP		"a work space serves match after match"

/*
 * The random comparison: how many pairs, how many texts each pattern that
 * compiles is run over, and the longest pattern and text drawn.  A text is
 * short enough that a set of positions is a 64-bit word.
 */
#define	RANDOM_PAIRS		1000000
#define	TEXTS_PER_PATTERN	10
#define	RANDOM_PATTERN_MAX	10
#define	RANDOM_TEXT_MAX		8
#define	RANDOM_PATTERNS		1000000

/*
 * The text the pathological patterns run over, the length of the run of
 * optional bytes in one of them, and the time any may take.  A matcher
 * linear in the text takes a small fraction of the bound even under a
 * sanitizer; one that backtracks takes longer than the universe has.
 */
#define	LONG_TEXT		100000
#define	OPTIONAL_RUN		20
#define	LINEAR_SECONDS		5.0

/* The most nodes a reference tree of a random pattern holds. */
#define	TREE_MAX		(4 * RANDOM_PATTERN_MAX + 8)

typedef struct compiled compiled_t;
typedef struct node node_t;
typedef struct reference reference_t;

/*
 * A pattern compiled with its own work space.
 */
struct compiled {
	relic_regex_inst_t	*insts;
	relic_regex_work_t	 work;
	uint32_t		 count;
	relic_regex_error_t	 error;
};

/*
 * A node of the reference's tree.
 */
enum node_kind {
	NODE_EMPTY,
	NODE_BYTE,
	NODE_ANY,
	NODE_SET,
	NODE_BOL,
	NODE_EOL,
	NODE_CAT,
	NODE_ALT,
	NODE_STAR,
	NODE_PLUS,
	NODE_QUEST
};

struct node {
	enum node_kind	kind;
	uint8_t		byte;
	bool		set[256];
	int		left;
	int		right;
};

/*
 * A pattern being parsed by the reference: its bytes, the next to read, the
 * nesting depth, the nodes built, and the first failure.
 */
struct reference {
	const uint8_t		*pattern;
	size_t			 length;
	size_t			 position;
	unsigned int		 depth;
	node_t			 nodes[TREE_MAX];
	int			 count;
	bool			 failed;
	enum relic_regex_failure failure;
	size_t			 failed_at;
};

static bool	 compile(compiled_t *compiled, const char *pattern,
		    size_t length);
static void	 release(compiled_t *compiled);
static int64_t	 match_bytes(const char *pattern, size_t pattern_length,
		    const char *text, size_t text_length);
static int64_t	 match(const char *pattern, const char *text);
static int	 ref_node(reference_t *ref, enum node_kind kind, int left,
		    int right);
static int	 ref_alternation(reference_t *ref);
static int	 ref_branch(reference_t *ref);
static int	 ref_piece(reference_t *ref);
static int	 ref_atom(reference_t *ref);
static int	 ref_set(reference_t *ref);
static int	 ref_fail(reference_t *ref, enum relic_regex_failure failure,
		    size_t position);
static bool	 ref_parse(reference_t *ref, const char *pattern,
		    size_t length);
static uint64_t	 ref_ends(const reference_t *ref, int node,
		    const uint8_t *text, size_t length, uint64_t starts);
static int64_t	 ref_match(const reference_t *ref, int root,
		    const char *text, size_t length);
static uint32_t	 next_random(uint32_t *state);
static void	 each_construct_matches_what_it_means(void);
static void	 each_error_is_found_at_its_byte(void);
static void	 a_match_is_the_leftmost(void);
static void	 nesting_stops_at_the_bound(void);
static void	 no_pattern_takes_more_than_linear_time(void);
static void	 a_program_fits_the_room_its_pattern_gives(void);
static void	 the_matcher_agrees_with_the_reference(void);
static void	 a_work_space_serves_match_after_match(void);

/*
 * Compile `length` bytes of pattern into a program of its own room, with a
 * work space for it.  Returns whether the pattern compiled; either way the
 * caller releases what was allocated.
 */
static bool
compile(compiled_t *compiled, const char *pattern, size_t length)
{
	size_t capacity;

	bzero(compiled, sizeof(*compiled));
	capacity = relic_regex_capacity(length);
	compiled->insts = calloc(capacity, sizeof(*compiled->insts));
	compiled->work.words = calloc(RELIC_REGEX_WORK_WORDS * capacity,
	    sizeof(*compiled->work.words));
	compiled->work.capacity = capacity;
	if (compiled->insts == NULL || compiled->work.words == NULL) {
		check(false, "the test", "memory should be available");
		return (false);
	}

	compiled->count = relic_regex_compile(pattern, length,
	    compiled->insts, &compiled->error);

	return (compiled->count > 0);
}

static void
release(compiled_t *compiled)
{
	free(compiled->insts);
	free(compiled->work.words);
}

/*
 * Where the leftmost match of a pattern begins in a text, -1 when none
 * does, or -2 when the pattern does not compile.
 */
static int64_t
match_bytes(const char *pattern, size_t pattern_length, const char *text,
    size_t text_length)
{
	compiled_t compiled;
	int64_t found;

	found = -2;
	if (compile(&compiled, pattern, pattern_length))
		found = relic_regex_match(compiled.insts, compiled.count,
		    &compiled.work, text, text_length);
	release(&compiled);

	return (found);
}

static int64_t
match(const char *pattern, const char *text)
{
	return (match_bytes(pattern, strlen(pattern), text, strlen(text)));
}

/*
 * Every construct alone and combined with others, each against a text it
 * matches and, where there is one, a text it does not.
 */
static void
each_construct_matches_what_it_means(void)
{
	static const struct {
		const char	*pattern;
		const char	*text;
		int64_t		 expected;
	} cases[] = {
		{ "", "", 0 },
		{ "", "abc", 0 },
		{ "a", "a", 0 },
		{ "a", "ba", 1 },
		{ "a", "b", -1 },
		{ "abc", "xxabcx", 2 },
		{ ".", "", -1 },
		{ ".", "\n", 0 },
		{ "a.c", "abc", 0 },
		{ "a.c", "ac", -1 },
		{ "a*", "", 0 },
		{ "ba*c", "xbaaac", 1 },
		{ "ba*c", "xbc", 1 },
		{ "ba+c", "xbc", -1 },
		{ "ba+c", "xbaac", 1 },
		{ "ba?c", "bac", 0 },
		{ "ba?c", "baac", -1 },
		{ "a**", "b", 0 },
		{ "ba+?c", "bc", 0 },
		{ "ba?+c", "baac", 0 },
		{ "ba++c", "bc", -1 },
		{ "ba??c", "baac", -1 },
		{ "a|b", "cb", 1 },
		{ "ab|cd|ef", "xef", 1 },
		{ "a(b|c)d", "acd", 0 },
		{ "a(b|c)d", "aed", -1 },
		{ "(ab)+", "xababx", 1 },
		{ "(ab)*c", "ababc", 0 },
		{ "(a|)b", "b", 0 },
		{ "()", "x", 0 },
		{ "(|a)+b", "aab", 0 },
		{ "^a", "a", 0 },
		{ "^a", "ba", -1 },
		{ "a$", "ba", 1 },
		{ "a$", "ab", -1 },
		{ "^$", "", 0 },
		{ "^$", "a", -1 },
		{ "^a|b$", "cb", 1 },
		{ "(^a|b)c", "bc", 0 },
		{ "(^a|b)c", "xac", -1 },
		{ "x(a$|b)", "xa", 0 },
		{ "x(a$|b)", "xay", -1 },
		{ "^*a", "a", 0 },
		{ "[abc]", "xxc", 2 },
		{ "[a-c]+", "xcab", 1 },
		{ "[^a-c]", "abcd", 3 },
		{ "[]a]", "x]", 1 },
		{ "[^]a]", "]ab", 2 },
		{ "[-a]", "b-", 1 },
		{ "[a-]", "b-", 1 },
		{ "[a-c-e]", "-", 0 },
		{ "[--0]", "x.", 1 },
		{ "[]-a]", "x^", 1 },
		{ "[\\]]", "]", 0 },
		{ "[\\\\]", "\\", 0 },
		{ "[a\\-z]", "b", -1 },
		{ "[a\\-z]", "-", 0 },
		{ "\\.c$", "main.c", 4 },
		{ "\\.c$", "mainxc", -1 },
		{ "\\(\\)", "()", 0 },
		{ "\\*", "a*", 1 },
		{ "]", "a]", 1 },
		{ "a{2}", "aa", -1 },
		{ "a{2}", "a{2}", 0 },
		{ "\xe9+", "caf\xe9\xe9", 3 },
		{ "[\x80-\xff]", "abc\xc3", 3 },
		{ "a", "", -1 },
		{ "ab", "aaab", 2 },
		{ "a+b", "caaab", 1 },
		{ "ab*c", "abxabbc", 3 },
		{ "[xy]z", "xxyz", 2 },
		{ "[xy]z", "xyx", -1 },
		{ "a$", "aaa", 2 }
	};
	int64_t found;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		found = match(cases[i].pattern, cases[i].text);
		checkf(found == cases[i].expected, SCENARIO_CONSTRUCTS,
		    "\"%s\" over \"%s\" should give %lld, not %lld",
		    cases[i].pattern, cases[i].text,
		    (long long)cases[i].expected, (long long)found);
	}

	check(match_bytes("a\0b", 3, "xa\0b", 4) == 1, SCENARIO_CONSTRUCTS,
	    "a NUL in the pattern matches a NUL in the text");
	check(match_bytes("a.b", 3, "a\0b", 3) == 0, SCENARIO_CONSTRUCTS,
	    "any byte matches a NUL");
	check(match_bytes("[^a]", 4, "a\0", 2) == 1, SCENARIO_CONSTRUCTS,
	    "a negated set matches a NUL");
	check(match_bytes("\\\0", 2, "\0", 1) == 0, SCENARIO_CONSTRUCTS,
	    "an escaped NUL is a NUL");
}

/*
 * The seven failures, each at the byte where the compiler finds it.
 */
static void
each_error_is_found_at_its_byte(void)
{
	static const struct {
		const char			*pattern;
		enum relic_regex_failure	 failure;
		size_t				 position;
	} cases[] = {
		{ "(", RELIC_RE_UNMATCHED_OPEN, 0 },
		{ "a(b(c)", RELIC_RE_UNMATCHED_OPEN, 1 },
		{ ")", RELIC_RE_UNMATCHED_CLOSE, 0 },
		{ "a)b", RELIC_RE_UNMATCHED_CLOSE, 1 },
		{ "(a))", RELIC_RE_UNMATCHED_CLOSE, 3 },
		{ "[a", RELIC_RE_UNTERMINATED_SET, 0 },
		{ "x[]", RELIC_RE_UNTERMINATED_SET, 1 },
		{ "[^]", RELIC_RE_UNTERMINATED_SET, 0 },
		{ "*a", RELIC_RE_NOTHING_TO_REPEAT, 0 },
		{ "(*)", RELIC_RE_NOTHING_TO_REPEAT, 1 },
		{ "a|*", RELIC_RE_NOTHING_TO_REPEAT, 2 },
		{ "a|+b", RELIC_RE_NOTHING_TO_REPEAT, 2 },
		{ "?", RELIC_RE_NOTHING_TO_REPEAT, 0 },
		{ "[z-a]", RELIC_RE_REVERSED_RANGE, 1 },
		{ "x[ab-a]", RELIC_RE_REVERSED_RANGE, 3 },
		{ "a\\", RELIC_RE_TRAILING_BACKSLASH, 1 },
		{ "[a\\", RELIC_RE_TRAILING_BACKSLASH, 2 }
	};
	relic_regex_error_t error;
	compiled_t compiled;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (compile(&compiled, cases[i].pattern,
		    strlen(cases[i].pattern))) {
			checkf(false, SCENARIO_ERRORS, "\"%s\" should not "
			    "compile", cases[i].pattern);
			release(&compiled);
			continue;
		}
		error = compiled.error;
		checkf(error.failure == cases[i].failure &&
		    error.position == cases[i].position, SCENARIO_ERRORS,
		    "\"%s\" should fail with \"%s\" at %zu, not \"%s\" at %zu",
		    cases[i].pattern, relic_regex_message(cases[i].failure),
		    cases[i].position, relic_regex_message(error.failure),
		    error.position);
		release(&compiled);
	}
}

/*
 * The least position at which some match begins, whatever other matches
 * begin later or a longer one would prefer.
 */
static void
a_match_is_the_leftmost(void)
{
	check(match("abc|b", "abc") == 0, SCENARIO_LEFTMOST,
	    "abc|b over abc begins at 0, not at the b");
	check(match("a*", "baa") == 0, SCENARIO_LEFTMOST,
	    "a* over baa matches the empty string at 0");
	check(match("^b", "ab") == -1, SCENARIO_LEFTMOST,
	    "^b over ab matches nowhere");
	check(match("b$", "ba") == -1, SCENARIO_LEFTMOST,
	    "b$ over ba matches nowhere");
	check(match("b|ab", "ab") == 0, SCENARIO_LEFTMOST,
	    "the branch written second matches earlier");
	check(match("aab|ab", "aaab") == 1, SCENARIO_LEFTMOST,
	    "a failed start does not hide a later one");
}

/*
 * Parentheses nest 32 deep and not 33.
 */
static void
nesting_stops_at_the_bound(void)
{
	char pattern[2 * (RELIC_REGEX_DEPTH_MAX + 1) + 2];
	compiled_t compiled;
	size_t depth;
	size_t i;
	size_t used;

	for (depth = RELIC_REGEX_DEPTH_MAX; depth <= RELIC_REGEX_DEPTH_MAX + 1;
	    depth++) {
		used = 0;
		for (i = 0; i < depth; i++)
			pattern[used++] = '(';
		pattern[used++] = 'a';
		for (i = 0; i < depth; i++)
			pattern[used++] = ')';

		if (depth == RELIC_REGEX_DEPTH_MAX) {
			check(compile(&compiled, pattern, used) &&
			    relic_regex_match(compiled.insts, compiled.count,
			    &compiled.work, "xa", 2) == 1, SCENARIO_ERRORS,
			    "32 nested groups compile and match");
		} else {
			check(!compile(&compiled, pattern, used) &&
			    compiled.error.failure == RELIC_RE_TOO_DEEP &&
			    compiled.error.position == RELIC_REGEX_DEPTH_MAX,
			    SCENARIO_ERRORS, "the 33rd group is refused at "
			    "its (");
		}
		release(&compiled);
	}
}

/*
 * Patterns a backtracking matcher takes exponential time over complete in
 * time linear in the text: each of them over a hundred thousand bytes
 * takes far less than the second a linear run is allowed here, where a
 * backtracking matcher would not finish.
 */
static void
no_pattern_takes_more_than_linear_time(void)
{
	static const char *const fixed[] = {
		"(a*)*b", "(a|a)*b", "(a+)+b", "((a*)*)*$b"
	};
	char optional_run[3 * OPTIONAL_RUN + 1];
	const char *patterns[sizeof(fixed) / sizeof(fixed[0]) + 1];
	char *text;
	clock_t began;
	double seconds;
	size_t count;
	size_t i;

	text = malloc(LONG_TEXT);
	if (text == NULL) {
		check(false, SCENARIO_LINEAR, "the text should be allocated");
		return;
	}
	memset(text, 'a', LONG_TEXT);

	/*
	 * a?a?...a?aa...a, n of each: a backtracking matcher tries 2^n ways
	 * of taking the optional bytes at each start before it gives up.
	 */
	for (i = 0; i < OPTIONAL_RUN; i++)
		memcpy(&optional_run[2 * i], "a?", 2);
	memset(&optional_run[2 * OPTIONAL_RUN], 'a', OPTIONAL_RUN);
	optional_run[3 * OPTIONAL_RUN] = '\0';

	count = 0;
	for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
		patterns[count++] = fixed[i];
	patterns[count++] = optional_run;

	for (i = 0; i < count; i++) {
		began = clock();
		(void)match_bytes(patterns[i], strlen(patterns[i]), text,
		    LONG_TEXT);
		seconds = (double)(clock() - began) / CLOCKS_PER_SEC;
		checkf(seconds < LINEAR_SECONDS, SCENARIO_LINEAR, "\"%.16s\" "
		    "over %d bytes should take under %.0f seconds, not %.2f",
		    patterns[i], LONG_TEXT, LINEAR_SECONDS, seconds);
	}

	free(text);
}

/*
 * A random draw from a small generator, so that a failure reproduces.
 */
static uint32_t
next_random(uint32_t *state)
{
	*state = *state * 1103515245u + 12345u;

	return (*state >> 8);
}

/*
 * Random patterns over the dialect's metacharacters and two bytes never
 * compile into more instructions than relic_regex_capacity() gives, which
 * the compiler asserts as it emits and this checks from outside.
 */
static void
a_program_fits_the_room_its_pattern_gives(void)
{
	static const char alphabet[] = "ab.()|*+?^$[]-\\";
	char pattern[RANDOM_PATTERN_MAX * 3];
	char run[64];
	compiled_t compiled;
	uint32_t state;
	size_t length;
	size_t i;
	size_t j;
	bool fits;

	state = 1;
	fits = true;
	for (i = 0; i < RANDOM_PATTERNS / 10 && fits; i++) {
		length = next_random(&state) % sizeof(pattern);
		for (j = 0; j < length; j++)
			pattern[j] = alphabet[next_random(&state) %
			    (sizeof(alphabet) - 1)];
		if (compile(&compiled, pattern, length))
			fits = compiled.count <= relic_regex_capacity(length);
		release(&compiled);
	}
	check(fits, SCENARIO_BOUND, "every program fits 3 * length + 1");

	for (length = 1; length <= sizeof(run); length++) {
		memset(run, 'a', sizeof(run));
		for (j = 1; j < length; j += 2)
			run[j] = '*';
		if (compile(&compiled, run, length))
			checkf(compiled.count <= relic_regex_capacity(length),
			    SCENARIO_BOUND, "a*a*... of %zu bytes fits", length);
		release(&compiled);
	}
}

/*
 * The reference: a tree by the grammar, and the positions a subtree ends
 * at from positions it begins at.
 */
static int
ref_node(reference_t *ref, enum node_kind kind, int left, int right)
{
	node_t *node;

	if (ref->count >= TREE_MAX) {
		ref->failed = true;
		return (-1);
	}
	node = &ref->nodes[ref->count];
	bzero(node, sizeof(*node));
	node->kind = kind;
	node->left = left;
	node->right = right;

	return (ref->count++);
}

static int
ref_fail(reference_t *ref, enum relic_regex_failure failure, size_t position)
{
	if (!ref->failed) {
		ref->failed = true;
		ref->failure = failure;
		ref->failed_at = position;
	}

	return (-1);
}

static int
ref_alternation(reference_t *ref)
{
	int tree;
	int right;

	tree = ref_branch(ref);
	while (tree >= 0 && ref->position < ref->length &&
	    ref->pattern[ref->position] == '|') {
		ref->position++;
		right = ref_branch(ref);
		if (right < 0)
			return (-1);
		tree = ref_node(ref, NODE_ALT, tree, right);
	}

	return (tree);
}

static int
ref_branch(reference_t *ref)
{
	int tree;
	int piece;
	uint8_t byte;

	tree = ref_node(ref, NODE_EMPTY, -1, -1);
	while (tree >= 0 && ref->position < ref->length) {
		byte = ref->pattern[ref->position];
		if (byte == '|')
			break;
		if (byte == ')') {
			if (ref->depth == 0)
				return (ref_fail(ref,
				    RELIC_RE_UNMATCHED_CLOSE, ref->position));
			break;
		}
		piece = ref_piece(ref);
		if (piece < 0)
			return (-1);
		tree = ref_node(ref, NODE_CAT, tree, piece);
	}

	return (tree);
}

static int
ref_piece(reference_t *ref)
{
	enum node_kind kind;
	int tree;
	uint8_t byte;

	byte = ref->pattern[ref->position];
	if (byte == '*' || byte == '+' || byte == '?')
		return (ref_fail(ref, RELIC_RE_NOTHING_TO_REPEAT,
		    ref->position));

	tree = ref_atom(ref);
	while (tree >= 0 && ref->position < ref->length) {
		byte = ref->pattern[ref->position];
		if (byte == '*')
			kind = NODE_STAR;
		else if (byte == '+')
			kind = NODE_PLUS;
		else if (byte == '?')
			kind = NODE_QUEST;
		else
			break;
		ref->position++;
		tree = ref_node(ref, kind, tree, -1);
	}

	return (tree);
}

static int
ref_atom(reference_t *ref)
{
	size_t open;
	int tree;
	uint8_t byte;

	byte = ref->pattern[ref->position++];
	switch (byte) {
	case '(':
		open = ref->position - 1;
		if (ref->depth == RELIC_REGEX_DEPTH_MAX)
			return (ref_fail(ref, RELIC_RE_TOO_DEEP, open));
		ref->depth++;
		tree = ref_alternation(ref);
		ref->depth--;
		if (tree < 0)
			return (-1);
		if (ref->position >= ref->length)
			return (ref_fail(ref, RELIC_RE_UNMATCHED_OPEN, open));
		ref->position++;
		return (tree);
	case '[':
		return (ref_set(ref));
	case '.':
		return (ref_node(ref, NODE_ANY, -1, -1));
	case '^':
		return (ref_node(ref, NODE_BOL, -1, -1));
	case '$':
		return (ref_node(ref, NODE_EOL, -1, -1));
	case '\\':
		if (ref->position >= ref->length)
			return (ref_fail(ref, RELIC_RE_TRAILING_BACKSLASH,
			    ref->position - 1));
		byte = ref->pattern[ref->position++];
		break;
	default:
		break;
	}

	tree = ref_node(ref, NODE_BYTE, -1, -1);
	if (tree >= 0)
		ref->nodes[tree].byte = byte;

	return (tree);
}

static int
ref_set(reference_t *ref)
{
	bool members[256];
	size_t open;
	unsigned int b;
	uint8_t low;
	uint8_t high;
	bool negated;
	bool first;
	int tree;

	open = ref->position - 1;
	bzero(members, sizeof(members));
	negated = ref->position < ref->length &&
	    ref->pattern[ref->position] == '^';
	if (negated)
		ref->position++;

	for (first = true;; first = false) {
		if (ref->position >= ref->length)
			return (ref_fail(ref, RELIC_RE_UNTERMINATED_SET, open));
		low = ref->pattern[ref->position++];
		if (low == ']' && !first)
			break;
		if (low == '\\') {
			if (ref->position >= ref->length)
				return (ref_fail(ref,
				    RELIC_RE_TRAILING_BACKSLASH,
				    ref->position - 1));
			members[ref->pattern[ref->position++]] = true;
			continue;
		}
		high = low;
		if (ref->position + 1 < ref->length &&
		    ref->pattern[ref->position] == '-' &&
		    ref->pattern[ref->position + 1] != ']' &&
		    ref->pattern[ref->position + 1] != '\\') {
			high = ref->pattern[ref->position + 1];
			if (high < low)
				return (ref_fail(ref, RELIC_RE_REVERSED_RANGE,
				    ref->position - 1));
			ref->position += 2;
		}
		for (b = low; b <= high; b++)
			members[b] = true;
	}

	tree = ref_node(ref, NODE_SET, -1, -1);
	if (tree >= 0) {
		for (b = 0; b < 256; b++)
			ref->nodes[tree].set[b] = members[b] != negated;
	}

	return (tree);
}

/*
 * Parse a pattern into the reference's tree.  Returns whether it is one of
 * the dialect; the root is the last node built.
 */
static bool
ref_parse(reference_t *ref, const char *pattern, size_t length)
{
	int tree;

	bzero(ref, sizeof(*ref));
	ref->pattern = (const uint8_t *)pattern;
	ref->length = length;

	tree = ref_alternation(ref);
	if (tree >= 0 && ref->position < ref->length)
		return (ref_fail(ref, RELIC_RE_UNMATCHED_CLOSE,
		    ref->position) >= 0);

	return (tree >= 0 && tree == ref->count - 1);
}

/*
 * The set of positions a subtree can end at, having begun at one of
 * `starts`, bit p standing for position p.  A repetition is the least set
 * closed under one more of its operand, found by iterating to the fixed
 * point, which empty iterations cannot keep from ending.
 */
static uint64_t
ref_ends(const reference_t *ref, int index, const uint8_t *text,
    size_t length, uint64_t starts)
{
	const node_t *node;
	uint64_t ends;
	uint64_t more;
	size_t p;

	node = &ref->nodes[index];
	ends = 0;
	switch (node->kind) {
	case NODE_EMPTY:
		return (starts);
	case NODE_BYTE:
	case NODE_ANY:
	case NODE_SET:
		for (p = 0; p < length; p++) {
			if ((starts & ((uint64_t)1 << p)) == 0)
				continue;
			if ((node->kind == NODE_BYTE &&
			    text[p] == node->byte) || node->kind == NODE_ANY ||
			    (node->kind == NODE_SET && node->set[text[p]]))
				ends |= (uint64_t)1 << (p + 1);
		}
		return (ends);
	case NODE_BOL:
		return (starts & 1);
	case NODE_EOL:
		return (starts & ((uint64_t)1 << length));
	case NODE_CAT:
		return (ref_ends(ref, node->right, text, length,
		    ref_ends(ref, node->left, text, length, starts)));
	case NODE_ALT:
		return (ref_ends(ref, node->left, text, length, starts) |
		    ref_ends(ref, node->right, text, length, starts));
	case NODE_QUEST:
		return (starts | ref_ends(ref, node->left, text, length,
		    starts));
	case NODE_PLUS:
		starts = ref_ends(ref, node->left, text, length, starts);
		/* FALLTHROUGH */
	case NODE_STAR:
		ends = starts;
		do {
			more = ends | ref_ends(ref, node->left, text, length,
			    ends);
			if (more == ends)
				break;
			ends = more;
		} while (true);
		return (ends);
	}

	return (0);
}

static int64_t
ref_match(const reference_t *ref, int root, const char *text, size_t length)
{
	size_t p;

	for (p = 0; p <= length; p++) {
		if (ref_ends(ref, root, (const uint8_t *)text, length,
		    (uint64_t)1 << p) != 0)
			return ((int64_t)p);
	}

	return (-1);
}

/*
 * A million random patterns and texts: the matcher and the reference agree
 * on whether each pattern is one, on why not and where, and on where the
 * leftmost match begins.
 */
static void
the_matcher_agrees_with_the_reference(void)
{
	static const char pattern_bytes[] = "ab.()|*+?^$[]-\\\0\xff";
	static const char text_bytes[] = "ab-]\n\0\xff";
	char pattern[RANDOM_PATTERN_MAX];
	char text[RANDOM_TEXT_MAX];
	static reference_t ref;
	compiled_t compiled;
	uint32_t state;
	size_t pattern_length;
	size_t text_length;
	size_t pairs;
	size_t texts;
	size_t i;
	int64_t expected;
	int64_t found;
	bool compiled_ok;
	bool parsed;
	bool agreed;

	state = 7;
	agreed = true;
	for (pairs = 0; pairs < RANDOM_PAIRS && agreed;) {
		pattern_length = next_random(&state) %
		    (RANDOM_PATTERN_MAX + 1);
		for (i = 0; i < pattern_length; i++)
			pattern[i] = pattern_bytes[next_random(&state) %
			    (sizeof(pattern_bytes) - 1)];

		parsed = ref_parse(&ref, pattern, pattern_length);
		compiled_ok = compile(&compiled, pattern, pattern_length);
		agreed = parsed == compiled_ok && (parsed ||
		    (ref.failure == compiled.error.failure &&
		    ref.failed_at == compiled.error.position));
		checkf(agreed, SCENARIO_REFERENCE, "pattern %zu of %zu bytes "
		    "should %s", pairs, pattern_length, parsed ?
		    "compile" : "fail as the reference does");

		for (texts = 0; texts < TEXTS_PER_PATTERN && agreed &&
		    compiled_ok; texts++, pairs++) {
			text_length = next_random(&state) %
			    (RANDOM_TEXT_MAX + 1);
			for (i = 0; i < text_length; i++)
				text[i] = text_bytes[next_random(&state) %
				    (sizeof(text_bytes) - 1)];
			expected = ref_match(&ref, ref.count - 1, text,
			    text_length);
			found = relic_regex_match(compiled.insts,
			    compiled.count, &compiled.work, text, text_length);
			agreed = found == expected;
			checkf(agreed, SCENARIO_REFERENCE, "pair %zu should "
			    "match at %lld, not %lld", pairs,
			    (long long)expected, (long long)found);
		}
		if (!compiled_ok)
			pairs++;
		release(&compiled);
	}
}

/*
 * One work space serves many programs one match at a time, and the stamp
 * that empties its marks wraps without leaving a mark that looks current.
 */
static void
a_work_space_serves_match_after_match(void)
{
	relic_regex_inst_t insts[16];
	relic_regex_error_t error;
	relic_regex_work_t work;
	uint32_t words[RELIC_REGEX_WORK_WORDS * 16];
	uint32_t count;
	bool agreed;
	int round;

	bzero(&work, sizeof(work));
	bzero(words, sizeof(words));
	work.words = words;
	work.capacity = 16;

	count = relic_regex_compile("(a|b)*c", 7, insts, &error);
	agreed = count > 0;
	work.stamp = UINT32_MAX - 3;
	for (round = 0; round < 4 && agreed; round++)
		agreed = relic_regex_match(insts, count, &work, "abxabc",
		    6) == 3;
	check(agreed && work.stamp < 100, SCENARIO_STAMP,
	    "matches across the stamp's wrap agree");

	count = relic_regex_compile("x$", 2, insts, &error);
	check(count > 0 && relic_regex_match(insts, count, &work, "axbx",
	    4) == 3, SCENARIO_STAMP, "a second program reuses the space");
}

int
main(void)
{
	each_construct_matches_what_it_means();
	each_error_is_found_at_its_byte();
	a_match_is_the_leftmost();
	nesting_stops_at_the_bound();
	no_pattern_takes_more_than_linear_time();
	a_program_fits_the_room_its_pattern_gives();
	the_matcher_agrees_with_the_reference();
	a_work_space_serves_match_after_match();

	return (check_summary("regex"));
}
