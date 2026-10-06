#!/bin/sh
#
# The command-line tool's handling of its input and output: a program read
# from standard input, a table imported from it, results written to a file
# with -o, a write that fails, and a query that fails part way through a
# run.  The golden language tests cover what the tool prints; these cover
# where it reads from and writes to, and what a run leaves behind, which a
# golden file cannot express.

set -eu

: "${RELIC:?RELIC must name the relic executable}"

fail() {
	echo "$1" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

cat > "$work/graph.dl" <<'EOF'
edge(a, b).
edge(b, c).
path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).
path(a, X)?
EOF

expected=$(printf 'b\nc')

# With no operands and standard input not a terminal, the program is read
# from standard input and its queries are answered.
actual=$("$RELIC" -s < "$work/graph.dl")
if [ "$actual" != "$expected" ]; then
	fail "standard input answered '$actual', expected '$expected'"
fi

# -q applies to a program read from standard input as it does to a file.
actual=$("$RELIC" -q 'edge(X, c)?' < "$work/graph.dl")
if [ "$actual" != "b" ]; then
	fail "-q over standard input answered '$actual', expected 'b'"
fi

# -o writes the results to a file and nothing to standard output, replacing
# whatever the file held.
echo stale > "$work/out.txt"
actual=$("$RELIC" -s -o "$work/out.txt" "$work/graph.dl")
if [ -n "$actual" ]; then
	fail "-o also wrote '$actual' to standard output"
fi
if [ "$(cat "$work/out.txt")" != "$expected" ]; then
	fail "-o wrote '$(cat "$work/out.txt")', expected '$expected'"
fi

# The output file is opened only once the program and its queries are known
# to be valid, so a run that fails validation leaves an existing file alone.
echo kept > "$work/out.txt"
status=0
"$RELIC" -o "$work/out.txt" -q 'X > 1?' "$work/graph.dl" 2>/dev/null ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "an invalid query exited $status, expected 1"
fi
if [ "$(cat "$work/out.txt")" != "kept" ]; then
	fail "a run that failed validation replaced the output file"
fi

# -c evaluates nothing, so it writes nothing, and -o does not replace the
# file it names.
echo kept > "$work/out.txt"
"$RELIC" -c -o "$work/out.txt" "$work/graph.dl"
if [ "$(cat "$work/out.txt")" != "kept" ]; then
	fail "-c replaced the -o file"
fi

# An output file that cannot be created is an error, not a usage error.
status=0
"$RELIC" -o "$work/missing/out.txt" "$work/graph.dl" 2>/dev/null ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "an unwritable -o exited $status, expected 1"
fi

# A write that fails must not be reported as success.  Standard output is
# open for reading only here, so every write to it fails.  A closed descriptor
# would fail the same way, but the AddressSanitizer runtime on macOS reports a
# flush to one as successful, and the test would then pass or fail with the
# build rather than with the tool.
status=0
diagnostic=$("$RELIC" "$work/graph.dl" 2>&1 1</dev/null) || status=$?
if [ "$status" -ne 1 ]; then
	fail "a failed write exited $status, expected 1"
fi
case "$diagnostic" in
*"cannot write standard output"*)
	;;
*)
	fail "a failed write was reported as '$diagnostic'"
	;;
esac

# Each query's results reach the stream before anything written after them,
# on a terminal as in a file: the next query's plan under -e, and the count
# of -v.  Standard error goes to the same file here, as a terminal shows
# both streams.
"$RELIC" -e -q 'edge(X, Y)?' -q 'path(a, X)?' "$work/graph.dl" \
    > "$work/merged.txt" 2>&1
rows_at=$(grep -n '^a b$' "$work/merged.txt" | head -n 1 | cut -d : -f 1)
plan_at=$(grep -n '^magic rewrite of ' "$work/merged.txt" | head -n 1 |
    cut -d : -f 1)
if [ -z "$rows_at" ] || [ -z "$plan_at" ] || [ "$rows_at" -gt "$plan_at" ]
then
	fail "the first query's rows did not come before the second's plan"
fi

"$RELIC" -v -q 'edge(X, Y)?' -q 'path(a, X)?' "$work/graph.dl" \
    > "$work/merged.txt" 2>&1
printf 'a b\nb c\nquery 1: 2 rows\n\nb\nc\nquery 2: 2 rows\n' \
    > "$work/merged.expected"
if ! cmp -s "$work/merged.expected" "$work/merged.txt"; then
	fail "-v counted queries out of order: '$(cat "$work/merged.txt")'"
fi

# Results reach the stream in blocks of 64 KiB and once more when a query's
# results end, and a write that fails is reported once, whichever write it
# is, and ends the run.  A file size limit makes the writes past it fail,
# with SIGXFSZ ignored so that they fail rather than end the process.  The
# shell counts the limit in blocks of 512 or 1024 bytes, and the sizes here
# allow either: a limit of 256 blocks lets at least two writes of 64 KiB
# through and stops the results of a 400-node chain, 750 KB of them, well
# short of their end.
awk 'BEGIN {
	for (i = 1; i < 400; i++)
		printf "edge(n%d, n%d).\n", i, i + 1
	print "path(X, Y) :- edge(X, Y)."
	print "path(X, Z) :- edge(X, Y), path(Y, Z)."
	print "path(X, Y)?"
}' > "$work/chain.dl"

# write_limited BLOCKS ARGUMENTS...: run the tool with ARGUMENTS, writing
# into $work/limited.txt under a file size limit of BLOCKS, and set `status`
# to its exit status and `diagnostic` to what it wrote to standard error.
# Standard error is read through a pipe, which the limit does not apply to,
# so that the report of the failure is not lost to the same limit.
write_limited() {
	status=0
	diagnostic=$(
		blocks=$1
		shift
		trap '' XFSZ
		ulimit -f "$blocks"
		exec "$RELIC" -o "$work/limited.txt" "$@" 2>&1
	) || status=$?
}

# expect_one_failure WHEN: the run of write_limited exited 1 and reported
# exactly one failed write.
expect_one_failure() {
	if [ "$status" -ne 1 ]; then
		fail "a write failing $1 exited $status, expected 1"
	fi
	if [ "$(printf '%s\n' "$diagnostic" | grep -c 'cannot write')" -ne 1 ]
	then
		fail "a write failing $1 was reported as '$diagnostic'"
	fi
}

write_limited 0 "$work/chain.dl"
expect_one_failure "at the first write"

write_limited 256 "$work/chain.dl"
expect_one_failure "at a later write"
written=$(wc -c < "$work/limited.txt")
if [ "$written" -lt 131072 ] || [ "$written" -gt 262144 ]; then
	fail "a write failing at a later write left $written bytes"
fi

write_limited 0 "$work/graph.dl"
expect_one_failure "at a query's last write"

# The query whose write failed is the last that runs: the next one's plan
# never appears.
write_limited 0 -e -q 'edge(X, Y)?' -q 'path(a, X)?' "$work/graph.dl"
expect_one_failure "before another query"
case "$diagnostic" in
*"magic rewrite of"*)
	fail "a query ran after a write failed"
	;;
esac

# A query that fails during evaluation writes nothing -- not even the blank
# line that would separate it from the query before it -- and ends the run,
# while what the earlier query wrote stands.  The program holds five tuples
# and the first query's answers two more, so a limit of seven lets the first
# query through and refuses the second's first answer.
#
# The output is compared as a file, byte for byte, because command
# substitution would strip the very trailing newline this is looking for.
status=0
"$RELIC" -l tuples=7 -q 'edge(X, Y)?' -q 'path(X, Y)?' \
    -q 'edge(a, X)?' "$work/graph.dl" > "$work/partial.txt" 2>/dev/null ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "a query failing in evaluation exited $status, expected 1"
fi
printf 'a b\nb c\n' > "$work/partial.expected"
if ! cmp -s "$work/partial.expected" "$work/partial.txt"; then
	fail "a query failing in evaluation left '$(cat "$work/partial.txt")'"
fi

# -n writes no rows, and a query that fails under it fails the run all the
# same: counting the answers is still answering the query.
status=0
diagnostic=$("$RELIC" -n -l tuples=7 -q 'edge(X, Y)?' -q 'path(X, Y)?' \
    "$work/graph.dl" 2>&1 >/dev/null) || status=$?
if [ "$status" -ne 1 ]; then
	fail "a query failing under -n exited $status, expected 1"
fi
case "$diagnostic" in
*"limit exceeded: tuples=7"*)
	;;
*)
	fail "a query failing under -n was reported as '$diagnostic'"
	;;
esac

# -X accepts only the behaviours that exist.  Any other name is a usage error
# naming it, rather than a no-op that would let a comparison run a strategy
# against itself and pass.
status=0
diagnostic=$("$RELIC" -X bogus "$work/graph.dl" 2>&1 >/dev/null) ||
    status=$?
if [ "$status" -ne 2 ]; then
	fail "an unknown -X exited $status, expected 2"
fi
case "$diagnostic" in
*"unknown debug behaviour: bogus"*)
	;;
*)
	fail "an unknown -X was reported as '$diagnostic'"
	;;
esac

# The planner's references, answering without the Magic rewrite, gathering
# every answer before writing it, deferring every round's insertions and
# compiling every aggregate body to a group rule are selected by name and
# answer as the default does.
for reference in no-index no-reorder no-magic no-stream parallel \
    group-rules; do
	actual=$("$RELIC" -s -X "$reference" "$work/graph.dl")
	if [ "$actual" != "$expected" ]; then
		fail "-X $reference answered '$actual', expected '$expected'"
	fi
done

# -e writes the plans to standard error, leaving standard output the rows
# alone, and with -o the file holds the rows and nothing else.
actual=$("$RELIC" -e -s "$work/graph.dl" 2> "$work/plans.txt")
if [ "$actual" != "$expected" ]; then
	fail "-e changed the rows to '$actual', expected '$expected'"
fi
if ! grep -q -e '^query \$q0(X) :- path\$bf(a, X)\.$' "$work/plans.txt"; then
	fail "-e wrote no plan for the query: '$(cat "$work/plans.txt")'"
fi
"$RELIC" -e -s -o "$work/out.txt" "$work/graph.dl" 2>/dev/null
if [ "$(cat "$work/out.txt")" != "$expected" ]; then
	fail "-e with -o wrote '$(cat "$work/out.txt")', expected '$expected'"
fi

# -X stats reports the counters even when evaluation fails, after the error
# that ended it.
status=0
diagnostic=$("$RELIC" -X stats -l tuples=3 "$work/graph.dl" 2>&1 \
    >/dev/null) || status=$?
if [ "$status" -ne 1 ]; then
	fail "a failing run with -X stats exited $status, expected 1"
fi
case "$diagnostic" in
*"limit exceeded: tuples=3"*"stats: iterations"*)
	;;
*)
	fail "a failing run with -X stats reported '$diagnostic'"
	;;
esac

# -l sets each limit the library enforces, by the key the diagnostic names
# it by, and zero asks for the default.  A repeated key takes its later value.
# The memory limit counts the symbol pool a load's constants fill, so a
# limit of one byte refuses graph.dl's load at its first constant, before any
# predicate has a store to name, and the refusal names the file being
# loaded.  The run
# names it relative to the work directory, so the message holds the name as
# written whatever the temporary directory's path.
status=0
diagnostic=$(cd "$work" && "$RELIC" -l memory=1 graph.dl 2>&1 >/dev/null) ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "an exceeded memory limit exited $status, expected 1"
fi
case "$diagnostic" in
*"limit exceeded: memory=1 while loading graph.dl")
	;;
*)
	fail "an exceeded memory limit was reported as '$diagnostic'"
	;;
esac

# The limit also counts the working state of a query's Magic rewrite.  A
# kilobyte past what the program holds without its query, a program whose
# fact holds the query's constant already, has room for the query and not
# for the rewrite's first allocation, which is refused, naming the query.
rules=$(printf '%s\n' 'p(X) :- q(X).' 'q(X) :- p(X).' 'r(a).')
held=$(printf '%s\n' "$rules" | "$RELIC" -X stats 2>&1 >/dev/null |
    sed -n 's/^stats: storage bytes //p')
status=0
diagnostic=$(printf '%s\n' "$rules" 'p(a)?' |
    "$RELIC" -l memory=$((held + 1024)) 2>&1 >/dev/null) || status=$?
if [ "$status" -ne 1 ]; then
	fail "an exceeded memory limit in a rewrite exited $status, expected 1"
fi
case "$diagnostic" in
*"limit exceeded: memory=$((held + 1024)) while rewriting the query"*)
	;;
*)
	fail "an exceeded memory limit in a rewrite was reported as '$diagnostic'"
	;;
esac

# The memory limit counts the plans an evaluation holds as well as its
# relations, so a limit smaller than the evaluation arena's first chunk
# refuses the first component's plans, named by its first member, before any
# tuple is derived: two/2, since edge/2 holds facts alone and has no
# component to plan.  Building an index is relation storage like any other,
# and a limit that admits the plans and a relation's tuples can refuse the
# index a later rule looks them up through; that refusal names the relation
# being indexed.
i=0
: > "$work/chain.dl"
while [ "$i" -lt 100 ]; do
	echo "edge(n$i, n$((i + 1)))." >> "$work/chain.dl"
	i=$((i + 1))
done
echo 'two(X, Z) :- edge(X, Y), edge(Y, Z).' >> "$work/chain.dl"

status=0
diagnostic=$("$RELIC" -a -l memory=15000 "$work/chain.dl" 2>&1 >/dev/null) ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "refused plans exited $status, expected 1"
fi
case "$diagnostic" in
*"limit exceeded: memory=15000 while planning two/2"*)
	;;
*)
	fail "refused plans were reported as '$diagnostic'"
	;;
esac

status=0
diagnostic=$("$RELIC" -a -l memory=20000 "$work/chain.dl" 2>&1 >/dev/null) ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "a refused index build exited $status, expected 1"
fi
case "$diagnostic" in
*"limit exceeded: memory=20000 while indexing edge/2"*)
	;;
*)
	fail "a refused index build was reported as '$diagnostic'"
	;;
esac

# A refusal is reported once, as the limit it is, naming what it fell in:
# the load interning the program's facts, the check compiling its rules, the
# plans of the rules allocated one after another, tuples being derived, or an
# index being built.  Each limit of the sweep refuses the program somewhere,
# so each run fails with exactly one such message: none would be a failure
# in silence, and a second diagnostic after the first -- an allocation
# attempted after a refused one, say -- would misreport the refusal as the
# allocator running out.
{
	echo 'e(1, 2).'
	echo 'e(2, 3).'
	echo 'e(3, 4).'
	for i in 1 2 3 4 5; do
		echo "r$i(X, W) :- e(X, Y), e(Y, Z), e(Z, W)."
	done
} > "$work/rules.dl"

limit=3000
while [ "$limit" -le 10000 ]; do
	status=0
	diagnostic=$(cd "$work" && "$RELIC" -a -l memory=$limit rules.dl 2>&1 \
	    >/dev/null) || status=$?
	refused="relic: error: limit exceeded: memory=$limit while"
	case "$diagnostic" in
	*"
"*)
		fail "memory=$limit was reported more than once: '$diagnostic'"
		;;
	esac
	case "$status:$diagnostic" in
	1:"$refused loading rules.dl" | 1:"$refused checking the program" | \
	    1:"$refused "[a-z]*" "[a-z]*/[0-9]*)
		;;
	*)
		fail "memory=$limit exited $status, reported as '$diagnostic'"
		;;
	esac
	limit=$((limit + 100))
done

# The Magic limit is a ceiling on the rewrite, not on the query: a rewrite
# that outgrows it is abandoned with one warning naming the limit and the
# predicate being adorned, and the query is answered from the whole program,
# with the same rows and a status of 0.
status=0
actual=$("$RELIC" -s -l magic=1 "$work/graph.dl" 2>"$work/magic.err") ||
    status=$?
if [ "$status" -ne 0 ]; then
	fail "an outgrown magic limit exited $status, expected 0"
fi
if [ "$actual" != "$expected" ]; then
	fail "an outgrown magic limit answered '$actual', expected '$expected'"
fi
outgrown="relic: warning: the query's Magic rewrite outgrew magic=1 while"
outgrown="$outgrown adorning path/2; answering it from the whole program"
if [ "$(cat "$work/magic.err")" != "$outgrown" ]; then
	fail "an outgrown magic limit was reported as '$(cat "$work/magic.err")'"
fi

actual=$("$RELIC" -s -l tuples=1 -l tuples=0 -l memory=0 \
    -l magic=1 -l magic=0 "$work/graph.dl")
if [ "$actual" != "$expected" ]; then
	fail "limits of zero answered '$actual', expected '$expected'"
fi

# A -l that is not a limit is a usage error naming what was wrong with it:
# no `=`, a key that names no limit, and a value that is empty, not a
# number, signed, surrounded by space, followed by other bytes, or too large
# for a size.
for argument in tuples tuples5 bogus=1 =1 tuples= tuples=many \
    tuples=-1 tuples=+1 tuples=-0 'tuples= 5' tuples=5x 'tuples=5 ' \
    tuples=99999999999999999999999; do
	status=0
	diagnostic=$("$RELIC" -l "$argument" "$work/graph.dl" 2>&1 \
	    >/dev/null) || status=$?
	if [ "$status" -ne 2 ]; then
		fail "-l '$argument' exited $status, expected 2"
	fi
	case "$diagnostic" in
	*"key=value: $argument"*|*"unknown limit: $argument"*|\
	*"limit value is "*": $argument"*)
		;;
	*)
		fail "-l '$argument' was reported as '$diagnostic'"
		;;
	esac
done

# -j shares evaluation among threads, 0 among one per processor, and the
# rows are the ones one thread writes, in the same order, whether the rounds
# are shared by their size or all of them under -X parallel.
for threads in 1 2 16 0 256; do
	actual=$("$RELIC" -s -j "$threads" "$work/graph.dl")
	if [ "$actual" != "$expected" ]; then
		fail "-j $threads answered '$actual', expected '$expected'"
	fi
done
for threads in 1 2 16; do
	actual=$("$RELIC" -s -j "$threads" -X parallel "$work/graph.dl")
	if [ "$actual" != "$expected" ]; then
		fail "-j $threads -X parallel answered '$actual', expected" \
		    "'$expected'"
	fi
done

# A -j that is not a number from 0 to 256 is a usage error naming it: empty,
# signed, too large, not a number, surrounded by space or followed by other
# bytes.
for argument in '' -1 +1 257 99999999999999999999 many 4x ' 4' '4 '; do
	status=0
	diagnostic=$("$RELIC" -j "$argument" "$work/graph.dl" 2>&1 \
	    >/dev/null) || status=$?
	if [ "$status" -ne 2 ]; then
		fail "-j '$argument' exited $status, expected 2"
	fi
	case "$diagnostic" in
	*"thread count is "*": $argument"*)
		;;
	*)
		fail "-j '$argument' was reported as '$diagnostic'"
		;;
	esac
done

# An import of - reads its table from standard input, and with no file
# operand the program is empty rather than read from there too.
actual=$(printf 'a\tb\na\tc\n' |
    "$RELIC" -s -I tsv:edge:symbol,symbol=- -q 'edge(a, X)?')
if [ "$actual" != "$expected" ]; then
	fail "an import of - with no program answered '$actual'"
fi

# A table named by path claims nothing, so a program piped to standard input
# is read as it always is, and its rules reach the table's facts.
printf 'c\td\n' > "$work/edge.tsv"
actual=$("$RELIC" -s -I "tsv:edge:symbol,symbol=$work/edge.tsv" \
    < "$work/graph.dl")
if [ "$actual" != "$(printf 'b\nc\nd')" ]; then
	fail "a piped program with an import answered '$actual'"
fi

# Standard input has one reader, so two imports of -, or one under -i, are a
# usage error naming both, and nothing is read.
for claims in "-I tsv:a:symbol=- -I csv:b:symbol=-" \
    "-i -I tsv:a:symbol=-"; do
	status=0
	# shellcheck disable=SC2086 # the claims are separate options
	diagnostic=$(echo 'x' | "$RELIC" $claims "$work/graph.dl" 2>&1 \
	    >/dev/null) || status=$?
	if [ "$status" -ne 2 ]; then
		fail "'$claims' exited $status, expected 2"
	fi
	case "$diagnostic" in
	*"standard input has two readers: "*)
		;;
	*)
		fail "'$claims' was reported as '$diagnostic'"
		;;
	esac
done

# A file that does not exist is an error in the input, reported once.
status=0
"$RELIC" "$work/absent.dl" 2>/dev/null || status=$?
if [ "$status" -ne 1 ]; then
	fail "a missing file exited $status, expected 1"
fi

# A directory opens for reading on the systems relic runs on, and reading it
# fails.  That is an error in the input like a missing file, and it names the
# operand; the reason is the system's, and its wording is not compared.
status=0
diagnostic=$("$RELIC" "$work" 2>&1 >/dev/null) || status=$?
if [ "$status" -ne 1 ]; then
	fail "a directory operand exited $status, expected 1"
fi
case "$diagnostic" in
*"cannot read $work: "?*)
	;;
*)
	fail "a directory operand was reported as '$diagnostic'"
	;;
esac

# An unknown option is named in the usage error, and a byte that would not
# print is named by its value in hexadecimal instead of written to the
# terminal.
status=0
diagnostic=$("$RELIC" "-$(printf '\001')" "$work/graph.dl" 2>&1 \
    >/dev/null) || status=$?
if [ "$status" -ne 2 ]; then
	fail "an unprintable option exited $status, expected 2"
fi
case "$diagnostic" in
*"unknown option -- \\x01"*)
	;;
*)
	fail "an unprintable option was reported as '$diagnostic'"
	;;
esac

exit 0
