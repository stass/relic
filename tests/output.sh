#!/bin/sh
#
# The tool's results are the same bytes wherever its writes are divided.
#
# The tool gathers what it writes into a buffer of 64 KiB and hands the
# buffer to the stream whenever the next byte would not fit, so a row can be
# divided between two writes at any byte: inside a symbol, inside an integer,
# or inside the escape sequence of a string.  These tests move that division
# across every byte of a row and check that the bytes written never change,
# in every format.
#
# The oracle is the same row written with no division in it.  The row's
# first value is a filler symbol whose length decides where in the rest of
# the row the 64 KiB mark falls; with a filler of one letter the whole output
# is far smaller than the buffer and is written at once.  So the expected
# output for a filler of any length is the one-letter output with that letter
# written as the filler's letters, which is never divided.

set -eu

: "${RELIC:?RELIC must name the relic executable}"

fail() {
	echo "$*" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# The writer's OUTPUT_BUFFER_SIZE.  The two must change together: were the
# writer's larger, no row here would reach its end, and every check below
# would pass having divided nothing.
buffer_size=65536

# After the filler, one value of each kind, each with bytes worth dividing:
# a symbol; a string holding a control byte, a tab, a quote, a backslash, a
# C1 control character, a letter of two bytes and a line feed, which the
# formats escape in pieces of two to ten bytes or, in a quoted csv field,
# copy; and a negative integer of nineteen digits, so that the mark falls
# at every place inside each.
values='abcdefghijklmnop, "a\x01b\tc\"d\\e,f\xc2\x9bg\xc3\xa9h\ni",'
values="$values -1234567890123456789"

# first_bytes COUNT FILE: the first COUNT bytes of FILE, none for a COUNT
# of zero, which head -c refuses on some systems.
first_bytes() {
	if [ "$1" -gt 0 ]; then
		head -c "$1" "$2"
	fi
}

# Enough x's for any filler, cut to length.
awk -v count="$buffer_size" 'BEGIN {
	line = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
	for (written = 0; written < count; written += length(line))
		printf "%s", line
}' > "$work/xs"

# letters LENGTH: a symbol of LENGTH letters, an f and then x's.
letters() {
	printf 'f'
	first_bytes "$(($1 - 1))" "$work/xs"
}

# row LENGTH: a program holding one row, its filler of LENGTH letters.
row() {
	{
		printf 'row('
		letters "$1"
		printf ', %s).\n' "$values"
	} > "$work/row.dl"
}

# lengthen LENGTH: what the query writes for a filler of LENGTH letters,
# made from what it writes for the one-letter filler by writing LENGTH
# letters where that one letter was.  The format frames a symbol the same
# way whatever its length, so this is the output undivided.  The one letter
# is at byte `letter_offset`, counting from zero; tail -c counts from one, so
# starting it at letter_offset + 2 skips the bytes before the letter and the
# letter itself.
lengthen() {
	first_bytes "$letter_offset" "$work/short"
	letters "$1"
	tail -c "+$((letter_offset + 2))" "$work/short"
}

# answer FORMAT OUTPUT: the row's query, into OUTPUT.
answer() {
	"$RELIC" -F "$1" -q 'row(F, S, T, I)?' "$work/row.dl" > "$2"
}

for format in text csv tsv json; do
	# What the query writes for a one-letter filler, undivided.
	row 1
	answer "$format" "$work/short"
	short_bytes=$(wc -c < "$work/short")

	# Where the one letter is: the first f, since no format frames a
	# value or names a column with one.  awk reads a line at a time, so
	# its offset is its place in its line plus the bytes of the lines
	# before, their newlines included.
	letter_offset=$(awk '{
		at = index($0, "f")
		if (at > 0) {
			print bytes_before_line + at - 1
			exit
		}
		bytes_before_line += length($0) + 1
	}' "$work/short")

	# The output is `letter_offset` bytes, then the filler's letters,
	# then `rest_bytes` bytes.  A filler of `length` letters puts the
	# 64 KiB mark `buffer_size - letter_offset - length` bytes into that
	# rest, so each offset short of its end divides the output there.
	rest_bytes=$((short_bytes - letter_offset - 1))

	offset=0
	while [ "$offset" -lt "$rest_bytes" ]; do
		length=$((buffer_size - letter_offset - offset))
		row "$length"
		answer "$format" "$work/long"
		if [ "$(wc -c < "$work/long")" -le "$buffer_size" ]; then
			fail "-F $format wrote no more than the buffer holds," \
			    "so nothing divided the row"
		fi
		lengthen "$length" > "$work/expected"
		if ! cmp -s "$work/expected" "$work/long"; then
			fail "-F $format changed, divided $offset bytes after" \
			    "the filler"
		fi
		offset=$((offset + 1))
	done
done

# A string longer than the buffer is written whole, its escapes intact.
# Its literal repeats "ab\t\x01", which the text format writes back as the
# same eight bytes; 20,000 repetitions make a row of 160,003 bytes, divided
# twice.
awk 'BEGIN {
	printf "long(\""
	for (i = 0; i < 20000; i++)
		printf "ab\\t\\x01"
	printf "\").\n"
}' > "$work/long.dl"
awk 'BEGIN {
	printf "\""
	for (i = 0; i < 20000; i++)
		printf "ab\\t\\x01"
	printf "\"\n"
}' > "$work/long.expected"
"$RELIC" -q 'long(X)?' "$work/long.dl" > "$work/long.out"
if ! cmp -s "$work/long.expected" "$work/long.out"; then
	fail "a string longer than the buffer was written differently"
fi

# A value csv and tsv write as its bytes, and a symbol in any format, is
# handed to the buffer in one piece, which at 100,000 bytes is longer than
# the buffer and is divided on its way in.
awk 'BEGIN {
	for (i = 0; i < 100000; i++)
		printf "a"
}' > "$work/as"
{
	printf 'plain("'
	cat "$work/as"
	printf '").\nlong_symbol('
	cat "$work/as"
	printf ').\n'
} > "$work/plain.dl"
{
	cat "$work/as"
	printf '\n'
} > "$work/plain.expected"
for format in csv tsv; do
	"$RELIC" -F "$format" -q 'plain(X)?' "$work/plain.dl" \
	    > "$work/plain.out"
	if ! cmp -s "$work/plain.expected" "$work/plain.out"; then
		fail "-F $format wrote a value longer than the buffer differently"
	fi
done
"$RELIC" -q 'long_symbol(X)?' "$work/plain.dl" > "$work/plain.out"
if ! cmp -s "$work/plain.expected" "$work/plain.out"; then
	fail "a symbol longer than the buffer was written differently"
fi

exit 0
