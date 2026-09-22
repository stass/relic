/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdlib.h>
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
 * Safety is one set computation rather than a search.  Let B be the slots
 * occurring in an argument of a positive relational body literal, and R the
 * slots occurring in the head, in a negated literal or in a comparison.  The
 * rule is safe exactly when R \ B is empty, and the offending variables are
 * precisely its members.  No reordering search is needed, because a positive
 * literal needs nothing bound in order to be scanned: every positive literal
 * can always be scheduled before every negated literal and every comparison,
 * so if a binding order exists at all, that one is such an order.
 *
 * Compilation is incremental, and that is what makes checking cached.  The
 * syntax tree is append-only, so the statements behind the program's cursor
 * can never change; a check compiles from the cursor to the end.  A check
 * with nothing new to do therefore compiles nothing and says nothing, each
 * statement is compiled once for the life of the context, and each
 * diagnostic is emitted once rather than repeated by every later check.
 */

/*
 * Where an argument sits, which is the whole of what safety turns on.
 *
 * A positive relational body literal binds its arguments.  Everywhere else
 * -- the head, a negated literal, a comparison -- an argument is read and
 * must already have been bound by one of those.
 */
enum relic_arg_position {
	RELIC_POSITION_BINDING,
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

typedef struct relic_compiler relic_compiler_t;

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
 * this file, and are the whole of that analysis.  Nothing here records its
 * verdict, because the verdict is `required & ~bindable`, and a stored copy
 * of a derived fact is a second thing to keep in step.
 */
struct relic_compiler {
	relic_t			*ctx;
	const relic_ast_stmt_t	*statement;
	relic_rule_t		 rule;
	relic_variable_t	 variables[RELIC_MAX_VARIABLES];
	uint64_t		 bindable;	/* the set B */
	uint64_t		 required;	/* the set R */
	uint16_t		 variable_count;
};

static enum relic_compile_result append_rule(relic_compiler_t *compiler);
static void	*compile_alloc(relic_compiler_t *compiler, size_t count,
		    size_t size);
static enum relic_compile_result compile_atom(relic_compiler_t *compiler,
		    const relic_ast_atom_t *source,
		    enum relic_arg_position position, relic_atom_t *atom);
static enum relic_compile_result compile_body(relic_compiler_t *compiler);
static enum relic_compile_result compile_comparison(
		    relic_compiler_t *compiler,
		    const relic_ast_comparison_t *source,
		    relic_comparison_t *comparison);
static enum relic_compile_result compile_head(relic_compiler_t *compiler);
static enum relic_compile_result compile_literal(relic_compiler_t *compiler,
		    const relic_ast_literal_t *source,
		    relic_literal_t *literal);
static bool	 compile_pending_statements(relic_t *ctx);
static enum relic_compile_result compile_rule(relic_t *ctx,
		    const relic_ast_stmt_t *statement);
static enum relic_compile_result compile_term(relic_compiler_t *compiler,
		    const relic_ast_term_t *term,
		    enum relic_arg_position position, relic_arg_t *arg);
static const relic_ast_stmt_t *first_pending_statement(const relic_t *ctx);
static enum relic_compile_result occupy_slot(relic_compiler_t *compiler,
		    const relic_ast_variable_t *variable,
		    const relic_location_t *where, uint16_t *slotp);
static enum relic_compile_result register_query(relic_t *ctx,
		    const relic_ast_stmt_t *statement);
static void	 report_singleton_variables(relic_compiler_t *compiler,
		    uint64_t unbound);
static void	 report_unbound_variables(relic_compiler_t *compiler,
		    uint64_t unbound);
static void	 report_variable_limit(relic_compiler_t *compiler,
		    const relic_location_t *where);
static bool	 singleton_exempt(const relic_compiler_t *compiler,
		    const relic_variable_t *variable);
static const char *statement_noun(const relic_compiler_t *compiler);
static enum relic_compile_result store_variables(relic_compiler_t *compiler);
static uint64_t	 unbound_variables(const relic_compiler_t *compiler);
static const char *variable_name(const relic_compiler_t *compiler,
		    const relic_variable_t *variable, char *buffer,
		    size_t size);

void
relic_program_init(relic_program_t *program)
{
	bzero(program, sizeof(*program));
	relic_predtab_init(&program->predicates);
}

void
relic_program_reset(relic_program_t *program)
{
	/*
	 * The variable-length parts of every rule belong to the program
	 * arena, which releases them when it is reset.  Only the array of
	 * rules is this module's to free.
	 */
	relic_predtab_reset(&program->predicates);
	free(program->rules.entries);
	bzero(program, sizeof(*program));
}

/*
 * Allocate `count` items of `size` bytes for the rule being built.
 *
 * A null return means the program arena has been marked failed, and
 * relic_check() reports that once at its boundary.  A call site therefore
 * answers RELIC_COMPILE_INCOMPLETE without reporting for itself, so one
 * exhaustion produces one message however many allocations a statement was
 * part way through.
 *
 * The product is checked rather than assumed, because a wrapped product
 * would be answered with a block smaller than what is written into it.  That
 * rejection marks the arena rather than merely returning null, so it reaches
 * the boundary by the same path an exhausted arena does.
 */
static void *
compile_alloc(relic_compiler_t *compiler, size_t count, size_t size)
{
	relic_arena_t *arena;

	arena = &compiler->ctx->program_arena;

	if (count > SIZE_MAX / size) {
		relic_arena_fail(arena);
		return (NULL);
	}

	return (relic_arena_alloc(arena, count * size));
}

/*
 * What a message calls the statement: what its writer wrote.  The tree
 * represents a fact as a rule whose body has no literals, and only the
 * wording distinguishes them.
 *
 * The answer is read from the statement rather than from the rule being
 * built, so that it is available from the first line of compilation and does
 * not depend on how far the build has got.
 */
static const char *
statement_noun(const relic_compiler_t *compiler)
{
	if (compiler->statement->body_length == 0)
		return ("fact");

	return ("rule");
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
	char described[RELIC_PRED_TEXT_MAX];

	relic_diagf(compiler->ctx, RELIC_ERROR, where,
	    "%s %s uses more than %d distinct variables",
	    statement_noun(compiler),
	    relic_pred_describe(compiler->ctx, compiler->rule.head, described,
	    sizeof(described)), RELIC_MAX_VARIABLES);
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
	 * `_` in an argument of a positive body literal is a wildcard: it
	 * binds nothing and nothing reads it, so a slot for it would be a
	 * frame entry counting against the limit of 64 for no reason a reader
	 * could see.  A rule scanning two wide relations with nothing but `_`
	 * would otherwise exceed the limit without holding a single variable.
	 *
	 * Everywhere else `_` can never be bound, so it does take a slot, and
	 * the one computation below reports it alongside the named variables
	 * and in the same textual order.  The rule is always rejected, so
	 * such a slot never reaches the evaluator.
	 */
	if (variable->anonymous && position == RELIC_POSITION_BINDING) {
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
 * A comparison is a filter: it reads both operands and binds neither, so
 * both are required positions.
 */
static enum relic_compile_result
compile_comparison(relic_compiler_t *compiler,
    const relic_ast_comparison_t *source, relic_comparison_t *comparison)
{
	enum relic_compile_result result;

	bzero(comparison, sizeof(*comparison));
	comparison->op = source->op;

	result = compile_term(compiler, &source->left,
	    RELIC_POSITION_REQUIRED, &comparison->left);
	if (result != RELIC_COMPILE_OK)
		return (result);

	return (compile_term(compiler, &source->right,
	    RELIC_POSITION_REQUIRED, &comparison->right));
}

static enum relic_compile_result
compile_literal(relic_compiler_t *compiler,
    const relic_ast_literal_t *source, relic_literal_t *literal)
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
		    RELIC_POSITION_REQUIRED, &literal->u.atom));
	case RELIC_LITERAL_COMPARISON:
		return (compile_comparison(compiler, &source->u.comparison,
		    &literal->u.comparison));
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
 * Compile the head, and record that something derives into its predicate.
 *
 * The predicate is marked defined even when the statement goes on to be
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

	relic_pred_define(compiler->ctx, compiler->rule.head);

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
		result = compile_literal(compiler, &statement->body[i],
		    &compiler->rule.body[i]);
		if (result != RELIC_COMPILE_OK)
			return (result);
	}

	return (RELIC_COMPILE_OK);
}

/*
 * The slots the rule reads and no positive body literal binds.
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
 * no positive relational body literal at all, so every one of its
 * occurrences is in a position that requires it, and its first occurrence is
 * therefore its first required occurrence.  Slots are assigned in
 * first-occurrence order, so walking the slots walks the offending
 * occurrences from left to right.
 *
 * Every offender is reported rather than only the first.  Each is an
 * independent mistake, unlike the cascade that follows one bad token in a
 * parse, so suppressing the second would hide a real fact about the rule.
 */
static void
report_unbound_variables(relic_compiler_t *compiler, uint64_t unbound)
{
	const relic_variable_t *variable;
	char described[RELIC_PRED_TEXT_MAX];
	char named[RELIC_NAME_TEXT_MAX];
	uint16_t slot;

	if (unbound == 0)
		return;

	relic_pred_describe(compiler->ctx, compiler->rule.head, described,
	    sizeof(described));

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
			    "anonymous variable in %s %s cannot be bound by a "
			    "positive body literal",
			    statement_noun(compiler), described);
			continue;
		}

		relic_diagf(compiler->ctx, RELIC_ERROR, &variable->where,
		    "variable %s in %s %s is not bound by a positive body "
		    "literal",
		    variable_name(compiler, variable, named, sizeof(named)),
		    statement_noun(compiler), described);
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
 * Publish the finished rule.  This is the last step, so nothing incomplete
 * is ever reachable through the program.
 */
static enum relic_compile_result
append_rule(relic_compiler_t *compiler)
{
	relic_ruletab_t *table;
	relic_rule_t *entries;

	table = &compiler->ctx->program.rules;

	entries = relic_grow_array(table->entries, &table->capacity,
	    table->count + 1, sizeof(*table->entries));
	if (entries == NULL) {
		relic_diag_no_memory(compiler->ctx);
		return (RELIC_COMPILE_INCOMPLETE);
	}
	table->entries = entries;

	table->entries[table->count] = compiler->rule;
	table->count++;

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
 *
 * Singletons are reported even for a rule that is being rejected, because a
 * rule can be wrong in both ways at once and one run should say so.
 */
static enum relic_compile_result
compile_rule(relic_t *ctx, const relic_ast_stmt_t *statement)
{
	relic_compiler_t compiler;
	enum relic_compile_result result;
	uint64_t unbound;

	bzero(&compiler, sizeof(compiler));
	compiler.ctx = ctx;
	compiler.statement = statement;
	compiler.rule.where = statement->where;

	result = compile_head(&compiler);
	if (result != RELIC_COMPILE_OK)
		return (result);

	result = compile_body(&compiler);
	if (result != RELIC_COMPILE_OK)
		return (result);

	unbound = unbound_variables(&compiler);
	report_unbound_variables(&compiler, unbound);
	report_singleton_variables(&compiler, unbound);

	if (unbound != 0)
		return (RELIC_COMPILE_REJECTED);

	result = store_variables(&compiler);
	if (result != RELIC_COMPILE_OK)
		return (result);

	return (append_rule(&compiler));
}

/*
 * Intern the predicates a query mentions, and nothing else.
 *
 * Predicate identity is a property of the program rather than of a rule, so
 * a query's atoms are interned like any other and a typo in a query is
 * caught by the undefined-predicate warning.  The rest of a query is not
 * compiled here: architecture section 11 turns it into a rule over a
 * synthetic predicate whose arguments are its output columns, and that
 * transformation belongs with the planner that consumes it.  A query's
 * variables are numbered and its safety is decided when that rule is built,
 * by this same code, because "every head variable is bound" is exactly
 * "every output column is bound".
 */
static enum relic_compile_result
register_query(relic_t *ctx, const relic_ast_stmt_t *statement)
{
	const relic_ast_literal_t *literal;
	relic_pred_id_t pred;
	size_t i;

	for (i = 0; i < statement->body_length; i++) {
		literal = &statement->body[i];

		if (literal->kind == RELIC_LITERAL_COMPARISON)
			continue;

		if (relic_pred_intern(ctx, literal->u.atom.name,
		    literal->u.atom.arity, &literal->u.atom.where,
		    &pred) != 0)
			return (RELIC_COMPILE_INCOMPLETE);
	}

	return (RELIC_COMPILE_OK);
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
		if (statement->kind == RELIC_STMT_QUERY)
			result = register_query(ctx, statement);
		else
			result = compile_rule(ctx, statement);

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
	 * The undefined-predicate sweep is skipped when compilation stopped
	 * early, because its answer depends on having seen every definition
	 * and some of them are in the statements that were not reached.
	 */
	if (!completed)
		return (-1);

	relic_pred_report_undefined(ctx);

	return (ctx->program.invalid ? -1 : 0);
}
