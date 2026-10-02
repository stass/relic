/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Run a command on a pseudo-terminal, as someone typing would.
 *
 *	pty [-p prompt] command [argument ...]
 *
 * The command's standard input, output and error are the terminal, which is
 * its controlling terminal, so it sees what it would see in an interactive
 * shell.  The lines of this program's standard input are typed into it, and
 * everything it writes to the terminal is copied to this program's standard
 * output, carriage returns and all.  The terminal does not echo, so what
 * comes back is what the command wrote.
 *
 * Typing ahead is a race: a line written before the command reads it may be
 * read in another mode, or the end of input taken before the command has
 * started, or taken by a line editor as a key that edits the line.  With -p
 * each line, and the end of input after the last, is typed only once the
 * command has written `prompt` since the line before and then written
 * nothing for QUIET_MILLISECONDS, which is when a prompting program is
 * waiting for one; a line editor that draws its prompt twice is still typed
 * into once.  The quiet is a heuristic rather than a proof: a command that
 * stalls for that long between its prompt and its read is typed into early.
 * Without -p everything is typed at once, for a command that reads no input
 * or exits first.
 *
 * The options of this program end at the command, so that options of the
 * command's own are passed to it rather than read here, by a getopt(3) that
 * permutes its arguments as well as by one that does not.
 *
 * The end of input is control-D, the end-of-file character of a terminal in
 * its default modes and the key a line editor ends its input on.  It is not
 * read from the terminal's modes: by the time it is typed a line editor may
 * have put the terminal in a mode where that setting means something else.
 *
 * This program exits with the command's status; 124 when the system has no
 * pseudo-terminal to give, so that a test can tell a system it cannot run on
 * from a failure; or 125 when the command does not finish within
 * TIMEOUT_SECONDS, so that a command left waiting fails its test rather than
 * hanging it.  A command left behind by an early exit is not left running:
 * this program holds the only descriptor of the master side, whose closing
 * hangs up the terminal and sends the command SIGHUP.
 */

#include "config.h"

#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define	TIMEOUT_SECONDS		20
#define	QUIET_MILLISECONDS	200
#define	EXIT_NO_TERMINAL	124
#define	EXIT_TIMEOUT		125
#define	END_OF_FILE		'\004'	/* control-D */

/*
 * What this program's standard input holds, to be typed a line at a time.
 */
typedef struct typing {
	char	*text;
	size_t	 length;
	size_t	 typed;		/* bytes of `text` written so far */
	bool	 ended;		/* the end-of-file character has been sent */
} typing_t;

/*
 * The search for the prompt in what the command writes: how much of it the
 * output read so far ends with, so that a prompt split across two reads is
 * still found.
 */
typedef struct prompt_watch {
	const char	*prompt;
	size_t		 length;
	size_t		 matched;
} prompt_watch_t;

static void	 usage(void);
static void	 read_all(int fd, typing_t *typing);
static int	 open_terminal(char **slave_namep);
static pid_t	 start_command(int master, const char *slave_name,
		    char *argv[]);
static bool	 prompt_seen(prompt_watch_t *watch, const char *bytes,
		    size_t count);
static void	 type_next(int master, typing_t *typing);
static void	 write_all(int fd, const char *bytes, size_t count);
static int	 command_status(pid_t pid);
static void	 on_timeout(int signal_number);

static void
usage(void)
{
	fprintf(stderr, "usage: pty [-p prompt] command [argument ...]\n");
	exit(2);
}

/*
 * Read everything on `fd` into `typing`.
 */
static void
read_all(int fd, typing_t *typing)
{
	size_t capacity;
	ssize_t count;
	char *grown;

	capacity = 0;
	for (;;) {
		if (typing->length == capacity) {
			capacity = capacity == 0 ? 4096 : capacity * 2;
			grown = realloc(typing->text, capacity);
			if (grown == NULL)
				err(1, "reading the input to type");
			typing->text = grown;
		}

		count = read(fd, typing->text + typing->length,
		    capacity - typing->length);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			err(1, "reading the input to type");
		if (count == 0)
			return;
		typing->length += (size_t)count;
	}
}

/*
 * Open a pseudo-terminal, returning its master side and the name of its
 * slave side.
 */
static int
open_terminal(char **slave_namep)
{
	char *name;
	int master;

	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0)
		err(EXIT_NO_TERMINAL, "posix_openpt");
	if (master >= FD_SETSIZE)
		errx(1, "the pseudo-terminal's descriptor %d is too large to "
		    "wait on", master);
	if (grantpt(master) != 0)
		err(1, "grantpt");
	if (unlockpt(master) != 0)
		err(1, "unlockpt");

	name = ptsname(master);
	if (name == NULL)
		err(1, "ptsname");
	*slave_namep = strdup(name);
	if (*slave_namep == NULL)
		err(1, "ptsname");

	return (master);
}

/*
 * Start the command in a session of its own whose controlling terminal is
 * the slave side, with echo turned off, as its standard input, output and
 * error.  The command does not keep the master side, so that this program's
 * exit is the terminal's hangup.
 */
static pid_t
start_command(int master, const char *slave_name, char *argv[])
{
	struct termios modes;
	pid_t pid;
	int slave;

	pid = fork();
	if (pid < 0)
		err(1, "fork");
	if (pid > 0)
		return (pid);

	close(master);
	if (setsid() < 0)
		err(1, "setsid");
	slave = open(slave_name, O_RDWR);
	if (slave < 0)
		err(1, "%s", slave_name);
#ifdef TIOCSCTTY
	(void)ioctl(slave, TIOCSCTTY, 0);
#endif

	if (tcgetattr(slave, &modes) != 0)
		err(1, "tcgetattr");
	modes.c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK | ECHONL);
	if (tcsetattr(slave, TCSANOW, &modes) != 0)
		err(1, "tcsetattr");

	if (dup2(slave, STDIN_FILENO) < 0)
		err(1, "the terminal as standard input");
	if (dup2(slave, STDOUT_FILENO) < 0)
		err(1, "the terminal as standard output");
	if (dup2(slave, STDERR_FILENO) < 0)
		err(1, "the terminal as standard error");
	if (slave > STDERR_FILENO)
		close(slave);

	execvp(argv[0], argv);
	err(126, "%s", argv[0]);
}

/*
 * Whether `bytes` complete the prompt, carrying a partial match over from
 * the bytes before them.
 */
static bool
prompt_seen(prompt_watch_t *watch, const char *bytes, size_t count)
{
	bool seen;
	size_t i;

	seen = false;
	for (i = 0; i < count; i++) {
		if (bytes[i] == watch->prompt[watch->matched])
			watch->matched++;
		else
			watch->matched = bytes[i] == watch->prompt[0] ? 1 : 0;

		if (watch->matched == watch->length) {
			seen = true;
			watch->matched = 0;
		}
	}

	return (seen);
}

/*
 * Type the next line of the input, or, when every line has been typed, the
 * end-of-file character.
 */
static void
type_next(int master, typing_t *typing)
{
	const char end_of_file = END_OF_FILE;
	const char *newline;
	size_t count;

	if (typing->typed < typing->length) {
		newline = memchr(typing->text + typing->typed, '\n',
		    typing->length - typing->typed);
		count = newline == NULL ? typing->length - typing->typed :
		    (size_t)(newline - (typing->text + typing->typed)) + 1;
		write_all(master, typing->text + typing->typed, count);
		typing->typed += count;
		return;
	}

	if (typing->ended)
		return;
	write_all(master, &end_of_file, 1);
	typing->ended = true;
}

static void
write_all(int fd, const char *bytes, size_t count)
{
	ssize_t written;

	while (count > 0) {
		written = write(fd, bytes, count);
		if (written < 0 && errno == EINTR)
			continue;
		if (written < 0)
			err(1, "typing into the terminal");
		bytes += written;
		count -= (size_t)written;
	}
}

/*
 * The command's exit status, as a shell reports it: 128 plus the signal
 * for one that was killed.
 */
static int
command_status(pid_t pid)
{
	int status;

	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			err(1, "waitpid");
	}

	if (WIFEXITED(status))
		return (WEXITSTATUS(status));
	if (WIFSIGNALED(status))
		return (128 + WTERMSIG(status));

	return (1);
}

static void
on_timeout(int signal_number)
{
	(void)signal_number;
	_exit(EXIT_TIMEOUT);
}

int
main(int argc, char *argv[])
{
	prompt_watch_t watch;
	typing_t typing;
	struct timeval quiet;
	fd_set readable;
	char buffer[4096], *slave_name;
	ssize_t count;
	pid_t pid;
	bool prompted;
	int master, option, ready;

	/*
	 * The + stops a getopt that permutes at the first operand, the
	 * command; one that does not stops there already and takes the + as
	 * an option of its own, which usage() then rejects.
	 */
	bzero(&watch, sizeof(watch));
	while ((option = getopt(argc, argv, "+p:")) != -1) {
		switch (option) {
		case 'p':
			watch.prompt = optarg;
			watch.length = strlen(optarg);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc == 0 || (watch.prompt != NULL && watch.length == 0))
		usage();

	bzero(&typing, sizeof(typing));
	read_all(STDIN_FILENO, &typing);

	signal(SIGALRM, on_timeout);
	alarm(TIMEOUT_SECONDS);

	master = open_terminal(&slave_name);
	pid = start_command(master, slave_name, argv);

	/*
	 * Without a prompt to wait for, everything is typed now; with one,
	 * each line waits for the prompt and the quiet after it.  The
	 * command's output is copied until its side of the terminal is
	 * closed, which a read reports as the end of input or, on Linux, as
	 * EIO; any other error is this program's.  The waiting is select(2)'s,
	 * since poll(2) does not report a terminal's readiness on macOS.
	 */
	if (watch.prompt == NULL) {
		while (!typing.ended)
			type_next(master, &typing);
	}

	prompted = false;
	for (;;) {
		FD_ZERO(&readable);
		FD_SET(master, &readable);
		quiet.tv_sec = 0;
		quiet.tv_usec = QUIET_MILLISECONDS * 1000;
		ready = select(master + 1, &readable, NULL, NULL,
		    prompted ? &quiet : NULL);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			err(1, "select");
		if (ready == 0) {
			type_next(master, &typing);
			prompted = false;
			continue;
		}

		count = read(master, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0 && errno != EIO)
			err(1, "reading from the terminal");
		if (count <= 0)
			break;

		write_all(STDOUT_FILENO, buffer, (size_t)count);
		if (watch.prompt != NULL &&
		    prompt_seen(&watch, buffer, (size_t)count))
			prompted = true;
	}

	free(slave_name);
	free(typing.text);

	return (command_status(pid));
}
