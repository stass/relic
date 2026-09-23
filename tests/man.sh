#!/bin/sh
#
# The manual pages are well-formed and document the whole interface.
#
# man/relic.3 is written by hand beside a header written by hand, and nothing
# in the build connects them: an entry point added to <relic.h> and left out
# of the page, or one removed and left in, would ship silently.  The names
# the page lists in its NAME section must therefore be exactly the names the
# header declares with RELIC_EXPORT, which tests/symbols.sh in turn holds to
# what the library exports.
#
# man/relic.1 stands in the same relation to the tool's options.  The letters
# its option list describes must be exactly the letters the usage message
# offers, which is the tool's own statement of what it accepts.
#
# mandoc's own lint judges the markup.  It is a development tool, not a
# build requirement, so its absence skips that part and says so.

set -eu

: "${RELIC:?RELIC must name the relic executable}"
: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"

header="$RELIC_SRCDIR/include/relic.h"
page="$RELIC_SRCDIR/man/relic.3"
tool_page="$RELIC_SRCDIR/man/relic.1"

fail() {
	echo "$1" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

sed -n 's/^RELIC_EXPORT .*[ *]\(relic_[A-Za-z0-9_]*\)(.*/\1/p' "$header" |
    sort > "$work/declared"
sed -n 's/^\.Nm \(relic_[A-Za-z0-9_]*\).*/\1/p' "$page" | sort > "$work/named"

if ! cmp -s "$work/declared" "$work/named"; then
	echo "the functions $page names differ from those $header declares:" >&2
	comm -23 "$work/declared" "$work/named" |
	    sed 's/^/  declared, not documented: /' >&2
	comm -13 "$work/declared" "$work/named" |
	    sed 's/^/  documented, not declared: /' >&2
	exit 1
fi

# Every option word of the usage message, such as [-aceinsv] or -V, split
# into its letters.  An unknown option is the one way to see the message.
# Both lists are sorted bytewise, so that -v and -V stay two letters whatever
# the locale's collation says.
LC_ALL=C
export LC_ALL
"$RELIC" '-?' 2>&1 >/dev/null |
    grep -o -e '-[A-Za-z][A-Za-z]*' | sed 's/^-//' | fold -w 1 |
    sort -u > "$work/offered"
sed -n 's/^\.It Fl \([A-Za-z]\).*/\1/p' "$tool_page" | sort -u \
    > "$work/described"

if [ ! -s "$work/offered" ] || ! cmp -s "$work/offered" "$work/described"; then
	echo "the options $tool_page describes differ from the usage message:" >&2
	comm -23 "$work/offered" "$work/described" |
	    sed 's/^/  offered, not described: -/' >&2
	comm -13 "$work/offered" "$work/described" |
	    sed 's/^/  described, not offered: -/' >&2
	exit 1
fi

if ! command -v mandoc >/dev/null 2>&1; then
	echo "mandoc is not installed; skipping the markup lint" >&2
	exit 77
fi

for manual in "$page" "$tool_page"; do
	if ! mandoc -Tlint -W warning "$manual" > "$work/lint" 2>&1; then
		cat "$work/lint" >&2
		fail "mandoc reports problems in $manual"
	fi
done

exit 0
