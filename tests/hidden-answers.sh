#!/bin/sh
#
# tests/hidden-answers.awk names the runs of answers whose truth the text
# format hides, and refuses a layout it does not know.
#
# The language tests run it over every listed test's answers and pass when
# it names nothing, so a classifier that named nothing at all would pass
# them too.  This feeds it answers in the layout relic -F json writes, runs
# that hide which answers held and runs that do not, and layouts it must
# refuse rather than read as no answers.

set -eu

: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"

classifier=$RELIC_SRCDIR/tests/hidden-answers.awk
failures=0

# The answers of one query each, as relic writes them.
held='[
  []
]'
failed='[]'
shown='[
  ["a"],
  ["b"]
]'

# expect STATUS REPORT NAME ANSWER...: the classifier, given the answers
# one after another, exits STATUS and writes REPORT, or for status 2 any
# report naming the line it refused.
expect() {
	want_status=$1
	want_report=$2
	name=$3
	shift 3

	# printf given no arguments still writes one line, and no answers
	# is no lines at all.
	got_status=0
	got_report=$( { [ $# -eq 0 ] || printf '%s\n' "$@"; } |
	    awk -f "$classifier") || got_status=$?

	if [ "$got_status" != "$want_status" ]; then
		echo "$name: exited $got_status, expected $want_status" >&2
		failures=1
	elif [ "$want_status" = 2 ]; then
		case $got_report in
		"line "*|"the output ends inside answer "*)
			;;
		*)
			echo "$name: refused without saying where:" \
			    "$got_report" >&2
			failures=1
			;;
		esac
	elif [ "$got_report" != "$want_report" ]; then
		printf '%s: reported\n%s\nexpected\n%s\n' "$name" \
		    "$got_report" "$want_report" >&2
		failures=1
	fi
}

hides() {
	echo "queries $1 to $2 answer no row of values, and some hold and" \
	    "some fail"
}

expect 0 '' "no answers"
expect 0 '' "answers that all show rows" "$shown" "$shown"
expect 0 '' "a held answer between shown ones" \
    "$shown" "$held" "$shown" "$failed"
expect 0 '' "a run of held answers" "$held" "$held" "$shown"
expect 0 '' "a run of failed answers" "$shown" "$failed" "$failed"

expect 1 "$(hides 1 2)" "a held and a failed answer first" \
    "$held" "$failed" "$shown"
expect 1 "$(hides 2 4)" "a run between shown answers" \
    "$shown" "$failed" "$failed" "$held" "$shown"
expect 1 "$(hides 3 4)" "a run that ends the output" \
    "$shown" "$shown" "$failed" "$held"
expect 1 "$(hides 1 2)
$(hides 4 5)" "two runs" \
    "$failed" "$held" "$shown" "$held" "$failed"

expect 2 '' "a held answer on one line" '[[]]' "$failed"
expect 2 '' "rows outside an answer" '  ["a"]'
expect 2 '' "a row that is not an array" '[' '  "a"' ']'
expect 2 '' "an answer left open" "$shown" '[' '  ["a"]'

exit "$failures"
