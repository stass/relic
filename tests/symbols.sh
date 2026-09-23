#!/bin/sh
#
# The shared library exports exactly the interface <relic.h> declares.
#
# Two fences keep the library's internals in: hidden visibility, which each
# public declaration's RELIC_EXPORT lifts, and the link's export list, which
# admits only names beginning relic_.  Internal functions are named relic_ as
# well, so the export list alone would let them through, and nothing in the
# build notices a function that was declared public and never exported, or
# one exported and never declared.  The first fails an embedder at link time;
# the second is an interface nobody documented, which the next release would
# break without knowing it had one.
#
# So the names the header declares with RELIC_EXPORT and the names the built
# shared library defines in its dynamic symbol table must be the same set.
#
# A dynamic symbol whose C identifier begins with an underscore is not
# relic's: identifiers at file scope beginning with one are reserved to the
# implementation, and a linker defines several -- _init, _fini, _edata, _end
# -- in every shared object on some platforms.  relic-owned code may not use
# such a name, so nothing of relic's can hide behind the exclusion.

set -eu

: "${RELIC_BUILDDIR:?RELIC_BUILDDIR must name the build directory}"
: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"

fail() {
	echo "$1" >&2
	exit 1
}

header="$RELIC_SRCDIR/include/relic.h"

library=""
for candidate in "$RELIC_BUILDDIR/.libs/librelic.dylib" \
    "$RELIC_BUILDDIR/.libs/librelic.so"; do
	if [ -f "$candidate" ]; then
		library=$candidate
		break
	fi
done

# Exit 77 rather than 0: automake reports 0 as a pass, and a check that could
# not run must not be indistinguishable from one that ran and found nothing.
if [ -z "$library" ]; then
	echo "no shared library was built; skipping the export audit" >&2
	exit 77
fi

if ! command -v nm >/dev/null 2>&1; then
	echo "nm is not available; skipping the export audit" >&2
	exit 77
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# Every public declaration names its function on the RELIC_EXPORT line, just
# before the opening parenthesis.  The count of names is held to the count of
# such lines, so a declaration written another way fails here rather than
# being left out of the comparison.
grep -c '^RELIC_EXPORT ' "$header" > "$work/declarations"
sed -n 's/^RELIC_EXPORT .*[ *]\(relic_[A-Za-z0-9_]*\)(.*/\1/p' "$header" |
    sort > "$work/declared"
if [ "$(wc -l < "$work/declared" | tr -d ' ')" != \
    "$(cat "$work/declarations")" ]; then
	fail "$header has a RELIC_EXPORT declaration whose name was not found"
fi

# Mach-O prefixes every C identifier with an underscore, which is removed so
# that both formats give C identifiers.  nm -gU lists the defined external
# symbols of a Mach-O image; on ELF the dynamic table is asked for directly.
if nm -gU "$library" >/dev/null 2>&1 &&
    nm -gU "$library" 2>/dev/null | grep -q ' _relic_'; then
	nm -gU "$library" | awk '{ print $NF }' | sed 's/^_//' > "$work/defined"
elif nm -D --defined-only "$library" >/dev/null 2>&1; then
	nm -D --defined-only "$library" | awk '{ print $NF }' |
	    sed 's/@.*//' > "$work/defined"
else
	fail "nm cannot list the dynamic symbols of $library"
fi

grep -v '^_' "$work/defined" | sort -u > "$work/exported"

if ! cmp -s "$work/declared" "$work/exported"; then
	echo "the symbols $library exports differ from those $header" \
	    "declares:" >&2
	comm -23 "$work/declared" "$work/exported" |
	    sed 's/^/  declared, not exported: /' >&2
	comm -13 "$work/declared" "$work/exported" |
	    sed 's/^/  exported, not declared: /' >&2
	exit 1
fi

exit 0
