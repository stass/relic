/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_H
#define RELIC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The version relic was compiled against.  Only the three components are
 * written down; the string and the ordered number are derived from them, so
 * a release that bumps one of them cannot leave the others describing the
 * previous version.
 *
 * RELIC_VERSION_NUMBER orders releases so that a consumer can require a
 * minimum version in one comparison:
 *
 *	#if RELIC_VERSION_NUMBER < 10200
 *	#error relic 1.2.0 or later is required
 *	#endif
 *
 * The minor and patch fields are allotted two decimal digits each, so a
 * release must keep them below 100.
 */
#define	RELIC_VERSION_MAJOR	0
#define	RELIC_VERSION_MINOR	1
#define	RELIC_VERSION_PATCH	0

#define	RELIC_VERSION_TEXT_(v)	#v
#define	RELIC_VERSION_TEXT(v)	RELIC_VERSION_TEXT_(v)

#define	RELIC_VERSION_STRING \
	RELIC_VERSION_TEXT(RELIC_VERSION_MAJOR) "." \
	RELIC_VERSION_TEXT(RELIC_VERSION_MINOR) "." \
	RELIC_VERSION_TEXT(RELIC_VERSION_PATCH)

#define	RELIC_VERSION_NUMBER \
	((RELIC_VERSION_MAJOR * 10000) + (RELIC_VERSION_MINOR * 100) + \
	 RELIC_VERSION_PATCH)

/*
 * Mark a symbol as part of the library's public interface.  The library is
 * compiled with hidden visibility where the compiler supports it, so a
 * declaration that omits this would resolve within the library and be
 * unreachable from a program linked against it.  Expanding to nothing on a
 * compiler without visibility attributes is correct: there, nothing was
 * hidden in the first place.
 */
#if defined(__GNUC__) && __GNUC__ >= 4
#define	RELIC_EXPORT	__attribute__((__visibility__("default")))
#else
#define	RELIC_EXPORT
#endif

/*
 * The version of the library the caller is linked against, formatted as
 * "major.minor.patch".  It need not equal RELIC_VERSION_STRING: that macro
 * records the version the caller was compiled against, and a shared library
 * may since have been upgraded underneath it.  The returned string is static
 * and outlives the caller.
 */
RELIC_EXPORT const char *relic_version(void);

/*
 * A relic context: a loaded program, the values it mentions, and the
 * relations derived from it.  Contexts share nothing, so a program may hold
 * as many as it likes, but one context is not itself thread-safe.
 */
typedef struct relic relic_t;

typedef struct relic_config relic_config_t;

/*
 * Limits a context enforces, and switches that select internal behaviour.
 * A zero limit means unlimited, so a zeroed structure asks for the defaults.
 *
 * The fields correspond one to one with the tool's repeatable -l key=value
 * options, which exist only to parse into this structure: a limit is defined
 * in one place and nowhere else.
 *
 * `max_tuples` counts tuples across every relation and `max_memory` the bytes
 * their storage holds.  Exceeding either is an ordinary error: the tuple is
 * refused, a diagnostic names the limit and its configured value, and the
 * relation is left as it was.
 *
 * `max_memory` bounds relation and index storage, which is what grows with
 * the data, and not the memory the library uses in total: the loaded program,
 * the symbols and values it mentions, and the working state of a query are
 * sized by the program rather than by what it derives.  Nor is it a bound on
 * peak usage, since growing an array holds the old one until the new one is
 * filled.
 *
 * `max_magic_preds` and `max_magic_rules` are accepted and stored but not yet
 * acted on: nothing generates a Magic predicate or rule to count, so setting
 * either has no effect.
 *
 * `flags` holds RELIC_F_ switches, each of which selects an alternative
 * evaluation strategy that must give the same answers as the default.  They
 * exist so that strategies can be compared against one another, and are not
 * meant for ordinary use.  Bits no switch defines are ignored.
 */
struct relic_config {
	size_t		max_memory;	/* relation and index storage, bytes */
	size_t		max_tuples;
	size_t		max_magic_preds;
	size_t		max_magic_rules;
	unsigned int	flags;
};

/*
 * Evaluate recursion with the naive fixed point, which reruns every rule of
 * a recursive component against whole relations until nothing new is
 * derived, instead of the semi-naive one, which after the first round
 * reruns only the rule variants reading what the previous round derived.
 * The naive evaluator is kept as the reference the default is tested
 * against.
 */
#define	RELIC_F_NAIVE	0x0001u

/*
 * Create a context.  Returns NULL when memory is exhausted; there is no
 * other way for it to fail, so no diagnostic callback can be installed yet
 * and none is needed.
 *
 * relic_create() is relic_create_config() with every limit at its default.
 * relic_destroy() releases everything the context owns and accepts NULL.
 */
RELIC_EXPORT relic_t *relic_create(void);
RELIC_EXPORT relic_t *relic_create_config(const relic_config_t *config);
RELIC_EXPORT void relic_destroy(relic_t *ctx);

/*
 * The three value types.  They are distinct domains: the symbol foo and the
 * string "foo" are never equal, and a comparison between values of different
 * types is never true.
 */
enum relic_type {
	RELIC_SYMBOL,
	RELIC_STRING,
	RELIC_INTEGER
};

typedef struct relic_value relic_value_t;

/*
 * A value, as a caller sees it.  This is a complete type because callers
 * construct values to insert and read values out of query results.
 *
 * For RELIC_SYMBOL and RELIC_STRING, `v.id` identifies interned bytes that
 * relic_string() returns.  For RELIC_INTEGER, `v.integer` is the value.
 */
struct relic_value {
	enum relic_type	type;
	union {
		uint32_t	id;
		int64_t		integer;
	} v;
};

/*
 * The bytes of an interned symbol or string, or NULL for an identifier this
 * context never assigned.
 *
 * The result is NUL-terminated for the convenience of a caller that wants a
 * C string, but a string literal may contain NUL bytes, so a caller that must
 * not lose them passes lenp and uses the length.  The bytes belong to the
 * context and stay valid until it is destroyed; the caller neither frees nor
 * modifies them.
 */
RELIC_EXPORT const char *relic_string(relic_t *ctx, uint32_t id,
    size_t *lenp);

typedef struct relic_location relic_location_t;

/*
 * Where in the source a diagnostic points.  Line and column count from one;
 * `file` is the source name the text was loaded under and may be NULL when
 * the diagnostic belongs to no particular file.
 */
struct relic_location {
	const char	*file;
	uint32_t	 line;
	uint32_t	 column;
};

enum relic_diag_level {
	RELIC_NOTE,
	RELIC_WARNING,
	RELIC_ERROR
};

/*
 * Receives every diagnostic the context produces: the context pointer given
 * when it was installed, the level, where the diagnostic points or NULL, and
 * the message.
 *
 * Both the message and the location are borrowed and do not outlive the
 * call; a callback that keeps either copies it. The `file` name inside a
 * location is the exception and belongs to the context, so it stays valid
 * until the context is destroyed.
 */
typedef void (*relic_diag_cb)(
    void *,
    enum relic_diag_level,
    const relic_location_t *,
    const char *);

/*
 * Install a diagnostic callback, replacing any previous one.  Passing NULL
 * discards diagnostics, which is the default: a library must not write to
 * its caller's stderr uninvited, and an error remains recoverable through
 * relic_error() whether or not a callback saw it.
 *
 * A diagnostic can arrive in the middle of loading, checking or evaluating,
 * when the context is part way through an operation and cannot be entered
 * again.  The callback therefore must not call into the context, except to
 * read with relic_string() and relic_error().
 */
RELIC_EXPORT void relic_diag_set(relic_t *ctx, relic_diag_cb callback,
    void *callback_ctx);

/*
 * A diagnostic callback that writes to stderr, for a program that wants one
 * and has no opinion about the format.  This is what the relic tool installs.
 *
 * A located diagnostic is written as "file:line:column: level: message" and
 * an unlocated one as "prefix: level: message", where the prefix is the
 * callback context read as a NUL-terminated program name and is omitted when
 * that is NULL.
 *
 * Bytes outside printable ASCII are escaped, in the message and in the file
 * name alike.  Both can carry text taken from a source file or a command
 * line, and a diagnostic must not be able to send a terminal escape sequence
 * to whoever is reading it.
 */
RELIC_EXPORT void relic_diag_stderr(void *callback_ctx,
    enum relic_diag_level level, const relic_location_t *where,
    const char *message);

/*
 * Load a program.
 *
 * relic_load_path() opens the file and delegates; relic_load_file() reads
 * the stream whole and delegates; relic_load_string() takes a
 * NUL-terminated program text.  `source_name` is what diagnostics from that
 * text point at and is copied, so it need not outlive the call; NULL names
 * the input "<input>".
 *
 * Each returns 0, or -1 with every mistake already reported through the
 * diagnostic callback.  Several loads append to one logical program, which
 * is what lets facts and rules be split across files.
 *
 * A load is all or nothing: a failed one adds none of its statements, so a
 * program never holds half of a file.  A program with an error in it is not
 * a smaller program, it is a different one.
 */
RELIC_EXPORT int relic_load_file(relic_t *ctx, FILE *stream,
    const char *source_name);
RELIC_EXPORT int relic_load_path(relic_t *ctx, const char *path);
RELIC_EXPORT int relic_load_string(relic_t *ctx, const char *source_name,
    const char *text);

/*
 * Validate the loaded program, returning 0 when it is usable and -1 when it
 * is not, with every mistake already reported through the diagnostic
 * callback.
 *
 * A program is rejected when a rule is not range restricted: every variable
 * in a rule head, in a negated literal and in a comparison must also occur in
 * a positive relational body literal, because nothing else can bind it.
 * Warnings that do not reject a program are reported too, among them a
 * variable a rule mentions only once and a predicate the program references
 * and never defines.
 *
 * A check compiles only the statements loaded since the previous one, so
 * calling it twice with nothing loaded in between reports nothing the second
 * time and returns the same answer. It stays -1 once a program has contained
 * a rejected rule: loading more cannot repair one already loaded.
 *
 * The queries a program contains are part of it and are checked the same
 * way: every output column of a query, which is to say every variable it
 * names, must occur in a positive relational literal of the query.
 */
RELIC_EXPORT int relic_check(relic_t *ctx);

/*
 * Evaluate the whole program, computing every tuple its facts and rules
 * derive.  Returns 0, or -1 with the reason already reported.
 *
 * Recursion is evaluated semi-naively unless the context was created with
 * RELIC_F_NAIVE; the two give the same relations.
 *
 * The program is checked first and must pass.  Evaluation fails when a
 * configured limit refuses a tuple, when memory runs out, or when a rule
 * holds a negated literal, which the evaluator does not execute.  A failure
 * leaves every relation holding only tuples the program justifies, and the
 * next evaluation continues from them.
 *
 * Nothing is recomputed when nothing has been loaded since the last
 * evaluation that succeeded.  A query materializes the program before it is
 * answered, so a caller that only queries need not call this.
 */
RELIC_EXPORT int relic_materialize(relic_t *ctx);

/*
 * A query, compiled and ready to run as often as the caller likes.
 */
typedef struct relic_query relic_query_t;

/*
 * Receives one row of a query's answer: the callback context given to
 * relic_query_run(), the row's values, and how many there are.
 *
 * A row holds one value per output column, and the columns are the variables
 * the query names, in the order they first occur, with `_` contributing none.
 * A query that names no variable asks whether something holds, and its
 * answer is one row of no values when it does and no rows when it does not.
 *
 * The values are borrowed for the duration of the call; a callback that keeps
 * a row copies it.  The bytes of a symbol or string belong to the context and
 * stay valid until it is destroyed.  The callback returns 0 to receive the
 * next row and nonzero to stop.
 */
typedef int (*relic_row_cb)(void *, const relic_value_t *, size_t);

/*
 * Prepare a query from text, such as "path(a, X)?".  Returns the query, or
 * NULL with every mistake reported: the text must be exactly one query as the
 * grammar writes it, ending in `?`, and the query must be safe.  Diagnostics
 * about the text point at `source_name`, which is copied and may be NULL to
 * name the text "<query>"; a caller preparing several queries names each so
 * that a message says which one it is about.
 *
 * The loaded program is checked first and must pass, since a query against a
 * program that cannot be evaluated could never be run.  Preparing a query
 * adds nothing to the program: a query that is wrong is refused, and the
 * program stays as valid as it was.  The predicates a query names are
 * recorded like any others, so one that nothing defines draws the same
 * warning it would in a rule, once.
 *
 * The query belongs to the caller, who releases it with relic_query_destroy()
 * before the context is destroyed.
 */
RELIC_EXPORT relic_query_t *relic_query_prepare(relic_t *ctx,
    const char *source_name, const char *text);

/*
 * The queries written in the loaded program.
 *
 * relic_query_count() is how many there are, in load order.
 * relic_query_loaded() returns the one at `index`, counting from zero, as a
 * query like any prepared one, or NULL when the program does not pass its
 * check or holds no such query.  The query belongs to the caller, who
 * releases it with relic_query_destroy().
 */
RELIC_EXPORT size_t relic_query_count(relic_t *ctx);
RELIC_EXPORT relic_query_t *relic_query_loaded(relic_t *ctx, size_t index);

/*
 * Answer a query, passing each row of the answer to `callback` in turn.
 * Returns 0 when every row was delivered or the callback asked to stop, and
 * -1 with the reason reported when the query could not be answered.
 *
 * The program is evaluated first, as relic_materialize() does, and no row is
 * delivered until the whole answer is known.  A query that fails -- a limit
 * exceeded, say -- therefore delivers no rows at all rather than a truncated
 * answer that would be indistinguishable from a complete one.
 *
 * Rows are distinct, because a query's answer is a set.  Their order is
 * unspecified by the language but is the same on every run over the same
 * input; relic_value_compare() orders them when a caller needs an order that
 * does not depend on that.
 *
 * The callback may read strings through the context with relic_string() and
 * compare values with relic_value_compare().  It must not otherwise call into
 * the context -- to load, to prepare or run a query, or to destroy the query
 * or the context -- while rows are being delivered.
 */
RELIC_EXPORT int relic_query_run(relic_t *ctx, relic_query_t *query,
    relic_row_cb callback, void *callback_ctx);

/*
 * Release a query.  Accepts NULL.
 */
RELIC_EXPORT void relic_query_destroy(relic_query_t *query);

/*
 * Compare two values in the total order used to sort results, returning a
 * negative number, zero or a positive number as `a` sorts before, with or
 * after `b`.
 *
 * Every integer sorts before every symbol and every symbol before every
 * string; integers then compare numerically, and symbols and strings
 * bytewise.  The order exists so that output can be made stable when a
 * column holds more than one type, and it is not the order the language's
 * comparison operators use: those never order values of different types.
 * Both values must belong to `ctx`.
 */
RELIC_EXPORT int relic_value_compare(relic_t *ctx, const relic_value_t *a,
    const relic_value_t *b);

typedef struct relic_stats relic_stats_t;

/*
 * Counters describing the work a context has done and what it holds, for
 * understanding and comparing evaluation strategies.
 *
 * The first four accumulate over the life of the context, and count the
 * rules of the program and of every query answered alike:
 *
 *	iterations	rounds of fixed-point evaluation of recursive
 *			components; a component that is not recursive is
 *			evaluated in one pass, which is not counted
 *	derivations	head tuples produced by rules, facts included, whether
 *			or not the relation already held them
 *	tuples_inserted	derivations that added a tuple to its relation
 *	tuples_scanned	tuples read while scanning relations
 *
 * The rest describe the context when the counters are read: the tuples held
 * across every relation, the answers of queries included, the bytes of
 * relation and index storage they occupy, and the distinct values interned,
 * by type.
 *
 * The counts depend on the evaluation strategy and on how the engine is
 * implemented, and are not stable from one release to the next; only the
 * answers are.  The structure's layout is part of the binary interface, so a
 * release that adds a counter changes the library's interface version.
 */
struct relic_stats {
	uint64_t	iterations;
	uint64_t	derivations;
	uint64_t	tuples_inserted;
	uint64_t	tuples_scanned;
	size_t		tuples;
	size_t		storage_bytes;
	size_t		symbol_values;
	size_t		string_values;
	size_t		integer_values;
};

/*
 * Fill *stats, which must not be NULL, with the context's counters.
 */
RELIC_EXPORT void relic_stats(relic_t *ctx, relic_stats_t *stats);

/*
 * The text of the most recent RELIC_ERROR diagnostic, or NULL if the context
 * has reported none.  The string belongs to the context and is replaced by
 * the next error, so a caller that needs to keep it copies it.
 *
 * The text is raw.  A diagnostic may quote an identifier, a string literal
 * or a file name taken from whatever relic was asked to read, so it can
 * contain control characters and terminal escape sequences.  A caller
 * sending it to a terminal should escape it, as relic_diag_stderr() does;
 * escaping it here would corrupt it for a caller that renders it some other
 * way.
 */
RELIC_EXPORT const char *relic_error(relic_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RELIC_H */
