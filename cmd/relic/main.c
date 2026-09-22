/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
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

static int		 check_program(const char *program_name, int argc,
			    char *argv[]);
static void		*diag_context(const char *program_name);
static const char	*invoked_name(const char *argv0);
static bool		 is_printable_ascii(const char *text);
static int		 load_operands(relic_t *ctx, int argc, char *argv[]);
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
	fprintf(stderr, "usage: %s -c [file ...]\n", program_name);
	fprintf(stderr, "       %s -V\n", program_name);
}

/*
 * The program name as the diagnostic callback's context.
 *
 * relic_diag_stderr() takes a void * and reads it as a borrowed, read-only
 * name, so the qualifier has to come off somewhere; the round trip through
 * uintptr_t is what does it without a cast the compiler objects to.  The
 * name itself points either into argv or at a string literal, so it outlives
 * every diagnostic the context can produce.
 */
static void *
diag_context(const char *program_name)
{
	return ((void *)(uintptr_t)program_name);
}

/*
 * Load every operand, or standard input when there are none.
 *
 * Every file is attempted even after one of them fails, so that a run
 * reports the mistakes in all of the input rather than in whichever file
 * happened to be named first.
 */
static int
load_operands(relic_t *ctx, int argc, char *argv[])
{
	int status;
	int i;

	if (argc == 0)
		return (relic_load_file(ctx, stdin, "<stdin>"));

	status = 0;
	for (i = 0; i < argc; i++) {
		if (relic_load_path(ctx, argv[i]) != 0)
			status = -1;
	}

	return (status);
}

/*
 * Check the input: load it, report whatever is wrong with it, and evaluate
 * nothing.
 *
 * Validation runs only when every operand loaded.  A load is all or nothing,
 * so a file with a syntax error contributes no statements at all, and
 * checking what is left would report the predicates that file defined as
 * referenced and never defined.  Those complaints would be about the file
 * that is missing rather than about the program the user wrote.
 */
static int
check_program(const char *program_name, int argc, char *argv[])
{
	relic_t *ctx;
	int status;

	/*
	 * Reading a program from a terminal would leave the tool waiting for
	 * input with no prompt and nothing said.  Interactive use arrives as
	 * an option of its own.
	 */
	if (argc == 0 && isatty(STDIN_FILENO) == 1) {
		usage(program_name);
		return (RELIC_EXIT_USAGE);
	}

	ctx = relic_create();
	if (ctx == NULL) {
		fprintf(stderr, "%s: out of memory\n", program_name);
		return (RELIC_EXIT_ERROR);
	}
	relic_diag_set(ctx, relic_diag_stderr, diag_context(program_name));

	status = load_operands(ctx, argc, argv);
	if (status == 0)
		status = relic_check(ctx);

	relic_destroy(ctx);

	return (status == 0 ? RELIC_EXIT_OK : RELIC_EXIT_ERROR);
}

int
main(int argc, char *argv[])
{
	const char *program_name;
	int ch;
	bool check_only;
	bool print_version;

	program_name = invoked_name(argv[0]);
	check_only = false;
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

	while ((ch = getopt(argc, argv, "cV")) != -1) {
		switch (ch) {
		case 'c':
			check_only = true;
			break;
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

	if (print_version) {
		if (check_only || argc != 0) {
			usage(program_name);
			return (RELIC_EXIT_USAGE);
		}

		/*
		 * The version is printed under the package name rather than
		 * the invoked name: it identifies the software, which a
		 * rename does not change, and keeping it independent of
		 * argv[0] keeps the output the same whether the tool runs
		 * from the build tree or from its installed location.
		 */
		printf("%s %s\n", PACKAGE_NAME, relic_version());

		return (RELIC_EXIT_OK);
	}

	/*
	 * Checking is the only thing the tool can be asked to do so far, so
	 * it must be asked explicitly.  Evaluating a program is what a bare
	 * invocation will mean, and letting it mean checking in the meantime
	 * would make the eventual behaviour a change rather than an addition.
	 */
	if (!check_only) {
		usage(program_name);
		return (RELIC_EXIT_USAGE);
	}

	return (check_program(program_name, argc, argv));
}
