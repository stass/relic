#!/bin/sh
#
# The manual page is well-formed and documents the whole interface.
#
# man/relic.3 is written by hand beside a header written by hand, and nothing
# in the build connects them: an entry point added to <relic.h> and left out
# of the page, or one removed and left in, would ship silently.  The names
# the page lists in its NAME section must therefore be exactly the names the
# header declares with RELIC_EXPORT, which tests/symbols.sh in turn holds to
# what the library exports.
#
# mandoc's own lint judges the markup.  It is a development tool, not a
# build requirement, so its absence skips that part and says so.

set -eu

: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"

header="$RELIC_SRCDIR/include/relic.h"
page="$RELIC_SRCDIR/man/relic.3"

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

if ! command -v mandoc >/dev/null 2>&1; then
	echo "mandoc is not installed; skipping the markup lint" >&2
	exit 77
fi

if ! mandoc -Tlint -W warning "$page" > "$work/lint" 2>&1; then
	cat "$work/lint" >&2
	fail "mandoc reports problems in $page"
fi

exit 0
