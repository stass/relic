#!/bin/sh
#
# The command-line tool's handling of its input and output: a program read
# from standard input, a table imported from it, results written to a file
# with -o, relations written to files with -O, a write that fails, and a
# query that fails part way through a run.  The golden language tests cover
# what the tool prints; these cover where it reads from and writes to, and
# what a run leaves behind, which a golden file cannot express.

set -eu

: "${RELIC:?RELIC must name the relic executable}"

fail() {
	echo "$*" >&2
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

# The output file is opened before the program is read but truncated only
# once results reach it, so a run that fails validation leaves an existing
# file alone.
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

# -c evaluates nothing, so it writes nothing, and -o neither replaces the
# file it names nor creates one.
echo kept > "$work/out.txt"
"$RELIC" -c -o "$work/out.txt" "$work/graph.dl"
if [ "$(cat "$work/out.txt")" != "kept" ]; then
	fail "-c replaced the -o file"
fi
"$RELIC" -c -o "$work/never.txt" "$work/graph.dl"
if [ -e "$work/never.txt" ]; then
	fail "-c created the -o file"
fi

# An output file that cannot be created is an error, not a usage error.
status=0
"$RELIC" -o "$work/missing/out.txt" "$work/graph.dl" 2>/dev/null ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "an unwritable -o exited $status, expected 1"
fi

# The -o file is truncated only once results reach it, so a run whose
# evaluation fails at a limit leaves an existing file as it was.
echo precious > "$work/keep.txt"
status=0
"$RELIC" -o "$work/keep.txt" -l tuples=2 "$work/graph.dl" 2>/dev/null ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "a run over the tuple limit exited $status, expected 1"
fi
if [ "$(cat "$work/keep.txt")" != "precious" ]; then
	fail "a run that failed in evaluation replaced the -o file"
fi

# A run that succeeds with no query to answer still leaves the -o file
# holding exactly its results, which are none.
printf 'edge(a, b).\n' > "$work/facts.dl"
echo stale > "$work/out.txt"
"$RELIC" -o "$work/out.txt" "$work/facts.dl"
if [ -s "$work/out.txt" ]; then
	fail "a run with no query left '$(cat "$work/out.txt")' in the -o file"
fi

# An empty -o is a usage error, as an empty -I or -O is.
status=0
"$RELIC" -o '' "$work/graph.dl" >/dev/null 2>&1 || status=$?
if [ "$status" -ne 2 ]; then
	fail "an empty -o exited $status, expected 2"
fi

# No output may be the same file as anything else the run reads or writes,
# however the command line spells it: as written, through ./, a symbolic or
# a hard link, standard input or output redirected to it, and a file that
# does not exist yet named twice.  Each is a usage error that writes
# nothing, and every file is left as it was, or, created to be compared,
# removed.
cp "$work/graph.dl" "$work/q.dl"
ln -s q.dl "$work/q-symlink.dl"
ln "$work/q.dl" "$work/q-hardlink.dl"
printf 'a\tb\n' > "$work/edge.tsv"
rm -f "$work/new.txt"
for run in "-o $work/q.dl $work/q.dl" \
    "-O dl:path=$work/./q.dl $work/q.dl" \
    "-o $work/q-symlink.dl $work/q.dl" \
    "-O dl:path=$work/q-hardlink.dl $work/q.dl" \
    "-T edge:symbol,symbol -I $work/edge.tsv -o $work/./edge.tsv $work/q.dl" \
    "-o $work/new.txt -O dl:path=$work/./new.txt $work/q.dl" \
    "-O tsv:edge=$work/new.txt -O dl:path=$work/../${work##*/}/new.txt $work/q.dl"
do
	cp "$work/q.dl" "$work/q.kept"
	cp "$work/edge.tsv" "$work/edge.kept"
	status=0
	# shellcheck disable=SC2086 # the run's options are separate words
	"$RELIC" $run >/dev/null 2>&1 || status=$?
	if [ "$status" -ne 2 ]; then
		fail "an output that is another file of the run exited $status, expected 2: $run"
	fi
	if ! cmp -s "$work/q.dl" "$work/q.kept" ||
	    ! cmp -s "$work/edge.tsv" "$work/edge.kept"; then
		fail "an output replaced a file the run reads: $run"
	fi
	if [ -e "$work/new.txt" ]; then
		fail "a refused run created an output: $run"
	fi
done

# The program read from standard input is a file the run reads.
cp "$work/q.dl" "$work/q.kept"
status=0
"$RELIC" -o "$work/q.dl" < "$work/q.dl" >/dev/null 2>&1 || status=$?
if [ "$status" -ne 2 ] || ! cmp -s "$work/q.dl" "$work/q.kept"; then
	fail "-o over the program on standard input exited $status, expected 2"
fi

status=0
"$RELIC" -o /dev/stdout -O "tsv:path=$work/stdout.tsv" "$work/q.dl" \
    >"$work/stdout.tsv" 2>/dev/null || status=$?
if [ "$status" -ne 2 ]; then
	fail "-o /dev/stdout and an export to its file exited $status, expected 2"
fi

# -c writes nothing to standard output, so appending it to the program is
# not an output over an input.
"$RELIC" -c "$work/q.dl" >>"$work/q.dl" ||
    fail "-c with standard output appended to the program was refused"
if ! cmp -s "$work/q.dl" "$work/q.kept"; then
	fail "-c changed the program its standard output is appended to"
fi

# Standard output redirected to a file is an output the results go to.
status=0
diagnostic=$(cd "$work" && "$RELIC" -O tsv:path=stdout.tsv q.dl 2>&1 \
    >stdout.tsv) || status=$?
if [ "$status" -ne 2 ]; then
	fail "an export to the file standard output writes exited $status, expected 2"
fi
case "$diagnostic" in
*"error: -O tsv:path=stdout.tsv and the standard output are the same file"*)
	;;
*)
	fail "an export to standard output's file was reported as '$diagnostic'"
	;;
esac

# The diagnostic names both spellings of the one file.
status=0
diagnostic=$(cd "$work" && "$RELIC" -o q-symlink.dl q.dl 2>&1 >/dev/null) ||
    status=$?
case "$diagnostic" in
*"error: -o q-symlink.dl and the program file q.dl are the same file"*)
	;;
*)
	fail "an -o over the program was reported as '$diagnostic'"
	;;
esac

# A device is not a file an output replaces, so any number of them may
# write to /dev/null.
"$RELIC" -o /dev/null -O tsv:path=/dev/null -O dl:path=/dev/null \
    "$work/q.dl" || fail "two outputs to /dev/null were refused"

# expect_one_new_file ABSENT ARGUMENTS...: a run over q.dl writing the
# outputs ARGUMENTS name, two names of one file that does not exist yet, is
# refused as a usage error and leaves no file at ABSENT, the one it would
# have created.
expect_one_new_file() {
	absent=$1
	shift
	status=0
	"$RELIC" "$@" "$work/q.dl" >/dev/null 2>&1 || status=$?
	if [ "$status" -ne 2 ]; then
		fail "two names of one new file exited $status, expected 2: $*"
	fi
	if [ -e "$absent" ]; then
		fail "a refused run left $absent behind: $*"
	fi
}

# Two names of one file that does not exist yet are caught however they
# differ: through a symbolic link to the directory, through a symbolic link
# whose target does not exist yet and that target, and, on a file system
# that ignores case, by case alone.
mkdir "$work/d1"
ln -s d1 "$work/d2"
expect_one_new_file "$work/d1/x.txt" \
    -o "$work/d1/x.txt" -O "dl:path=$work/d2/x.txt"
ln -s made.txt "$work/dangling"
expect_one_new_file "$work/made.txt" \
    -o "$work/dangling" -O "dl:path=$work/made.txt"
if [ ! -L "$work/dangling" ]; then
	fail "a refused run removed the symbolic link it was given"
fi
touch "$work/CaseProbe"
if [ -e "$work/caseprobe" ]; then
	expect_one_new_file "$work/Case.txt" \
	    -o "$work/Case.txt" -O "dl:path=$work/case.txt"
fi

# An output that cannot be opened is reported before anything is evaluated:
# no plan under -e, no row on standard output, the one error and exit 1.
# expect_unopenable WHAT ARGUMENTS...: the run over graph.dl writing the
# output ARGUMENTS name, which WHAT describes, fails that way.
expect_unopenable() {
	what=$1
	shift
	status=0
	"$RELIC" -e "$@" "$work/graph.dl" >"$work/rows.txt" \
	    2>"$work/diagnostic.txt" || status=$?
	if [ "$status" -ne 1 ]; then
		fail "$what exited $status, expected 1"
	fi
	if [ -s "$work/rows.txt" ]; then
		fail "$what was reported after rows were written"
	fi
	if [ "$(wc -l < "$work/diagnostic.txt")" -ne 1 ] ||
	    ! grep -q 'cannot open' "$work/diagnostic.txt"; then
		fail "$what was reported as '$(cat "$work/diagnostic.txt")'"
	fi
}

expect_unopenable "an -o in a missing directory" -o "$work/missing/out.txt"
expect_unopenable "an export in a missing directory" \
    -O "tsv:edge=$work/missing/edge.tsv"
if [ "$(id -u)" -ne 0 ]; then
	mkdir "$work/read-only"
	chmod 555 "$work/read-only"
	expect_unopenable "an -o in a read-only directory" \
	    -o "$work/read-only/out.txt"
fi

# A run that fails before writing an output removes the file it created for
# it, through a symbolic link or a chain of them included, and leaves an
# existing file as it was: whether validation fails or evaluation does.
echo precious > "$work/keep.tsv"
status=0
"$RELIC" -q 'X > 1?' -o "$work/new.txt" -O "tsv:path=$work/keep.tsv" \
    "$work/graph.dl" 2>/dev/null || status=$?
if [ "$status" -ne 1 ]; then
	fail "an invalid query exited $status, expected 1"
fi
ln -s made-through-hops.tsv "$work/hop1"
ln -s hop1 "$work/hop2"
status=0
"$RELIC" -l tuples=2 -o "$work/dangling" -O "tsv:edge=$work/new.tsv" \
    -O "tsv:path=$work/keep.tsv" -O "tsv:path=$work/hop2" \
    "$work/graph.dl" 2>/dev/null || status=$?
if [ "$status" -ne 1 ]; then
	fail "a run over the tuple limit exited $status, expected 1"
fi
for created in new.txt new.tsv made.txt made-through-hops.tsv; do
	if [ -e "$work/$created" ]; then
		fail "a failed run left the $created it created"
	fi
done
if [ "$(cat "$work/keep.tsv")" != "precious" ]; then
	fail "a failed run replaced an export's existing file"
fi

# -n writes no rows, so its -o file is neither opened nor created.
"$RELIC" -n -o "$work/never.txt" "$work/graph.dl" ||
    fail "-n with an -o file failed"
if [ -e "$work/never.txt" ]; then
	fail "-n created the -o file"
fi

# A path naming a descriptor that the system opens as the descriptor itself,
# sharing its offset and its O_APPEND, as /dev/stdout does on macOS and the
# BSDs, is written from where the descriptor stands: neither what was
# written before the run nor a file appended to is cut.
case "$(uname -s)" in
Darwin | *BSD | DragonFly)
	printf 'header\nb\nc\n' > "$work/after-header.expected"
	{
		echo header
		"$RELIC" -o /dev/stdout "$work/graph.dl"
	} > "$work/after-header.txt"
	if ! cmp -s "$work/after-header.expected" "$work/after-header.txt"; then
		fail "-o /dev/stdout after a header wrote" \
		    "'$(od -c "$work/after-header.txt")'"
	fi
	echo header > "$work/appended.txt"
	"$RELIC" -o /dev/stdout "$work/graph.dl" >> "$work/appended.txt"
	if ! cmp -s "$work/after-header.expected" "$work/appended.txt"; then
		fail "-o /dev/stdout appended '$(od -c "$work/appended.txt")'"
	fi
	;;
esac

# A FIFO that no one reads yet holds a run up only once rows are written to
# it: a run that fails before then ends without waiting for a reader, and a
# reader that comes later reads the results.
mkfifo "$work/fifo"
status=0
"$RELIC" -o "$work/fifo" -q 'X > 1?' "$work/graph.dl" 2>/dev/null ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "an invalid query writing a FIFO exited $status, expected 1"
fi
cat "$work/fifo" > "$work/fifo.txt" &
reader=$!
status=0
"$RELIC" -o "$work/fifo" "$work/graph.dl" || status=$?
if [ "$status" -ne 0 ]; then
	kill "$reader" 2>/dev/null || :
	fail "a run writing a FIFO exited $status, expected 0"
fi
wait "$reader"
if [ "$(cat "$work/fifo.txt")" != "$expected" ]; then
	fail "a FIFO read '$(cat "$work/fifo.txt")', expected '$expected'"
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

# The planner's references, answering without the Magic rewrite or without
# its factoring, gathering every answer before writing it, deferring every
# round's insertions, compiling every aggregate body to a group rule and
# importing a table a field at a time are selected by name and answer as the
# default does.
for reference in no-index no-reorder no-exists no-magic no-factor no-stream \
    parallel group-rules no-batch-import; do
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

# The input limit bounds a program file, refused whole and naming the file,
# and a field of an imported table, refused where the field begins.  A file
# exactly at the limit loads.  A query's text is held to no input limit,
# whether -q gives it or the tool writes it for -O: the query an export of
# long.dl prepares, long_name_of_a_relation(V1, V2)?, is a byte longer than
# the file.
size=$(wc -c < "$work/graph.dl" | tr -d ' ')
status=0
diagnostic=$(cd "$work" && "$RELIC" -l input=$((size - 1)) graph.dl 2>&1 \
    >/dev/null) || status=$?
if [ "$status" -ne 1 ]; then
	fail "an exceeded input limit exited $status, expected 1"
fi
case "$diagnostic" in
*"limit exceeded: input=$((size - 1)) while loading graph.dl")
	;;
*)
	fail "an exceeded input limit was reported as '$diagnostic'"
	;;
esac
actual=$("$RELIC" -s -l input="$size" "$work/graph.dl")
if [ "$actual" != "$expected" ]; then
	fail "a program at the input limit answered '$actual'"
fi
printf 'long_name_of_a_relation(a, b).\n' > "$work/long.dl"
size=$(wc -c < "$work/long.dl" | tr -d ' ')
actual=$("$RELIC" -s -l input="$size" \
    -O "tsv:long_name_of_a_relation=$work/long.out" \
    -q 'long_name_of_a_relation(X, Y), long_name_of_a_relation(X, Z)?' \
    "$work/long.dl")
if [ "$actual" != "a b b" ] ||
    [ "$(cat "$work/long.out")" != "$(printf 'a\tb')" ]; then
	fail "a query or an export longer than the input limit answered" \
	    "'$actual' and wrote '$(cat "$work/long.out")'"
fi

printf 'a\tbcde\n' > "$work/long.tsv"
status=0
diagnostic=$(cd "$work" && "$RELIC" -l input=3 \
    -I tsv:edge:symbol,symbol=long.tsv -q 'edge(X, Y)?' 2>&1 >/dev/null) ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "a field past the input limit exited $status, expected 1"
fi
case "$diagnostic" in
*"long.tsv:1:3: error: limit exceeded: input=3 while importing long.tsv")
	;;
*)
	fail "a field past the input limit was reported as '$diagnostic'"
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
    -l input=1 -l input=0 -l magic=1 -l magic=0 "$work/graph.dl")
if [ "$actual" != "$expected" ]; then
	fail "limits of zero answered '$actual', expected '$expected'"
fi

# A -l that is not a limit is a usage error naming what was wrong with it:
# no `=`, a key that names no limit, and a value that is empty, not a
# number, signed, surrounded by space, followed by other bytes, or too large
# for a size.
for argument in tuples tuples5 bogus=1 =1 tuples= tuples=many \
    input= input=x max_input=1 tuples=-1 tuples=+1 tuples=-0 'tuples= 5' \
    tuples=5x 'tuples=5 ' tuples=99999999999999999999999; do
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

# A path alone names its predicate by its file name, not by the directories
# above it, and its format by its extension, csv as well as tsv.
mkdir "$work/tables"
printf 'c\td\n' > "$work/tables/edge.tsv"
printf 'c,e\n' > "$work/tables/edge.csv"
actual=$("$RELIC" -s -T edge:symbol,symbol -I "$work/tables/edge.tsv" \
    -I "$work/tables/edge.csv" < "$work/graph.dl")
if [ "$actual" != "$(printf 'b\nc\nd\ne')" ]; then
	fail "imports of paths alone answered '$actual'"
fi

# -O writes a whole relation to its file, replacing whatever it held, after
# the queries, whose results still go to standard output; -n, which writes
# no query's rows, does not stop it.
printf 'a\tb\na\tc\nb\tc\n' > "$work/path.expected"
for discard in "" -n; do
	echo stale > "$work/path.tsv"
	# shellcheck disable=SC2086 # an empty option is no option
	actual=$("$RELIC" -s $discard -O "$work/path.tsv" "$work/graph.dl")
	if [ "$actual" != "$(if [ -z "$discard" ]; then echo "$expected"; fi)" ]
	then
		fail "-O $discard wrote '$actual' to standard output"
	fi
	if ! cmp -s "$work/path.expected" "$work/path.tsv"; then
		fail "-O $discard wrote '$(cat "$work/path.tsv")'"
	fi
done

# An export's file is opened only when its turn comes, after the queries:
# a run that fails validation, a run under -c, and a run whose query fails
# in evaluation leave it as it was.
for run in "-q X>1?" "-c" "-l tuples=7 -q path(X,Y)?"; do
	echo kept > "$work/path.tsv"
	# shellcheck disable=SC2086 # the run's options are separate words
	"$RELIC" $run -O "$work/path.tsv" "$work/graph.dl" >/dev/null 2>&1 ||
	    :
	if [ "$(cat "$work/path.tsv")" != "kept" ]; then
		fail "a run with '$run' replaced an export's file"
	fi
done

# An export never writes a path the command line spells otherwise: a
# program operand or an imported table would be replaced by one of its
# relations.  Each is a usage error, and the file is left as it was.
printf 'a\tb\n' > "$work/edge.tsv"
cp "$work/graph.dl" "$work/path.dl"
for run in "-O dl:path=$work/path.dl $work/path.dl" \
    "-T edge:symbol,symbol -I $work/edge.tsv -O $work/edge.tsv $work/path.dl"
do
	cp "$work/edge.tsv" "$work/edge.kept"
	cp "$work/path.dl" "$work/path.kept"
	status=0
	# shellcheck disable=SC2086 # the run's options are separate words
	"$RELIC" $run >/dev/null 2>&1 || status=$?
	if [ "$status" -ne 2 ]; then
		fail "an export of a file the run reads exited $status, expected 2: $run"
	fi
	if ! cmp -s "$work/edge.tsv" "$work/edge.kept" ||
	    ! cmp -s "$work/path.dl" "$work/path.kept"; then
		fail "an export replaced a file the run reads: $run"
	fi
done

# Exports are written in command-line order, after the materialized program
# under -a: a head's arity chooses among a name's predicates, and a
# predicate of no columns without its fact is an empty file.
cat > "$work/arities.dl" <<'EOF2'
edge(a, b).
edge(x, y, z).
never :- edge(z, z).
EOF2
printf 'x\ty\tz\n' > "$work/edge3.expected"
"$RELIC" -a -O "tsv:edge/3=$work/edge3.tsv" -O "$work/never.tsv" \
    "$work/arities.dl" || fail "exporting edge/3 and never failed"
if ! cmp -s "$work/edge3.expected" "$work/edge3.tsv"; then
	fail "edge/3 was exported as '$(cat "$work/edge3.tsv")'"
fi
if [ ! -f "$work/never.tsv" ] || [ -s "$work/never.tsv" ]; then
	fail "a predicate of no columns and no fact was not an empty file"
fi

# An export that cannot be opened is an error, reported once naming the
# file, before any export is written: the run leaves no file behind, the
# ones before it on the command line included.
rm -f "$work/edge3.tsv" "$work/never.tsv"
status=0
diagnostic=$("$RELIC" -a -O "tsv:edge/3=$work/edge3.tsv" \
    -O "$work/never.tsv" -O "tsv:edge/2=$work/missing/edge2.tsv" \
    -O "tsv:edge/2=$work/edge2.tsv" "$work/arities.dl" 2>&1 >/dev/null) ||
    status=$?
if [ "$status" -ne 1 ]; then
	fail "an unwritable export exited $status, expected 1"
fi
if [ "$(printf '%s\n' "$diagnostic" |
    grep -c "cannot open $work/missing/edge2.tsv")" -ne 1 ]; then
	fail "an unwritable export was reported as '$diagnostic'"
fi
for export in edge3.tsv never.tsv edge2.tsv; do
	if [ -e "$work/$export" ]; then
		fail "a run with an unwritable export left $export behind"
	fi
done

# An export whose evaluation fails -- here at a limit the queries before it
# stay within -- fails the run and leaves its file as it was: the file is
# truncated only when the first row reaches it, and the library delivers
# none until the evaluation has succeeded.
echo stale > "$work/path.tsv"
status=0
"$RELIC" -l tuples=4 -q 'edge(X, Y)?' -O "$work/path.tsv" \
    "$work/graph.dl" >/dev/null 2>&1 || status=$?
if [ "$status" -ne 1 ]; then
	fail "an export failing in evaluation exited $status, expected 1"
fi
if [ "$(cat "$work/path.tsv")" != "stale" ]; then
	fail "an export failing in evaluation replaced its file"
fi

# A write to an export's file that fails is reported once, naming the file,
# and fails the run.
status=0
diagnostic=$(
	trap '' XFSZ
	ulimit -f 0
	exec "$RELIC" -O "$work/path.tsv" "$work/graph.dl" 2>&1 >/dev/null
) || status=$?
if [ "$status" -ne 1 ]; then
	fail "a failed write to an export exited $status, expected 1"
fi
if [ "$(printf '%s\n' "$diagnostic" |
    grep -c "cannot write $work/path.tsv")" -ne 1 ]; then
	fail "a failed write to an export was reported as '$diagnostic'"
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

# An unknown option is an error like any other the tool reports.
diagnostic=$("$RELIC" -Z "$work/graph.dl" 2>&1 >/dev/null) || true
case "$diagnostic" in
*": error: unknown option -- Z"*)
	;;
*)
	fail "an unknown option was reported as '$diagnostic'"
	;;
esac

# -V with anything else says why the command line is refused.
status=0
diagnostic=$("$RELIC" -V "$work/graph.dl" 2>&1 >/dev/null) || status=$?
if [ "$status" -ne 2 ]; then
	fail "-V with an operand exited $status, expected 2"
fi
case "$diagnostic" in
*": error: -V takes no other option or operand"*)
	;;
*)
	fail "-V with an operand was reported as '$diagnostic'"
	;;
esac

# A version that cannot be written is a failure.  Standard output is open
# for reading only, as for the failed write above.
status=0
diagnostic=$("$RELIC" -V 2>&1 1</dev/null) || status=$?
if [ "$status" -ne 1 ]; then
	fail "-V with standard output unwritable exited $status, expected 1"
fi
case "$diagnostic" in
*"cannot write standard output"*)
	;;
*)
	fail "a failed -V write was reported as '$diagnostic'"
	;;
esac

exit 0
