/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "internal.h"

/*
 * Diagnostics.
 *
 * Every failure in the library reports through relic_diagf() and returns a
 * failure status; nothing in lib/ writes to stderr.  Where a diagnostic ends
 * up is the caller's decision, made once by installing a callback, and the
 * tool is an ordinary caller that happens to install the formatter below.
 *
 * The last RELIC_ERROR message is also kept in the context, so a caller that
 * wants only the reason for a failure can read relic_error() and install no
 * callback at all.
 */

/* What relic_error() says of a call refused because its context was busy. */
#define	RELIC_REFUSED_ERROR \
	"the context was called from one of its own callbacks"

static const char	*level_name(enum relic_diag_level level);
static void		 record_error(relic_t *ctx, const char *message);
static size_t		 string_piece(unsigned char byte, char *piece,
			    size_t size);
static void		 write_escaped(FILE *stream, const char *text);

static const char *
level_name(enum relic_diag_level level)
{
	switch (level) {
	case RELIC_NOTE:
		return ("note");
	case RELIC_WARNING:
		return ("warning");
	case RELIC_ERROR:
		return ("error");
	}

	/* Unreachable; a diagnostic of no known level is reported as one. */
	return ("error");
}

/*
 * Make `message` the context's last error, which relic_error() returns.
 */
static void
record_error(relic_t *ctx, const char *message)
{
	strlcpy(ctx->diag.last_error, message, sizeof(ctx->diag.last_error));
	ctx->diag.has_error = true;
	ctx->diag.refused = false;
	ctx->diag.error_count++;
}

void
relic_diag_init(relic_diag_t *diag)
{
	bzero(diag, sizeof(*diag));
}

void
relic_diag_set(relic_t *ctx, relic_diag_cb callback, void *callback_ctx)
{
	if (relic_enter(ctx, NULL, NULL) != 0)
		return;

	ctx->diag.callback = callback;
	ctx->diag.callback_ctx = callback_ctx;

	relic_leave(ctx);
}

const char *
relic_error(relic_t *ctx)
{
	if (ctx->diag.refused)
		return (RELIC_REFUSED_ERROR);

	if (!ctx->diag.has_error)
		return (NULL);

	return (ctx->diag.last_error);
}

void
relic_diagf(relic_t *ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	relic_vdiagf(ctx, level, where, format, arguments);
	va_end(arguments);
}

void
relic_vdiagf(relic_t *ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *format, va_list arguments)
{
	char message[sizeof(ctx->diag.last_error)];

	/*
	 * A message longer than the buffer is truncated.  A diagnostic
	 * reports a failure and must not be able to cause one, so it does not
	 * allocate; every message the library produces names one thing and
	 * fits comfortably.
	 *
	 * The buffer is emptied first because vsnprintf(3) leaves its
	 * contents unspecified, and need not terminate them, when it returns
	 * a negative value.  What is copied out below and handed to
	 * relic_error() would then be whatever the stack held here, so the
	 * terminator is placed before the call rather than assumed after it.
	 */
	message[0] = '\0';
	vsnprintf(message, sizeof(message), format, arguments);

	if (level == RELIC_ERROR)
		record_error(ctx, message);

	if (ctx->diag.callback != NULL)
		ctx->diag.callback(ctx->diag.callback_ctx, level, where,
		    message);
}

void
relic_diag_record_refusal(relic_t *ctx)
{
	ctx->diag.refused = true;
}

void
relic_diag_no_memory(relic_t *ctx)
{
	if (ctx->diag.memory_reported) {
		relic_account_take_refusal(&ctx->account);
		return;
	}
	ctx->diag.memory_reported = true;

	if (!relic_account_take_refusal(&ctx->account)) {
		relic_diagf(ctx, RELIC_ERROR, NULL, "out of memory");
		return;
	}

	relic_diag_limit(ctx, "memory", ctx->account.limit,
	    ctx->diag.activity.verb, ctx->diag.activity.subject);
}

void
relic_diag_limit(relic_t *ctx, const char *limit, size_t configured,
    const char *verb, const char *subject)
{
	if (verb == NULL) {
		relic_diagf(ctx, RELIC_ERROR, NULL, "limit exceeded: %s=%zu",
		    limit, configured);
		return;
	}

	relic_diagf(ctx, RELIC_ERROR, NULL,
	    "limit exceeded: %s=%zu while %s %s", limit, configured, verb,
	    subject);
}

void
relic_diag_begin_operation(relic_t *ctx, const char *verb,
    const char *subject)
{
	ctx->diag.memory_reported = false;
	relic_diag_activity(ctx, verb, subject);
}

void
relic_diag_activity(relic_t *ctx, const char *verb, const char *subject)
{
	ctx->diag.activity.verb = verb;
	if (verb != NULL)
		relic_diag_name(subject, ctx->diag.activity.subject,
		    sizeof(ctx->diag.activity.subject));
	relic_account_take_refusal(&ctx->account);
}

void
relic_diag_save_activity(const relic_t *ctx, relic_activity_t *saved)
{
	*saved = ctx->diag.activity;
}

void
relic_diag_restore_activity(relic_t *ctx, const relic_activity_t *saved)
{
	ctx->diag.activity = *saved;
}

/*
 * Write text with every byte outside printable ASCII replaced by a \xHH
 * escape, and a backslash doubled so that an escape this function produced
 * cannot be confused with one the text contained.
 *
 * Both halves of a diagnostic pass through here.  A message quotes
 * identifiers and string literals taken from a source file, and a file name
 * comes from a command line, so either can hold a control character or a
 * terminal escape sequence.  Printing one unexamined would let a program
 * relic was asked to read retitle the reader's terminal or forge a
 * diagnostic line of its own.
 */
static void
write_escaped(FILE *stream, const char *text)
{
	size_t i;

	for (i = 0; text[i] != '\0'; i++) {
		unsigned char byte;

		byte = (unsigned char)text[i];

		if (byte == '\\')
			fputs("\\\\", stream);
		else if (byte >= ' ' && byte <= '~')
			fputc((int)byte, stream);
		else
			fprintf(stream, "\\x%02x", byte);
	}
}

void
relic_diag_stderr(void *callback_ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	const char *program_name;

	/* The tool installs the name it was invoked by as the context. */
	program_name = callback_ctx;

	if (where != NULL && where->file != NULL) {
		write_escaped(stderr, where->file);
		fprintf(stderr, ":%u:%u: ", (unsigned int)where->line,
		    (unsigned int)where->column);
	} else if (program_name != NULL) {
		write_escaped(stderr, program_name);
		fputs(": ", stderr);
	}

	fprintf(stderr, "%s: ", level_name(level));
	write_escaped(stderr, message);
	fputc('\n', stderr);
}

/*
 * Describe one byte for a message: as a character in single quotes when it
 * is printable and not a backslash, and as its value otherwise.  A control
 * byte rendered as itself would tell the reader nothing and would travel to
 * a terminal, and a backslash would be doubled by a formatter that escapes
 * what it prints, misstating the byte.
 */
const char *
relic_diag_byte(unsigned char byte, char *buffer, size_t size)
{
	if (byte >= ' ' && byte <= '~' && byte != '\\')
		snprintf(buffer, size, "'%c'", (int)byte);
	else
		snprintf(buffer, size, "0x%02x", (unsigned int)byte);

	return (buffer);
}

/*
 * Shorten a name for a message, into a buffer the caller owns.
 *
 * A predicate or variable name is an identifier of arbitrary length.  Left
 * whole, a long one would push the rest of the sentence past the end of the
 * diagnostic buffer, so a message would lose its meaning rather than a
 * detail.  What is cut instead is the name, and the cut is marked: a reader
 * who sees "..." knows they are not looking at the whole spelling, and
 * advice that quotes one -- the singleton warning's suggested rename is the
 * case that exists -- is visibly incomplete rather than quietly naming a
 * variable that does not exist.
 *
 * Returns `buffer`, so the call can stand as an argument of the message it
 * decorates.
 */
const char *
relic_diag_name(const char *name, char *buffer, size_t size)
{
	if (strlcpy(buffer, name, size) >= size)
		relic_diag_mark_cut(buffer, size);

	return (buffer);
}

/*
 * Mark a name cut to fill all of `buffer` as cut, by ending it in "...".  A
 * buffer too small to hold the mark is not one any caller passes, and a name
 * with no room to say so is better short than unwritten.
 */
void
relic_diag_mark_cut(char *buffer, size_t size)
{
	static const char ellipsis[] = "...";

	if (size > sizeof(ellipsis))
		bcopy(ellipsis, buffer + size - sizeof(ellipsis),
		    sizeof(ellipsis));
}

/*
 * The text one byte of a string takes in a quoted literal, into `piece`,
 * which has room for four bytes and a terminator.  Returns its length.
 */
static size_t
string_piece(unsigned char byte, char *piece, size_t size)
{
	if (byte == '\\' || byte == '"')
		snprintf(piece, size, "\\%c", (int)byte);
	else if (byte >= ' ' && byte <= '~')
		snprintf(piece, size, "%c", (int)byte);
	else
		snprintf(piece, size, "\\x%02x", (unsigned int)byte);

	return (strlen(piece));
}

/*
 * A string written as the literal that spells it, the way explain writes one,
 * so that a message quotes what the program wrote.  A string whose literal
 * does not fit is cut after its last byte that does, and the cut is marked
 * after the closing quote, `"abc"...`, so that the quote still says where
 * the bytes shown end.  A buffer too small for the opening quote, the mark
 * and a terminator is not one any caller passes, and is left empty.
 */
const char *
relic_diag_string(const char *bytes, size_t length, char *buffer, size_t size)
{
	static const char cut_mark[] = "\"...";
	char piece[5];
	size_t whole;
	size_t used;
	size_t room;
	size_t i;

	if (size < 1 + sizeof(cut_mark)) {
		if (size > 0)
			buffer[0] = '\0';
		return (buffer);
	}

	whole = 2;
	for (i = 0; i < length; i++)
		whole += string_piece((unsigned char)bytes[i], piece,
		    sizeof(piece));

	room = whole < size ? size - 2 : size - sizeof(cut_mark);

	buffer[0] = '"';
	used = 1;
	for (i = 0; i < length; i++) {
		size_t piece_length;

		piece_length = string_piece((unsigned char)bytes[i], piece,
		    sizeof(piece));
		if (used + piece_length > room)
			break;
		bcopy(piece, buffer + used, piece_length);
		used += piece_length;
	}

	if (whole < size)
		strlcpy(buffer + used, "\"", size - used);
	else
		strlcpy(buffer + used, cut_mark, size - used);

	return (buffer);
}
