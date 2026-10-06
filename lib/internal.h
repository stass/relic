/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_INTERNAL_H
#define RELIC_INTERNAL_H

/*
 * This header defines what code compiled against frozen.h alone must not
 * see, and such code defines RELIC_FROZEN_ONLY to have it refused.
 */
#ifdef RELIC_FROZEN_ONLY
#error "code that only reads the store must see it only through frozen.h"
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <relic.h>

#include "account.h"
#include "arena.h"
#include "ast.h"
#include "atomic.h"
#include "cdefs.h"
#include "compat.h"
#include "exec.h"
#include "frozen.h"
#include "plan.h"
#include "program.h"
#include "queue.h"
#include "relation.h"
#include "round.h"
#include "strfn.h"
#include "util.h"

/*
 * Types and prototypes shared across lib/.  Nothing here is installed and
 * nothing here is exported from the shared library: every symbol declared
 * below has external linkage so that the modules can call one another, and
 * hidden visibility so that a program linked against librelic cannot.
 */

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
typedef struct relic_fresh_string relic_fresh_string_t;
typedef struct relic_symbol relic_symbol_t;
typedef struct relic_symtab relic_symtab_t;
typedef struct relic_token relic_token_t;
typedef struct relic_pending_map relic_pending_map_t;
typedef struct relic_valtab relic_valtab_t;
typedef struct relic_activity relic_activity_t;
typedef struct relic_walk relic_walk_t;

/*
 * Where diagnostics go, and the text of the last error.
 *
 * With no callback installed, diagnostics are discarded and an error is still
 * recoverable through relic_error().  Silence is the right default for a
 * library: a caller must not be made to accept output on its stderr.
 *
 * `error_count` counts every error reported, which is how an operation that
 * failed learns whether anything said why.  `last_class` is the class of
 * the error `last_error` holds, recorded with it.  `refused` records that a
 * call was refused during the operation in progress, because the context
 * was busy; relic_error() and relic_error_class() then describe the refusal
 * in place of the last error until the operation returns.
 *
 * `activity` says what the operation in progress is doing, for a refusal by
 * the memory limit to name.  It is written when an operation begins and
 * again when a phase that uses a charged arena begins, and read only when
 * the account records such a refusal.  `memory_reported` records that the
 * operation in progress has reported running out of memory, which it does
 * once.
 */
#define	RELIC_ACTIVITY_SUBJECT_MAX	256

/*
 * What an operation is doing, as a verb and the description of what it acts
 * on, such as "loading" and "graph.dl" or "planning" and "path/2".  The verb
 * is a string literal, NULL while nothing is recorded.  The subject is cut to
 * RELIC_ACTIVITY_SUBJECT_MAX bytes, room for a source's path.
 */
struct relic_activity {
	const char	*verb;
	char		 subject[RELIC_ACTIVITY_SUBJECT_MAX];
};

struct relic_diag {
	relic_diag_cb	 callback;
	void		*callback_ctx;
	size_t		 error_count;
	char		 last_error[512];
	enum relic_error_class last_class;
	relic_activity_t activity;
	bool		 has_error;
	bool		 refused;
	bool		 memory_reported;
};

/*
 * One interned byte string, found at index `id - 1` of the symbol table.
 *
 * The bytes live in the pool's string arena, where they never move, so a
 * pointer handed out by relic_string() stays valid until the context is
 * destroyed even after the array of these has been reallocated several
 * times.
 *
 * The length is recorded because a string literal may contain NUL bytes: the
 * lexer is byte-oriented and a string holds whatever bytes the source did.  A
 * terminator is stored past the recorded length anyway, so that a caller with
 * no use for the length can treat the result as a C string.  The hash is the
 * one the pool's index placed the entry by, kept so that growing the index
 * reads each entry's hash and none of its bytes.
 */
struct relic_symbol {
	const char	*bytes;
	uint32_t	 length;
	uint32_t	 hash;
};

/*
 * Byte strings interned to identifiers of at most RELIC_DOM_INDEX_MASK, so
 * that one fits beside a domain identifier's type.
 *
 * Symbols and string literals share this pool.  What distinguishes the symbol
 * foo from the string "foo" is the value that refers to the bytes, not the
 * bytes themselves, so the two intern to one symbol identifier and to two
 * different domain identifiers.
 *
 * The bytes are packed in an arena of alignment one: a string is read as
 * bytes and never as an object, so one symbol's bytes and terminator follow
 * the previous symbol's with no padding.
 */
struct relic_symtab {
	relic_symbol_t	*entries;	/* indexed by id - 1 */
	size_t		 count;
	size_t		 capacity;
	relic_idtab_t	 index;
	relic_arena_t	 strings;
	uint64_t	 hash_seed;	/* the context's */
};

/*
 * The integers interned to domain identifiers, and which symbols have been
 * interned as values of each type.
 *
 * A symbol or a string needs no entry here: its domain identifier is its
 * symbol identifier under its type, so the pool's one identifier per byte
 * string is already one identifier per value.  An integer is found by its
 * position in `integers`.  `symbol_uses` records, per symbol identifier,
 * whether the symbol has been interned as a symbol value and as a string
 * value, which is what counts the distinct values of each type.
 */
struct relic_valtab {
	int64_t		*integers;	/* indexed by the dom's index - 1 */
	size_t		 integer_count;
	size_t		 integer_capacity;
	relic_idtab_t	 index;		/* over integers */
	uint8_t		*symbol_uses;	/* indexed by symbol id - 1 */
	size_t		 symbol_use_capacity;
	size_t		 symbol_values;
	size_t		 string_values;
	uint64_t	 hash_seed;	/* the context's */
};

/*
 * The work evaluation has done over the life of a context, which the
 * executor advances as it runs and relic_stats() reports.  Each field is
 * defined where relic_stats_t documents the one of the same name, and the
 * fields of `read` are relic_stats_t's tuples_scanned and tuples_looked_up.
 */
struct relic_counters {
	uint64_t		iterations;
	uint64_t		derivations;
	uint64_t		tuples_inserted;
	relic_exec_counters_t	read;
	uint64_t		parallel_rounds;
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
 * `group_rules` are the group rules of the query's aggregates, in body
 * order, `group_rule_count` of them, held in the arena that holds the
 * query's rule.  Answering the query runs each once, into its group
 * predicate's relation, and empties those relations again with the answer.
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
	const relic_rule_t	*group_rules;
	size_t			 group_rule_count;
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
 *
 * `walks` holds, for a kept adornment, the walk of each rule deriving its
 * source, in program order, which the search records as it reads the rules
 * and the draft replays.  It is NULL until the search reads them, once the
 * rewrite has returned, and for a subsumed call.
 */
struct relic_adornment {
	relic_pred_id_t	 source;
	relic_pred_id_t	 adorned;
	relic_pred_id_t	 magic;
	uint32_t	 bound;
	size_t		 kept_before;
	relic_walk_t	*walks;
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
 * drafted, kept or dropped, which `generated_literals` counts.  The budget
 * and both of its counts, this one and `searched_literals`, are in the
 * units relic_body_size() gives, a literal counting one and one more per
 * node of its expressions, so that none of them counts in literals alone
 * while another counts nodes.  Configuring
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
 * the literal budget.  A search records the walk of every rule it reads,
 * `query_walk` the query's and an adornment's `walks` the rest, and the
 * draft replays the walks of the last search rather than walking any body
 * itself.  `walked` holds the adornments the first search kept, with their
 * walks, while a second search reads its walks from them.  The walks, a
 * step per body literal read, are in `walk_arena`, which is emptied when
 * the rewrite returns.  `scratch` holds what walking or drafting one rule
 * needs, the planner's work and the drafts, and is emptied once the rule is
 * walked or drafted, so that a search holds no more than its adornments and
 * their walks however many rules it reads, and `arena` no more than the
 * rules kept however many are drafted.
 *
 * Everything variable-length is in `arena`, in `walk_arena`, in `scratch`
 * or in an array grown with relic_grow_array(), and relic_magic_reset()
 * releases all of it.
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
	relic_walk_t		*query_walk;
	relic_adornment_set_t	 walked;
	relic_arena_t		 walk_arena;
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
 * store's storage, the chunks of the charged arenas, `eval_arena` and the
 * arenas of a query's Magic rewrite, and the buffers of a deferred round
 * while it runs.  The other arenas hold what the caller wrote and are not
 * charged.
 *
 * `pool` runs the rounds whose insertions wait for their end (lib/round.c).
 * It is NULL until the first such round, and then kept, with the storage
 * it has grown, until the context is destroyed.
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
 *
 * `interrupted` is raised by relic_interrupt(), from a signal handler,
 * another thread or caller code, and lowered by relic_enter() as each
 * operation begins, so it asks only the operation in progress to stop.  It
 * is the one member written from outside an operation.  An evaluation
 * reads it after every return of the executor, a worker of a shared round
 * likewise, through the job, and the delivery of a gathered answer before
 * each row; elsewhere it is ignored.
 */
struct relic {
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
	relic_pool_t		*pool;
	relic_signal_flag_t	 interrupted;
	bool			 query_mixed_noted;
	bool			 busy;
};

/*
 * lib/api.c
 *
 * The one way into an operation, and out of it.  Every public entry point
 * that changes a context, or can call caller code, begins with
 * relic_enter() and returns only through relic_leave().  relic_enter()
 * refuses a context that is already busy: it returns -1 having recorded the
 * refusal, and the entry point returns its failure at once.  Entry points
 * never call one another inside the library, since the inner one would be
 * refused; each has a worker the others share instead.
 *
 * An operation states its activity as it enters, as relic_diag_activity()
 * takes it: a verb and what it acts on, such as "loading" and the source's
 * name, which a refusal by the memory limit names.  An operation with
 * nothing of its own to name -- one that only reads, or an evaluation, whose
 * phases record their own -- passes a NULL verb and subject.
 */
int	 relic_enter(relic_t *ctx, const char *verb, const char *subject);
void	 relic_leave(relic_t *ctx);

/*
 * lib/diag.c
 *
 * Each of these formats a diagnostic and hands it to the installed
 * callback.  A note and a warning take no class, since neither is a
 * failure.  An error is reported only through a function that names its
 * class, as an argument or, for relic_diag_internal(), by being that
 * class's function, so no error can be reported without one; the class is
 * recorded with the text as the last error before the callback runs, and
 * an error of class RELIC_ERROR_INTERNAL begins "internal error: " however
 * it was reported.  A message longer than the buffer is truncated rather
 * than allocated for: a diagnostic reports a failure and must not be able
 * to cause one.
 */
void	 relic_diag_init(relic_diag_t *diag);
void	 relic_diag_note(relic_t *ctx, const relic_location_t *where,
	    const char *format, ...)
	    RELIC_PRINTFLIKE(3, 4);
void	 relic_diag_warning(relic_t *ctx, const relic_location_t *where,
	    const char *format, ...)
	    RELIC_PRINTFLIKE(3, 4);
void	 relic_diag_error(relic_t *ctx, enum relic_error_class error_class,
	    const relic_location_t *where, const char *format, ...)
	    RELIC_PRINTFLIKE(4, 5);

/*
 * relic_diag_error() for a caller that already holds a va_list.  The parser
 * formats its own messages through this because it decides whether a
 * diagnostic is reported at all, and that decision has to sit in front of
 * the call rather than inside it.
 */
void	 relic_diag_verror(relic_t *ctx, enum relic_error_class error_class,
	    const relic_location_t *where, const char *format,
	    va_list arguments)
	    RELIC_PRINTFLIKE(4, 0);

/*
 * Report that an invariant the library checks at run time is false, as an
 * error of class RELIC_ERROR_INTERNAL, whose text begins "internal error: "
 * as that class's always does.
 */
void	 relic_diag_internal(relic_t *ctx, const relic_location_t *where,
	    const char *format, ...)
	    RELIC_PRINTFLIKE(3, 4);

/*
 * Report that an allocation failed, in the one wording the whole library
 * uses.  Out-of-memory is the failure every module can hit and the one whose
 * message carries the least information, so it is written once.
 *
 * A charged arena refused by the memory limit fails exactly as one refused
 * by the allocator, and every caller reports it here like any other failed
 * allocation.  So this is where the two are told apart: when the account
 * records that the limit refused, the report is the limit's, of class
 * RELIC_ERROR_LIMIT and in the form every limit shares, naming the activity
 * in progress, and the record is taken so that it is reported once;
 * otherwise it is of class RELIC_ERROR_MEMORY.
 *
 * An operation reports running out of memory once.  An arena that refused
 * refuses every later request, so the caller that makes a second allocation
 * before checking the first, or the construction that meets one exhaustion
 * in several arenas, calls this again for the same failure; every call
 * after the operation's first reports nothing and takes the account's
 * record, which says nothing the first report did not.  No caller has to
 * check one allocation before making the next for the report to be right.
 */
void	 relic_diag_no_memory(relic_t *ctx);

/*
 * Report a configured limit refusing something, in the form every limit
 * shares: "limit exceeded: memory=1048576 while deriving path/2" -- the
 * limit, what it was set to, and what was being done, as a verb and the
 * description of what it acts on.  With a NULL verb the clause naming the
 * activity is left out.  The error's class is RELIC_ERROR_LIMIT.
 */
void	 relic_diag_limit(relic_t *ctx, const char *limit, size_t configured,
	    const char *verb, const char *subject);

/*
 * Record what the operation in progress is now doing, as a verb and the
 * description of what it acts on -- "planning" and "path/2" -- which
 * relic_diag_no_memory() names when the memory limit refused a charged
 * arena.  The verb is kept by reference, so it must be a string literal; the
 * subject is copied, cut and marked when it is longer than
 * RELIC_ACTIVITY_SUBJECT_MAX.  A NULL verb records that nothing is named,
 * and the subject is then ignored.  An operation records its activity as
 * it begins, through relic_enter() and relic_diag_begin_operation(), and a
 * phase that allocates from a charged arena calls this before it
 * allocates.  A refusal the account recorded and nothing
 * reported is forgotten, so that no failure of this activity is blamed on an
 * earlier one.
 */
void	 relic_diag_activity(relic_t *ctx, const char *verb,
	    const char *subject);

/*
 * Begin an operation: it has reported nothing about memory yet, and its
 * activity is `verb` and `subject`, as relic_diag_activity() takes them.
 */
void	 relic_diag_begin_operation(relic_t *ctx, const char *verb,
	    const char *subject);

/*
 * A phase that runs inside more than one kind of operation, as a check runs
 * inside a query's preparation, records an activity of its own and puts the
 * operation's back when it ends.  relic_diag_save_activity() copies the
 * activity in progress into *saved, and relic_diag_restore_activity() makes
 * it current again; restoring forgets nothing, since a refusal in the phase
 * has been reported by the time the phase returns.
 */
void	 relic_diag_save_activity(const relic_t *ctx, relic_activity_t *saved);
void	 relic_diag_restore_activity(relic_t *ctx,
	    const relic_activity_t *saved);

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
 * relic_diag_name() for a name given as `length` bytes with no terminator,
 * such as a spelling still in the text being parsed.
 */
const char *relic_diag_spelling(const char *bytes, size_t length,
	    char *buffer, size_t size);

/*
 * Room for a string a message quotes: about forty bytes of literal, its
 * quotes, and the mark of a cut.
 */
#define	RELIC_STRING_TEXT_MAX	48

/*
 * Write a string for a message as a quoted literal that spells its bytes --
 * a backslash and a quote escaped, and every byte outside printable ASCII
 * as \xHH -- into a caller-owned buffer, cut and marked as
 * relic_diag_name() cuts a name, and return the buffer.
 */
const char *relic_diag_string(const char *bytes, size_t length, char *buffer,
	    size_t size);

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
 *
 * relic_symbol_hash() gives the hash the pool places bytes by, and
 * relic_symbol_find() the identifier of the entry holding bytes of that
 * hash, or RELIC_ID_NONE: the lookup half of interning, which writes
 * nothing.  relic_symbol_pool_bytes() is what the pool occupies -- its
 * entries, its index and its arena of bytes -- which the memory account
 * counts as `pool_bytes`.
 */
const char *relic_symbol_bytes(const relic_t *ctx, uint32_t id,
	    size_t *lengthp);
size_t	 relic_symbol_count(const relic_t *ctx);
uint32_t relic_symbol_find(const relic_t *ctx, const void *bytes,
	    size_t length, uint32_t hash);
uint32_t relic_symbol_hash(const relic_t *ctx, const void *bytes,
	    size_t length);
int	 relic_symbol_intern(relic_t *ctx, const void *bytes, size_t length,
	    uint32_t *idp);
size_t	 relic_symbol_pool_bytes(const relic_t *ctx);
void	 relic_symtab_init(relic_symtab_t *table, uint64_t hash_seed);
void	 relic_symtab_reset(relic_symtab_t *table);

/*
 * A pool of the symbol pool's type that no context owns, as a collected
 * result's is: the pool's lookup, reading and interning on the table
 * itself, which charge nothing and report nothing.  relic_symtab_intern()
 * returns -1, every symbol as it was, when the length or the pool's ceiling
 * refuses the bytes or memory ran out.
 */
const char *relic_symtab_bytes(const relic_symtab_t *table, uint32_t id,
	    size_t *lengthp);
uint32_t relic_symtab_find(const relic_symtab_t *table, const void *bytes,
	    size_t length, uint32_t hash);
int	 relic_symtab_intern(relic_symtab_t *table, const void *bytes,
	    size_t length, uint32_t *idp);

/*
 * Interning, all at once, byte strings the pool does not hold, distinct
 * from one another, each of which is to receive the identifier after the
 * last: `count` of them, in the order they are to be numbered.  This is
 * relic_symbol_intern() split into the steps a caller that must ask the
 * memory limit first, and must not fail once it starts to write, needs.
 *
 * relic_symbol_pool_bytes_after() is what the pool would occupy once it
 * held them, having grown as interning them one at a time would grow it,
 * or SIZE_MAX when a size cannot count it.  It reads the pool and writes
 * nothing.  relic_symbol_has_room() says whether identifiers remain for
 * them.  relic_symbol_reserve() makes room for them all -- the index, the
 * entries, and a copy of each one's bytes in the arena -- and returns -1,
 * having reported nothing, when there is not the room or the allocator
 * refuses; the index and the entries it grew stay grown and counted, no
 * copy stays in the arena, and the pool's contents are as they were.
 * relic_symbol_append() then numbers them, and cannot fail.  A reservation
 * sets *copiesp to where its copies begin in the arena, and
 * relic_symbol_unreserve() gives back the copies of one that will not be
 * appended after all, leaving the pool as a reservation that failed does:
 * nothing may allocate from the pool between the two.
 *
 * A fresh string's `bytes` must stay readable until it is reserved, and
 * `copy` is where the reservation put it, or NULL once a reservation has
 * failed or been given back.
 */
struct relic_fresh_string {
	const char	*bytes;
	char		*copy;
	size_t		 length;
	uint32_t	 hash;
};

size_t	 relic_symbol_pool_bytes_after(const relic_t *ctx,
	    const relic_fresh_string_t *fresh, size_t count);
bool	 relic_symbol_has_room(const relic_t *ctx, size_t count);
int	 relic_symbol_reserve(relic_t *ctx, relic_fresh_string_t *fresh,
	    size_t count, relic_arena_mark_t *copiesp);
void	 relic_symbol_unreserve(relic_t *ctx, relic_fresh_string_t *fresh,
	    size_t count, const relic_arena_mark_t *copies);
void	 relic_symbol_append(relic_t *ctx, const relic_fresh_string_t *fresh,
	    size_t count);

/*
 * lib/value.c
 *
 * relic_value_decode() fills *value with the value a domain identifier names
 * and returns true, or returns false for an identifier the context never
 * assigned.  relic_value_total_order() and relic_value_order(), which
 * frozen.h declares, treat such an identifier as unordered and unequal
 * rather than reading past a table.  relic_value_storage_bytes() is what
 * the value table and the pool occupy together, which the memory account
 * counts as `value_bytes` and `pool_bytes`.
 */
size_t	 relic_value_count(const relic_t *ctx);
size_t	 relic_value_count_of(const relic_t *ctx, enum relic_type type);
bool	 relic_value_decode(const relic_t *ctx, relic_dom_t dom,
	    relic_value_t *value);
int	 relic_value_total_order(const relic_t *ctx, relic_dom_t a,
	    relic_dom_t b);

/*
 * The bit a collected result's symbol and string identifiers carry, which
 * no identifier a context assigns has, since a context's stay below the
 * domain's type bits: a result's value handed to the context is refused as
 * one it never assigned, rather than read as some value of its own.
 */
#define	RELIC_RESULT_ID_BIT	UINT32_C(0x80000000)

_Static_assert(RELIC_DOM_INDEX_MASK < RELIC_RESULT_ID_BIT,
    "a context's identifiers stay below a result's bit");

/*
 * The bytes a symbol or string identifier names in `source`, or NULL for
 * one the source never assigned: the context's pool, or a result's.
 * relic_value_order_totally() is the total order relic_value_compare()
 * documents, over values whose bytes `bytes_of` reads from `source`.
 */
typedef const char *(*relic_bytes_of_fn)(const void *source, uint32_t id,
	    size_t *lengthp);

int	 relic_value_order_totally(const relic_value_t *left,
	    const relic_value_t *right, relic_bytes_of_fn bytes_of,
	    const void *source);
int	 relic_value_intern(relic_t *ctx, const relic_value_t *value,
	    relic_dom_t *domp);
int	 relic_value_intern_bytes(relic_t *ctx, enum relic_type type,
	    const void *bytes, size_t length, relic_dom_t *domp);
int	 relic_value_intern_constant(relic_t *ctx,
	    const relic_constant_t *constant, relic_dom_t *domp);
int	 relic_value_intern_integer(relic_t *ctx, int64_t value,
	    relic_dom_t *domp);
size_t	 relic_value_storage_bytes(const relic_t *ctx);
void	 relic_valtab_init(relic_valtab_t *table, uint64_t hash_seed);
void	 relic_valtab_reset(relic_valtab_t *table);

/*
 * Interning the values a batch of derived tuples holds pending, in three
 * steps that separate what reads the value table and the pool from what
 * writes them.
 *
 * An integer or a string the executor computed and the context had not
 * assigned has no identifier yet, and its column holds a pending marker
 * (lib/exec.h), which names an entry of the batch's pending values; a
 * string's bytes are in the batch's pending bytes.  The integer table and
 * the pool assign identifiers in order, so the identifier a new value will
 * receive is known before it is assigned: the count plus the value's
 * position among the new values of its kind in the batch, in order of first
 * occurrence.
 *
 * relic_pending_measure() walks the batch's `count` pending values, the
 * strings' bytes in `bytes`, and fills `map` with each one's identifier, the
 * one the context has assigned it or the one it will be given, and returns the
 * bytes the value table and the pool together will occupy once the new values
 * are interned: the capacity interning them one at a time would reach, so that
 * each holds the same bytes however its values arrived, or SIZE_MAX when a
 * size cannot count it.  A string whose bytes the pool holds with no string
 * value assigned to them -- a symbol's spelling, or a value a caller made and
 * has not interned -- is promised the pool's identifier for them, and the
 * bytes must stay where they are until the commit.  It writes nothing but the
 * map.
 *
 * relic_pending_rewrite() replaces every pending marker in `tuples` tuples
 * of `arity` columns with the identifier the map gives it.  It writes
 * nothing but the tuples.
 *
 * relic_pending_commit() interns the map's new values, each under exactly
 * the identifier the map promised, and makes every string it promised a
 * string value.  It returns 0, or -1 with a diagnostic reported when the
 * table or the pool could not grow, for memory or because it holds as many
 * values as an identifier can name; their contents are then as they were.
 * It is two halves, for a caller that must make every fallible step before
 * it changes anything and must report nothing itself:
 * relic_pending_reserve() grows the table and the pool to hold the new
 * values, returning -1, with no diagnostic and the contents unchanged, when
 * it could not, and relic_pending_append() then interns them, and cannot
 * fail.  Nothing may intern a value between the two.  A caller that decides
 * not to append after a reservation succeeded calls
 * relic_pending_unreserve() instead, which leaves the table and the pool as
 * a reservation that failed would: grown and counted, with no copy of a new
 * string left behind.
 *
 * A map is given room for `room` pending values by
 * relic_pending_map_init(), from `arena`, and returns -1 when the arena
 * refused, which the caller's boundary reports.  relic_pending_map_open()
 * gives it that room from the allocator instead, for a caller that charges
 * the map itself: relic_pending_map_bytes() is what the map then holds,
 * SIZE_MAX when a size cannot count it, and relic_pending_map_close()
 * frees it.  An opened map has room for at least one pending value and at
 * most RELIC_DOM_INDEX_MASK.
 */
struct relic_pending_map {
	relic_dom_t		*ids;		/* per pending value, batch order */
	int64_t			*fresh;		/* new integers, as first met */
	uint32_t		*seen;		/* hash of fresh, holding index + 1 */
	relic_fresh_string_t	*strings;	/* new strings, as first met */
	uint32_t		*strings_seen;	/* hash of strings, index + 1 */
	size_t			 room;
	size_t			 count;
	size_t			 fresh_count;
	size_t			 string_count;
	uint32_t		 highest_string; /* symbol, or RELIC_ID_NONE */
	relic_arena_mark_t	 copies;	/* the reserved strings' */
};

int	 relic_pending_map_init(relic_pending_map_t *map, relic_arena_t *arena,
	    size_t room);
size_t	 relic_pending_map_bytes(size_t room);
int	 relic_pending_map_open(relic_pending_map_t *map, size_t room);
void	 relic_pending_map_close(relic_pending_map_t *map);
size_t	 relic_pending_measure(const relic_t *ctx,
	    const relic_pending_t *pending, size_t count, const char *bytes,
	    relic_pending_map_t *map);
void	 relic_pending_rewrite(const relic_pending_map_t *map,
	    relic_dom_t *tuples, size_t count, uint16_t arity);
int	 relic_pending_commit(relic_t *ctx, relic_pending_map_t *map);
int	 relic_pending_reserve(relic_t *ctx, relic_pending_map_t *map);
void	 relic_pending_unreserve(relic_t *ctx, relic_pending_map_t *map);
void	 relic_pending_append(relic_t *ctx, const relic_pending_map_t *map);

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
	RELIC_TOKEN_INTEGER,	/* [0-9]+, its magnitude in `magnitude` */
	RELIC_TOKEN_OPERATOR,	/* + - * /, which one in `arith` */
	RELIC_TOKEN_COMPARE,	/* = != < <= > >=, which one in `compare` */
	RELIC_TOKEN_IMPLIES,	/* :- */
	RELIC_TOKEN_LPAREN,
	RELIC_TOKEN_RPAREN,
	RELIC_TOKEN_LBRACE,	/* {, which opens an aggregate's body */
	RELIC_TOKEN_RBRACE,
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
	RELIC_LEX_INTEGER_TRAILING,	/* identifier byte after the digits */
	RELIC_LEX_NAME_TOO_LONG,	/* longer than a value may be */
	RELIC_LEX_NUMBER_UNSUPPORTED,	/* the reserved floating-point form */
	RELIC_LEX_STRING_TOO_LONG,	/* decoded, longer than a value */
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
 * `bytes` and `length` are a name's spelling, which is its text, and a
 * string literal's *decoded* bytes, which are in the lexer's scratch buffer
 * and stay valid only until the next token is scanned.  The lexer interns
 * nothing: an identifier does not yet know whether it names a predicate, a
 * variable, a function or a symbol constant, and only a constant is a value.
 * Neither is longer than a value may be, RELIC_STRING_LENGTH_MAX bytes: a
 * longer one is an error token, which keeps every length a spelling or a
 * constant holds within 32 bits.
 *
 * `magnitude` is an integer literal's digits as a number, without a sign: a
 * `-` is always a token of its own, since it may be a subtraction, and the
 * parser joins it to the digits where it makes a negative literal.  So only
 * the parser knows the sign, and only it decides whether the value fits.  A
 * magnitude above RELIC_MAGNITUDE_MAX fits no sign, and is recorded as
 * RELIC_MAGNITUDE_MAX + 1 however large it was, which every range check
 * refuses without a case of its own.
 */
struct relic_token {
	const char		*text;
	size_t			 text_length;
	const char		*bytes;		/* names and string bytes */
	size_t			 length;
	relic_location_t	 where;
	uint64_t		 magnitude;	/* RELIC_TOKEN_INTEGER */
	enum relic_token_kind	 kind;
	enum relic_lex_error	 error;
	enum relic_compare_op	 compare;	/* RELIC_TOKEN_COMPARE */
	enum relic_arith_op	 arith;		/* RELIC_TOKEN_OPERATOR */
	unsigned char		 byte;		/* the byte an error names */
};

/*
 * A scan in progress.
 *
 * `text` is `length` bytes, and nothing past them is read.  A NUL inside the
 * range is an ordinary byte that begins no token.
 *
 * `decoded` is the scratch buffer a string literal's escapes are decoded
 * into, which the literal's token points at.  It is reused by every literal
 * in the input and released by relic_lexer_reset().
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
 * error token, or -1 when a string literal's scratch buffer could not grow,
 * in which case the diagnostic has already been reported and scanning cannot
 * continue.
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
 * The language's integer rule, in two pieces that the lexer and the parser
 * each use and that table import uses together.
 *
 * relic_lex_magnitude() converts `length` bytes that must be `[0-9]+` into
 * their magnitude, which must be at most RELIC_MAGNITUDE_MAX, the magnitude
 * of INT64_MIN.  relic_integer_signed() gives a magnitude a sign, and says
 * whether the result fits int64_t: RELIC_MAGNITUDE_MAX fits only when it is
 * negative.
 *
 * relic_lex_integer() is the whole rule applied to `length` bytes: the range
 * must be `-?[0-9]+` and its value must fit int64_t.
 *
 * Each returns RELIC_INTEGER_VALID with its result stored, or the way the
 * bytes fail the rule, leaving the result untouched, since a range that is
 * no integer and one that is an integer out of range are reported
 * differently.  Nothing past the range is read.  A leading zero is
 * accepted, and `-0` is zero.
 */
#define	RELIC_MAGNITUDE_MAX	((uint64_t)INT64_MAX + 1u)

enum relic_integer_verdict {
	RELIC_INTEGER_VALID,
	RELIC_INTEGER_MALFORMED,	/* not the digits the rule asks for */
	RELIC_INTEGER_OUT_OF_RANGE	/* digits whose value does not fit */
};

enum relic_integer_verdict relic_lex_magnitude(const char *bytes,
	    size_t length, uint64_t *magnitudep);
enum relic_integer_verdict relic_integer_signed(uint64_t magnitude,
	    bool negative, int64_t *valuep);
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
 * Open the file at `path` for a load or an import to read, closed on exec so
 * that a child an embedder starts meanwhile does not inherit it.  Returns
 * NULL having reported why it could not be opened.
 */
FILE	*relic_open_input(relic_t *ctx, const char *path);

/*
 * Close a file relic_open_input() opened, once what it holds has been read
 * and before any of it is committed, so that a close that fails fails the
 * load or the import with nothing added.  Returns 0, or -1 having reported
 * the failure.
 */
int	 relic_close_input(relic_t *ctx, const char *path, FILE *stream);

/*
 * The name the locations of what is parsed under a source name point at: a
 * copy of the name, or of "<input>" for NULL, in `arena`, which holds what
 * is parsed, so that a location lives exactly as long as what it locates.
 * Returns NULL, the arena marked failed, when the arena refused.
 */
#define	RELIC_UNNAMED_SOURCE	"<input>"

const char *relic_source_name(relic_arena_t *arena, const char *name);

/*
 * lib/pred.c
 *
 * relic_pred_intern() assigns an identifier to a (name, arity) pair, the
 * name given as bytes the table copies when the pair is new, or returns the
 * one it already has; `where` is what a refusal names.  The
 * marks of struct relic_pred are set by relic_pred_name_head(),
 * relic_pred_name_generated(), relic_pred_name_group(),
 * relic_pred_mark_derived(),
 * relic_pred_reference() and
 * relic_pred_make_view(), and read by relic_pred_is_derived() and
 * relic_pred_is_defined(); relic_pred_reference() alone can fail, having
 * reported that memory ran out, since it copies the reference's source
 * name.  relic_pred_count_program_rule() counts one more
 * of the program's rules deriving the predicate.
 *
 * relic_pred_describe() writes "name/arity" into the caller's buffer, which
 * must hold RELIC_PRED_TEXT_MAX bytes, and returns it so that the call can
 * stand as an argument of the message it decorates; a predicate the Magic
 * rewrite generated is described as the program's predicate it is part of,
 * or as RELIC_QUERY_DESCRIPTION when it is part of the query, and the group
 * predicate of an aggregate as an aggregate of its rule's predicate or of
 * the query.
 * relic_pred_identify() writes the predicate itself, generated or not, for
 * an internal error.
 *
 * relic_pred_table_bytes() is what the table occupies -- its entries, its
 * index, the array and the index of the source names, and the name store
 * -- which the memory account counts as `predtab_bytes`.
 */
#define	RELIC_QUERY_DESCRIPTION	"the query"

void	 relic_predtab_init(relic_predtab_t *table, uint64_t hash_seed);
void	 relic_predtab_reset(relic_predtab_t *table);
int	 relic_pred_intern(relic_t *ctx, const char *name, size_t length,
	    uint16_t arity, const relic_location_t *where,
	    relic_pred_id_t *idp);
const relic_pred_t *relic_pred_of(const relic_t *ctx, relic_pred_id_t id);
size_t	 relic_pred_count(const relic_t *ctx);
size_t	 relic_pred_table_bytes(const relic_t *ctx);
void	 relic_pred_name_head(relic_t *ctx, relic_pred_id_t id);
void	 relic_pred_name_generated(relic_t *ctx, relic_pred_id_t id,
	    relic_pred_id_t stands_for);
void	 relic_pred_name_group(relic_t *ctx, relic_pred_id_t id,
	    relic_pred_id_t holder);
void	 relic_pred_mark_derived(relic_t *ctx, relic_pred_id_t id);
int	 relic_pred_reference(relic_t *ctx, relic_pred_id_t id,
	    const relic_location_t *where);
void	 relic_pred_make_view(relic_t *ctx, relic_pred_id_t view,
	    relic_pred_id_t viewed);
void	 relic_pred_count_program_rule(relic_t *ctx, relic_pred_id_t id);
bool	 relic_pred_is_derived(const relic_t *ctx, relic_pred_id_t id);
bool	 relic_pred_is_defined(const relic_t *ctx, relic_pred_id_t id);
const char *relic_pred_describe(const relic_t *ctx, relic_pred_id_t id,
	    char *buffer, size_t size);
const char *relic_pred_identify(const relic_t *ctx, relic_pred_id_t id,
	    char *buffer, size_t size);
void	 relic_pred_report_undefined(relic_t *ctx);

/*
 * lib/expr.c
 *
 * relic_expr_slots() and relic_comparison_slots() give the slots an
 * expression, or both sides of a comparison, read.
 * relic_comparison_nodes() counts the nodes of a comparison's two trees.
 * relic_literal_size() is a literal's size: one, and one more per node of
 * its expressions, so that a count of sizes sees what copying and walking
 * a literal costs where a count of literals would not; relic_body_size()
 * sums the sizes of a body's literals, saturating at SIZE_MAX.
 * relic_comparison_assigns() says whether a comparison placed where the
 * slots in `bound` are bound binds a variable rather than testing values,
 * and if so which slot and to what.  relic_expr_equal() says whether two
 * trees are the same.
 */
uint64_t relic_expr_slots(const relic_expr_t *expr);
uint64_t relic_comparison_slots(const relic_comparison_t *comparison);
size_t	 relic_comparison_nodes(const relic_comparison_t *comparison);
size_t	 relic_literal_size(const relic_literal_t *literal);
size_t	 relic_body_size(const relic_literal_t *body, size_t length);
bool	 relic_comparison_assigns(const relic_comparison_t *comparison,
	    uint64_t bound, uint16_t *slotp, const relic_expr_t **valuep);
bool	 relic_expr_equal(const relic_expr_t *a, const relic_expr_t *b);

/*
 * lib/literal.c
 *
 * Questions about a compiled literal that the checker, the stratifier, the
 * planner and the Magic rewrite each ask, answered once.
 *
 * relic_literal_reads() says whether a literal reads a relation and which:
 * the atom's predicate for a positive or a negated literal, the group
 * predicate for an aggregate, and none for a comparison.  How it reads it
 * is the literal's kind.  relic_aggregate_shared_slots() gives the slots an
 * aggregate's key is built from, and relic_aggregate_binds() whether an
 * aggregate placed where the slots in `bound` are bound binds its result
 * rather than comparing with it: its key must be bound, and its result not.
 * relic_aggregate_spelling() gives how an aggregate function is written.
 */
bool	 relic_literal_reads(const relic_literal_t *literal,
	    relic_pred_id_t *predp);
const char *relic_aggregate_spelling(enum relic_aggregate_fn fn);
uint64_t relic_aggregate_shared_slots(const relic_aggregate_t *aggregate);
bool	 relic_aggregate_binds(const relic_aggregate_t *aggregate,
	    uint64_t bound);

/*
 * lib/rule.c
 *
 * relic_program_init() prepares the empty program a context starts with,
 * whose predicate table hashes with `hash_seed`, and relic_program_reset()
 * releases what the program owns outside the program arena, the schedule's
 * arena among it.
 * The public entry point into this module is relic_check().
 *
 * relic_program_ready() is the check relic_check() makes and every use of
 * the program begins with: preparing a query, handing out one of the
 * program's own, or evaluating.  Returns 0, or -1 with at least one error
 * reported by this call.
 *
 * relic_ruletab_place() is the one way a rule joins a rule table, the
 * program's or a rewrite's, and marks its head predicate derived.  It
 * cannot fail: relic_ruletab_reserve() first makes room for `count` more
 * rules, returning -1, reported, when memory ran out, and the table is then
 * as it was.  relic_ruletab_append() is the two for one rule.
 */
void	 relic_program_init(relic_program_t *program, uint64_t hash_seed);
int	 relic_program_ready(relic_t *ctx);
void	 relic_program_reset(relic_program_t *program);
int	 relic_ruletab_reserve(relic_t *ctx, relic_ruletab_t *table,
	    size_t count);
void	 relic_ruletab_place(relic_t *ctx, relic_ruletab_t *table,
	    const relic_rule_t *rule);
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
 * The slots a body literal binds when the planner places it where the slots
 * in `bound` are bound: every variable of a positive literal, the variable
 * of an equality that assigns it there, the result of an aggregate that
 * binds it there, and nothing for any other test.
 * The planner decides what each literal binds through this, and so does the
 * Magic rewrite, walking a body in the order relic_plan_order() gave, so the
 * two cannot disagree about what is bound where.
 */
uint64_t relic_plan_literal_binds(const relic_t *ctx,
	    const relic_literal_t *literal, uint64_t bound);

/*
 * Whether a plan, run once over relations that do not change while it
 * runs, derives each of its head tuples once: so that inserting what it
 * derives would reject nothing, and its derivations are a set as they come.
 */
bool	 relic_plan_derives_distinct(const relic_plan_t *plan);

/*
 * Whether a plan computes a value: whether some FILTER or ASSIGN runs a
 * program holding an operator or a function, or some AGGREGATE counts or
 * sums.  Such a plan can meet an undefined result on any binding, and can
 * derive a value the context has not assigned.
 */
bool	 relic_plan_computes(const relic_plan_t *plan);

/*
 * Whether a plan calls a function: whether some FILTER or ASSIGN runs a
 * program holding a call.  Such a plan needs the scratch's text stack, and
 * can derive a string the context has not assigned.
 */
bool	 relic_plan_calls(const relic_plan_t *plan);

/*
 * Whether a plan holds an AGGREGATE.
 */
bool	 relic_plan_folds(const relic_plan_t *plan);

/*
 * The atom of an operation over a relation's tuple -- a SCAN, a LOOKUP, a
 * MEMBER, an ANTI_MEMBER or the EMIT -- or NULL for a FILTER, an ASSIGN or
 * an AGGREGATE, whose union holds no atom.  A question about the relation an
 * operation reads or writes asks this rather than the union, so that an
 * operation kind over no relation is answered by construction.
 */
const relic_op_atom_t *relic_op_atom(const relic_op_t *op);

/*
 * lib/magic.c
 *
 * A query's Magic rewrite.  relic_magic_find_complete() decides which
 * predicates the query reads completely, from the program's schedule, before
 * anything is evaluated: among them, whatever the bodies of the query's group
 * rules read.  Once the caller has evaluated those,
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
	    const relic_query_t *query);
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
 *
 * relic_report_mixed_note() reports the mixed-type comparison a note
 * records for a plan of `rule`, unless the rule has had one reported, and
 * clears the note.
 */
int	 relic_query_answer(relic_t *ctx, relic_query_t *query,
	    relic_row_cb callback, void *callback_ctx);
bool	 relic_relations_current(const relic_t *ctx);
void	 relic_discard_derived(relic_t *ctx);
void	 relic_report_mixed_note(relic_t *ctx, const relic_rule_t *rule,
	    relic_mixed_note_t *note);

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
 * position plus one, keyed by its predicate.  `base_tuples`, `new_tuples`
 * and `columns_floor` are what staging measures a new fact against the
 * limits with, as lib/facts.c describes.
 */
struct relic_staging {
	relic_t			*ctx;
	relic_staged_set_t	*sets;
	size_t			 set_count;
	size_t			 set_capacity;
	size_t			 last;
	relic_idtab_t		 by_pred;
	size_t			 base_tuples;	/* held as the load began */
	size_t			 new_tuples;	/* staged, not yet held */
	size_t			 columns_floor;	/* bytes; see facts.c */
};

void	 relic_staging_init(relic_staging_t *staging, relic_t *ctx);
void	 relic_staging_reset(relic_staging_t *staging);
int	 relic_staging_add(relic_staging_t *staging, relic_pred_id_t pred,
	    const relic_dom_t *tuple);
int	 relic_staging_commit(relic_staging_t *staging);

/*
 * lib/import.c
 *
 * RELIC_IMPORT_READ_CHUNK is how much of the stream an import reads per
 * call.  Only the number of read calls depends on it, since the reader holds
 * one field, not the chunk, and it bounds how far an import may have read
 * past the record that ends it.
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

#define	RELIC_IMPORT_READ_CHUNK	(64u * 1024u)

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
 * query's heading, a query's plan, its EMIT shown as `stream` when
 * `streams` says it delivers the query's rows as it derives them, the
 * heading of a query's Magic rewrite, and the line that marks a rewrite
 * abandoned over its budget, after which the whole program is evaluated
 * and shown.  The rules shown between a query's heading and its plan are
 * its group rules.  A query's rule names no predicate, so the functions
 * that show a query are given its answer relation, whose name its head is
 * written with.  `depth` is the level a
 * component's heading is indented to, one more for a component of a
 * rewritten program than for one of the program itself; what belongs to
 * the component is indented beneath it.  None can fail.
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
	    const relic_relation_t *answer);
void	 relic_explain_query_plan(relic_t *ctx, const relic_rule_t *rule,
	    const relic_plan_t *plan, bool streams);
void	 relic_explain_rewrite(relic_t *ctx, const relic_rule_t *query,
	    const relic_relation_t *answer, const relic_magic_t *magic);
void	 relic_explain_abandoned_rewrite(relic_t *ctx,
	    const relic_rule_t *query, const relic_relation_t *answer);

/*
 * lib/index.c
 *
 * A secondary index over the tuples of one relation, keyed on the columns of
 * a mask.  A tuple is added in four steps, of which only the third can fail:
 * relic_index_classify() hashes the key of the tuple about to become tuple
 * `id` and finds it, keeping the hash, the key found and `id` as the index's
 * pending insertion; relic_index_pending_bytes() and
 * relic_index_pending_bytes_after() are the bytes of the parts the insertion
 * can grow, before and after, which the memory limit is asked with;
 * relic_index_reserve_pending() makes room for the tuple's link and for its
 * key when it is new; and relic_index_add_pending() links the tuple in once
 * the relation holds it.  Tuples must be added in increasing identifier
 * order.  The walks over an index are in frozen.h.  relic_index_raise_since()
 * raises the start the index's frontiers stand for to `since`, before any
 * lookup from it; the starts raised to should never decrease over the life
 * of the index, as a semi-naive delta's never do, or a lookup from an
 * earlier one walks the whole chain.  relic_index_free() releases an index,
 * which relic_index_init() must set up again before it is used.
 *
 * An append of many new tuples at once, as a round's merge makes one, goes
 * through the index in steps of its own, several of which run a stripe to
 * a thread: relic_index_stage() records each tuple's key hash and stripe;
 * relic_index_classify_append() finds, for the tuples of one stripe, the
 * key each joins, numbering the keys new to the stripe; the projection and
 * the reservation, relic_index_bytes_after_append() and
 * relic_index_reserve_append(), are exact, as one insertion at a time would
 * grow the index, and relic_index_bytes_replaced_by_append() is what the
 * reservation holds beside that until the tables it replaces are freed;
 * and relic_index_link_append() fills one stripe's grown table, freeing the
 * one it replaces, and links its tuples.
 *
 * The relation store builds and extends indexes and does all the
 * accounting; the evaluator raises a member's indexes to its delta's start
 * whenever it sets the delta, and otherwise only walks them, moving a key's
 * frontier as a lookup of a delta walks toward the delta's start.
 */
void	 relic_index_init(relic_index_t *index, uint32_t key_mask);
void	 relic_index_free(relic_index_t *index);
size_t	 relic_index_bytes(const relic_index_t *index);
void	 relic_index_classify(relic_index_t *index,
	    const relic_relation_t *relation, const relic_dom_t *tuple,
	    relic_tuple_id_t id);
size_t	 relic_index_pending_bytes(const relic_index_t *index);
size_t	 relic_index_pending_bytes_after(const relic_index_t *index);
int	 relic_index_reserve_pending(relic_index_t *index,
	    const relic_relation_t *relation);
void	 relic_index_add_pending(relic_index_t *index,
	    const relic_relation_t *relation);
void	 relic_index_raise_since(relic_index_t *index, relic_tuple_id_t since);
void	 relic_index_stage(const relic_index_t *index,
	    const relic_relation_t *relation, relic_append_keys_t *keys,
	    size_t position, const relic_dom_t *tuple);
void	 relic_index_classify_append(const relic_index_t *index,
	    const relic_relation_t *relation, const relic_append_t *append,
	    relic_append_keys_t *keys, size_t s, relic_new_keys_t *new_keys);
size_t	 relic_index_bytes_after_append(const relic_index_t *index,
	    size_t tuples, const relic_append_t *append,
	    const relic_append_keys_t *keys);
size_t	 relic_index_bytes_replaced_by_append(const relic_index_t *index,
	    const relic_append_keys_t *keys);
int	 relic_index_reserve_append(relic_index_t *index, size_t tuples,
	    const relic_append_t *append, relic_append_keys_t *keys);
void	 relic_index_link_append(relic_index_t *index,
	    const relic_relation_t *relation, const relic_append_t *append,
	    relic_append_keys_t *keys, size_t s);

/*
 * lib/relation.c
 *
 * A predicate's relations are obtained through two functions, one per
 * role.  relic_relation_written() gives the derived relation a rule writes,
 * and refuses a predicate no rule derives.  relic_relation_read() gives the
 * relation an operation reads: the derived relation of a derived predicate,
 * and otherwise the base store of the predicate read, or of the one a base
 * view views.  Either is created empty the first time it is asked for, so
 * no caller decides whether a relation exists yet, and never moves, so a
 * caller may hold as many of them as it likes for as long as the relation
 * lives: a base store as long as the context, a derived relation until the
 * next discard.
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
 * relic_base_append_hashed() then appends one fact the store lacks without
 * looking anything up.  relic_bases_refuse() reports a limit refusing a
 * load, naming the first predicate whose store the load would grow, as the
 * reservation and staging both do.
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
 * RELIC_INSERT_BATCH tuples at a time.  relic_relation_insert_batch_hashed()
 * does the same with each tuple's hash given, as relic_relation_screen()
 * gave it.
 *
 * relic_relation_admit_values() asks the memory limit whether the value
 * table and the pool may grow from what they hold now to `projected_bytes`,
 * which the values a batch derived into `relation` holds pending need, and
 * returns 0, or -1 having reported the refusal as an insertion into
 * `relation` reports one.
 *
 * relic_relation_reserve() makes room in a relation without secondary
 * indexes, as a base store is when a load commits, for a growth: tuples by
 * the stripe of the canonical index each falls in, for the stripes' slots,
 * and in all, relic_tuple_growth_total(), for the columns.  It grows each
 * array at most once and keeps the storage total in step, and refuses a
 * relation with secondary indexes as an internal error.  An insertion makes
 * its own room, for one tuple and the keys it brings.
 *
 * A reservation applies no configured limit.
 * relic_relation_bytes_after_growth() gives the bytes the relation would
 * then hold, and a caller bound by the memory limit projects that against
 * the account first, as an insertion does.  A reservation past the
 * identifier space is refused as a relation too large to grow; capacity a
 * failed reservation obtained is kept, and accounted.
 *
 * An append gives a relation many tuples it does not hold at once, in the
 * identifier order they take, as a round's merge gathers them (relation.h).
 * relic_relation_append_open() gives it its arrays, charged to the round,
 * and relic_relation_append_close() releases everything it holds, applied
 * or not.  relic_relation_stage() records a tuple of it, and
 * relic_relation_classify_append() classifies its keys in each of the
 * relic_relation_classify_parts() parts, a key stripe each, with room for
 * relic_relation_classify_tuples() new keys.
 * relic_relation_bytes_after_append() is exactly what the relation would
 * then hold, relic_relation_bytes_during_append() what it holds while the
 * append is applied, and relic_relation_reserve_append() makes that room,
 * reporting nothing when it cannot.  relic_relation_take_append() stores
 * the columns and counts the tuples; relic_relation_apply_append() places
 * or links the tuples of each of the relic_relation_apply_parts() parts, a
 * stripe each; and relic_relation_finish_append() brings the storage total
 * up to date.  Each part is meant for a thread of its own.
 * relic_tuples_admissible() is how many more tuples the tuple limit admits,
 * SIZE_MAX when none is set.
 *
 * relic_relation_contains() says whether a relation holds a tuple of some
 * columns.  The reads the executor makes -- the tuple an identifier names,
 * the identifier of the tuple holding some columns, the range of every
 * tuple, a relation's predicate and its indexes -- are in frozen.h.
 *
 * Every relation of a context hashes a tuple with relic_tuple_hash() under
 * the context's seed, so a caller that has already hashed one that way, as
 * a load's staging does, passes the hash to the _hashed forms rather than
 * have it computed again.
 *
 * relic_relation_since() gives the identifier range a semi-naive delta
 * walks, and relic_relation_end() is the mark a later delta is measured
 * from.
 *
 * relic_relation_discard_all() frees every derived relation and drops the
 * base stores' secondary indexes, which ends the span over which a derived
 * relation, a range or an index position means anything; no caller may hold
 * one across it, nor a range of a relation across that relation's clear.
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
 * buffer of RELIC_PRED_TEXT_MAX bytes, a generated predicate's relation as
 * the program's predicate it stands for; relic_relation_identify() writes
 * the relation itself, for an internal error.  They are the only way a
 * relation is named, in explain output and in diagnostics alike.
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
int	 relic_bases_refuse(relic_t *ctx, const char *key, size_t limit,
	    relic_pred_id_t pred);
int	 relic_bases_reserve(relic_t *ctx, const relic_base_growth_t *growths,
	    size_t count);
void	 relic_base_append_hashed(relic_t *ctx, relic_pred_id_t pred,
	    const relic_dom_t *columns, uint64_t hash);
relic_relation_t *relic_relation_create(relic_t *ctx, relic_pred_id_t pred,
	    size_t serial, uint16_t arity);
void	 relic_relation_clear(relic_t *ctx, relic_relation_t *relation);
void	 relic_relation_free(relic_relation_t *relation);
int	 relic_relation_insert(relic_t *ctx, relic_relation_t *relation,
	    const relic_dom_t *columns, bool *addedp);
int	 relic_relation_insert_batch(relic_t *ctx, relic_relation_t *relation,
	    const relic_dom_t *tuples, size_t count, bool *added,
	    size_t *completedp);
int	 relic_relation_admit_values(relic_t *ctx,
	    const relic_relation_t *relation, size_t projected_bytes);
int	 relic_relation_insert_batch_hashed(relic_t *ctx,
	    relic_relation_t *relation, const relic_dom_t *tuples,
	    const uint64_t *hashes, size_t count, bool *added,
	    size_t *completedp);
bool	 relic_relation_contains(const relic_relation_t *relation,
	    const relic_dom_t *columns);
bool	 relic_relation_contains_hashed(const relic_relation_t *relation,
	    const relic_dom_t *columns, uint64_t hash);
uint64_t relic_tuple_hash(uint64_t seed, const relic_dom_t *columns,
	    uint16_t arity);
size_t	 relic_relation_bytes(const relic_relation_t *relation);
size_t	 relic_relation_bytes_after_growth(const relic_relation_t *relation,
	    const relic_tuple_growth_t *growth);
size_t	 relic_tuple_growth_total(const relic_tuple_growth_t *growth);
int	 relic_relation_append_open(relic_t *ctx,
	    const relic_relation_t *relation, relic_append_t *append);
void	 relic_relation_append_close(relic_t *ctx, relic_append_t *append);
void	 relic_relation_stage(const relic_relation_t *relation,
	    relic_append_t *append, size_t position, const relic_dom_t *columns,
	    uint64_t hash);
size_t	 relic_relation_classify_parts(const relic_append_t *append);
size_t	 relic_relation_classify_tuples(const relic_append_t *append,
	    size_t part);
void	 relic_relation_classify_append(const relic_relation_t *relation,
	    relic_append_t *append, size_t part, relic_new_keys_t *new_keys);
size_t	 relic_relation_bytes_after_append(const relic_relation_t *relation,
	    const relic_append_t *append);
size_t	 relic_relation_bytes_during_append(const relic_relation_t *relation,
	    const relic_append_t *append);
int	 relic_relation_reserve_append(relic_t *ctx, relic_relation_t *relation,
	    relic_append_t *append);
void	 relic_relation_take_append(relic_t *ctx, relic_relation_t *relation,
	    const relic_append_t *append);
size_t	 relic_relation_apply_parts(const relic_append_t *append);
void	 relic_relation_apply_append(relic_relation_t *relation,
	    relic_append_t *append, size_t part);
void	 relic_relation_finish_append(relic_t *ctx,
	    const relic_relation_t *relation, const relic_append_t *append);
size_t	 relic_tuples_admissible(const relic_t *ctx);
int	 relic_relation_reserve(relic_t *ctx, relic_relation_t *relation,
	    const relic_tuple_growth_t *growth);
size_t	 relic_relation_count(const relic_relation_t *relation);
relic_tuple_id_t relic_relation_end(const relic_relation_t *relation);
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
const char *relic_relation_identify(const relic_t *ctx,
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
