#!/bin/sh
#
# A session at a terminal edits its lines through libedit, driven through
# the pseudo-terminal of tests/pty, which types each line once the prompt is
# written.
#
# Each line typed holds the keys that edit it, as someone at the terminal
# would press them, and comes to a query only if libedit took every key as
# the emacs binding it is.  A key passed through instead would leave its
# byte in the line and the line a mistake, which the tool reports in place
# of an answer.  The answers go to a file with -o, where nothing libedit
# draws to redraw a line can reach them, and each line asks a different
# question, so a line answered as another would have been shows.
#
# libedit reads characters in the encoding of the locale, so the lines
# typed in a UTF-8 locale hold characters beyond ASCII and edit them whole.
# Those checks need a UTF-8 locale and are skipped on a system without one,
# after the others have run.
#
# A build without libedit reads lines with getline(3), which edits nothing,
# and these checks are skipped there.

set -eu

: "${RELIC:?RELIC must name the relic executable}"
: "${RELIC_BUILDDIR:?RELIC_BUILDDIR must name the build directory}"

pty="$RELIC_BUILDDIR/tests/pty"

fail() {
	echo "$*" >&2
	exit 1
}

# grep exits 1 when the build has no libedit and 2 when it cannot read the
# configuration, which is a broken run rather than a reason to skip.
status=0
grep -q '^#define HAVE_LIBEDIT 1' "$RELIC_BUILDDIR/config.h" || status=$?
case "$status" in
0)
	;;
1)
	echo "relic is built without libedit; skipping the line editing checks" >&2
	exit 77
	;;
*)
	fail "$RELIC_BUILDDIR/config.h cannot be read"
	;;
esac

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# libedit reads the editrc of $EDITRC or of the home directory, which may
# bind the keys below to something else, draws the line for the terminal
# $TERM names, and reads characters in the encoding of the locale; each is
# fixed here so that what the checks see is libedit's default emacs bindings
# in the C locale, unless a check names another, and not the configuration
# of whoever runs them.
HOME=$work
TERM=vt100
export HOME TERM
unset EDITRC LANG LC_ALL LC_CTYPE

# A system with no pseudo-terminal to give, such as a container without
# devpts, cannot run these checks, and says so rather than failing them.
status=0
"$pty" true < /dev/null > /dev/null 2>&1 || status=$?
if [ "$status" -eq 124 ]; then
	echo "no pseudo-terminal is available; skipping the line editing checks" >&2
	exit 77
fi

cat > "$work/graph.dl" <<'EOF'
edge(a, b).
edge(b, c).
path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).
name(a, "caf\xc3\xa9").
name(b, "\xc3\xa9t\xc3\xa9").
EOF

# Four lines, each edited by its own keys:
#
#	dge(a, X)?  control-A  e     edge(a, X)?    the start of the line
#	path(b, X)  control-A  control-E  ?
#	                             path(b, X)?    its start, then its end
#	path(a, ,  delete  X)?       path(a, X)?    the character before
#	control-P  control-P         path(b, X)?    the history, two lines back
#
# The history holds each line as it was answered, edits made, so the line
# two back from the fourth is the second as it ended.
printf 'dge(a, X)?\001e\npath(b, X)\001\005?\npath(a, ,\177X)?\n\020\020\n' |
    "$pty" -p 'relic> ' "$RELIC" -i -o "$work/answers" "$work/graph.dl" \
    > "$work/terminal" || fail "an edited session exited $?"

printf 'b\n\nc\n\nb\nc\n\nc\n' > "$work/expected"
if ! cmp -s "$work/expected" "$work/answers"; then
	echo "an edited session answered:" >&2
	cat "$work/answers" >&2
	echo "and wrote to the terminal:" >&2
	tr -d '\r' < "$work/terminal" | cat -v >&2
	fail "a line edited at the terminal was not the query its keys make"
fi

# The first of the commonly installed UTF-8 locales, or nothing.
utf8_locale=
for locale in C.UTF-8 C.utf8 en_US.UTF-8 en_US.utf8; do
	if [ "$(LC_ALL=$locale locale charmap 2>/dev/null)" = UTF-8 ]; then
		utf8_locale=$locale
		break
	fi
done
if [ -z "$utf8_locale" ]; then
	echo "no UTF-8 locale is installed; skipping the UTF-8 checks" >&2
	exit 77
fi

# Two lines in UTF-8, each asking for the name it spells:
#
#	name(X, "caf\303\251")?              cafe with its acute accent
#	name(X, "\303\251t\303\251\303\251  delete  ")?
#	                                     the character before, both bytes
printf 'name(X, "caf\303\251")?\nname(X, "\303\251t\303\251\303\251\177")?\n' |
    LC_ALL=$utf8_locale "$pty" -p 'relic> ' "$RELIC" -i -o "$work/answers" \
    "$work/graph.dl" > "$work/terminal" ||
    fail "a session typed in UTF-8 exited $?"

printf 'a\n\nb\n' > "$work/expected"
if ! cmp -s "$work/expected" "$work/answers"; then
	echo "a session typed in UTF-8 answered:" >&2
	cat "$work/answers" >&2
	echo "and wrote to the terminal:" >&2
	tr -d '\r' < "$work/terminal" | cat -v >&2
	fail "a line typed in UTF-8 was not the query its characters make"
fi

# A line that is not UTF-8, the first byte of a character followed by bytes
# that cannot continue it, is refused, or loses the bytes that are not text,
# as the libedit chooses; either way the line fails, leaving the session's
# status 1, and the session goes on to answer the line after it.  A libedit
# that refuses the line reads only a few bytes past the one that is not
# text, so the rest of the line, typed ahead, must not become a line of its
# own: here it would end the session.
status=0
printf '\303abcde.quit\nname(X, "caf\303\251")?\n' |
    LC_ALL=$utf8_locale "$pty" -p 'relic> ' "$RELIC" -i -o "$work/answers" \
    "$work/graph.dl" > "$work/terminal" || status=$?
if [ "$status" -ne 1 ]; then
	tr -d '\r' < "$work/terminal" | cat -v >&2
	fail "a session given a line that is not UTF-8 exited $status"
fi

printf 'a\n' > "$work/expected"
if ! cmp -s "$work/expected" "$work/answers"; then
	echo "a session given a line that is not UTF-8 answered:" >&2
	cat "$work/answers" >&2
	echo "and wrote to the terminal:" >&2
	tr -d '\r' < "$work/terminal" | cat -v >&2
	fail "a line that is not UTF-8 ended the session"
fi

exit 0
