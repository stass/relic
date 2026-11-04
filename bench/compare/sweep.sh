#!/bin/sh
# Run a complete comparison into a new directory; never resume a cohort.
# Usage: sweep.sh -o DIRECTORY [-j PARALLEL_THREADS] [-n]
# -n checks tools and output routing without starting the benchmark sweep.
# Engine paths use the same environment variables as run.sh; hyperfine must
# be on PATH. The default parallel setting is 64 workers for the FreeBSD host.
freeze_sources() {
	tar -C "$root" --exclude 'bench/compare/cache' \
		--exclude '*.o' --exclude '*.lo' \
	    --exclude '.deps' --exclude '.libs' --exclude 'bench/compare/results.tsv' \
		--exclude 'bench/compare/suite-summary.tsv' -cf "$work/source.tar" \
		bench/datasets.sh bench/compare
	mkdir "$work/source"
	tar -xf "$work/source.tar" -C "$work/source"
	snapshot=$work/source/bench/compare
	ln -s "$here/cache" "$snapshot/cache"
}

check_output_exports() {
	mkdir "$work/sf-out" "$work/nmo-out"
	printf '1\t2\n' > "$work/expected"
	cat > "$work/check.dl" <<-'RULE'
	.decl answer(x:number, y:number)
	answer(1,2).
	.output answer(filename="answer.csv")
	RULE
	(cd "$work/sf-out" && "$SOUFFLE" -j1 -D - "$work/check.dl") > "$work/sf-stdout"
	awk 'NF == 2 && $1 == 1 && $2 == 2 { print $1 "\t" $2 }' \
		"$work/sf-stdout" > "$work/sf-rows"
	cmp "$work/expected" "$work/sf-rows" || fail 'Souffle stdout export differs'
	# Generated binaries need the same headers, domain width and OpenMP flags
	# as run.sh; an OpenMP interpreter alone does not establish that they work.
	souffle_include=$(dirname "$(dirname "$(realpath "$(command -v "$SOUFFLE")")")")/include
	domain_bits=$("$SOUFFLE" --version | awk '/Word size:/ { print $3 }')
	case $domain_bits in
	32 | 64) ;;
	*) fail 'Souffle must report a 32-bit or 64-bit domain' ;;
	esac
	"$SOUFFLE" -j"$parallel" -g "$work/check.cpp" "$work/check.dl"
	# BENCH_OPENMP_FLAGS, like run.sh's compiler flags, are trusted shell words.
	build="$(quote "$CXX") -std=c++17 -O3 -DRAM_DOMAIN_SIZE=$domain_bits"
	build="$build ${BENCH_OPENMP_FLAGS:--fopenmp} -I $(quote "$souffle_include")"
	build="$build -o $(quote "$work/check") $(quote "$work/check.cpp")"
	sh -c "$build"
	(cd "$work/sf-out" && "$work/check" -j"$parallel" -D -) > "$work/sf-compiled-stdout"
	awk 'NF == 2 && $1 == 1 && $2 == 2 { print $1 "\t" $2 }' \
		"$work/sf-compiled-stdout" > "$work/sf-compiled-rows"
	cmp "$work/expected" "$work/sf-compiled-rows" || fail 'Compiled Souffle stdout export differs'
	cat > "$work/check.rls" <<-'RULE'
	@parameter $answer_resource = "answer.tsv" .
	answer(1,2).
	@export answer :- tsv{resource=$answer_resource} .
	RULE
	"$NMO" --report none -o -D "$work/nmo-out" "$work/check.rls" > "$work/nmo-stdout"
	cmp "$work/expected" "$work/nmo-out/answer.tsv" || fail 'Nemo verification export differs'
	[ ! -s "$work/nmo-stdout" ] || fail 'Nemo verification unexpectedly prints rows'
	rm "$work/nmo-out/answer.tsv"
	"$NMO" --report none -o -D "$work/nmo-out" --param 'answer_resource=""' \
		"$work/check.rls" > "$work/nmo-stdout"
	cmp "$work/expected" "$work/nmo-stdout" || fail 'Nemo stdout export differs'
	[ "$(find "$work/sf-out" "$work/nmo-out" -type f | wc -l)" -eq 0 ] ||
		fail 'stdout preflight unexpectedly created an answer file'
}

run_scenario() {
	scale=$1
	threads=$2
	export BENCH_SCALE=$scale BENCH_THREADS=$threads
	BENCH_RUNS=3
	workloads=$generated_workloads
	if [ "$scale" = 1 ]; then BENCH_RUNS=10; workloads=$all_workloads; fi
	export BENCH_RUNS
	engines=relic,swipl,souffle-interp,souffle,souffle-magic,clingo,nemo,duckdb,sqlite
	if [ "$threads" != 1 ]; then engines=relic,souffle-interp,souffle,souffle-magic,duckdb; fi
	prefix=$record/scale-$scale-threads-$threads
	printf 'Starting scale %s threads %s at %s\n' "$scale" "$threads" "$(date -u '+%FT%TZ')"
	printf 'Scenario incomplete until the harness returns\n' > "$prefix.interrupted"
	code=0
	sh "$snapshot/run.sh" -e "$engines" -w "$workloads" -o "$prefix.tsv" \
		> "$prefix.txt" 2> "$prefix.stderr.txt" || code=$?
	printf '%s\n' "$code" > "$prefix.exit-status"
	[ "$code" -le 1 ] || fail "scenario aborted with exit $code; evidence preserved"
	rm "$prefix.interrupted"
	printf 'Completed scale %s threads %s with exit %s at %s\n' \
		"$scale" "$threads" "$code" "$(date -u '+%FT%TZ')"
}

main() {
	set -eu
	here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
	root=$(CDPATH= cd -- "$here/../.." && pwd)
	record=
	parallel=64
	check_only=no
	fail() { printf 'sweep: %s\n' "$*" >&2; exit 2; }
	# Match run.sh's shell-word handling for paths and trusted compiler flags.
	quote() {
		printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
	}
	while getopts 'j:no:' option; do
		case $option in
		j) parallel=$OPTARG ;;
		n) check_only=yes ;;
		o) record=$OPTARG ;;
		*) fail 'usage: sweep.sh -o DIRECTORY [-j PARALLEL_THREADS] [-n]' ;;
		esac
	done
	shift $((OPTIND - 1))
	[ "$#" -eq 0 ] && [ -n "$record" ] || fail 'a fresh -o directory is required'
	case $parallel in
	'' | *[!0-9]* | 0*) fail '-j must be a positive integer' ;;
	esac
	[ "$parallel" -gt 1 ] && [ "$parallel" -le 256 ] || fail '-j must be from 2 to 256'
	case $record in
	/*) ;;
	*) record=$PWD/$record ;;
	esac
	[ ! -e "$record" ] || fail 'output directory already exists; choose a fresh cohort'
	export RELIC=${RELIC:-$root/relic} SWIPL=${SWIPL:-swipl}
	export SOUFFLE=${SOUFFLE:-souffle} CLINGO=${CLINGO:-clingo} NMO=${NMO:-nmo}
	export DUCKDB=${DUCKDB:-duckdb} SQLITE=${SQLITE:-sqlite3} CXX=${CXX:-c++}
	export BENCH_TIMEOUT=${BENCH_TIMEOUT:-300}
	export CC=${CC:-cc}
	case $BENCH_TIMEOUT in
	'' | *[!0-9]* | 0*) fail 'BENCH_TIMEOUT must be a positive integer' ;;
	esac
	if [ -n "${BENCH_PAIRSET:-}" ]; then
		command -v "$BENCH_PAIRSET" > /dev/null 2>&1 || fail 'BENCH_PAIRSET is not executable'
	fi
	for tool in "$RELIC" "$SWIPL" "$SOUFFLE" "$CLINGO" "$NMO" \
		"$DUCKDB" "$SQLITE" "$CXX" "$CC" hyperfine; do
		command -v "$tool" > /dev/null 2>&1 || fail "missing executable: $tool"
	done
	command -v timeout > /dev/null 2>&1 || command -v gtimeout > /dev/null 2>&1 ||
		fail 'timeout(1) or gtimeout is required'
	"$SOUFFLE" --version | grep -qi openmp || fail 'Souffle needs an OpenMP build'

	# Check the installed engines, not only their advertised options. Nemo's
	# parameter changes only the destination; the default still exports TSV.
	work=$(mktemp -d)
	trap 'rm -rf "$work"' EXIT
	trap 'exit 1' HUP INT TERM
	# Copy sources before running any cohort. Live repository edits cannot change
	# the executing harness, dataset generators or query programs mid-sweep.
	freeze_sources
	check_output_exports

	# Workloads are discovered rather than frozen in a second list. Only the
	# downloaded datasets stay fixed at scale 1, as in run.sh's default selection.
	all_workloads=
	generated_workloads=
	for dataset in "$snapshot"/*/dataset; do
		workload=${dataset%/dataset}
		workload=${workload##*/}
		all_workloads="${all_workloads:+$all_workloads,}$workload"
		case $(cat "$dataset") in
		graspan-*) continue ;;
		esac
		generated_workloads="${generated_workloads:+$generated_workloads,}$workload"
	done
	printf 'Preflight passed. Scales: 1, 2, 4, 8; threads: 1, %s; runs: 10, 3, 3, 3.\n' "$parallel"
	[ "$check_only" = no ] || exit 0

	# mkdir is the final atomic refusal to overwrite a concurrently created cohort.
	mkdir -p "$here/cache"
	mkdir -p "$(dirname -- "$record")"
	mkdir "$record"
	record=$(CDPATH= cd -- "$record" && pwd)
	export BENCH_ARTIFACTS=$record/samples
	if [ -z "${BENCH_PAIRSET:-}" ]; then
		"$CC" -O3 -std=c11 -pthread "$snapshot/pairset.c" -o "$work/pairset"
		export BENCH_PAIRSET=$work/pairset
	fi
	{
		printf 'OS: '; uname -sr
		printf 'Started UTC: '; date -u '+%FT%TZ'
		printf 'Parallel workers: %s\n' "$parallel"
		printf 'Verification timeout: %s seconds\n' "$BENCH_TIMEOUT"
		printf 'Timed output: complete answer serialization to discarded stdout\n'
		printf 'Repetitions: scale 1 = 10; scales 2, 4, 8 = 3\n'
		sysctl hw.model hw.ncpu hw.physmem 2>/dev/null || true
	} > "$record/environment.txt"
	(git -C "$root" rev-parse HEAD && git -C "$root" status --porcelain) \
		> "$record/source-revision.txt" 2>/dev/null || printf 'Git metadata unavailable\n' >> "$record/source-revision.txt"
	cp "$work/source.tar" "$record/source.tar"
	if command -v sha256 > /dev/null 2>&1; then
		(cd "$work/source" && find bench -type f -exec sha256 -r {} +) > "$record/source-sha256.txt"
	elif command -v sha256sum > /dev/null 2>&1; then
		(cd "$work/source" && find bench -type f -exec sha256sum {} +) > "$record/source-sha256.txt"
	else
		command -v shasum > /dev/null 2>&1 || fail 'sha256, sha256sum or shasum is required'
		(cd "$work/source" && find bench -type f -exec shasum -a 256 {} +) > "$record/source-sha256.txt"
	fi
	for scale in 1 2 4 8; do
		for threads in 1 "$parallel"; do
			run_scenario "$scale" "$threads"
		done
	done
	: > "$record/main-sweep-complete"
}

main "$@"
