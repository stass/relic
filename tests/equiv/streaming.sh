#!/bin/sh
#
# A query that streams its rows writes exactly what its answer relation
# would have written, row for row and in the same order.
#
# A query whose plan derives each of its rows once delivers them as the plan
# derives them, unless the run sets a tuple or memory limit, and -X no-stream
# makes every query gather its answer in the answer relation first, as a
# query whose plan may derive a row twice must.  The rows are the same because
# no derivation of such a plan repeats an earlier one, so the relation would
# reject none; they come in the same order because the relation lists its
# tuples in insertion order, which is the derivation order.  Each registered
# language test is run both ways, and with the Magic rewrite and without it,
# since the rewrite changes the rule a query runs.  The runs must agree byte
# for byte on standard output, on the diagnostics on standard error, and on
# exit status.  Unlike the comparison of strategies, which sorts the rows
# because a strategy may derive them in another order, these runs keep the
# order the test gives, since the order is the claim.
#
# Each run prints -X stats, which are left out of the comparison, and the
# suite fails unless some run inserted fewer tuples streaming than
# gathering: a corpus in which no query streamed would compare a behaviour
# with itself.
#
# The corpus is that of tests/equiv/strategies.sh, less the tests that pass
# -X, -e or -i, for the same reasons.  The tests that suite also leaves out
# -- those with a Magic limit or a .per-strategy file -- are kept: their
# diagnostics, or whether they succeed, may differ between the rewrite and
# whole-program evaluation, but each run here is compared only with a run
# under the same choice.  Under a tuple or
# memory limit no query streams, so a test setting one would compare
# gathering with itself; the rule that turns streaming off under a limit is
# tested in tests/unit/test_query.c.

set -eu

: "${RELIC:?RELIC must name the relic executable}"
: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"
: "${RELIC_LANG_TESTS:?RELIC_LANG_TESTS must list the registered tests}"

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# Run one program with the given extra options, from its own directory as
# the language tests do, recording its output and status under `label`.
# Options are word-split and never glob-expanded, for the reasons
# tests/langtest.sh gives.
run() {
	label=$1
	shift

	status=0
	( cd "$directory" && $RELIC $args -X stats "$@" "$program" ) \
	    < "$input" > "$work/$label.out" 2> "$work/$label.all" ||
	    status=$?
	echo "$status" > "$work/$label.status"

	grep -v -e '^stats: ' "$work/$label.all" > "$work/$label.err" || true
	sed -n 's/^stats: tuples inserted //p' "$work/$label.all" \
	    > "$work/$label.inserted"
}

# Compare the named parts of two runs, reporting each that differs.
compare() {
	reference=$1
	label=$2
	shift 2

	for part in "$@"; do
		if ! cmp -s "$work/$reference.$part" "$work/$label.$part"; then
			echo "$path: $label differs from $reference in its" \
			    "$part:" >&2
			diff -u "$work/$reference.$part" \
			    "$work/$label.$part" >&2 || true
			failures=$((failures + 1))
		fi
	done
}

# Count the pair of runs as one that streamed when the streaming run
# inserted fewer tuples than the gathering one.  A run that printed no count
# streamed nothing that can be counted.
count_streamed() {
	streamed_inserted=$(cat "$work/$1.inserted")
	gathered_inserted=$(cat "$work/$2.inserted")

	if [ -n "$streamed_inserted" ] && [ -n "$gathered_inserted" ] &&
	    [ "$streamed_inserted" -lt "$gathered_inserted" ]; then
		streamed=$((streamed + 1))
	fi
}

set -f
failures=0
compared=0
streamed=0

for path in $RELIC_LANG_TESTS; do
	directory=$RELIC_SRCDIR/${path%/*}
	program=${path##*/}
	base=${program%.dl}

	if [ ! -f "$directory/$program" ]; then
		echo "$path: no such program" >&2
		failures=$((failures + 1))
		continue
	fi

	args=""
	if [ -f "$directory/$base.args" ]; then
		args=$(cat "$directory/$base.args")
	fi

	input=/dev/null
	if [ -f "$directory/$base.stdin" ]; then
		input=$directory/$base.stdin
	fi

	case " $args " in
	*" -X"* | *" -e"* | *" -i"*)
		continue
		;;
	esac

	run streamed
	run gathered -X no-stream
	compare streamed gathered out err status

	run streamed-whole -X no-magic
	run gathered-whole -X no-magic -X no-stream
	compare streamed-whole gathered-whole out err status

	if [ "$(cat "$work/streamed.status")" -eq 0 ]; then
		count_streamed streamed gathered
		count_streamed streamed-whole gathered-whole
	fi

	compared=$((compared + 1))
done

if [ "$compared" -eq 0 ] || [ "$streamed" -eq 0 ]; then
	echo "no language test streamed a query's rows" >&2
	exit 1
fi

[ "$failures" -eq 0 ]
