#!/bin/sh
#
# A table relic writes is a table relic reads back.
#
# A program's facts are written with -F csv and with -F tsv, one query per
# relation, and each file is imported with -I under its columns' types.  The
# imported facts must equal the program's, compared in the text format, which
# spells every value with its type, and writing them out again must give the
# first file byte for byte.  The facts are built to hold what each format
# treats specially: every byte value in a string, first, last, alone and
# doubled, and the pair of a carriage return and a line feed; symbols;
# integers at both ends of the range; and a row of no columns, which both
# formats write as an empty line.

set -eu

# The queries end in `?`, which the shell would take for a pattern.
set -f

: "${RELIC:?RELIC must name the relic executable}"

fail() {
	echo "$*" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# The files are named relative to the work directory, so that the -I options
# built below split into words wherever the directory is.
cd "$work"

# Every byte value in each position, as a \xHH escape in a string literal.
awk 'BEGIN {
	for (byte = 0; byte < 256; byte++) {
		b = sprintf("\\x%02x", byte);
		printf("str(\"%s\").\n", b);
		printf("str(\"%sa\").\n", b);
		printf("str(\"a%s\").\n", b);
		printf("str(\"%s%s\").\n", b, b);
	}
}' > "facts.dl"

cat >> "facts.dl" <<'EOF'
str("").
str("a\r\nb").
str("\r\n").
str("\n\r").
sym(a).
sym(aB_9).
sym(not).
int(-9223372036854775808).
int(9223372036854775807).
int(0).
row(a, "x,y\t\"z\"\\", -1).
row(b, "", 1).
none().
EOF

# Each relation, the query that writes it, and the columns that read it back.
relations="str:str(X)?:string
sym:sym(X)?:symbol
int:int(X)?:integer
row:row(X,Y,Z)?:symbol,string,integer
none:none()?:_"

for format in csv tsv; do
	imports=""
	for relation in $relations; do
		name=${relation%%:*}
		rest=${relation#*:}
		query=${rest%%:*}
		columns=${rest#*:}

		"$RELIC" -s -F "$format" -q "$query" "facts.dl" \
		    > "$name.$format"
		imports="$imports -I $format:$name:$columns=$name.$format"
	done

	for relation in $relations; do
		name=${relation%%:*}
		rest=${relation#*:}
		query=${rest%%:*}

		"$RELIC" -s -q "$query" "facts.dl" > "$name.text"
		# shellcheck disable=SC2086 # each import is its own option
		"$RELIC" -s -q "$query" $imports /dev/null \
		    > "$name.$format.text"
		if ! cmp -s "$name.text" "$name.$format.text"; then
			diff -u "$name.text" "$name.$format.text" \
			    >&2 || true
			fail "$name read back from $format differs from" \
			    "the program's"
		fi

		# shellcheck disable=SC2086
		"$RELIC" -s -F "$format" -q "$query" $imports /dev/null \
		    > "$name.$format.again"
		if ! cmp -s "$name.$format" "$name.$format.again"
		then
			fail "$name written again as $format differs from" \
			    "the first"
		fi
	done
done

# The comparison must have compared something: every byte value in four
# positions, less the two that coincide for `a`, and the four strings
# after them.
count=$(wc -l < "str.text")
if [ "$count" -ne 1026 ]; then
	fail "the program wrote $count strings, expected 1026"
fi

exit 0
