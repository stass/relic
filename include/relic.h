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
 *
 * A function that takes a context and can fail returns -1 or NULL when it
 * does, and 0 or a pointer otherwise, and a failure is always explained by a
 * RELIC_ERROR diagnostic, the last of which relic_error() returns.  The
 * functions that only read -- relic_string(), relic_value_compare(),
 * relic_query_count(), relic_stats() -- cannot fail and report nothing;
 * relic_string() returns NULL for an identifier it does not know.
 *
 * Caller code runs while the context is part way through an operation in
 * four ways: the diagnostic, explain and row callbacks, and the read
 * function of a stream given to relic_load_file().  It may read through the
 * context with the functions above and relic_error().  Any other call on the
 * same context is refused: a call that returns a status fails with -1 or
 * NULL, one that returns nothing does nothing, and relic_error() describes
 * the refusal until the next error or until the operation the caller code
 * runs inside returns; the diagnostic callback is not called for it.  A
 * refused relic_destroy() releases nothing, so a callback that tries to
 * destroy its context leaks it rather than freeing memory in use.  A
 * callback must return: one that leaves by longjmp() or an exception leaves
 * its context refusing every later call.
 */
typedef struct relic relic_t;

typedef struct relic_config relic_config_t;

/*
 * Limits a context enforces, and switches that select internal behaviour.
 * A zeroed structure asks for the defaults: no bound on memory or tuples,
 * and for the Magic Sets rewrite the budget described below.  SIZE_MAX sets
 * no bound on anything.
 *
 * The fields correspond one to one with the tool's repeatable -l key=value
 * options, which exist only to parse into this structure: a limit is defined
 * in one place and nowhere else.
 *
 * `max_tuples` counts tuples across every relation and `max_memory` the bytes
 * their storage holds together with the working state of an evaluation and
 * of a query's Magic rewrite.  Exceeding either is an ordinary error: what
 * would exceed it is refused, a diagnostic names the limit, its configured
 * value and what was being done, and the relation is left as it was.
 *
 * `max_memory` bounds what running the program holds: relation and index
 * storage, which grows with the data, and the plans of an evaluation and the
 * rewritten rules of a query, which grow with the square of a rule's length.
 * A limit that fits a program's data may therefore refuse its plans, before
 * any tuple is derived.  It does not bound the memory the library uses in
 * total: the loaded program, the symbols and values it mentions, the
 * evaluation order and a prepared query are sized by what the caller wrote,
 * and the tables and evaluation order of a query's rewrite are a fraction of
 * the rewrite the limit does count.
 * Nor is it a bound on peak usage, since growing an array holds the old one
 * until the new one is filled.
 *
 * `max_magic_preds` and `max_magic_rules` bound the Magic Sets rewrite of a
 * single query: the adorned and magic predicates it generates, and the rules
 * it keeps once duplicates are dropped.  Exceeding either fails the query
 * with a diagnostic naming the limit, its configured value and the predicate
 * being adorned.  Setting either sizes the rewrite: a limit left at zero
 * beside one that is set is unbounded.  With both at zero, the library sets
 * budgets in proportion to the program, on the predicates and rules the
 * rewrite generates and on the literals of every rule it drafts, and a
 * rewrite outgrowing one is abandoned with a warning and the query answered
 * from the whole program, as it would be with RELIC_F_NO_MAGIC: the rewrite
 * is an optimization, and without a limit the caller asked for it must not
 * decide whether a query is answered.  The budgets bound the rewrite, not
 * what its rules derive.
 *
 * `flags` holds RELIC_F_ switches.  Most select an alternative evaluation
 * strategy that must give the same answers as the default; they exist so
 * that strategies can be compared against one another, and are not meant
 * for ordinary use.  RELIC_F_NOTE_MIXED instead asks for a diagnostic the
 * library does not otherwise give.  No switch changes an answer.  Bits no
 * switch defines are ignored, so a library that predates a switch runs a
 * caller that sets it as if it were not set.
 *
 * The structure's layout is part of the binary interface, so a release that
 * adds a limit changes the library's interface version.
 */
struct relic_config {
	size_t		max_memory;	/* storage and working state, bytes */
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
 * Plan every literal with bound arguments, short of all of them, as a scan
 * matching those arguments rather than a lookup through an index on them.
 * No secondary index is built.
 */
#define	RELIC_F_NO_INDEX	0x0002u

/*
 * Plan each rule's positive literals in the order they are written, after
 * the one reading a semi-naive delta, rather than choosing at each step the
 * one expected to produce the fewest tuples.
 */
#define	RELIC_F_NO_REORDER	0x0004u

/*
 * Answer every query from the fixed point of the whole program instead of
 * through its Magic Sets rewrite, which derives only what the query's
 * bindings can reach.
 */
#define	RELIC_F_NO_MAGIC	0x0008u

/*
 * Report, as a RELIC_NOTE pointing at the comparison, the first ordering
 * comparison in each rule that compared values of different types.  Such a
 * comparison is false and filters its binding out, which is the language's
 * rule rather than a mistake, so it is reported only when asked for; the
 * note is how a column holding types nobody meant to mix is discovered.
 *
 * A rule of the program is reported at most once for the life of the
 * context, and the rules a Magic Sets rewrite generates from it count as
 * that rule.  A query is reported on at most once each time it is answered.
 * Asking for the note costs nothing that can fail.
 */
#define	RELIC_F_NOTE_MIXED	0x0010u

/*
 * Create a context.  Returns NULL when memory is exhausted or when
 * getentropy(2) cannot supply the random bytes the context keys its hash
 * tables with, as in a sandbox with no random device.  There is no other way
 * for it to fail, and no context yet to report through, so neither failure
 * is reported to a diagnostic callback.
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
 * Build a symbol or a string value from bytes, interning them in the context.
 * Each fills *value and returns 0, or returns -1 with the reason reported.
 * The identifier stays valid for the life of the context, and the same bytes
 * always give the same one.
 *
 * A symbol is what the language can write as one: a lowercase ASCII letter
 * followed by ASCII letters, digits and underscores.  Any other bytes -- a
 * space, a capital first, a NUL, UTF-8 -- are refused by
 * relic_value_symbol() and must be built with relic_value_string(), whose
 * string holds whatever bytes it is given.  The rule holds wherever a
 * value enters the program, so a symbol value built by hand from a string's
 * identifier is refused there too.
 *
 * An integer needs no constructor: set `type` to RELIC_INTEGER and
 * `v.integer` to the value.
 */
RELIC_EXPORT int relic_value_symbol(relic_t *ctx, const char *bytes,
    size_t length, relic_value_t *value);
RELIC_EXPORT int relic_value_string(relic_t *ctx, const char *bytes,
    size_t length, relic_value_t *value);

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
 * again.  The callback may read through the context, and any other call it
 * makes is refused, as the description of relic_t sets out.
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
 * the stream whole and delegates, and a read function of the caller's own
 * behind the stream is held to the rule for callbacks that the description
 * of relic_t sets out; relic_load_string() takes a NUL-terminated program
 * text.  `source_name` is what diagnostics from that
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
RELIC_EXPORT int relic_load_file(relic_t *ctx, const char *source_name,
    FILE *stream);
RELIC_EXPORT int relic_load_path(relic_t *ctx, const char *path);
RELIC_EXPORT int relic_load_string(relic_t *ctx, const char *source_name,
    const char *text);

/*
 * Add the fact predicate(values[0], ..., values[arity - 1]) to the program,
 * exactly as loading its text would: it is appended after everything loaded
 * before it, checked with the next check, and shown by explain like any other
 * fact.  Returns 0, or -1 with the reason reported and nothing added.
 *
 * `predicate` is a NUL-terminated name, which must be a symbol by the rule of
 * relic_value_symbol() and must not be the negation keyword `not`.  `arity`
 * is at most 32, and `values` holds that many, each built by the value
 * constructors, an integer, or read out of a query's answer; it may be NULL
 * when `arity` is zero.  A predicate is identified by its name and arity
 * together, so a fact of a new arity starts a new predicate.
 *
 * Adding a fact, like loading, changes the program, so everything derived
 * before it is discarded by the next evaluation and derived again from the
 * facts: a fact can make a negated literal false, and nothing derived before
 * it can be kept without maintaining it incrementally, which relic does not
 * do.
 */
RELIC_EXPORT int relic_fact_add(relic_t *ctx, const char *predicate,
    size_t arity, const relic_value_t *values);

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
 * configured limit refuses a tuple or when memory runs out, and the next
 * evaluation starts again from nothing.
 *
 * Nothing is recomputed when nothing has been loaded since the last
 * evaluation that succeeded.  When something has, every derived tuple is
 * discarded and the whole program evaluated again: a program with negation
 * is not monotone, and a fact loaded later can make false a tuple derived
 * before it.  A query materializes the program before it is answered, so a
 * caller that only queries need not call this.
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
 * before or after the context is destroyed; it cannot be run once the
 * context is gone.
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
 * A program already evaluated by relic_materialize() is answered from what it
 * derived.  Otherwise the query is answered through its Magic Sets rewrite:
 * the predicates it reads through negation, those derived by facts alone and
 * everything those depend on are evaluated in full, and the rest only as far
 * as the query's bindings reach.  What that derives is discarded by the next
 * query or evaluation.  A rewrite that outgrows its budget, when no Magic
 * limit is configured, is abandoned with a warning, and the query answered
 * from the whole program instead.
 *
 * No row is delivered until the whole answer is known.  A query that fails
 * -- a limit exceeded, say -- therefore delivers no rows at all rather than a
 * truncated answer that would be indistinguishable from a complete one.
 *
 * Rows are distinct, because a query's answer is a set.  Their order is
 * unspecified by the language but is the same on every run over the same
 * input; relic_value_compare() orders them when a caller needs an order that
 * does not depend on that.
 *
 * The answer is transient.  Once its rows have been delivered, or the
 * callback has stopped them, the context keeps nothing of it, so answering
 * many queries does not grow the context and earlier answers do not count
 * against the tuple and memory limits.  A caller that wants to keep an answer
 * copies it, as relic_query_collect() does.
 *
 * The callback may read through the context, and any other call it makes is
 * refused, as the description of relic_t sets out.
 */
RELIC_EXPORT int relic_query_run(relic_t *ctx, relic_query_t *query,
    relic_row_cb callback, void *callback_ctx);

/*
 * A query's answer, copied out of the context by relic_query_collect().
 */
typedef struct relic_result relic_result_t;

/*
 * Answer a query into a result the caller owns, or return NULL with the
 * reason reported -- the query failed, or the copy ran out of memory -- and
 * never a partial result.  The collector is relic_query_run() with a callback
 * that copies each row, and delivers what the stream would, in its order.
 *
 * relic_result_rows() and relic_result_columns() give the result's size; the
 * columns are the query's output columns even when there are no rows.
 * relic_result_row() returns the `row`th row's values, or NULL when `row` is
 * not less than the number of rows; for a query with no output columns a
 * present row is a non-NULL pointer to no values.
 *
 * A result holds nothing of the context's state, so it survives later loads,
 * facts and queries and may be destroyed after the context.  The bytes of its
 * symbols and strings are read with relic_string() on the context, for as
 * long as that lives.  relic_result_destroy() accepts NULL.
 */
RELIC_EXPORT relic_result_t *relic_query_collect(relic_t *ctx,
    relic_query_t *query);
RELIC_EXPORT size_t relic_result_rows(const relic_result_t *result);
RELIC_EXPORT size_t relic_result_columns(const relic_result_t *result);
RELIC_EXPORT const relic_value_t *relic_result_row(
    const relic_result_t *result, size_t row);
RELIC_EXPORT void relic_result_destroy(relic_result_t *result);

/*
 * Release a query.  Accepts NULL.  It may be called before or after
 * relic_destroy(), and from caller code the library is running, except
 * while the query itself is being answered, when it does nothing.
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
 * The first five accumulate over the life of the context, and count the
 * rules of the program and of every query answered alike:
 *
 *	iterations	rounds of fixed-point evaluation of recursive
 *			components; a component that is not recursive is
 *			evaluated in one pass, which is not counted
 *	derivations	head tuples produced by rules, facts included, whether
 *			or not the relation already held them
 *	tuples_inserted	derivations that added a tuple to its relation
 *	tuples_scanned	tuples read while scanning relations
 *	tuples_looked_up
 *			tuples read while looking up relations through an
 *			index on the bound arguments of a literal
 *
 * The rest describe the context when the counters are read: the tuples held
 * across every relation -- answers are transient, so only a row callback
 * finds one among them, the answer it is receiving -- the bytes of relation
 * and index storage they occupy, and the distinct values interned, by type.
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
	uint64_t	tuples_looked_up;
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
 * Receives the plans evaluation runs, one line at a time: the context
 * pointer given when it was installed, and the line, which is printable
 * ASCII, has no newline, is at most 1023 bytes long and does not outlive the
 * call.
 */
typedef void (*relic_explain_cb)(void *, const char *);

/*
 * Install an explain callback, replacing any previous one; NULL, the
 * default, explains nothing.
 *
 * While one is installed, every component the program's evaluation plans is
 * shown as soon as it is planned and before it runs -- its heading, how many
 * facts it has, and each other rule followed by its plan and the plans of
 * its semi-naive delta variants -- and a query's plan is shown before the
 * query runs.  A query answered through its Magic Sets rewrite also shows the
 * rewrite: the adornments and seed rules it generated, then each component of
 * the rewritten rules, indented beneath it.  What is shown is what runs, so a
 * query against a program already evaluated shows only its own plan.  The
 * text is meant for a person and may change from one release to the next.  A
 * line longer than the limit is cut and ends in "...".
 *
 * As with a diagnostic callback, the callback may read through the context
 * and any other call it makes is refused.
 */
RELIC_EXPORT void relic_explain_set(relic_t *ctx, relic_explain_cb callback,
    void *callback_ctx);

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
