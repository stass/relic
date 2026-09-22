/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_INTERNAL_H
#define RELIC_INTERNAL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <relic.h>

#include "arena.h"
#include "ast.h"
#include "compat.h"
#include "plan.h"
#include "program.h"
#include "queue.h"
#include "relation.h"
#include "util.h"

/*
 * Types and prototypes shared across lib/.  Nothing here is installed and
 * nothing here is exported from the shared library: every symbol declared
 * below has external linkage so that the modules can call one another, and
 * hidden visibility so that a program linked against librelic cannot.
 */

/*
 * Mark a parameter the implementation does not use.  A function whose shape
 * is fixed by a callback type will have some; writing the attribute is
 * clearer than a (void) cast in the body, which reads like a statement that
 * does something.
 */
#if defined(__GNUC__)
#define	RELIC_UNUSED	__attribute__((__unused__))
#else
#define	RELIC_UNUSED
#endif

/*
 * Ask the compiler to check a printf-style call against its format string.
 * The diagnostic interface is the one place in the library where a format
 * string and its arguments are separated by a call boundary.
 */
#if defined(__GNUC__)
#define	RELIC_PRINTFLIKE(fmt, args) \
	__attribute__((__format__(__printf__, fmt, args)))
#else
#define	RELIC_PRINTFLIKE(fmt, args)
#endif

typedef struct relic_counters relic_counters_t;
typedef struct relic_diag relic_diag_t;
typedef struct relic_lexer relic_lexer_t;
typedef struct relic_symbol relic_symbol_t;
typedef struct relic_symtab relic_symtab_t;
typedef struct relic_token relic_token_t;
typedef struct relic_valtab relic_valtab_t;

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
 * Where diagnostics go, and the text of the last error.
 *
 * With no callback installed, diagnostics are discarded and an error is still
 * recoverable through relic_error().  Silence is the right default for a
 * library: a caller must not be made to accept output on its stderr.
 *
 * `error_count` counts every error reported, which is how an operation that
 * failed learns whether anything said why.
 */
struct relic_diag {
	relic_diag_cb	 callback;
	void		*callback_ctx;
	size_t		 error_count;
	char		 last_error[512];
	bool		 has_error;
};

/*
 * One interned byte string, found at index `id - 1` of the symbol table.
 *
 * The bytes live in the context arena, where they never move, so a pointer
 * handed out by relic_string() stays valid until the context is destroyed
 * even after the array of these has been reallocated several times.
 *
 * The length is recorded because a string literal may contain NUL bytes: the
 * lexer is byte-oriented and a string holds whatever bytes the source did.  A
 * terminator is stored past the recorded length anyway, so that a caller with
 * no use for the length can treat the result as a C string.
 */
struct relic_symbol {
	const char	*bytes;
	size_t		 length;
};

/*
 * Byte strings interned to 32-bit identifiers.
 *
 * Symbols and string literals share this pool.  What distinguishes the symbol
 * foo from the string "foo" is the value that refers to the bytes, not the
 * bytes themselves, so the two intern to one symbol identifier and to two
 * different domain identifiers.
 */
struct relic_symtab {
	relic_symbol_t	*entries;	/* indexed by id - 1 */
	size_t		 count;
	size_t		 capacity;
	relic_idtab_t	 index;
};

/*
 * Values interned to domain identifiers.
 *
 * Interning happens here for every type, integers included, so that the
 * parser and relic_fact_add() cannot disagree about a value's canonical
 * form: there is one path to a domain identifier and this is it.
 */
struct relic_valtab {
	relic_value_t	*entries;	/* indexed by dom - 1 */
	size_t		 count;
	size_t		 capacity;
	relic_idtab_t	 index;
};

/*
 * The work evaluation has done over the life of a context, which the
 * executor advances as it runs and relic_stats() reports.  Each field is
 * defined where relic_stats_t documents the one of the same name.
 */
struct relic_counters {
	uint64_t	iterations;
	uint64_t	derivations;
	uint64_t	tuples_inserted;
	uint64_t	tuples_scanned;
};

/*
 * A query ready to run: a compiled rule over the query's answer predicate.
 *
 * A query prepared from text owns `arena`, which holds its syntax tree and
 * the variable-length parts of its rule, and is released when the query is
 * destroyed.  A query written in the program borrows its rule from the
 * program arena and leaves `arena` empty.  Either way the rule is a copy held
 * here rather than a pointer into the program's table of queries, which is a
 * growable array and moves.
 *
 * `ctx` is the context the query was compiled against.  Its predicates, its
 * constants and its answer relation all belong to that context, so running it
 * against another would read identifiers that mean something else there.
 */
struct relic_query {
	relic_t		*ctx;
	relic_arena_t	 arena;
	relic_rule_t	 rule;
};

/*
 * A relic context owns everything it allocates, and two contexts share
 * nothing: the library holds no mutable global state at all, which is what
 * lets an embedding program keep several contexts without a lock.  A single
 * context is not itself thread-safe.
 *
 * `materialized_rules` is how many of the program's rules the relations are
 * a fixed point of.  Rules are only ever appended, so the relations are
 * current exactly when it equals the number of compiled rules, and an
 * evaluation after a later load continues from what they already hold.  It
 * advances only when a fixed point is reached, so a failed evaluation leaves
 * it where it was and the next one tries again.
 *
 * `counters` is the work evaluation has done.  What the context holds is
 * not counted here: relic_stats() measures it when asked.
 */
struct relic {
	relic_arena_t		 arena;	/* context lifetime: symbol bytes */
	relic_arena_t		 program_arena;	/* the parsed program */
	relic_arena_t		 eval_arena;	/* one evaluation's plans */
	relic_symtab_t		 symbols;
	relic_valtab_t		 values;
	relic_ast_program_t	 ast;
	relic_program_t		 program;
	relic_reltab_t		 relations;
	relic_diag_t		 diag;
	relic_config_t		 config;
	relic_counters_t	 counters;
	size_t			 materialized_rules;
};

/*
 * lib/api.c
 *
 * Allocate from the context arena, reporting exhaustion and returning NULL.
 *
 * Every context-lifetime allocation goes through here rather than calling
 * the arena directly, because that arena is never reset while the context
 * lives.  A failure flag left standing on it would refuse every later
 * request and turn one transient exhaustion into a context that can no
 * longer intern anything, so the flag is cleared here, once the failure has
 * been observed and reported.
 *
 * The flag's other purpose, letting a half-finished construction stop
 * without threading a status back through every level, belongs to the arenas
 * that are reset between phases.  A context-lifetime allocation is a single
 * object whose caller checks the pointer it gets.
 */
void	*relic_context_alloc(relic_t *ctx, size_t size);

/*
 * lib/diag.c
 *
 * relic_diagf() formats a diagnostic, records it as the last error when its
 * level is RELIC_ERROR, and hands it to the installed callback.  A message
 * longer than the buffer is truncated rather than allocated for: a
 * diagnostic reports a failure and must not be able to cause one.
 */
void	 relic_diag_init(relic_diag_t *diag);
void	 relic_diagf(relic_t *ctx, enum relic_diag_level level,
	    const relic_location_t *where, const char *format, ...)
	    RELIC_PRINTFLIKE(4, 5);

/*
 * The same, for a caller that already holds a va_list.  The parser formats
 * its own messages through this because it decides whether a diagnostic is
 * reported at all, and that decision has to sit in front of the call rather
 * than inside it.
 */
void	 relic_vdiagf(relic_t *ctx, enum relic_diag_level level,
	    const relic_location_t *where, const char *format, va_list arguments)
	    RELIC_PRINTFLIKE(4, 0);

/*
 * Report that an allocation failed, in the one wording the whole library
 * uses.  Out-of-memory is the failure every module can hit and the one whose
 * message carries the least information, so it is written once.
 */
void	 relic_diag_no_memory(relic_t *ctx);

/*
 * Shorten a name for a message into a caller-owned buffer, marking the cut
 * with "..." when one was made, and return the buffer so that the call can
 * stand as an argument of the message it decorates.
 */
const char *relic_diag_name(const char *name, char *buffer, size_t size);

/*
 * lib/symbol.c
 */
const char *relic_symbol_bytes(const relic_t *ctx, uint32_t id,
	    size_t *lengthp);
size_t	 relic_symbol_count(const relic_t *ctx);
int	 relic_symbol_intern(relic_t *ctx, const void *bytes, size_t length,
	    uint32_t *idp);
void	 relic_symtab_init(relic_symtab_t *table);
void	 relic_symtab_reset(relic_symtab_t *table);

/*
 * lib/value.c
 *
 * relic_value_of() returns NULL for an identifier the context never assigned.
 * Both order functions treat such an identifier as unordered and unequal
 * rather than reading past the table.
 */
size_t	 relic_value_count(const relic_t *ctx);
size_t	 relic_value_count_of(const relic_t *ctx, enum relic_type type);
const relic_value_t *relic_value_of(const relic_t *ctx, relic_dom_t dom);
enum relic_order relic_value_order(const relic_t *ctx, relic_dom_t a,
	    relic_dom_t b);
int	 relic_value_total_order(const relic_t *ctx, relic_dom_t a,
	    relic_dom_t b);
int	 relic_value_intern(relic_t *ctx, const relic_value_t *value,
	    relic_dom_t *domp);
int	 relic_value_intern_bytes(relic_t *ctx, enum relic_type type,
	    const void *bytes, size_t length, relic_dom_t *domp);
int	 relic_value_intern_integer(relic_t *ctx, int64_t value,
	    relic_dom_t *domp);
void	 relic_valtab_init(relic_valtab_t *table);
void	 relic_valtab_reset(relic_valtab_t *table);

/*
 * lib/lex.c
 *
 * The lexer decides what a byte sequence is; it emits no diagnostics.  A
 * sequence that is not a token comes back as RELIC_TOKEN_ERROR carrying the
 * reason, and the parser decides whether to report it.  That is what keeps
 * error recovery in one module: a lexer that reported for itself would have
 * to be told when the parser is discarding wreckage and wants silence.
 */

enum relic_token_kind {
	RELIC_TOKEN_END,	/* no more input */
	RELIC_TOKEN_SYMBOL,	/* [a-z][A-Za-z0-9_]* */
	RELIC_TOKEN_VARIABLE,	/* [A-Z_][A-Za-z0-9_]* */
	RELIC_TOKEN_STRING,
	RELIC_TOKEN_INTEGER,
	RELIC_TOKEN_COMPARE,	/* = != < <= > >=, which one in `compare` */
	RELIC_TOKEN_IMPLIES,	/* :- */
	RELIC_TOKEN_LPAREN,
	RELIC_TOKEN_RPAREN,
	RELIC_TOKEN_COMMA,
	RELIC_TOKEN_DOT,
	RELIC_TOKEN_QUESTION,
	RELIC_TOKEN_ERROR	/* not a token; `error` says why */
};

enum relic_lex_error {
	RELIC_LEX_OK,
	RELIC_LEX_BAD_BYTE,		/* part of no token */
	RELIC_LEX_BAD_ESCAPE,		/* not one of the escapes */
	RELIC_LEX_BAD_HEX_ESCAPE,	/* \x without two hex digits */
	RELIC_LEX_EXPECTED_IMPLIES,	/* : not followed by - */
	RELIC_LEX_EXPECTED_NE,		/* ! not followed by = */
	RELIC_LEX_INTEGER_RANGE,	/* outside int64_t */
	RELIC_LEX_INTEGER_TRAILING,	/* an identifier byte after the digits */
	RELIC_LEX_UNTERMINATED_STRING
};

/*
 * One token.
 *
 * `text` spans the bytes the token was read from and belongs to the source
 * text, so it is valid only while the lexer's input is; diagnostics quote it
 * and nothing stores it.  `where` is the token's first byte, except where a
 * scanner found a more precise place to point at, such as the offending
 * byte of a bad escape.
 *
 * `symbol` is the interned spelling of a name and the interned *decoded*
 * bytes of a string literal.  The lexer interns bytes and not values: an
 * identifier does not yet know whether it names a predicate or is a symbol
 * constant, and interning it as a value on sight would give every predicate
 * name a domain identifier it never uses.
 */
struct relic_token {
	const char		*text;
	size_t			 text_length;
	relic_location_t	 where;
	int64_t			 integer;	/* RELIC_TOKEN_INTEGER */
	uint32_t		 symbol;	/* names and string bytes */
	enum relic_token_kind	 kind;
	enum relic_lex_error	 error;
	enum relic_compare_op	 compare;	/* RELIC_TOKEN_COMPARE */
	unsigned char		 byte;		/* the byte an error names */
};

/*
 * A scan in progress.
 *
 * `text` must hold a NUL at index `length`.  strtoll(3) is what needs it,
 * and nothing else: the terminator is what stops a conversion from reading
 * past a digit span that ends at the end of the buffer.  A NUL *inside* the
 * range is an ordinary byte that begins no token.
 *
 * `decoded` is the scratch buffer a string literal's escapes are decoded
 * into before the result is interned.  It is reused by every literal in the
 * input and released by relic_lexer_reset().
 */
struct relic_lexer {
	relic_t			*ctx;
	const char		*text;
	char			*decoded;
	size_t			 length;
	size_t			 offset;
	size_t			 decoded_length;
	size_t			 decoded_capacity;
	relic_location_t	 where;		/* of the byte at `offset` */
};

void	 relic_lexer_init(relic_lexer_t *lexer, relic_t *ctx,
	    const char *source_name, const char *text, size_t length);
void	 relic_lexer_reset(relic_lexer_t *lexer);

/*
 * Scan the next token.  Returns 0 having filled *token, which may be an
 * error token, or -1 when interning failed for want of memory, in which case
 * the diagnostic has already been reported and scanning cannot continue.
 */
int	 relic_lexer_next(relic_lexer_t *lexer, relic_token_t *token);

/*
 * lib/parse.c
 *
 * relic_ast_program_init() prepares the empty statement list a context
 * starts with.  The statements themselves live in the program arena, which
 * the context releases when it is destroyed, so there is no matching
 * teardown.
 */
void	 relic_ast_program_init(relic_ast_program_t *program);

/*
 * Parse text that must hold exactly one query, into `arena` rather than into
 * the program.  Returns 0 with the statement in *statementp, or -1 with every
 * mistake reported: a syntax error, or text that is not one query.
 */
int	 relic_parse_query(relic_t *ctx, relic_arena_t *arena,
	    const char *source_name, const char *text,
	    const relic_ast_stmt_t **statementp);

/*
 * lib/pred.c
 *
 * relic_pred_intern() assigns an identifier to a (name, arity) pair, or
 * returns the one it already has.  `where` is recorded only for a pair this
 * is the first mention of, so a later message points at the first place the
 * program named the predicate.
 *
 * relic_pred_describe() writes "name/arity" into the caller's buffer, which
 * must hold RELIC_PRED_TEXT_MAX bytes, and returns it so that the call can
 * stand as an argument of the message it decorates.
 *
 * relic_pred_is_answer() says whether a predicate is a query's synthetic
 * answer predicate, which a message about the query must not name.
 */
void	 relic_predtab_init(relic_predtab_t *table);
void	 relic_predtab_reset(relic_predtab_t *table);
int	 relic_pred_intern(relic_t *ctx, uint32_t name, uint16_t arity,
	    const relic_location_t *where, relic_pred_id_t *idp);
const relic_pred_t *relic_pred_of(const relic_t *ctx, relic_pred_id_t id);
size_t	 relic_pred_count(const relic_t *ctx);
void	 relic_pred_define(relic_t *ctx, relic_pred_id_t id);
const char *relic_pred_describe(const relic_t *ctx, relic_pred_id_t id,
	    char *buffer, size_t size);
bool	 relic_pred_is_answer(const relic_t *ctx, relic_pred_id_t id);
void	 relic_pred_report_undefined(relic_t *ctx);

/*
 * lib/rule.c
 *
 * relic_program_init() prepares the empty program a context starts with, and
 * relic_program_reset() releases what the program owns outside the program
 * arena, the schedule's arena among it.
 * The public entry point into this module is relic_check().
 *
 * relic_program_ready() is the check every use of the program begins with:
 * preparing a query, handing out one of the program's own, or evaluating.
 * Returns 0, or -1 with at least one error reported by this call.
 */
void	 relic_program_init(relic_program_t *program);
int	 relic_program_ready(relic_t *ctx);
void	 relic_program_reset(relic_program_t *program);

/*
 * lib/stratify.c
 *
 * relic_stratify() brings the program's schedule up to date with its rules,
 * rejecting a program with recursion through negation.  Returns 0 when the
 * analysis finished -- a program found unstratified has then been reported
 * and marked invalid -- or -1, reported, when memory ran out.
 */
void	 relic_schedule_init(relic_schedule_t *schedule);
void	 relic_schedule_reset(relic_schedule_t *schedule);
int	 relic_stratify(relic_t *ctx);

/*
 * lib/plan.c
 *
 * Plan one rule into `arena`, creating the relations it reads and writes.
 * `delta_literal` is RELIC_PLAN_NO_DELTA for the rule's ordinary plan, or the
 * body index of the positive literal a semi-naive variant reads from the
 * delta.  Returns 0, or -1 with a diagnostic reported: memory ran out, or the
 * rule holds a literal the evaluator cannot execute.
 */
int	 relic_plan_rule(relic_t *ctx, relic_arena_t *arena,
	    const relic_rule_t *rule, size_t delta_literal, relic_plan_t *plan);

/*
 * lib/relation.c
 *
 * relic_relation_for() is the only way to obtain a relation: one is created
 * empty for a predicate the first time it is asked for, so no caller decides
 * whether a relation exists yet.  A relation belongs to the context and never
 * moves, so a caller may hold as many of them as it likes for as long as the
 * context lives.
 *
 * Every function below takes a relation of the context it was obtained from.
 *
 * relic_relation_insert() reports through *addedp whether the tuple was not
 * already present, which is what fixed-point detection reads, and returns -1
 * with a diagnostic when memory or a configured limit refuses it.  A refused
 * insertion adds no tuple and leaves the relation's contents and its index
 * agreeing with each other; capacity that a partial reservation obtained is
 * kept, and accounted.
 *
 * relic_relation_all() and relic_relation_since() give the identifier ranges
 * a scan and a semi-naive delta walk, and relic_relation_end() is the mark a
 * later delta is measured from.
 *
 * The two totals are what the configurable tuple and memory limits are
 * enforced against: tuples across every relation, and the bytes their columns
 * and index slots hold.
 */
void	 relic_reltab_init(relic_reltab_t *table);
void	 relic_reltab_reset(relic_reltab_t *table);
relic_relation_t *relic_relation_for(relic_t *ctx, relic_pred_id_t pred);
int	 relic_relation_insert(relic_t *ctx, relic_relation_t *relation,
	    const relic_dom_t *columns, bool *addedp);
bool	 relic_relation_contains(const relic_relation_t *relation,
	    const relic_dom_t *columns);
const relic_dom_t *relic_relation_tuple(const relic_relation_t *relation,
	    relic_tuple_id_t id);
size_t	 relic_relation_bytes(const relic_relation_t *relation);
size_t	 relic_relation_count(const relic_relation_t *relation);
relic_tuple_id_t relic_relation_end(const relic_relation_t *relation);
relic_tuple_range_t relic_relation_all(const relic_relation_t *relation);
relic_tuple_range_t relic_relation_since(const relic_relation_t *relation,
	    relic_tuple_id_t first);
size_t	 relic_relation_total_bytes(const relic_t *ctx);
size_t	 relic_relation_total_tuples(const relic_t *ctx);

#endif /* RELIC_INTERNAL_H */
