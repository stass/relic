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
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cmd.h"
#include "compat.h"

/*
 * No output of a run may be the same file as anything else the run reads or
 * writes.
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
 * x, ./x, a symbolic link to x and a hard link to it are one file.
 */

/*
 * What a file's identity is made of, when it has one.
 */
enum file_kind {
	FILE_NONE,
	FILE_EXISTING,
	FILE_NEW
};

typedef struct file_identity file_identity_t;
typedef struct named_file named_file_t;

/*
 * Which file a path names, as far as an output replacing another file goes.
 *
 * An existing regular file (FILE_EXISTING) is its device and inode, which
 * every spelling of it shares.  A file that does not exist yet and that an
 * output would create (FILE_NEW) is the device and inode of the directory
 * it would be created in, with `name`, borrowed, its final component there:
 * x and ./x are one new file, and sub/../x another spelling of it.
 *
 * Anything else has no identity (FILE_NONE) and is the same file as
 * nothing.  A device, a FIFO or a socket holds no stored data for an output
 * to replace, so any number of outputs may write /dev/null.  A directory is
 * a file an output cannot open, and the run fails there.  A path stat(2)
 * cannot examine is left unidentified rather than refused: whatever opening
 * it comes to is reported when it is opened.  An input that does not exist
 * fails as it is read, before any output is opened.
 */
struct file_identity {
	const char	*name;
	dev_t		 dev;
	ino_t		 ino;
	enum file_kind	 kind;
};

/*
 * One file a command line reads or writes: its identity, whether the run
 * writes it, and how a message names it, `role` followed by `text`, as
 * "-o " and the path, or "the standard input" and nothing.  `text` is
 * borrowed from argv.
 */
struct named_file {
	file_identity_t	 id;
	const char	*role;
	const char	*text;
	bool		 written;
};

static void	identify_descriptor(int descriptor, file_identity_t *id);
static void	identify_new(const char *path, file_identity_t *id);
static void	identify_path(const char *path, bool written,
		    file_identity_t *id);
static void	identify_stat(const struct stat *st, file_identity_t *id);
static size_t	list_files(const command_files_t *files, named_file_t *list);
static named_file_t *name_descriptor(named_file_t *file, const char *role,
		    int descriptor, bool written);
static named_file_t *name_path(named_file_t *file, const char *role,
		    const char *text, const char *path, bool written);
static void	report_same_file(const char *program_name,
		    const named_file_t *later, const named_file_t *earlier);
static bool	same_file(const file_identity_t *a, const file_identity_t *b);

int
command_files_check(const char *program_name, const command_files_t *files)
{
	named_file_t *list;
	size_t count;
	size_t i;
	size_t j;

	/* Standard input and output, the -o file, and one per argument. */
	list = calloc(3 + files->program_count + files->import_count +
	    files->export_count, sizeof(*list));
	if (list == NULL) {
		report_error(program_name, "out of memory");
		return (RELIC_EXIT_ERROR);
	}
	count = list_files(files, list);

	for (i = 0; i < count; i++) {
		if (!list[i].written || list[i].id.kind == FILE_NONE)
			continue;
		for (j = 0; j < i; j++) {
			if (same_file(&list[i].id, &list[j].id)) {
				report_same_file(program_name, &list[i],
				    &list[j]);
				free(list);
				return (RELIC_EXIT_USAGE);
			}
		}
	}

	free(list);

	return (RELIC_EXIT_OK);
}

/*
 * Fill `list` with every file the command line reads and then every file it
 * writes, each identified, and return how many there are.  An import of `-`
 * reads standard input, which is listed once as itself.
 */
static size_t
list_files(const command_files_t *files, named_file_t *list)
{
	named_file_t *file;
	size_t i;

	file = list;
	if (files->reads_stdin)
		file = name_descriptor(file, "the standard input", STDIN_FILENO,
		    false);
	for (i = 0; i < files->program_count; i++)
		file = name_path(file, "the program file ", files->programs[i],
		    files->programs[i], false);
	for (i = 0; i < files->import_count; i++) {
		if (!import_operand_reads_stdin(&files->imports[i]))
			file = name_path(file, "-I ", files->imports[i].operand,
			    files->imports[i].path, false);
	}

	if (files->writes_stdout)
		file = name_descriptor(file, "the standard output", STDOUT_FILENO,
		    true);
	if (files->output_path != NULL)
		file = name_path(file, "-o ", files->output_path,
		    files->output_path, true);
	for (i = 0; i < files->export_count; i++)
		file = name_path(file, "-O ", files->exports[i].operand,
		    files->exports[i].path, true);

	return ((size_t)(file - list));
}

/*
 * Describe and identify the file at `path` in *file, named in a message by
 * `role` followed by `text`, the path or the operand that holds it, and
 * return the entry after it.
 */
static named_file_t *
name_path(named_file_t *file, const char *role, const char *text,
    const char *path, bool written)
{
	file->role = role;
	file->text = text;
	file->written = written;
	identify_path(path, written, &file->id);

	return (file + 1);
}

/*
 * Describe and identify standard input or output in *file, named by `role`
 * alone, and return the entry after it.
 */
static named_file_t *
name_descriptor(named_file_t *file, const char *role, int descriptor,
    bool written)
{
	file->role = role;
	file->text = "";
	file->written = written;
	identify_descriptor(descriptor, &file->id);

	return (file + 1);
}

/*
 * Identify the file at `path`.  Only a file the run writes can be a new
 * one: the identity of an input that does not exist does not matter, since
 * reading it fails first.
 *
 * A regular file is identified by the descriptor opening it gives, not by
 * stat(2) of its path alone.  A path that names a descriptor, such as
 * /dev/stdin or /dev/fd/1, opens the file the descriptor has open, but on
 * some systems stat(2) of it reports the device of the file system /dev is,
 * which would match nothing.  The file is opened only after stat(2) says it
 * is a regular file, without blocking, creating or truncating it and
 * without taking a controlling terminal, so nothing is written; a watcher
 * of the file sees it opened and closed.  It is opened the way the run will
 * use it, for writing when the run writes it, because a path naming a
 * descriptor opens only in a mode the descriptor allows.  A file that cannot
 * be opened or examined through its descriptor keeps what stat(2) said.
 */
static void
identify_path(const char *path, bool written, file_identity_t *id)
{
	struct stat opened;
	struct stat st;
	int descriptor;

	bzero(id, sizeof(*id));
	if (stat(path, &st) != 0) {
		if (written && errno == ENOENT)
			identify_new(path, id);
		return;
	}
	if (!S_ISREG(st.st_mode))
		return;

	descriptor = open(path, (written ? O_WRONLY : O_RDONLY) | O_NONBLOCK |
	    O_NOCTTY | O_CLOEXEC);
	if (descriptor >= 0) {
		if (fstat(descriptor, &opened) == 0)
			st = opened;
		close(descriptor);
	}
	identify_stat(&st, id);
}

/*
 * Identify the file at `path`, which does not exist, as the one an output
 * would create there: its directory and its final name.
 *
 * A path whose directory part is longer than PATH_MAX is left unidentified,
 * as one fopen(3) refuses with ENAMETOOLONG.  stat(2) follows a symbolic
 * link, so a link to a file that does not exist is identified as a new file
 * named by the link rather than by its target, and two outputs, one through
 * such a link and one to its target, are not caught.  Names are compared by
 * their bytes, so on a file system that ignores case or normalizes Unicode,
 * two spellings of one new name are two files here.
 */
static void
identify_new(const char *path, file_identity_t *id)
{
	struct stat st;
	char prefix[PATH_MAX];
	const char *directory;
	const char *name;
	const char *slash;
	size_t length;

	slash = strrchr(path, '/');
	if (slash == NULL) {
		directory = ".";
		name = path;
	} else {
		/* The directory of /x is /, the slash itself. */
		length = slash == path ? 1 : (size_t)(slash - path);
		if (length >= sizeof(prefix))
			return;
		bcopy(path, prefix, length);
		prefix[length] = '\0';
		directory = prefix;
		name = slash + 1;
	}

	/* A path ending in a slash names a directory, not a file to create. */
	if (name[0] == '\0')
		return;
	if (stat(directory, &st) != 0 || !S_ISDIR(st.st_mode))
		return;

	id->kind = FILE_NEW;
	id->dev = st.st_dev;
	id->ino = st.st_ino;
	id->name = name;
}

static void
identify_descriptor(int descriptor, file_identity_t *id)
{
	struct stat st;

	bzero(id, sizeof(*id));
	if (fstat(descriptor, &st) == 0)
		identify_stat(&st, id);
}

static void
identify_stat(const struct stat *st, file_identity_t *id)
{
	if (!S_ISREG(st->st_mode))
		return;

	id->kind = FILE_EXISTING;
	id->dev = st->st_dev;
	id->ino = st->st_ino;
}

static bool
same_file(const file_identity_t *a, const file_identity_t *b)
{
	if (a->kind == FILE_NONE || a->kind != b->kind)
		return (false);
	if (a->dev != b->dev || a->ino != b->ino)
		return (false);

	return (a->kind == FILE_EXISTING || strcmp(a->name, b->name) == 0);
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
