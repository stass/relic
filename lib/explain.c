/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "internal.h"

/*
 * Explain: the plans evaluation runs, written as text for a person.
 *
 * Every line is built in a fixed buffer on the stack and handed to the
 * installed callback, so that explaining a plan cannot fail the evaluation
 * it describes.  A line that would not fit is cut, and its last three bytes
 * become "..." so that the cut is visible.
 *
 * A line holds only printable ASCII.  Names and constants come from the
 * program, and a string constant can hold any byte at all, so every byte
 * outside printable ASCII is written as the \xHH escape the language already
 * has, and inside a string literal the backslash and the double quote take
 * their escapes too.  No program can send a terminal escape sequence through
 * its plan, and the callback need not escape anything.
 *
 * An operation is written as its kind followed by the literal it reads, the
 * way a program would write it: its relation's name, and its arguments the
 * operation's terms, a variable named as the rule names the slot it reads or
 * binds.  What is shown is therefore the plan itself, not the rule it was
 * built from.  A relation is named by the relation layer, which is what
 * names a query's answer `$q` and the query's serial number; a query's rule
 * is written with its head named the same way, so the head and the EMIT
 * that writes it read alike.
 */

#define	EXPLAIN_LINE_MAX	1024	/* including the terminating NUL */
#define	EXPLAIN_INDENT		4	/* spaces per level of nesting */

typedef struct relic_explain_line relic_explain_line_t;

/*
 * A line under construction.  `length` bytes of `text` are written and
 * `text` always has room for the terminating NUL.  `truncated` is set once a
 * byte did not fit, after which nothing more is written.
 */
struct relic_explain_line {
	char	text[EXPLAIN_LINE_MAX];
	size_t	length;
	bool	truncated;
};

static void	 begin_variant_heading(const relic_t *ctx,
		    relic_explain_line_t *line, const relic_rule_t *rule,
		    size_t delta_literal, unsigned int depth);
static unsigned int binds_as_tightly(const relic_expr_t *expr);
static void	 finish_heading(const relic_t *ctx, relic_explain_line_t *line);
static void	 line_begin(relic_explain_line_t *line, unsigned int depth);
static void	 line_finish(const relic_t *ctx, relic_explain_line_t *line);
static void	 line_put(relic_explain_line_t *line, const char *text);
static void	 line_put_byte(relic_explain_line_t *line, char byte);
static void	 line_put_escaped(relic_explain_line_t *line,
		    unsigned char byte);
static bool	 negation_needs_parentheses(const relic_t *ctx,
		    const relic_expr_t *operand);
static void	 put_adorn(const relic_t *ctx, relic_explain_line_t *line,
		    relic_pred_id_t source, uint32_t bound);
static void	 put_adorn_line(relic_t *ctx,
		    const relic_adornment_t *adornment);
static void	 put_adorn_lines(relic_t *ctx, const relic_magic_t *magic);
static void	 put_adorned_pred(const relic_t *ctx,
		    relic_explain_line_t *line, relic_pred_id_t source,
		    uint32_t bound);
static void	 put_adornment(const relic_t *ctx, relic_explain_line_t *line,
		    relic_pred_id_t pred, uint32_t bound);
static void	 put_arg(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_arg_t *arg);
static void	 put_args(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_arg_t *args,
		    uint16_t count);
static void	 put_aggregate(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule,
		    const relic_aggregate_t *aggregate);
static void	 put_atom(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_atom_t *atom);
static void	 put_call(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_expr_call_t *call);
static void	 put_comparison(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule,
		    const relic_comparison_t *comparison);
static void	 put_compare(relic_explain_line_t *line,
		    enum relic_compare_op compare);
static void	 put_expr(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_expr_t *expr);
static void	 put_operand(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_expr_t *operand,
		    bool parenthesized);
static void	 put_key(relic_explain_line_t *line, uint32_t key_mask);
static void	 put_literal(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_literal_t *literal);
static void	 put_op_aggregate(const relic_t *ctx,
		    relic_explain_line_t *line, const relic_rule_t *rule,
		    const relic_op_aggregate_t *fold);
static void	 put_op(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_op_t *op,
		    bool streams);
static void	 put_op_atom(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_op_atom_t *atom);
static void	 put_plan(const relic_t *ctx, const relic_rule_t *rule,
		    const relic_plan_t *plan, unsigned int depth, bool streams);
static void	 put_pred(const relic_t *ctx, relic_explain_line_t *line,
		    relic_pred_id_t pred);
static void	 put_probe(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_op_atom_t *atom);
static void	 put_pred_name(const relic_t *ctx, relic_explain_line_t *line,
		    relic_pred_id_t pred);
static void	 put_query_rule(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_relation_t *answer);
static void	 put_read_completely(relic_t *ctx,
		    const relic_magic_t *magic);
static void	 put_relation_name(const relic_t *ctx,
		    relic_explain_line_t *line,
		    const relic_relation_t *relation);
static void	 put_rule(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule);
static void	 put_rule_body(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule);
static void	 put_size(relic_explain_line_t *line, size_t value);
static void	 put_string_literal(relic_explain_line_t *line,
		    const char *bytes, size_t length);
static void	 put_subsumed_line(relic_t *ctx,
		    const relic_adornment_t *adornment);
static void	 put_spelling(relic_explain_line_t *line,
		    const relic_spelling_t *spelling);
static void	 put_symbol(const relic_t *ctx, relic_explain_line_t *line,
		    uint32_t symbol);
static void	 put_term(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_plan_term_t *term);
static void	 put_text(relic_explain_line_t *line, const char *bytes,
		    size_t length);
static void	 put_uint64(relic_explain_line_t *line, uint64_t value);
static void	 put_value(const relic_t *ctx, relic_explain_line_t *line,
		    relic_dom_t dom);
static void	 put_variable(relic_explain_line_t *line,
		    const relic_rule_t *rule, uint16_t slot);
static void	 put_view(relic_explain_line_t *line, enum relic_view view);
static bool	 reads_as_negation(const relic_t *ctx,
		    const relic_expr_t *left);

void
relic_explain_set(relic_t *ctx, relic_explain_cb callback, void *callback_ctx)
{
	if (relic_enter(ctx, NULL, NULL) != 0)
		return;

	ctx->explain = callback;
	ctx->explain_ctx = callback_ctx;

	relic_leave(ctx);
}

bool
relic_explaining(const relic_t *ctx)
{
	return (ctx->explain != NULL);
}

/*
 * Start an empty line indented by `depth` levels.
 */
static void
line_begin(relic_explain_line_t *line, unsigned int depth)
{
	unsigned int i;

	line->length = 0;
	line->truncated = false;

	for (i = 0; i < depth * EXPLAIN_INDENT; i++)
		line_put_byte(line, ' ');
}

/*
 * Append one byte, which the caller has made printable ASCII.
 */
static void
line_put_byte(relic_explain_line_t *line, char byte)
{
	if (line->truncated)
		return;

	if (line->length == sizeof(line->text) - 1) {
		line->truncated = true;
		return;
	}

	line->text[line->length] = byte;
	line->length++;
}

static void
line_put(relic_explain_line_t *line, const char *text)
{
	for (; *text != '\0'; text++)
		line_put_byte(line, *text);
}

static void
line_put_escaped(relic_explain_line_t *line, unsigned char byte)
{
	char escape[5];

	snprintf(escape, sizeof(escape), "\\x%02x", (unsigned int)byte);
	line_put(line, escape);
}

/*
 * Terminate the line, marking a cut one with "...", and hand it to the
 * callback, if one is still set: a callback that breaks its contract by
 * clearing itself stops the lines rather than calling through nothing.  A
 * line is never cut shorter than the marker, because the buffer is far
 * longer than the three bytes it replaces.
 */
static void
line_finish(const relic_t *ctx, relic_explain_line_t *line)
{
	if (line->truncated) {
		line->text[line->length - 3] = '.';
		line->text[line->length - 2] = '.';
		line->text[line->length - 1] = '.';
	}

	line->text[line->length] = '\0';
	if (ctx->explain != NULL)
		ctx->explain(ctx->explain_ctx, line->text);
}

static void
put_size(relic_explain_line_t *line, size_t value)
{
	char text[24];

	snprintf(text, sizeof(text), "%zu", value);
	line_put(line, text);
}

static void
put_uint64(relic_explain_line_t *line, uint64_t value)
{
	char text[24];

	snprintf(text, sizeof(text), "%" PRIu64, value);
	line_put(line, text);
}

/*
 * Bytes written as they are when printable ASCII and as \xHH otherwise: a
 * name, or a symbol's text.
 */
static void
put_text(relic_explain_line_t *line, const char *bytes, size_t length)
{
	unsigned char byte;
	size_t i;

	for (i = 0; i < length; i++) {
		byte = (unsigned char)bytes[i];

		if (byte >= 0x20 && byte < 0x7f)
			line_put_byte(line, (char)byte);
		else
			line_put_escaped(line, byte);
	}
}

/*
 * A string as a quoted literal that spells exactly its bytes.
 */
static void
put_string_literal(relic_explain_line_t *line, const char *bytes,
    size_t length)
{
	unsigned char byte;
	size_t i;

	line_put_byte(line, '"');

	for (i = 0; i < length; i++) {
		byte = (unsigned char)bytes[i];

		if (byte == '\\')
			line_put(line, "\\\\");
		else if (byte == '"')
			line_put(line, "\\\"");
		else if (byte >= 0x20 && byte < 0x7f)
			line_put_byte(line, (char)byte);
		else
			line_put_escaped(line, byte);
	}

	line_put_byte(line, '"');
}

static void
put_symbol(const relic_t *ctx, relic_explain_line_t *line, uint32_t symbol)
{
	const char *bytes;
	size_t length;

	bytes = relic_symbol_bytes(ctx, symbol, &length);
	if (bytes == NULL) {
		line_put(line, "?");
		return;
	}

	put_text(line, bytes, length);
}

static void
put_spelling(relic_explain_line_t *line, const relic_spelling_t *spelling)
{
	put_text(line, spelling->bytes, spelling->length);
}

/*
 * A constant, the way the text output format writes it: an integer in
 * decimal, a symbol as its text, and a string as a quoted literal.
 */
static void
put_value(const relic_t *ctx, relic_explain_line_t *line, relic_dom_t dom)
{
	relic_value_t value;
	const char *bytes;
	char integer[24];
	size_t length;

	if (!relic_value_decode(ctx, dom, &value)) {
		line_put(line, "?");
		return;
	}

	switch (value.type) {
	case RELIC_INTEGER:
		snprintf(integer, sizeof(integer), "%" PRId64,
		    value.v.integer);
		line_put(line, integer);
		return;
	case RELIC_SYMBOL:
		put_symbol(ctx, line, value.v.id);
		return;
	case RELIC_STRING:
		bytes = relic_symbol_bytes(ctx, value.v.id, &length);
		if (bytes == NULL)
			break;
		put_string_literal(line, bytes, length);
		return;
	}

	line_put(line, "?");
}

static void
put_variable(relic_explain_line_t *line, const relic_rule_t *rule,
    uint16_t slot)
{
	if (slot >= rule->variable_count) {
		line_put(line, "?");
		return;
	}

	put_spelling(line, &rule->variables[slot].name);
}

static void
put_pred_name(const relic_t *ctx, relic_explain_line_t *line,
    relic_pred_id_t pred)
{
	const relic_pred_t *entry;

	entry = relic_pred_of(ctx, pred);
	if (entry == NULL) {
		line_put(line, "?");
		return;
	}

	put_spelling(line, &entry->name);
}

static void
put_compare(relic_explain_line_t *line, enum relic_compare_op compare)
{
	switch (compare) {
	case RELIC_CMP_EQ:
		line_put(line, " = ");
		return;
	case RELIC_CMP_NE:
		line_put(line, " != ");
		return;
	case RELIC_CMP_LT:
		line_put(line, " < ");
		return;
	case RELIC_CMP_LE:
		line_put(line, " <= ");
		return;
	case RELIC_CMP_GT:
		line_put(line, " > ");
		return;
	case RELIC_CMP_GE:
		line_put(line, " >= ");
		return;
	}

	line_put(line, " ? ");
}

/*
 * How tightly a node binds its operands, as the grammar decides: a sum
 * loosest, then a product, then a negation, and a leaf or a call, which is
 * a primary and needs no parentheses anywhere, tightest of all.
 */
static unsigned int
binds_as_tightly(const relic_expr_t *expr)
{
	switch (expr->kind) {
	case RELIC_EXPR_CONSTANT:
	case RELIC_EXPR_VARIABLE:
	case RELIC_EXPR_CALL:
		return (4);
	case RELIC_EXPR_NEGATE:
		return (3);
	case RELIC_EXPR_BINARY:
		break;
	}

	switch (expr->u.binary.op) {
	case RELIC_ARITH_ADD:
	case RELIC_ARITH_SUBTRACT:
		return (1);
	case RELIC_ARITH_MULTIPLY:
	case RELIC_ARITH_DIVIDE:
	case RELIC_ARITH_REMAINDER:
		break;
	}

	return (2);
}

/*
 * Whether a negation's operand is printed in parentheses.  An operator binds
 * less tightly than negation, and a constant that is not negative would read
 * back folded into a negative literal, `-5` rather than the negation of 5.
 * A variable, a call, a negation and a negative constant need none: `- -A`
 * and `- -5` read back as the negations they are, and parentheses there
 * would nest the text two levels for each level of the tree.
 */
static bool
negation_needs_parentheses(const relic_t *ctx, const relic_expr_t *operand)
{
	relic_value_t value;

	switch (operand->kind) {
	case RELIC_EXPR_VARIABLE:
	case RELIC_EXPR_CALL:
	case RELIC_EXPR_NEGATE:
		return (false);
	case RELIC_EXPR_BINARY:
		return (true);
	case RELIC_EXPR_CONSTANT:
		break;
	}

	return (!relic_value_decode(ctx, operand->u.constant, &value) ||
	    value.type != RELIC_INTEGER || value.v.integer >= 0);
}

/*
 * Whether a comparison's left operand is the symbol `not`, which at the start
 * of a literal would read back as the negation keyword.  It is printed in the
 * parentheses it must have been written in.
 */
static bool
reads_as_negation(const relic_t *ctx, const relic_expr_t *left)
{
	relic_value_t value;
	const char *bytes;
	size_t length;

	if (left->kind != RELIC_EXPR_CONSTANT ||
	    !relic_value_decode(ctx, left->u.constant, &value) ||
	    value.type != RELIC_SYMBOL)
		return (false);

	bytes = relic_symbol_bytes(ctx, value.v.id, &length);

	return (bytes != NULL && length == 3 && bcmp(bytes, "not", 3) == 0);
}

static void
put_operand(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_expr_t *operand, bool parenthesized)
{
	if (parenthesized)
		line_put_byte(line, '(');
	put_expr(ctx, line, rule, operand);
	if (parenthesized)
		line_put_byte(line, ')');
}

/*
 * An expression with exactly the parentheses its tree requires, so that
 * reading the text back builds the tree that was printed, and two texts
 * that build one tree print alike.
 *
 * An operand binding less tightly than its operator is parenthesized, and so
 * is a right operand binding exactly as tightly: `A - (B - C)` and
 * `2 * (3 / 2)` compute something else without them, and so does
 * `A + (B + C)`, which may overflow where `(A + B) + C` does not.  A negative
 * constant needs nothing: `X - -1` reads back as the constant it is.  A
 * negation's operand is parenthesized only where negation_needs_parentheses()
 * says, and is set off by a space when it begins with a minus of its own.
 *
 * Every parenthesis the printer writes, and every minus of a negation, is
 * owned by a distinct node on the path from the root to what it encloses, so
 * no token of the text is nested deeper than the tree is tall, and a tree
 * the parser accepted prints as text it accepts.
 */
static void
put_expr(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_expr_t *expr)
{
	const relic_expr_t *left;
	const relic_expr_t *right;
	unsigned int tightness;
	bool parenthesized;

	switch (expr->kind) {
	case RELIC_EXPR_CONSTANT:
		put_value(ctx, line, expr->u.constant);
		return;
	case RELIC_EXPR_VARIABLE:
		put_variable(line, rule, expr->u.slot);
		return;
	case RELIC_EXPR_NEGATE:
		parenthesized = negation_needs_parentheses(ctx,
		    expr->u.operand);
		line_put_byte(line, '-');
		if (!parenthesized &&
		    (expr->u.operand->kind == RELIC_EXPR_NEGATE ||
		    expr->u.operand->kind == RELIC_EXPR_CONSTANT))
			line_put_byte(line, ' ');
		put_operand(ctx, line, rule, expr->u.operand, parenthesized);
		return;
	case RELIC_EXPR_CALL:
		put_call(ctx, line, rule, &expr->u.call);
		return;
	case RELIC_EXPR_BINARY:
		break;
	}

	left = expr->u.binary.left;
	right = expr->u.binary.right;
	tightness = binds_as_tightly(expr);

	put_operand(ctx, line, rule, left, binds_as_tightly(left) < tightness);

	switch (expr->u.binary.op) {
	case RELIC_ARITH_ADD:
		line_put(line, " + ");
		break;
	case RELIC_ARITH_SUBTRACT:
		line_put(line, " - ");
		break;
	case RELIC_ARITH_MULTIPLY:
		line_put(line, " * ");
		break;
	case RELIC_ARITH_DIVIDE:
		line_put(line, " / ");
		break;
	case RELIC_ARITH_REMAINDER:
		line_put(line, " mod ");
		break;
	}

	put_operand(ctx, line, rule, right,
	    binds_as_tightly(right) <= tightness);
}

/*
 * A call as it is written, `fn(arg, ...)`.  Each argument is a whole
 * expression between a `(` or a `,` and a `,` or a `)`, so none needs
 * parentheses at its top.
 */
static void
put_call(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_expr_call_t *call)
{
	uint8_t i;

	line_put(line, relic_function_info(call->fn)->spelling);
	line_put_byte(line, '(');
	for (i = 0; i < call->argc; i++) {
		if (i > 0)
			line_put(line, ", ");
		put_expr(ctx, line, rule, call->args[i]);
	}
	line_put_byte(line, ')');
}

/*
 * An argument as the rule writes it.
 */
static void
put_arg(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_arg_t *arg)
{
	switch (arg->kind) {
	case RELIC_ARG_CONSTANT:
		put_value(ctx, line, arg->u.constant);
		return;
	case RELIC_ARG_VARIABLE:
		put_variable(line, rule, arg->u.slot);
		return;
	case RELIC_ARG_WILDCARD:
		break;
	}

	line_put(line, "_");
}

/*
 * An operation's term as the literal it reads would write it: a constant, a
 * variable whether it is read or bound here, or `_`.
 */
static void
put_term(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_plan_term_t *term)
{
	switch (term->action) {
	case RELIC_PLAN_CONSTANT:
		put_value(ctx, line, term->u.constant);
		return;
	case RELIC_PLAN_BOUND:
	case RELIC_PLAN_NUMBER:
	case RELIC_PLAN_TEXT:
	case RELIC_PLAN_BIND:
		put_variable(line, rule, term->u.slot);
		return;
	case RELIC_PLAN_ANY:
		break;
	}

	line_put(line, "_");
}

/*
 * The arguments that follow a name, as the language writes them: nothing for
 * none, and `(a, b)` otherwise.
 */
static void
put_args(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_arg_t *args, uint16_t count)
{
	uint16_t i;

	if (count == 0)
		return;

	line_put_byte(line, '(');
	for (i = 0; i < count; i++) {
		if (i > 0)
			line_put(line, ", ");
		put_arg(ctx, line, rule, &args[i]);
	}
	line_put_byte(line, ')');
}

/*
 * An atom of the rule, written `name` for arity zero and `name(a, b)`
 * otherwise, as the language writes it.
 */
static void
put_atom(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_atom_t *atom)
{
	const relic_pred_t *pred;

	put_pred_name(ctx, line, atom->pred);

	pred = relic_pred_of(ctx, atom->pred);
	if (pred != NULL)
		put_args(ctx, line, rule, atom->args, pred->arity);
}

/*
 * A comparison as the rule writes it.  A left operand that would read back
 * as the negation keyword keeps the parentheses it was written in.
 */
static void
put_comparison(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_comparison_t *comparison)
{
	put_operand(ctx, line, rule, comparison->left,
	    reads_as_negation(ctx, comparison->left));
	put_compare(line, comparison->op);
	put_expr(ctx, line, rule, comparison->right);
}

static void
put_literal(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_literal_t *literal)
{
	switch (literal->kind) {
	case RELIC_LITERAL_POSITIVE:
		put_atom(ctx, line, rule, &literal->u.atom);
		return;
	case RELIC_LITERAL_NEGATED:
		line_put(line, "not ");
		put_atom(ctx, line, rule, &literal->u.atom);
		return;
	case RELIC_LITERAL_COMPARISON:
		put_comparison(ctx, line, rule, &literal->u.comparison);
		return;
	case RELIC_LITERAL_AGGREGATE:
		put_aggregate(ctx, line, rule, &literal->u.aggregate);
		return;
	}
}

/*
 * An aggregate as the rule writes it: `N = count { ... }` or `S =
 * sum(Size) { ... }`.  The result is a variable of the rule; the aggregated
 * variable and the body are written from the compiled body, whose
 * variables they are, so the braces read back as the writer wrote them.
 */
static void
put_aggregate(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_aggregate_t *aggregate)
{
	const relic_rule_t *body;
	size_t i;

	body = aggregate->body;

	put_variable(line, rule, aggregate->result);
	line_put(line, " = ");
	line_put(line, relic_aggregate_spelling(aggregate->fn));
	if (aggregate->fn != RELIC_AGG_COUNT) {
		line_put_byte(line, '(');
		put_variable(line, body, aggregate->column);
		line_put_byte(line, ')');
	}

	line_put(line, " { ");
	for (i = 0; i < body->body_length; i++) {
		if (i > 0)
			line_put(line, ", ");
		put_literal(ctx, line, body, &body->body[i]);
	}
	line_put(line, " }");
}

/*
 * What follows a rule's head as it is written: its body, if it has one, and
 * the full stop.
 */
static void
put_rule_body(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule)
{
	size_t i;

	for (i = 0; i < rule->body_length; i++) {
		line_put(line, i == 0 ? " :- " : ", ");
		put_literal(ctx, line, rule, &rule->body[i]);
	}

	line_put_byte(line, '.');
}

/*
 * A rule of the program, or one generated from it, as it is written.  Its
 * head is an atom over a predicate.
 */
static void
put_rule(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule)
{
	relic_atom_t head;

	head.args = rule->head_args;
	head.pred = rule->head;
	put_atom(ctx, line, rule, &head);

	put_rule_body(ctx, line, rule);
}

/*
 * A query's rule as it is written.  Its head names no predicate, and is
 * written as the query's answer relation is named.
 */
static void
put_query_rule(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_relation_t *answer)
{
	put_relation_name(ctx, line, answer);
	put_args(ctx, line, rule, rule->head_args, rule->head_arity);

	put_rule_body(ctx, line, rule);
}

/*
 * The columns a lookup is keyed on, as "key={0,2}".
 */
static void
put_key(relic_explain_line_t *line, uint32_t key_mask)
{
	bool first;
	uint16_t column;

	line_put(line, " key={");

	first = true;
	for (column = 0; column < RELIC_MAX_ARITY; column++) {
		if ((key_mask & (UINT32_C(1) << column)) == 0)
			continue;

		if (!first)
			line_put_byte(line, ',');
		put_size(line, column);
		first = false;
	}

	line_put_byte(line, '}');
}

/*
 * What a relation is called, as the relation layer names it.  The buffer is
 * as long as a line, so a name is cut here only where the line would cut it
 * anyway.
 */
static void
put_relation_name(const relic_t *ctx, relic_explain_line_t *line,
    const relic_relation_t *relation)
{
	char name[EXPLAIN_LINE_MAX];

	relic_relation_name(ctx, relation, name, sizeof(name));
	put_text(line, name, strlen(name));
}

/*
 * The literal an operation over a relation reads, from its terms.
 */
static void
put_op_atom(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_op_atom_t *atom)
{
	uint16_t i;

	put_relation_name(ctx, line, atom->relation);
	if (atom->relation->arity == 0)
		return;

	line_put_byte(line, '(');
	for (i = 0; i < atom->relation->arity; i++) {
		if (i > 0)
			line_put(line, ", ");
		put_term(ctx, line, rule, &atom->terms[i]);
	}
	line_put_byte(line, ')');
}

/*
 * The view an operation reads through, after its kind, for the views a
 * reader needs to see: `delta` and `old`.  A member literal of a variant
 * reads `current` unless marked otherwise, and every other operation reads
 * `all`, so neither is written.
 */
static void
put_view(relic_explain_line_t *line, enum relic_view view)
{
	switch (view) {
	case RELIC_VIEW_DELTA:
		line_put(line, "delta ");
		return;
	case RELIC_VIEW_OLD:
		line_put(line, "old ");
		return;
	case RELIC_VIEW_ALL:
	case RELIC_VIEW_CURRENT:
		break;
	}
}

/*
 * A probe's atom and key.  The atom shows which columns are `_`, so the key
 * is printed only where it names the secondary index the probe took, as a
 * lookup's does; a probe that scans prints without one, as a lookup planned
 * without indexes prints as a scan.
 */
static void
put_probe(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_op_atom_t *atom)
{
	put_op_atom(ctx, line, rule, atom);
	if (relic_plan_probe_shape(atom->key_mask, atom->arity) ==
	    RELIC_PROBE_INDEX)
		put_key(line, atom->key_mask);
}

/*
 * One operation of a plan.  `streams` says that the plan's EMIT delivers
 * its tuples to the caller as a query's rows rather than writing the
 * relation it names, which its line shows as `stream`.
 */
static void
put_op(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_op_t *op, bool streams)
{
	switch (op->kind) {
	case RELIC_OP_SCAN:
		line_put(line, "scan ");
		put_view(line, op->u.atom.view);
		put_op_atom(ctx, line, rule, &op->u.atom);
		return;
	case RELIC_OP_LOOKUP:
		line_put(line, "lookup ");
		put_view(line, op->u.atom.view);
		put_op_atom(ctx, line, rule, &op->u.atom);
		put_key(line, op->u.atom.key_mask);
		return;
	case RELIC_OP_MEMBER:
		line_put(line, "member ");
		put_view(line, op->u.atom.view);
		put_op_atom(ctx, line, rule, &op->u.atom);
		return;
	case RELIC_OP_EXISTS:
		line_put(line, "exists ");
		put_view(line, op->u.atom.view);
		put_probe(ctx, line, rule, &op->u.atom);
		return;
	case RELIC_OP_ANTI_MEMBER:
		line_put(line, "anti-member ");
		put_probe(ctx, line, rule, &op->u.atom);
		return;
	case RELIC_OP_FILTER:
		line_put(line, "filter ");
		put_comparison(ctx, line, rule, op->u.filter.comparison);
		return;
	case RELIC_OP_ASSIGN:
		line_put(line, "assign ");
		put_variable(line, rule, op->u.assign.slot);
		line_put(line, " = ");
		put_expr(ctx, line, rule, op->u.assign.expr);
		return;
	case RELIC_OP_AGGREGATE:
		line_put(line, "aggregate ");
		put_op_aggregate(ctx, line, rule, &op->u.aggregate);
		return;
	case RELIC_OP_EMIT:
		line_put(line, streams ? "stream " : "emit ");
		put_op_atom(ctx, line, rule, &op->u.atom);
		return;
	}

	line_put(line, "?");
}

/*
 * A fold as the equality it stands for, then the relation it folds with
 * its columns named after the variables of the compiled body, whose slot c
 * is column c, and its key columns when it has any: `N = count
 * agg$p$1$0$2(X, Y) key={0}` for a group relation, `N = count edge(Y, X)
 * key={1}` for an atom folded in place.
 */
static void
put_op_aggregate(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_op_aggregate_t *fold)
{
	const relic_rule_t *body;
	uint16_t column;

	body = fold->aggregate->body;

	put_variable(line, rule, fold->slot);
	line_put(line, " = ");
	line_put(line, relic_aggregate_spelling(fold->fn));
	if (fold->fn != RELIC_AGG_COUNT) {
		line_put_byte(line, '(');
		put_variable(line, body, fold->column);
		line_put_byte(line, ')');
	}
	line_put_byte(line, ' ');

	put_relation_name(ctx, line, fold->relation);
	if (fold->arity > 0) {
		line_put_byte(line, '(');
		for (column = 0; column < fold->arity; column++) {
			if (column > 0)
				line_put(line, ", ");
			put_variable(line, body, column);
		}
		line_put_byte(line, ')');
	}

	if (fold->key_count > 0)
		put_key(line, fold->key_mask);
}

/*
 * One line per operation, in plan order, indented by `depth` levels, the
 * EMIT shown as put_op() shows it for `streams`.
 */
static void
put_plan(const relic_t *ctx, const relic_rule_t *rule,
    const relic_plan_t *plan, unsigned int depth, bool streams)
{
	relic_explain_line_t line;
	size_t i;

	for (i = 0; i < plan->op_count; i++) {
		line_begin(&line, depth);
		put_op(ctx, &line, rule, &plan->ops[i], streams);
		line_finish(ctx, &line);
	}
}

/*
 * "name/arity", as a message names a predicate but without shortening it.
 */
static void
put_pred(const relic_t *ctx, relic_explain_line_t *line, relic_pred_id_t pred)
{
	const relic_pred_t *entry;

	put_pred_name(ctx, line, pred);

	entry = relic_pred_of(ctx, pred);
	line_put_byte(line, '/');
	put_size(line, entry == NULL ? 0 : entry->arity);
}

/*
 * "stratum S, component C: p/2, q/1", with "(recursive)" after the
 * component's position when it is, and then "facts: N" when it has any.
 */
void
relic_explain_component(relic_t *ctx, const relic_schedule_t *schedule,
    size_t position, size_t facts, unsigned int depth)
{
	const relic_component_t *component;
	relic_explain_line_t line;
	size_t i;

	if (!relic_explaining(ctx))
		return;

	component = &schedule->components[position];

	line_begin(&line, depth);
	line_put(&line, "stratum ");
	put_size(&line, component->stratum);
	line_put(&line, ", component ");
	put_size(&line, position);
	if (component->recursive)
		line_put(&line, " (recursive)");
	line_put_byte(&line, ':');

	for (i = 0; i < component->member_count; i++) {
		line_put(&line, i == 0 ? " " : ", ");
		put_pred(ctx, &line,
		    schedule->members[component->first_member + i]);
	}
	line_finish(ctx, &line);

	if (facts == 0)
		return;

	line_begin(&line, depth + 1);
	line_put(&line, "facts: ");
	put_size(&line, facts);
	line_finish(ctx, &line);
}

/*
 * A rule, followed by its plan when it has one.  A recursive rule under the
 * semi-naive fixed point runs only as its delta variants and has none.
 */
void
relic_explain_rule(relic_t *ctx, const relic_rule_t *rule,
    const relic_plan_t *plan, unsigned int depth)
{
	relic_explain_line_t line;

	if (!relic_explaining(ctx))
		return;

	line_begin(&line, depth + 1);
	line_put(&line, "rule ");
	put_rule(ctx, &line, rule);
	line_finish(ctx, &line);

	if (plan != NULL)
		put_plan(ctx, rule, plan, depth + 2, false);
}

/*
 * Begin the heading of one of a delta variant's plans: the literal that
 * reads the delta, after which the caller says when a round runs the plan,
 * if the variant has two.
 */
static void
begin_variant_heading(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, size_t delta_literal, unsigned int depth)
{
	line_begin(line, depth);
	line_put(line, "delta ");
	if (delta_literal < rule->body_length)
		put_literal(ctx, line, rule, &rule->body[delta_literal]);
}

/*
 * End a heading with its colon and hand the line over.
 */
static void
finish_heading(const relic_t *ctx, relic_explain_line_t *line)
{
	line_put_byte(line, ':');
	line_finish(ctx, line);
}

/*
 * A delta variant, headed by the literal that reads the delta and nested
 * under the rule it is a variant of, with the plans it was given: NULL for
 * a plan not built because no round would run it.  A variant with both
 * shows the placed plan headed by the threshold a round's delta must exceed
 * for it to run, then the delta-first plan.  Which a round chose is not
 * shown, so that the lines are a function of the plans and not of how many
 * rounds ran.
 */
void
relic_explain_variant(relic_t *ctx, const relic_rule_t *rule,
    size_t delta_literal, const relic_plan_t *placed,
    const relic_plan_t *delta_first, uint64_t threshold, unsigned int depth)
{
	relic_explain_line_t line;

	if (!relic_explaining(ctx))
		return;

	if (placed == NULL || delta_first == NULL) {
		begin_variant_heading(ctx, &line, rule, delta_literal,
		    depth + 2);
		finish_heading(ctx, &line);
		put_plan(ctx, rule, placed != NULL ? placed : delta_first,
		    depth + 3, false);
		return;
	}

	begin_variant_heading(ctx, &line, rule, delta_literal, depth + 2);
	line_put(&line, " when the delta holds more than ");
	put_uint64(&line, threshold);
	line_put(&line, threshold == 1 ? " tuple" : " tuples");
	finish_heading(ctx, &line);
	put_plan(ctx, rule, placed, depth + 3, false);

	begin_variant_heading(ctx, &line, rule, delta_literal, depth + 2);
	line_put(&line, " otherwise");
	finish_heading(ctx, &line);
	put_plan(ctx, rule, delta_first, depth + 3, false);
}

void
relic_explain_query(relic_t *ctx, const relic_rule_t *rule,
    const relic_relation_t *answer)
{
	relic_explain_line_t line;

	if (!relic_explaining(ctx))
		return;

	line_begin(&line, 0);
	line_put(&line, "query ");
	put_query_rule(ctx, &line, rule, answer);
	line_finish(ctx, &line);
}

/*
 * A query's plan, one level in under its heading.
 */
void
relic_explain_query_plan(relic_t *ctx, const relic_rule_t *rule,
    const relic_plan_t *plan, bool streams)
{
	if (!relic_explaining(ctx))
		return;

	put_plan(ctx, rule, plan, 1, streams);
}

/*
 * A predicate's b and f per column for the columns of `bound`, with nothing
 * written for a predicate of no columns.
 */
static void
put_adornment(const relic_t *ctx, relic_explain_line_t *line,
    relic_pred_id_t pred, uint32_t bound)
{
	const relic_pred_t *entry;
	uint16_t column;

	entry = relic_pred_of(ctx, pred);
	for (column = 0; entry != NULL && column < entry->arity; column++)
		line_put_byte(line, (bound & (UINT32_C(1) << column)) != 0 ?
		    'b' : 'f');
}

/*
 * One adornment of a query's rewrite, "adorn p/2 bf".
 */
static void
put_adorn_line(relic_t *ctx, const relic_adornment_t *adornment)
{
	relic_explain_line_t line;

	line_begin(&line, 1);
	put_adorn(ctx, &line, adornment->source, adornment->bound);
	line_finish(ctx, &line);
}

/*
 * One call a query's rewrite found subsumed by the free adornment of its
 * predicate: "adorn p/2 bf, subsumed by p/2 ff".  The free adornment is
 * named as an adornment rather than as a predicate, since a rewrite that
 * reads what it reaches completely names no predicate for it.
 */
static void
put_subsumed_line(relic_t *ctx, const relic_adornment_t *adornment)
{
	relic_explain_line_t line;

	line_begin(&line, 1);
	put_adorn(ctx, &line, adornment->source, adornment->bound);
	line_put(&line, ", subsumed by ");
	put_adorned_pred(ctx, &line, adornment->source, 0);
	line_finish(ctx, &line);
}

/*
 * "adorn " and the adornment of `source` binding the columns of `bound`.
 */
static void
put_adorn(const relic_t *ctx, relic_explain_line_t *line,
    relic_pred_id_t source, uint32_t bound)
{
	line_put(line, "adorn ");
	put_adorned_pred(ctx, line, source, bound);
}

/*
 * A predicate and an adornment of it, "p/2 bf", or "p/0" alone for a
 * predicate with no columns to adorn.
 */
static void
put_adorned_pred(const relic_t *ctx, relic_explain_line_t *line,
    relic_pred_id_t source, uint32_t bound)
{
	const relic_pred_t *pred;

	pred = relic_pred_of(ctx, source);

	put_pred(ctx, line, source);
	if (pred != NULL && pred->arity > 0)
		line_put_byte(line, ' ');
	put_adornment(ctx, line, source, bound);
}

/*
 * The predicates a rewrite that generated nothing reads completely: those of
 * the adornments it kept, every one of them free.
 */
static void
put_read_completely(relic_t *ctx, const relic_magic_t *magic)
{
	relic_explain_line_t line;
	size_t i;

	for (i = 0; i < magic->kept.count; i++) {
		line_begin(&line, 1);
		line_put(&line, "read completely: ");
		put_pred(ctx, &line, magic->kept.entries[i].source);
		line_finish(ctx, &line);
	}
}

/*
 * Every adornment the search reached, in the order it reached them: the
 * subsumed calls interleaved with the adornments kept, each listed before
 * the kept adornment its `kept_before` names.
 */
static void
put_adorn_lines(relic_t *ctx, const relic_magic_t *magic)
{
	const relic_adornment_set_t *subsumed;
	size_t next;
	size_t i;

	subsumed = &magic->subsumed;
	next = 0;

	for (i = 0; i < magic->kept.count; i++) {
		for (; next < subsumed->count &&
		    subsumed->entries[next].kept_before <= i; next++)
			put_subsumed_line(ctx, &subsumed->entries[next]);
		put_adorn_line(ctx, &magic->kept.entries[i]);
	}

	for (; next < subsumed->count; next++)
		put_subsumed_line(ctx, &subsumed->entries[next]);
}

/*
 * The heading of a query's Magic rewrite: the query as written, and each
 * adornment and subsumed call in the order the search reached it.  Then, when
 * the rewrite found every predicate the query reaches read free, the predicates
 * it reads completely instead, and the heading ends there; the components they
 * need follow as the program's own.  Otherwise each rule the query's own body
 * generated, and the rewritten program's components follow, nested one level
 * beneath the heading.
 */
void
relic_explain_rewrite(relic_t *ctx, const relic_rule_t *query,
    const relic_relation_t *answer, const relic_magic_t *magic)
{
	relic_explain_line_t line;
	size_t i;

	if (!relic_explaining(ctx))
		return;

	line_begin(&line, 0);
	line_put(&line, "magic rewrite of ");
	put_query_rule(ctx, &line, query, answer);
	line_finish(ctx, &line);

	put_adorn_lines(ctx, magic);

	if (magic->reads_completely)
		put_read_completely(ctx, magic);

	for (i = 0; i < magic->seed_count; i++) {
		line_begin(&line, 1);
		line_put(&line, "seed ");
		put_rule(ctx, &line, &magic->rules.entries[i]);
		line_finish(ctx, &line);
	}
}

/*
 * The line that marks a query's Magic rewrite abandoned over its limit.
 * The components evaluated for the rewrite before it was abandoned are
 * shown above it, and the whole program's, evaluated to answer the query
 * instead, below it, so the same component can appear on both sides.
 */
void
relic_explain_abandoned_rewrite(relic_t *ctx, const relic_rule_t *query,
    const relic_relation_t *answer)
{
	relic_explain_line_t line;

	if (!relic_explaining(ctx))
		return;

	line_begin(&line, 0);
	line_put(&line, "abandoned magic rewrite of ");
	put_query_rule(ctx, &line, query, answer);
	line_finish(ctx, &line);
}
