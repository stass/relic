#!/bin/sh
#
# No relic-owned source file contains a byte outside printable ASCII.
#
# The restriction is not cosmetic.  A typographic dash or quotation mark that
# reaches a diagnostic string changes what the golden test suite compares,
# renders differently depending on the reader's locale, and is invisible in a
# diff, so it is cheaper to reject the byte than to debug its consequences.
# Control characters are rejected for the same reason.
#
# The whole source tree is walked and the files the project does not own are
# excluded, rather than the project's own directories being listed.  A list of
# what to check has to be extended for every directory added to the tree, and
# forgetting costs nothing visible, because the check goes on passing.
# Excluding what is generated fails the other way: a generated file nobody
# accounted for is reported until it is named here, and that is the failure
# somebody notices.

set -eu

: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"

# Files matching one of these names are produced by autoconf, automake,
# libtool or a build, and their contents are not the project's to change.
is_generated() {
	case "$1" in
	aclocal.m4|libtool.m4|lt*.m4|Makefile.in|config.h|config.h.in)
		return 0
		;;
	esac

	return 1
}

# Every file whose suffix marks it as source, from anywhere in the tree except
# the directories that hold version-control state, autotools auxiliary
# scripts, build output and the disposable sanitizer trees.
owned_files() {
	find "$RELIC_SRCDIR" \
	    \( -name '.git' -o -name '.deps' -o -name '.libs' \
	       -o -name 'autom4te.cache' -o -name 'build-aux' \
	       -o -name 'build-*san' \) -prune -o \
	    -type f \( -name '*.ac' -o -name '*.am' -o -name '*.c' \
	       -o -name '*.h' -o -name '*.in' -o -name '*.m4' \
	       -o -name '*.sh' -o -name '*.[1-9]' -o -name '*.dl' \
	       -o -name '*.pl' -o -name '*.lp' -o -name '*.rls' \) -print
}

candidates=$(mktemp) || exit 1
trap 'rm -f "$candidates"' EXIT
trap 'exit 1' HUP INT TERM

owned_files > "$candidates"

if [ ! -s "$candidates" ]; then
	echo "no source files were found under $RELIC_SRCDIR" >&2
	exit 1
fi

# Every offending file is reported rather than only the first, so that one
# run tells the whole story.
status=0

while read -r file; do
	if is_generated "${file##*/}"; then
		continue
	fi

	# In the C locale every byte above 0x7f is unprintable, so one
	# expression rejects both non-ASCII bytes and stray control
	# characters.  Tabs and spaces are the only whitespace a line may
	# contain.
	offending=$(LC_ALL=C grep -n '[^[:print:][:blank:]]' "$file" || true)

	if [ -n "$offending" ]; then
		echo "$file contains a byte outside printable ASCII:" >&2
		echo "$offending" >&2
		status=1
	fi
done < "$candidates"

exit "$status"
