#!/bin/sh
#
# The tool reports its version, distinguishes a malformed command line from a
# successful run, and keeps its diagnostics free of anything that varies with
# how it was invoked.
#
# The version check is also the only test that proves the tool resolves
# relic_version() out of the shared library: the symbol is reachable across
# the library boundary only if the export list and the visibility attribute
# agree, and neither shows up as a build failure when it is wrong.

set -eu

: "${RELIC:?RELIC must name the relic executable}"
: "${RELIC_BUILDDIR:?RELIC_BUILDDIR must name the build directory}"
: "${RELIC_VERSION:?RELIC_VERSION must hold the expected version}"

fail() {
	echo "$1" >&2
	exit 1
}

expected="relic $RELIC_VERSION"
actual=$("$RELIC" -V)

if [ "$actual" != "$expected" ]; then
	fail "relic -V printed '$actual', expected '$expected'"
fi

# Output the caller asked for goes to stdout; nothing else belongs there or
# on stderr alongside it.
noise=$("$RELIC" -V 2>&1 >/dev/null)

if [ -n "$noise" ]; then
	fail "relic -V wrote '$noise' to stderr, expected nothing"
fi

# An unknown option is a usage error, which the tool reports as status 2 and
# keeps distinct from the status 1 it uses for a rejected program or query.
# So is an operand -V cannot act on, an option missing its argument, a format
# that does not exist, and -c with -a, which contradict each other.
for arguments in '-Z' '-V extra' '-V -c' '-q' '-F bogus' '-c -a'; do
	status=0
	# Word splitting is wanted here: each case is a whole command line.
	# shellcheck disable=SC2086
	"$RELIC" $arguments >/dev/null 2>&1 || status=$?

	if [ "$status" -ne 2 ]; then
		fail "relic $arguments exited $status, expected 2"
	fi
done

# Diagnostics must not vary with the path the tool was invoked by.  getopt(3)
# reports an unknown option by printing argv[0], which in this build tree is
# an absolute path through a libtool wrapper; a diagnostic carrying it would
# differ on every machine and defeat the golden output comparisons the later
# suites rely on.
diagnostic=$("$RELIC" -Z 2>&1 >/dev/null || true)

if [ -z "$diagnostic" ]; then
	fail "relic -Z reported nothing, expected a diagnostic"
fi

case "$diagnostic" in
*/*)
	fail "relic -Z reported a path in its diagnostic: $diagnostic"
	;;
*"unknown option"*)
	;;
*)
	fail "relic -Z did not name the problem: $diagnostic"
	;;
esac

# The program name in a diagnostic comes from argv[0], whose last component
# is a filename and may hold any byte a filesystem accepts, terminal escape
# sequences included.  The tool must not pass such a name through to the
# reader's terminal.
#
# libtool wraps a program that links an uninstalled shared library in a
# script that re-executes the real binary under a name of its own, which puts
# argv[0] out of reach from here.  Whether this build does that is determined
# rather than assumed, and the check is skipped when argv[0] cannot be
# controlled instead of passing without having tested anything.
probe_link="$RELIC_BUILDDIR/relic-argv0-probe"
ln -sf "$RELIC" "$probe_link"
probe=$("$probe_link" -Z 2>&1 >/dev/null || true)
rm -f "$probe_link"

case "$probe" in
relic-argv0-probe:*)
	escaped_link="$RELIC_BUILDDIR/$(printf 'relic\033]0;owned\007')"
	ln -sf "$RELIC" "$escaped_link"
	diagnostic=$("$escaped_link" -Z 2>&1 >/dev/null || true)
	rm -f "$escaped_link"

	if printf '%s' "$diagnostic" |
	    LC_ALL=C grep -q '[^[:print:][:blank:]]'; then
		fail "relic echoed an unprintable byte from its own name"
	fi
	;;
*)
	echo "this build does not pass argv[0] through; skipping the" >&2
	echo "program-name check" >&2
	;;
esac

exit 0
