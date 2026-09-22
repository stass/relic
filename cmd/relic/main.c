/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <relic.h>

/*
 * The tool reports exactly three outcomes: the requested work succeeded, the
 * program or query was rejected, or the command line was malformed.  Every
 * option added later maps onto this set rather than introducing a status of
 * its own, so that a caller in a shell script can branch on it.
 */
enum {
	RELIC_EXIT_OK = 0,
	RELIC_EXIT_ERROR = 1,
	RELIC_EXIT_USAGE = 2
};

static const char	*invoked_name(const char *argv0);
static bool		 is_printable_ascii(const char *text);
static void		 report_unknown_option(const char *program_name,
			    int option);
static void		 usage(const char *program_name);

/*
 * Whether every byte of text is printable ASCII, space included.
 */
static bool
is_printable_ascii(const char *text)
{
	size_t i;

	for (i = 0; text[i] != '\0'; i++) {
		if (text[i] < ' ' || text[i] > '~')
			return (false);
	}

	return (true);
}

/*
 * The trailing component of the path the program was invoked by, used in
 * diagnostics so that an installation renamed with --program-prefix reports
 * the name the user typed rather than one compiled into it.
 *
 * Anything unusable falls back to the package name: an absent or empty
 * argv[0], a path ending in a slash, and a name holding a byte that is not
 * printable ASCII.  That last case is the reason this is a function rather
 * than a use of strrchr at the call site.  argv[0] is chosen by whoever
 * executes the program, and its final component is no more than a filename,
 * so it can carry newlines or terminal escape sequences; printing one
 * unexamined would let an invocation retitle the reader's terminal or forge a
 * diagnostic line of its own.  Such a name is replaced rather than escaped,
 * because unlike an unknown option byte it tells the reader nothing worth
 * showing.  Every name this returns is therefore safe to print, and no caller
 * needs to remember that.
 *
 * The result is borrowed: it points either into argv[0] or to a string
 * literal, both of which outlive main, so no caller copies or frees it.
 */
static const char *
invoked_name(const char *argv0)
{
	const char *candidate;
	const char *last_slash;

	if (argv0 == NULL || argv0[0] == '\0')
		return (PACKAGE_NAME);

	last_slash = strrchr(argv0, '/');
	if (last_slash == NULL)
		candidate = argv0;
	else
		candidate = last_slash + 1;

	if (candidate[0] == '\0' || !is_printable_ascii(candidate))
		return (PACKAGE_NAME);

	return (candidate);
}

/*
 * Report an option the tool does not accept.
 *
 * optopt holds a byte taken straight from the command line and may be any
 * value at all, so a byte outside printable ASCII is reported by its value
 * rather than echoed: a diagnostic must not carry a terminal escape sequence
 * out of the argument vector and into the reader's terminal.
 */
static void
report_unknown_option(const char *program_name, int option)
{
	unsigned int byte;

	byte = (unsigned int)option & 0xffu;

	if (byte >= '!' && byte <= '~')
		fprintf(stderr, "%s: unknown option -- %c\n", program_name,
		    (int)byte);
	else
		fprintf(stderr, "%s: unknown option -- \\x%02x\n",
		    program_name, byte);
}

static void
usage(const char *program_name)
{
	fprintf(stderr, "usage: %s -V\n", program_name);
}

int
main(int argc, char *argv[])
{
	const char *program_name;
	int ch;
	bool print_version;

	program_name = invoked_name(argv[0]);
	print_version = false;

	/*
	 * getopt(3) reports an unknown option by printing argv[0], which is
	 * whatever path the program was invoked by.  Silencing it and
	 * reporting the option here keeps diagnostics identical whether the
	 * tool runs from an absolute path, from the build tree through a
	 * libtool wrapper, or from its installed location, which is what the
	 * golden output tests compare.
	 */
	opterr = 0;

	while ((ch = getopt(argc, argv, "V")) != -1) {
		switch (ch) {
		case 'V':
			print_version = true;
			break;
		default:
			report_unknown_option(program_name, optopt);
			usage(program_name);
			return (RELIC_EXIT_USAGE);
		}
	}
	argc -= optind;
	argv += optind;

	if (!print_version || argc != 0) {
		usage(program_name);
		return (RELIC_EXIT_USAGE);
	}

	/*
	 * The version is printed under the package name rather than the
	 * invoked name: it identifies the software, which a rename does not
	 * change, and keeping it independent of argv[0] keeps the output the
	 * same whether the tool runs from the build tree or from its
	 * installed location.
	 */
	printf("%s %s\n", PACKAGE_NAME, relic_version());

	return (RELIC_EXIT_OK);
}
