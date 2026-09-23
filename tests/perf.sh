#!/bin/sh
#
# Workloads that must take time proportional to their size rather than to
# its square.
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
# An interactive session of 100 000 lines, each the same query over a small
# graph, prepares, answers and destroys a query per line.  A query's answer
# belongs to the query rather than to a predicate of the program, so every
# line costs what the program holds.  Were each query to leave anything
# behind that later answers walk, each line would cost every line before it,
# about 10^10 steps.  No counter measures such a walk, so this workload is on
# the clock like the cycle: linear, it takes a few seconds, and quadratic,
# many minutes.
#
# Two workloads plan long bodies, and must plan them in time proportional to
# the plans they produce rather than to the square of a body.  A recursive
# rule of 8000 literals, 500 of them reading its own predicate, has 500
# delta variants of 8000 operations each; its bound set grows once, so each
# variant is ordered in a pass or two.  A rule of 8000 literals over
# complete relations, called with 125 distinct bound patterns through rules
# a query reads them all through, is copied and ordered once per pattern by
# the Magic rewrite.  Each holds some 4 million operations or literals, and
# ordering them by rescoring the body at every step costs the square of the
# body per plan, 10^10 steps or more: a second or two each in passes, even
# under a sanitizer, and minutes step by step on an optimized build.
#
# RELIC_PERF_BUDGET is how many seconds reachability may take, 600 unless
# set, which leaves room for a sanitizer build on a slow machine; for
# reachability the clock only guards against a run that never ends.  The
# cycle and the session each have a tenth of it, and each planning workload
# a fortieth, but never less than five seconds.

set -eu

: "${RELIC:?RELIC must name the relic executable}"

budget=${RELIC_PERF_BUDGET:-600}
cycle_budget=$((budget / 10))
planning_budget=$((budget / 40))
if [ "$planning_budget" -lt 5 ]; then
	planning_budget=5
fi
nodes=100000
edges=1000000
work_bound=2500000
cycle=200000
session_lines=100000
body_length=8000
recursive_every=16
patterns=125

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

cat > "$work/session.dl" <<'EOF'
edge(a, b).
edge(b, c).
edge(c, d).
path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).
EOF

awk -v count="$session_lines" 'BEGIN {
	for (i = 0; i < count; i++)
		print "path(a, X)?";
}' > "$work/session.in"

# A command run in the background reads its standard input from /dev/null,
# so the session's lines are redirected inside the command, and exec keeps
# the process run_within watches the session itself.
status=0
run_within "$work/session-rows" "$work/session-err" "$cycle_budget" \
    sh -c 'exec "$0" -i "$1" < "$2"' \
    "$RELIC" "$work/session.dl" "$work/session.in" || status=$?

if [ "$status" -ne 0 ]; then
	cat "$work/session-err" >&2
	fail "the session of $session_lines queries exited $status within a budget of $cycle_budget seconds"
fi

rows=$(grep -c . "$work/session-rows" || true)
if [ "$rows" -ne $((session_lines * 3)) ]; then
	fail "the session answered $rows rows, expected $((session_lines * 3))"
fi

# The recursive body is evaluated from the whole program, so that what is
# measured is planning its variants; the Magic rewrite of the same query
# generates a magic rule per call, which is a cost of its own.
awk -v length_="$body_length" -v every="$recursive_every" 'BEGIN {
	print "p(a).";
	print "q(a).";
	printf "p(X) :- p(X)";
	for (i = 1; i < length_; i++)
		printf ", %s(X)", (i % every == 0 ? "p" : "q");
	print ".";
}' > "$work/long.dl"

status=0
run_within "$work/long-rows" "$work/long-err" "$planning_budget" \
    "$RELIC" -X no-magic -q 'p(X)?' "$work/long.dl" || status=$?

if [ "$status" -ne 0 ]; then
	cat "$work/long-err" >&2
	fail "a recursive body of $body_length literals exited $status within a budget of $planning_budget seconds"
fi

if [ "$(cat "$work/long-rows")" != "a" ]; then
	fail "the recursive body answered something other than a"
fi

# p/12 reads q/2 only, and its body binds all twelve variables within a few
# literals, so each copy is ordered in a few passes.  Pattern n binds the
# columns of the binary digits of n + 1, and every caller cN is read by all.
awk -v length_="$body_length" -v patterns="$patterns" 'BEGIN {
	print "e(a).";
	print "q(a, a).";
	printf "p(A0, A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11) :- ";
	for (i = 0; i < length_; i++)
		printf "%sq(A%d, A%d)", (i > 0 ? ", " : ""), i % 12,
		    (i * 5 + 1) % 12;
	print ".";
	for (n = 0; n < patterns; n++) {
		printf "c%d(X) :- e(X), p(", n;
		for (c = 0; c < 12; c++)
			printf "%s%s", (c > 0 ? ", " : ""),
			    (int((n + 1) / 2 ^ c) % 2 == 1 ? "X" : "_");
		print ").";
		printf "all(X) :- c%d(X).\n", n;
	}
}' > "$work/patterns.dl"

status=0
run_within "$work/patterns-rows" "$work/patterns-err" "$planning_budget" \
    "$RELIC" -q 'all(X)?' "$work/patterns.dl" || status=$?

if [ "$status" -ne 0 ]; then
	cat "$work/patterns-err" >&2
	fail "a body of $body_length literals under $patterns patterns exited $status within a budget of $planning_budget seconds"
fi

if [ "$(cat "$work/patterns-rows")" != "a" ]; then
	fail "the patterns workload answered something other than a"
fi

if [ -s "$work/patterns-err" ]; then
	cat "$work/patterns-err" >&2
	fail "the patterns workload was not answered through its rewrite"
fi
