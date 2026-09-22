/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
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

static const char	*level_name(enum relic_diag_level level);
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

void
relic_diag_init(relic_diag_t *diag)
{
	bzero(diag, sizeof(*diag));
}

void
relic_diag_set(relic_t *ctx, relic_diag_cb callback, void *callback_ctx)
{
	ctx->diag.callback = callback;
	ctx->diag.callback_ctx = callback_ctx;
}

const char *
relic_error(relic_t *ctx)
{
	if (!ctx->diag.has_error)
		return (NULL);

	return (ctx->diag.last_error);
}

void
relic_diagf(relic_t *ctx, enum relic_diag_level level,
    const relic_location_t *where, const char *format, ...)
{
	va_list arguments;
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

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	if (level == RELIC_ERROR) {
		strlcpy(ctx->diag.last_error, message,
		    sizeof(ctx->diag.last_error));
		ctx->diag.has_error = true;
	}

	if (ctx->diag.callback != NULL)
		ctx->diag.callback(ctx->diag.callback_ctx, level, where,
		    message);
}

void
relic_diag_no_memory(relic_t *ctx)
{
	relic_diagf(ctx, RELIC_ERROR, NULL, "out of memory");
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
