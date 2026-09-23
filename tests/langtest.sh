#!/bin/sh
#
# Run one language test: a .dl program and the output it must produce.
#
# A test is a file and its expectations, with no code:
#
#	tests/lang/<name>.dl		 the program
#	tests/lang/<name>.args		 optional: the options to run it with
#	tests/lang/<name>.stdin		 optional: standard input, such as the
#					 lines of an interactive session
#	tests/lang/<name>.expected	 expected standard output
#	tests/lang/<name>.expected-err	 optional: expected standard error
#	tests/lang/<name>.exit		 optional: expected status, default 0
#
# Automake runs this as DL_LOG_COMPILER, so every .dl file listed in TESTS is
# a test with no per-test registration.  There is no way to add a test that is
# silently not run, and no way to add one that is registered and has no
# expectations: a missing .expected file fails the test rather than skipping
# the comparison.
#
# An absent .expected-err means no diagnostics at all are expected, rather
# than meaning that diagnostics are not compared.  Diagnostics are the
# feature these tests exist to protect, and one that appears where none was
# expected must fail.
#
# The program is run from its own directory, under its bare file name, so the
# diagnostics name the file the same way wherever the source tree is and
# whether the build is in-tree or beside it.  Standard input is the .stdin
# file, or else empty, so that no test reads whatever the harness was given:
# an interactive session driven from a file is compared like any other run,
# and one that found a terminal instead would wait for it.

set -eu

: "${RELIC:?RELIC must name the relic executable}"

test_path=${1:?a .dl file must be given}

directory=$(dirname "$test_path")
program=$(basename "$test_path")
base=${program%.dl}

if [ ! -f "$directory/$base.expected" ]; then
	echo "$test_path has no $base.expected; every test states its output" >&2
	exit 1
fi

# Options are read as a line of words and are deliberately left unquoted at
# the point of use, so that a file holding "-c -F csv" passes two options
# rather than one argument containing a space.  Pathname expansion is turned
# off, because a query such as path(a,X)? is also a glob pattern, and a file
# that happened to match it would silently replace the query.  A query in an
# options file is therefore written without spaces.
set -f
args=""
if [ -f "$directory/$base.args" ]; then
	args=$(cat "$directory/$base.args")
fi

input=/dev/null
if [ -f "$directory/$base.stdin" ]; then
	input=$directory/$base.stdin
fi

expected_status=0
if [ -f "$directory/$base.exit" ]; then
	expected_status=$(cat "$directory/$base.exit")
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# The status is captured rather than allowed to end the script, because a
# nonzero one is what several of these tests expect.
status=0
( cd "$directory" && $RELIC $args "$program" ) < "$input" \
    > "$work/stdout" 2> "$work/stderr" || status=$?

failures=0

if [ "$status" != "$expected_status" ]; then
	echo "$test_path: exited $status, expected $expected_status" >&2
	failures=1
fi

if ! diff -u "$directory/$base.expected" "$work/stdout" >&2; then
	echo "$test_path: standard output differs" >&2
	failures=1
fi

if [ -f "$directory/$base.expected-err" ]; then
	expected_stderr=$directory/$base.expected-err
else
	expected_stderr=$work/empty
	: > "$expected_stderr"
fi

if ! diff -u "$expected_stderr" "$work/stderr" >&2; then
	echo "$test_path: standard error differs" >&2
	failures=1
fi

exit "$failures"
