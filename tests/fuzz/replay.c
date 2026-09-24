/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <err.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "fuzz.h"

/*
 * The replay driver: a main() for a fuzz target that passes each file named
 * on its command line to the target's entry point once, and for a directory,
 * each file in it.  It needs nothing but a C compiler, so the corpus of every
 * target is replayed by make check on every platform, under every sanitizer
 * build, whether or not libFuzzer exists there.  A target that aborts on an
 * input ends the run, and the name of the input it was replaying is on
 * standard error just before.
 */

static size_t	 replay_directory(const char *path);
static void	 replay_file(const char *path);
static void	 replay_path(const char *path, size_t *replayed);

int
main(int argc, char **argv)
{
	size_t replayed = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: replay file | directory ...\n");
		return (2);
	}

	for (int i = 1; i < argc; i++)
		replay_path(argv[i], &replayed);

	printf("%zu inputs replayed\n", replayed);
	return (0);
}

static void
replay_path(const char *path, size_t *replayed)
{
	struct stat status;

	if (stat(path, &status) == -1)
		err(1, "%s", path);

	if (S_ISDIR(status.st_mode)) {
		*replayed += replay_directory(path);
	} else {
		replay_file(path);
		(*replayed)++;
	}
}

/*
 * Replay every regular file directly in a directory.  Names beginning with a
 * dot are skipped, which leaves out the directory's own entries and the
 * editor and version-control files a corpus should not be taken to hold.
 */
static size_t
replay_directory(const char *path)
{
	struct dirent *entry;
	size_t replayed = 0;
	DIR *directory;

	directory = opendir(path);
	if (directory == NULL)
		err(1, "%s", path);

	while ((entry = readdir(directory)) != NULL) {
		char file[PATH_MAX];
		struct stat status;
		int length;

		if (entry->d_name[0] == '.')
			continue;
		length = snprintf(file, sizeof(file), "%s/%s", path,
		    entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(file))
			errx(1, "%s/%s: path too long", path, entry->d_name);
		if (stat(file, &status) == -1)
			err(1, "%s", file);
		if (!S_ISREG(status.st_mode))
			continue;

		replay_file(file);
		replayed++;
	}

	closedir(directory);
	return (replayed);
}

/* Read the whole file and hand it to the target. */
static void
replay_file(const char *path)
{
	uint8_t *data = NULL;
	size_t size = 0, capacity = 0;
	FILE *in;

	in = fopen(path, "rb");
	if (in == NULL)
		err(1, "%s", path);

	for (;;) {
		size_t got;

		if (size == capacity) {
			uint8_t *grown;

			capacity = capacity == 0 ? 4096 : capacity * 2;
			grown = realloc(data, capacity);
			if (grown == NULL)
				err(1, "%s", path);
			data = grown;
		}
		got = fread(data + size, 1, capacity - size, in);
		size += got;
		if (got == 0)
			break;
	}
	if (ferror(in))
		err(1, "%s", path);
	fclose(in);

	fprintf(stderr, "replaying %s\n", path);
	LLVMFuzzerTestOneInput(data, size);
	free(data);
}
