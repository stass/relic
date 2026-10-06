#!/bin/sh
#
# Every evaluation strategy gives the default's answers.
#
# Each registered language test is run several times: once as the default
# evaluator runs it, and once under each alternative -X selects -- the naive
# fixed point, the planner without indexes, without reordering and without
# existence checks, each of those again answering from the whole fixed point
# instead of through the Magic rewrite, rounds whose insertions wait for their end, and every
# aggregate body compiled to a group rule.  Every run must agree with the
# default on standard output, the diagnostics on standard error, and exit
# status, but for the group rules, held to the default's output and status
# alone: they hold copies the default does not, which a limit message
# counts, and an unstratified program's cycle, found by the shortest path
# through one aggregate edge rather than two, may be noted by another
# equally valid path.  Some diagnostics are left out.  The
# warning that a rewrite outgrew its limit says which strategy answered,
# which runs without the rewrite never report, and the answer it announces
# is compared like any other.  The notes -vv gives about a comparison of
# mixed types, about an arithmetic operand that is not an integer, and
# about an aggregate over rows that are not all integers or not all of one
# type say where evaluation first met one, which is earlier when a query
# evaluates the whole program, and the answers they filtered are compared
# like any other.  Rows are sorted with -s, because a strategy that derives
# the same set of tuples in a different order is correct, and the
# comparison is of sets.
#
# Queries read only some relations, so each run also prints -X stats, and
# runs that evaluate the same rules must agree on how many tuples every
# relation together holds.  That stands for comparing the relations
# themselves, given two premises: the naive fixed point is complete, so its
# total is the size of the least model of the rules it evaluates; and the
# default inserts only tuples those rules justify -- by construction, since a
# delta is a range of its relation, so every match a variant finds, the
# rule's full plan would find too.  Every tuple the default holds is then in
# the least model, and holding as many as the model has, it holds all of
# them.  The same runs must agree on how many strings the context holds as
# values, since a string evaluation computes is interned only as a column
# of a derived tuple, and the tuples are the model's.  The other counters
# describe the work done or the storage used, differ between strategies by
# design, and are left out.
#
# The rules evaluated are the same within each of two groups.  Answering from
# the whole fixed point evaluates the program's rules, whatever the fixed
# point or the planner.  The Magic rewrite evaluates rules generated in the
# order the planner chooses for each body, and neither the naive fixed point
# nor the planner without indexes or without existence checks changes that
# order; the planner without
# reordering does, so under the rewrite it generates other rules, and only
# its answers are compared.
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
# since the plans are what the alternatives change, one that limits the
# Magic rewrite, which only some of the alternatives run, and an interactive
# session, whose .plan and .stats show what the strategy did.  So is a test
# with a .per-strategy file beside it, saying why: a program on which one
# strategy meets an undefined arithmetic result that another never
# evaluates, a difference the language allows and a golden test per
# strategy pins.  Every run
# reads the test's .stdin file, or else an empty standard input, as the
# language tests do.

set -eu

: "${RELIC:?RELIC must name the relic executable}"
: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"
: "${RELIC_LANG_TESTS:?RELIC_LANG_TESTS must list the registered tests}"

# The strategies compared, each its -X names joined by `+`, among which jN
# stands for -j N.  Those in `same_rules` evaluate the default's rules and
# are held to its tuple total too, and those in `other_rules` only to its
# answers and diagnostics.  The no-magic run is compared with the default
# on its answers, and is the reference whose tuple total those in
# `whole_rules`, which answer from the whole fixed point too, are held to.
# Those in `answer_rules` are held to the default's answers and exit status.
same_rules="naive no-index no-exists parallel j4+parallel"
other_rules="no-reorder"
whole_rules="naive+no-magic no-index+no-magic no-reorder+no-magic
    no-exists+no-magic"
answer_rules="group-rules"

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
	( cd "$directory" && $RELIC $args -s -X stats "$@" "$program" ) \
	    < "$input" > "$work/$label.out" 2> "$work/$label.all" ||
	    status=$?
	echo "$status" > "$work/$label.status"

	grep -v -e '^stats: ' -e "Magic rewrite outgrew magic=" \
	    -e "note: ordering comparison between " \
	    -e "note: operand of .* is a .*; the comparison is false" \
	    -e "note: a row of .* holds a value that is not an integer" \
	    -e "note: the rows of .* hold values of more than one type" \
	    "$work/$label.all" > "$work/$label.err" || true
	grep -e '^stats: tuples held ' -e '^stats: string values ' \
	    "$work/$label.all" > "$work/$label.held" || true
}

# Run the strategy whose names `label` joins with `+`: jN as -j N, and
# every other name as -X.
run_strategy() {
	label=$1
	options=""
	for name in $(echo "$label" | tr '+' ' '); do
		case $name in
		j[0-9]*)
			options="$options -j ${name#j}"
			;;
		*)
			options="$options -X $name"
			;;
		esac
	done

	run "$label" $options
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

	input=/dev/null
	if [ -f "$directory/$base.stdin" ]; then
		input=$directory/$base.stdin
	fi

	case " $args " in
	*" -X"* | *" -e"* | *" -i"* | *" -l magic="*)
		continue
		;;
	esac
	if [ -f "$directory/$base.per-strategy" ]; then
		continue
	fi

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

	for strategy in $same_rules; do
		run_strategy "$strategy"
		compare default "$strategy" out err status held
	done

	for strategy in $other_rules; do
		run_strategy "$strategy"
		compare default "$strategy" out err status
	done

	for strategy in $answer_rules; do
		run_strategy "$strategy"
		compare default "$strategy" out status
	done

	run_strategy no-magic
	compare default no-magic out err status

	for strategy in $whole_rules; do
		run_strategy "$strategy"
		compare no-magic "$strategy" out err status held
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
