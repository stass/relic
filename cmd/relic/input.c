/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <sys/types.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifdef HAVE_LIBEDIT
#include <histedit.h>
#endif

#include "cmd.h"
#include "compat.h"

/*
 * Reading the interactive session's lines.
 *
 * A session reads standard input a line at a time.  When someone is typing
 * the lines, which is taken to be when standard input and standard error are
 * both a terminal, and the build has libedit, each line is read through
 * libedit, which prompts, lets the line be edited, and keeps a history of the
 * session's lines.  Otherwise a line is read with getline(3), after a prompt
 * on standard error when someone is typing and with no prompt when not, so
 * that a scripted session writes its answers and nothing else, and a session
 * whose standard error is a file does not fill it with prompts.  Should
 * libedit fail to start, the reader falls back to getline(3): line editing
 * is a convenience, and its absence must not stop a session.
 *
 * The two ways meet in line_reader_next(), which copies or keeps the line in
 * the reader's own buffer, NUL-terminated and without its newline.  What the
 * session does with a line cannot depend on how it was read.
 *
 * The prompt goes to standard error rather than standard output, so that
 * standard output carries only what the session answers even when it is
 * redirected to a file and the session is typed at a terminal.  No history
 * is kept between sessions.
 */

#define	PROMPT		"relic> "

/* How many of the session's lines libedit's history remembers. */
#define	HISTORY_LINES	500

/*
 * `editor` and `history` are libedit's, present only when lines are read
 * through it.  `buffer` holds the line last returned, grown to `capacity`
 * by getline(3) or by copying libedit's line into it.  `number` counts the
 * lines returned.  `typed` is whether someone is taken to be typing the
 * lines, which decides whether there is a prompt.
 */
struct line_reader {
#ifdef HAVE_LIBEDIT
	EditLine	*editor;
	History		*history;
#endif
	char		*buffer;
	const char	*program_name;
	size_t		 capacity;
	size_t		 number;
	bool		 typed;
};

static int	 read_line(line_reader_t *reader, size_t *lengthp);
static int	 read_plain(line_reader_t *reader, size_t *lengthp);
static void	 report_read_failure(const line_reader_t *reader);

#ifdef HAVE_LIBEDIT
static char	*editor_prompt(EditLine *editor);
static void	 open_editor(line_reader_t *reader);
static int	 read_edited(line_reader_t *reader, size_t *lengthp);
static int	 keep_line(line_reader_t *reader, const char *line,
		    size_t length);

/*
 * The prompt libedit shows.  Its interface asks for a modifiable string and
 * never modifies it; the round trip through uintptr_t drops the qualifier
 * without a cast the compiler objects to.
 */
static char *
editor_prompt(EditLine *editor RELIC_CMD_UNUSED)
{
	return ((char *)(uintptr_t)PROMPT);
}

/*
 * Start libedit on the terminal: emacs key bindings unless the user's editrc
 * chooses otherwise, a history of the session's lines, and the terminal
 * restored when a signal ends the process.  When libedit cannot be started,
 * whatever was started is released and `editor` stays NULL, which leaves the
 * reader reading plainly.
 */
static void
open_editor(line_reader_t *reader)
{
	HistEvent event;

	reader->history = history_init();
	if (reader->history == NULL)
		return;
	history(reader->history, &event, H_SETSIZE, HISTORY_LINES);

	reader->editor = el_init(reader->program_name, stdin, stderr, stderr);
	if (reader->editor == NULL) {
		history_end(reader->history);
		reader->history = NULL;
		return;
	}

	el_set(reader->editor, EL_EDITOR, "emacs");
	el_set(reader->editor, EL_SIGNAL, 1);
	el_set(reader->editor, EL_PROMPT, editor_prompt);
	el_set(reader->editor, EL_HIST, history, reader->history);
	el_source(reader->editor, NULL);
}

/*
 * Copy a line libedit returned into the reader's buffer, whose contents
 * libedit does not own and the caller may therefore terminate early.
 * Returns -1 having reported that memory ran out.
 */
static int
keep_line(line_reader_t *reader, const char *line, size_t length)
{
	char *buffer;

	if (length + 1 > reader->capacity) {
		buffer = realloc(reader->buffer, length + 1);
		if (buffer == NULL) {
			report_error(reader->program_name, "out of memory");
			return (-1);
		}
		reader->buffer = buffer;
		reader->capacity = length + 1;
	}

	bcopy(line, reader->buffer, length);
	reader->buffer[length] = '\0';

	return (0);
}

/*
 * Read a line through libedit and remember it in the history unless it is
 * empty.  libedit's count is in characters rather than bytes once a line
 * holds UTF-8, so the length is measured instead; a line typed at a terminal
 * cannot hold a NUL for the measurement to stop at.
 */
static int
read_edited(line_reader_t *reader, size_t *lengthp)
{
	HistEvent event;
	const char *line;
	size_t length;
	int count;

	line = el_gets(reader->editor, &count);
	if (count < 0) {
		report_read_failure(reader);
		return (-1);
	}
	if (line == NULL || count == 0)
		return (0);

	length = strlen(line);
	if (keep_line(reader, line, length) != 0)
		return (-1);

	if (strspn(line, " \t\n") != length)
		history(reader->history, &event, H_ENTER, line);

	*lengthp = length;

	return (1);
}
#endif

static void
report_read_failure(const line_reader_t *reader)
{
	report_error(reader->program_name, "cannot read standard input: %s",
	    strerror(errno));
}

/*
 * Read a line with getline(3), which counts bytes, NULs included, so that
 * the session can refuse a line holding one.  getline(3) fails without
 * setting the stream's error indicator when it cannot grow its buffer, so
 * whatever failure is not the end of input is a read that failed.
 */
static int
read_plain(line_reader_t *reader, size_t *lengthp)
{
	ssize_t length;

	if (reader->typed) {
		fputs(PROMPT, stderr);
		fflush(stderr);
	}

	length = getline(&reader->buffer, &reader->capacity, stdin);
	if (length < 0) {
		if (feof(stdin) && !ferror(stdin))
			return (0);
		report_read_failure(reader);
		return (-1);
	}

	*lengthp = (size_t)length;

	return (1);
}

static int
read_line(line_reader_t *reader, size_t *lengthp)
{
#ifdef HAVE_LIBEDIT
	if (reader->editor != NULL)
		return (read_edited(reader, lengthp));
#endif

	return (read_plain(reader, lengthp));
}

line_reader_t *
line_reader_open(const char *program_name)
{
	line_reader_t *reader;

	reader = calloc(1, sizeof(*reader));
	if (reader == NULL) {
		report_error(program_name, "out of memory");
		return (NULL);
	}
	reader->program_name = program_name;
	reader->typed = isatty(STDIN_FILENO) == 1 &&
	    isatty(STDERR_FILENO) == 1;

#ifdef HAVE_LIBEDIT
	if (reader->typed)
		open_editor(reader);
#endif

	return (reader);
}

/*
 * The next line, without the newline that ended it.  Text left without a
 * newline at the end of input is a line like any other.
 *
 * At the end of typed input, a newline goes to standard error so that
 * whatever runs next does not begin on the line of the last prompt.
 */
int
line_reader_next(line_reader_t *reader, char **linep, size_t *lengthp)
{
	size_t length;
	int status;

	status = read_line(reader, &length);
	if (status == 0 && reader->typed)
		fputc('\n', stderr);
	if (status != 1)
		return (status);

	if (length > 0 && reader->buffer[length - 1] == '\n') {
		length--;
		reader->buffer[length] = '\0';
	}
	reader->number++;

	*linep = reader->buffer;
	*lengthp = length;

	return (1);
}

size_t
line_reader_number(const line_reader_t *reader)
{
	return (reader->number);
}

void
line_reader_close(line_reader_t *reader)
{
	if (reader == NULL)
		return;

#ifdef HAVE_LIBEDIT
	if (reader->editor != NULL)
		el_end(reader->editor);
	if (reader->history != NULL)
		history_end(reader->history);
#endif
	free(reader->buffer);
	free(reader);
}
