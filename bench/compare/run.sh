#!/bin/sh
#
# Compare relic with other Datalog and tabled-Prolog engines and with SQL
# databases.
#
# usage: bench/compare/run.sh [-e engines] [-o results.tsv] [-w workloads]
#
# A workload is a directory beside this script holding one program per
# engine and a file naming the data set, which bench/datasets.sh generates.
# Only the SQL engines may lack a program, for a workload SQL cannot
# express; they are reported n/a there.
# relic answers each workload first, and its answer becomes the reference;
# every other engine runs the workload once under a time limit, and its
# answer is checked against the reference.  An engine that agrees is then
# timed by hyperfine.  For every workload and engine the report gives the
# median and fastest wall-clock seconds, their standard deviation, the mean
# user seconds, the peak resident memory, the rows of the answer and the
# status, and it ends with every engine's median as a multiple of relic's.
#
# -e and -w take comma-separated names and restrict the run; relic always
# runs.  -o also writes the results as a tab-separated file.  RELIC, SWIPL,
# SOUFFLE, CLINGO, NMO, DUCKDB and SQLITE name the engines' commands, and
# CXX the compiler, one command without arguments, for Souffle's compiled
# programs.
# BENCH_SCALE, 1 by default, multiplies every data set as it does for
# bench/run.sh; BENCH_RUNS, 5, is the number of timed runs; and
# BENCH_TIMEOUT, 300, the seconds each engine is given to answer.
#
# Nothing fails on a time, and an engine that runs out of time is reported,
# not failed.  The exit status is 1 if an engine failed or answered
# differently from relic, or relic's answer was empty, and 2 on a usage
# error.

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/../datasets.sh"

relic=${RELIC:-relic}
swipl=${SWIPL:-swipl}
souffle=${SOUFFLE:-souffle}
clingo=${CLINGO:-clingo}
nmo=${NMO:-nmo}
duckdb=${DUCKDB:-duckdb}
sqlite=${SQLITE:-sqlite3}
cxx=${CXX:-c++}
scale=${BENCH_SCALE:-1}
runs=${BENCH_RUNS:-5}
limit=${BENCH_TIMEOUT:-300}

# The switches each engine runs with, recorded in the report's header.
# SWI-Prolog reads no initialization file, which could change its flags,
# and its default stack and table space are too small for the larger
# closures.  Souffle and DuckDB are held to one thread, and DuckDB may not
# download an extension during a run.  DuckDB and SQLite read no
# initialization file, stop at the first error, and print rows without a
# header, their columns separated by tabs.  The compiler's
# switches depend on the Souffle installed and are set once it is found.
relic_switches="-F tsv"
swipl_switches="-f none --stack-limit=16g --table-space=16g"
souffle_switches="-j1"
magic_switches="--magic-transform=*"
clingo_switches="-V0"
nemo_switches="--report none -o"
duckdb_switches="-batch -bail -no-init -noheader -cmd '.mode tabs'"
duckdb_switches="$duckdb_switches -cmd 'SET threads = 1'"
duckdb_switches="$duckdb_switches -cmd"
duckdb_switches="$duckdb_switches 'SET autoinstall_known_extensions = false'"
sqlite_switches="-batch -bail -init /dev/null -noheader -tabs"
cxx_switches=

# The engines, in the report's order; relic, the reference, is first.
all_engines="relic swipl souffle-interp souffle souffle-magic clingo nemo"
all_engines="$all_engines duckdb sqlite"

# The columns of a result, in the order result_row writes them.
columns="workload engine status rows median fastest spread user"
columns="$columns peak_bytes compile_seconds"

# An awk prelude giving each result column's number, col["status"] and so on,
# from the names passed in `columns`.
column_numbers='BEGIN {
	count = split(columns, column_name, " ")
	for (i = 1; i <= count; i++)
		col[column_name[i]] = i
}'

usage() {
	echo "usage: run.sh [-e engines] [-o results.tsv] [-w workloads]" >&2
	exit 2
}

die() {
	echo "compare: $*" >&2
	exit 2
}

# member WORD WORDS...: whether WORD is one of WORDS.
member() (
	word=$1
	shift
	for candidate in "$@"; do
		if [ "$candidate" = "$word" ]; then
			exit 0
		fi
	done
	exit 1
)

# require_positive NAME VALUE: exit unless VALUE is a positive integer.
require_positive() {
	case $2 in
	'' | *[!0-9]* | 0*)
		die "$1 must be a positive integer"
		;;
	esac
}

# quote WORD: WORD in single quotes, as both sh and hyperfine read it.
quote() {
	printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

# dot_argument TEXT: TEXT as one argument of a dot-command of the DuckDB or
# SQLite command-line shell, in double quotes, within which that shell
# resolves backslash escapes.
dot_argument() {
	printf '"%s"' "$(printf '%s' "$1" | sed 's/[\\"]/\\&/g')"
}

# words LIST: the comma-separated LIST as words.
words() {
	printf '%s\n' "$1" | tr ',' ' '
}

# home_relative PATH: PATH with the home directory written as ~, so that a
# recorded report names no user.
home_relative() {
	home=${HOME:-}
	if [ -n "$home" ]; then
		case $1 in
		"$home"/*)
			printf '~%s\n' "${1#"$home"}"
			return 0
			;;
		esac
	fi
	printf '%s\n' "$1"
}

# workload_names: every directory beside this script that holds a workload,
# in the order of their names.
workload_names() {
	for dataset_file in "$here"/*/dataset; do
		basename "$(dirname "$dataset_file")"
	done
}

# dataset_of WORKLOAD: the data set WORKLOAD queries, as its directory names.
# The name becomes a directory under $work, so it may not leave it.
dataset_of() {
	read -r dataset_name < "$here/$1/dataset"
	case $dataset_name in
	'' | */* | .*) die "$1 names no valid data set" ;;
	esac
	printf '%s\n' "$dataset_name"
}

# engine_tool ENGINE: the command ENGINE's programs are run with.
engine_tool() {
	case $1 in
	relic) printf '%s\n' "$relic" ;;
	swipl) printf '%s\n' "$swipl" ;;
	souffle-interp | souffle | souffle-magic) printf '%s\n' "$souffle" ;;
	clingo) printf '%s\n' "$clingo" ;;
	nemo) printf '%s\n' "$nmo" ;;
	duckdb) printf '%s\n' "$duckdb" ;;
	sqlite) printf '%s\n' "$sqlite" ;;
	*) die "internal error: no command for engine $1" ;;
	esac
}

# installed ENGINE: whether what ENGINE needs is installed.  Souffle's
# compiled modes also need the C++ compiler.
installed() {
	command -v "$(engine_tool "$1")" >/dev/null 2>&1 || return 1
	case $1 in
	souffle | souffle-magic)
		command -v "$cxx" >/dev/null 2>&1
		;;
	esac
}

# engine_version ENGINE: the version ENGINE reports, on one line.
engine_version() {
	tool=$(engine_tool "$1")
	case $1 in
	relic) "$tool" -V ;;
	swipl) "$tool" --version ;;
	souffle-interp | souffle | souffle-magic)
		"$tool" --version | awk '/Version:/ { print "souffle", $2 }'
		;;
	clingo) "$tool" --version | head -n 1 ;;
	nemo) "$tool" --version ;;
	duckdb) "$tool" --version | awk '{ print "duckdb", $1; exit }' ;;
	sqlite) "$tool" --version | awk '{ print "sqlite", $1; exit }' ;;
	*) die "internal error: no version for engine $1" ;;
	esac
}

# engine_switches ENGINE: the switches ENGINE runs with, and for Souffle's
# compiled modes those it is compiled with.
engine_switches() {
	case $1 in
	relic) echo "$relic_switches" ;;
	swipl) echo "$swipl_switches" ;;
	souffle-interp) echo "$souffle_switches" ;;
	souffle) echo "$souffle_switches; compiled with $cxx_switches" ;;
	souffle-magic)
		echo "$souffle_switches $magic_switches; compiled with" \
		    "$cxx_switches"
		;;
	clingo) echo "$clingo_switches" ;;
	nemo) echo "$nemo_switches" ;;
	duckdb) echo "$duckdb_switches" ;;
	sqlite) echo "$sqlite_switches" ;;
	*) die "internal error: no switches for engine $1" ;;
	esac
}

# souffle_domain_bits: the width of Souffle's numbers, which its compiled
# programs must be built for.
souffle_domain_bits() {
	"$souffle" --version | awk '/Word size:/ { print $3 }'
}

# machine: the processor's name.
machine() {
	processor=$(sysctl -n machdep.cpu.brand_string 2>/dev/null ||
	    sysctl -n hw.model 2>/dev/null ||
	    awk -F ': ' '/^model name/ { print $2; exit }' /proc/cpuinfo \
	    2>/dev/null || true)
	printf '%s\n' "${processor:-$(uname -m)}"
}

# prepare_dataset NAME: generate the data set NAME into $work/data/NAME,
# once, and its facts as source text into $work/data/NAME.pl.
prepare_dataset() {
	dir=$work/data/$1
	if [ -d "$dir" ]; then
		return 0
	fi

	mkdir -p "$dir"
	dataset "$1" "$dir" "$scale"
	source_facts "$dir" > "$dir.pl"
}

# program_file ENGINE: the name of ENGINE's program in a workload's
# directory.
program_file() {
	case $1 in
	relic) echo relic.dl ;;
	swipl) echo swipl.pl ;;
	souffle-interp | souffle | souffle-magic) echo souffle.dl ;;
	clingo) echo clingo.lp ;;
	nemo) echo nemo.rls ;;
	duckdb) echo duckdb.sql ;;
	sqlite) echo sqlite.sql ;;
	*) die "internal error: no program for engine $1" ;;
	esac
}

# optional_program ENGINE: whether a workload may have no program for
# ENGINE.  Every workload is written for every Datalog engine; DuckDB and
# SQLite cannot express nonlinear recursion or recursion through two
# relations.
optional_program() {
	case $1 in
	duckdb | sqlite) return 0 ;;
	*) return 1 ;;
	esac
}

# relic_imports DIR: an -I option importing each relation file of DIR into
# its relation, each column typed as source_facts would spell its values: a
# column of decimal digits, signed or not, holds integers and any other
# symbols.  The data sets write every column one way, so the first row
# decides.  An empty relation file is not imported, as source_facts writes
# no facts for one.
relic_imports() (
	for file in "$1"/*.tsv; do
		[ -s "$file" ] || continue
		columns=$(awk -F '\t' 'NR == 1 {
			for (i = 1; i <= NF; i++)
				printf "%s%s", (i > 1 ? "," : ""),
				    ($i ~ /^-?[0-9]+$/ ? "integer" : "symbol")
			exit
		}' "$file")
		printf ' -I %s' "$(quote \
		    "tsv:$(basename "$file" .tsv):$columns=$file")"
	done
)

# engine_command ENGINE WORKLOAD: the command that runs WORKLOAD's program
# with ENGINE.  relic, SWI-Prolog, clingo, DuckDB and SQLite write the
# answer to standard output; Souffle and Nemo write it into
# $work/out/ENGINE.  The SQL engines change to the data set's directory,
# where the program names its relation files and DuckDB writes any data it
# spills to disk.
engine_command() (
	program=$here/$2/$(program_file "$1")
	data=$work/data/$(dataset_of "$2")
	out=$work/out/$1

	case $1 in
	relic)
		echo "$(quote "$relic") $relic_switches" \
		    "$(relic_imports "$data") $(quote "$program")"
		;;
	swipl)
		echo "$(quote "$swipl") $swipl_switches" \
		    "$(quote "$here/driver.pl") $(quote "$program")" \
		    "$(quote "$data.pl")"
		;;
	souffle-interp)
		echo "$(quote "$souffle") $souffle_switches" \
		    "-F $(quote "$data") -D $(quote "$out")" \
		    "$(quote "$program")"
		;;
	souffle | souffle-magic)
		echo "$(quote "$work/bin/$1") $souffle_switches" \
		    "-F $(quote "$data") -D $(quote "$out")"
		;;
	clingo)
		echo "$(quote "$clingo") $clingo_switches" \
		    "$(quote "$data.pl") $(quote "$program")"
		;;
	nemo)
		echo "$(quote "$nmo") $nemo_switches" \
		    "-I $(quote "$data") -D $(quote "$out")" \
		    "$(quote "$program")"
		;;
	duckdb)
		echo "$(quote "$duckdb") $duckdb_switches" \
		    "-cmd $(quote ".cd $(dot_argument "$data")")" \
		    "-f $(quote "$program")"
		;;
	sqlite)
		echo "$(quote "$sqlite") $sqlite_switches" \
		    "-cmd $(quote ".cd $(dot_argument "$data")") :memory:" \
		    "$(quote ".read $(dot_argument "$program")")"
		;;
	*)
		die "internal error: no command line for engine $1"
		;;
	esac
)

# answer_file ENGINE: the file ENGINE's verification run leaves its answer
# in.
answer_file() {
	case $1 in
	relic | swipl | clingo | duckdb | sqlite)
		printf '%s\n' "$work/stdout"
		;;
	souffle-interp | souffle | souffle-magic)
		printf '%s\n' "$work/out/$1/answer.csv"
		;;
	nemo) printf '%s\n' "$work/out/$1/answer.tsv" ;;
	*) die "internal error: no answer file for engine $1" ;;
	esac
}

# succeeded ENGINE STATUS: whether exit status STATUS is ENGINE's success.
# clingo reports what it found in its status: 10 for a model, 30 for a model
# found by an exhaustive search.
succeeded() {
	case $1 in
	clingo) [ "$2" -eq 10 ] || [ "$2" -eq 30 ] ;;
	*) [ "$2" -eq 0 ] ;;
	esac
}

# compile_souffle ENGINE WORKLOAD: compile WORKLOAD's Souffle program for
# ENGINE into $work/bin/ENGINE and print the seconds both steps took:
# Souffle translates the program to C++, and the compiler builds it with the
# switches recorded in the header.  Fails, with the errors shown, if either
# step does.
compile_souffle() (
	source=$work/bin/$1.cpp
	binary=$work/bin/$1
	program=$here/$2/$(program_file "$1")
	mkdir -p "$work/bin"

	translate="$(quote "$souffle")"
	if [ "$1" = souffle-magic ]; then
		translate="$translate $(quote "$magic_switches")"
	fi
	translate="$translate -g $(quote "$source") $(quote "$program")"
	build="$(quote "$cxx") $cxx_switches -I $(quote "$souffle_include")"
	build="$build -o $(quote "$binary") $(quote "$source")"

	if ! /usr/bin/time -p -o "$work/translate.time" sh -c "$translate" \
	    > /dev/null 2> "$work/compile.err"; then
		echo "compare: Souffle could not translate $2:" >&2
		head -n 20 "$work/compile.err" >&2
		exit 1
	fi
	if ! /usr/bin/time -p -o "$work/build.time" sh -c "$build" \
	    > /dev/null 2> "$work/compile.err"; then
		echo "compare: $cxx could not compile $2 for $1:" >&2
		head -n 20 "$work/compile.err" >&2
		exit 1
	fi

	awk '/^real / { total += $2 } END { printf "%.2f\n", total }' \
	    "$work/translate.time" "$work/build.time"
)

# compile ENGINE WORKLOAD: build WORKLOAD's program if ENGINE runs a
# compiled one, setting compile_seconds, or status on a failure.
compile() {
	case $1 in
	souffle | souffle-magic)
		if ! compile_seconds=$(compile_souffle "$1" "$2"); then
			compile_seconds=-
			status=failed
		fi
		;;
	esac
}

# normalize_answer ENGINE: the answer ENGINE's verification run left, as
# sorted, distinct rows of tab-separated columns.  clingo prints the atoms
# it shows, name(a,b), on one line.
normalize_answer() {
	file=$(answer_file "$1")
	if [ ! -f "$file" ]; then
		echo "compare: $1 left no answer in $file" >&2
		return 1
	fi

	case $1 in
	clingo)
		tr ' ' '\n' < "$file" | awk '/^[a-z_][A-Za-z0-9_]*\(.*\)$/ {
			sub(/^[a-z_][A-Za-z0-9_]*\(/, "")
			sub(/\)$/, "")
			gsub(/,/, "\t")
			print
		}'
		;;
	*)
		cat "$file"
		;;
	esac | LC_ALL=C sort -u
}

# verify ENGINE WORKLOAD: run WORKLOAD once with ENGINE under the time limit,
# leaving its normalized answer in $work/answer and setting status and rows.
# timeout(1) reports an engine it stopped with 124, or with 137 if the
# engine ignored the termination signal and had to be killed.  An engine the
# system killed, as it kills one that exhausts memory, also ends with 137,
# so 137 is a timeout only once the limit has passed.
verify() {
	rm -rf "$work/out/$1" "$work/stdout"
	mkdir -p "$work/out/$1"

	code=0
	started=$(date +%s)
	"$timeout" -k 10 "$limit" sh -c "$(engine_command "$1" "$2")" \
	    < /dev/null > "$work/stdout" 2> "$work/stderr" || code=$?
	elapsed=$(($(date +%s) - started))
	if [ "$code" -eq 124 ] ||
	    { [ "$code" -eq 137 ] && [ "$elapsed" -ge "$limit" ]; }; then
		status=timeout
		return 0
	fi
	if [ "$code" -eq 137 ]; then
		status=failed
		echo "compare: $1 was killed on $2 after ${elapsed}s," \
		    "perhaps for exhausting memory" >&2
		return 0
	fi
	if ! succeeded "$1" "$code"; then
		status=failed
		echo "compare: $1 failed on $2 with status $code:" >&2
		head -n 20 "$work/stderr" >&2
		return 0
	fi

	if ! normalize_answer "$1" > "$work/answer"; then
		status=failed
		return 0
	fi
	rows=$(wc -l < "$work/answer" | tr -d ' ')
}

# judge_answer ENGINE WORKLOAD: relic's answer becomes the workload's
# reference, unless it is empty: engines agreeing on no rows would check
# nothing.  Any other engine's answer must equal the reference.
judge_answer() {
	if [ "$1" = relic ]; then
		if [ "$rows" -eq 0 ]; then
			status=empty
			echo "compare: relic's answer to $2 is empty" >&2
		else
			mv "$work/answer" "$work/reference"
		fi
	elif ! cmp -s "$work/reference" "$work/answer"; then
		status=mismatch
		echo "compare: $1 disagrees with relic on $2:" >&2
		diff "$work/reference" "$work/answer" | head -n 10 >&2
	fi
}

# parse_timing FILE: the median, fastest, standard deviation, mean user
# seconds and peak bytes of hyperfine's JSON export FILE, on one line, or a
# failure if one is missing.  One run has no deviation, which hyperfine
# writes as null and this writes as -.  hyperfine writes one key per line
# and one array element per line; the peak is the largest of the runs'
# peaks.
parse_timing() {
	awk '
		/"median":/ { median = $2 }
		/"min":/ { fastest = $2 }
		/"stddev":/ { spread = $2 }
		/"user":/ { user = $2 }
		/"memory_usage_byte":/ { in_memory = 1; next }
		in_memory && /]/ { in_memory = 0 }
		in_memory {
			sub(/,/, "", $1)
			if (peak == "" || $1 + 0 > peak + 0)
				peak = $1
		}
		END {
			if (median == "" || fastest == "" || spread == "" ||
			    user == "" || peak == "")
				exit 1
			gsub(/,/, "", median)
			gsub(/,/, "", fastest)
			gsub(/,/, "", spread)
			if (spread == "null")
				spread = "-"
			gsub(/,/, "", user)
			print median, fastest, spread, user, peak
		}' "$1"
}

# measure ENGINE WORKLOAD: time BENCH_RUNS runs with hyperfine, setting
# median, fastest, spread, user and peak_bytes.  clingo's successful statuses
# are not zero, so hyperfine is told to ignore statuses for it; its
# verification run has already checked what they mean.
measure() {
	ignore=
	if [ "$1" = clingo ]; then
		ignore=-i
	fi

	if ! hyperfine -N $ignore --style none --runs "$runs" \
	    --export-json "$work/timing.json" "$(engine_command "$1" "$2")" \
	    > /dev/null 2> "$work/hyperfine.err"; then
		status=failed
		echo "compare: timing $1 on $2 failed:" >&2
		head -n 20 "$work/hyperfine.err" >&2
		return 0
	fi
	if ! timing=$(parse_timing "$work/timing.json"); then
		status=failed
		echo "compare: hyperfine's report on $1 lacks a measure" >&2
		return 0
	fi

	read -r median fastest spread user peak_bytes <<EOF
$timing
EOF
}

# result_row: the result being recorded, tab-separated, one field for each
# name in `columns` and in that order.
result_row() {
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
	    "$workload" "$engine" "$status" "$rows" "$median" "$fastest" \
	    "$spread" "$user" "$peak_bytes" "$compile_seconds"
}

# print_result ROW: ROW as a line of the report.
print_result() {
	printf '%s\n' "$1" | awk -F '\t' -v columns="$columns" \
	    -v width="$name_width" "$column_numbers"'{
		first_column = "%-" width "s "
		spread = $col["spread"]
		if (spread != "-")
			spread = sprintf("%.3f", spread)
		if ($col["status"] == "ok")
			printf first_column "%-14s %8.3f %8.3f %7s %8.3f %8.1f", \
			    $col["workload"], $col["engine"], $col["median"], \
			    $col["fastest"], spread, $col["user"], \
			    $col["peak_bytes"] / 1048576
		else
			printf first_column "%-14s %8s %8s %7s %8s %8s", \
			    $col["workload"], $col["engine"], "-", "-", "-", \
			    "-", "-"
		printf " %8s %7s  %s\n", $col["rows"], \
		    $col["compile_seconds"], $col["status"]
	}'
}

# run_engine: compile if $engine needs it, answer $workload, judge the
# answer, time the engine if it agrees, and record the result.  A workload
# with no program for the engine is n/a if the engine's programs are
# optional, and a failure otherwise.
run_engine() {
	status=ok
	rows=-
	median=-
	fastest=-
	spread=-
	user=-
	peak_bytes=-
	compile_seconds=-

	if [ ! -f "$here/$workload/$(program_file "$engine")" ]; then
		if optional_program "$engine"; then
			status=n/a
		else
			status=failed
			echo "compare: $workload has no program for $engine" >&2
		fi
	fi
	if [ "$status" = ok ]; then
		compile "$engine" "$workload"
	fi
	if [ "$status" = ok ]; then
		verify "$engine" "$workload"
	fi
	if [ "$status" = ok ]; then
		judge_answer "$engine" "$workload"
	fi
	if [ "$status" = ok ]; then
		measure "$engine" "$workload"
	fi

	row=$(result_row)
	printf '%s\n' "$row" >> "$work/results.tsv"
	print_result "$row"
}

# summarize: every engine's median as a multiple of relic's, one row per
# workload, or the engine's status where it has no time.
summarize() {
	awk -F '\t' -v columns="$columns" -v engines="$engines" \
	    -v width="$name_width" "$column_numbers"'
	BEGIN {
		first_column = "%-" width "s"
		engine_count = split(engines, engine, " ")
		printf "\n" first_column, "relative"
		for (i = 1; i <= engine_count; i++)
			printf " %14s", engine[i]
		printf "\n"
	}
	!($col["workload"] in seen) {
		seen[$col["workload"]] = 1
		order[++workloads] = $col["workload"]
	}
	{
		key = $col["workload"] SUBSEP $col["engine"]
		status[key] = $col["status"]
		median[key] = $col["median"]
	}
	END {
		for (w = 1; w <= workloads; w++) {
			workload = order[w]
			printf first_column, workload
			base = median[workload, "relic"]
			for (i = 1; i <= engine_count; i++) {
				key = workload SUBSEP engine[i]
				if (!(key in status))
					cell = "-"
				else if (status[key] != "ok")
					cell = status[key]
				else if (base > 0)
					cell = sprintf("%.2f",
					    median[key] / base)
				else
					cell = "-"
				printf " %14s", cell
			}
			printf "\n"
		}
	}' "$work/results.tsv"
}

# print_header: what a recorded run needs to be reproduced.
print_header() {
	echo "# machine: $(machine), $(uname -sr)"
	echo "# scale $scale, $runs timed runs, ${limit}s to answer"
	tools=
	for engine in $engines; do
		tool=$(engine_tool "$engine")
		if member "$tool" $tools; then
			continue
		fi
		tools="$tools $tool"
		echo "# $(engine_version "$engine"):" \
		    "$(home_relative "$(command -v "$tool")")"
	done
	if [ -n "$cxx_switches" ]; then
		echo "# $("$cxx" --version | head -n 1):" \
		    "$(home_relative "$(command -v "$cxx")")"
	fi
	for engine in $engines; do
		echo "# $engine $(engine_switches "$engine")"
	done
	echo
	printf "%-${name_width}s %-14s %8s %8s %7s %8s %8s %8s %7s  %s\n" \
	    workload engine median fastest spread user "peak MB" rows compile \
	    status
}

requested_engines=$all_engines
workloads=$(workload_names)
results=
while getopts e:o:w: option; do
	case $option in
	e) requested_engines=$(words "$OPTARG") ;;
	o) results=$OPTARG ;;
	w) workloads=$(words "$OPTARG") ;;
	*) usage ;;
	esac
done
shift $((OPTIND - 1))
if [ $# -ne 0 ]; then
	usage
fi

require_positive BENCH_SCALE "$scale"
require_positive BENCH_RUNS "$runs"
require_positive BENCH_TIMEOUT "$limit"
for engine in $requested_engines; do
	member "$engine" $all_engines || die "unknown engine $engine"
done
for workload in $workloads; do
	case $workload in
	*/* | .*) die "unknown workload $workload" ;;
	esac
	[ -f "$here/$workload/dataset" ] || die "unknown workload $workload"
done

# The report's first column is as wide as the longest workload name, or its
# own headings, "workload" and "relative".
name_width=$(printf '%s\n' workload relative $workloads |
    awk '{ if (length($0) > width) width = length($0) } END { print width }')

command -v hyperfine >/dev/null 2>&1 ||
    die "hyperfine is needed to time the engines"
if command -v timeout >/dev/null 2>&1; then
	timeout=timeout
elif command -v gtimeout >/dev/null 2>&1; then
	timeout=gtimeout
else
	die "timeout(1) is needed to limit the engines"
fi
[ -x /usr/bin/time ] || die "/usr/bin/time is needed to time compilation"
installed relic || die "cannot run $relic; name the relic to compare"
case $(head -c 2 "$(command -v "$relic")") in
'#!')
	die "$relic is a script, perhaps libtool's wrapper, whose start-up" \
	    "would be timed with relic; name an installed relic or one built" \
	    "with --disable-shared"
	;;
esac

# The engines to run besides relic: every requested one that is installed.
other_engines=
for engine in $all_engines; do
	if [ "$engine" = relic ] || ! member "$engine" $requested_engines; then
		continue
	fi
	if ! installed "$engine"; then
		echo "compare: $(engine_tool "$engine") is not installed;" \
		    "skipping $engine" >&2
		continue
	fi
	other_engines="$other_engines $engine"
done
engines="relic$other_engines"

# Souffle's compiled programs include the headers installed beside it and
# are built for the width of its numbers, as souffle -o builds them.
if member souffle $engines || member souffle-magic $engines; then
	souffle_include=$(dirname "$(dirname "$(realpath \
	    "$(command -v "$souffle")")")")/include
	domain_bits=$(souffle_domain_bits)
	[ -n "$domain_bits" ] ||
	    die "$souffle --version does not report its word size"
	cxx_switches="-std=c++17 -O3 -DRAM_DOMAIN_SIZE=$domain_bits"
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM
: > "$work/results.tsv"

print_header
for workload in $workloads; do
	prepare_dataset "$(dataset_of "$workload")"
	rm -f "$work/reference"

	engine=relic
	run_engine
	if [ "$status" != ok ]; then
		echo "compare: relic has no reference answer to $workload;" \
		    "skipping the other engines" >&2
		continue
	fi
	for engine in $other_engines; do
		run_engine
	done
done
summarize

if [ -n "$results" ]; then
	{
		printf '%s\n' $columns | paste -s -d '\t' -
		cat "$work/results.tsv"
	} > "$results"
fi
if awk -F '\t' -v columns="$columns" "$column_numbers"'
    $col["status"] ~ /^(failed|mismatch|empty)$/ { found = 1 }
    END { exit !found }' "$work/results.tsv"; then
	exit 1
fi
