#!/bin/sh
#
# A query answered through the Magic rewrite derives only what its bindings
# reach, and answers exactly what the whole fixed point does.
#
# The graph is CHAINS disjoint chains of LENGTH nodes each.  The closure of
# one chain has LENGTH * (LENGTH - 1) / 2 pairs, and the whole fixed point
# derives that for every chain and for both path predicates, some quarter of
# a million tuples.  A query that binds a node of the first chain reaches that
# chain alone, so its rewrite derives the closure of one chain at most, and
# holds the edges, which are read completely, and little else.
#
# path recurses on the right and lpath on the left, which the rewrite treats
# differently: the magic rule of path passes the binding along each edge,
# while the one of lpath would derive its magic predicate from itself and is
# dropped.  Each query binds a different column, or a different predicate.
#
# The check is on the counters, not the clock: the rewrite must hold at most
# a tenth of the tuples the whole fixed point does, which the one chain it
# reads clears by a wide margin and a rewrite that read every chain could not.

set -eu

: "${RELIC:?RELIC must name the relic executable}"

chains=100
length=50

fail() {
	echo "$1" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

awk -v chains="$chains" -v size="$length" 'BEGIN {
	for (c = 0; c < chains; c++)
		for (i = 0; i + 1 < size; i++)
			printf "edge(c%d_%d, c%d_%d).\n", c, i, c, i + 1;

	print "path(X, Y) :- edge(X, Y).";
	print "path(X, Z) :- edge(X, Y), path(Y, Z).";
	print "lpath(X, Y) :- edge(X, Y).";
	print "lpath(X, Z) :- lpath(X, Y), edge(Y, Z).";
}' > "$work/graph.dl"

# The counter lines are "stats: <name> <value>"; the value is the last field.
counter() {
	awk -v name="stats: $2 " 'index($0, name) == 1 { print $NF }' "$1"
}

for query in 'path(c0_0, X)?' 'path(X, c0_49)?' 'lpath(c0_0, X)?' \
    'lpath(X, c0_49)?' 'path(c0_0, c0_49)?'; do
	"$RELIC" -s -X stats -q "$query" "$work/graph.dl" \
	    > "$work/magic.rows" 2> "$work/magic.stats" ||
	    fail "$query through the rewrite exited $?"
	"$RELIC" -s -X stats -X no-magic -q "$query" "$work/graph.dl" \
	    > "$work/whole.rows" 2> "$work/whole.stats" ||
	    fail "$query from the whole fixed point exited $?"

	if ! cmp -s "$work/whole.rows" "$work/magic.rows"; then
		diff -u "$work/whole.rows" "$work/magic.rows" >&2 || true
		fail "$query answered differently through the rewrite"
	fi
	if [ ! -s "$work/magic.rows" ]; then
		fail "$query answered nothing, which compares nothing"
	fi

	magic=$(counter "$work/magic.stats" "tuples held")
	whole=$(counter "$work/whole.stats" "tuples held")
	if [ -z "$magic" ] || [ -z "$whole" ]; then
		fail "$query reported no tuple counters"
	fi

	if [ $((magic * 10)) -gt "$whole" ]; then
		fail "$query held $magic tuples through the rewrite, more than a tenth of the $whole the whole fixed point holds"
	fi
done
