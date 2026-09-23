#!/bin/sh
#
# The interactive session's handling of its input and output: where it reads
# its lines from, where its answers and replies go, a line no query can be,
# and a write that fails part way through.  The golden language tests cover
# what a session prints; these cover what a golden file cannot express.
#
# A session's prompt and line editing belong to a terminal and are not
# exercised here: a pseudo-terminal cannot be driven portably from sh.

set -eu

: "${RELIC:?RELIC must name the relic executable}"

fail() {
	echo "$*" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

cat > "$work/graph.dl" <<'EOF'
edge(a, b).
edge(b, c).
EOF

# Under -i standard input is the session's and is never read as a program:
# with no operands the program is empty, and a fact on standard input is a
# line that is not a query.
status=0
diagnostic=$(printf 'edge(a, b).\n' | "$RELIC" -i 2>&1 >/dev/null) ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "a fact typed into an empty session exited $status, expected 1"
fi
case "$diagnostic" in
"<query 1>:1:1: error: expected a query, found a fact")
	;;
*)
	fail "a fact typed into a session was reported as '$diagnostic'"
	;;
esac

# With no operands the session answers from the empty program, which holds
# nothing; the predicate it names is undefined, as it would be in a -q.
actual=$(printf 'edge(X, Y)?\n' | "$RELIC" -i 2>/dev/null)
if [ -n "$actual" ]; then
	fail "an empty program answered '$actual'"
fi

# -o receives the session's rows and nothing else; a command's reply goes to
# standard output.
printf 'edge(a, X)?\n.help\nedge(b, X)?\n' |
    "$RELIC" -i -o "$work/out.txt" "$work/graph.dl" > "$work/replies.txt"
printf 'b\n\nc\n' > "$work/out.expected"
if ! cmp -s "$work/out.expected" "$work/out.txt"; then
	fail "-o under -i holds '$(cat "$work/out.txt")'"
fi
if ! grep -q '^\.quit ' "$work/replies.txt" ||
    grep -q '^[bc]$' "$work/replies.txt"; then
	fail "standard output under -i -o held '$(cat "$work/replies.txt")'"
fi

# A line holding a NUL byte is refused rather than cut short at the NUL, and
# the session goes on to the next line.
status=0
printf 'edge(a, X)?\000 junk\nedge(b, X)?\n' |
    "$RELIC" -i "$work/graph.dl" > "$work/nul.txt" 2> "$work/nul.err" ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "a line holding a NUL exited $status, expected 1"
fi
if [ "$(cat "$work/nul.txt")" != "c" ]; then
	fail "after a line holding a NUL the session answered" \
	    "'$(cat "$work/nul.txt")'"
fi
if ! grep -q 'line 1 of standard input holds a NUL byte' "$work/nul.err"; then
	fail "a line holding a NUL was reported as '$(cat "$work/nul.err")'"
fi

# A line ending in a carriage return, as a file written on another system
# holds, is answered: the return is whitespace to the language.
actual=$(printf 'edge(a, X)?\r\n' | "$RELIC" -i "$work/graph.dl")
if [ "$actual" != "b" ]; then
	fail "a line ending in a carriage return answered '$actual'"
fi

# A command line is split at any blank, a carriage return and a tab
# included, and .plan's plan goes to standard output even under -o.
actual=$(printf '.quit\r\nedge(a, X)?\n' | "$RELIC" -i "$work/graph.dl")
if [ -n "$actual" ]; then
	fail ".quit ending in a carriage return went on to answer '$actual'"
fi
printf '.plan\tedge(a, X)?\n.stats\n' |
    "$RELIC" -i -o "$work/plan.txt" "$work/graph.dl" > "$work/plan.replies"
if [ -s "$work/plan.txt" ]; then
	fail ".plan and .stats wrote '$(cat "$work/plan.txt")' to the -o file"
fi
if ! grep -q '^query .* :- edge(a, X)\.$' "$work/plan.replies" ||
    ! grep -q '^stats: derivations ' "$work/plan.replies"; then
	fail ".plan and .stats replied '$(cat "$work/plan.replies")'"
fi

# An unknown command's name is repeated with its control bytes escaped, so
# that a mistyped line cannot drive the terminal the diagnostic reaches.
diagnostic=$(printf '.\033[2Jx\n' | "$RELIC" -i 2>&1 >/dev/null) || true
case "$diagnostic" in
*'unknown command .\x1b[2Jx;'*)
	;;
*)
	fail "an unknown command holding ESC was reported as '$diagnostic'"
	;;
esac

# Under -n the output is never opened, in a session as in a run.
printf 'edge(a, X)?\n' | "$RELIC" -n -i -o "$work/never.txt" "$work/graph.dl"
if [ -e "$work/never.txt" ]; then
	fail "-n -i -o created the file it names"
fi

# The rows of the queries run before the session reach their reader before
# the session waits for its first line.  The session's input is a FIFO held
# open and empty, so the rows can only arrive by being pushed out before the
# read that blocks.
mkfifo "$work/typed"
"$RELIC" -q 'edge(a, X)?' -i "$work/graph.dl" < "$work/typed" \
    > "$work/early.txt" &
session=$!
exec 3> "$work/typed"
tries=0
while [ "$(cat "$work/early.txt")" != "b" ] && [ "$tries" -lt 20 ]; do
	sleep 1
	tries=$((tries + 1))
done
early=$(cat "$work/early.txt")
exec 3>&-
wait "$session"
if [ "$early" != "b" ]; then
	fail "the rows of -q were not written before the session read"
fi

# A command's reply that cannot be written ends the session like a row that
# cannot: the -o file then holds nothing the session answered after it.
status=0
printf '.help\nedge(a, X)?\n' |
    "$RELIC" -i -o "$work/after.txt" "$work/graph.dl" \
    2> "$work/reply.err" 1</dev/null || status=$?
if [ "$status" -ne 1 ]; then
	fail "a session whose replies failed exited $status, expected 1"
fi
if [ "$(grep -c 'cannot write standard output' "$work/reply.err")" -ne 1 ] ||
    [ -s "$work/after.txt" ]; then
	fail "a failed reply was reported as '$(cat "$work/reply.err")'" \
	    "with '$(cat "$work/after.txt")' written after it"
fi

# A read that fails ends the session, reported, and the run exits 1, rather
# than passing for the end of input.  Standard input is open for writing
# only, so every read from it fails.
status=0
"$RELIC" -i "$work/graph.dl" 0> "$work/unreadable" 2> "$work/read.err" ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "a session whose input failed exited $status, expected 1"
fi
if ! grep -q 'cannot read standard input' "$work/read.err"; then
	fail "a failed read was reported as '$(cat "$work/read.err")'"
fi

# A write that fails ends the session, reported once, and the run exits 1.
# Standard output is open for reading only, as in tests/cli.sh, so every
# write to it fails; the lines after the first answer are never answered.
status=0
printf 'edge(a, X)?\nedge(b, X)?\nedge(X, Y)?\n' |
    "$RELIC" -i "$work/graph.dl" 2> "$work/write.err" 1</dev/null ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "a session whose output failed exited $status, expected 1"
fi
if [ "$(grep -c 'cannot write standard output' "$work/write.err")" -ne 1 ]
then
	fail "a failed write in a session was reported as" \
	    "'$(cat "$work/write.err")'"
fi

exit 0
