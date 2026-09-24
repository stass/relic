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
case $scale in
'' | *[!0-9]* | 0)
	echo "bench: BENCH_SCALE must be a positive integer" >&2
	exit 2
	;;
esac

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# A linear congruential sequence, so that a "random" graph is the same on
# every run.  The multiplier keeps every intermediate product below 2^53,
# where awk's arithmetic is exact.
lcg='function next_random(bound) {
	state = (state * 1103515 + 12345) % 2147483648
	return state % bound
}'

# chain N: a path of N edges, and its closure written twice.  With the
# first argument bound, the right-recursive `path` demands a path from every
# node the query reaches, so its rewrite derives the whole closure anyway;
# the left-recursive `from` stays bound to the query's node and derives only
# what that node reaches.  The pair shows what the order of a recursive body
# costs the Magic rewrite.
chain() {
	awk -v n="$1" 'BEGIN {
		for (i = 0; i < n; i++)
			printf "edge(n%d, n%d).\n", i, i + 1
		print "path(X, Y) :- edge(X, Y)."
		print "path(X, Z) :- edge(X, Y), path(Y, Z)."
		print "from(X, Y) :- edge(X, Y)."
		print "from(X, Z) :- from(X, Y), edge(Y, Z)."
	}'
}

# graph N M: N nodes and M edges, a chain through every node so all are
# reachable from n0, and the rest drawn from the sequence.
graph() {
	awk -v n="$1" -v m="$2" "$lcg"'
	BEGIN {
		state = 42
		for (i = 0; i + 1 < n; i++)
			printf "edge(n%d, n%d).\n", i, i + 1
		for (i = n - 1; i < m; i++)
			printf "edge(n%d, n%d).\n", next_random(n), next_random(n)
		print "reach(X) :- edge(n0, X)."
		print "reach(Y) :- reach(X), edge(X, Y)."
		print "from(X, Y) :- edge(X, Y)."
		print "from(X, Z) :- from(X, Y), edge(Y, Z)."
	}'
}

# grid N: an N by N grid with edges right and down, and the cells reachable
# from the corner.
grid() {
	awk -v n="$1" 'BEGIN {
		for (r = 0; r < n; r++) {
			for (c = 0; c < n; c++) {
				if (c + 1 < n)
					printf "edge(c%d_%d, c%d_%d).\n", r, c, r, c + 1
				if (r + 1 < n)
					printf "edge(c%d_%d, c%d_%d).\n", r, c, r + 1, c
			}
		}
		print "reach(X) :- edge(c0_0, X)."
		print "reach(Y) :- reach(X), edge(X, Y)."
	}'
}

# tree D: a complete binary tree of depth D, and same generation over it.
tree() {
	awk -v depth="$1" 'BEGIN {
		nodes = 2 ^ (depth + 1) - 1
		for (i = 1; 2 * i + 1 <= nodes; i++) {
			printf "parent(t%d, t%d).\n", i, 2 * i
			printf "parent(t%d, t%d).\n", i, 2 * i + 1
		}
		print "sg(X, X) :- parent(_, X)."
		print "sg(X, Y) :- parent(P, X), sg(P, Q), parent(Q, Y)."
	}'
}

# tasks N: N tasks, each depending on up to two earlier ones, some finished,
# and which tasks are ready -- every dependency finished -- through negation.
tasks() {
	awk -v n="$1" "$lcg"'
	BEGIN {
		state = 7
		for (i = 1; i < n; i++) {
			printf "task(k%d).\n", i
			printf "depends(k%d, k%d).\n", i, next_random(i)
			if (next_random(2) == 0)
				printf "depends(k%d, k%d).\n", i, next_random(i)
			if (next_random(3) == 0)
				printf "done(k%d).\n", i
		}
		print "waits(X, Y) :- depends(X, Y), not done(Y)."
		print "waits(X, Z) :- depends(X, Y), waits(Y, Z)."
		print "blocked(X) :- waits(X, _)."
		print "ready(X) :- task(X), not done(X), not blocked(X)."
	}'
}

# facts N: N ternary facts and a join over them, which is mostly loading.
facts() {
	awk -v n="$1" "$lcg"'
	BEGIN {
		state = 99
		for (i = 0; i < n; i++)
			printf "row(r%d, v%d, %d).\n", i, next_random(1000), \
			    next_random(100)
		print "pair(A, B) :- row(A, V, 7), row(B, V, 13)."
	}'
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

chain $((2000 * scale)) > "$work/chain.dl"
graph $((20000 * scale)) $((100000 * scale)) > "$work/graph.dl"
grid $((150 * scale)) > "$work/grid.dl"
tree $((12 + scale - 1)) > "$work/tree.dl"
tree $((9 + scale - 1)) > "$work/small-tree.dl"
tasks $((10000 * scale)) > "$work/tasks.dl"
facts $((300000 * scale)) > "$work/facts.dl"

printf '%-28s %8s %11s %10s %11s %11s\n' workload seconds derived held \
    scanned "looked up"
run "chain, right-recursive" "$work/chain.dl" -q 'path(n0, X)?'
run "chain, left-recursive" "$work/chain.dl" -q 'from(n0, X)?'
run "chain, whole closure" "$work/chain.dl" -X no-magic -q 'path(n0, X)?'
run "graph, reachability" "$work/graph.dl" -q 'reach(X)?'
run "graph, left-recursive" "$work/graph.dl" -q 'from(n17, X)?'
run "grid, reachability" "$work/grid.dl" -q 'reach(X)?'
run "tree, same generation" "$work/tree.dl" -q 'sg(t2048, X)?'
run "tree, all generations" "$work/small-tree.dl" -q 'sg(X, Y)?'
run "tasks, ready" "$work/tasks.dl" -q 'ready(X)?'
run "facts, load and join" "$work/facts.dl" -q 'pair(A, B)?'
