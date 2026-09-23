#!/bin/sh
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
# The clock only guards against a run that never ends.  RELIC_PERF_BUDGET is
# how many seconds it may take, 600 unless set, which leaves room for a
# sanitizer build on a slow machine.

set -eu

: "${RELIC:?RELIC must name the relic executable}"

budget=${RELIC_PERF_BUDGET:-600}
nodes=100000
edges=1000000
work_bound=2500000

fail() {
	echo "$1" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

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

"$RELIC" -X stats -q 'reach(X)?' "$work/graph.dl" \
    > "$work/rows" 2> "$work/stats" &
relic_pid=$!

# Poll rather than start a watchdog process, which would outlive the run it
# guards unless it too were killed.
elapsed=0
while kill -0 "$relic_pid" 2>/dev/null; do
	if [ "$elapsed" -ge "$budget" ]; then
		kill "$relic_pid" 2>/dev/null || true
		break
	fi
	sleep 1
	elapsed=$((elapsed + 1))
done

status=0
wait "$relic_pid" || status=$?

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
