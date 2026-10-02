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
# bind the keys below to something else, and draws the line for the
# terminal $TERM names; each is fixed here so that what the checks see is
# libedit's default emacs bindings and not the configuration of whoever
# runs them.
HOME=$work
TERM=vt100
export HOME TERM
unset EDITRC

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

exit 0
