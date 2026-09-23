/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <inttypes.h>
#include <stdio.h>

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
 * way a program would write it: its arguments are the operation's terms, a
 * variable named as the rule names the slot it reads or binds.  What is
 * shown is therefore the plan itself, not the rule it was built from.
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

static void	 line_begin(relic_explain_line_t *line, unsigned int depth);
static void	 line_finish(const relic_t *ctx, relic_explain_line_t *line);
static void	 line_put(relic_explain_line_t *line, const char *text);
static void	 line_put_byte(relic_explain_line_t *line, char byte);
static void	 line_put_escaped(relic_explain_line_t *line,
		    unsigned char byte);
static void	 put_arg(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_arg_t *arg);
static void	 put_atom(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_atom_t *atom);
static void	 put_compare(relic_explain_line_t *line,
		    enum relic_compare_op compare);
static void	 put_key(relic_explain_line_t *line, uint32_t key_mask);
static void	 put_literal(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_literal_t *literal);
static void	 put_op(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_op_t *op);
static void	 put_op_atom(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_op_atom_t *atom);
static void	 put_plan(const relic_t *ctx, const relic_rule_t *rule,
		    const relic_plan_t *plan, unsigned int depth);
static void	 put_pred_name(const relic_t *ctx, relic_explain_line_t *line,
		    relic_pred_id_t pred);
static void	 put_rule(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule);
static void	 put_size(relic_explain_line_t *line, size_t value);
static void	 put_string_literal(relic_explain_line_t *line,
		    const char *bytes, size_t length);
static void	 put_symbol(const relic_t *ctx, relic_explain_line_t *line,
		    uint32_t symbol);
static void	 put_term(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, const relic_plan_term_t *term);
static void	 put_text(relic_explain_line_t *line, const char *bytes,
		    size_t length);
static void	 put_value(const relic_t *ctx, relic_explain_line_t *line,
		    relic_dom_t dom);
static void	 put_variable(const relic_t *ctx, relic_explain_line_t *line,
		    const relic_rule_t *rule, uint16_t slot);

void
relic_explain_set(relic_t *ctx, relic_explain_cb callback, void *callback_ctx)
{
	ctx->explain = callback;
	ctx->explain_ctx = callback_ctx;
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

/*
 * A constant, the way the text output format writes it: an integer in
 * decimal, a symbol as its text, and a string as a quoted literal.
 */
static void
put_value(const relic_t *ctx, relic_explain_line_t *line, relic_dom_t dom)
{
	const relic_value_t *value;
	const char *bytes;
	char integer[24];
	size_t length;

	value = relic_value_of(ctx, dom);
	if (value == NULL) {
		line_put(line, "?");
		return;
	}

	switch (value->type) {
	case RELIC_INTEGER:
		snprintf(integer, sizeof(integer), "%" PRId64,
		    value->v.integer);
		line_put(line, integer);
		return;
	case RELIC_SYMBOL:
		put_symbol(ctx, line, value->v.id);
		return;
	case RELIC_STRING:
		bytes = relic_symbol_bytes(ctx, value->v.id, &length);
		if (bytes == NULL)
			break;
		put_string_literal(line, bytes, length);
		return;
	}

	line_put(line, "?");
}

static void
put_variable(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, uint16_t slot)
{
	if (slot >= rule->variable_count) {
		line_put(line, "?");
		return;
	}

	put_symbol(ctx, line, rule->variables[slot].name);
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

	put_symbol(ctx, line, entry->name);
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
		put_variable(ctx, line, rule, arg->u.slot);
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
	case RELIC_PLAN_BIND:
		put_variable(ctx, line, rule, term->u.slot);
		return;
	case RELIC_PLAN_ANY:
		break;
	}

	line_put(line, "_");
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
	uint16_t i;

	put_pred_name(ctx, line, atom->pred);

	pred = relic_pred_of(ctx, atom->pred);
	if (pred == NULL || pred->arity == 0)
		return;

	line_put_byte(line, '(');
	for (i = 0; i < pred->arity; i++) {
		if (i > 0)
			line_put(line, ", ");
		put_arg(ctx, line, rule, &atom->args[i]);
	}
	line_put_byte(line, ')');
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
		put_arg(ctx, line, rule, &literal->u.comparison.left);
		put_compare(line, literal->u.comparison.op);
		put_arg(ctx, line, rule, &literal->u.comparison.right);
		return;
	}
}

/*
 * A rule as it is written, ending in its full stop.
 */
static void
put_rule(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule)
{
	relic_atom_t head;
	size_t i;

	head.args = rule->head_args;
	head.pred = rule->head;
	put_atom(ctx, line, rule, &head);

	for (i = 0; i < rule->body_length; i++) {
		line_put(line, i == 0 ? " :- " : ", ");
		put_literal(ctx, line, rule, &rule->body[i]);
	}

	line_put_byte(line, '.');
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
 * The literal an operation over a relation reads, from its terms.
 */
static void
put_op_atom(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_op_atom_t *atom)
{
	uint16_t i;

	put_pred_name(ctx, line, atom->relation->pred);
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

static void
put_op(const relic_t *ctx, relic_explain_line_t *line,
    const relic_rule_t *rule, const relic_op_t *op)
{
	switch (op->kind) {
	case RELIC_OP_SCAN:
		line_put(line, op->u.atom.delta ? "scan delta " : "scan ");
		put_op_atom(ctx, line, rule, &op->u.atom);
		return;
	case RELIC_OP_LOOKUP:
		line_put(line, "lookup ");
		put_op_atom(ctx, line, rule, &op->u.atom);
		put_key(line, op->u.atom.key_mask);
		return;
	case RELIC_OP_MEMBER:
		line_put(line, "member ");
		put_op_atom(ctx, line, rule, &op->u.atom);
		return;
	case RELIC_OP_ANTI_MEMBER:
		line_put(line, "anti-member ");
		put_op_atom(ctx, line, rule, &op->u.atom);
		return;
	case RELIC_OP_FILTER:
		line_put(line, "filter ");
		put_term(ctx, line, rule, &op->u.filter.left);
		put_compare(line, op->u.filter.compare);
		put_term(ctx, line, rule, &op->u.filter.right);
		return;
	case RELIC_OP_EMIT:
		line_put(line, "emit ");
		put_op_atom(ctx, line, rule, &op->u.atom);
		return;
	}

	line_put(line, "?");
}

/*
 * One line per operation, in plan order, indented by `depth` levels.
 */
static void
put_plan(const relic_t *ctx, const relic_rule_t *rule,
    const relic_plan_t *plan, unsigned int depth)
{
	relic_explain_line_t line;
	size_t i;

	for (i = 0; i < plan->op_count; i++) {
		line_begin(&line, depth);
		put_op(ctx, &line, rule, &plan->ops[i]);
		line_finish(ctx, &line);
	}
}

/*
 * "stratum S, component C: p/2, q/1", with "(recursive)" after the
 * component's position when it is, and then "facts: N" when it has any.
 */
void
relic_explain_component(relic_t *ctx, const relic_schedule_t *schedule,
    size_t position, size_t facts)
{
	const relic_component_t *component;
	const relic_pred_t *pred;
	relic_pred_id_t member;
	relic_explain_line_t line;
	size_t i;

	if (!relic_explaining(ctx))
		return;

	component = &schedule->components[position];

	line_begin(&line, 0);
	line_put(&line, "stratum ");
	put_size(&line, component->stratum);
	line_put(&line, ", component ");
	put_size(&line, position);
	if (component->recursive)
		line_put(&line, " (recursive)");
	line_put_byte(&line, ':');

	for (i = 0; i < component->member_count; i++) {
		member = schedule->members[component->first_member + i];

		line_put(&line, i == 0 ? " " : ", ");
		put_pred_name(ctx, &line, member);
		pred = relic_pred_of(ctx, member);
		line_put_byte(&line, '/');
		put_size(&line, pred == NULL ? 0 : pred->arity);
	}
	line_finish(ctx, &line);

	if (facts == 0)
		return;

	line_begin(&line, 1);
	line_put(&line, "facts: ");
	put_size(&line, facts);
	line_finish(ctx, &line);
}

void
relic_explain_rule(relic_t *ctx, const relic_rule_t *rule,
    const relic_plan_t *plan)
{
	relic_explain_line_t line;

	if (!relic_explaining(ctx))
		return;

	line_begin(&line, 1);
	line_put(&line, "rule ");
	put_rule(ctx, &line, rule);
	line_finish(ctx, &line);

	put_plan(ctx, rule, plan, 2);
}

/*
 * A delta variant, headed by the literal that reads the delta and nested
 * under the rule it is a variant of.
 */
void
relic_explain_variant(relic_t *ctx, const relic_rule_t *rule,
    size_t delta_literal, const relic_plan_t *plan)
{
	relic_explain_line_t line;

	if (!relic_explaining(ctx))
		return;

	line_begin(&line, 2);
	line_put(&line, "delta ");
	if (delta_literal < rule->body_length)
		put_literal(ctx, &line, rule, &rule->body[delta_literal]);
	line_put_byte(&line, ':');
	line_finish(ctx, &line);

	put_plan(ctx, rule, plan, 3);
}

void
relic_explain_query(relic_t *ctx, const relic_rule_t *rule,
    const relic_plan_t *plan)
{
	relic_explain_line_t line;

	if (!relic_explaining(ctx))
		return;

	line_begin(&line, 0);
	line_put(&line, "query ");
	put_rule(ctx, &line, rule);
	line_finish(ctx, &line);

	put_plan(ctx, rule, plan, 1);
}
