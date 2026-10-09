#!/bin/sh
#
# Compare relic with other Datalog and tabled-Prolog engines and with SQL
# databases.
#
# usage: bench/compare/run.sh [-e engines] [-o results.tsv] [-w workloads]
#
# A workload is a directory beside this script holding one program per
# engine and a file naming the data set, which bench/datasets.sh generates;
# or, in place of the programs, a `programs` file naming the directory
# whose programs it runs, for workloads that differ only in their data.
# The SQL engines may lack a program, for a workload SQL cannot express,
# and so may an engine a workload's `absent` file names; they are reported
# n/a there.
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
# BENCH_TIMEOUT, 300, the seconds each engine is given to verify its answer.
# BENCH_THREADS, 1, sets relic, Souffle and DuckDB's thread count; other
# engines keep their serial execution. Parallel compiled Souffle needs
# OpenMP and BENCH_OPENMP_FLAGS (default -fopenmp), trusted compiler flags.
# BENCH_ARTIFACTS names a directory retaining each timed cell's JSON samples;
# commands in these reports use engine/workload labels instead of local paths.
# BENCH_DUCKDB_MEMORY_LIMIT optionally raises DuckDB memory allowance, in GiB.
#
# A workload whose data set is downloaded rather than generated, as the
# Graspan points-to inputs are, runs only when -w names it.  Its archive is
# kept in bench/compare/cache/ and checked against the SHA-256 checksum
# recorded here before every use; one that is missing or does not match is
# downloaded, a download that does not match is discarded, and a workload
# whose data cannot be had is skipped with a note.  BENCH_SCALE does not
# change a downloaded data set.
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
cache=$here/cache
runs=${BENCH_RUNS:-5}
limit=${BENCH_TIMEOUT:-300}
threads=${BENCH_THREADS:-1}
artifacts=${BENCH_ARTIFACTS:-}
openmp_flags=${BENCH_OPENMP_FLAGS:--fopenmp}
duckdb_memory_limit=${BENCH_DUCKDB_MEMORY_LIMIT:-}

# The switches each engine runs with, recorded in the report's header.
# SWI-Prolog reads no initialization file, which could change its flags,
# and its default stack and table space are too small for the larger
# closures.  Souffle and DuckDB use the requested threads, and DuckDB may not
# download an extension during a run.  DuckDB and SQLite read no
# initialization file, stop at the first error, and print rows without a
# header, their columns separated by tabs.  The compiler's
# switches depend on the Souffle installed and are set once it is found.
relic_switches="-F tsv -j $threads"
swipl_switches="-f none --stack-limit=16g --table-space=64g"
souffle_switches="-j$threads"
magic_switches="--magic-transform=*"
clingo_switches="-V0"
nemo_switches="--report none -o"
duckdb_switches="-batch -bail -no-init -noheader -cmd '.mode tabs'"
duckdb_switches="$duckdb_switches -cmd 'SET threads = $threads'"
duckdb_switches="$duckdb_switches -cmd"
duckdb_switches="$duckdb_switches 'SET autoinstall_known_extensions = false'"
sqlite_switches="-batch -bail -init /dev/null -noheader -tabs"
cxx_switches=

# The engines, in the report's order; relic, the reference, is first.
all_engines="relic swipl souffle-interp souffle souffle-magic clingo nemo"
all_engines="$all_engines duckdb sqlite"

# The columns of a result, in the order result_row writes them.
columns="workload engine threads scale status rows median fastest spread user"
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

# joined WORD...: the words joined by spaces, as echo would print them.
# echo decodes backslash escapes on some systems, which would turn a quoted
# path's `\047` into a quote that ends its quoting.
joined() {
	printf '%s\n' "$*"
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

# workload_names: every directory beside this script that holds a workload
# whose data set is generated, in the order of their names.  A workload of
# a downloaded data set runs only when it is named.
workload_names() {
	for dataset_file in "$here"/*/dataset; do
		workload=$(basename "$(dirname "$dataset_file")")
		if ! external_checksum "$(dataset_of "$workload")" > /dev/null
		then
			printf '%s\n' "$workload"
		fi
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

# engine_threads ENGINE: the configured count, or one for serial engines.
engine_threads() {
	case $1 in
	relic | souffle | souffle-interp | souffle-magic | duckdb)
		printf '%s\n' "$threads"
		;;
	*) printf '1\n' ;;
	esac
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
	duckdb)
		if [ -n "$duckdb_memory_limit" ]; then
			echo "$duckdb_switches; memory limit $duckdb_memory_limit GiB"
		else
			echo "$duckdb_switches; memory limit default"
		fi
		;;
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

# The Graspan data sets: the pointer-assignment and dereference relations
# Graspan extracted from Apache httpd 2.2.18, PostgreSQL 8.3.9 and Linux
# 4.4.0-rc5, as the FlowLog benchmark collection publishes them under the
# Apache License 2.0, one archive per program holding Assign.csv and
# Dereference.csv.  The URL names one revision of the collection, the one
# whose archives the checksums are of; the checksum, not the URL, decides
# that an archive is the one recorded.
graspan_url=https://huggingface.co/datasets/NemoYuu/flowlog_benchmark
graspan_url=$graspan_url/resolve/da9e91b3ff75d94604f57ba2b21ef3aa97e241ec
graspan_url=$graspan_url/dataset/csv
graspan_checksums='
httpd 881ab8a5c1bd654a82806b4b618a94ac2f5b02cee18caed77cd2d81c736b8f5a
postgresql 567bc58b4c0fe50f74774208ebcc75956260b9671073d1082ccd378d95748d9c
linux c4f3f9bc393b34e04ccbef5864e2dbbb82b9fe57cc26ed9b4aac73b9ac85877d
'

# The seconds a download may take before it is abandoned.
download_limit=600

# external_checksum NAME: the SHA-256 checksum of the archive of the
# downloaded data set NAME, graspan-<program>, or a failure if NAME is
# generated.
external_checksum() {
	case $1 in
	graspan-*) ;;
	*) return 1 ;;
	esac
	printf '%s\n' "$graspan_checksums" | awk -v program="${1#graspan-}" '
		$1 == program { print $2; found = 1 }
		END { exit !found }'
}

# sha256_of FILE: FILE's SHA-256 checksum, by whichever tool the system has.
# The digest is taken from the tool's output by the shell rather than by a
# pipeline, so that the tool's failure is the function's.
sha256_of() {
	if command -v sha256sum > /dev/null 2>&1; then
		digest=$(sha256sum < "$1") || return 1
	elif command -v shasum > /dev/null 2>&1; then
		digest=$(shasum -a 256 < "$1") || return 1
	elif command -v sha256 > /dev/null 2>&1; then
		digest=$(sha256 -q < "$1") || return 1
	else
		echo "compare: no sha256sum, shasum or sha256 to check a" \
		    "download" >&2
		return 1
	fi
	printf '%s\n' "${digest%% *}"
}

# matches_checksum FILE NAME: whether FILE is the archive recorded for the
# downloaded data set NAME.
matches_checksum() {
	actual=$(sha256_of "$1") || return 1
	[ "$actual" = "$(external_checksum "$2")" ]
}

# download URL FILE: fetch URL into FILE within download_limit seconds, by
# curl, or by fetch(1) where FreeBSD's base system has no curl.  curl reads
# no configuration file, which could weaken its checks, and takes HTTPS
# alone, redirects included.  The checksum, not the transport, is what
# authenticates the archive.
download() {
	if command -v curl > /dev/null 2>&1; then
		"$timeout" -k 10 "$download_limit" curl -q --proto =https \
		    --proto-redir =https -fsSL -o "$2" "$1"
	elif command -v fetch > /dev/null 2>&1; then
		"$timeout" -k 10 "$download_limit" fetch -q -o "$2" "$1"
	else
		echo "compare: no curl or fetch to download $1" >&2
		return 1
	fi
}

# download_to_cache NAME: download the archive of the data set NAME into
# the cache, replacing any archive there.  It is written to a file of this
# run's own, cache_part, checked, and only then renamed into place, so that
# the cache holds a complete, verified archive or none, no other run can be
# writing the file it names, and nothing is ever removed from it.  The exit
# trap removes cache_part if the run ends before the rename.
download_to_cache() {
	echo "compare: downloading the data set $1" >&2
	if ! mkdir -p "$cache"; then
		return 1
	fi
	if ! cache_part=$(mktemp "$cache/$1.XXXXXX"); then
		cache_part=
		return 1
	fi

	if ! download "$graspan_url/cspa-${1#graspan-}.zip" "$cache_part"
	then
		rm -f "$cache_part"
		cache_part=
		return 1
	fi
	if ! matches_checksum "$cache_part" "$1"; then
		echo "compare: the download of $1 does not match its" \
		    "checksum" >&2
		rm -f "$cache_part"
		cache_part=
		return 1
	fi
	if ! mv "$cache_part" "$cache/$1.zip"; then
		rm -f "$cache_part"
		cache_part=
		return 1
	fi
	cache_part=
}

# copy_checked NAME: copy the cached archive of the data set NAME to
# $work/archives/NAME.zip and check the copy, which is the only file the
# run reads it from: what is read is then what was checked, whatever
# happens to the cache meanwhile.  Fails, leaving no copy, if the cache
# holds no archive or one that does not match.
copy_checked() {
	cached=$cache/$1.zip
	copy=$work/archives/$1.zip
	if [ ! -f "$cached" ]; then
		return 1
	fi
	if ! mkdir -p "$work/archives"; then
		return 1
	fi
	if ! cp "$cached" "$copy.part"; then
		rm -f "$copy.part"
		return 1
	fi
	if ! matches_checksum "$copy.part" "$1"; then
		rm -f "$copy.part"
		return 1
	fi
	mv "$copy.part" "$copy"
}

# fetch_archive NAME: make $work/archives/NAME.zip a checked copy of the
# archive of the downloaded data set NAME, from the cache, or from a fresh
# download into the cache if the cache lacks the archive or holds one that
# does not match.  Fails, having said why, if the archive cannot be had.
# Called as a condition, where errexit does not apply, so every step's
# failure is tested.
fetch_archive() {
	if [ -f "$work/archives/$1.zip" ]; then
		return 0
	fi
	if ! command -v unzip > /dev/null 2>&1; then
		echo "compare: unzip is needed to unpack $1" >&2
		return 1
	fi

	if copy_checked "$1"; then
		return 0
	fi
	if [ -f "$cache/$1.zip" ]; then
		echo "compare: $(home_relative "$cache/$1.zip") does not" \
		    "match its checksum" >&2
	fi
	if ! download_to_cache "$1"; then
		return 1
	fi
	copy_checked "$1"
}

# unpack_member ARCHIVE MEMBER FILE: the comma-separated relation MEMBER of
# ARCHIVE as FILE, a relation file whose columns are separated by tabs.
# unzip writes the member to its standard output, so nothing in the archive
# names a path that is written, and it is written whole before it is
# converted, so that a failure of unzip is not hidden by a pipeline.
unpack_member() {
	unzip -p "$1" "$2" > "$3.csv"
	tr ',' '\t' < "$3.csv" > "$3"
	rm "$3.csv"
}

# prepare_dataset NAME: make the data set NAME in $work/data/NAME, once,
# and its facts as source text in $work/data/NAME.pl: generated, or
# unpacked from the archive fetch_archive has checked.  A downloaded
# relation may repeat a row, and unique_rows keeps only its first
# occurrence, as a generator's relation files are sets.  Runs under
# errexit, so a step that fails ends the run.
prepare_dataset() {
	dir=$work/data/$1
	if [ -d "$dir" ]; then
		return 0
	fi

	mkdir -p "$dir"
	if external_checksum "$1" > /dev/null; then
		archive=$work/archives/$1.zip
		member=cspa-${1#graspan-}
		unpack_member "$archive" "$member/Assign.csv" "$dir/assign.tsv"
		unpack_member "$archive" "$member/Dereference.csv" \
		    "$dir/dereference.tsv"
		unique_rows "$dir"
	else
		dataset "$1" "$dir" "$scale"
	fi
	source_facts "$dir" > "$dir.pl"
}

# programs_of WORKLOAD: the directory holding WORKLOAD's programs and its
# `absent` file: the workload's own, or the one its `programs` file names,
# for workloads that run the same programs over different data sets.
programs_of() {
	if [ ! -f "$here/$1/programs" ]; then
		printf '%s\n' "$here/$1"
		return 0
	fi

	read -r programs_name < "$here/$1/programs"
	case $programs_name in
	'' | */* | .*) die "$1 names no valid directory of programs" ;;
	esac
	printf '%s\n' "$here/$programs_name"
}

# program_file ENGINE: the name of ENGINE's program in a workload's
# directory of programs.
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

# optional_program ENGINE WORKLOAD: whether WORKLOAD may have no program
# for ENGINE.  DuckDB and SQLite cannot express nonlinear recursion or
# recursion through two relations, so neither needs a program for any
# workload.  Every other engine needs one unless the workload's `absent`
# file names it, one engine per line followed by why its language cannot
# say what the workload asks.
optional_program() {
	case $1 in
	duckdb | sqlite) return 0 ;;
	esac
	absent=$(programs_of "$2")/absent
	[ -f "$absent" ] && awk -v engine="$1" '
		$1 == engine { found = 1 }
		END { exit !found }' "$absent"
}

# relic_imports DIR: an -I option importing each relation file of DIR into
# its relation, each column typed as source_facts would spell its values: a
# column of decimal digits, signed or not, holds integers, one spelled as a
# symbol symbols, and any other strings.  The data sets write every column
# one way, so the first row decides.  An empty relation file is not
# imported, as source_facts writes no facts for one.
relic_imports() (
	for file in "$1"/*.tsv; do
		[ -s "$file" ] || continue
		columns=$(awk -F '\t' 'NR == 1 {
			for (i = 1; i <= NF; i++) {
				type = "string"
				if ($i ~ /^-?[0-9]+$/)
					type = "integer"
				else if ($i ~ /^[a-z][A-Za-z0-9_]*$/)
					type = "symbol"
				printf "%s%s", (i > 1 ? "," : ""), type
			}
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
	program=$(programs_of "$2")/$(program_file "$1")
	data=$work/data/$(dataset_of "$2")
	out=$work/out/$1

	case $1 in
	relic)
		joined "$(quote "$relic") $relic_switches" \
		    "$(relic_imports "$data") $(quote "$program")"
		;;
	swipl)
		joined "$(quote "$swipl") $swipl_switches" \
		    "$(quote "$here/driver.pl") $(quote "$program")" \
		    "$(quote "$data.pl")"
		;;
	souffle-interp)
		joined "$(quote "$souffle") $souffle_switches" \
		    "-F $(quote "$data") -D $(quote "$out")" \
		    "$(quote "$program")"
		;;
	souffle | souffle-magic)
		joined "$(quote "$work/bin/$1") $souffle_switches" \
		    "-F $(quote "$data") -D $(quote "$out")"
		;;
	clingo)
		joined "$(quote "$clingo") $clingo_switches" \
		    "$(quote "$data.pl") $(quote "$program")"
		;;
	nemo)
		joined "$(quote "$nmo") $nemo_switches" \
		    "-I $(quote "$data") -D $(quote "$out")" \
		    "$(quote "$program")"
		;;
	duckdb)
		memory_switch=
		if [ -n "$duckdb_memory_limit" ]; then
			memory_switch="-cmd $(quote "SET memory_limit = '${duckdb_memory_limit}GiB'")"
		fi
		joined "$(quote "$duckdb") $duckdb_switches $memory_switch" \
		    "-cmd $(quote ".cd $(dot_argument "$data")")" \
		    "-f $(quote "$program")"
		;;
	sqlite)
		joined "$(quote "$sqlite") $sqlite_switches" \
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
	program=$(programs_of "$2")/$(program_file "$1")
	mkdir -p "$work/bin"

	translate="$(quote "$souffle")"
	if [ "$1" = souffle-magic ]; then
		translate="$translate $(quote "$magic_switches")"
	fi
	translate="$translate $souffle_switches -g $(quote "$source") $(quote "$program")"
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
# failure if one is missing. Both pretty-printed JSON schemas are accepted.
# One run has no deviation, written as null and reported as -. The peak is
# the largest resident memory across the runs, in bytes.
parse_timing() {
	awk -f "$here/timing.awk" "$1"
}

# measure ENGINE WORKLOAD: time BENCH_RUNS runs with hyperfine, setting
# median, fastest, spread, user and peak_bytes.  clingo's successful statuses
# are not zero, so hyperfine is told to ignore statuses for it; its
# verification run has already checked what they mean.
measure() {
	ignore=
	if [ "$1" = clingo ]; then
		ignore=$clingo_ignore
	fi

	if ! hyperfine -N $ignore $timing_switches --style none --runs "$runs" \
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
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
	    "$workload" "$engine" "$(engine_threads "$engine")" "$scale" \
	    "$status" "$rows" "$median" "$fastest" \
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
			printf first_column \
			    "%-14s %8.3f %8.3f %7s %8.3f %8.1f", \
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

# reset_result: a result of no measurement, whose status is ok until a
# step sets another.
reset_result() {
	status=ok
	rows=-
	median=-
	fastest=-
	spread=-
	user=-
	peak_bytes=-
	compile_seconds=-
}

# record_result: append the result being recorded to the results and print
# it as a line of the report.
record_result() {
	row=$(result_row)
	printf '%s\n' "$row" >> "$work/results.tsv"
	if [ -n "$results" ]; then
		printf '%s\n' "$row" >> "$results"
	fi
	print_result "$row"
}

# run_engine: compile if $engine needs it, answer $workload, judge the
# answer, time the engine if it agrees, and record the result.  A workload
# with no program for the engine is n/a if the engine's programs are
# optional, and a failure otherwise.
run_engine() {
	reset_result
	rm -f "$work/timing.json"

	if [ ! -f "$(programs_of "$workload")/$(program_file "$engine")" ]
	then
		if optional_program "$engine" "$workload"; then
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

	if [ -n "$artifacts" ] && [ -f "$work/timing.json" ]; then
		# Commands contain disposable paths; sample values stay unchanged.
		awk -v label="$engine $workload" '
			/"command":/ {
				print "      \"command\": \"" label "\","; next
			}
			{ print }
		' "$work/timing.json" > "$artifacts/$workload.$engine.json"
	fi
	record_result
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
	echo "# scale $scale, $runs timed runs, ${limit}s to answer, $threads threads"
	echo "# $(hyperfine --version)"
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
	    workload engine median fastest spread user "peak MiB" rows compile \
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
require_positive BENCH_THREADS "$threads"
if [ -n "$duckdb_memory_limit" ]; then
	require_positive BENCH_DUCKDB_MEMORY_LIMIT "$duckdb_memory_limit"
fi
[ "$threads" -le 256 ] || die "BENCH_THREADS must be from 1 to 256"
for engine in $requested_engines; do
	member "$engine" $all_engines || die "unknown engine $engine"
done
for workload in $workloads; do
	case $workload in
	*/* | .*) die "unknown workload $workload" ;;
	esac
	[ -f "$here/$workload/dataset" ] || die "unknown workload $workload"
	[ -d "$(programs_of "$workload")" ] ||
	    die "$workload names no directory of programs"
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
# Hyperfine 2 names metrics explicitly and accepts selected success codes.
# Earlier releases include the same measures in their default JSON export.
timing_switches=
clingo_ignore=-i
case $(hyperfine --version) in
'hyperfine 2.'*)
	timing_switches="--metrics time_wall_clock,time_user,memory_peak_resident"
	clingo_ignore=--ignore-failure=10,30
	;;
esac
if [ -n "$artifacts" ]; then
	artifacts=$artifacts/scale-$scale/threads-$threads
	[ ! -e "$artifacts" ] || die "artifact directory already exists: $artifacts"
	mkdir -p "$artifacts"
	artifacts=$(CDPATH= cd -- "$artifacts" && pwd)
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

# The interpreter must also refuse a thread count its build cannot honor.
if [ "$threads" -gt 1 ]; then
	for engine in $engines; do
		case $engine in
		souffle | souffle-interp | souffle-magic)
			"$souffle" --version | grep -qi 'openmp' ||
			    die "parallel Souffle requires a build with OpenMP"
			break
			;;
		esac
	done
fi

# Souffle's compiled programs include the headers installed beside it and
# are built for the width of its numbers, as souffle -o builds them.
if member souffle $engines || member souffle-magic $engines; then
	souffle_include=$(dirname "$(dirname "$(realpath \
	    "$(command -v "$souffle")")")")/include
	domain_bits=$(souffle_domain_bits)
	[ -n "$domain_bits" ] ||
	    die "$souffle --version does not report its word size"
	cxx_switches="-std=c++17 -O3 -DRAM_DOMAIN_SIZE=$domain_bits"
	if [ "$threads" -gt 1 ]; then
		cxx_switches="$cxx_switches $openmp_flags"
	fi
fi

work=$(mktemp -d) || exit 1
cache_part=
trap 'rm -rf "$work"; [ -z "$cache_part" ] || rm -f "$cache_part"' EXIT
trap 'exit 1' HUP INT TERM
: > "$work/results.tsv"
if [ -n "$results" ]; then
	printf '%s\n' $columns | paste -s -d '\t' - > "$results"
fi

print_header
for workload in $workloads; do
	dataset_name=$(dataset_of "$workload")
	if external_checksum "$dataset_name" > /dev/null &&
	    ! fetch_archive "$dataset_name"; then
		echo "compare: the data set of $workload is not available;" \
		    "skipping it" >&2
		engine=relic
		reset_result
		status=no-data
		record_result
		continue
	fi
	prepare_dataset "$dataset_name"
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

if awk -F '\t' -v columns="$columns" "$column_numbers"'
    $col["status"] ~ /^(failed|mismatch|empty)$/ { found = 1 }
    END { exit !found }' "$work/results.tsv"; then
	exit 1
fi
