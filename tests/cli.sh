#!/bin/sh
#
# The command-line tool's handling of its input and output: a program read
# from standard input, results written to a file with -o, and a write that
# fails.  The golden language tests cover what the tool prints; these cover
# where it reads from and writes to, which a golden file cannot express.

set -eu

: "${RELIC:?RELIC must name the relic executable}"

fail() {
	echo "$1" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

cat > "$work/graph.dl" <<'EOF'
edge(a, b).
edge(b, c).
path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).
path(a, X)?
EOF

expected=$(printf 'b\nc')

# With no operands and standard input not a terminal, the program is read
# from standard input and its queries are answered.
actual=$("$RELIC" -s < "$work/graph.dl")
if [ "$actual" != "$expected" ]; then
	fail "standard input answered '$actual', expected '$expected'"
fi

# -q applies to a program read from standard input as it does to a file.
actual=$("$RELIC" -q 'edge(X, c)?' < "$work/graph.dl")
if [ "$actual" != "b" ]; then
	fail "-q over standard input answered '$actual', expected 'b'"
fi

# -o writes the results to a file and nothing to standard output, replacing
# whatever the file held.
echo stale > "$work/out.txt"
actual=$("$RELIC" -s -o "$work/out.txt" "$work/graph.dl")
if [ -n "$actual" ]; then
	fail "-o also wrote '$actual' to standard output"
fi
if [ "$(cat "$work/out.txt")" != "$expected" ]; then
	fail "-o wrote '$(cat "$work/out.txt")', expected '$expected'"
fi

# The output file is opened only once the program and its queries are known
# to be valid, so a run that fails validation leaves an existing file alone.
echo kept > "$work/out.txt"
status=0
"$RELIC" -o "$work/out.txt" -q 'X = 1?' "$work/graph.dl" 2>/dev/null ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "an invalid query exited $status, expected 1"
fi
if [ "$(cat "$work/out.txt")" != "kept" ]; then
	fail "a run that failed validation replaced the output file"
fi

# -c evaluates nothing, so it writes nothing, and -o does not replace the
# file it names.
echo kept > "$work/out.txt"
"$RELIC" -c -o "$work/out.txt" "$work/graph.dl"
if [ "$(cat "$work/out.txt")" != "kept" ]; then
	fail "-c replaced the -o file"
fi

# An output file that cannot be created is an error, not a usage error.
status=0
"$RELIC" -o "$work/missing/out.txt" "$work/graph.dl" 2>/dev/null ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "an unwritable -o exited $status, expected 1"
fi

# A write that fails must not be reported as success.  Standard output is
# open for reading only here, so every write to it fails.  A closed descriptor
# would fail the same way, but the AddressSanitizer runtime on macOS reports a
# flush to one as successful, and the test would then pass or fail with the
# build rather than with the tool.
status=0
diagnostic=$("$RELIC" "$work/graph.dl" 2>&1 1</dev/null) || status=$?
if [ "$status" -ne 1 ]; then
	fail "a failed write exited $status, expected 1"
fi
case "$diagnostic" in
*"cannot write standard output"*)
	;;
*)
	fail "a failed write was reported as '$diagnostic'"
	;;
esac

# A query that fails during evaluation writes nothing -- not even the blank
# line that would separate it from the query before it -- and ends the run,
# while what the earlier query wrote stands.  Negation cannot be evaluated,
# so the second query fails when it is planned, after the first has run.
#
# The output is compared as a file, byte for byte, because command
# substitution would strip the very trailing newline this is looking for.
status=0
"$RELIC" -q 'edge(X, Y)?' -q 'edge(X, Y), not edge(Y, X)?' \
    -q 'edge(a, X)?' "$work/graph.dl" > "$work/partial.txt" 2>/dev/null ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "a query failing in evaluation exited $status, expected 1"
fi
printf 'a b\nb c\n' > "$work/partial.expected"
if ! cmp -s "$work/partial.expected" "$work/partial.txt"; then
	fail "a query failing in evaluation left '$(cat "$work/partial.txt")'"
fi

# A file that does not exist is an error in the input, reported once.
status=0
"$RELIC" "$work/absent.dl" 2>/dev/null || status=$?
if [ "$status" -ne 1 ]; then
	fail "a missing file exited $status, expected 1"
fi

exit 0
