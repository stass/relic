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
# one of the few a busy context never refuses -- the reads a callback may
# make, relic_interrupt(), which only raises a flag, and the functions that
# take no context -- or called by the test's re-entering code.  The first
# list is short and changes only with the callback rule; a new entry point
# fails here until one of the two names it.
#
# A failure is told by the return value alone, so a function declared void
# must be one that cannot fail.  Those are listed apart, each with the reason
# it cannot, and any other void declaration fails here, refusable or not.

set -eu

: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"

header="$RELIC_SRCDIR/include/relic.h"
test_source="$RELIC_SRCDIR/tests/unit/test_api.c"

# The entry points a busy context never refuses: the reads a callback may
# make, relic_interrupt(), and the functions that take no context at all.
never_refused="relic_create relic_create_config relic_diag_stderr relic_error
relic_error_class relic_interrupt relic_query_count relic_result_columns
relic_result_compare relic_result_destroy relic_result_row relic_result_rows
relic_result_string relic_stats relic_string relic_value_compare
relic_version"

# The entry points that return nothing, because they cannot fail:
#	relic_interrupt		raises a flag, and nothing else
#	relic_result_destroy	frees memory the result alone owns
#	relic_diag_stderr	writes to stderr, which has no failure to report
#				to a diagnostic that is already being reported
cannot_fail="relic_diag_stderr relic_interrupt relic_result_destroy"

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
[ "$(wc -l < "$work/declared")" -eq "$(grep -c '^RELIC_EXPORT' "$header")" ] ||
    fail "a RELIC_EXPORT declaration in $header is not one name per line"
void_name='s/^RELIC_EXPORT[[:space:]]\{1,\}void[[:space:]]\{1,\}'
void_name="$void_name"'\(relic_[A-Za-z0-9_]*\)(.*/\1/p'
sed -n "$void_name" "$header" | sort > "$work/void"

status=0
while read -r name; do
	case " $(echo $cannot_fail) " in
	*" $name "*)
		;;
	*)
		echo "$name is exported, returns nothing, and is not one of" \
		    "the entry points that cannot fail" >&2
		status=1
		;;
	esac
done < "$work/void"
for name in $cannot_fail; do
	grep -qx "$name" "$work/void" ||
	    fail "$name is listed as returning nothing but is not declared void"
done

# The re-entering code: reenter_every_way(), and reenter_from_row(), which
# also tries to destroy the query being answered.  Each function runs from
# its definition's name at the start of a line to the closing brace that
# ends it.
awk '/^(reenter_every_way|reenter_from_row)\(/ { inside = 1 }
    inside { print }
    inside && /^}/ { inside = 0 }' "$test_source" > "$work/reentering"
[ -s "$work/reentering" ] || fail "no re-entering code found in $test_source"

while read -r name; do
	case " $(echo $never_refused) " in
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
