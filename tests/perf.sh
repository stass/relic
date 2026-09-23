#!/bin/sh
#
# Two workloads that must take time proportional to their size rather than
# to its square.
#
# Reachability over a graph of one million edges finishes, answers every
# node, and does work proportional to the graph rather than to its square.
#
# The graph has 100 000 nodes: a chain n0 -> n1 -> ... -> n99999, which makes
# every node reachable from n0, and 900 001 more edges between nodes drawn
# from a fixed linear congruential sequence, so the graph is the same on every
# run and every platform.  The multiplier keeps every intermediate product
# below 2^53, where awk's arithmetic is exact.
#
# The regression check is on the counters, not the clock.  With an index the
# recursive rule looks up the edges leaving each newly reached node, so every
# edge is read about once and the tuples scanned and looked up together stay
# near the edge count; a planner that scanned the edges instead would read all
# of them once per reached node, some 10^11 tuples.  The bound below is
# generous against the first and unreachable by the second, and it does not
# depend on how fast the machine is.
#
# A recursive cycle of 200 000 predicates, p0 :- p1, p1 :- p2 and so on back
# to p0, with one fact, is answered through its Magic rewrite.  Everything is
# one component, and the fact travels one predicate per semi-naive round, so
# the fixed point takes as many rounds as the cycle is long.  Each round must
# cost what changed in the previous one, and finding what the query reads and
# generating each adornment's rules must cost what the rules hold; any of the
# three done by searching the whole component makes the run quadratic, about
# 10^10 steps.  No counter measures that, so this workload is on the clock:
# linear, it takes a second or two, and quadratic, many minutes.
#
# RELIC_PERF_BUDGET is how many seconds reachability may take, 600 unless
# set, which leaves room for a sanitizer build on a slow machine; for
# reachability the clock only guards against a run that never ends.  The
# cycle has a tenth of it.

set -eu

: "${RELIC:?RELIC must name the relic executable}"

budget=${RELIC_PERF_BUDGET:-600}
cycle_budget=$((budget / 10))
nodes=100000
edges=1000000
work_bound=2500000
cycle=200000

fail() {
	echo "$1" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# Run a command with its output going to the files named by the first two
# arguments, killing it if it has not finished within as many seconds as the
# third says, and return its exit status.  Polling rather than a watchdog
# process, which would outlive the run it guards unless it too were killed.
run_within() {
	out=$1
	err=$2
	seconds=$3
	shift 3

	"$@" > "$out" 2> "$err" &
	pid=$!

	elapsed=0
	while kill -0 "$pid" 2>/dev/null; do
		if [ "$elapsed" -ge "$seconds" ]; then
			kill "$pid" 2>/dev/null || true
			break
		fi
		sleep 1
		elapsed=$((elapsed + 1))
	done

	status=0
	wait "$pid" || status=$?
	return "$status"
}

awk -v nodes="$nodes" -v edges="$edges" 'BEGIN {
	for (i = 0; i + 1 < nodes; i++)
		printf "edge(n%d, n%d).\n", i, i + 1;

	x = 12345;
	for (i = nodes - 1; i < edges; i++) {
		x = (x * 69069 + 1) % 4294967296;
		from = int(x / 4294967296 * nodes);
		x = (x * 69069 + 1) % 4294967296;
		to = int(x / 4294967296 * nodes);
		printf "edge(n%d, n%d).\n", from, to;
	}

	print "reach(n0).";
	print "reach(Y) :- reach(X), edge(X, Y).";
}' > "$work/graph.dl"

status=0
run_within "$work/rows" "$work/stats" "$budget" \
    "$RELIC" -X stats -q 'reach(X)?' "$work/graph.dl" || status=$?

if [ "$status" -ne 0 ]; then
	cat "$work/stats" >&2
	fail "reachability exited $status within a budget of $budget seconds"
fi

rows=$(wc -l < "$work/rows" | tr -d ' ')
if [ "$rows" -ne "$nodes" ]; then
	fail "reachability answered $rows nodes, expected $nodes"
fi

# The counter lines are "stats: <name> <value>"; the value is the last field.
counter() {
	awk -v name="stats: $1 " 'index($0, name) == 1 { print $NF }' \
	    "$work/stats"
}

scanned=$(counter "tuples scanned")
looked_up=$(counter "tuples looked up")
if [ -z "$scanned" ] || [ -z "$looked_up" ]; then
	cat "$work/stats" >&2
	fail "the run reported no scan or lookup counters"
fi

work_done=$((scanned + looked_up))
if [ "$work_done" -gt "$work_bound" ]; then
	fail "reachability read $work_done tuples ($scanned scanned, $looked_up looked up), more than the bound of $work_bound"
fi

awk -v count="$cycle" 'BEGIN {
	printf "p%d(a).\n", count - 1;
	printf "p%d(X) :- p0(X).\n", count - 1;
	for (i = count - 1; i > 0; i--)
		printf "p%d(X) :- p%d(X).\n", i - 1, i;
}' > "$work/cycle.dl"

status=0
run_within "$work/cycle-rows" "$work/cycle-err" "$cycle_budget" \
    "$RELIC" -q 'p0(X)?' "$work/cycle.dl" || status=$?

if [ "$status" -ne 0 ]; then
	cat "$work/cycle-err" >&2
	fail "the recursive cycle exited $status within a budget of $cycle_budget seconds"
fi

if [ "$(cat "$work/cycle-rows")" != "a" ]; then
	fail "the recursive cycle answered something other than a"
fi
