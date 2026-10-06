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
# 10^10 steps.  No counter measures that, so this workload is held to a
# budget of CPU time: linear, it takes a second or two, and quadratic, many
# minutes.
#
# An interactive session of 100 000 lines, each the same query over a small
# graph, prepares, answers and destroys a query per line.  A query's answer
# belongs to the query rather than to a predicate of the program, so every
# line costs what the program holds.  Were each query to leave anything
# behind that later answers walk, each line would cost every line before it,
# about 10^10 steps.  No counter measures such a walk, so this workload is
# held to a budget of CPU time like the cycle: linear, it takes a few
# seconds, and quadratic, many minutes.
#
# Two workloads plan long bodies, and must plan them in time proportional to
# the plans they produce rather than to the square of a body.  A recursive
# rule of 8000 literals, 500 of them reading its own predicate, has 500
# delta variants of 8000 operations each; its bound set grows once, so each
# variant is ordered in a pass or two.  A rule of 8000 literals over
# complete relations, called with 125 distinct bound patterns through rules
# a query reads them all through, is copied and ordered once per pattern by
# the Magic rewrite; that is far more than the default limit on the
# rewrite's work admits, so its runs raise the limit.  Each workload holds
# some 4 million operations or literals, and ordering them by rescoring the
# body at every step costs the square of the body per plan, 10^10 steps or
# more: a second or two each in passes, even under AddressSanitizer, and
# minutes step by step on an optimized build.
#
# Each planning workload then runs again under a memory limit of 16 MiB,
# far below the hundreds of megabytes its plans or its rewrite hold.  The
# plans and the rewrite are charged to the limit, so each must fail with the
# limit's message, naming what was being planned or rewritten, once it has
# built what the limit allows and no more, within the same budget.
#
# A rule of 400 calls on one predicate read on demand, r(X) :- p(X), ...,
# p(X), is answered through its Magic rewrite under the same memory limit.
# Each call's magic rule is derived from the literals before it; written out
# in full, those prefixes hold the square of the calls in literals and, in
# the recursive component they form, the cube in delta-variant operations,
# some 1.7 GB.  Cut into a supplementary predicate before every call after
# the first, each prefix is one literal and the rewrite is linear in the
# calls, a few megabytes.  The run must answer without a diagnostic: a
# rewrite abandoned over its limit would still answer, from the whole
# program, and say so.
#
# A rule s(X) :- p(_, X), ..., p(_, X) over two facts of p that differ only
# in their first column is answered from the whole program, with 48 calls
# and then 64.  Every call after the first has X bound and binds nothing
# the rule reads, so it is a test of whether p holds a tuple for X, and the
# rule derives s(x) once for each of the two tuples the first call scans.
# Planned as lookups, each call would continue the join once per tuple it
# matched, twice, and the rule would derive s(x) 2^48 or 2^64 times: hours,
# or never.  No counter is needed to tell them
# apart, so these workloads are held to the planning budget.
#
# RELIC_PERF_BUDGET is how many seconds of CPU time reachability may take,
# 600 unless set, which leaves room for an AddressSanitizer build on a slow
# machine; make check-msan and make check-tsan set more for MemorySanitizer
# and ThreadSanitizer, which are slower.  For reachability the budget only
# guards against a run that never ends.  The cycle and the session each have
# a tenth of it, and each planning workload a fortieth, but never less than
# five seconds.  The budgets are CPU time rather than time by the clock, so
# that a machine busy with other work, which stretches a run's time by the
# clock and not its CPU time, fails a run that is linear only when it
# stretches it past the whole budget, which every workload is also held to
# by the clock.

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
demand_calls=400
planning_memory=16777216

fail() {
	echo "$1" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# The seconds of CPU time a run may take: `seconds`, or the hard limit on CPU
# time this test inherited when that is lower, since a soft limit cannot be
# raised past the hard one.
cpu_seconds() {
	hard=$(ulimit -H -t)
	if [ "$hard" != unlimited ] && [ "$hard" -lt "$1" ]; then
		echo "$hard"
	else
		echo "$1"
	fi
}

# Run a command with its output going to the files named by the first two
# arguments, and return its exit status.  The command may use as many
# seconds of CPU time as the third argument says, which a soft limit set by
# ulimit holds it to: past them the system stops it with SIGXCPU.  A command
# that waits rather than computes uses no CPU time, so it is also killed once
# the whole budget has passed by the clock, which bounds every workload by
# what the sanitizer jobs' time allows.  The clock is polled rather than
# watched by a second process, which would outlive the run it guards unless
# it too were killed.
run_within() {
	out=$1
	err=$2
	limit=$(cpu_seconds "$3")
	shift 3

	(
		if ! ulimit -S -t "$limit"; then
			echo "CPU time cannot be limited to $limit seconds" >&2
			exit 125
		fi
		exec "$@"
	) > "$out" 2> "$err" &
	pid=$!

	elapsed=0
	while kill -0 "$pid" 2>/dev/null; do
		if [ "$elapsed" -ge "$budget" ]; then
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
	fail "reachability exited $status within a budget of $budget CPU seconds"
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
	fail "the recursive cycle exited $status within a budget of $cycle_budget CPU seconds"
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
	fail "the session of $session_lines queries exited $status within a budget of $cycle_budget CPU seconds"
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
	fail "a recursive body of $body_length literals exited $status within a budget of $planning_budget CPU seconds"
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
    "$RELIC" -l magic=100000000 -q 'all(X)?' "$work/patterns.dl" ||
    status=$?

if [ "$status" -ne 0 ]; then
	cat "$work/patterns-err" >&2
	fail "a body of $body_length literals under $patterns patterns exited $status within a budget of $planning_budget CPU seconds"
fi

if [ "$(cat "$work/patterns-rows")" != "a" ]; then
	fail "the patterns workload answered something other than a"
fi

if [ -s "$work/patterns-err" ]; then
	cat "$work/patterns-err" >&2
	fail "the patterns workload was not answered through its rewrite"
fi

# Run a planning workload under the memory limit, which must refuse it with
# the diagnostic given as the first argument.
refused_within() {
	expected=$1
	shift

	status=0
	run_within "$work/limited-rows" "$work/limited-err" \
	    "$planning_budget" "$RELIC" -l "memory=$planning_memory" "$@" ||
	    status=$?

	if [ "$status" -ne 1 ]; then
		cat "$work/limited-err" >&2
		fail "$* under a memory limit exited $status within a budget of $planning_budget CPU seconds, expected 1"
	fi

	if [ "$(cat "$work/limited-err")" != "relic: error: $expected" ]; then
		cat "$work/limited-err" >&2
		fail "$* under a memory limit was not refused with '$expected'"
	fi

	if [ -s "$work/limited-rows" ]; then
		fail "$* under a memory limit answered rows"
	fi
}

refused_within "limit exceeded: memory=$planning_memory while planning p/1" \
    -X no-magic -q 'p(X)?' "$work/long.dl"
refused_within \
    "limit exceeded: memory=$planning_memory while rewriting the query" \
    -l magic=100000000 -q 'all(X)?' "$work/patterns.dl"

awk -v calls="$demand_calls" 'BEGIN {
	for (i = 1; i <= 200; i++)
		printf "e(%d, %d).\n", i, i + 1;
	print "p(X) :- e(X, _Y).";
	printf "r(X) :- p(X)";
	for (i = 1; i < calls; i++)
		printf ", p(X)";
	print ".";
}' > "$work/calls.dl"

status=0
run_within "$work/calls-rows" "$work/calls-err" "$planning_budget" \
    "$RELIC" -l "memory=$planning_memory" -q 'r(1)?' "$work/calls.dl" ||
    status=$?

if [ "$status" -ne 0 ]; then
	cat "$work/calls-err" >&2
	fail "a rule of $demand_calls demand calls exited $status under a memory limit within a budget of $planning_budget CPU seconds"
fi

# r(1)? binds every column, so its one answer is the empty row.
if [ "$(wc -l < "$work/calls-rows")" -ne 1 ] ||
    [ -n "$(cat "$work/calls-rows")" ]; then
	fail "the demand calls workload answered something other than one empty row"
fi

if [ -s "$work/calls-err" ]; then
	cat "$work/calls-err" >&2
	fail "the demand calls workload was not answered through its rewrite"
fi

# Each call after the first tests p for the X the first bound, and the
# rule's one row is x.
for checks in 48 64; do
	awk -v calls="$checks" 'BEGIN {
		print "p(a, x).";
		print "p(b, x).";
		printf "s(X) :- p(_, X)";
		for (i = 1; i < calls; i++)
			printf ", p(_, X)";
		print ".";
	}' > "$work/checks.dl"

	status=0
	run_within "$work/checks-rows" "$work/checks-err" "$planning_budget" \
	    "$RELIC" -X no-magic -q 's(X)?' "$work/checks.dl" || status=$?

	if [ "$status" -ne 0 ]; then
		cat "$work/checks-err" >&2
		fail "a rule of $checks calls binding nothing it reads exited $status within a budget of $planning_budget CPU seconds"
	fi

	if [ "$(cat "$work/checks-rows")" != "x" ]; then
		fail "the rule of $checks calls answered something other than x"
	fi
done
