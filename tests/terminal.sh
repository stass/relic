#!/bin/sh
#
# What the tool does when someone is at a terminal, driven through the
# pseudo-terminal of tests/pty, which types each line of a session once the
# prompt is written.
#
# What comes back is the terminal's output: carriage returns before every
# newline, the prompt, and, where libedit edits the line, the line as it was
# typed, which libedit writes back.  Each check therefore removes those
# before comparing, and asserts on what remains: the answers and the lines
# between them.

set -eu

: "${RELIC:?RELIC must name the relic executable}"
: "${RELIC_BUILDDIR:?RELIC_BUILDDIR must name the build directory}"

pty="$RELIC_BUILDDIR/tests/pty"

fail() {
	echo "$*" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# libedit reads the editrc of $EDITRC or of the home directory, which may
# bind control-D or the return key to something else, and draws the line for
# the terminal $TERM names; each is fixed here so that what the checks see
# is relic's and not the configuration of whoever runs them.
HOME=$work
TERM=vt100
export HOME TERM
unset EDITRC

# A system with no pseudo-terminal to give, such as a container without
# devpts, cannot run these checks, and says so rather than failing them.
status=0
"$pty" true < /dev/null > /dev/null 2>&1 || status=$?
if [ "$status" -eq 124 ]; then
	echo "no pseudo-terminal is available; skipping the terminal checks" >&2
	exit 77
fi

cat > "$work/graph.dl" <<'EOF'
edge(a, b).
edge(b, c).
path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).
EOF

# The answers a session wrote to the terminal, with the terminal's carriage
# returns, the prompts, the typed lines libedit writes back -- every one a
# query, ending in a question mark -- and the end-of-input mark removed, and
# the empty lines that end the session after the last answer dropped.  The
# terminal and libedit may each write the mark as ^D, and libedit may back
# over one with backspaces, so all three are deleted wherever they are.
# What comes back is bytes, not always valid in the locale's encoding, and
# is read in the C locale.
answers_of() {
	LC_ALL=C tr -d '\r\b' < "$1" |
	    LC_ALL=C sed -e 's/relic> //g' -e 's/\^D//g' |
	    LC_ALL=C grep -v '?$' |
	    LC_ALL=C awk '{ lines[NR] = $0 } END {
		last = NR
		while (last > 0 && lines[last] == "")
			last--
		for (i = 1; i <= last; i++)
			print lines[i]
	    }'
}

# At a terminal the prompt marks where each query's answer begins, so the
# blank line that sets one answer off from the last in a file is not
# written: the answers of two queries follow each other with nothing between.
printf 'path(a, X)?\nN = count { path(X, c) }?\n' |
    "$pty" -p 'relic> ' "$RELIC" -i "$work/graph.dl" > "$work/typed" ||
    fail "a session at a terminal exited $?"
answers_of "$work/typed" > "$work/answers"
printf 'b\nc\n2\n' > "$work/expected"
if ! cmp -s "$work/expected" "$work/answers"; then
	echo "a session at a terminal wrote these answers:" >&2
	cat "$work/answers" >&2
	fail "two answers at a terminal are separated, or wrong"
fi

# Answers sent to a file under -o are read later, by a program or a person
# with no prompt to go by, so they keep the blank line between queries even
# when the session is typed at a terminal.
printf 'path(a, X)?\nN = count { path(X, c) }?\n' |
    "$pty" -p 'relic> ' "$RELIC" -i -o "$work/file" "$work/graph.dl" \
    > /dev/null || fail "a session writing to a file exited $?"
printf 'b\nc\n\n2\n' > "$work/expected"
if ! cmp -s "$work/expected" "$work/file"; then
	echo "a session at a terminal wrote this to its -o file:" >&2
	cat "$work/file" >&2
	fail "the answers in an -o file are not separated"
fi

# Control-C while a query runs interrupts it and returns to the prompt: the
# query fails with the library's diagnostic, the next line is answered, and
# the session, having had a line fail, exits 1.  The recursion counts up
# without end, so only the interrupt finishes the first query.
cat > "$work/count.dl" <<'EOF'
n(0).
n(Y) :- n(X), Y = X + 1.
EOF
status=0
printf 'n(X), X < 0?\n\003\nN = count { path(a, X) }?\n' |
    "$pty" -p 'relic> ' "$RELIC" -i "$work/count.dl" "$work/graph.dl" \
    > "$work/interrupted" || status=$?
answers_of "$work/interrupted" > "$work/answers"
if [ "$status" -ne 1 ] ||
    ! grep -q 'relic: error: interrupted while deriving n/1' \
    "$work/answers" ||
    [ "$(tail -n 1 "$work/answers")" != 2 ]; then
	echo "a session interrupted at a terminal wrote this:" >&2
	cat "$work/answers" >&2
	fail "an interrupted query did not return to the prompt (status $status)"
fi

# A program read from a terminal would be waited for with nothing said, so
# the tool prints its usage and exits 2 instead.
status=0
"$pty" "$RELIC" > "$work/usage" < /dev/null || status=$?
if [ "$status" -ne 2 ] || ! grep -q 'usage: relic' "$work/usage"; then
	cat "$work/usage" >&2
	fail "relic with no operand at a terminal exited $status"
fi

# So is a table read from a terminal with -I and `-`, and the refusal names
# the import.
status=0
"$pty" "$RELIC" -I 'tsv:edge:symbol,symbol=-' -q 'edge(a, X)?' \
    > "$work/import" < /dev/null || status=$?
if [ "$status" -ne 2 ] ||
    ! grep -q 'standard input is a terminal, and -I .* would read a table' \
    "$work/import"; then
	cat "$work/import" >&2
	fail "an import of - at a terminal exited $status"
fi

# csv and tsv write a value's bytes to a file or a pipe, so that the table
# reads back, but at a terminal every byte that is not part of a printable
# character is written as \xHH, so that a value cannot send the terminal an
# escape sequence: here an OSC that would retitle the window, a C1 CSI, a
# carriage return that would write over the line, a newline, a byte that is
# not UTF-8, NUL and DEL.  Printable characters of two, three and four bytes
# are written as they are.  tsv keeps its own escapes, and so its backslash
# is doubled and csv's is not.  The check is made on the stream the rows go
# to: standard output, a file named by -o, or one named by -O.
cat > "$work/hostile.dl" <<'EOF'
p("a\x1b]0;pwn\x07b").
p("c\xc2\x9bd").
p("e\rf\ng").
p("h\xffi\\j").
p("k\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80l").
p("m\0n").
p("o\x7fp").
p(X)?
EOF
printable=$(printf 'k\303\251\342\202\254\360\237\230\200l')
printf '%s\n' 'a\x1b]0;pwn\x07b' 'c\xc2\x9bd' '"e\x0df\x0ag"' 'h\xffi\j' \
    "$printable" 'm\x00n' 'o\x7fp' > "$work/expected.csv"
printf '%s\n' 'a\x1b]0;pwn\x07b' 'c\xc2\x9bd' 'e\rf\ng' 'h\xffi\\j' \
    "$printable" 'm\x00n' 'o\x7fp' > "$work/expected.tsv"
for format in csv tsv; do
	for output in '' '-o /dev/tty' "-n -O $format:p=/dev/tty"; do
		# $output is unquoted to split into its options, or into
		# nothing.
		"$pty" "$RELIC" -s -F "$format" $output "$work/hostile.dl" \
		    < /dev/null > "$work/typed" ||
		    fail "-F $format $output at a terminal exited $?"
		answers_of "$work/typed" > "$work/answers"
		if ! cmp -s "$work/expected.$format" "$work/answers"; then
			echo "-F $format $output wrote at a terminal:" >&2
			od -c "$work/answers" >&2
			fail "-F $format $output escaped wrongly at a terminal"
		fi
	done
done

# A session's answers are written the same way.
grep -v '?$' "$work/hostile.dl" > "$work/facts.dl"
for format in csv tsv; do
	printf 'p(X)?\n' |
	    "$pty" -p 'relic> ' "$RELIC" -i -s -F "$format" "$work/facts.dl" \
	    > "$work/typed" || fail "a session with -F $format exited $?"
	answers_of "$work/typed" > "$work/answers"
	if ! cmp -s "$work/expected.$format" "$work/answers"; then
		echo "a session with -F $format wrote at a terminal:" >&2
		od -c "$work/answers" >&2
		fail "a session with -F $format escaped wrongly at a terminal"
	fi
done

# Through a pipe the same rows are the values' bytes.
{
	printf 'a\033]0;pwn\007b\nc\302\233d\n"e\rf\ng"\nh\377i\\j\n'
	printf '%s\nm\000n\no\177p\n' "$printable"
} > "$work/expected.csv"
{
	printf 'a\033]0;pwn\007b\nc\302\233d\ne\\rf\\ng\nh\377i\\\\j\n'
	printf '%s\nm\000n\no\177p\n' "$printable"
} > "$work/expected.tsv"
for format in csv tsv; do
	"$RELIC" -s -F "$format" "$work/hostile.dl" | cat > "$work/piped" ||
	    fail "-F $format into a pipe exited $?"
	if ! cmp -s "$work/expected.$format" "$work/piped"; then
		od -c "$work/piped" >&2
		fail "-F $format into a pipe did not write the values' bytes"
	fi
done

exit 0
