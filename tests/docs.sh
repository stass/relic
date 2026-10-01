#!/bin/sh
#
# Every example the manual pages and the README show does what they say.
#
# An example that was never run is the text most likely to be wrong, and it
# goes wrong silently: nothing fails when a later change makes it false.  So
# the examples are extracted from the documents and run, and an example with
# no expectation fails as surely as one whose output is wrong.
#
# A command line is a line of an example block that begins "$ ": an mdoc
# .Dl line or a line of a .Bd -literal block, or a line of a fenced block of
# README.md.  Every one must begin "$ relic ", and every line of a document
# that holds "$ relic " must be one, so that an example written in markup
# this script does not read fails rather than going unrun.  The lines after
# a command line in its block, up to the next, are its transcript, and so
# none of them begins "$ ": a line beginning "relic> " is a line of input to
# interactive mode, which prints no prompt when its input is not a terminal,
# and every other line is a line it prints.
#
# tests/examples/expected holds an entry per command line, in the same form:
# the command line, the lines it must print on standard output, lines
# beginning "2> " for what it must print on standard error, and "# exit N"
# when it must exit with a status other than zero.  The set of command lines the documents
# show and the set of entries must be equal, so that a new example cannot go
# unrun and an entry cannot outlive its example.  A transcript the documents
# show must be the entry's, so that what a reader sees is what is checked.
#
# A fenced block of README.md whose info string names a file after its
# language, as "```text graph.dl" does, shows that file of tests/examples, and
# must hold exactly its bytes, so that the program a reader is shown is the
# one the examples run.
#
# Each command runs through sh -c in a copy of tests/examples, which holds the
# files the examples name, with relic defined as a function running the
# built tool, so that quoting and redirection mean what they mean to a
# reader's shell.  The documents and tests/examples are therefore trusted, as
# any test is: what they say is run with the tester's privileges.
#
# The C example of relic(3) is examples/query.c verbatim, its copyright
# notice aside, which make check builds against the library like any test;
# its output is the entry of the command line "$ ./query".

set -eu

: "${RELIC:?RELIC must name the relic executable}"
: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"
: "${RELIC_BUILDDIR:?RELIC_BUILDDIR must name the build directory}"

fixtures="$RELIC_SRCDIR/tests/examples"
expected="$fixtures/expected"
query_example="$RELIC_SRCDIR/examples/query.c"
query_program="$RELIC_BUILDDIR/examples/query"

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

failures=0

fail() {
	echo "$1" >&2
	failures=$((failures + 1))
}

# The literal text of mdoc source on standard input: the escapes a literal
# block or a .Dl line can hold, turned back into the bytes a reader sees.
unescape_mdoc() {
	sed -e 's/\\(aq/'"'"'/g' -e 's/\\(dq/"/g' -e 's/\\-/-/g' \
	    -e 's/\\&//g' -e 's/\\e/\\/g'
}

# The examples of an mdoc page, as entries: each command line, then its
# transcript.  A .Dl line is a command line with no transcript.
mdoc_examples() {
	awk '
	/^\.Bd/ { literal = ($0 ~ /[ \t]-literal([ \t]|$)/); open = 0; next }
	/^\.Ed/ { literal = 0; open = 0; next }
	/^\.Dl \$ / { print substr($0, 5); open = 0; next }
	literal && /^\$ / { print; open = 1; next }
	literal && open { print }
	' "$1" | unescape_mdoc
}

# The examples of a Markdown file: command lines and transcripts inside
# fenced blocks.
markdown_examples() {
	awk '
	/^(```|~~~)/ { fenced = !fenced; open = 0; next }
	fenced && /^\$ / { print; open = 1; next }
	fenced && open { print }
	' "$1"
}

# Hold the examples extracted from a document to what it holds: every
# command line runs relic, and every line holding "$ relic " was extracted.
check_extracted() {
	document=$1
	extracted=$2

	grep '^\$ ' "$extracted" | grep -v '^\$ relic ' > "$work/unsupported" ||
	    true
	if [ -s "$work/unsupported" ]; then
		fail "${document##*/} shows commands that do not run relic:"
		cat "$work/unsupported" >&2
	fi

	shown=$(grep -c '\$ relic ' "$document" || true)
	found=$(grep -c '^\$ relic ' "$extracted" || true)
	if [ "$shown" -ne "$found" ]; then
		fail "${document##*/} holds $shown lines with '\$ relic ', of which" \
		    "$found are examples this script reads"
	fi
}

# The C example of relic(3): the one literal block of its EXAMPLES section.
mdoc_c_example() {
	awk '
	/^\.Sh / { examples = ($0 == ".Sh EXAMPLES"); next }
	examples && /^\.Bd -literal/ { literal = 1; next }
	examples && /^\.Ed/ { literal = 0; next }
	literal { print }
	' "$1" | unescape_mdoc
}

# Split a file of entries into one file per entry under a directory, named
# by its position, and list each command line beside its file.  An entry
# begins at a line beginning "$ " and runs to the next.
split_entries() {
	awk -v directory="$2" '
	/^\$ / {
		count++
		file = sprintf("%s/%04d", directory, count)
		printf "%s\t%s\n", file, $0
	}
	count > 0 { print > file }
	' "$1"
}

mkdir "$work/documented" "$work/expected"

mdoc_examples "$RELIC_SRCDIR/man/relic.1" > "$work/relic.1"
mdoc_examples "$RELIC_SRCDIR/man/relic.3" > "$work/relic.3"
markdown_examples "$RELIC_SRCDIR/README.md" > "$work/README.md"
for document in man/relic.1 man/relic.3 README.md; do
	check_extracted "$RELIC_SRCDIR/$document" "$work/${document##*/}"
done

{
	cat "$work/relic.1" "$work/relic.3" "$work/README.md"
	echo '$ ./query'
} > "$work/documented.txt"

split_entries "$work/documented.txt" "$work/documented" \
    > "$work/documented.index"
split_entries "$expected" "$work/expected" > "$work/expected.index"

cut -f 2- "$work/documented.index" | sort -u > "$work/documented.commands"
cut -f 2- "$work/expected.index" | sort > "$work/expected.commands"

if [ -n "$(uniq -d "$work/expected.commands")" ]; then
	fail "tests/examples/expected has more than one entry for:"
	uniq -d "$work/expected.commands" >&2
fi

comm -23 "$work/documented.commands" "$work/expected.commands" \
    > "$work/unexpected"
comm -13 "$work/documented.commands" "$work/expected.commands" \
    > "$work/undocumented"
if [ -s "$work/unexpected" ]; then
	fail "these documented examples have no entry in tests/examples/expected:"
	cat "$work/unexpected" >&2
fi
if [ -s "$work/undocumented" ]; then
	fail "these entries of tests/examples/expected are not documented:"
	cat "$work/undocumented" >&2
fi

tab=$(printf '\t')

# The file of the entry for a command line, compared byte for byte.
entry_of() {
	while IFS="$tab" read -r file entry_command; do
		if [ "$entry_command" = "$1" ]; then
			echo "$file"
			return
		fi
	done < "$work/expected.index"
}

# A transcript the documents show must be the entry's, the "2> " markers and
# the exit line aside, which a reader's terminal does not show.
while IFS="$tab" read -r documented command; do
	entry=$(entry_of "$command")
	if [ -z "$entry" ]; then
		fail "no entry was found for '$command'"
		continue
	fi
	if [ "$(sed -n '2,$p' "$documented" | wc -l)" -eq 0 ]; then
		continue
	fi
	sed -e '/^# exit /d' -e 's/^2> //' "$entry" > "$work/shown"
	if ! diff -u "$work/shown" "$documented" >&2; then
		fail "the documented transcript of '$command' is not its entry"
	fi
done < "$work/documented.index"

# Every file README.md shows is the fixture of that name.  The info string's
# second word names it.  Each such block, empty or not, is written to a file
# of its own and listed beside the name, and compared with the fixture.
mkdir "$work/shown-files"
awk -v directory="$work/shown-files" '
/^(```|~~~)/ && fenced { fenced = 0; next }
/^(```|~~~)/ {
	fenced = 1
	file = ""
	if (NF < 2)
		next
	count++
	file = sprintf("%s/%04d", directory, count)
	printf "" > file
	printf "%s\t%s\n", file, $2
	next
}
fenced && file != "" { print > file }
' "$RELIC_SRCDIR/README.md" > "$work/shown-files.index"
while IFS="$tab" read -r shown name; do
	case $name in
	.* | *[!A-Za-z0-9_.-]*)
		fail "README.md shows a file no fixture can be named: $name"
		continue
		;;
	esac
	if [ ! -f "$fixtures/$name" ]; then
		fail "README.md shows $name, which tests/examples does not hold"
	elif ! diff -u "$fixtures/$name" "$shown" >&2; then
		fail "README.md shows $name other than tests/examples/$name"
	fi
done < "$work/shown-files.index"

# Run every entry and compare what it prints and its status.
while IFS="$tab" read -r entry command; do
	want_status=$(sed -n 's/^# exit //p' "$entry")
	case $want_status in
	"")
		want_status=0
		;;
	[0-9] | [1-9][0-9] | 1[0-9][0-9] | 2[0-4][0-9] | 25[0-5])
		;;
	*)
		fail "'$command' has an exit line that is not one status"
		continue
		;;
	esac

	sed -n 's/^relic> //p' "$entry" > "$work/stdin"
	sed -n -e '1d' -e '/^relic> /d' -e '/^2> /d' -e '/^# exit /d' -e p \
	    "$entry" > "$work/want.stdout"
	sed -n 's/^2> //p' "$entry" > "$work/want.stderr"

	# The copy is made writable, as a reader's would be: make distcheck
	# runs the tests from a read-only source tree, whose modes cp keeps,
	# and a read-only copy could not be removed for the next entry.
	rm -rf "$work/run"
	cp -R "$fixtures" "$work/run"
	chmod -R u+w "$work/run"

	status=0
	if [ "$command" = '$ ./query' ]; then
		( cd "$work/run" && "$query_program" ) < "$work/stdin" \
		    > "$work/got.stdout" 2> "$work/got.stderr" || status=$?
	else
		( cd "$work/run" &&
		    sh -c 'relic() { "$RELIC" "$@"; }; '"${command#\$ }" ) \
		    < "$work/stdin" > "$work/got.stdout" \
		    2> "$work/got.stderr" || status=$?
	fi

	if ! diff -u "$work/want.stdout" "$work/got.stdout" >&2; then
		fail "'$command' printed other standard output"
	fi
	if ! diff -u "$work/want.stderr" "$work/got.stderr" >&2; then
		fail "'$command' printed other standard error"
	fi
	if [ "$status" -ne "$want_status" ]; then
		fail "'$command' exited $status, expected $want_status"
	fi
done < "$work/expected.index"

# The C example is the program make check builds, without the comment
# holding its copyright notice and the blank line after it.
mdoc_c_example "$RELIC_SRCDIR/man/relic.3" > "$work/query.c"
awk 'noticed { print; next } /^ \*\/$/ { noticed = 1; getline }' \
    "$query_example" > "$work/query.source"
if ! diff -u "$work/query.source" "$work/query.c" >&2; then
	fail "the example of relic(3) is not examples/query.c"
fi

if [ "$failures" -ne 0 ]; then
	echo "$failures documentation check(s) failed" >&2
	exit 1
fi

exit 0
