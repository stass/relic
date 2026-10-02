/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_PROGRAM_H
#define RELIC_PROGRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <relic.h>

#include "arena.h"
#include "ast.h"
#include "regex.h"
#include "util.h"

/*
 * The program model: what the syntax tree becomes once its predicates are
 * resolved, its variables are numbered and its rules are known to be safe.
 *
 * These types exist so that no later stage re-resolves anything.  A planner
 * reading a compiled rule needs no symbol table and no variable names, and
 * an executor needs no case for a name that has not been numbered yet.  The
 * syntax tree is left untouched beside them: it is the record of what was
 * written, and diagnostics read it for spellings and locations.
 *
 * Everything variable-length here is allocated from the program arena and
 * lives as long as the statement it came from.  Nothing is freed
 * individually.
 */

/*
 * At most 64 distinct variables in a rule.
 *
 * The limit is structural rather than configurable: it is what lets a
 * binding frame be a plain array and the set of bound slots a single
 * uint64_t, with no allocation anywhere in the join loop.
 */
#define	RELIC_MAX_VARIABLES	64

/*
 * Buffer sizes for the names a message quotes.
 *
 * A predicate or variable name is an identifier of any length, and a message
 * names one thing, so a name is shortened to at most forty bytes plus the
 * "..." that says it was.  A predicate then adds the separator and an arity
 * of at most five digits.
 */
#define	RELIC_NAME_TEXT_MAX	44
#define	RELIC_PRED_TEXT_MAX	50

typedef struct relic_aggregate relic_aggregate_t;
typedef struct relic_arg relic_arg_t;
typedef struct relic_atom relic_atom_t;
typedef struct relic_comparison relic_comparison_t;
typedef struct relic_component relic_component_t;
typedef struct relic_expr relic_expr_t;
typedef struct relic_expr_call relic_expr_call_t;
typedef struct relic_literal relic_literal_t;
typedef struct relic_loaded_query relic_loaded_query_t;
typedef struct relic_pred relic_pred_t;
typedef struct relic_predtab relic_predtab_t;
typedef struct relic_program relic_program_t;
typedef struct relic_querytab relic_querytab_t;
typedef struct relic_rule relic_rule_t;
typedef struct relic_ruletab relic_ruletab_t;
typedef struct relic_schedule relic_schedule_t;
typedef struct relic_variable relic_variable_t;

/*
 * What a predicate is.  A written one, the program's or the base view of
 * one, is described by its own name.  One the Magic rewrite generates is
 * part of the predicate it adorns, for an adorned, a magic or a
 * supplementary predicate, or of the query, and is described as that
 * predicate or as the query.  The group predicate of an aggregate is part of the predicate
 * whose rule holds the aggregate, or of the query, but holds the rows of
 * the aggregate's body rather than tuples of either, and is described as an
 * aggregate of the one or the other.
 */
enum relic_pred_kind {
	RELIC_PRED_WRITTEN,
	RELIC_PRED_REWRITTEN,
	RELIC_PRED_GROUP
};

/*
 * A predicate, identified by its position in the predicate table.
 *
 * The typedef says, by convention rather than by anything the compiler
 * enforces, that this is not a symbol identifier and not a domain
 * identifier, all three of which are 32-bit and none of which may be used
 * where another is meant.  RELIC_ID_NONE names no predicate.
 */
typedef uint32_t relic_pred_id_t;

/*
 * A predicate: (name, arity) and nothing else.
 *
 * foo/1 and foo/2 are unrelated predicates, so there is no such thing as an
 * arity mismatch in this language -- two arities are two predicates rather
 * than one predicate used inconsistently.
 *
 * Three marks record what derives into it, each set in one place and never
 * cleared.  `head_named` is set when a rule's head names it, as soon as the
 * head is resolved and before the rule is known to be safe, so that one
 * unsafe rule draws one complaint rather than also a warning that nothing
 * defines its predicate; and when the Magic rewrite generates it.
 * `derived` is set when a rule with it as head is appended to a rule table,
 * and decides the relation it is read and written through.  `views` names
 * the predicate whose base store this one reads, for a base view, and is
 * RELIC_ID_NONE otherwise.  Whether a predicate is defined is a function of
 * these and of its base store, relic_pred_is_defined().
 *
 * `referenced` is set when an atom of a rule body or a query names it,
 * whether or not that statement is accepted, and `referenced_at` is where
 * that first atom is: where the undefined-predicate warning points.  A
 * predicate interned only for facts a failed load never committed, or for a
 * rewrite that was abandoned, is referenced by nothing and never named.
 *
 * `program_rules` counts the program's rules deriving the predicate, which
 * is the position the next one takes among them: what the group predicate
 * of an aggregate in that rule is named after.  Rules are only appended, so
 * a position once taken never shifts.
 *
 * `kind` says whether a diagnostic names the predicate by its own name, as
 * it does one the program writes and the base view of one, or which part
 * of the library generated it, and `stands_for` what a generated predicate
 * is part of: a predicate the program writes, or the query when it is
 * RELIC_ID_NONE, which it always is for a predicate named by its own name.
 * A diagnostic names a generated predicate by what it is part of, since
 * that is what the reader wrote and can change.
 */
struct relic_pred {
	relic_location_t	referenced_at;
	size_t			program_rules;
	uint32_t		name;	/* interned spelling */
	relic_pred_id_t		views;
	relic_pred_id_t		stands_for;
	enum relic_pred_kind	kind;
	uint16_t		arity;
	bool			head_named;
	bool			derived;
	bool			referenced;
	bool			reported_undefined;
};

/*
 * Predicates interned to identifiers, found at index `id - 1`.
 *
 * The table is context-lifetime state rather than something a check derives
 * and discards: identifiers are stable indexes that every later pass uses,
 * and the relation holding a predicate's tuples hangs off its entry and
 * outlives any one query.
 */
struct relic_predtab {
	relic_pred_t	*entries;	/* indexed by id - 1 */
	size_t		 count;
	size_t		 capacity;
	relic_idtab_t	 index;
	uint64_t	 hash_seed;	/* the context's */
};

enum relic_arg_kind {
	RELIC_ARG_CONSTANT,	/* an interned value */
	RELIC_ARG_VARIABLE,	/* a slot in the rule's binding frame */
	RELIC_ARG_WILDCARD	/* `_`: matches anything and binds nothing */
};

/*
 * One argument of a compiled atom.
 *
 * A wildcard appears only in an argument of a positive relational body
 * literal, because that is the only position where `_` is legal; elsewhere
 * nothing could ever bind it and the rule is rejected before it is built.
 */
struct relic_arg {
	union {
		relic_dom_t	constant;
		uint16_t	slot;
	}			u;
	enum relic_arg_kind	kind;	/* which arm of u is live */
};

/*
 * A predicate applied to arguments, resolved.
 *
 * No arity is stored: the predicate has one, and a second copy of a fact is
 * a second thing to keep in step.  `where` points at the predicate name,
 * which is what a diagnostic about the atom should underline.
 */
struct relic_atom {
	relic_arg_t		*args;	/* the predicate's arity of them */
	relic_location_t	 where;
	relic_pred_id_t		 pred;
};

enum relic_expr_kind {
	RELIC_EXPR_CONSTANT,		/* an interned value */
	RELIC_EXPR_VARIABLE,		/* a slot */
	RELIC_EXPR_NEGATE,		/* -operand */
	RELIC_EXPR_BINARY,		/* left op right */
	RELIC_EXPR_CALL			/* fn(args) */
};

/*
 * A call of a function on `argc` arguments, the function's arity, each an
 * expression.
 *
 * `pattern` is a match's pattern compiled with the rule, when the pattern
 * is a string constant, and NULL for every other call.  It lives in the
 * arena the rule does, and a copy of the node shares it, since nothing
 * writes a compiled pattern.
 */
struct relic_expr_call {
	relic_expr_t		*args[RELIC_MAX_FUNCTION_ARITY];
	const relic_pattern_t	*pattern;
	enum relic_function	 fn;
	uint8_t			 argc;
};

/*
 * An expression, resolved: the parser's tree with its variables numbered and
 * its calls resolved to functions.
 *
 * The tree is kept rather than flattened here, because three consumers want
 * its shape: the explain output prints a rule with the parentheses the tree
 * requires, the Magic rewrite renumbers and compares rules by walking their
 * literals, and the planner reads the slots an expression mentions.  The
 * form the executor runs is built from it with each plan.
 *
 * `height` is one for a leaf and one more than the tallest operand or
 * argument otherwise, never more than RELIC_MAX_EXPRESSION_DEPTH, so a walk
 * over a tree is bounded without a check of its own.  `nodes` counts this
 * node and every node below it, so that a cost charged by size needs no
 * walk.
 */
struct relic_expr {
	union {
		relic_dom_t		 constant;	/* CONSTANT */
		uint16_t		 slot;		/* VARIABLE */
		relic_expr_t		*operand;	/* NEGATE */
		struct {
			relic_expr_t		*left;
			relic_expr_t		*right;
			enum relic_arith_op	 op;
		}			 binary;	/* BINARY */
		relic_expr_call_t	 call;		/* CALL */
	}				 u;
	size_t				 nodes;
	enum relic_expr_kind		 kind;	/* which arm of u is live */
	uint8_t				 height;
};

/*
 * A comparison of two expressions.  A term is an expression of one node, so
 * a comparison of two terms holds two one-node trees.
 */
struct relic_comparison {
	relic_expr_t		*left;
	relic_expr_t		*right;
	enum relic_compare_op	 op;
};

/*
 * An aggregate literal, `result = fn(column) { ... }`, resolved.
 *
 * Its body is compiled into the group rule, an ordinary rule whose head is
 * the group predicate: the variables the body shares with the rest of the
 * rule, in the order of their slots in it, then the variables that are the
 * body's own, in order of first occurrence.  The group predicate's relation
 * then holds, under every binding of the shared columns, exactly the rows
 * the body denotes, distinct because a relation is a set, and the literal
 * folds the tuples of that relation whose first `shared_count` columns hold
 * the shared slots' values.  Nothing else of the body is needed to evaluate
 * the literal: the group relation is complete before any rule reading it
 * runs, as a negated relation is.
 *
 * `shared` holds the slot of this rule that each shared column is bound
 * from, in column order, and is NULL when there are none.  `column` is the
 * aggregated column of the group relation, unused by COUNT, and `result`
 * the slot the literal binds, or compares with when it is bound already.
 * `where` is the function name.
 *
 * `group_rule` is the group rule as it was compiled, which explain prints
 * the body from and the Magic rewrite reads what the body reads from.  It
 * is a copy of its own in the arena that holds the literal, fixed from the
 * moment the literal is, rather than a position in a rule table: the
 * program's table grows and moves, and a query's group rules are held by
 * the query, so the literal reaches its body the same way whoever holds it,
 * a copy of it the rewrite made included.  Its slot c is column c.
 */
struct relic_aggregate {
	const uint16_t		*shared;	/* shared_count of them */
	const relic_rule_t	*group_rule;
	relic_location_t	 where;
	relic_pred_id_t		 group;
	uint16_t		 shared_count;
	uint16_t		 column;	/* not COUNT */
	uint16_t		 result;
	enum relic_aggregate_fn	 fn;
};

/*
 * One conjunct of a compiled body.
 *
 * The four cases are the parser's own classification, reused rather than
 * restated: a second enumeration of positive, negated, comparison and
 * aggregate would be a second representation of one fact, and two such
 * eventually disagree.
 */
struct relic_literal {
	union {
		relic_atom_t		 atom;
		relic_comparison_t	 comparison;
		relic_aggregate_t	 aggregate;
	}				 u;
	relic_location_t		 where;
	enum relic_ast_literal_kind	 kind;	/* which arm of u is live */
};

/*
 * One distinct variable of a rule, in the slot its first occurrence was
 * assigned.
 *
 * `name` is the interned spelling, which diagnostics quote and which later
 * explain output names.  `where` is the first occurrence, which is where
 * both the safety error and the singleton warning point.  `occurrences` is
 * what the singleton check reads.
 *
 * `anonymous` marks a slot taken by a bare `_`.  Such a slot exists only in
 * a rule that is being rejected: `_` in a positive body literal compiles to
 * a wildcard and takes no slot, and `_` anywhere else can never be bound.
 * It is given a slot so that the one safety computation reports it in the
 * same order as the named variables around it.
 */
struct relic_variable {
	relic_location_t	where;
	uint32_t		name;
	uint32_t		occurrences;
	bool			anonymous;
};

/*
 * A compiled rule.
 *
 * A load stores a program's facts in base stores rather than compiling
 * them, so a rule whose body has no literals is a seed of the Magic rewrite,
 * or a statement with a variable in its head, which the check rejects.  A
 * query is a rule too, whose head names no predicate: `head` is
 * RELIC_ID_NONE and the head's arguments are the query's named variables in
 * order of first occurrence.  Its tuples go to the answer relation the query
 * object owns, which is why the predicate table holds nothing for it.
 *
 * `head_arity` is the number of head arguments, set wherever a rule is
 * built: the head predicate's arity, or a query's number of output columns.
 * Every stage that needs a head's arity reads it here rather than from the
 * predicate table, which a query's head is not in.  `head_args` holds that
 * many arguments and is NULL when there are none.
 *
 * Every slot below `variable_count` is bound by some positive relational
 * body literal or by an assignment whose other side those bind, because a
 * rule in which one was not would have been rejected.  The planner can
 * therefore always place every literal, and the evaluator has no
 * unbound-slot case to handle.
 *
 * `origin` is the rule as the program wrote it: its position among the
 * program's rules, or RELIC_ORIGIN_QUERY for a query's rule.  A rule the
 * Magic rewrite generates takes the origin of the rule it was generated
 * from, and a group rule the origin of the rule its aggregate was written
 * in, so every copy of a rule, and every rule a statement compiled to,
 * leads back to the one the reader wrote.
 * Every rule is compiled with RELIC_ORIGIN_QUERY, which is safe for any
 * rule, and a rule receives its position only as it joins the program's
 * rules, so no rule can hold a position that is not a program rule's.
 * `mixed_noted` is meaningful only on a program rule, and records that
 * RELIC_F_NOTE_MIXED has reported a mixed-type comparison of it or of a
 * copy of it.
 */
struct relic_rule {
	relic_arg_t		*head_args;
	relic_literal_t		*body;
	relic_variable_t	*variables;
	size_t			 body_length;
	size_t			 origin;
	relic_location_t	 where;
	relic_pred_id_t		 head;		/* RELIC_ID_NONE for a query */
	uint16_t		 head_arity;
	uint16_t		 variable_count;
	bool			 mixed_noted;
};

/* The origin of a query's rule, and of every rule generated from it. */
#define	RELIC_ORIGIN_QUERY	SIZE_MAX

/*
 * Compiled rules, in the order their statements were loaded.
 *
 * A growable array, released by relic_free_array(), rather than an arena
 * block, because an arena cannot resize and a rule must be reachable by
 * index once the dependency graph exists.  The variable-length parts of each
 * rule are arena allocated and are not moved by a growth.
 */
struct relic_ruletab {
	relic_rule_t	*entries;
	size_t		 count;
	size_t		 capacity;
};

/*
 * A query written in the program, compiled, with the serial number that
 * names its answer relation.  The program's queries and the queries prepared
 * from text share one sequence of serial numbers in the order they were
 * compiled, and loads and preparations interleave, so a query's serial is
 * not its position among the program's queries and is recorded beside it.
 *
 * `group_rules` are the group rules of the query's aggregates, in body
 * order, `group_rule_count` of them in the program arena.  They belong to
 * the query rather than to the program's rules, so that preparing or
 * running a query never changes the program.
 */
struct relic_loaded_query {
	relic_rule_t		 rule;
	const relic_rule_t	*group_rules;
	size_t			 group_rule_count;
	size_t			 serial;
};

/*
 * The program's queries, in the order they were loaded.  A growable array
 * for the reason relic_ruletab_t gives.
 */
struct relic_querytab {
	relic_loaded_query_t	*entries;
	size_t			 count;
	size_t			 capacity;
};

/*
 * One strongly connected component of the dependency graph, as the evaluator
 * runs it.
 *
 * Its members are `schedule->members[first_member]` onward, in identifier
 * order, and its rules are the program rules whose indexes are
 * `schedule->rule_indexes[first_rule]` onward, in program order.
 * `recursive` says whether some rule of the component reads a member, which
 * is what decides between a fixed point and a single pass.
 */
struct relic_component {
	size_t		first_member;
	size_t		member_count;
	size_t		first_rule;
	size_t		rule_count;
	uint32_t	stratum;
	bool		recursive;
};

/*
 * The order a stratified program is evaluated in: its components, sorted by
 * stratum, each after every component it depends on.
 *
 * Only components with at least one rule are listed, and every rule of the
 * program belongs to exactly one of them -- the one containing its head -- so
 * `rule_count` is both the length of `rule_indexes` and the number of program
 * rules the schedule describes.  The schedule is current exactly when that is
 * the program's rule count, because rules are only ever appended.
 *
 * Rules are held as indexes into the program's rule table rather than as
 * pointers, which a growth of the table would leave dangling, or as copies,
 * which would double the memory of a program of a million facts.
 *
 * Everything is allocated from `arena`, which is reset when a new schedule
 * replaces this one.
 */
struct relic_schedule {
	relic_arena_t		 arena;
	relic_component_t	*components;	/* evaluation order */
	relic_pred_id_t		*members;	/* grouped by component */
	size_t			*rule_indexes;	/* grouped by component */
	size_t			 component_count;
	size_t			 member_count;
	size_t			 rule_count;
};

/*
 * The program a context has validated.
 *
 * `compiled` is the last statement of the syntax tree a check has compiled,
 * and NULL before the first one.  The tree is append-only -- a load splices
 * its statements onto the end and nothing ever rewrites one -- so everything
 * up to and including it can never change, and a check compiles from the one
 * after it to the end.  That single cursor is what makes checking cached,
 * incremental, and silent when it has nothing new to say.  It is a pointer
 * rather than a count so that resuming costs nothing proportional to what is
 * already done; statements live in the program arena and are never freed
 * while the context is, so it cannot dangle.
 *
 * `invalid` is sticky because an unsafe rule stays in the tree and stays
 * unsafe: a later load cannot repair it, so a check that compiles only
 * blameless statements must still refuse the program.
 *
 * `queries` holds the queries written in the program, compiled, apart from
 * `rules` because evaluating the program must not answer every query it
 * happens to contain.  `query_serial` counts the queries this context has
 * compiled, those written in the program and those prepared from text alike,
 * and is the serial number of the next one.  It advances only when a
 * compiled query is published, so a query that fails part way and is
 * compiled again reuses its number.
 *
 * `schedule` is the evaluation order the dependency analysis last produced.
 * `unstratified` records that the analysis found recursion through negation.
 * It is sticky for the same reason `invalid` is -- adding rules only adds
 * dependencies, so no later load can break the cycle -- and while it is set
 * the analysis does not run again, so the cycle is reported once.
 */
struct relic_program {
	relic_predtab_t		 predicates;
	relic_ruletab_t		 rules;
	relic_querytab_t	 queries;
	relic_schedule_t	 schedule;
	const relic_ast_stmt_t	*compiled;
	size_t			 query_serial;
	bool			 invalid;
	bool			 unstratified;
};

/*
 * Whether the schedule describes every rule the program has.  Rules are only
 * ever appended, so the count says it.
 */
static inline bool
relic_schedule_is_current(const relic_program_t *program)
{
	return (program->schedule.rule_count == program->rules.count);
}

#endif /* RELIC_PROGRAM_H */
