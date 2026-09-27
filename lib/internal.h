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
 * Mark a parameter the implementation does not use, or a local or a static
 * function read only by an assertion, which NDEBUG leaves unread.  A function
 * whose shape is fixed by a callback type will have some; writing the attribute
 * is clearer than a (void) cast in the body, which reads like a statement that
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

/*
 * Ask the compiler to reject a call whose result is ignored, for a function
 * whose result says whether its caller may go on.
 */
#if defined(__GNUC__)
#define	RELIC_MUST_CHECK	__attribute__((__warn_unused_result__))
#else
#define	RELIC_MUST_CHECK
#endif

typedef struct relic_adornment relic_adornment_t;
typedef struct relic_adornment_set relic_adornment_set_t;
typedef struct relic_counters relic_counters_t;
typedef struct relic_diag relic_diag_t;
typedef struct relic_lexer relic_lexer_t;
typedef struct relic_magic relic_magic_t;
typedef struct relic_magic_limit relic_magic_limit_t;
typedef struct relic_staged_set relic_staged_set_t;
typedef struct relic_staging relic_staging_t;
typedef struct relic_materialized relic_materialized_t;
typedef struct relic_symbol relic_symbol_t;
typedef struct relic_symtab relic_symtab_t;
typedef struct relic_token relic_token_t;
typedef struct relic_valtab relic_valtab_t;

/*
 * The longest description of an activity: a verb of a few letters and the
 * description of a predicate or a query.
 */
#define	RELIC_ACTIVITY_TEXT_MAX	(16 + RELIC_PRED_TEXT_MAX)

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
 * failed learns whether anything said why.  `refused` records that a call
 * was refused during the operation in progress, because the context was
 * busy; relic_error() then describes the refusal in place of `last_error`
 * until the operation returns.
 *
 * `activity` says what the operation in progress is doing, such as
 * "planning path/2", for a refusal by the memory limit to name.  It is
 * written when a phase that uses a charged arena begins, and read only when
 * the account records such a refusal.
 */
struct relic_diag {
	relic_diag_cb	 callback;
	void		*callback_ctx;
	size_t		 error_count;
	char		 last_error[512];
	char		 activity[RELIC_ACTIVITY_TEXT_MAX];
	bool		 has_error;
	bool		 refused;
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
	uint64_t	 hash_seed;	/* the context's */
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
	uint64_t	 hash_seed;	/* the context's */
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
	uint64_t	tuples_looked_up;
};

/*
 * A query ready to run: a compiled rule whose head names no predicate, and
 * the relation its answer is derived into.
 *
 * A query prepared from text owns `arena`, which holds its syntax tree and
 * the variable-length parts of its rule, and is released when the query is
 * destroyed.  A query written in the program borrows its rule from the
 * program arena and leaves `arena` empty.  Either way the rule is a copy held
 * here rather than a pointer into the program's table of queries, which is a
 * growable array and moves.
 *
 * `answer` is the query's own relation, of the rule's head arity and named
 * after the query's serial number.  It is created with the query and freed
 * with it, and it is in no table of the context, so a query leaves nothing
 * behind in the context's predicates or relations.  It holds tuples only
 * while the query is being answered and is empty at every other moment.
 *
 * `ctx` is the context the query was compiled against.  Its predicates and
 * its constants belong to that context, so running it against another would
 * read identifiers that mean something else there.
 *
 * `running` is set while the query is being answered.  Destroying a query
 * reads nothing of its context, which a caller may have destroyed first,
 * except to refuse destroying the query being answered: while `running` is
 * set its context is certainly alive and busy with it.  A context points
 * into a query object only while answering it -- the plan that fills the
 * answer holds the answer's address, and is released before the answer
 * returns -- so destroying any other query from caller code the library is
 * running is safe.
 */
struct relic_query {
	relic_t			*ctx;
	relic_arena_t		 arena;
	relic_rule_t		 rule;		/* head RELIC_ID_NONE */
	relic_relation_t	*answer;
	bool			 running;
};

/*
 * One adornment a query's Magic rewrite recorded: `source` called with the
 * columns of `bound` bound, bit c for column c.  `adorned` is the predicate
 * that stands for that call, named from the source and the adornment, and
 * `magic` the predicate holding the bound values the call was made with, or
 * RELIC_ID_NONE when `bound` is empty and there is nothing to record.  Both
 * are RELIC_ID_NONE until the rewrite drafts rules, which it may never do.
 *
 * `kept_before` counts the adornments the search had kept when it recorded
 * this one: a kept adornment's own position among them, and for a subsumed
 * call the kept adornment explain lists it before, so that both are listed
 * in the order they were reached.
 */
struct relic_adornment {
	relic_pred_id_t	source;
	relic_pred_id_t	adorned;
	relic_pred_id_t	magic;
	uint32_t	bound;
	size_t		kept_before;
};

/*
 * Adornments in the order they were recorded, with `index` finding one by
 * source and bound columns: it holds positions in `entries` plus one, hashed
 * under `hash_seed`, the context's.
 */
struct relic_adornment_set {
	relic_adornment_t	*entries;
	size_t			 count;
	size_t			 capacity;
	relic_idtab_t		 index;
	uint64_t		 hash_seed;
};

/*
 * How much a query's Magic rewrite may generate of one thing: at most `most`,
 * and `configured` says whether that is a limit the caller set, which fails
 * the query when exceeded, or a budget the library chose, which abandons
 * the rewrite and leaves the query to be answered without it.  A caller who
 * sets either Magic limit leaves no budget in force, so a count they left
 * unset is then configured with `most` SIZE_MAX, and never exceeded.
 */
struct relic_magic_limit {
	size_t	most;
	bool	configured;
};

/*
 * A query's Magic rewrite, for one run of the query.
 *
 * The fields fall in two groups.  The first is what the rewrite hands the
 * evaluator and explain, and all they read; the second is the rewrite's own
 * working state, which only lib/magic.c touches.
 *
 * What the rewrite hands on:
 *
 * `complete_components` marks, by position in the program's schedule, the
 * components the query reads completely, which the evaluator evaluates
 * before the rewrite.  The predicates the query reads completely are those
 * the query reaches through negation, everything they depend on, and those
 * no rule derives, which their base stores hold whole.
 *
 * `kept` holds the adornments the rewrite generates predicates and rules
 * for, in the order they were first asked for, which is also the order their
 * rules are generated in.  `subsumed` holds the calls the query makes with
 * columns bound on a predicate it also reads with none bound: such a call
 * reads the free adornment, which holds every tuple of its source, so it
 * generates nothing and is recorded only for explain to show.  `rules` holds
 * the generated rules, the first `seed_count` of them generated by the
 * query's own body.  `query` is the query rewritten over adorned
 * predicates, kept apart from `rules` and run once after they reach their
 * fixed point.
 *
 * `reads_completely` is set when no adornment the rewrite keeps binds a
 * column.  Such a rewrite would derive exactly the relations of the
 * predicates it reaches, so it generates no rule: those predicates are read
 * completely instead, `completed_late` marks the components that adds to
 * `complete_components`, for the evaluator to evaluate once the rewrite has
 * returned, and `query` is the query as written.
 *
 * `over_budget` is set when a bound on what the rewrite generates that was
 * not configured, and so is a budget rather than a limit, is exceeded: the
 * rewrite then stops, with a warning rather than an error, and the query is
 * to be answered some other way.
 *
 * The rewrite's working state:
 *
 * `incomplete` marks the predicates the query reads on demand, indexed by
 * predicate identifier minus one for the `predicate_count` predicates that
 * existed when it was computed.  It is the planner's view of the rewrite,
 * since what the query reads on demand is what nothing has evaluated when the
 * rewrite is planned.  `component_of` gives, for each predicate, the position
 * of its component in the program's schedule, or SIZE_MAX when no rule
 * derives it.
 *
 * `rules_by_head` holds the indexes into the program's rule table of every
 * rule, grouped by head and in program order within a head: those deriving
 * predicate p are rules_by_head[first_rule_of[p - 1]] up to
 * rules_by_head[first_rule_of[p]].
 *
 * `base_view_count` counts the derived predicates whose base stores hold
 * facts, each of which has a base view and gives each of its adornments one
 * rule over it; the literal budget counts each as a rule of two literals.
 *
 * `pred_limit` bounds the predicates the rewrite generates, by a limit or a
 * budget, and `generated_preds` counts the adorned and magic predicates of
 * the adornments kept so far.  `rule_limit` bounds the rules it keeps by a
 * limit alone, SIZE_MAX when none is set: the literal budget bounds them
 * otherwise.  `literal_budget` bounds the body literals of every rule
 * drafted, kept or dropped, which `generated_literals` counts.  Configuring
 * either Magic limit lifts both budgets: an unset limit is then SIZE_MAX,
 * and so is the literal budget, which no limit configures.
 *
 * `rule_index` finds a generated rule by its canonical form, holding
 * positions in `rules` plus one.
 *
 * The adornments are found by one search or two, before any rule is
 * drafted.  `reads_free`, indexed by predicate identifier minus one like
 * `incomplete`, marks the predicates every call reads through the free
 * adornment: a search marks one when a call first reaches it free, and a
 * second search keeps the marks the first made.  Each search is held to the
 * limits and budgets on its own: the adornments it keeps count in
 * `generated_preds`, the rules it reads for them in `searched_rules`,
 * against the rule limit, and their bodies in `searched_literals`, against
 * the literal budget.  `scratch` holds what reading one rule needs, its
 * body's order and the planner's work, and is emptied once the rule is
 * read, so that a search holds no more than its adornments however many
 * rules it reads.
 *
 * Everything variable-length is in `arena`, in `scratch` or in an array
 * grown with relic_grow_array(), and relic_magic_reset() releases all of
 * it.
 */
struct relic_magic {
	/* What the rewrite hands on. */
	bool			*complete_components;
	bool			*completed_late;
	relic_adornment_set_t	 kept;
	relic_adornment_set_t	 subsumed;
	relic_ruletab_t		 rules;
	relic_rule_t		 query;
	size_t			 seed_count;
	bool			 over_budget;
	bool			 reads_completely;

	/* The rewrite's working state. */
	relic_t			*ctx;
	relic_arena_t		 arena;
	bool			*incomplete;
	size_t			*component_of;
	size_t			*rules_by_head;
	size_t			*first_rule_of;
	relic_idtab_t		 rule_index;
	uint64_t		 hash_seed;	/* the context's */
	size_t			 predicate_count;
	size_t			 base_view_count;
	size_t			 generated_preds;
	size_t			 generated_literals;
	size_t			 searched_rules;
	size_t			 searched_literals;
	relic_magic_limit_t	 pred_limit;
	relic_magic_limit_t	 rule_limit;
	size_t			 literal_budget;
	bool			*reads_free;
	relic_arena_t		 scratch;
};

/*
 * What the relations were last brought to the fixed point of: the number of
 * the program's rules and the number of facts its base stores held.  A
 * program changes only by gaining rules or facts, and both counts only
 * grow, so the relations are current exactly when a pair is recorded and
 * neither count has moved since.  relic_relations_current() asks, and only
 * a fixed point reached records a pair; relic_discard_derived() clears the
 * record with everything derived, so a failed evaluation leaves the
 * relations stale and the next one tries again.
 */
struct relic_materialized {
	size_t	rules;
	size_t	facts;
	bool	recorded;
};

/*
 * A relic context owns everything it allocates, and two contexts share
 * nothing: the library holds no mutable global state at all, which is what
 * lets an embedding program keep several contexts without a lock.  A single
 * context is not itself thread-safe.
 *
 * `materialized` records what the relations are the fixed point of; when
 * they are not current, the next evaluation discards what was derived and
 * starts again from the facts.
 *
 * `counters` is the work evaluation has done.  What the context holds is
 * not counted here: relic_stats() measures it when asked.
 *
 * `account` is what the memory limit is enforced against: the relation
 * store's storage and the chunks of the two charged arenas, `eval_arena` and
 * the arena of a query's Magic rewrite.  The other arenas hold what the
 * caller wrote and are not charged.
 *
 * `query_mixed_noted` records that RELIC_F_NOTE_MIXED has reported a
 * mixed-type comparison of the query being answered, or of a rule generated
 * from it.  A program rule keeps the same record on itself for the life of
 * the context; a query's is cleared when its answer begins, so each answer
 * is reported on once.
 *
 * `hash_seed` keys every hash the context's tables compute.  It is drawn
 * from the operating system when the context is created and never changes;
 * each table owner copies it when it is initialized.  No result depends on
 * it, and a program's author cannot learn it, which is what keeps an input
 * from being crafted to collide in a table.
 *
 * `busy` is set for the whole of an operation, from the public entry point
 * that began it until that entry point returns; relic_enter() and
 * relic_leave() are the only code that changes it.  Caller code runs during
 * an operation only by being called from it -- a diagnostic, explain or row
 * callback, or the read function of a stream handed to relic_load_file() --
 * and whatever it calls finds the context busy.  So no operation can find
 * its state changed under it, however the caller's code came to run.
 */
struct relic {
	relic_arena_t		 arena;	/* context lifetime: symbol bytes */
	relic_arena_t		 program_arena;	/* the parsed program */
	relic_arena_t		 eval_arena;	/* one evaluation's plans */
	relic_account_t		 account;
	relic_symtab_t		 symbols;
	relic_valtab_t		 values;
	relic_ast_program_t	 ast;
	relic_program_t		 program;
	relic_store_t		 store;
	relic_diag_t		 diag;
	relic_explain_cb	 explain;
	void			*explain_ctx;
	relic_config_t		 config;
	relic_counters_t	 counters;
	uint64_t		 hash_seed;
	relic_materialized_t	 materialized;
	bool			 query_mixed_noted;
	bool			 busy;
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
 * The one way into an operation, and out of it.  Every public entry point
 * that changes a context, or can call caller code, begins with
 * relic_enter() and returns only through relic_leave().  relic_enter()
 * refuses a context that is already busy: it returns -1 having recorded the
 * refusal, and the entry point returns its failure at once.  Entry points
 * never call one another inside the library, since the inner one would be
 * refused; each has a worker the others share instead.
 */
int	 relic_enter(relic_t *ctx);
void	 relic_leave(relic_t *ctx);

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
	    const relic_location_t *where, const char *format,
	    va_list arguments)
	    RELIC_PRINTFLIKE(4, 0);

/*
 * Report that an allocation failed, in the one wording the whole library
 * uses.  Out-of-memory is the failure every module can hit and the one whose
 * message carries the least information, so it is written once.
 *
 * A charged arena refused by the memory limit fails exactly as one refused
 * by the allocator, and every caller reports it here like any other failed
 * allocation.  So this is where the two are told apart: when the account
 * records that the limit refused, the report is the limit's, in the form
 * every limit shares, naming the activity in progress, and the record is
 * taken so that it is reported once.
 */
void	 relic_diag_no_memory(relic_t *ctx);

/*
 * Record what the operation in progress is now doing, as a verb and the
 * description of what it acts on -- "planning" and "path/2" -- which
 * relic_diag_no_memory() names when the memory limit refused a charged
 * arena.  A phase that allocates from a charged arena calls this before it
 * allocates.  A refusal the account recorded and nothing reported is
 * forgotten, so that no failure of this phase is blamed on an earlier one.
 */
void	 relic_diag_activity(relic_t *ctx, const char *verb,
	    const char *subject);

/*
 * Begin an operation with no activity recorded and no refusal: what the last
 * operation recorded and never reported belongs to it.  A charged arena used
 * before any activity is recorded is then reported as the limit without an
 * activity, rather than with the name of something an earlier operation did.
 */
void	 relic_diag_begin_operation(relic_t *ctx);

/*
 * Record that a call was refused because its context was busy.  relic_error()
 * describes the refusal until the next error is reported or the operation in
 * progress returns, whichever comes first, so a refusal never outlasts the
 * operation the refused caller code ran inside.  The diagnostic callback is
 * not called: the caller is inside one of its callbacks already, and possibly
 * inside that one.  Nor is the refusal counted among the operation's errors,
 * since the operation in progress has not failed.
 */
void	 relic_diag_record_refusal(relic_t *ctx);

/*
 * Shorten a name for a message into a caller-owned buffer, marking the cut
 * with "..." when one was made, and return the buffer so that the call can
 * stand as an argument of the message it decorates.
 */
const char *relic_diag_name(const char *name, char *buffer, size_t size);

/*
 * Mark a name that was cut to fill all of `buffer` by ending it in "...", as
 * relic_diag_name() marks the names it shortens.
 */
void	 relic_diag_mark_cut(char *buffer, size_t size);

/*
 * Describe one byte for a message, into a caller-owned buffer of
 * RELIC_BYTE_TEXT_MAX bytes, and return the buffer: as 'c' when the byte is
 * printable and not a backslash, and as 0x%02x otherwise, so that the
 * description is printable ASCII and holds no backslash.
 */
#define	RELIC_BYTE_TEXT_MAX	8

const char *relic_diag_byte(unsigned char byte, char *buffer, size_t size);

/*
 * lib/symbol.c
 */
const char *relic_symbol_bytes(const relic_t *ctx, uint32_t id,
	    size_t *lengthp);
size_t	 relic_symbol_count(const relic_t *ctx);
int	 relic_symbol_intern(relic_t *ctx, const void *bytes, size_t length,
	    uint32_t *idp);
void	 relic_symtab_init(relic_symtab_t *table, uint64_t hash_seed);
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
void	 relic_valtab_init(relic_valtab_t *table, uint64_t hash_seed);
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
	RELIC_LEX_INTEGER_TRAILING,	/* identifier byte after the digits */
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
 * `text` is `length` bytes, and nothing past them is read.  A NUL inside the
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
 * Whether `length` bytes are exactly one symbol as the lexer scans one: a
 * lowercase letter followed by letters, digits and underscores.  Bytes that
 * reach the program by another road than text -- a value or a predicate name
 * built through the interface -- are held to it, so that everything the
 * program holds is something the language could have written.
 */
bool	 relic_lex_is_symbol(const char *bytes, size_t length);

/*
 * Whether `length` bytes may name a predicate: a symbol, other than the
 * negation keyword, which the grammar reads as `not` wherever a predicate
 * name would stand.
 */
bool	 relic_lex_is_predicate_name(const char *bytes, size_t length);

/*
 * The language's integer rule applied to `length` bytes: the whole range must
 * be `-?[0-9]+` and its value must fit int64_t.  Returns RELIC_INTEGER_VALID
 * with the value in *valuep, or the way the bytes fail the rule, leaving
 * *valuep untouched, since a range that is no integer and one that is an
 * integer out of range are reported differently.  Nothing past the range is
 * read.  A leading zero is accepted, and `-0` is zero.
 */
enum relic_integer_verdict {
	RELIC_INTEGER_VALID,
	RELIC_INTEGER_MALFORMED,	/* not -?[0-9]+ */
	RELIC_INTEGER_OUT_OF_RANGE	/* -?[0-9]+ outside int64_t */
};

enum relic_integer_verdict relic_lex_integer(const char *bytes, size_t length,
	    int64_t *valuep);

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
 * The stable name a location may point at for the life of the context: the
 * source name interned, or "<input>" for NULL.  Returns NULL having reported
 * that memory ran out.
 */
#define	RELIC_UNNAMED_SOURCE	"<input>"

const char *relic_source_name(relic_t *ctx, const char *name);

/*
 * lib/pred.c
 *
 * relic_pred_intern() assigns an identifier to a (name, arity) pair, or
 * returns the one it already has; `where` is what a refusal names.  The
 * marks of struct relic_pred are set by relic_pred_name_head(),
 * relic_pred_mark_derived(), relic_pred_reference() and
 * relic_pred_make_view(), and read by relic_pred_is_derived() and
 * relic_pred_is_defined().
 *
 * relic_pred_describe() writes "name/arity" into the caller's buffer, which
 * must hold RELIC_PRED_TEXT_MAX bytes, and returns it so that the call can
 * stand as an argument of the message it decorates.
 */
void	 relic_predtab_init(relic_predtab_t *table, uint64_t hash_seed);
void	 relic_predtab_reset(relic_predtab_t *table);
int	 relic_pred_intern(relic_t *ctx, uint32_t name, uint16_t arity,
	    const relic_location_t *where, relic_pred_id_t *idp);
const relic_pred_t *relic_pred_of(const relic_t *ctx, relic_pred_id_t id);
size_t	 relic_pred_count(const relic_t *ctx);
void	 relic_pred_name_head(relic_t *ctx, relic_pred_id_t id);
void	 relic_pred_mark_derived(relic_t *ctx, relic_pred_id_t id);
void	 relic_pred_reference(relic_t *ctx, relic_pred_id_t id,
	    const relic_location_t *where);
void	 relic_pred_make_view(relic_t *ctx, relic_pred_id_t view,
	    relic_pred_id_t viewed);
bool	 relic_pred_is_derived(const relic_t *ctx, relic_pred_id_t id);
bool	 relic_pred_is_defined(const relic_t *ctx, relic_pred_id_t id);
const char *relic_pred_describe(const relic_t *ctx, relic_pred_id_t id,
	    char *buffer, size_t size);
void	 relic_pred_report_undefined(relic_t *ctx);

/*
 * lib/rule.c
 *
 * relic_program_init() prepares the empty program a context starts with,
 * whose predicate table hashes with `hash_seed`, and relic_program_reset()
 * releases what the program owns outside the program arena, the schedule's
 * arena among it.
 * The public entry point into this module is relic_check().
 *
 * relic_program_ready() is the check every use of the program begins with:
 * preparing a query, handing out one of the program's own, or evaluating.
 * Returns 0, or -1 with at least one error reported by this call.
 *
 * relic_ruletab_append() is the one way a rule joins a rule table, the
 * program's or a rewrite's, and marks its head predicate derived.
 */
void	 relic_program_init(relic_program_t *program, uint64_t hash_seed);
int	 relic_program_ready(relic_t *ctx);
void	 relic_program_reset(relic_program_t *program);
int	 relic_ruletab_append(relic_t *ctx, relic_ruletab_t *table,
	    const relic_rule_t *rule);

/*
 * lib/stratify.c
 *
 * relic_stratify() brings the program's schedule up to date with its rules,
 * rejecting a program with recursion through negation.  Returns 0 when the
 * analysis finished -- a program found unstratified has then been reported
 * and marked invalid -- or -1, reported, when memory ran out.
 *
 * relic_schedule_build() is the analysis itself, over any table of rules and
 * into a freshly initialized schedule; the program's check and a query's
 * Magic rewrite both use it.  It reports a cycle through negation and leaves
 * running out of memory to its caller to report, because the two mean
 * opposite things: a cycle is a mistake in the rules, and stays, while
 * running out of memory says nothing about them.
 */
enum relic_analysis_result {
	RELIC_ANALYSIS_STRATIFIED,
	RELIC_ANALYSIS_UNSTRATIFIED,
	RELIC_ANALYSIS_INCOMPLETE
};

void	 relic_schedule_init(relic_schedule_t *schedule);
void	 relic_schedule_reset(relic_schedule_t *schedule);
enum relic_analysis_result relic_schedule_build(relic_t *ctx,
	    const relic_ruletab_t *rules, relic_schedule_t *schedule);
int	 relic_stratify(relic_t *ctx);

/*
 * lib/plan.c
 *
 * Plan one rule's ordinary plan into `arena`, creating the relations it
 * reads and the indexes its lookups read.  The plan's EMIT writes into
 * `target`, which the caller names -- the relation of the head's predicate,
 * or a query's answer -- and whose arity must be the rule's head arity.
 * `incomplete`, indexed by predicate identifier minus one, marks the
 * relations still being derived, whose size the planner assumes rather than
 * measures; NULL marks none.  Returns 0, or -1 with a diagnostic reported:
 * memory or the memory limit refused, or the planner broke one of a plan's
 * invariants, which is an internal error.
 */
int	 relic_plan_rule(relic_t *ctx, relic_arena_t *arena,
	    const relic_rule_t *rule, relic_relation_t *target,
	    const bool *incomplete, relic_plan_t *plan);

/*
 * Plan a semi-naive delta variant of a rule into *plans (plan.h): its plan
 * with `delta_literal` ordered like any other positive literal, the same
 * variant with it placed first, and the delta size above which a round runs
 * the first rather than the second, emitting only the plans some delta
 * selects.  `delta_literal` is the body index of the positive literal that
 * reads the delta, which must be over a relation `incomplete` marks; the
 * variant's other literals over marked relations read the old or current
 * view.  Otherwise arguments and failures are as for relic_plan_rule().
 */
int	 relic_plan_variant(relic_t *ctx, relic_arena_t *arena,
	    const relic_rule_t *rule, relic_relation_t *target,
	    size_t delta_literal, const bool *incomplete,
	    relic_variant_plans_t *plans);

/*
 * The threshold between a variant's two plans, given the placed plan's
 * prefix estimate and the delta-first plan's fan-out estimate: the delta
 * size above which the placed plan is the cheaper, zero when it always is,
 * and RELIC_PLAN_NEVER when it never is.
 */
uint64_t relic_plan_delta_threshold(uint64_t prefix_estimate,
	    uint64_t fanout_estimate);

/*
 * The order relic_plan_rule() would place a rule's body literals in, as body
 * indexes in `order`, which has room for the whole body, but starting from
 * the slots in `bound` already bound.  Emits nothing and builds no index;
 * this is how the Magic rewrite orders a rule for the bindings its head is
 * called with.  Returns 0, or -1 with a diagnostic reported.
 */
int	 relic_plan_order(relic_t *ctx, relic_arena_t *arena,
	    const relic_rule_t *rule, uint64_t bound, const bool *incomplete,
	    size_t *order);

/*
 * The columns of an atom of the given arity that hold a constant or a slot
 * in `bound`: those a lookup placed at that point would be keyed on, and
 * those a Magic adornment calls bound.  A wildcard is never bound.
 */
uint32_t relic_plan_bound_columns(const relic_atom_t *atom, uint16_t arity,
	    uint64_t bound);

/*
 * Whether a plan, run once over relations that do not change while it
 * runs, derives each of its head tuples once: so that inserting what it
 * derives would reject nothing, and its derivations are a set as they come.
 */
bool	 relic_plan_derives_distinct(const relic_plan_t *plan);

/*
 * lib/magic.c
 *
 * A query's Magic rewrite.  relic_magic_find_complete() decides which
 * predicates the query reads completely, from the program's schedule, before
 * anything is evaluated.  Once the caller has evaluated those,
 * relic_magic_rewrite() generates the adorned and magic rules and the
 * rewritten query, ordering each rule's body with the planner, which then
 * measures what the caller evaluated; or, when no adornment it keeps binds
 * a column, generates nothing and sets `reads_completely`, and the
 * caller evaluates the components of `completed_late` too and runs the
 * query as written.  Each returns 0, or -1 with a
 * diagnostic reported: memory, a Magic limit exceeded, or a Magic budget
 * exceeded, which is reported as a warning and sets `over_budget`.
 * relic_magic_reset() releases the rewrite, whatever state it is in.
 */
void	 relic_magic_init(relic_magic_t *magic, relic_t *ctx);
void	 relic_magic_reset(relic_magic_t *magic);
int	 relic_magic_find_complete(relic_magic_t *magic,
	    const relic_rule_t *query);
int	 relic_magic_rewrite(relic_magic_t *magic, const relic_rule_t *query);

/*
 * lib/eval.c
 *
 * relic_query_answer() is relic_query_run() inside an operation already
 * entered, which relic_query_collect() shares.  Returns 0 once every row has
 * been delivered or the callback has stopped the stream, or -1 with an error
 * reported and no row delivered.
 *
 * relic_relations_current() says whether the relations are the fixed point
 * of the program as it stands.  relic_discard_derived() is the one discard:
 * it empties everything evaluation derived and clears the record of what
 * was materialized, and it cannot fail.  A load's commit calls it before it
 * adds a fact, and every evaluation that must start from the facts calls it
 * before it begins.
 */
int	 relic_query_answer(relic_t *ctx, relic_query_t *query,
	    relic_row_cb callback, void *callback_ctx);
bool	 relic_relations_current(const relic_t *ctx);
void	 relic_discard_derived(relic_t *ctx);

/*
 * lib/facts.c
 *
 * A load's facts are staged as they are read, then committed to the base
 * stores all or nothing.  relic_staging_add() stages one fact, given its
 * predicate and its arity of interned values; a fact staged twice is held
 * once.  relic_staging_commit() adds every staged fact the base stores lack,
 * or, refused by a limit or by memory, reports why and adds none.
 * relic_staging_reset() releases the staging, and is every load's exit.
 * `last` is the set the previous fact used, and `by_pred` holds each set's
 * position plus one, keyed by its predicate.
 */
struct relic_staging {
	relic_t			*ctx;
	relic_staged_set_t	*sets;
	size_t			 set_count;
	size_t			 set_capacity;
	size_t			 last;
	relic_idtab_t		 by_pred;
};

void	 relic_staging_init(relic_staging_t *staging, relic_t *ctx);
void	 relic_staging_reset(relic_staging_t *staging);
int	 relic_staging_add(relic_staging_t *staging, relic_pred_id_t pred,
	    const relic_dom_t *tuple);
int	 relic_staging_commit(relic_staging_t *staging);

/*
 * lib/import.c
 *
 * The table reader: the state machine that alone decides where the fields
 * and records of a csv or tsv table end, and what bytes each field holds
 * once its format's quoting or escapes are decoded.  It is a function of its
 * state and the bytes it is fed, with no context and no input or output of
 * its own, so it reads a table the same way whether the table arrives in one
 * piece or a byte at a time.
 *
 * relic_table_reader_init() prepares a reader of a table in `format` whose
 * records have `width` fields each, at least one, and names the locations
 * it reports `source_name`, which it borrows.  relic_table_feed() hands it
 * the next `length` bytes of the table, which it borrows until it asks for
 * more, and says whether they are the last.  relic_table_next() fills in
 * the next event and returns its kind:
 *
 *	RELIC_TABLE_MORE	every byte fed has been read; feed more
 *	RELIC_TABLE_FIELD	a field ended: its decoded bytes, where its
 *				first byte is, and its position in its record,
 *				from zero.  The bytes stay valid until the next
 *				call.
 *	RELIC_TABLE_RECORD	a record ended, with a field per column
 *	RELIC_TABLE_END		the table ended, at `where`, the end of the
 *				input
 *	RELIC_TABLE_MISTAKE	the table is malformed, as `mistake` says, at
 *				`where`
 *	RELIC_TABLE_NO_MEMORY	the field could not grow
 *
 * After END, MISTAKE or NO_MEMORY the reader stops, and every later call
 * returns the same event.  A record with the wrong number of fields is a
 * mistake: the reader checks the width when a field beyond the last column
 * begins, and when a record ends short, so the first mistake a table holds
 * is the first reported, whatever it is.
 *
 * relic_table_reader_reset() releases the field buffer.
 */
enum relic_table_event_kind {
	RELIC_TABLE_MORE,
	RELIC_TABLE_FIELD,
	RELIC_TABLE_RECORD,
	RELIC_TABLE_END,
	RELIC_TABLE_MISTAKE,
	RELIC_TABLE_NO_MEMORY
};

enum relic_table_mistake {
	RELIC_TABLE_TOO_MANY_FIELDS,	/* at the first extra field */
	RELIC_TABLE_TOO_FEW_FIELDS,	/* at the end of the record */
	RELIC_TABLE_UNKNOWN_ESCAPE,	/* tsv; at `byte`, after a backslash */
	RELIC_TABLE_ESCAPE_AT_END,	/* tsv; a backslash ends the input */
	RELIC_TABLE_STRAY_RETURN,	/* a return not before a line feed */
	RELIC_TABLE_QUOTE_IN_UNQUOTED,	/* csv; at the quote */
	RELIC_TABLE_AFTER_CLOSING_QUOTE, /* csv; at `byte` */
	RELIC_TABLE_UNTERMINATED_QUOTE	/* csv; at the opening quote */
};

/*
 * Where the reader stands.  Each state also decides what the end of the
 * input means there, since the end is not a byte.
 */
enum relic_table_state {
	RELIC_TABLE_AT_RECORD,		/* at the start of a record */
	RELIC_TABLE_AT_FIELD,		/* at a field, after a separator */
	RELIC_TABLE_IN_FIELD,		/* inside an unquoted field */
	RELIC_TABLE_IN_QUOTES,		/* inside a quoted csv field */
	RELIC_TABLE_AFTER_QUOTE,	/* after a quote inside quotes */
	RELIC_TABLE_AFTER_BACKSLASH,	/* after a tsv backslash */
	RELIC_TABLE_AFTER_RETURN,	/* after a carriage return */
	RELIC_TABLE_RECORD_ENDED,	/* the record's last field has ended */
	RELIC_TABLE_STOPPED		/* `stopped` is the last event */
};

typedef struct relic_table_event relic_table_event_t;
typedef struct relic_table_reader relic_table_reader_t;

/*
 * One event.  `bytes` and `length` are a field's decoded bytes, never NULL,
 * and `position` the field's position in its record.  `byte` is the byte a
 * mistake names, and `fields_found` the number of fields a record with too
 * few has.
 */
struct relic_table_event {
	const char			*bytes;
	size_t				 length;
	size_t				 position;
	size_t				 fields_found;
	relic_location_t		 where;
	enum relic_table_event_kind	 kind;
	enum relic_table_mistake	 mistake;
	unsigned char			 byte;
};

/*
 * A reader.  `input` is the part of the bytes last fed that has not been
 * read, and `input_is_last` says whether the table ends after it.
 * `field` accumulates the field being read; it grows by doubling to the
 * longest field and is reused for every one.  `fields_ended` counts the
 * fields of the current record read so far.
 *
 * `where` is the location of the next byte.  The other locations are the
 * places a later event points back to: the current field's first byte, the
 * carriage return that must be followed by a line feed, and the end of the
 * record whose last field has ended.
 */
struct relic_table_reader {
	const char			*input;
	char				*field;
	size_t				 input_length;
	size_t				 field_length;
	size_t				 field_capacity;
	size_t				 width;
	size_t				 fields_ended;
	relic_location_t		 where;
	relic_location_t		 field_where;
	relic_location_t		 return_where;
	relic_location_t		 record_end_where;
	relic_table_event_t		 stopped;
	enum relic_format		 format;
	enum relic_table_state		 state;
	bool				 input_is_last;
};

void	 relic_table_reader_init(relic_table_reader_t *reader,
	    enum relic_format format, size_t width, const char *source_name);
void	 relic_table_reader_reset(relic_table_reader_t *reader);
void	 relic_table_feed(relic_table_reader_t *reader, const char *bytes,
	    size_t length, bool last);
enum relic_table_event_kind relic_table_next(relic_table_reader_t *reader,
	    relic_table_event_t *event);

/*
 * lib/explain.c
 *
 * The lines of explain output, handed to the installed callback.
 * relic_explaining() says whether one is installed, so that a caller can skip
 * gathering what it would show.  Each of the others shows one thing when a
 * callback is installed and nothing otherwise: a component's heading and fact
 * count, a rule with its plan, one delta variant of the rule last shown, a
 * query with its plan, its EMIT shown as `stream` when `streams` says it
 * delivers the query's rows as it derives them, and the heading of a query's
 * Magic rewrite.  A query's rule names no predicate, so the two that show
 * one are given the query's answer relation, whose name its head is written
 * with.  `depth` is the level a component's heading is indented to, one more
 * for a component of a rewritten program than for one of the program
 * itself; what belongs to the component is indented beneath it.  None can
 * fail.
 */
bool	 relic_explaining(const relic_t *ctx);
void	 relic_explain_component(relic_t *ctx,
	    const relic_schedule_t *schedule, size_t position, size_t facts,
	    unsigned int depth);
void	 relic_explain_rule(relic_t *ctx, const relic_rule_t *rule,
	    const relic_plan_t *plan, unsigned int depth);
void	 relic_explain_variant(relic_t *ctx, const relic_rule_t *rule,
	    size_t delta_literal, const relic_plan_t *placed,
	    const relic_plan_t *delta_first, uint64_t threshold,
	    unsigned int depth);
void	 relic_explain_query(relic_t *ctx, const relic_rule_t *rule,
	    const relic_relation_t *answer, const relic_plan_t *plan,
	    bool streams);
void	 relic_explain_rewrite(relic_t *ctx, const relic_rule_t *query,
	    const relic_relation_t *answer, const relic_magic_t *magic);

/*
 * lib/index.c
 *
 * A secondary index over the tuples of one relation, keyed on the columns of
 * a mask.  relic_index_reserve() makes room for `tuples` tuples in all and
 * `added_keys` more keys and is the only operation that can fail;
 * relic_index_add() then links tuple `id` in, and tuples must be added in
 * increasing identifier order.
 * relic_index_first() and relic_index_next() walk, in identifier order, the
 * tuples that agree with a probe on the key columns; the probe is laid out as
 * a tuple, and its other columns are not read.  relic_index_first_since()
 * starts such a walk at the first of those tuples whose identifier is at
 * least `since`, from the key's frontier, which it moves forward; the starts
 * it is asked for should never decrease over the life of the index, as a
 * semi-naive delta's never do, or the walk costs the whole chain.
 * relic_index_bytes_after_reserve() is what the memory limit projects a
 * reservation with.  relic_index_free() releases an index, which
 * relic_index_init() must set up again before it is used.
 *
 * The relation store builds and extends indexes and does all the
 * accounting; the evaluator only walks them, moving a key's frontier as a
 * lookup of a delta walks toward the delta's start.
 */
void	 relic_index_init(relic_index_t *index, uint32_t key_mask);
void	 relic_index_free(relic_index_t *index);
size_t	 relic_index_bytes(const relic_index_t *index);
size_t	 relic_index_bytes_after_reserve(const relic_index_t *index,
	    size_t tuples, size_t added_keys);
int	 relic_index_reserve(relic_index_t *index,
	    const relic_relation_t *relation, size_t tuples, size_t added_keys);
void	 relic_index_add(relic_index_t *index, const relic_relation_t *relation,
	    relic_tuple_id_t id);
relic_tuple_id_t relic_index_first(const relic_index_t *index,
	    const relic_relation_t *relation, const relic_dom_t *probe);
relic_tuple_id_t relic_index_first_since(relic_index_t *index,
	    const relic_relation_t *relation, const relic_dom_t *probe,
	    relic_tuple_id_t since);
relic_tuple_id_t relic_index_next(const relic_index_t *index,
	    relic_tuple_id_t id);

/*
 * How many tuples relic_relation_insert_batch() prefetches for at a time,
 * and so how many an evaluation buffers before it inserts them.  Measured
 * on the closure workloads: 16 overlaps most of the misses of a duplicate
 * check, 64 a little more, and 256 no more than 64.
 */
#define	RELIC_INSERT_BATCH	64u

/*
 * lib/relation.c
 *
 * A predicate's relations are obtained through two functions, one per
 * role.  relic_relation_written() gives the derived relation a rule writes,
 * and refuses a predicate no rule derives.  relic_relation_read() gives the
 * relation an operation reads: the derived relation of a derived predicate,
 * and otherwise the base store of the predicate read, or of the one a base
 * view views.  Either is created empty the first time it is asked for, so
 * no caller decides whether a relation exists yet, and belongs to the
 * context and never moves, so a caller may hold as many of them as it likes
 * for as long as the context lives.
 *
 * relic_base_store() finds a predicate's base store without creating it,
 * and relic_base_count() and relic_base_total() count the facts one or
 * every base store holds; they serve what handles facts as facts: the
 * commit, seeding, explain and the question of whether the relations are
 * current.
 *
 * relic_relation_create() makes an empty relation, of a predicate or of a
 * query's answer; the table creates a predicate's through it, and a query
 * its answer, which the query owns.  relic_relation_clear() empties one
 * relation and gives back what it counted against both totals, and is the
 * one way a relation loses tuples.  relic_relation_free() releases a
 * relation that has been cleared and touches nothing else, which is what
 * lets a query be destroyed after its context.
 *
 * Every function below takes a relation of the context it was obtained from.
 *
 * The evaluator inserts into derived relations, through an EMIT and by
 * seeding one from its predicate's base store, and a load's commit appends
 * to base stores and nothing else does.  That is what makes every derived
 * tuple re-derivable, and so what makes discarding every derived relation
 * safe: the next evaluation seeds and derives it again.
 * relic_bases_reserve() makes room for a commit's new facts, checking both
 * limits once over every store it grows before growing any, and
 * relic_base_append() then appends one fact the store lacks without looking
 * anything up.
 *
 * relic_relation_insert() reports through *addedp whether the tuple was not
 * already present, which is what fixed-point detection reads, and returns -1
 * with a diagnostic when memory or a configured limit refuses it.  A refused
 * insertion adds no tuple and leaves the relation's contents and its index
 * agreeing with each other; capacity that a partial reservation obtained is
 * kept, and accounted.
 *
 * relic_relation_insert_batch() inserts `count` tuples, packed at the
 * relation's arity, exactly as relic_relation_insert() would one at a time
 * in order -- the same tuples added, under the same identifiers, refused at
 * the same one with the same diagnostic -- recording in added[i] whether
 * tuple i was added and in *completedp how many insertions completed before
 * any refusal.  It overlaps the cache misses of their duplicate checks,
 * RELIC_INSERT_BATCH tuples at a time.
 *
 * relic_relation_reserve() makes room for `added` more tuples -- their
 * columns, canonical index slots and room in every secondary index --
 * growing each array at most once, and keeps the storage total in step.  A
 * secondary index is given a key per tuple, the most the tuples could
 * bring, which over an index of few keys is far more than a batch would
 * use; a reservation for more than one tuple is therefore made only of a
 * relation without secondary indexes, as a base store is when a load
 * commits.
 *
 * A reservation applies no configured limit.
 * relic_relation_bytes_after_reserve() gives the bytes the relation would
 * then hold, and a caller bound by the memory limit projects that against
 * the account first, as an insertion does.  A reservation past the
 * identifier space is refused as a relation too large to grow; capacity a
 * failed reservation obtained is kept, and accounted.
 *
 * relic_relation_find() gives the identifier of the tuple holding some
 * columns, or RELIC_ID_NONE, and relic_relation_contains() whether there is
 * one.
 *
 * relic_relation_all() and relic_relation_since() give the identifier ranges
 * a scan and a semi-naive delta walk, and relic_relation_end() is the mark a
 * later delta is measured from.
 *
 * relic_relation_discard_all() empties every derived relation and drops the
 * base stores' secondary indexes, which ends the span over which a range
 * means anything; no caller may hold a range across it, nor one of a
 * relation across that relation's clear.
 *
 * relic_relation_index() finds, or builds from the tuples the relation holds,
 * the secondary index keyed on the columns of a mask and gives its position
 * in relation->indexes, which stays fixed until the next discard.  From then
 * on every insertion adds to it, and a discard drops it.  A build the memory
 * limit or memory refuses is reported and leaves no index behind.
 *
 * relic_relation_distinct() gives the number of distinct values in one
 * column, cached until the relation's tuple count changes.
 *
 * relic_relation_name() writes what a relation is called: its predicate's
 * name, or `$q` and the serial number of the query whose answer it is.
 * relic_relation_describe() writes "name/arity" for a message, into a
 * buffer of RELIC_PRED_TEXT_MAX bytes.  They are the only way a relation is
 * named, in explain output and in diagnostics alike.
 *
 * The two totals are what the configurable tuple and memory limits are
 * enforced against: tuples across every relation, and the bytes their columns
 * and indexes hold, which the memory limit counts together with the charged
 * arenas in the context's account.
 */
void	 relic_store_init(relic_store_t *store);
void	 relic_store_reset(relic_store_t *store);
relic_relation_t *relic_relation_written(relic_t *ctx, relic_pred_id_t pred);
relic_relation_t *relic_relation_read(relic_t *ctx, relic_pred_id_t pred);
const relic_relation_t *relic_base_store(const relic_t *ctx,
	    relic_pred_id_t pred);
size_t	 relic_base_count(const relic_t *ctx, relic_pred_id_t pred);
size_t	 relic_base_total(const relic_t *ctx);
int	 relic_bases_reserve(relic_t *ctx, const relic_base_growth_t *growths,
	    size_t count);
void	 relic_base_append(relic_t *ctx, relic_pred_id_t pred,
	    const relic_dom_t *columns);
relic_relation_t *relic_relation_create(relic_t *ctx, relic_pred_id_t pred,
	    size_t serial, uint16_t arity);
void	 relic_relation_clear(relic_t *ctx, relic_relation_t *relation);
void	 relic_relation_free(relic_relation_t *relation);
int	 relic_relation_insert(relic_t *ctx, relic_relation_t *relation,
	    const relic_dom_t *columns, bool *addedp);
int	 relic_relation_insert_batch(relic_t *ctx, relic_relation_t *relation,
	    const relic_dom_t *tuples, size_t count, bool *added,
	    size_t *completedp);
relic_tuple_id_t relic_relation_find(const relic_relation_t *relation,
	    const relic_dom_t *columns);
bool	 relic_relation_contains(const relic_relation_t *relation,
	    const relic_dom_t *columns);
const relic_dom_t *relic_relation_tuple(const relic_relation_t *relation,
	    relic_tuple_id_t id);
size_t	 relic_relation_bytes(const relic_relation_t *relation);
size_t	 relic_relation_bytes_after_reserve(const relic_relation_t *relation,
	    size_t added);
int	 relic_relation_reserve(relic_t *ctx, relic_relation_t *relation,
	    size_t added);
size_t	 relic_relation_count(const relic_relation_t *relation);
relic_tuple_id_t relic_relation_end(const relic_relation_t *relation);
relic_tuple_range_t relic_relation_all(const relic_relation_t *relation);
relic_tuple_range_t relic_relation_since(const relic_relation_t *relation,
	    relic_tuple_id_t first);
void	 relic_relation_discard_all(relic_t *ctx);
int	 relic_relation_index(relic_t *ctx, relic_relation_t *relation,
	    uint32_t key_mask, uint32_t *positionp);
int	 relic_relation_distinct(relic_t *ctx, relic_relation_t *relation,
	    uint16_t column, size_t *distinctp);
const char *relic_relation_name(const relic_t *ctx,
	    const relic_relation_t *relation, char *buffer, size_t size);
const char *relic_relation_describe(const relic_t *ctx,
	    const relic_relation_t *relation, char *buffer, size_t size);
size_t	 relic_relation_total_bytes(const relic_t *ctx);
size_t	 relic_relation_total_tuples(const relic_t *ctx);

/*
 * Whether a configured limit bounds what the relations may hold, so that an
 * insertion can be refused by a limit the caller set.  A query consults it
 * before delivering rows it does not store: under a limit a refusal has to
 * come before the first row, and a row that is never stored is never
 * refused.
 */
bool	 relic_limits_bound_relations(const relic_t *ctx);

#endif /* RELIC_INTERNAL_H */
