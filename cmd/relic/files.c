/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cmd.h"
#include "compat.h"

/*
 * Every file a run writes is opened before anything is read, and no output
 * of a run may be the same file as anything else the run reads or writes.
 *
 * An output that is an input -- a program file, an imported table, the file
 * standard input was redirected from -- would replace it, a program with
 * the rows of its own queries, or, for standard output appended to it,
 * leave the input holding rows that are not of its kind.  An output that is
 * another output would destroy what that one wrote, or interleave with it.
 * The command line is therefore refused, as a usage error and before
 * anything is read or written, when an output is the same file as anything
 * listed before it: the inputs first, then standard output, the -o file and
 * the exports in the order given.  Every pair of an output with another file
 * is compared, in one place, whichever options named them.
 *
 * "The same file" is decided by identity rather than by spelling, so that
 * x, ./x, a symbolic or hard link to x, and two names of x that differ only
 * in what the file system ignores are one file.  An output's identity is
 * read from the descriptor opening it gave, so a file that did not exist
 * before the run has one too: opening it created it, once, whatever names
 * lead there.
 *
 * Opening an output neither truncates it nor waits for a FIFO's reader.  A
 * file is truncated when the run first writes to it, so a run that fails
 * before then leaves an existing file's content as it was, and removes a
 * file it created.
 */

/* The flags every output is opened with, and the mode of one created. */
#define	OUTPUT_FLAGS	(O_WRONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC)
#define	OUTPUT_MODE	0666	/* as fopen(3) creates a file, less the umask */

/* How many symbolic links to nothing an output's path is followed through. */
#define	OUTPUT_LINK_HOPS	32

typedef struct named_file named_file_t;

/*
 * One file a command line reads or writes, as the check compares it: its
 * device and inode when `identified`, whether it `is_output`, and how a
 * message names it, `role` followed by `text`, as "-o " and the path, or
 * "the standard input" and nothing.  `text` is borrowed from argv.
 *
 * Only a regular file is identified.  A device, a FIFO or a socket holds no
 * stored data for an output to replace, so any number of outputs may write
 * /dev/null; such a file is the same file as nothing.  An input stat(2)
 * cannot examine is left unidentified rather than refused: an input that
 * does not exist fails as it is read.
 */
struct named_file {
	const char	*role;
	const char	*text;
	dev_t		 dev;
	ino_t		 ino;
	bool		 identified;
	bool		 is_output;
};

static int	cut_at_offset(int descriptor);
static int	follow_link(char *name, size_t size);
static void	identify_descriptor(int descriptor, named_file_t *file);
static void	identify_input(const char *path, named_file_t *file);
static size_t	list_files(const command_files_t *files,
		    const output_files_t *outputs, named_file_t *list);
static named_file_t *name_file(named_file_t *file, const char *role,
		    const char *text, bool is_output);
static int	open_awaited(output_file_t *file, const char *program_name);
static int	open_creating(const char *path, bool *createdp);
static int	open_output(output_file_t *file);
static int	open_outputs(const char *program_name,
		    const command_files_t *files, output_files_t *outputs);
static void	remove_created(const output_file_t *file);
static int	refuse_same_file(const char *program_name,
		    const named_file_t *list, size_t count);
static void	report_same_file(const char *program_name,
		    const named_file_t *later, const named_file_t *earlier);

int
command_files_open(const char *program_name, const command_files_t *files,
    output_files_t *outputs)
{
	named_file_t *list;
	size_t count;
	int status;

	status = open_outputs(program_name, files, outputs);
	if (status != RELIC_EXIT_OK)
		return (status);

	/* Standard input and output, the -o file, and one per argument. */
	list = calloc(3 + files->program_count + files->import_count +
	    files->export_count, sizeof(*list));
	if (list == NULL) {
		report_error(program_name, "out of memory");
		(void)output_files_close(outputs, program_name);
		return (RELIC_EXIT_ERROR);
	}
	count = list_files(files, outputs, list);
	status = refuse_same_file(program_name, list, count);
	free(list);

	if (status != RELIC_EXIT_OK)
		(void)output_files_close(outputs, program_name);

	return (status);
}

/*
 * Open every file the run writes, the -o file first and then the exports in
 * order, into *outputs.  Returns RELIC_EXIT_OK, or RELIC_EXIT_ERROR having
 * reported the first that cannot be opened, or that memory ran out, and
 * closed every one opened before it as a run that wrote nothing does.
 */
static int
open_outputs(const char *program_name, const command_files_t *files,
    output_files_t *outputs)
{
	output_file_t *file;
	size_t count;
	size_t i;

	bzero(outputs, sizeof(*outputs));
	count = (files->output_path != NULL ? 1 : 0) + files->export_count;
	outputs->files = calloc(count > 0 ? count : 1, sizeof(*outputs->files));
	if (outputs->files == NULL) {
		report_error(program_name, "out of memory");
		return (RELIC_EXIT_ERROR);
	}
	outputs->count = count;
	outputs->export_count = files->export_count;

	file = outputs->files;
	if (files->output_path != NULL) {
		outputs->results = file;
		file++->path = files->output_path;
	}
	outputs->exports = file;
	for (i = 0; i < files->export_count; i++)
		file++->path = files->exports[i].path;

	for (i = 0; i < outputs->count; i++) {
		file = &outputs->files[i];
		file->descriptor = -1;
		file->state = OUTPUT_FILE_CLOSED;
	}

	for (i = 0; i < outputs->count; i++) {
		file = &outputs->files[i];
		if (open_output(file) != 0) {
			report_error(program_name, "cannot open %s: %s",
			    file->path, strerror(errno));
			(void)output_files_close(outputs, program_name);
			return (RELIC_EXIT_ERROR);
		}
	}

	return (RELIC_EXIT_OK);
}

/*
 * Open the file at `path` for writing, created when it does not exist, and
 * record whether this run created it.  Returns 0, or -1 with errno set.
 *
 * Nothing is truncated, and nothing waits: a FIFO with no reader yet, which
 * open(2) without O_NONBLOCK would wait on, is left OUTPUT_FILE_AWAITED, to
 * be opened when the run first writes to it.  O_NONBLOCK is cleared once the
 * file is open, so that writes wait as they would on any output.  A path
 * naming a descriptor whose file description the open shares, /dev/stdout
 * on macOS and the BSDs, has it cleared on that description as well, which
 * an output that is to be written whole wants of it anyway.
 */
static int
open_output(output_file_t *file)
{
	struct stat st;
	int descriptor;
	int flags;

	descriptor = open_creating(file->path, &file->created);
	if (descriptor < 0) {
		if (errno != ENXIO)
			return (-1);
		if (stat(file->path, &st) != 0 || !S_ISFIFO(st.st_mode)) {
			errno = ENXIO;
			return (-1);
		}
		file->state = OUTPUT_FILE_AWAITED;
		return (0);
	}

	file->descriptor = descriptor;
	file->state = OUTPUT_FILE_HELD;

	flags = fcntl(descriptor, F_GETFL);
	if (flags == -1 || fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK) == -1)
		return (-1);

	return (0);
}

/*
 * Open `path` for writing as an output, creating the file it names when
 * there is none, and set *createdp to whether this open created it.
 * Returns the descriptor, or -1 with errno set.
 *
 * Only an O_EXCL open is taken to create a file, since it succeeds only
 * when it is what creates the file.  A path that exists is opened as it is.
 * A path that exists but does not open is a symbolic link to a file that
 * does not exist, and the link is followed by hand to the name it gives,
 * which is created with O_EXCL in turn, so a file another process creates
 * there meanwhile is opened as existing rather than taken for this run's.
 */
static int
open_creating(const char *path, bool *createdp)
{
	char name[PATH_MAX];
	int descriptor;
	int hops;

	if (strlcpy(name, path, sizeof(name)) >= sizeof(name)) {
		errno = ENAMETOOLONG;
		return (-1);
	}

	*createdp = false;
	for (hops = 0; hops < OUTPUT_LINK_HOPS; hops++) {
		descriptor = open(name, OUTPUT_FLAGS | O_CREAT | O_EXCL,
		    OUTPUT_MODE);
		if (descriptor >= 0) {
			*createdp = true;
			return (descriptor);
		}
		if (errno != EEXIST)
			return (-1);

		descriptor = open(name, OUTPUT_FLAGS);
		if (descriptor >= 0 || errno != ENOENT)
			return (descriptor);

		/* A name gone between the opens is tried again as it is. */
		if (follow_link(name, sizeof(name)) != 0 && errno != EINVAL &&
		    errno != ENOENT)
			return (-1);
	}

	errno = ELOOP;
	return (-1);
}

/*
 * Replace `name`, a symbolic link, with the path it points to, taken
 * relative to the link's directory unless it is absolute.  Returns 0, or -1
 * with errno set: EINVAL when `name` is not a link, ENAMETOOLONG when the
 * path does not fit in `size` bytes.
 */
static int
follow_link(char *name, size_t size)
{
	char target[PATH_MAX];
	const char *slash;
	size_t directory;
	ssize_t length;

	length = readlink(name, target, sizeof(target));
	if (length < 0)
		return (-1);
	if ((size_t)length == sizeof(target)) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	target[length] = '\0';

	slash = strrchr(name, '/');
	directory = target[0] == '/' || slash == NULL ? 0 :
	    (size_t)(slash - name) + 1;
	if (directory + (size_t)length >= size) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	bcopy(target, name + directory, (size_t)length + 1);

	return (0);
}

/*
 * Fill `list` with every file the command line reads and then every file it
 * writes, each identified, and return how many there are.  An import of `-`
 * reads standard input, which is listed once as itself.  An output is
 * identified by the descriptor it is open at; a FIFO still awaiting its
 * reader has none, and no identity either.
 */
static size_t
list_files(const command_files_t *files, const output_files_t *outputs,
    named_file_t *list)
{
	named_file_t *file;
	size_t i;

	file = list;
	if (files->reads_stdin) {
		identify_descriptor(STDIN_FILENO, file);
		file = name_file(file, "the standard input", "", false);
	}
	for (i = 0; i < files->program_count; i++) {
		identify_input(files->programs[i], file);
		file = name_file(file, "the program file ", files->programs[i],
		    false);
	}
	for (i = 0; i < files->import_count; i++) {
		if (import_operand_reads_stdin(&files->imports[i]))
			continue;
		identify_input(files->imports[i].path, file);
		file = name_file(file, "-I ", files->imports[i].operand, false);
	}

	if (files->writes_stdout) {
		identify_descriptor(STDOUT_FILENO, file);
		file = name_file(file, "the standard output", "", true);
	}
	if (outputs->results != NULL) {
		identify_descriptor(outputs->results->descriptor, file);
		file = name_file(file, "-o ", files->output_path, true);
	}
	for (i = 0; i < files->export_count; i++) {
		identify_descriptor(outputs->exports[i].descriptor, file);
		file = name_file(file, "-O ", files->exports[i].operand, true);
	}

	return ((size_t)(file - list));
}

/*
 * Say how a message names the file *file identifies, `role` followed by
 * `text`, the path or the operand that holds it, and whether the run writes
 * it, and return the entry after it.
 */
static named_file_t *
name_file(named_file_t *file, const char *role, const char *text,
    bool is_output)
{
	file->role = role;
	file->text = text;
	file->is_output = is_output;

	return (file + 1);
}

/*
 * Identify the input at `path`, which the run opens only later.
 *
 * A regular file is identified by the descriptor opening it gives, not by
 * stat(2) of its path alone.  A path that names a descriptor, such as
 * /dev/stdin or /dev/fd/0, opens the file the descriptor has open, but on
 * some systems stat(2) of it reports the device of the file system /dev is,
 * which would match nothing.  The file is opened only after stat(2) says it
 * is a regular file, for reading, without blocking or taking a controlling
 * terminal, so nothing is changed; a watcher of the file sees it opened and
 * closed.  A file that cannot be opened keeps what stat(2) said.
 */
static void
identify_input(const char *path, named_file_t *file)
{
	struct stat st;
	int descriptor;

	bzero(file, sizeof(*file));
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return;

	descriptor = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
	if (descriptor >= 0) {
		identify_descriptor(descriptor, file);
		close(descriptor);
		if (file->identified)
			return;
	}

	file->identified = true;
	file->dev = st.st_dev;
	file->ino = st.st_ino;
}

/*
 * Identify the file open at `descriptor`, which is -1 for an output that
 * has none yet.
 */
static void
identify_descriptor(int descriptor, named_file_t *file)
{
	struct stat st;

	bzero(file, sizeof(*file));
	if (descriptor < 0 || fstat(descriptor, &st) != 0 ||
	    !S_ISREG(st.st_mode))
		return;

	file->identified = true;
	file->dev = st.st_dev;
	file->ino = st.st_ino;
}

/*
 * Refuse the first output in `list` that is the same file as one listed
 * before it.  Returns RELIC_EXIT_OK, or RELIC_EXIT_USAGE having reported the
 * pair.
 */
static int
refuse_same_file(const char *program_name, const named_file_t *list,
    size_t count)
{
	const named_file_t *earlier;
	const named_file_t *later;

	for (later = list; later < list + count; later++) {
		if (!later->is_output || !later->identified)
			continue;
		for (earlier = list; earlier < later; earlier++) {
			if (earlier->identified && earlier->dev == later->dev &&
			    earlier->ino == later->ino) {
				report_same_file(program_name, later, earlier);
				return (RELIC_EXIT_USAGE);
			}
		}
	}

	return (RELIC_EXIT_OK);
}

static void
report_same_file(const char *program_name, const named_file_t *later,
    const named_file_t *earlier)
{
	cmd_quoted_t earlier_text;
	cmd_quoted_t later_text;

	cmd_quote(later->text, strlen(later->text), &later_text);
	cmd_quote(earlier->text, strlen(earlier->text), &earlier_text);
	report_error(program_name, "%s%s and %s%s are the same file",
	    later->role, later_text.text, earlier->role, earlier_text.text);
}

/*
 * Hand the file's descriptor to a stream, and then truncate the file, when
 * it is a regular file, which is the only kind that holds what an earlier
 * writer left.  A FIFO still awaiting its reader is opened first, waiting
 * for one.  The stream is made first because making it changes nothing, so
 * a failure there leaves the file as it was.
 *
 * The file is cut where the descriptor stands, which is its start for every
 * file this run opened afresh.  A path naming a descriptor, /dev/stdout on
 * macOS and the BSDs, opens the descriptor's own file description instead,
 * whose offset is past what was written to it before the run, and that is
 * kept; one open for appending is not cut at all.  Truncating at the start
 * would destroy what came before, and leave a hole of zero bytes up to the
 * offset the next write is made at.
 *
 * Once the stream holds the descriptor the file counts as written, even
 * when truncating it then fails, because only the stream can close it.
 */
FILE *
output_file_begin(output_file_t *file, const char *program_name)
{
	FILE *stream;

	if (file->state == OUTPUT_FILE_AWAITED &&
	    open_awaited(file, program_name) != 0)
		return (NULL);

	stream = fdopen(file->descriptor, "w");
	if (stream == NULL) {
		report_error(program_name, "cannot write %s: %s", file->path,
		    strerror(errno));
		return (NULL);
	}
	file->stream = stream;
	file->state = OUTPUT_FILE_WRITTEN;

	if (cut_at_offset(file->descriptor) != 0) {
		report_error(program_name, "cannot write %s: %s", file->path,
		    strerror(errno));
		return (NULL);
	}

	return (stream);
}

/*
 * Truncate the regular file open at `descriptor` where the descriptor
 * stands, unless it is open for appending, and leave any other kind of file
 * as it is.  Returns 0, or -1 with errno set.
 */
static int
cut_at_offset(int descriptor)
{
	struct stat st;
	off_t offset;
	int flags;

	if (fstat(descriptor, &st) != 0)
		return (-1);
	if (!S_ISREG(st.st_mode))
		return (0);

	flags = fcntl(descriptor, F_GETFL);
	if (flags == -1)
		return (-1);
	if ((flags & O_APPEND) != 0)
		return (0);

	offset = lseek(descriptor, 0, SEEK_CUR);
	if (offset == -1)
		return (-1);

	return (ftruncate(descriptor, offset));
}

/*
 * Open the FIFO an output found no reader for when the run began, waiting
 * for one now.  Returns 0, or -1 having reported why it cannot be.
 *
 * The check of every output against every other file was made when the run
 * began, and this file had no identity then.  A regular file found here now
 * escaped that check, and could be a file the run reads, so it is refused.
 */
static int
open_awaited(output_file_t *file, const char *program_name)
{
	struct stat st;
	int descriptor;

	descriptor = open(file->path, O_WRONLY | O_NOCTTY | O_CLOEXEC);
	if (descriptor < 0 || fstat(descriptor, &st) != 0) {
		report_error(program_name, "cannot open %s: %s", file->path,
		    strerror(errno));
		if (descriptor >= 0)
			close(descriptor);
		return (-1);
	}
	if (S_ISREG(st.st_mode)) {
		report_error(program_name, "cannot open %s: it became a "
		    "regular file after the run began", file->path);
		close(descriptor);
		return (-1);
	}

	file->descriptor = descriptor;
	file->state = OUTPUT_FILE_HELD;

	return (0);
}

/*
 * A file the run wrote is closed, and the result of closing it returned.  A
 * file it did not write is closed as it was found: its content untouched,
 * or, when this run created it, removed.
 */
int
output_file_close(output_file_t *file)
{
	int status;

	status = 0;
	switch (file->state) {
	case OUTPUT_FILE_CLOSED:
	case OUTPUT_FILE_AWAITED:
		break;
	case OUTPUT_FILE_HELD:
		if (file->created)
			remove_created(file);
		(void)close(file->descriptor);
		break;
	case OUTPUT_FILE_WRITTEN:
		status = fclose(file->stream);
		break;
	}

	file->stream = NULL;
	file->descriptor = -1;
	file->state = OUTPUT_FILE_CLOSED;

	return (status);
}

/*
 * Remove the file this run created for an output it did not write, when the
 * path still leads to it.  The path is resolved first, because unlink(2) of
 * a symbolic link would remove the link and keep the file created through
 * it.  The directory the resolved name is in is then opened, and the name
 * is compared and removed in that directory, so that a directory renamed or
 * replaced by a link meanwhile cannot send the removal elsewhere.  A file
 * the name no longer is, because another process renamed or replaced it, is
 * left alone.
 */
static void
remove_created(const output_file_t *file)
{
	struct stat named;
	struct stat opened;
	char *resolved;
	char *slash;
	int directory;

	resolved = realpath(file->path, NULL);
	if (resolved == NULL)
		return;

	/* realpath(3) gives an absolute path: it has a slash. */
	slash = strrchr(resolved, '/');
	*slash = '\0';
	directory = open(slash == resolved ? "/" : resolved,
	    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory >= 0) {
		if (fstat(file->descriptor, &opened) == 0 &&
		    fstatat(directory, slash + 1, &named,
		    AT_SYMLINK_NOFOLLOW) == 0 &&
		    named.st_dev == opened.st_dev &&
		    named.st_ino == opened.st_ino)
			(void)unlinkat(directory, slash + 1, 0);
		close(directory);
	}
	free(resolved);
}

/*
 * Every file is closed by output_file_close(), and one an output already
 * closed is left as it is.  The output that wrote a file closes it and
 * reports a close that fails; one still written here, which an output
 * did not close, is closed and reported the same way, so that no close
 * that failed can go unreported.
 */
int
output_files_close(output_files_t *outputs, const char *program_name)
{
	output_file_t *file;
	int status;

	status = 0;
	for (file = outputs->files; file < outputs->files + outputs->count;
	    file++) {
		if (output_file_close(file) != 0) {
			report_error(program_name, "cannot close %s: %s",
			    file->path, strerror(errno));
			status = -1;
		}
	}
	free(outputs->files);
	bzero(outputs, sizeof(*outputs));

	return (status);
}
