#!/bin/sh
#
# Benchmark workloads.
#
# usage: bench/run.sh [relic]
#
# Each workload is generated with awk, so it is the same on every run and
# every platform, and run once through the relic named on the command line,
# by RELIC, or found on the PATH.  The table gives the wall-clock seconds and
# what -X stats counted: the tuples derived, the tuples held at the end, and
# the tuples scanned and looked up, which say how the time was spent.
# BENCH_SCALE, 1 by default, grows every workload: it multiplies the
# chain's length, the graph's nodes and edges, the tasks and the facts, and
# the grid's side, and deepens the trees by one level per step, which
# doubles them.
#
# Nothing fails on a time.  Timing depends on the machine and is noisy;
# tests/perf.sh is the test that bounds performance, on counters where it
# can.  This is the instrument for looking closer: before a change meant to
# be faster, after one that might be slower, or when perf.sh fails.
# bench/baseline.txt holds one machine's run to compare with by eye.

set -eu

relic=${1:-${RELIC:-relic}}
scale=${BENCH_SCALE:-1}

if [ ! -x /usr/bin/time ]; then
	echo "bench: /usr/bin/time is needed to time the workloads" >&2
	exit 2
fi
if ! command -v "$relic" >/dev/null 2>&1; then
	echo "bench: cannot run $relic; name the relic to benchmark" >&2
	exit 2
fi
case $(head -c 2 "$(command -v "$relic")") in
'#!')
	echo "bench: $relic is a script, perhaps libtool's wrapper, whose" \
	    "start-up would be timed with relic; name an installed relic or" \
	    "one built with --disable-shared" >&2
	exit 2
	;;
esac
case $scale in
'' | *[!0-9]* | 0*)
	echo "bench: BENCH_SCALE must be a positive integer" >&2
	exit 2
	;;
esac

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

. "$(dirname "$0")/datasets.sh"

# The rules each workload's data set is queried through.
#
# Over the chain, the closure is written twice.  With the first argument
# bound, the right-recursive `path` demands a path from every node the query
# reaches, so its rewrite derives the whole closure anyway; the
# left-recursive `from` stays bound to the query's node and derives only
# what that node reaches.  The pair shows what the order of a recursive body
# costs the Magic rewrite.
chain_rules='path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).
from(X, Y) :- edge(X, Y).
from(X, Z) :- from(X, Y), edge(Y, Z).'

# Over the chain again, the closure with each path's length, computed as it
# is derived: a new integer for every length, interned when the first tuple
# holding it is stored.  The chain is acyclic, so the bound only filters,
# as a computing recursion's bound does: a million is beyond the chain's
# length up to a BENCH_SCALE of 500.
distance_rules='dist(X, Y, 1) :- edge(X, Y).
dist(X, Z, E) :- dist(X, Y, D), edge(Y, Z), D < 1000000, E = D + 1.'

# Over the graph, the nodes reachable from n0, and the closure from any node.
graph_rules='reach(X) :- edge(n0, X).
reach(Y) :- reach(X), edge(X, Y).
from(X, Y) :- edge(X, Y).
from(X, Z) :- from(X, Y), edge(Y, Z).'

# Over the graph again, each node's out-degree and in-degree: two folds per
# node, each reading the group relation of one aggregate through the index
# on its node's column.
degree_rules='node(X) :- edge(X, _).
node(Y) :- edge(_, Y).
degree(X, O, I) :- node(X), O = count { edge(X, Y) }, I = count { edge(Z, X) }.'

# Over the grid, the cells reachable from the corner.
grid_rules='reach(X) :- edge(c0_0, X).
reach(Y) :- reach(X), edge(X, Y).'

# Over the tree, same generation.
tree_rules='sg(X, X) :- parent(_, X).
sg(X, Y) :- parent(P, X), sg(P, Q), parent(Q, Y).'

# Over the tasks, which are ready -- every dependency finished -- through
# negation.
tasks_rules='waits(X, Y) :- depends(X, Y), not done(Y).
waits(X, Z) :- depends(X, Y), waits(Y, Z).
blocked(X) :- waits(X, _).
ready(X) :- task(X), not done(X), not blocked(X).'

# Over the facts, a join, so the workload is mostly loading.
facts_rules='pair(A, B) :- row(A, V, 7), row(B, V, 13).'

# make_program NAME RULES: generate the data set NAME into its own directory
# and write $work/NAME.dl, its facts followed by RULES.
make_program() {
	mkdir "$work/$1"
	dataset "$1" "$work/$1" "$scale"
	write_program "$1" "$1" "$2"
}

# write_program NAME DATA RULES: write $work/NAME.dl, the facts of the data
# set already generated into $work/DATA followed by RULES.
write_program() {
	{
		source_facts "$work/$2"
		printf '%s\n' "$3"
	} > "$work/$1.dl"
}

# Run one workload: its name, the program's file, and relic's options.  The
# run's standard error holds both the counters and time(1)'s report, and is
# shown whole if the run fails.
run() {
	name=$1
	program=$2
	shift 2

	if ! /usr/bin/time -p "$relic" -X stats "$@" "$program" > /dev/null \
	    2> "$work/stats"; then
		echo "bench: the $name workload failed:" >&2
		cat "$work/stats" >&2
		exit 1
	fi

	awk -v name="$name" '
		/^real / { seconds = $2 }
		/^stats: derivations / { derived = $3 }
		/^stats: tuples held / { held = $4 }
		/^stats: tuples scanned / { scanned = $4 }
		/^stats: tuples looked up / { looked = $5 }
		END {
			printf "%-28s %8.2f %11d %10d %11d %11d\n", name, \
			    seconds, derived, held, scanned, looked
		}' "$work/stats"
}

make_program chain "$chain_rules"
write_program distances chain "$distance_rules"
make_program graph "$graph_rules"
write_program degrees graph "$degree_rules"
make_program grid "$grid_rules"
make_program tree "$tree_rules"
make_program small-tree "$tree_rules"
make_program tasks "$tasks_rules"
make_program facts "$facts_rules"

printf '%-28s %8s %11s %10s %11s %11s\n' workload seconds derived held \
    scanned "looked up"
run "chain, right-recursive" "$work/chain.dl" -q 'path(n0, X)?'
run "chain, left-recursive" "$work/chain.dl" -q 'from(n0, X)?'
run "chain, whole closure" "$work/chain.dl" -X no-magic -q 'path(n0, X)?'
run "chain, distances" "$work/distances.dl" -q 'dist(X, Y, D)?'
run "graph, reachability" "$work/graph.dl" -q 'reach(X)?'
run "graph, left-recursive" "$work/graph.dl" -q 'from(n17, X)?'
run "graph, degrees" "$work/degrees.dl" -q 'degree(X, O, I)?'
run "grid, reachability" "$work/grid.dl" -q 'reach(X)?'
run "tree, same generation" "$work/tree.dl" -q 'sg(t2048, X)?'
run "tree, all generations" "$work/small-tree.dl" -q 'sg(X, Y)?'
run "tasks, ready" "$work/tasks.dl" -q 'ready(X)?'
run "facts, load and join" "$work/facts.dl" -q 'pair(A, B)?'
