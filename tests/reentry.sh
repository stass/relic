#!/bin/sh
#
# Every entry point that can change a context is tried from inside one.
#
# A context refuses every call that would change it while caller code it is
# running -- a callback, or a stream's read function -- is inside an
# operation.  tests/unit/test_api.c proves it by making each such call from
# each kind of caller code, but it names the calls by hand, so an entry point
# added to <relic.h> and forgotten there would go untested, and one that
# forgot to refuse would pass.
#
# So every function the header declares with RELIC_EXPORT must be either
# one of the few that never enter a context -- the reads a callback may make,
# relic_interrupt(), which only raises a flag, and the functions that take
# no context -- or called by the test's
# re-entering code.  The first list is short and changes only with the
# callback rule; a new entry point fails here until one of the two names it.

set -eu

: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"

header="$RELIC_SRCDIR/include/relic.h"
test_source="$RELIC_SRCDIR/tests/unit/test_api.c"

# The entry points that never enter a context: the reads a callback may
# make, relic_interrupt(), and the functions that take no context at all.
never_enter="relic_create relic_create_config relic_diag_stderr relic_error
relic_error_class relic_interrupt relic_query_count relic_result_columns
relic_result_compare relic_result_destroy relic_result_row relic_result_rows
relic_result_string relic_stats relic_string relic_value_compare
relic_version"

fail() {
	echo "$1" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

sed -n 's/^RELIC_EXPORT .*[ *]\(relic_[A-Za-z0-9_]*\)(.*/\1/p' "$header" |
    sort > "$work/declared"
[ -s "$work/declared" ] || fail "no RELIC_EXPORT declarations in $header"

# The re-entering code: reenter_every_way(), and reenter_from_row(), which
# also tries to destroy the query being answered.  Each function runs from
# its definition's name at the start of a line to the closing brace that
# ends it.
awk '/^(reenter_every_way|reenter_from_row)\(/ { inside = 1 }
    inside { print }
    inside && /^}/ { inside = 0 }' "$test_source" > "$work/reentering"
[ -s "$work/reentering" ] || fail "no re-entering code found in $test_source"

status=0
while read -r name; do
	case " $(echo $never_enter) " in
	*" $name "*)
		continue
		;;
	esac

	# A call, not the label a failure message gives it, which is the
	# name in quotes.
	if ! grep -Eq "(^|[^\"A-Za-z0-9_])$name\(" "$work/reentering"; then
		echo "$name is exported, may change a context, and is not" \
		    "tried from inside one by $test_source" >&2
		status=1
	fi
done < "$work/declared"

exit $status
