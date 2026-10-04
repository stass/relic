/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"

/*
 * The syntax tree becomes a program.
 *
 * Compiling a statement resolves its atoms against the predicate table,
 * numbers its variables into the slots of architecture section 25, and
 * decides whether the result is safe.  What comes out is a relic_rule_t that
 * mentions no names at all, so that no later stage re-resolves anything: a
 * planner reading one needs no symbol table, and an executor needs no case
 * for a name that has not been numbered yet.
 *
 * Safety is one set computation rather than a search.  Let R be the slots
 * occurring in the head, in a negated literal or in a comparison, and B the
 * slots a rule can bind: those occurring in an argument of a positive
 * relational body literal, closed under assignment -- an equality one side
 * of which is a bare variable not in B and whose other side reads only
 * slots of B adds that variable to B.  The rule is safe exactly when R \ B
 * is empty, and the offending variables are precisely its members.
 *
 * No reordering search is needed.  A positive literal needs nothing bound in
 * order to be scanned, so every positive literal can always be scheduled
 * first; after them, every assignment the closure fired can fire in the
 * order the closure fired it, since each needs only what came before it;
 * and after those, every remaining comparison and every negated literal
 * reads only slots of B.  So if a binding order exists at all, that one is
 * such an order, and a planner that places each test and each assignment as
 * soon as it can always places every literal.  The closure is monotone, so
 * its result does not depend on the order the equalities are tried in.
 *
 * Compilation is incremental, and that is what makes checking cached.  The
 * syntax tree is append-only, so the statements behind the program's cursor
 * can never change; a check compiles from the cursor to the end.  A check
 * with nothing new to do therefore compiles nothing and says nothing, each
 * statement is compiled once for the life of the context, and each
 * diagnostic is emitted once rather than repeated by every later check.
 *
 * An aggregate is compiled into two rules.  Its body becomes a group rule,
 * an ordinary rule whose head is a generated group predicate over the body's
 * shared and local variables, so that the group predicate's relation is,
 * for each binding of the shared variables, exactly the set of rows the
 * aggregate folds.  The rule holding the aggregate keeps a literal that
 * folds that relation.  Safety covers both with no third computation: the
 * group rule is checked as a rule, and the closure binds an aggregate's
 * result once B holds every shared slot of it, as it binds an assignment's
 * variable once B holds the other side.  A statement's rules are published
 * together or not at all.
 *
 * A query is compiled by the same code into a rule of its own.  Its head
 * names no predicate, and its arguments are the query's named variables in
 * order of first occurrence, so "every output column is bound" is the
 * ordinary safety check of a head.  The tuples of that head go to a relation
 * the query object owns rather than to a predicate's, so a query adds
 * nothing to the program's tables but the predicates its body names.  A
 * query written in the program is compiled with the program and makes it
 * invalid when it is unsafe; a query prepared from text is compiled into an
 * arena of its own.
 */

/*
 * What diagnostics about a prepared query point at when the caller names no
 * source, since query text comes from no file.
 */
#define	RELIC_QUERY_SOURCE	"<query>"

/*
 * Room for a statement's description: "fact " or "rule " and a predicate, or
 * "the query".
 */
#define	RELIC_STATEMENT_TEXT_MAX	(RELIC_PRED_TEXT_MAX + 8)

/*
 * Where an argument sits, which is the whole of what safety turns on.
 *
 * A positive relational body literal binds its arguments.  A negated
 * literal probes its relation by its arguments: each named one is read and
 * must already be bound, and `_` is a column the probe does not read.  In
 * the head and a comparison every argument is read, `_` included, and must
 * already have been bound.
 */
enum relic_arg_position {
	RELIC_POSITION_BINDING,
	RELIC_POSITION_PROBED,
	RELIC_POSITION_REQUIRED
};

/*
 * How compiling a statement ended, which is what the program's cursor is
 * decided by.
 *
 * The two failures mean opposite things to the cursor, so every step that
 * can fail returns this rather than a boolean its caller would have to
 * interpret.  Inferring the reason from a flag set somewhere else would rest
 * the cursor's correctness on a list of which failure site sets which flag,
 * and a list like that is only ever as good as its last entry.
 *
 *	RELIC_COMPILE_REJECTED		the statement is wrong and has been
 *					reported.  The cursor moves past it,
 *					the program is refused, and a later
 *					check does not repeat the message.
 *
 *	RELIC_COMPILE_INCOMPLETE	the library could not finish looking:
 *					it ran out of memory, or out of
 *					predicate identifiers.  Nothing was
 *					learned about the statement, so the
 *					cursor stays and a later check
 *					resumes at it.
 */
enum relic_compile_result {
	RELIC_COMPILE_OK,
	RELIC_COMPILE_REJECTED,
	RELIC_COMPILE_INCOMPLETE
};

typedef struct relic_compiled_statement relic_compiled_statement_t;
typedef struct relic_compiler relic_compiler_t;
typedef struct relic_group_columns relic_group_columns_t;
typedef struct relic_scope relic_scope_t;
typedef struct relic_scope_walk relic_scope_walk_t;
typedef struct relic_scopes relic_scopes_t;
typedef struct relic_share_walk relic_share_walk_t;

/*
 * Something done to each term of a piece of syntax, in the order the terms
 * are written.
 */
typedef void relic_term_visitor_t(void *state, const relic_ast_term_t *term);

/*
 * The region of a statement outside every aggregate's braces: its head and
 * its body literals, an aggregate's result among them.  An aggregate's
 * braces and the parentheses of its aggregated variable are the region
 * numbered by the aggregate literal's position in the body, from 1.
 */
#define	RELIC_REGION_RULE	0

/*
 * Which region a named variable of a statement holding aggregates belongs
 * to: an aggregate's, when every occurrence of the variable lies inside
 * that aggregate, and RELIC_REGION_RULE otherwise.  A variable that belongs
 * to an aggregate is local to it; one of the rule that occurs inside an
 * aggregate is shared with it.
 *
 * Whether a variable is local depends on the whole statement, since a later
 * literal can share a variable an earlier body mentions, so the occurrences
 * are classified before anything is compiled.  Each named occurrence is
 * recorded as its spelling and its region.  Sorted by spelling and then by
 * region, the occurrences of one spelling are adjacent, and they fold to one
 * entry whose home is their region when they all have one and the rule's
 * otherwise.  A lookup is then a binary search over the distinct spellings,
 * so a statement is classified in O(n log n) of its occurrences however
 * many aggregates it holds.
 */
struct relic_scope {
	uint32_t	name;
	size_t		home;
};

struct relic_scopes {
	relic_scope_t	*entries;	/* sorted by name, one per name */
	size_t		 count;
};

/*
 * The walk that records a statement's named occurrences: the region it is
 * in, and the array it writes them to, or NULL while it only counts them.
 */
struct relic_scope_walk {
	relic_scopes_t	*scopes;
	size_t		 region;
};

/*
 * The columns of an aggregate's group predicate, before they are ordered:
 * each named variable of the aggregate's body in order of first occurrence,
 * then the aggregated variable when the body does not mention it, each with
 * where it first occurs inside the aggregate.  At most one more than a
 * predicate may have is kept, since the one past the limit is the
 * occurrence a refusal points at and nothing after it is looked at.
 */
struct relic_group_columns {
	uint32_t		name[RELIC_MAX_ARITY + 1];
	relic_location_t	where[RELIC_MAX_ARITY + 1];
	uint16_t		count;
};

/*
 * The walk that gives the shared variables of the aggregate at `position`
 * their slots in the rule holding it: the slots it has given, and how the
 * first slot it could not give failed.
 */
struct relic_share_walk {
	relic_compiler_t		*compiler;
	size_t				 position;
	uint64_t			 shared;
	enum relic_compile_result	 result;
};

/*
 * One statement being compiled.
 *
 * The rule under construction lives here rather than in the caller, so that
 * every helper reaches it through one argument and a diagnostic can name the
 * predicate the statement defines without that being threaded down beside
 * it.  Nothing is published until the rule is finished: it is copied into
 * the program's rule table as the last step, so a statement that is rejected
 * or runs out of memory leaves no half-built rule behind.
 *
 * `variables` is a fixed array because a rule may hold at most
 * RELIC_MAX_VARIABLES of them, and the slot a variable occupies is its index
 * in it.  The occupied prefix is copied into the program arena once the
 * count is final.
 *
 * The two masks are the sets B and R of the safety computation at the top of
 * this file, and are the whole of that analysis: `bindable` holds the slots
 * of positive literals as the literals are compiled, and grows by the
 * assignment closure once the body is.  Nothing here records the verdict,
 * because the verdict is `required & ~bindable`, and a stored copy of a
 * derived fact is a second thing to keep in step.
 *
 * `arena` is where the rule's variable-length parts go: the program arena for
 * a statement of the program, a query's own arena for a prepared query.
 *
 * `scopes` says which variables of a statement holding aggregates are local
 * to a group body, and is NULL for a statement with none, all of whose
 * variables are the rule's.  A group rule is compiled by a compiler of its
 * own, whose `outer` is the compiler of the rule holding the aggregate and
 * whose `scopes` is NULL, since a group body holds no aggregate.
 * `group_rules` holds the statement's group rules once they are compiled,
 * one per aggregate, in body order.
 */
struct relic_compiler {
	relic_t			*ctx;
	relic_arena_t		*arena;
	const relic_ast_stmt_t	*statement;
	const relic_scopes_t	*scopes;
	const relic_compiler_t	*outer;
	relic_rule_t		*group_rules;
	size_t			 group_rule_count;
	relic_rule_t		 rule;
	relic_variable_t	 variables[RELIC_MAX_VARIABLES];
	uint64_t		 bindable;	/* the set B */
	uint64_t		 required;	/* the set R */
	uint16_t		 variable_count;
};

/*
 * What compiling one statement produces: its rule, and the group rules of
 * its aggregates, `group_rule_count` of them in body order, in the arena the
 * statement was compiled into.  Each aggregate literal of the rule points
 * at its own group rule in that array.
 */
struct relic_compiled_statement {
	relic_rule_t		 rule;
	relic_rule_t		*group_rules;
	size_t			 group_rule_count;
};

static bool	 anonymous_is_read(enum relic_arg_position position);
static enum relic_compile_result append_loaded_query(relic_t *ctx,
		    const relic_compiled_statement_t *compiled, size_t serial);
static enum relic_compile_result append_program_rules(relic_t *ctx,
		    relic_compiled_statement_t *compiled);
static void	 bind_by_assignment(relic_compiler_t *compiler);
static int	 check_program(relic_t *ctx);
static int	 compile_and_analyse(relic_t *ctx);
static enum relic_compile_result classify_scopes(relic_t *ctx,
		    const relic_ast_stmt_t *statement, relic_scopes_t *scopes);
static void	 collect_group_columns(const relic_ast_aggregate_t *aggregate,
		    relic_group_columns_t *columns);
static void	*compile_alloc(relic_compiler_t *compiler, size_t count,
		    size_t size);
static enum relic_compile_result compile_aggregate(relic_compiler_t *compiler,
		    const relic_ast_aggregate_t *source, size_t position,
		    relic_aggregate_t *aggregate);
static enum relic_compile_result compile_atom(relic_compiler_t *compiler,
		    const relic_ast_atom_t *source,
		    enum relic_arg_position position, relic_atom_t *atom);
static enum relic_compile_result compile_body(relic_compiler_t *compiler);
static enum relic_compile_result compile_call(relic_compiler_t *compiler,
		    const relic_ast_call_t *source, relic_expr_t *expr);
static enum relic_compile_result compile_constant_pattern(
		    relic_compiler_t *compiler, const relic_location_t *where,
		    relic_expr_call_t *call);
static enum relic_compile_result compile_comparison(
		    relic_compiler_t *compiler,
		    const relic_ast_comparison_t *source,
		    relic_comparison_t *comparison);
static enum relic_compile_result compile_expr(relic_compiler_t *compiler,
		    const relic_ast_expr_t *source, relic_expr_t **exprp);
static enum relic_compile_result compile_group_rule(relic_compiler_t *outer,
		    const relic_ast_aggregate_t *source, size_t position,
		    relic_aggregate_t *aggregate, relic_rule_t *group_rule);
static enum relic_compile_result compile_group_rules(
		    relic_compiler_t *compiler);
static enum relic_compile_result compile_head(relic_compiler_t *compiler);
static enum relic_compile_result compile_literal(relic_compiler_t *compiler,
		    const relic_ast_literal_t *source, size_t position,
		    relic_literal_t *literal);
static bool	 compile_pending_statements(relic_t *ctx);
static enum relic_compile_result compile_query(relic_compiler_t *compiler,
		    relic_compiled_statement_t *compiled);
static enum relic_compile_result compile_query_head(
		    relic_compiler_t *compiler);
static enum relic_compile_result compile_rule(relic_compiler_t *compiler,
		    relic_compiled_statement_t *compiled);
static enum relic_compile_result compile_statement(relic_t *ctx,
		    const relic_ast_stmt_t *statement);
static enum relic_compile_result compile_statement_rules(relic_t *ctx,
		    relic_arena_t *arena, const relic_ast_stmt_t *statement,
		    relic_compiled_statement_t *compiled);
static void	 compiler_init(relic_compiler_t *compiler, relic_t *ctx,
		    relic_arena_t *arena, const relic_ast_stmt_t *statement);
static enum relic_compile_result compile_term(relic_compiler_t *compiler,
		    const relic_ast_term_t *term,
		    enum relic_arg_position position, relic_arg_t *arg);
static relic_query_t *copy_loaded_query(relic_t *ctx, size_t index);
static size_t	 count_aggregates(const relic_ast_stmt_t *statement);
static const char *describe_statement(const relic_compiler_t *compiler,
		    char *buffer, size_t size);
static enum relic_compile_result finish_rule(relic_compiler_t *compiler,
		    relic_compiled_statement_t *compiled);
static const relic_ast_stmt_t *first_pending_statement(const relic_t *ctx);
static void	 fold_scopes(relic_scopes_t *scopes);
static enum relic_compile_result intern_body_predicates(
		    relic_compiler_t *compiler,
		    const relic_ast_aggregate_t *source);
static enum relic_compile_result intern_group_predicate(
		    relic_compiler_t *compiler, size_t position, uint16_t arity,
		    const relic_location_t *where, relic_pred_id_t *idp);
static bool	 literal_assigns(const relic_literal_t *literal,
		    uint64_t bound, uint16_t *slotp);
static void	 mark_group_predicates_derived(relic_t *ctx,
		    const relic_compiled_statement_t *compiled);
static void	 note_group_column(void *state, const relic_ast_term_t *term);
static enum relic_compile_result occupy_slot(relic_compiler_t *compiler,
		    const relic_ast_variable_t *variable,
		    const relic_location_t *where, uint16_t *slotp);
static relic_query_t *prepare_query(relic_t *ctx, const char *source_name,
		    const char *text);
static size_t	 next_query_serial(const relic_t *ctx);
static void	 publish_query_number(relic_t *ctx);
static relic_query_t *query_create(relic_t *ctx);
static int	 query_create_answer(relic_query_t *query, size_t serial);
static void	 record_occurrence(void *state, const relic_ast_term_t *term);
static void	 record_statement_occurrences(const relic_ast_stmt_t *statement,
		    relic_scopes_t *scopes);
static void	 report_group_unbound(relic_compiler_t *compiler,
		    const relic_aggregate_t *aggregate, uint64_t unbound);
static void	 report_singleton_variables(relic_compiler_t *compiler,
		    uint64_t unbound);
static void	 report_unbound_variables(relic_compiler_t *compiler,
		    uint64_t unbound);
static void	 report_variable_limit(relic_compiler_t *compiler,
		    const relic_location_t *where);
static int	 scope_compare(const void *left, const void *right);
static void	 seed_group_columns(relic_compiler_t *compiler,
		    const relic_group_columns_t *columns,
		    const relic_aggregate_t *aggregate, size_t position);
static void	 share_variable(void *state, const relic_ast_term_t *term);
static bool	 singleton_exempt(const relic_compiler_t *compiler,
		    const relic_variable_t *variable);
static enum relic_compile_result store_variables(relic_compiler_t *compiler);
static uint64_t	 unbound_variables(const relic_compiler_t *compiler);
static const char *variable_name(const relic_compiler_t *compiler,
		    const relic_variable_t *variable, char *buffer,
		    size_t size);
static bool	 variable_is_local(const relic_scopes_t *scopes,
		    uint32_t name, size_t region);
static void	 visit_expr_terms(const relic_ast_expr_t *expr,
		    relic_term_visitor_t *visit, void *state);
static void	 visit_group_terms(const relic_ast_aggregate_t *aggregate,
		    relic_term_visitor_t *visit, void *state);
static void	 visit_literal_terms(const relic_ast_literal_t *literal,
		    relic_term_visitor_t *visit, void *state);

void
relic_program_init(relic_program_t *program, uint64_t hash_seed)
{
	bzero(program, sizeof(*program));
	relic_predtab_init(&program->predicates, hash_seed);
	relic_schedule_init(&program->schedule);
}

void
relic_program_reset(relic_program_t *program)
{
	/*
	 * The variable-length parts of every rule belong to the program
	 * arena, which releases them when it is reset.  What is released here
	 * is the rest: the predicate table, the schedule with its own arena,
	 * and the arrays of rules and of the program's queries.
	 */
	relic_predtab_reset(&program->predicates);
	relic_schedule_reset(&program->schedule);
	relic_free_array(program->rules.entries);
	relic_free_array(program->queries.entries);
	bzero(program, sizeof(*program));
}

/*
 * Allocate `count` items of `size` bytes for the rule being built.
 *
 * A null return means the compiler's arena has been marked failed, and the
 * phase that owns the arena reports that once at its boundary.  A call site
 * therefore answers RELIC_COMPILE_INCOMPLETE without reporting for itself, so
 * one exhaustion produces one message however many allocations a statement
 * was part way through.
 */
static void *
compile_alloc(relic_compiler_t *compiler, size_t count, size_t size)
{
	return (relic_arena_allocarray(compiler->arena, count, size));
}

/*
 * What a message calls the statement being compiled: "fact edge/2" or "rule
 * path/2", after what its writer wrote, or "the query".  The tree represents
 * a fact as a rule whose body has no literals, and only the wording
 * distinguishes them.  A query's head names no predicate, so a message about
 * a query names none.
 *
 * The kind is read from the statement rather than from the rule being built,
 * so that the description is available from the first line of compilation
 * and does not depend on how far the build has got.  A group rule is
 * described as the statement it was written in, since its head is a name
 * the writer never wrote.
 */
static const char *
describe_statement(const relic_compiler_t *compiler, char *buffer,
    size_t size)
{
	const relic_ast_stmt_t *statement;
	const char *noun;
	char described[RELIC_PRED_TEXT_MAX];

	if (compiler->outer != NULL)
		return (describe_statement(compiler->outer, buffer, size));

	statement = compiler->statement;

	if (statement->kind == RELIC_STMT_QUERY) {
		strlcpy(buffer, "the query", size);
		return (buffer);
	}

	noun = statement->body_length == 0 ? "fact" : "rule";
	snprintf(buffer, size, "%s %s", noun,
	    relic_pred_describe(compiler->ctx, compiler->rule.head, described,
	    sizeof(described)));

	return (buffer);
}

/*
 * The spelling of a variable, shortened to fit a message.  Every byte of it
 * is an identifier byte, so it is safe to quote.
 */
static const char *
variable_name(const relic_compiler_t *compiler,
    const relic_variable_t *variable, char *buffer, size_t size)
{
	const char *bytes;

	bytes = relic_symbol_bytes(compiler->ctx, variable->name, NULL);
	if (bytes == NULL)
		bytes = "?";

	return (relic_diag_name(bytes, buffer, size));
}

static void
report_variable_limit(relic_compiler_t *compiler,
    const relic_location_t *where)
{
	char described[RELIC_STATEMENT_TEXT_MAX];

	relic_diagf(compiler->ctx, RELIC_ERROR, where,
	    "%s%s uses more than %d distinct variables",
	    compiler->outer != NULL ? "the aggregate in " : "",
	    describe_statement(compiler, described, sizeof(described)),
	    RELIC_MAX_VARIABLES);
}

/*
 * The slot this variable occurrence uses, assigning one if the rule has not
 * seen the variable before.
 *
 * Occurrences of one spelling share a slot.  Occurrences of `_` never do,
 * because every `_` denotes a distinct variable, so an anonymous occurrence
 * skips the search and always takes a fresh slot.
 */
static enum relic_compile_result
occupy_slot(relic_compiler_t *compiler, const relic_ast_variable_t *variable,
    const relic_location_t *where, uint16_t *slotp)
{
	relic_variable_t *entry;
	uint16_t slot;

	if (!variable->anonymous) {
		for (slot = 0; slot < compiler->variable_count; slot++) {
			entry = &compiler->variables[slot];

			if (entry->anonymous || entry->name != variable->name)
				continue;

			entry->occurrences++;
			*slotp = slot;

			return (RELIC_COMPILE_OK);
		}
	}

	if (compiler->variable_count == RELIC_MAX_VARIABLES) {
		report_variable_limit(compiler, where);
		return (RELIC_COMPILE_REJECTED);
	}

	slot = compiler->variable_count;
	entry = &compiler->variables[slot];

	entry->where = *where;
	entry->name = variable->name;
	entry->occurrences = 1;
	entry->anonymous = variable->anonymous;

	compiler->variable_count++;
	*slotp = slot;

	return (RELIC_COMPILE_OK);
}

/*
 * Compile one argument, recording what its position means for safety.
 */
static enum relic_compile_result
compile_term(relic_compiler_t *compiler, const relic_ast_term_t *term,
    enum relic_arg_position position, relic_arg_t *arg)
{
	const relic_ast_variable_t *variable;
	enum relic_compile_result result;
	uint16_t slot;

	bzero(arg, sizeof(*arg));

	if (term->kind == RELIC_TERM_CONSTANT) {
		arg->kind = RELIC_ARG_CONSTANT;
		arg->u.constant = term->u.constant;

		return (RELIC_COMPILE_OK);
	}

	variable = &term->u.variable;

	/*
	 * `_` in an argument of a positive or a negated body literal is a
	 * wildcard: it binds nothing and nothing reads it, so a slot for it
	 * would be a frame entry counting against the limit of 64 for no
	 * reason a reader could see.  A rule scanning two wide relations with
	 * nothing but `_` would otherwise exceed the limit without holding a
	 * single variable.
	 *
	 * In the head and a comparison `_` is read and can never be bound, so
	 * it does take a slot, and the one computation below reports it
	 * alongside the named variables and in the same textual order.  The
	 * rule is always rejected, so such a slot never reaches the evaluator.
	 */
	if (variable->anonymous && !anonymous_is_read(position)) {
		arg->kind = RELIC_ARG_WILDCARD;

		return (RELIC_COMPILE_OK);
	}

	result = occupy_slot(compiler, variable, &term->where, &slot);
	if (result != RELIC_COMPILE_OK)
		return (result);

	if (position == RELIC_POSITION_BINDING)
		compiler->bindable |= UINT64_C(1) << slot;
	else
		compiler->required |= UINT64_C(1) << slot;

	arg->kind = RELIC_ARG_VARIABLE;
	arg->u.slot = slot;

	return (RELIC_COMPILE_OK);
}

/*
 * Whether `_` as an argument in this position would be read, a value that
 * nothing binds: in the head and in a comparison.  A positive literal
 * matches any value where `_` stands, and a negated literal's probe leaves
 * the column out, so neither reads it, and there `_` is a wildcard.
 */
static bool
anonymous_is_read(enum relic_arg_position position)
{
	switch (position) {
	case RELIC_POSITION_BINDING:
	case RELIC_POSITION_PROBED:
		return (false);
	case RELIC_POSITION_REQUIRED:
		break;
	}

	return (true);
}

/*
 * Compile a predicate applied to arguments.  The arity comes from the atom
 * as written, which is what the predicate is interned under, so the argument
 * array and the predicate can never disagree about its length.
 */
static enum relic_compile_result
compile_atom(relic_compiler_t *compiler, const relic_ast_atom_t *source,
    enum relic_arg_position position, relic_atom_t *atom)
{
	enum relic_compile_result result;
	uint16_t i;

	bzero(atom, sizeof(*atom));
	atom->where = source->where;

	if (relic_pred_intern(compiler->ctx, source->name, source->arity,
	    &source->where, &atom->pred) != 0)
		return (RELIC_COMPILE_INCOMPLETE);
	relic_pred_reference(compiler->ctx, atom->pred, &source->where);

	if (source->arity == 0)
		return (RELIC_COMPILE_OK);

	atom->args = compile_alloc(compiler, source->arity,
	    sizeof(*atom->args));
	if (atom->args == NULL)
		return (RELIC_COMPILE_INCOMPLETE);

	for (i = 0; i < source->arity; i++) {
		result = compile_term(compiler, &source->terms[i], position,
		    &atom->args[i]);
		if (result != RELIC_COMPILE_OK)
			return (result);
	}

	return (RELIC_COMPILE_OK);
}

/*
 * Compile an expression tree, numbering its variables in the order they are
 * written.  Every variable of an expression is in a required position: a
 * comparison reads its operands, and even the variable an assignment binds
 * is bound only by the closure over the whole body, never by being written.
 * The tree is no taller than the parser allowed, so the recursion is
 * bounded.
 */
static enum relic_compile_result
compile_expr(relic_compiler_t *compiler, const relic_ast_expr_t *source,
    relic_expr_t **exprp)
{
	enum relic_compile_result result;
	relic_expr_t *expr;
	relic_arg_t arg;

	expr = compile_alloc(compiler, 1, sizeof(*expr));
	if (expr == NULL)
		return (RELIC_COMPILE_INCOMPLETE);
	bzero(expr, sizeof(*expr));
	expr->height = source->height;
	*exprp = expr;

	switch (source->kind) {
	case RELIC_AST_EXPR_TERM:
		result = compile_term(compiler, &source->u.term,
		    RELIC_POSITION_REQUIRED, &arg);
		if (result != RELIC_COMPILE_OK)
			return (result);

		if (arg.kind == RELIC_ARG_CONSTANT) {
			expr->kind = RELIC_EXPR_CONSTANT;
			expr->u.constant = arg.u.constant;
		} else {
			expr->kind = RELIC_EXPR_VARIABLE;
			expr->u.slot = arg.u.slot;
		}
		expr->nodes = 1;
		return (RELIC_COMPILE_OK);
	case RELIC_AST_EXPR_NEGATE:
		expr->kind = RELIC_EXPR_NEGATE;
		result = compile_expr(compiler, source->u.operand,
		    &expr->u.operand);
		if (result != RELIC_COMPILE_OK)
			return (result);

		expr->nodes = 1 + expr->u.operand->nodes;
		return (RELIC_COMPILE_OK);
	case RELIC_AST_EXPR_BINARY:
		expr->kind = RELIC_EXPR_BINARY;
		expr->u.binary.op = source->u.binary.op;
		result = compile_expr(compiler, source->u.binary.left,
		    &expr->u.binary.left);
		if (result != RELIC_COMPILE_OK)
			return (result);
		result = compile_expr(compiler, source->u.binary.right,
		    &expr->u.binary.right);
		if (result != RELIC_COMPILE_OK)
			return (result);

		expr->nodes = 1 + expr->u.binary.left->nodes +
		    expr->u.binary.right->nodes;
		return (RELIC_COMPILE_OK);
	case RELIC_AST_EXPR_CALL:
		return (compile_call(compiler, &source->u.call, expr));
	}

	/*
	 * Unreachable: the parser builds no other kind of node.  Reported
	 * rather than assumed, as compile_literal() reports a literal of no
	 * known kind.
	 */
	relic_diagf(compiler->ctx, RELIC_ERROR, &source->where,
	    "internal error: an expression holds a node of no known kind");

	return (RELIC_COMPILE_REJECTED);
}

/*
 * A call's arguments, into the node `expr`.  The parser refuses a call that
 * is no function's, so every call reaching here is resolved and has its
 * function's arity of arguments; a call that is not is refused as an
 * internal error rather than compiled into something the executor would
 * read past.
 */
static enum relic_compile_result
compile_call(relic_compiler_t *compiler, const relic_ast_call_t *source,
    relic_expr_t *expr)
{
	enum relic_compile_result result;
	relic_expr_call_t *call;
	size_t i;

	if (!source->resolved ||
	    source->argc != relic_function_info(source->fn)->arity) {
		relic_diagf(compiler->ctx, RELIC_ERROR, &source->open,
		    "internal error: a call of no function reached the "
		    "compiler");
		return (RELIC_COMPILE_REJECTED);
	}

	expr->kind = RELIC_EXPR_CALL;
	call = &expr->u.call;
	call->fn = source->fn;
	call->argc = (uint8_t)source->argc;
	expr->nodes = 1;

	for (i = 0; i < source->argc; i++) {
		result = compile_expr(compiler, source->args[i],
		    &call->args[i]);
		if (result != RELIC_COMPILE_OK)
			return (result);

		expr->nodes += call->args[i]->nodes;
	}

	if (call->fn == RELIC_FN_MATCH &&
	    call->args[1]->kind == RELIC_EXPR_CONSTANT)
		return (compile_constant_pattern(compiler,
		    &source->args[1]->where, call));

	return (RELIC_COMPILE_OK);
}

/*
 * Compile the pattern of a match whose pattern is a string constant,
 * written at `where`, into the rule's arena, so that it is compiled once
 * rather than once per binding.  A pattern that is not a regular expression
 * refuses the statement, as an unsafe rule is refused, so a program whose
 * patterns are constants learns of a bad one before anything is evaluated.
 *
 * A constant of another type is left to the executor, which gives the call
 * no value, as it does for any operand of the wrong type.  A pattern too
 * long for a program to number its instructions could never be laid out,
 * and is refused as the arena refusing it.
 */
static enum relic_compile_result
compile_constant_pattern(relic_compiler_t *compiler,
    const relic_location_t *where, relic_expr_call_t *call)
{
	relic_regex_error_t error;
	relic_regex_inst_t *insts;
	relic_pattern_t *pattern;
	const char *bytes;
	size_t capacity;
	size_t length;
	uint32_t count;

	if ((call->args[1]->u.constant & RELIC_DOM_TYPE_MASK) !=
	    RELIC_DOM_STRING)
		return (RELIC_COMPILE_OK);

	bytes = relic_value_bytes(compiler->ctx, call->args[1]->u.constant,
	    &length);
	assert(bytes != NULL);

	capacity = relic_regex_capacity(length);
	if (capacity == 0) {
		relic_arena_fail(compiler->arena);
		return (RELIC_COMPILE_INCOMPLETE);
	}

	pattern = compile_alloc(compiler, 1, sizeof(*pattern));
	insts = compile_alloc(compiler, capacity, sizeof(*insts));
	if (pattern == NULL || insts == NULL)
		return (RELIC_COMPILE_INCOMPLETE);

	count = relic_regex_compile(bytes, length, insts, &error);
	if (count == 0) {
		relic_diagf(compiler->ctx, RELIC_ERROR, where,
		    "invalid regular expression: %s",
		    relic_regex_message(error.failure));
		return (RELIC_COMPILE_REJECTED);
	}

	pattern->insts = insts;
	pattern->count = count;
	call->pattern = pattern;

	return (RELIC_COMPILE_OK);
}

/*
 * A comparison of two expressions.  Both are required positions; whether
 * the comparison binds one of its variables is a question about the whole
 * body, which bind_by_assignment() answers once the body is compiled.
 */
static enum relic_compile_result
compile_comparison(relic_compiler_t *compiler,
    const relic_ast_comparison_t *source, relic_comparison_t *comparison)
{
	enum relic_compile_result result;

	bzero(comparison, sizeof(*comparison));
	comparison->op = source->op;

	result = compile_expr(compiler, source->left, &comparison->left);
	if (result != RELIC_COMPILE_OK)
		return (result);

	return (compile_expr(compiler, source->right, &comparison->right));
}

/*
 * Visit the terms of an expression tree, left to right.  The tree is no
 * taller than the parser allowed, so the recursion is bounded.
 */
static void
visit_expr_terms(const relic_ast_expr_t *expr, relic_term_visitor_t *visit,
    void *state)
{
	size_t i;

	switch (expr->kind) {
	case RELIC_AST_EXPR_TERM:
		visit(state, &expr->u.term);
		return;
	case RELIC_AST_EXPR_NEGATE:
		visit_expr_terms(expr->u.operand, visit, state);
		return;
	case RELIC_AST_EXPR_BINARY:
		visit_expr_terms(expr->u.binary.left, visit, state);
		visit_expr_terms(expr->u.binary.right, visit, state);
		return;
	case RELIC_AST_EXPR_CALL:
		for (i = 0; i < expr->u.call.argc &&
		    i < RELIC_MAX_FUNCTION_ARITY; i++)
			visit_expr_terms(expr->u.call.args[i], visit, state);
		return;
	}
}

/*
 * Visit the terms a literal holds outside any aggregate's braces: an atom's
 * arguments, a comparison's operands, or an aggregate's result.  The terms
 * inside an aggregate are visit_group_terms()'s.
 */
static void
visit_literal_terms(const relic_ast_literal_t *literal,
    relic_term_visitor_t *visit, void *state)
{
	uint16_t i;

	switch (literal->kind) {
	case RELIC_LITERAL_POSITIVE:
	case RELIC_LITERAL_NEGATED:
		for (i = 0; i < literal->u.atom.arity; i++)
			visit(state, &literal->u.atom.terms[i]);
		return;
	case RELIC_LITERAL_COMPARISON:
		visit_expr_terms(literal->u.comparison.left, visit, state);
		visit_expr_terms(literal->u.comparison.right, visit, state);
		return;
	case RELIC_LITERAL_AGGREGATE:
		visit(state, &literal->u.aggregate.result);
		return;
	}
}

/*
 * Visit the terms inside an aggregate in the order they are written: the
 * aggregated variable, then the body's.  A body holds no aggregate, so each
 * of its literals is visited whole.
 */
static void
visit_group_terms(const relic_ast_aggregate_t *aggregate,
    relic_term_visitor_t *visit, void *state)
{
	size_t i;

	if (aggregate->fn != RELIC_AGG_COUNT)
		visit(state, &aggregate->column);

	for (i = 0; i < aggregate->body_length; i++)
		visit_literal_terms(&aggregate->body[i], visit, state);
}

/*
 * The number of aggregate literals in a statement's body.
 */
static size_t
count_aggregates(const relic_ast_stmt_t *statement)
{
	size_t count;
	size_t i;

	count = 0;
	for (i = 0; i < statement->body_length; i++) {
		if (statement->body[i].kind == RELIC_LITERAL_AGGREGATE)
			count++;
	}

	return (count);
}

/*
 * Record one named occurrence in the walk's region, or only count it while
 * the walk has no array yet.
 */
static void
record_occurrence(void *state, const relic_ast_term_t *term)
{
	relic_scope_walk_t *walk;
	relic_scope_t *entry;

	walk = state;
	if (term->kind != RELIC_TERM_VARIABLE || term->u.variable.anonymous)
		return;

	if (walk->scopes->entries != NULL) {
		entry = &walk->scopes->entries[walk->scopes->count];
		entry->name = term->u.variable.name;
		entry->home = walk->region;
	}
	walk->scopes->count++;
}

/*
 * Record every named occurrence of a statement with its region: the head's
 * and each literal's in the rule's region, and those inside an aggregate in
 * the aggregate's.
 */
static void
record_statement_occurrences(const relic_ast_stmt_t *statement,
    relic_scopes_t *scopes)
{
	const relic_ast_literal_t *literal;
	relic_scope_walk_t walk;
	uint16_t column;
	size_t i;

	walk.scopes = scopes;
	walk.region = RELIC_REGION_RULE;

	if (statement->kind == RELIC_STMT_RULE) {
		for (column = 0; column < statement->head.arity; column++)
			record_occurrence(&walk,
			    &statement->head.terms[column]);
	}

	for (i = 0; i < statement->body_length; i++) {
		literal = &statement->body[i];

		walk.region = RELIC_REGION_RULE;
		visit_literal_terms(literal, record_occurrence, &walk);

		if (literal->kind != RELIC_LITERAL_AGGREGATE)
			continue;
		walk.region = i + 1;
		visit_group_terms(&literal->u.aggregate, record_occurrence,
		    &walk);
	}
}

/*
 * Order occurrences by spelling, and those of one spelling by region.
 */
static int
scope_compare(const void *left, const void *right)
{
	const relic_scope_t *a;
	const relic_scope_t *b;

	a = left;
	b = right;

	if (a->name != b->name)
		return (a->name < b->name ? -1 : 1);
	if (a->home != b->home)
		return (a->home < b->home ? -1 : 1);

	return (0);
}

/*
 * Fold sorted occurrences to one entry per spelling, in place: the region
 * every occurrence has when they agree, and the rule's when they do not.
 */
static void
fold_scopes(relic_scopes_t *scopes)
{
	relic_scope_t *kept;
	size_t in;
	size_t out;

	out = 0;
	for (in = 0; in < scopes->count; in++) {
		kept = out > 0 ? &scopes->entries[out - 1] : NULL;

		if (kept != NULL && kept->name == scopes->entries[in].name) {
			if (kept->home != scopes->entries[in].home)
				kept->home = RELIC_REGION_RULE;
			continue;
		}

		scopes->entries[out] = scopes->entries[in];
		out++;
	}

	scopes->count = out;
}

/*
 * Classify the named variables of a statement holding aggregates into
 * *scopes, whose array the caller frees.  The occurrences are counted, then
 * recorded into an array of exactly that size, sorted and folded.  Returns
 * RELIC_COMPILE_OK, or RELIC_COMPILE_INCOMPLETE having reported that memory
 * ran out.
 */
static enum relic_compile_result
classify_scopes(relic_t *ctx, const relic_ast_stmt_t *statement,
    relic_scopes_t *scopes)
{
	size_t occurrences;

	bzero(scopes, sizeof(*scopes));
	record_statement_occurrences(statement, scopes);
	occurrences = scopes->count;
	if (occurrences == 0)
		return (RELIC_COMPILE_OK);

	scopes->entries = calloc(occurrences, sizeof(*scopes->entries));
	if (scopes->entries == NULL) {
		scopes->count = 0;
		relic_diag_no_memory(ctx);
		return (RELIC_COMPILE_INCOMPLETE);
	}

	scopes->count = 0;
	record_statement_occurrences(statement, scopes);
	qsort(scopes->entries, scopes->count, sizeof(*scopes->entries),
	    scope_compare);
	fold_scopes(scopes);

	return (RELIC_COMPILE_OK);
}

/*
 * Whether the named variable `name` is local to the aggregate whose region
 * is `region`: whether every occurrence of it lies inside that aggregate.
 */
static bool
variable_is_local(const relic_scopes_t *scopes, uint32_t name, size_t region)
{
	size_t low;
	size_t high;
	size_t middle;

	low = 0;
	high = scopes->count;
	while (low < high) {
		middle = low + (high - low) / 2;

		if (scopes->entries[middle].name == name)
			return (scopes->entries[middle].home == region);

		if (scopes->entries[middle].name < name)
			low = middle + 1;
		else
			high = middle;
	}

	return (false);
}

/*
 * Add a variable occurrence to an aggregate's columns unless it is `_`, a
 * constant, a column already, or past the one occurrence beyond the limit.
 */
static void
note_group_column(void *state, const relic_ast_term_t *term)
{
	relic_group_columns_t *columns;
	uint16_t i;

	columns = state;
	if (term->kind != RELIC_TERM_VARIABLE || term->u.variable.anonymous)
		return;

	for (i = 0; i < columns->count; i++) {
		if (columns->name[i] == term->u.variable.name)
			return;
	}

	if (columns->count == RELIC_MAX_ARITY + 1)
		return;

	columns->name[columns->count] = term->u.variable.name;
	columns->where[columns->count] = term->where;
	columns->count++;
}

/*
 * The columns of an aggregate's group predicate: the body's named variables
 * in order of first occurrence, then the aggregated variable if the body
 * does not name it.  Each search is over at most RELIC_MAX_ARITY + 1 names.
 */
static void
collect_group_columns(const relic_ast_aggregate_t *aggregate,
    relic_group_columns_t *columns)
{
	size_t i;

	columns->count = 0;

	for (i = 0; i < aggregate->body_length; i++)
		visit_literal_terms(&aggregate->body[i], note_group_column,
		    columns);

	if (aggregate->fn != RELIC_AGG_COUNT)
		note_group_column(columns, &aggregate->column);
}

/*
 * Give a variable inside an aggregate a slot of the rule holding it when it
 * is the rule's rather than the aggregate's own, as a required occurrence:
 * the fold reads it, so it must be bound before the fold runs.
 */
static void
share_variable(void *state, const relic_ast_term_t *term)
{
	relic_share_walk_t *walk;
	uint16_t slot;

	walk = state;
	if (walk->result != RELIC_COMPILE_OK)
		return;
	if (term->kind != RELIC_TERM_VARIABLE || term->u.variable.anonymous)
		return;
	if (variable_is_local(walk->compiler->scopes, term->u.variable.name,
	    walk->position))
		return;

	walk->result = occupy_slot(walk->compiler, &term->u.variable,
	    &term->where, &slot);
	if (walk->result != RELIC_COMPILE_OK)
		return;

	walk->compiler->required |= UINT64_C(1) << slot;
	walk->shared |= UINT64_C(1) << slot;
}

/*
 * Intern the group predicate of the aggregate at `position` in the body of
 * the statement being compiled, with `arity` columns, and name it as the
 * group of an aggregate in that statement.
 *
 * The name is built from positions and never from a counter, so the same
 * program always gives the same names.  The aggregate at position P of the
 * rule that is the Nth, from 0, of the rules deriving name/A reads
 * agg$name$A$N$P; one of a query reads agg$$q0$P, whatever the query's
 * serial, so that every query with an aggregate at one position and of one
 * arity shares the entry.  The name holds `$`, which no identifier the lexer
 * accepts contains, so no program can name such a predicate.
 */
#define	GROUP_PREFIX		"agg$"
#define	GROUP_PREFIX_LENGTH	(sizeof(GROUP_PREFIX) - 1)
#define	GROUP_QUERY_NAME	"$q0"
#define	GROUP_ORDINALS_MAX	64	/* "$A$N$P", each at most 20 digits */

static enum relic_compile_result
intern_group_predicate(relic_compiler_t *compiler, size_t position,
    uint16_t arity, const relic_location_t *where, relic_pred_id_t *idp)
{
	const relic_pred_t *head;
	relic_pred_id_t holder;
	const char *name;
	char *text;
	size_t name_length;
	size_t length;
	size_t room;
	uint32_t symbol;
	int written;

	head = NULL;
	holder = RELIC_ID_NONE;
	name = GROUP_QUERY_NAME;
	name_length = sizeof(GROUP_QUERY_NAME) - 1;
	if (compiler->statement->kind == RELIC_STMT_RULE) {
		holder = compiler->rule.head;
		head = relic_pred_of(compiler->ctx, holder);
		name = relic_symbol_bytes(compiler->ctx, head->name,
		    &name_length);
	}

	room = GROUP_PREFIX_LENGTH + name_length + GROUP_ORDINALS_MAX;
	text = malloc(room);
	if (text == NULL) {
		relic_diag_no_memory(compiler->ctx);
		return (RELIC_COMPILE_INCOMPLETE);
	}

	bcopy(GROUP_PREFIX, text, GROUP_PREFIX_LENGTH);
	bcopy(name, text + GROUP_PREFIX_LENGTH, name_length);
	length = GROUP_PREFIX_LENGTH + name_length;
	if (head != NULL)
		written = snprintf(text + length, room - length,
		    "$%u$%zu$%zu", (unsigned)head->arity, head->program_rules,
		    position);
	else
		written = snprintf(text + length, room - length, "$%zu",
		    position);
	assert(written > 0 && (size_t)written < room - length);
	length += (size_t)written;

	if (relic_symbol_intern(compiler->ctx, text, length, &symbol) != 0) {
		free(text);
		return (RELIC_COMPILE_INCOMPLETE);
	}
	free(text);

	if (relic_pred_intern(compiler->ctx, symbol, arity, where, idp) != 0)
		return (RELIC_COMPILE_INCOMPLETE);
	relic_pred_name_group(compiler->ctx, *idp, holder);

	return (RELIC_COMPILE_OK);
}

/*
 * Intern and reference the predicates the atoms of an aggregate's body
 * read, in the order they are written.  The group rule compiled later finds
 * them interned; doing it here keeps identifiers in the order predicates
 * are first written in, and makes an undefined predicate's warning point
 * at its first mention.
 */
static enum relic_compile_result
intern_body_predicates(relic_compiler_t *compiler,
    const relic_ast_aggregate_t *source)
{
	const relic_ast_atom_t *atom;
	relic_pred_id_t pred;
	size_t i;

	for (i = 0; i < source->body_length; i++) {
		if (source->body[i].kind != RELIC_LITERAL_POSITIVE &&
		    source->body[i].kind != RELIC_LITERAL_NEGATED)
			continue;

		atom = &source->body[i].u.atom;
		if (relic_pred_intern(compiler->ctx, atom->name, atom->arity,
		    &atom->where, &pred) != 0)
			return (RELIC_COMPILE_INCOMPLETE);
		relic_pred_reference(compiler->ctx, pred, &atom->where);
	}

	return (RELIC_COMPILE_OK);
}

/*
 * Compile an aggregate literal into the rule holding it: its result, the
 * slots of its shared variables, and the group predicate its fold reads.
 * Its body is compiled later, into its group rule, once every slot of this
 * rule is numbered; compile_group_rule() completes the literal then.
 *
 * The variables inside the aggregate that are the rule's take their slots
 * here, in the order they are written, and are required, since the fold
 * needs them bound.  The result is required too, and the closure of
 * bind_by_assignment() is what binds it.  The aggregate's own variables
 * take no slot of this rule.
 *
 * The shared slots are listed in slot order, which is the order of the
 * group predicate's leading columns.  Every shared variable is named inside
 * the aggregate, so there are no more of them than the group has columns.
 */
static enum relic_compile_result
compile_aggregate(relic_compiler_t *compiler,
    const relic_ast_aggregate_t *source, size_t position,
    relic_aggregate_t *aggregate)
{
	relic_group_columns_t columns;
	relic_share_walk_t walk;
	enum relic_compile_result result;
	relic_arg_t written;
	char described[RELIC_STATEMENT_TEXT_MAX];
	uint16_t *shared;
	uint16_t slot;

	bzero(aggregate, sizeof(*aggregate));
	aggregate->fn = source->fn;
	aggregate->where = source->where;

	result = compile_term(compiler, &source->result,
	    RELIC_POSITION_REQUIRED, &written);
	if (result != RELIC_COMPILE_OK)
		return (result);
	aggregate->result = written.u.slot;

	walk.compiler = compiler;
	walk.position = position;
	walk.shared = 0;
	walk.result = RELIC_COMPILE_OK;
	visit_group_terms(source, share_variable, &walk);
	if (walk.result != RELIC_COMPILE_OK)
		return (walk.result);

	collect_group_columns(source, &columns);
	if (columns.count > RELIC_MAX_ARITY) {
		relic_diagf(compiler->ctx, RELIC_ERROR,
		    &columns.where[RELIC_MAX_ARITY],
		    "the aggregate in %s mentions more than %d variables",
		    describe_statement(compiler, described, sizeof(described)),
		    RELIC_MAX_ARITY);
		return (RELIC_COMPILE_REJECTED);
	}

	result = intern_group_predicate(compiler, position, columns.count,
	    &source->where, &aggregate->group);
	if (result != RELIC_COMPILE_OK)
		return (result);

	result = intern_body_predicates(compiler, source);
	if (result != RELIC_COMPILE_OK)
		return (result);

	if (walk.shared == 0)
		return (RELIC_COMPILE_OK);

	shared = compile_alloc(compiler, columns.count, sizeof(*shared));
	if (shared == NULL)
		return (RELIC_COMPILE_INCOMPLETE);
	for (slot = 0; slot < compiler->variable_count; slot++) {
		if ((walk.shared & (UINT64_C(1) << slot)) == 0)
			continue;
		shared[aggregate->shared_count] = slot;
		aggregate->shared_count++;
	}
	aggregate->shared = shared;

	return (RELIC_COMPILE_OK);
}

/*
 * Number a group rule's columns as its variables, before its body is
 * compiled, so that column c is slot c: first the shared variables, in the
 * slot order of the rule holding the aggregate, then the aggregate's own
 * named variables in the order `columns` lists them.  Each is placed where
 * it first occurs inside the aggregate, which is what a message about it
 * points at, and counts the head as one occurrence.
 */
static void
seed_group_columns(relic_compiler_t *compiler,
    const relic_group_columns_t *columns, const relic_aggregate_t *aggregate,
    size_t position)
{
	const relic_compiler_t *outer;
	relic_variable_t *seeded;
	uint32_t name;
	uint16_t column;
	uint16_t i;

	outer = compiler->outer;

	for (i = 0; i < aggregate->shared_count; i++) {
		name = outer->variables[aggregate->shared[i]].name;

		for (column = 0; column < columns->count; column++) {
			if (columns->name[column] == name)
				break;
		}
		assert(column < columns->count);

		seeded = &compiler->variables[compiler->variable_count];
		bzero(seeded, sizeof(*seeded));
		seeded->name = name;
		seeded->where = columns->where[column];
		seeded->occurrences = 1;
		compiler->variable_count++;
	}

	for (column = 0; column < columns->count; column++) {
		if (!variable_is_local(outer->scopes, columns->name[column],
		    position))
			continue;

		seeded = &compiler->variables[compiler->variable_count];
		bzero(seeded, sizeof(*seeded));
		seeded->name = columns->name[column];
		seeded->where = columns->where[column];
		seeded->occurrences = 1;
		compiler->variable_count++;
	}
}

/*
 * Report every variable of a group rule its body cannot bind, in column
 * order, which puts the shared variables first.  The messages name the
 * aggregate and the statement it is in rather than the group predicate,
 * which the writer never wrote, and a shared variable's says why a variable
 * bound outside must be bound inside too.
 */
static void
report_group_unbound(relic_compiler_t *compiler,
    const relic_aggregate_t *aggregate, uint64_t unbound)
{
	const relic_variable_t *variable;
	char described[RELIC_STATEMENT_TEXT_MAX];
	char named[RELIC_NAME_TEXT_MAX];
	uint16_t slot;

	if (unbound == 0)
		return;

	describe_statement(compiler, described, sizeof(described));

	for (slot = 0; slot < compiler->variable_count; slot++) {
		if ((unbound & (UINT64_C(1) << slot)) == 0)
			continue;

		variable = &compiler->variables[slot];

		if (variable->anonymous) {
			relic_diagf(compiler->ctx, RELIC_ERROR,
			    &variable->where,
			    "anonymous variable in the aggregate of %s cannot "
			    "be bound by a positive literal of the aggregate "
			    "body", described);
			continue;
		}

		relic_diagf(compiler->ctx, RELIC_ERROR, &variable->where,
		    "variable %s in the aggregate of %s is not bound by a "
		    "positive literal or an assignment of the aggregate body",
		    variable_name(compiler, variable, named, sizeof(named)),
		    described);
		if (slot < aggregate->shared_count)
			relic_diagf(compiler->ctx, RELIC_NOTE, &variable->where,
			    "a variable bound outside an aggregate must also "
			    "be bound inside it");
	}
}

/*
 * Compile an aggregate's body into its group rule, and complete the
 * aggregate literal of the rule holding it with the rule and the
 * aggregated column.
 *
 * The group rule's head is the group predicate over its columns, which
 * seed_group_columns() numbers so that column c is slot c; the aggregated
 * variable's slot is therefore its column.  The rule is checked as a rule:
 * every column must be bound inside the braces, a shared one included,
 * since the relation needs a value for it that only the body can give.  No
 * singleton warning arises from it, because each named variable occurs in
 * the head as well as in the body.
 */
static enum relic_compile_result
compile_group_rule(relic_compiler_t *outer,
    const relic_ast_aggregate_t *source, size_t position,
    relic_aggregate_t *aggregate, relic_rule_t *group_rule)
{
	relic_compiler_t compiler;
	relic_group_columns_t columns;
	enum relic_compile_result result;
	uint64_t unbound;
	uint16_t column;
	size_t i;

	compiler_init(&compiler, outer->ctx, outer->arena, outer->statement);
	compiler.outer = outer;
	compiler.rule.where = source->where;
	compiler.rule.head = aggregate->group;

	collect_group_columns(source, &columns);
	seed_group_columns(&compiler, &columns, aggregate, position);

	compiler.rule.head_arity = compiler.variable_count;
	if (compiler.rule.head_arity > 0) {
		compiler.rule.head_args = compile_alloc(&compiler,
		    compiler.rule.head_arity,
		    sizeof(*compiler.rule.head_args));
		if (compiler.rule.head_args == NULL)
			return (RELIC_COMPILE_INCOMPLETE);
	}
	for (column = 0; column < compiler.variable_count; column++) {
		bzero(&compiler.rule.head_args[column],
		    sizeof(compiler.rule.head_args[column]));
		compiler.rule.head_args[column].kind = RELIC_ARG_VARIABLE;
		compiler.rule.head_args[column].u.slot = column;
		compiler.required |= UINT64_C(1) << column;
	}

	compiler.rule.body_length = source->body_length;
	compiler.rule.body = compile_alloc(&compiler, source->body_length,
	    sizeof(*compiler.rule.body));
	if (compiler.rule.body == NULL)
		return (RELIC_COMPILE_INCOMPLETE);
	for (i = 0; i < source->body_length; i++) {
		result = compile_literal(&compiler, &source->body[i], i + 1,
		    &compiler.rule.body[i]);
		if (result != RELIC_COMPILE_OK)
			return (result);
	}

	bind_by_assignment(&compiler);
	unbound = unbound_variables(&compiler);
	report_group_unbound(&compiler, aggregate, unbound);
	if (unbound != 0)
		return (RELIC_COMPILE_REJECTED);

	result = store_variables(&compiler);
	if (result != RELIC_COMPILE_OK)
		return (result);

	if (source->fn != RELIC_AGG_COUNT) {
		for (column = 0; column < compiler.variable_count; column++) {
			if (compiler.variables[column].name ==
			    source->column.u.variable.name)
				break;
		}
		assert(column < compiler.rule.head_arity);
		aggregate->column = column;
	}

	*group_rule = compiler.rule;
	aggregate->group_rule = group_rule;

	return (RELIC_COMPILE_OK);
}

/*
 * Compile the group rule of every aggregate of the statement, into an
 * array of the compiler's arena, once the rule holding them is numbered.
 * Every one is compiled even after one is rejected, so that one run reports
 * every mistake.
 */
static enum relic_compile_result
compile_group_rules(relic_compiler_t *compiler)
{
	const relic_ast_stmt_t *statement;
	enum relic_compile_result verdict;
	enum relic_compile_result result;
	size_t count;
	size_t i;

	statement = compiler->statement;
	count = count_aggregates(statement);
	if (count == 0)
		return (RELIC_COMPILE_OK);

	compiler->group_rules = compile_alloc(compiler, count,
	    sizeof(*compiler->group_rules));
	if (compiler->group_rules == NULL)
		return (RELIC_COMPILE_INCOMPLETE);

	verdict = RELIC_COMPILE_OK;
	for (i = 0; i < statement->body_length; i++) {
		if (statement->body[i].kind != RELIC_LITERAL_AGGREGATE)
			continue;

		result = compile_group_rule(compiler,
		    &statement->body[i].u.aggregate, i + 1,
		    &compiler->rule.body[i].u.aggregate,
		    &compiler->group_rules[compiler->group_rule_count]);
		if (result == RELIC_COMPILE_INCOMPLETE)
			return (result);
		if (result == RELIC_COMPILE_REJECTED)
			verdict = result;
		compiler->group_rule_count++;
	}

	return (verdict);
}

/*
 * Compile the literal at `position` in the body, from 1.
 */
static enum relic_compile_result
compile_literal(relic_compiler_t *compiler,
    const relic_ast_literal_t *source, size_t position,
    relic_literal_t *literal)
{
	bzero(literal, sizeof(*literal));
	literal->where = source->where;
	literal->kind = source->kind;

	switch (source->kind) {
	case RELIC_LITERAL_POSITIVE:
		return (compile_atom(compiler, &source->u.atom,
		    RELIC_POSITION_BINDING, &literal->u.atom));
	case RELIC_LITERAL_NEGATED:
		return (compile_atom(compiler, &source->u.atom,
		    RELIC_POSITION_PROBED, &literal->u.atom));
	case RELIC_LITERAL_COMPARISON:
		return (compile_comparison(compiler, &source->u.comparison,
		    &literal->u.comparison));
	case RELIC_LITERAL_AGGREGATE:
		/*
		 * The parser refuses an aggregate inside an aggregate's body,
		 * and a statement holding one is classified before it is
		 * compiled, so a compiler without scopes never meets one.
		 */
		if (compiler->scopes == NULL)
			break;
		return (compile_aggregate(compiler, &source->u.aggregate,
		    position, &literal->u.aggregate));
	}

	/*
	 * Unreachable: the parser produces no other kind of literal.  It is
	 * reported rather than refused in silence, because a check that
	 * rejects a program and says nothing about why leaves its caller
	 * nowhere to go, and because assertions are compiled out of an
	 * ordinary build.
	 */
	relic_diagf(compiler->ctx, RELIC_ERROR, &source->where,
	    "internal error: the body holds a literal of no known kind");

	return (RELIC_COMPILE_REJECTED);
}

/*
 * Compile the head, and record that a head names its predicate.
 *
 * The predicate is marked even when the statement goes on to be
 * rejected, because it is defined: a rule with an unbound variable is still
 * a rule about that predicate, and warning that nothing defines it would be
 * a second, false complaint about one mistake.
 */
static enum relic_compile_result
compile_head(relic_compiler_t *compiler)
{
	const relic_ast_atom_t *head;
	enum relic_compile_result result;
	uint16_t i;

	head = &compiler->statement->head;

	if (relic_pred_intern(compiler->ctx, head->name, head->arity,
	    &head->where, &compiler->rule.head) != 0)
		return (RELIC_COMPILE_INCOMPLETE);

	relic_pred_name_head(compiler->ctx, compiler->rule.head);
	compiler->rule.head_arity = head->arity;

	if (head->arity == 0)
		return (RELIC_COMPILE_OK);

	compiler->rule.head_args = compile_alloc(compiler, head->arity,
	    sizeof(*compiler->rule.head_args));
	if (compiler->rule.head_args == NULL)
		return (RELIC_COMPILE_INCOMPLETE);

	for (i = 0; i < head->arity; i++) {
		result = compile_term(compiler, &head->terms[i],
		    RELIC_POSITION_REQUIRED, &compiler->rule.head_args[i]);
		if (result != RELIC_COMPILE_OK)
			return (result);
	}

	return (RELIC_COMPILE_OK);
}

static enum relic_compile_result
compile_body(relic_compiler_t *compiler)
{
	const relic_ast_stmt_t *statement;
	enum relic_compile_result result;
	size_t i;

	statement = compiler->statement;
	compiler->rule.body_length = statement->body_length;

	if (statement->body_length == 0)
		return (RELIC_COMPILE_OK);

	compiler->rule.body = compile_alloc(compiler, statement->body_length,
	    sizeof(*compiler->rule.body));
	if (compiler->rule.body == NULL)
		return (RELIC_COMPILE_INCOMPLETE);

	for (i = 0; i < statement->body_length; i++) {
		result = compile_literal(compiler, &statement->body[i], i + 1,
		    &compiler->rule.body[i]);
		if (result != RELIC_COMPILE_OK)
			return (result);
	}

	return (RELIC_COMPILE_OK);
}

/*
 * Whether a literal binds a slot B does not hold, given the slots B holds,
 * and which: the variable of an equality relic_comparison_assigns() says
 * assigns it, or the result of an aggregate whose every shared slot B
 * holds.  A relational literal binds by being scanned, which B already
 * counts.
 */
static bool
literal_assigns(const relic_literal_t *literal, uint64_t bound,
    uint16_t *slotp)
{
	const relic_expr_t *value;

	switch (literal->kind) {
	case RELIC_LITERAL_COMPARISON:
		return (relic_comparison_assigns(&literal->u.comparison, bound,
		    slotp, &value));
	case RELIC_LITERAL_AGGREGATE:
		if (!relic_aggregate_binds(&literal->u.aggregate, bound))
			return (false);
		*slotp = literal->u.aggregate.result;
		return (true);
	case RELIC_LITERAL_POSITIVE:
	case RELIC_LITERAL_NEGATED:
		break;
	}

	return (false);
}

/*
 * Close B, the slots the compiled body can bind, under assignment: add the
 * slot of every literal literal_assigns() says binds one, given the slots B
 * holds, until a pass over the body adds nothing.
 *
 * Each pass adds a slot or is the last, so there is at most one pass more
 * than there are slots, and a body with no assignment takes one.  `_` is
 * never added: each occurrence is a distinct variable, and one computed and
 * discarded is no binding anything could read, so it stays unbound and is
 * reported with the message that says so.
 */
static void
bind_by_assignment(relic_compiler_t *compiler)
{
	const relic_literal_t *literal;
	uint16_t slot;
	size_t i;
	bool grew;

	do {
		grew = false;

		for (i = 0; i < compiler->rule.body_length; i++) {
			literal = &compiler->rule.body[i];
			if (!literal_assigns(literal, compiler->bindable,
			    &slot))
				continue;
			if (compiler->variables[slot].anonymous)
				continue;

			compiler->bindable |= UINT64_C(1) << slot;
			grew = true;
		}
	} while (grew);
}

/*
 * The slots the rule reads and nothing can bind.
 *
 * This is the safety verdict of the whole subsystem, and it is one
 * expression: R \ B.  An empty result is a safe rule.
 */
static uint64_t
unbound_variables(const relic_compiler_t *compiler)
{
	return (compiler->required & ~compiler->bindable);
}

/*
 * Report every unbound variable.
 *
 * The messages come out in the order the variables were written, and that is
 * so by construction rather than by sorting.  A variable in R \ B occurs in
 * no positive relational literal of the rule at all -- one inside an
 * aggregate's braces is its group rule's -- and nothing assigns it, so
 * every one of its occurrences is in a position that requires it, and its
 * first occurrence is therefore its first required occurrence.  Slots are
 * assigned in first-occurrence order, so walking the slots walks the
 * offending occurrences from left to right.
 *
 * Every offender is reported rather than only the first.  Each is an
 * independent mistake, unlike the cascade that follows one bad token in a
 * parse, so suppressing the second would hide a real fact about the rule.
 */
static void
report_unbound_variables(relic_compiler_t *compiler, uint64_t unbound)
{
	const relic_variable_t *variable;
	char described[RELIC_STATEMENT_TEXT_MAX];
	char named[RELIC_NAME_TEXT_MAX];
	uint16_t slot;

	if (unbound == 0)
		return;

	describe_statement(compiler, described, sizeof(described));

	for (slot = 0; slot < compiler->variable_count; slot++) {
		if ((unbound & (UINT64_C(1) << slot)) == 0)
			continue;

		variable = &compiler->variables[slot];

		/*
		 * `_` gets a message of its own because the advice differs.
		 * A named variable can be bound by adding a literal; `_`
		 * never can, whatever is added, so telling its writer to add
		 * one would be wrong.
		 */
		if (variable->anonymous) {
			relic_diagf(compiler->ctx, RELIC_ERROR,
			    &variable->where,
			    "anonymous variable in %s cannot be bound by a "
			    "positive body literal", described);
			continue;
		}

		relic_diagf(compiler->ctx, RELIC_ERROR, &variable->where,
		    "variable %s in %s is not bound by a positive body literal "
		    "or an assignment",
		    variable_name(compiler, variable, named, sizeof(named)),
		    described);
	}
}

/*
 * Whether a variable is exempt from the singleton warning.
 *
 * Architecture section 7 names two exemptions: `_`, which is not a named
 * variable at all, and a longer `_`-prefixed name such as `_Unused`, which
 * is how a rule documents a column it deliberately ignores.
 */
static bool
singleton_exempt(const relic_compiler_t *compiler,
    const relic_variable_t *variable)
{
	const char *bytes;

	if (variable->anonymous)
		return (true);

	bytes = relic_symbol_bytes(compiler->ctx, variable->name, NULL);

	return (bytes != NULL && bytes[0] == '_');
}

/*
 * Warn about every variable the rule mentions exactly once.
 *
 * A singleton is the most common Datalog typo, and the check is nearly free
 * once the occurrences have been counted.
 */
static void
report_singleton_variables(relic_compiler_t *compiler, uint64_t unbound)
{
	const relic_variable_t *variable;
	char named[RELIC_NAME_TEXT_MAX];
	uint16_t slot;

	for (slot = 0; slot < compiler->variable_count; slot++) {
		variable = &compiler->variables[slot];

		if (variable->occurrences != 1)
			continue;

		if (singleton_exempt(compiler, variable))
			continue;

		/*
		 * A variable already reported as unbound gets no advice to
		 * rename it.  The error is the message that matters, and a
		 * rename would disguise the bug rather than fix it.
		 */
		if ((unbound & (UINT64_C(1) << slot)) != 0)
			continue;

		variable_name(compiler, variable, named, sizeof(named));

		relic_diagf(compiler->ctx, RELIC_WARNING, &variable->where,
		    "variable %s occurs only once in this rule", named);
		relic_diagf(compiler->ctx, RELIC_NOTE, &variable->where,
		    "rename it to _%s to silence this warning", named);
	}
}

/*
 * Copy the occupied prefix of the slot array into the program arena.
 */
static enum relic_compile_result
store_variables(relic_compiler_t *compiler)
{
	relic_variable_t *stored;

	if (compiler->variable_count == 0)
		return (RELIC_COMPILE_OK);

	stored = compile_alloc(compiler, compiler->variable_count,
	    sizeof(*stored));
	if (stored == NULL)
		return (RELIC_COMPILE_INCOMPLETE);
	bcopy(compiler->variables, stored,
	    compiler->variable_count * sizeof(*stored));

	compiler->rule.variables = stored;
	compiler->rule.variable_count = compiler->variable_count;

	return (RELIC_COMPILE_OK);
}

/*
 * Make room in a rule table for `count` more rules.  Returns 0, or -1
 * having reported that memory ran out, with the table as it was.
 */
int
relic_ruletab_reserve(relic_t *ctx, relic_ruletab_t *table, size_t count)
{
	relic_rule_t *entries;

	if (count > SIZE_MAX - table->count) {
		relic_diag_no_memory(ctx);
		return (-1);
	}

	entries = relic_grow_array(table->entries, &table->capacity,
	    table->count + count, sizeof(*table->entries));
	if (entries == NULL) {
		relic_diag_no_memory(ctx);
		return (-1);
	}
	table->entries = entries;

	return (0);
}

/*
 * Place a finished rule in a rule table that has room for it: the
 * program's, or the one a query's Magic rewrite builds.
 *
 * This is the one way a rule joins a table.  It stores the rule, counts it
 * and marks its head predicate derived, none of which can fail, so a
 * predicate is derived exactly when some table has held a rule deriving it,
 * and the mark needs no other place to be kept in step with.  A rule table
 * is indexed by its heads' predicates wherever it is analysed, so a query's
 * rule, whose head names none, never joins one.
 */
void
relic_ruletab_place(relic_t *ctx, relic_ruletab_t *table,
    const relic_rule_t *rule)
{
	assert(table->count < table->capacity);
	assert(rule->head != RELIC_ID_NONE);

	table->entries[table->count] = *rule;
	table->count++;
	relic_pred_mark_derived(ctx, rule->head);
}

/*
 * Append one finished rule to a rule table.  Returns 0, or -1 having
 * reported that memory ran out.
 */
int
relic_ruletab_append(relic_t *ctx, relic_ruletab_t *table,
    const relic_rule_t *rule)
{
	if (relic_ruletab_reserve(ctx, table, 1) != 0)
		return (-1);

	relic_ruletab_place(ctx, table, rule);

	return (0);
}

/*
 * Publish a program statement's rules: its group rules in body order, then
 * its rule, each with the rule's position among the program's rules as its
 * origin, and each counted against its head predicate.  This is the last
 * step of compiling a statement, and room for every rule is made before
 * the first is placed, so a statement is published whole or, when memory
 * runs out, not at all, and nothing incomplete is ever reachable through
 * the program.
 */
static enum relic_compile_result
append_program_rules(relic_t *ctx, relic_compiled_statement_t *compiled)
{
	relic_ruletab_t *table;
	size_t origin;
	size_t i;

	table = &ctx->program.rules;

	if (relic_ruletab_reserve(ctx, table,
	    compiled->group_rule_count + 1) != 0)
		return (RELIC_COMPILE_INCOMPLETE);

	origin = table->count + compiled->group_rule_count;

	for (i = 0; i < compiled->group_rule_count; i++) {
		compiled->group_rules[i].origin = origin;
		relic_ruletab_place(ctx, table, &compiled->group_rules[i]);
		relic_pred_count_program_rule(ctx,
		    compiled->group_rules[i].head);
	}

	compiled->rule.origin = origin;
	relic_ruletab_place(ctx, table, &compiled->rule);
	relic_pred_count_program_rule(ctx, compiled->rule.head);

	return (RELIC_COMPILE_OK);
}

/*
 * Mark the group predicates of a query's aggregates derived as the query is
 * published.  A query's group rules join no rule table -- the query holds
 * them and runs them itself -- so this is the mark relic_ruletab_place()
 * would have made.
 */
static void
mark_group_predicates_derived(relic_t *ctx,
    const relic_compiled_statement_t *compiled)
{
	size_t i;

	for (i = 0; i < compiled->group_rule_count; i++)
		relic_pred_mark_derived(ctx, compiled->group_rules[i].head);
}

/*
 * Publish a finished query into the program's queries, under the serial
 * number that names its answer, as the last step of compiling it.
 */
static enum relic_compile_result
append_loaded_query(relic_t *ctx, const relic_compiled_statement_t *compiled,
    size_t serial)
{
	relic_querytab_t *table;
	relic_loaded_query_t *entries;
	relic_loaded_query_t *entry;

	table = &ctx->program.queries;

	entries = relic_grow_array(table->entries, &table->capacity,
	    table->count + 1, sizeof(*table->entries));
	if (entries == NULL) {
		relic_diag_no_memory(ctx);
		return (RELIC_COMPILE_INCOMPLETE);
	}
	table->entries = entries;

	entry = &table->entries[table->count];
	bzero(entry, sizeof(*entry));
	entry->rule = compiled->rule;
	entry->group_rules = compiled->group_rules;
	entry->group_rule_count = compiled->group_rule_count;
	entry->serial = serial;
	table->count++;
	mark_group_predicates_derived(ctx, compiled);

	return (RELIC_COMPILE_OK);
}

static void
compiler_init(relic_compiler_t *compiler, relic_t *ctx, relic_arena_t *arena,
    const relic_ast_stmt_t *statement)
{
	bzero(compiler, sizeof(*compiler));
	compiler->ctx = ctx;
	compiler->arena = arena;
	compiler->statement = statement;
	compiler->rule.where = statement->where;
	compiler->rule.origin = RELIC_ORIGIN_QUERY;
}

/*
 * Compile one statement into `arena`, and store its rules in *compiled
 * without publishing them.
 *
 * A statement holding aggregates is read twice: once to classify its
 * variables as the rule's or an aggregate's own, which depends on text
 * that may come after an occurrence, and once to compile it.  Every other
 * statement is compiled in the one pass.
 */
static enum relic_compile_result
compile_statement_rules(relic_t *ctx, relic_arena_t *arena,
    const relic_ast_stmt_t *statement, relic_compiled_statement_t *compiled)
{
	relic_compiler_t compiler;
	relic_scopes_t scopes;
	enum relic_compile_result result;

	compiler_init(&compiler, ctx, arena, statement);

	bzero(&scopes, sizeof(scopes));
	if (count_aggregates(statement) > 0) {
		result = classify_scopes(ctx, statement, &scopes);
		if (result != RELIC_COMPILE_OK)
			return (result);
		compiler.scopes = &scopes;
	}

	if (statement->kind == RELIC_STMT_QUERY)
		result = compile_query(&compiler, compiled);
	else
		result = compile_rule(&compiler, compiled);

	free(scopes.entries);

	return (result);
}

/*
 * Check a rule whose head and body are compiled, compile its aggregates'
 * group rules, and store the finished rules in *compiled.
 *
 * Singletons are reported even for a rule that is being rejected, because a
 * rule can be wrong in both ways at once and one run should say so, and the
 * group rules are compiled and checked for the same reason.
 */
static enum relic_compile_result
finish_rule(relic_compiler_t *compiler, relic_compiled_statement_t *compiled)
{
	enum relic_compile_result groups;
	enum relic_compile_result result;
	uint64_t unbound;

	bind_by_assignment(compiler);
	unbound = unbound_variables(compiler);
	report_unbound_variables(compiler, unbound);
	report_singleton_variables(compiler, unbound);

	groups = compile_group_rules(compiler);
	if (groups == RELIC_COMPILE_INCOMPLETE)
		return (groups);

	if (unbound != 0 || groups != RELIC_COMPILE_OK)
		return (RELIC_COMPILE_REJECTED);

	result = store_variables(compiler);
	if (result != RELIC_COMPILE_OK)
		return (result);

	compiled->rule = compiler->rule;
	compiled->group_rules = compiler->group_rules;
	compiled->group_rule_count = compiler->group_rule_count;

	return (RELIC_COMPILE_OK);
}

/*
 * Compile one rule or fact.
 *
 * The rule is built as it is analysed, in one pass, because everything whose
 * length the build needs is known before the walk begins: the head's arity,
 * the body's length and each atom's arity all come from the tree, and only
 * the variable count is discovered along the way, into a fixed array.  A
 * separate analysis pass would have to look each variable's slot up again by
 * name during the build, and that lookup could not fail for any reason a
 * reader could name -- which is exactly the kind of unreachable branch worth
 * designing out.
 *
 * What the single pass costs is the arena memory of a statement that turns
 * out to be rejected.  It is bounded by the statement, spent once for the
 * life of the context, and spent only on a program that will not be
 * evaluated anyway.
 */
static enum relic_compile_result
compile_rule(relic_compiler_t *compiler, relic_compiled_statement_t *compiled)
{
	enum relic_compile_result result;

	result = compile_head(compiler);
	if (result != RELIC_COMPILE_OK)
		return (result);

	result = compile_body(compiler);
	if (result != RELIC_COMPILE_OK)
		return (result);

	return (finish_rule(compiler, compiled));
}

/*
 * Build the head of a query's rule: one argument for every named variable, in
 * slot order, which is the order of first occurrence.  `_` contributes no
 * column.  In a positive or a negated literal it is a wildcard with no slot
 * at all, and a slot it takes anywhere else belongs to a query about to be
 * rejected as unsafe.
 *
 * The head counts as one occurrence of each variable it names and requires
 * each one to be bound, exactly as a written head would.  That is what makes
 * "every output column is bound" the ordinary safety check, and what keeps
 * `path(a, X)?` free of a singleton warning: X is written once, but the rule
 * the text means mentions it twice.
 *
 * The head names no predicate: `head` stays RELIC_ID_NONE, and the number of
 * output columns is the head arity.  Each answer is a tuple of a relation, so
 * a query can have at most as many output columns as a relation has columns,
 * although a rule may hold twice as many variables.
 */
static enum relic_compile_result
compile_query_head(relic_compiler_t *compiler)
{
	relic_variable_t *variable;
	uint16_t column;
	uint16_t columns;
	uint16_t slot;

	columns = 0;
	for (slot = 0; slot < compiler->variable_count; slot++) {
		variable = &compiler->variables[slot];
		if (variable->anonymous)
			continue;

		if (columns == RELIC_MAX_ARITY) {
			relic_diagf(compiler->ctx, RELIC_ERROR,
			    &variable->where,
			    "the query has more than %d output columns",
			    RELIC_MAX_ARITY);
			return (RELIC_COMPILE_REJECTED);
		}
		columns++;
	}

	compiler->rule.head = RELIC_ID_NONE;
	compiler->rule.head_arity = columns;
	if (columns == 0)
		return (RELIC_COMPILE_OK);

	compiler->rule.head_args = compile_alloc(compiler, columns,
	    sizeof(*compiler->rule.head_args));
	if (compiler->rule.head_args == NULL)
		return (RELIC_COMPILE_INCOMPLETE);

	column = 0;
	for (slot = 0; slot < compiler->variable_count; slot++) {
		variable = &compiler->variables[slot];
		if (variable->anonymous)
			continue;

		bzero(&compiler->rule.head_args[column],
		    sizeof(compiler->rule.head_args[column]));
		compiler->rule.head_args[column].kind = RELIC_ARG_VARIABLE;
		compiler->rule.head_args[column].u.slot = slot;
		compiler->required |= UINT64_C(1) << slot;
		variable->occurrences++;
		column++;
	}

	return (RELIC_COMPILE_OK);
}

/*
 * Compile a query into a rule whose head names no predicate.
 *
 * The body is compiled first, because the head is made of the body's named
 * variables and is not known until the body has numbered them.  Safety is
 * then the same computation as for any rule.
 */
static enum relic_compile_result
compile_query(relic_compiler_t *compiler,
    relic_compiled_statement_t *compiled)
{
	enum relic_compile_result result;

	result = compile_body(compiler);
	if (result != RELIC_COMPILE_OK)
		return (result);

	result = compile_query_head(compiler);
	if (result != RELIC_COMPILE_OK)
		return (result);

	return (finish_rule(compiler, compiled));
}

/*
 * Compile one statement of the program and publish the result: a rule or
 * fact, with its group rules, into the program's rules, and a query into
 * its queries.
 *
 * A query's serial number advances only once the query is published, so a
 * query that runs out of memory part way is compiled again under the same
 * name rather than skipping one.
 */
static enum relic_compile_result
compile_statement(relic_t *ctx, const relic_ast_stmt_t *statement)
{
	relic_compiled_statement_t compiled;
	enum relic_compile_result result;

	result = compile_statement_rules(ctx, &ctx->program_arena, statement,
	    &compiled);
	if (result != RELIC_COMPILE_OK)
		return (result);

	if (statement->kind == RELIC_STMT_RULE)
		return (append_program_rules(ctx, &compiled));

	result = append_loaded_query(ctx, &compiled, next_query_serial(ctx));
	if (result != RELIC_COMPILE_OK)
		return (result);

	publish_query_number(ctx);

	return (RELIC_COMPILE_OK);
}

/*
 * The serial number of the query being published: how many queries this
 * context has published before it.  Identical sequences of loads and
 * preparations therefore number their queries identically.
 */
static size_t
next_query_serial(const relic_t *ctx)
{
	return (ctx->program.query_serial);
}

/*
 * Record that a compiled query has been published under the number
 * next_query_serial() gave it, so that the next one takes the next number.
 *
 * A query is published by one of two routes -- appended to the program by a
 * check, or handed to the caller by relic_query_prepare() -- and both end
 * here, after the last step that can fail.  A query that fails before this
 * is compiled again under the same number, so a failure part way skips none.
 */
static void
publish_query_number(relic_t *ctx)
{
	ctx->program.query_serial++;
}

/*
 * The first statement no check has compiled yet, or NULL when there is none.
 *
 * The cursor is the last statement compiled rather than a count, so resuming
 * costs nothing proportional to what is already done.  The pointer stays
 * valid because statements live in the program arena and are never freed
 * while the context does.
 */
static const relic_ast_stmt_t *
first_pending_statement(const relic_t *ctx)
{
	if (ctx->program.compiled == NULL)
		return (TAILQ_FIRST(&ctx->ast.statements));

	return (TAILQ_NEXT(ctx->program.compiled, link));
}

/*
 * Compile every statement the program has not seen yet.
 *
 * Returns false when the library could not finish, leaving the cursor on the
 * statement it stopped at so that a later check resumes there.  A rejected
 * statement advances the cursor and compilation continues, so one run
 * reports everything wrong with the program rather than stopping at the
 * first bad rule.
 */
static bool
compile_pending_statements(relic_t *ctx)
{
	relic_program_t *program;
	const relic_ast_stmt_t *statement;
	enum relic_compile_result result;

	program = &ctx->program;

	for (statement = first_pending_statement(ctx); statement != NULL;
	    statement = TAILQ_NEXT(statement, link)) {
		result = compile_statement(ctx, statement);

		if (result == RELIC_COMPILE_INCOMPLETE)
			return (false);

		if (result == RELIC_COMPILE_REJECTED)
			program->invalid = true;

		program->compiled = statement;
	}

	return (true);
}

int
relic_check(relic_t *ctx)
{
	int status;

	if (relic_enter(ctx, NULL, NULL) != 0)
		return (-1);

	status = check_program(ctx);

	relic_leave(ctx);

	return (status);
}

/*
 * relic_check() inside an operation already entered: compile what was loaded
 * since the last check, then analyse the program and sweep for undefined
 * predicates.  A check runs alone and at the start of every operation that
 * uses the program, so it names itself to the memory limit for as long as
 * it runs and then gives the operation its own activity back.
 */
static int
check_program(relic_t *ctx)
{
	relic_activity_t operation;
	int status;

	relic_diag_save_activity(ctx, &operation);
	relic_diag_activity(ctx, "checking", "the program");

	status = compile_and_analyse(ctx);

	relic_diag_restore_activity(ctx, &operation);

	return (status);
}

/*
 * The body of check_program().
 */
static int
compile_and_analyse(relic_t *ctx)
{
	bool completed;

	completed = compile_pending_statements(ctx);

	/*
	 * A refusal by the program arena is reported here rather than where
	 * it happened, so that one exhaustion produces one message however
	 * many allocations a statement was part way through.  The flag is
	 * cleared once it has been reported: a check is the phase it belongs
	 * to, and leaving it set would make one transient exhaustion refuse
	 * every later check for the life of the context.
	 */
	if (relic_arena_failed(&ctx->program_arena)) {
		relic_diag_no_memory(ctx);
		relic_arena_clear_failure(&ctx->program_arena);
	}

	/*
	 * The dependency analysis and the undefined-predicate sweep are
	 * skipped when compilation stopped early, because the answer to each
	 * depends on having seen every rule and some of them are in the
	 * statements that were not reached.  The analysis, which can run out
	 * of memory, goes first, so that a check that fails for want of memory
	 * has answered neither question and the check that retries it answers
	 * both.
	 *
	 * The analysis runs even when a statement was rejected.  It sees only
	 * the rules that were accepted, and a cycle through negation among
	 * those is a mistake no later load can repair, so reporting it now
	 * saves the reader a second round.
	 */
	if (!completed)
		return (-1);

	if (relic_stratify(ctx) != 0)
		return (-1);

	relic_pred_report_undefined(ctx);

	return (ctx->program.invalid ? -1 : 0);
}

/*
 * Check the program before it is used, and say so when it cannot be.
 *
 * relic_check() reports each mistake once and then refuses the program in
 * silence, which is right for a check -- repeating a message says nothing new
 * -- and wrong for an operation that fails because of it: every failure must
 * report at least one error, or a caller is left with -1 and no reason.  So
 * when a check fails having reported nothing this time, the reason is given
 * here as a pointer to the messages that came before.
 */
int
relic_program_ready(relic_t *ctx)
{
	size_t errors_before;

	errors_before = ctx->diag.error_count;

	if (check_program(ctx) == 0)
		return (0);

	if (ctx->diag.error_count == errors_before)
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "the program cannot be used: its errors were reported "
		    "when it was checked");

	return (-1);
}

/*
 * A query object with no rule and no answer yet.  Returns NULL having
 * reported when memory ran out.
 */
static relic_query_t *
query_create(relic_t *ctx)
{
	relic_query_t *query;

	query = calloc(1, sizeof(*query));
	if (query == NULL) {
		relic_diag_no_memory(ctx);
		return (NULL);
	}

	query->ctx = ctx;
	relic_arena_init(&query->arena, RELIC_ARENA_ANY_OBJECT);

	return (query);
}

/*
 * Give a query whose rule is compiled the relation its answer is derived
 * into: of the rule's head arity, and named after the query's serial number.
 * Returns 0, or -1 having reported when memory ran out.
 */
static int
query_create_answer(relic_query_t *query, size_t serial)
{
	query->answer = relic_relation_create(query->ctx, RELIC_ID_NONE, serial,
	    query->rule.head_arity);

	return (query->answer != NULL ? 0 : -1);
}

/*
 * Prepare a query from text: parse it, compile it into a rule, warn about any
 * predicate it names that nothing defines, and give it its answer relation.
 *
 * The program is checked first, and must pass.  A query against a program
 * that cannot be evaluated could never be run, and the undefined-predicate
 * sweep that follows compilation would otherwise report the program's own
 * predicates before the program had been compiled far enough to define them.
 * With the program checked, the sweep speaks only about predicates the query
 * introduced.
 *
 * A query that is wrong is reported and refused, and the program is not
 * affected: it stays as valid as it was.  The answer relation is created
 * only once the rule has compiled, so a refused query creates none, and
 * before the query is published, so one handed out always has one.
 */
relic_query_t *
relic_query_prepare(relic_t *ctx, const char *source_name, const char *text)
{
	relic_query_t *query;

	if (relic_enter(ctx, "preparing",
	    source_name != NULL ? source_name : RELIC_QUERY_SOURCE) != 0)
		return (NULL);

	query = prepare_query(ctx, source_name, text);

	relic_leave(ctx);

	return (query);
}

static relic_query_t *
prepare_query(relic_t *ctx, const char *source_name, const char *text)
{
	relic_compiled_statement_t compiled;
	relic_query_t *query;
	const relic_ast_stmt_t *statement;
	enum relic_compile_result result;

	if (text == NULL) {
		relic_diagf(ctx, RELIC_ERROR, NULL, "no query text was given");
		return (NULL);
	}

	if (relic_program_ready(ctx) != 0)
		return (NULL);

	query = query_create(ctx);
	if (query == NULL)
		return (NULL);

	if (source_name == NULL)
		source_name = RELIC_QUERY_SOURCE;

	if (relic_parse_query(ctx, &query->arena, source_name, text,
	    &statement) != 0) {
		relic_query_destroy(query);
		return (NULL);
	}

	result = compile_statement_rules(ctx, &query->arena, statement,
	    &compiled);

	/*
	 * A refusal by the query's arena was not reported where it happened,
	 * for the same reason relic_check() reports the program arena's at
	 * its boundary: one exhaustion, one message.
	 */
	if (relic_arena_failed(&query->arena))
		relic_diag_no_memory(ctx);

	/*
	 * A query that is refused has still interned the predicates it names,
	 * and one of them may be the typo that explains the refusal, so the
	 * sweep runs for it too.  It does not run for a query that ran out of
	 * memory, which may not have reached all of its predicates.
	 */
	if (result != RELIC_COMPILE_INCOMPLETE)
		relic_pred_report_undefined(ctx);

	if (result != RELIC_COMPILE_OK) {
		relic_query_destroy(query);
		return (NULL);
	}

	query->rule = compiled.rule;
	query->group_rules = compiled.group_rules;
	query->group_rule_count = compiled.group_rule_count;
	if (query_create_answer(query, next_query_serial(ctx)) != 0) {
		relic_query_destroy(query);
		return (NULL);
	}

	mark_group_predicates_derived(ctx, &compiled);
	publish_query_number(ctx);

	return (query);
}

size_t
relic_query_count(relic_t *ctx)
{
	return (ctx->ast.query_count);
}

/*
 * A query written in the program, by its position among the program's
 * queries in load order, with an answer relation of its own named by the
 * serial number the check gave the query.
 *
 * The program is checked first, because that is what compiles its queries.
 * Once a check has passed, every query statement has been compiled -- a
 * query that could not be would have made the check fail -- so an index below
 * relic_query_count() always names one.
 */
relic_query_t *
relic_query_loaded(relic_t *ctx, size_t index)
{
	relic_query_t *query;

	if (relic_enter(ctx, NULL, NULL) != 0)
		return (NULL);

	query = copy_loaded_query(ctx, index);

	relic_leave(ctx);

	return (query);
}

static relic_query_t *
copy_loaded_query(relic_t *ctx, size_t index)
{
	const relic_querytab_t *queries;
	relic_query_t *query;

	if (relic_program_ready(ctx) != 0)
		return (NULL);

	queries = &ctx->program.queries;
	if (index >= queries->count) {
		relic_diagf(ctx, RELIC_ERROR, NULL,
		    "no query at index %zu: the program's queries number %zu",
		    index, queries->count);
		return (NULL);
	}

	query = query_create(ctx);
	if (query == NULL)
		return (NULL);

	query->rule = queries->entries[index].rule;
	query->group_rules = queries->entries[index].group_rules;
	query->group_rule_count = queries->entries[index].group_rule_count;
	if (query_create_answer(query, queries->entries[index].serial) != 0) {
		relic_query_destroy(query);
		return (NULL);
	}

	return (query);
}

/*
 * Only the query being answered is refused, from whatever caller code runs
 * while it is; any other is released without reading its context, which
 * may be gone.  Its answer relation is empty, since a query holds tuples
 * only while it is being answered, so freeing it gives nothing back to the
 * context's totals and needs nothing of the context.
 */
void
relic_query_destroy(relic_query_t *query)
{
	if (query == NULL)
		return;

	if (query->running) {
		relic_diag_record_refusal(query->ctx);
		return;
	}

	relic_relation_free(query->answer);
	relic_arena_reset(&query->arena);
	free(query);
}
