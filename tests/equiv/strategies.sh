#!/bin/sh
#
# Every evaluation strategy gives the default's answers.
#
# Each registered language test is run several times: once as the default
# evaluator runs it, and once under each alternative -X selects -- the naive
# fixed point, and the planner without indexes and without reordering.  Every
# run must agree with the default on standard output, standard error and exit
# status.
# Rows are sorted with -s, because a strategy that derives the same set of
# tuples in a different order is correct, and the comparison is of sets.
#
# Queries read only some relations, so each run also prints -X stats, and the
# runs must agree on how many tuples every relation together holds.  That
# stands for comparing the relations themselves, given two premises: the
# naive fixed point is complete, so its total is the size of the program's
# least model; and the default inserts only tuples the program justifies --
# by construction, since a delta is a range of its relation, so every match a
# variant finds, the rule's full plan would find too.  Every tuple the default
# holds is then in the least model, and holding as many as the model has, it
# holds all of them.  The other counters describe the work done or the
# storage used, differ between strategies by design, and are left out.
#
# A comparison between two runs that both went wrong the same way would pass,
# so the default run is also held to the test's golden output wherever the
# test sorts its rows itself, and the suite fails unless some run was checked
# that way.
#
# The corpus is the registered list of language tests rather than a list of
# its own, so a program added to exercise the language is compared across
# strategies without anyone deciding that it should be.  A test that chooses
# a strategy itself, with -X in its options, is about that strategy and is
# left to its own expectations, and so is one that shows its plans with -e,
# since the plans are what the alternatives change.

set -eu

: "${RELIC:?RELIC must name the relic executable}"
: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"
: "${RELIC_LANG_TESTS:?RELIC_LANG_TESTS must list the registered tests}"

# The -X names of the strategies compared against the default.
strategies="naive no-index no-reorder"

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

# Run one program with the given extra options, from its own directory as
# the language tests do, recording its output and status under `label`.
# Options are word-split and never glob-expanded, for the reasons
# tests/langtest.sh gives.
run() {
	label=$1
	shift

	status=0
	( cd "$directory" && $RELIC $args -s -X stats "$@" "$program" ) \
	    > "$work/$label.out" 2> "$work/$label.all" || status=$?
	echo "$status" > "$work/$label.status"

	grep -v -e '^stats: ' "$work/$label.all" > "$work/$label.err" || true
	grep -e '^stats: tuples held ' "$work/$label.all" \
	    >> "$work/$label.err" || true
}

set -f
failures=0
compared=0
anchored=0

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

	case " $args " in
	*" -X"* | *" -e"*)
		continue
		;;
	esac

	run default

	case " $args " in
	*" -s "*)
		if ! cmp -s "$directory/$base.expected" "$work/default.out"; then
			echo "$path: the default run differs from" \
			    "$base.expected" >&2
			failures=$((failures + 1))
		fi
		anchored=$((anchored + 1))
		;;
	esac

	for strategy in $strategies; do
		run "$strategy" -X "$strategy"

		for part in out err status; do
			if ! cmp -s "$work/default.$part" \
			    "$work/$strategy.$part"; then
				echo "$path: -X $strategy differs from the" \
				    "default in its $part:" >&2
				diff -u "$work/default.$part" \
				    "$work/$strategy.$part" >&2 || true
				failures=$((failures + 1))
			fi
		done
	done

	compared=$((compared + 1))
done

# A corpus that silently shrank to nothing would pass every comparison, and
# so would one whose runs all failed alike.
if [ "$compared" -eq 0 ] || [ "$anchored" -eq 0 ]; then
	echo "no language test was compared against its golden output" >&2
	exit 1
fi

[ "$failures" -eq 0 ]
