/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
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
#define	RELIC_VERSION_MAJOR	1
#define	RELIC_VERSION_MINOR	0
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
 * as many as it likes, but one context is not itself thread-safe:
 * relic_interrupt() is the one call another thread may make on it.
 *
 * A function that takes a context and can fail returns -1 or NULL when it
 * does, and 0 or a pointer otherwise, and a failure is always explained by a
 * RELIC_ERROR diagnostic, the last of which relic_error() returns, and
 * relic_error_class() what kind of failure it was.  The functions that only
 * read -- relic_string(), relic_value_compare(), relic_query_count(),
 * relic_stats() -- cannot fail; relic_string() returns NULL for an
 * identifier it does not know, and relic_value_compare() alone reports an
 * error, for a value the context does not hold, when no caller code the
 * library is running made the call.
 *
 * Caller code runs while the context is part way through an operation in
 * four ways: the diagnostic, explain and row callbacks, and the read
 * function of a stream given to relic_load_file() or relic_import_file().
 * It may read through the context with the functions above,
 * relic_error() and relic_error_class(), and may call relic_interrupt(),
 * which a signal handler or another thread may call too.  Any other call
 * on the same context is refused: a call that returns a status fails with
 * -1 or NULL, one that returns nothing does nothing, and relic_error() and
 * relic_error_class() describe the refusal until the next
 * error or until the operation the caller code runs inside returns; the
 * diagnostic callback is not called for it.  A refused relic_destroy()
 * releases nothing, so a callback that tries to destroy its context leaks
 * it rather than freeing memory in use.  A callback must return: one that
 * leaves by longjmp() or an exception leaves its context refusing every
 * later call.
 */
typedef struct relic relic_t;

typedef struct relic_config relic_config_t;

/*
 * Limits a context enforces, and switches that select internal behaviour.
 * A zeroed structure asks for the defaults: no bound on memory, tuples or
 * input, and for the Magic Sets rewrite the limit described below.  A limit
 * of SIZE_MAX bounds nothing it counts.
 *
 * The fields correspond one to one with the tool's repeatable -l key=value
 * options, which exist only to parse into this structure: a limit is defined
 * in one place and nowhere else.
 *
 * `max_tuples` counts tuples across every relation, the facts the program
 * states among them, and `max_memory` the bytes their storage holds together
 * with the working state of an evaluation and of a query's Magic rewrite.
 * Exceeding either is an ordinary error: what would exceed it is refused, a
 * diagnostic names the limit, its configured value and what was being done,
 * and the relation is left as it was.  A load, an import or relic_fact_add()
 * whose facts do not fit is refused with nothing added, naming the first
 * predicate, in the order its text names them, that would gain facts:
 * "limit exceeded: tuples=4 while loading e/1".
 *
 * `max_memory` bounds what the program's stores hold and what running the
 * program holds: relation and index storage, the program's facts and what it
 * derives alike, which grows with the data; the table of integer values, which
 * gains an entry for every distinct integer the program states or evaluation
 * computes; the symbol pool, which holds every symbol and string the program
 * states and every string evaluation computes; the names of the predicates the
 * program and the rewrites of its queries name, and of the sources each
 * predicate was first read from; the buffers an evaluation computes strings
 * in; and the plans of an evaluation and the rewritten rules of a query, which
 * grow with the square of a rule's length.  A limit that fits a program's data
 * may therefore refuse its plans, before any tuple is derived.  The names,
 * symbols, strings and integers the caller writes are counted as those
 * evaluation computes are, so a load, an import, a fact or a query whose
 * values or names would grow the tables past the limit is refused as it
 * interns them, with "limit exceeded: memory=N".  The limit does not bound the
 * memory the library uses in total: the program's rules and queries, the text
 * being loaded and the field of a table being imported, which `max_input`
 * bounds, the evaluation order, a prepared query, and each predicate's slot
 * in the relation tables with the header of its relation are sized by what
 * the caller wrote, and the tables and evaluation order of a query's rewrite
 * are a fraction of the rewrite the limit does count.  Nor is it a bound on
 * peak usage, since growing an array holds the old one until the new one is
 * filled.
 *
 * Those tables release nothing before the context is destroyed.  Every
 * distinct value a load, a fact, a query's constants or an evaluation interns,
 * and every predicate a query or its rewrite names, stays for the life of the
 * context, the values and the names counting against `max_memory`, so a
 * context that goes on answering queries over new constants grows until it
 * reaches the limit.  A caller
 * serving such queries for as long as it runs loads the program into a new
 * context and destroys the old one.
 *
 * `max_magic` bounds the work of the Magic Sets rewrite of a single query:
 * the body literals it reads while it searches for what to generate and
 * those of every rule it generates, a literal counting one and one more per
 * node of its expressions, and the names of the predicates it generates, a
 * literal per 64 bytes.  Every predicate and rule the rewrite generates
 * is counted through the literals it took, so the one limit bounds them
 * all.  Zero asks for a default in proportion to the program and the query,
 * large enough for every rewrite that specializes the program rather than
 * multiplies it; any other value replaces the default, and none removes
 * the limit.  A rewrite outgrowing it, set or not, is abandoned with a
 * warning and the query answered from the whole program, as it would be
 * with RELIC_F_NO_MAGIC: the rewrite is an optimization, and must not
 * decide whether a query is answered.  The whole program never answers
 * differently, but it may meet an undefined arithmetic result, a string too
 * long or an invalid pattern the rewrite would not have evaluated, and fail;
 * `max_tuples` and `max_memory` bound it as they bound the rewrite.  The
 * limit bounds the rewrite, not what its rules derive.
 *
 * `max_input` bounds the bytes of one input: the text of a load, whether
 * relic_load_string(), relic_load_file() or relic_load_path() gives it, and
 * the bytes one field of an imported table holds once its format is
 * decoded, a header's and a skipped column's among them.  A text or a field
 * longer than that is refused with nothing added, as any failed load or
 * import is, with "limit exceeded: input=N while loading f.dl" for a text
 * and the same, "while importing", for a field, located at the field's
 * first byte.  A stream is read no further than one byte past the limit,
 * and an import's field, a quoted csv field left open among them, holds no
 * more than the limit before it is refused, so a stream that never ends is
 * refused too.  A table read a window of records at a time may have read up
 * to two windows and a chunk past the field, buffers whose size is fixed by
 * the library, which the limit does not count.  A query's text is not an
 * input and is held to no such limit.
 *
 * `threads` is how many threads an evaluation may share a round's work
 * among, and an import the reading of a table, the calling thread
 * included; zero and one both mean the calling thread alone, which is the
 * default, and more than RELIC_THREADS_MAX is refused when the context is
 * created.  It is the one field no -l option sets: the tool's -j sets it.
 * The threads are the context's own: they are started the first time an
 * evaluation shares a round or an import shares the reading of a table,
 * run only inside an operation the caller started, call none of the
 * caller's callbacks, block every signal but those a fault in them raises,
 * and are joined when the context is destroyed.  A child forked between
 * operations may go on using a context its parent created, on the calling
 * thread alone.  A round is shared only when it holds enough work to repay
 * the threads' coordination, and a table only among eight threads or
 * more, and only when reading its first 2 MiB does not reach its end.
 * The answers, the relations, the values, every diagnostic and every
 * counter but parallel_rounds are the same whatever the number of
 * threads; only how much of its stream a refused import has read may
 * differ.  A thread that cannot be started is not an error: the operation
 * uses those that could be.  A library built without threads accepts the
 * field and runs everything on the calling thread.
 *
 * `flags` holds RELIC_F_ switches.  Most select an alternative evaluation
 * strategy that must give the same answers as the default; they exist so
 * that strategies can be compared against one another, and are not meant
 * for ordinary use.  RELIC_F_NOTE_MIXED instead asks for a diagnostic the
 * library does not otherwise give.  No switch changes an answer, though two
 * strategies may hold different amounts while they compute one, and so one
 * may meet a limit the other does not.  Bits no switch defines are ignored,
 * so a library that predates a switch runs a caller that sets it as if it
 * were not set.
 *
 * The structure's layout is part of the binary interface, so a release that
 * adds a limit changes the library's interface version.
 */
struct relic_config {
	size_t		max_memory;	/* storage and working state, bytes */
	size_t		max_tuples;
	size_t		max_magic;	/* the rewrite's work, literals */
	size_t		max_input;	/* a text or a table field, bytes */
	unsigned int	flags;
	size_t		threads;	/* the calling thread included */
};

/*
 * The most threads a context may be configured with.
 */
#define	RELIC_THREADS_MAX	256

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
 * Report, as a RELIC_NOTE pointing at the comparison, the first comparison
 * in each rule that ordered values of different types, that applied an
 * arithmetic operator to a symbol or a string, or that called a string
 * function with an argument of a type it does not take.  Such a comparison
 * is false and filters its binding out, which is the language's rule
 * rather than a mistake, so it is reported only when asked for; the note
 * is how a column holding types nobody meant to mix is discovered.
 *
 * A rule of the program is reported at most once for the life of the
 * context, and the rules a Magic Sets rewrite generates from it count as
 * that rule.  A query is reported on at most once each time it is answered.
 * Asking for the note costs nothing that can fail.
 */
#define	RELIC_F_NOTE_MIXED	0x0010u

/*
 * Gather every query's answer in its answer relation before delivering a
 * row, as a query whose derivations may repeat must be answered, even one
 * whose plan derives each row once and could deliver its rows as it derives
 * them.  The rows are the same and come in the same order, which tests show
 * by answering both ways.
 */
#define	RELIC_F_NO_STREAM	0x0020u

/*
 * Defer every round of evaluation whose rules read none of the relations it
 * derives except as they stood when it began, which is every round but a
 * pass of the naive fixed point: what its rules derive is gathered while
 * they run and inserted when the last has finished, in the order it would
 * have been inserted as it was derived, and the scan each rule starts from
 * is cut into pieces run one after another.  That is how a round runs on
 * several threads, and the switch runs every round that way however little
 * work it holds, so that the way is exercised by every program.  It reads
 * every table an import reads a window of records at a time too, as a large
 * table is read on several threads, whatever the table's size and the
 * number of threads.  The relations, the values and every count of work
 * come out as they do without it.
 */
#define	RELIC_F_PARALLEL_ALWAYS	0x0040u

/*
 * Compile the body of every aggregate into a rule of its own, whose relation
 * holds the body's rows and is what the aggregate folds, even a body that is
 * one positive atom of distinct variables, which otherwise denotes the
 * atom's relation and is folded where it stands.  The answers are the same;
 * the copies are tuples held and counted, so a limit may refuse one of the
 * two ways and not the other.
 */
#define	RELIC_F_GROUP_RULES	0x0080u

/*
 * Plan a positive literal that binds nothing the rest of its rule reads --
 * one whose arguments not bound before it are `_` or variables the rule
 * mentions nowhere else -- as a scan or a lookup, continuing the join once
 * per tuple it matches, rather than as a test that continues it at most
 * once.  The answers and the relations are the same; the rule derives each
 * of its tuples as many more times as the literal matches.
 */
#define	RELIC_F_NO_EXISTS	0x0100u

/*
 * Rewrite every predicate a query reads on demand by the ordinary Magic
 * Sets construction, never by bound query factoring.  Factoring replaces
 * a bound recursive predicate whose rules pass its free columns through
 * unchanged with a walk of the states reachable from each requested
 * binding, paired with that binding, and answers drawn from the reached
 * states; it derives no answers for the states it passes through.  The
 * answers are the same; the ordinary rewrite derives every answer of every
 * state its demand reaches.
 */
#define	RELIC_F_NO_FACTOR	0x0200u

/*
 * Import every table a field at a time, typing and interning each field
 * and staging each record as it is read, whatever the number of threads
 * and even with RELIC_F_PARALLEL_ALWAYS, as a small table or one read on
 * fewer than eight threads always is.  A table is otherwise read a window
 * of records at a time, each window's fields decoded and its values and
 * records checked before any is interned or staged.  The facts, the values
 * and the identifiers they are given are the same, and so is a failed
 * import's message and what it leaves interned.
 */
#define	RELIC_F_NO_BATCH_IMPORT	0x0400u

/*
 * Fold every aggregate by walking its source relation's stored rows, and
 * store every relation the program names, rather than reducing a source
 * that a query's run proves distinct and finds read only by folds and by
 * rules projecting its grouping key straight into one state per group,
 * building a summary over a complete relation that folds read repeatedly,
 * or counting an unkeyed group by its relation's size.  The answers are
 * the same; a reduced source holds its groups' state instead of its rows,
 * so a limit may refuse one of the two ways and not the other.
 */
#define	RELIC_F_NO_GROUP_REDUCTION	0x0800u

/*
 * Create a context.  Returns NULL when the configuration asks for more than
 * RELIC_THREADS_MAX threads, when memory is exhausted, or when getentropy(2)
 * cannot supply the random bytes the context keys its hash tables with, as
 * in a sandbox with no random device.  There is no other way for it to
 * fail, and no context yet to report through, so no failure is reported to
 * a diagnostic callback.
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
 *
 * A context holds at most 2^30 - 1 distinct byte strings, which symbols and
 * strings share, and at most 2^30 - 1 distinct integers; a byte string is at
 * most 2^32 - 2 bytes long.  Interning past a limit fails with the reason
 * reported.
 */
RELIC_EXPORT int relic_value_symbol(relic_t *ctx, const char *bytes,
    size_t length, relic_value_t *value);
RELIC_EXPORT int relic_value_string(relic_t *ctx, const char *bytes,
    size_t length, relic_value_t *value);

/*
 * The bytes of an interned symbol or string, or NULL for an identifier this
 * context never assigned, a collected result's among them.  A value carries
 * no owner, only its identifier: another context's identifier that this one
 * has assigned too reads as this context's value, so a value is passed only
 * to the context or result that made it.
 *
 * The result is NUL-terminated for the convenience of a caller that wants a
 * C string, but a string literal may contain NUL bytes, so a caller that must
 * not lose them passes lenp and uses the length.  The bytes belong to the
 * context; the caller neither frees nor modifies them.  The bytes of a value
 * the program holds -- one it states, or one a caller built with the
 * constructors above -- stay valid until the context is destroyed; the bytes
 * of a value a query computed are valid while the row callback receiving it
 * runs, as the row is.
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
 * call; a callback that keeps either copies it, the `file` name inside the
 * location included.  The name lives as long as what was parsed under it:
 * a loaded program's as long as the context, a prepared query's as long as
 * the query, and an import's for the call.
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
 * and has no opinion about the format.  The relic tool reports every
 * diagnostic through it.
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
 * A load is all or nothing: a failed one adds none of its facts and none of
 * its statements, so a program never holds half of a file.  A program with
 * an error in it is not a smaller program, it is a different one.  A load
 * whose facts `max_tuples` or `max_memory` does not admit is refused, as
 * relic_config_t describes.
 */
RELIC_EXPORT int relic_load_file(relic_t *ctx, const char *source_name,
    FILE *stream);
RELIC_EXPORT int relic_load_path(relic_t *ctx, const char *path);
RELIC_EXPORT int relic_load_string(relic_t *ctx, const char *source_name,
    const char *text);

/*
 * Add the fact predicate(values[0], ..., values[arity - 1]) to the program,
 * exactly as loading its text would: it is stored with the program's other
 * facts before the call returns, and counted by explain like any other fact.
 * Returns 0, or -1 with the reason reported and nothing added; a tuple or
 * memory limit the fact does not fit is one such reason, reported as a
 * load's refusal is.
 *
 * `predicate` is a NUL-terminated name, which must be a symbol by the rule of
 * relic_value_symbol() and must not be the negation keyword `not`.  `arity`
 * is at most 32, and `values` holds that many, each built by the value
 * constructors or an integer; it may be NULL when `arity` is zero.  A symbol
 * or string read out of a query's answer is built again from its bytes,
 * since a collected result's are the result's and are refused here.  A
 * predicate is identified by its name and arity together, so a fact of a
 * new arity starts a new predicate.
 *
 * Adding a fact the program does not hold, like loading one, changes the
 * program, so everything derived before it is discarded and the next
 * evaluation derives it again from the facts: a fact can make a negated
 * literal false, and nothing derived before it can be kept without
 * maintaining it incrementally, which relic does not do.  Adding a fact the
 * program holds changes nothing.
 */
RELIC_EXPORT int relic_fact_add(relic_t *ctx, const char *predicate,
    size_t arity, const relic_value_t *values);

/*
 * The table formats an import reads.  Each is the exact inverse of the
 * relic tool's output format of the same name, and reads what other tools
 * write in it wherever that does not conflict:
 *
 *	RELIC_FORMAT_CSV	RFC 4180: fields separated by commas; a field
 *				beginning with `"` is quoted, holds commas and
 *				line ends as data, and writes a `"` as `""`
 *	RELIC_FORMAT_TSV	fields separated by tabs, with the escapes
 *				\\, \t, \n and \r and no others
 *
 * In both, a record ends at a line feed or a carriage return and line feed
 * outside a quoted field, and bytes are data: no encoding is assumed and no
 * whitespace is trimmed.
 */
enum relic_format {
	RELIC_FORMAT_CSV = 1,
	RELIC_FORMAT_TSV
};

/*
 * What a column of an imported table holds.  A field is typed after its
 * format has decoded it, and a field its column refuses is an error:
 *
 *	RELIC_COLUMN_SYMBOL	a symbol, by the rule of relic_value_symbol()
 *	RELIC_COLUMN_STRING	a string of the field's bytes, whatever they are
 *	RELIC_COLUMN_INTEGER	an integer as the language writes one,
 *				-?[0-9]+ within int64_t
 *	RELIC_COLUMN_SKIP	nothing: the field is read by the format's rules
 *				and is not part of the fact
 *
 * Nothing about a field's type is inferred from its bytes.
 */
enum relic_column {
	RELIC_COLUMN_SYMBOL = 1,
	RELIC_COLUMN_STRING,
	RELIC_COLUMN_INTEGER,
	RELIC_COLUMN_SKIP
};

/* The table's first record is a header, and is read and not imported. */
#define	RELIC_IMPORT_HEADER	0x1u

typedef struct relic_import relic_import_t;

/*
 * An import: the facts of `predicate` that a table holds, one per record,
 * each record's fields typed by `columns`, which has `column_count` entries.
 * The predicate's arity is the number of columns that are not
 * RELIC_COLUMN_SKIP, at most 32.  `flags` holds RELIC_IMPORT_ bits.
 *
 * The enumerators start at one, so a description or column left zeroed is
 * refused rather than read as some format or type.  The structure's layout
 * is part of the binary interface; a later boolean goes in `flags`.
 */
struct relic_import {
	const char		*predicate;
	const enum relic_column	*columns;
	size_t			 column_count;
	enum relic_format	 format;
	unsigned int		 flags;
};

/*
 * Add the facts a table holds to the program, as though its text had
 * stated them.
 *
 * relic_import_path() opens the file, names diagnostics by its path and
 * closes it.  relic_import_file() reads a stream the caller owns, naming
 * diagnostics `source_name`, or "<input>" when that is NULL; a read
 * function of the caller's own behind the stream is held to the rule for
 * callbacks that the description of relic_t sets out.  The description, and
 * the columns it points at, are borrowed for the call.
 *
 * Each returns 0, or -1 with the reason reported and nothing added.  An
 * import is all or nothing, like a load: a table that cannot be read, a
 * malformed record, a field its column refuses, or a limit the facts do not
 * fit adds no fact.  The first mistake ends an import, and is the one
 * reported, at its line and column.  With RELIC_IMPORT_HEADER the first
 * record must be there, and must have a field per column.  Facts already
 * held are held once, and an import that adds a fact discards what was
 * derived, as relic_fact_add() does.
 *
 * An import is refused, with nothing read, when the description, the path
 * or the stream is NULL; when `predicate` is not a name relic_fact_add()
 * accepts; when there are no columns, a column is not an enumerator of
 * relic_column, or more than 32 are kept; when `format` is not an
 * enumerator of relic_format; or when `flags` holds a bit this library does
 * not define, since an import flag changes what is read.
 */
RELIC_EXPORT int relic_import_file(relic_t *ctx, const char *source_name,
    FILE *stream, const relic_import_t *import);
RELIC_EXPORT int relic_import_path(relic_t *ctx, const char *path,
    const relic_import_t *import);

/*
 * The arities at which the program has a predicate named `name`: bit n of
 * *arities is set when a statement, a fact, an import or a query has named
 * name/n, a load or import that failed included, and no bit is set for a
 * name nothing has named.  A predicate is a name and an arity together, so
 * this is how a caller that names a relation without its arity learns which
 * one it means, or that the name has several.  Predicates the library
 * generates are never reported.
 *
 * The program is checked first, as by every operation that uses it, so that
 * every rule loaded has named its predicates.  Returns 0, or -1 with the
 * reason reported and *arities untouched: the check failed, `name` or
 * `arities` is NULL, or caller code the context is running made the call.
 */
RELIC_EXPORT int relic_predicate_arities(relic_t *ctx, const char *name,
    uint64_t *arities);

/*
 * Validate the loaded program, returning 0 when it is usable and -1 when it
 * is not, with every mistake already reported through the diagnostic
 * callback.
 *
 * A program is rejected when a rule is not range restricted: every variable
 * in a rule head, in a negated literal and in a comparison must also occur in
 * a positive relational body literal, or be assigned by an equality whose
 * one side is the bare variable and whose other side is bound, because
 * nothing else can bind it.
 * Warnings that do not reject a program are reported too, among them a
 * variable a rule mentions only once and a predicate the program references
 * and never defines.
 *
 * A check compiles only the statements loaded since the previous one, so
 * calling it twice with nothing loaded in between repeats none of the first
 * call's messages and returns the same answer. It stays -1 once a program
 * has contained a rejected rule: loading more cannot repair one already
 * loaded. A failing check whose mistakes were reported before reports only
 * that the program cannot be used.
 *
 * The queries a program contains are part of it and are checked the same
 * way: every output column of a query, which is to say every variable it
 * names, must occur in a positive relational literal of the query or be
 * assigned by one of its equalities.
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
 * configured limit refuses a tuple, when memory runs out, when an
 * arithmetic expression has no defined result -- one outside the 64-bit
 * integers, or a division or remainder by zero -- when a string function
 * would compute a string longer than a string value can be, or when `match`
 * is given a pattern that is not a regular expression, or when
 * relic_interrupt() asks it to stop, and the next evaluation starts again
 * from the facts.
 *
 * Nothing is recomputed when the program has gained no rule and no fact
 * since the last evaluation that succeeded.  When it has, every derived
 * tuple is discarded and the whole program evaluated again: a program with
 * negation is not monotone, and a fact loaded later can make false a tuple
 * derived before it.  A query is answered from the materialized program
 * when it is current, and otherwise through its Magic rewrite, or under
 * RELIC_F_NO_MAGIC by materializing the program first, so a caller that
 * only queries need not call this.
 */
RELIC_EXPORT int relic_materialize(relic_t *ctx);

/*
 * Ask the operation in progress on the context to stop.  An evaluation --
 * relic_materialize(), relic_query_run() or relic_query_collect() -- looks
 * for the request after every few thousand steps of a join and before each
 * row of a gathered answer, and on finding it fails with "interrupted" in
 * its diagnostic, leaving the context as any failed evaluation does: the
 * next one starts again from the facts.  Every other operation runs to its
 * end.
 *
 * This only records the request: it enters nothing, allocates nothing and
 * cannot fail, so it may be called from a signal handler, from another
 * thread, or from a callback of the operation itself, while the caller
 * keeps the context alive.  The request is a C11 atomic; a library built by
 * a compiler without C11 atomics, which no system relic supports has,
 * records it in a volatile sig_atomic_t instead, which a signal handler or
 * a callback may still set and another thread may not.  Each operation
 * forgets any request made before it began, so an interrupt with no
 * operation in progress stops nothing.
 * Accepts NULL.
 */
RELIC_EXPORT void relic_interrupt(relic_t *ctx);

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
 * The values are borrowed for the duration of the call, and so are the bytes
 * relic_string() reads for a symbol or a string among them: a callback that
 * keeps a row copies the bytes it needs, not the identifiers, or the caller
 * collects the answer with relic_query_collect() instead.  The callback
 * returns 0 to receive the next row and nonzero to stop.
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
 * derived.  Otherwise the query is answered through its Magic Sets rewrite: the
 * predicates it reads through negation and everything those depend on are
 * evaluated in full, those no rule derives are read as their facts stand, and
 * the rest only as far as the query's bindings reach.  What that derives is
 * discarded by the next query or evaluation.  A rewrite that outgrows
 * max_magic is abandoned with a warning, and the query answered from the
 * whole program instead, which can fail on an undefined arithmetic result,
 * a string too long or an invalid pattern the rewrite would not have
 * reached, and never answers differently.
 *
 * No row is delivered until everything that can fail has succeeded.  A query
 * answered in a context whose max_tuples and max_memory each set no limit,
 * being zero or SIZE_MAX, has its rows delivered as they are derived, once
 * every relation it reads is complete, when the plan of the rule it runs --
 * its own, or its rewrite's -- derives each row once and evaluates no
 * arithmetic operator and calls no function: when it names every variable a
 * relational literal of its body binds, matches no argument with `_`, and
 * computes nothing, since any computation may fail.  Any other query's
 * answer is gathered whole, its duplicates dropped and the limits bounding
 * it, before its first row is delivered.  A query that fails -- a limit
 * exceeded, say, an arithmetic result that is not defined, a string too
 * long or an invalid pattern -- therefore delivers no rows at all rather
 * than a truncated answer that would be indistinguishable from a complete
 * one.  The exception is relic_interrupt(): a query interrupted after its
 * first row fails all the same, and the rows already delivered are a prefix
 * of its answer.  A diagnostic the query's evaluation reports, such as the note
 * RELIC_F_NOTE_MIXED asks for, may come between rows delivered as they are
 * derived.
 *
 * Rows are distinct, because a query's answer is a set.  Their order is
 * unspecified by the language but is the same on every run over the same
 * input; relic_value_compare() orders them when a caller needs an order that
 * does not depend on that.
 *
 * The answer is transient.  Once its rows have been delivered, or the
 * callback has stopped them, the context keeps nothing of it, so earlier
 * answers do not count against the tuple limit.  The integers and strings
 * a query computes are interned when its answer is gathered, and those
 * stay for the life of the context, counted by the memory limit, as every
 * value interned does.  An answer delivered as it is derived
 * is never held at all.  A caller that wants to keep an answer copies it, as
 * relic_query_collect() does.
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
 * facts and queries and may be read and destroyed after the context.  It
 * holds a copy of the bytes of each of its symbols and strings, and its
 * values are its own: relic_result_string() reads the bytes of a symbol's or
 * a string's identifier, as relic_string() does for the context's, until
 * the result is destroyed, and returns NULL for an identifier the result
 * did not assign; relic_result_compare() orders two of its values as
 * relic_value_compare() orders the context's, and compares a value whose
 * identifier the result did not assign equal to everything.  A result's
 * symbol or string is not the context's: relic_string() returns NULL for
 * it, and relic_value_compare() and relic_fact_add() refuse it, so a caller
 * handing one back builds it again from its bytes.  An integer is its own
 * value in both.  relic_result_destroy() accepts NULL.
 */
RELIC_EXPORT relic_result_t *relic_query_collect(relic_t *ctx,
    relic_query_t *query);
RELIC_EXPORT size_t relic_result_rows(const relic_result_t *result);
RELIC_EXPORT size_t relic_result_columns(const relic_result_t *result);
RELIC_EXPORT const relic_value_t *relic_result_row(
    const relic_result_t *result, size_t row);
RELIC_EXPORT const char *relic_result_string(const relic_result_t *result,
    uint32_t id, size_t *lenp);
RELIC_EXPORT int relic_result_compare(const relic_result_t *result,
    const relic_value_t *a, const relic_value_t *b);
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
 * Both values must belong to `ctx`.  A symbol or string whose identifier
 * the context never assigned, a collected result's among them, compares
 * equal to everything, and is reported as an error, unless caller code the
 * library is running made the call: the report would displace the error of
 * the operation the code runs inside, so a comparison there reports
 * nothing.
 */
RELIC_EXPORT int relic_value_compare(relic_t *ctx, const relic_value_t *a,
    const relic_value_t *b);

typedef struct relic_stats relic_stats_t;

/*
 * Counters describing the work a context has done and what it holds, for
 * understanding and comparing evaluation strategies.
 *
 * The first six accumulate over the life of the context, and count the
 * rules of the program and of every query answered alike:
 *
 *	iterations	rounds of fixed-point evaluation of recursive
 *			components; a component that is not recursive is
 *			evaluated in one pass, which is not counted
 *	derivations	head tuples produced by rules, the rows a query
 *			delivers as it derives them among them, and facts
 *			seeded into the relation of a predicate rules
 *			derive, whether or not the relation already held
 *			them; the rows of an aggregate's body are derived
 *			into a relation of their own and count, except
 *			those of a body that is one atom of distinct
 *			variables, whose rows are the atom's relation
 *	tuples_inserted	derivations that added a tuple to its relation,
 *			which a row delivered as it is derived never does
 *	tuples_scanned	tuples read while scanning relations
 *	tuples_looked_up
 *			tuples read while looking up relations through an
 *			index on the bound arguments of a literal
 *	parallel_rounds	rounds of evaluation shared among more than one
 *			thread, and a grouped source whose producer ran
 *			that way, the one counter that depends on the
 *			number of threads
 *
 * The rest describe the context when the counters are read: the tuples held
 * across every relation -- answers are transient, so only a row callback finds
 * one among them, the answer it is receiving when its query gathers the answer
 * before delivering it -- the bytes of relation and index storage they occupy
 * together with those of the table of integer values, of the symbol pool and
 * of the names of predicates and their sources, which is what the memory limit
 * counts beside evaluation's transient buffers, and the distinct values
 * interned, by type: those the program states and the integers and strings
 * evaluation computed alike.
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
	uint64_t	parallel_rounds;
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
 * facts its predicates have, and each rule with a body followed by its plan
 * and the plans of its semi-naive delta variants -- and a query's plan is
 * shown before the query runs.  A predicate of facts alone needs no
 * evaluation, is in no component and is not shown.  A query answered through
 * its Magic Sets rewrite also shows the rewrite: the adornments and seed
 * rules it generated, then each component of the rewritten rules, indented
 * beneath it.  What is shown is what runs, so a query against a program
 * already evaluated shows only its own plan.  The text is meant for a person
 * and may change from one release to the next.  A line longer than the limit
 * is cut and ends in "...".
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

/*
 * What kind of failure an error is, which says what the caller can do about
 * it.  The text says what happened and is for a person; the class is for a
 * program, and no caller need match the text to learn it.
 *
 *	NONE		no error has been recorded
 *	REJECTED	something the language spells was refused, in text, a
 *			table or a call: a syntax, safety or stratification
 *			error, a query that does not parse or check, a
 *			statement past a structural limit such as 32
 *			arguments, a constant pattern that is not a regular
 *			expression, a table field that is not a value of its
 *			column's type, bytes that are not a symbol, a
 *			predicate name or arity no predicate can have
 *	EVALUATION	evaluation met a result the language leaves undefined:
 *			integer overflow, division by zero, a string longer than
 *			a value can hold, a pattern computed during evaluation
 *			that is not a regular expression
 *	LIMIT		a configured limit refused the operation, or a fixed
 *			capacity was reached, such as the number of distinct
 *			symbols a context can hold
 *	IO		a file could not be opened, read or closed
 *	MEMORY		the allocator refused, and no limit was the cause
 *	INTERRUPTED	relic_interrupt() stopped the operation
 *	ARGUMENT	the call was malformed in C's terms: a NULL argument,
 *			an index, enumerator or flag the library does not
 *			know, a value whose type or identifier is not one the
 *			context made, a query of another context, or a call
 *			made from one of the context's own callbacks
 *	INTERNAL	an invariant the library checks was false, which is a
 *			bug in relic
 *
 * A value is never renumbered or removed.  A class added later takes the
 * next value, and a caller meeting one it does not know treats it as a
 * failure it cannot classify.  In C++ the function's name hides the tag, so
 * a variable of the type is declared as `enum relic_error_class`.
 */
enum relic_error_class {
	RELIC_ERROR_NONE,
	RELIC_ERROR_REJECTED,
	RELIC_ERROR_EVALUATION,
	RELIC_ERROR_LIMIT,
	RELIC_ERROR_IO,
	RELIC_ERROR_MEMORY,
	RELIC_ERROR_INTERRUPTED,
	RELIC_ERROR_ARGUMENT,
	RELIC_ERROR_INTERNAL
};

/*
 * The class of the error relic_error() returns the text of, and
 * RELIC_ERROR_NONE when it returns NULL.  The two have one lifetime: both
 * are set when an error is reported and stand until the next one, and a
 * later success clears neither, so a caller reads them right after the
 * call that failed.  A call refused because the context was busy is
 * RELIC_ERROR_ARGUMENT.  Like relic_error(), it may be called from a
 * callback.
 */
RELIC_EXPORT enum relic_error_class relic_error_class(relic_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RELIC_H */
