# FreeBSD benchmark builds

The Threadripper PRO 5995WX comparison uses FreeBSD 16.0-CURRENT, 64 physical
cores, 128 hardware threads and 256 GiB installed memory. The requested
parallel setting is 64 workers. CPU placement follows the OS scheduler;
workers are not pinned to individual cores.

The engine ports are clingo 5.8.0, SWI-Prolog 10.0.2 and DuckDB 1.5.6.
SQLite 3.54.1 is built from upstream source. DuckDB, SQLite and SWI-Prolog differ from the Mac versions;
results retain each host's versions and do not attribute cross-host timing
differences solely to hardware. Souffle 2.5, Nemo 0.10.1 and hyperfine 2.0.0
are built from their release sources. This ports tree has no Souffle or Nemo
rule-engine port, and its hyperfine port provides 1.20.0.

## Prerequisites

Use the ports tree for installations. Autoconf, Automake, CMake, Ninja,
Bison, flex, GNU make, Python 3.12 and pkgconf are build tools. FreeBSD's
base clang 19.1.7 and OpenMP runtime supply the native C/C++ compiler and
parallel runtime. The installed tools and versions belong in the machine's
benchmark environment record.

Install missing tools and the engine ports with noninteractive builds:

```sh
sudo make -C /usr/ports/devel/libtool -DBATCH install clean
sudo make -C /usr/ports/devel/mcpp -DBATCH install clean
sudo make -C /usr/ports/lang/rust-nightly -DBATCH CPUTYPE= \
    MAKE_JOBS_NUMBER=16 install clean
sudo make -C /usr/ports/math/clingo -DBATCH MAKE_JOBS_NUMBER=16 install clean
sudo make -C /usr/ports/databases/duckdb -DBATCH MAKE_JOBS_NUMBER=16 install clean
sudo make -C /usr/ports/lang/swipl -DBATCH MAKE_JOBS_NUMBER=16 install clean
```

Nemo requires nightly Rust language features. Clearing `CPUTYPE` for the
Rust port prevents a host-specific `-march=native` flag from reaching its
WebAssembly compiler. This invocation does not change global ports settings.
Souffle's interpreter and translator require `mcpp` or GCC preprocessing;
FreeBSD's base `cpp` alone does not satisfy their automatic discovery.

## Source builds

The [Apple Silicon build instructions](README.md) pin the Souffle 2.5
archive and compatibility patch. Apply that patch and the
[FreeBSD alignment patch](souffle-2.5-freebsd-alignment.patch) to its source.
The alignment patch uses a 64-byte fallback on FreeBSD/x86-64: this CPU's
CPUID reports 64 bytes, while `2 * sizeof(max_align_t)` evaluates to an
invalid 96-byte alignment. Installed headers carry the same change.

Choose a tool installation prefix and configure Souffle:

```sh
BENCH_TOOLS="$HOME/relic-work/tools/installed"
cmake -S souffle-2.5 -B souffle-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
    -DCMAKE_INSTALL_PREFIX="$BENCH_TOOLS/souffle" \
    -DSOUFFLE_GIT=OFF -DGIT_PACKAGE_VERSION=2.5 -DPACKAGE_VERSION=2.5 \
    -DSOUFFLE_USE_LIBCPP=OFF -DSOUFFLE_USE_OPENMP=ON \
    -DSOUFFLE_ENABLE_TESTING=OFF -DSOUFFLE_USE_CURSES=OFF \
    -DSOUFFLE_USE_SQLITE=OFF -DSOUFFLE_USE_ZLIB=OFF \
    -DSOUFFLE_USE_LIBFFI=OFF
cmake --build souffle-build -j8
cmake --install souffle-build
"$BENCH_TOOLS/souffle/bin/souffle" --version
```

`SOUFFLE_USE_LIBCPP=OFF` keeps FreeBSD's default libc++ runtime and avoids
adding the Linux-style `-lc++abi` library flag. Confirm the version output
includes `Options enabled: openmp`. Generated programs also need C++
compilation and linking with `-fopenmp`; the comparison harness supplies
that flag for parallel runs.

Additional release archives:

| Tool | Archive | SHA-256 |
| --- | --- | --- |
| Nemo 0.10.1 | https://github.com/knowsys/nemo/archive/refs/tags/v0.10.1.tar.gz | `19a07dc021929b44ed6d99c17235755096c4245a5bffc64fe5f3bf0f5b697682` |
| hyperfine 2.0.0 | https://github.com/sharkdp/hyperfine/archive/refs/tags/v2.0.0.tar.gz | `f4b71df3c78e4cf752ca6fb6ebc4b025f7ea6a5ca5c48fea75f8a1fdb4c7d721` |

Check each archive with `sha256 -q`, extract it, and build hyperfine with
`cargo build --locked --release -j16`. Build Nemo with
`cargo build --locked --release -p nemo-cli -j16`. Their executables are
`target/release/hyperfine` and `target/release/nmo` respectively.

Build SQLite from
`https://www.sqlite.org/2026/sqlite-autoconf-3540100.tar.gz`, with SHA3-256
`b6ced585c750916b1052667239a12c25a7ff34e7f06e1e6e01ecb0427657d9c4`.
Check the archive with `openssl dgst -sha3-256`, extract it, and run
`./configure --disable-shared --prefix="$BENCH_TOOLS/sqlite" CFLAGS='-O3 -g -march=native'`,
then `gmake -j8` and `gmake install`.

The comparison uses no explicit whole-program LTO for any engine. Each
engine keeps its release build defaults: the ports use native CPU targeting,
SWI-Prolog's port selects its PGO build, and Nemo uses its Cargo release
profile. Souffle and relic use their documented `-O3` builds with default
CPU targeting. Preserve these differences in the environment record.

Build relic with `autoreconf -fi`,
`./configure --disable-shared --enable-threads CFLAGS='-O3 -g'`, and
`gmake -j8`. Run `gmake check` before timing. Select the native executables
through `RELIC`, `SWIPL`, `SOUFFLE`, `CLINGO`, `NMO`, `DUCKDB`, `SQLITE` and
`CXX`; put hyperfine 2.0.0 on `PATH`.

## Measurement checks

Verify exact answers from Souffle at both requested thread counts, including
a generated program compiled with OpenMP. Check the timing parser fixtures
and the timer's peak-resident-memory metric with explicitly touched pages;
a short process or an untouched zero allocation may not establish its
resident-memory peak before exit.

The sweep requests ten timed repetitions at scale 1 and three at scales 2,
4 and 8, with five-minute answer verification limits. The three pinned
Graspan archives run at scale 1 only. Download and checksum them before
starting measurements. Each engine verifies its complete answer against
relic before timing; cases and scenarios run sequentially on each host.
Raw JSON, TSVs, reports, diagnostics, build details and any failed setup
attempts are preserved separately.

## Reusable full sweep

Run [sweep.sh](../sweep.sh) from a built checkout. Engine paths use the
same environment variables as `run.sh`; preflight compiles a tiny Souffle
program with OpenMP and checks both engines' stdout exports. Missing engines stop the runner
instead of silently reducing the comparison. For the source-build layout
above and the installed ports:

```sh
cd "$HOME/relic-work/src/relic"
export PATH="$HOME/relic-work/tools/installed/hyperfine/bin:$PATH"
export SOUFFLE="$HOME/relic-work/tools/installed/souffle/bin/souffle"
export NMO="$HOME/relic-work/tools/installed/nemo/bin/nmo"
export SQLITE="$HOME/relic-work/tools/installed/sqlite/bin/sqlite3"
export RELIC="$PWD/relic" CXX=/usr/bin/c++ CC=/usr/bin/cc

# Check tools and actual stdout exports without starting the sweep.
sh bench/compare/sweep.sh -n -j 64 -o "$HOME/relic-work/results/stdout-01"

# Start after competing workloads have stopped.
sh bench/compare/sweep.sh -j 64 -o "$HOME/relic-work/results/stdout-01"
```

The default parallel setting is 64; `-j` selects another worker count.
The script runs scales 1, 2, 4 and 8 at one thread and that parallel count,
using ten repetitions at scale 1 and three at larger scales. It discovers
all workloads, includes the downloaded Graspan datasets only at scale 1,
and enables the exact bitmap verifier for supported tree answers. Cached
Graspan archives are checksum-checked; missing archives are downloaded by
the harness before their verification. `BENCH_TIMEOUT` defaults to 300
seconds; other resource/compiler controls remain those of `run.sh`.

For a run that survives SSH disconnection, FreeBSD's base `daemon` can
supervise the same command (after exporting the environment above):

```sh
mkdir -p "$HOME/relic-work/logs"
daemon -o "$HOME/relic-work/logs/stdout-01.log" \
    -p "$HOME/relic-work/logs/stdout-01.pid" \
    sh bench/compare/sweep.sh -j 64 -o "$HOME/relic-work/results/stdout-01"
tail -f "$HOME/relic-work/logs/stdout-01.log"
```

Each scenario retains its TSV, report, diagnostics, exit code and raw JSON
samples. The directory also records host information, source revision,
a frozen benchmark-source archive, SHA-256 checksums and output policy.
The sweep executes that source snapshot, so repository edits cannot change
the harness or queries during a run. A `.interrupted` marker persists if a
scenario aborts. `main-sweep-complete` means every scenario returned, not
that every engine succeeded; review all statuses before publication.
Choose a new output directory for every restart. The runner does not resume,
overwrite or mix file-output measurements with stdout-output measurements.

## Publishing a cohort

`publish-results.py --record DIRECTORY --check` validates the whole sweep
without changing publication files. Omit `--check` to write the canonical
matrix, comparison summary, environment, full report and bounded generated
README/website blocks. Regenerate README charts and the website JSON with
the commands in the [chart instructions](README.md#readme-charts).

The cohort's `environment.json` supplies `processor`, `physical_cores`,
`logical_cpus`, `installed_memory_gib`, `os`, `parallel_threads`,
`source_commit`, `output_policy` (`stdout`),
`verification_timeout_seconds` (300), `relic_configure`, `relic_cflags`,
`compiler` and `optimization_policy`. Its `engine_builds` object maps
display names to the recorded build settings, including Souffle's separate
generated-program compilation. Its `version_strings` object maps `relic`, `swipl`, `souffle`,
`clingo`, `nemo`, `duckdb`, `sqlite`, `hyperfine` and `compiler` to the
version text recorded in scenario headers. Only participating engines are
required in a parallel header. Record source and executable hashes, build
logs and initial/final machine context with the cohort. `started_utc` is the
cohort's start, in the form `2026-11-05T02:59:15Z`.

A record may rerun relic on every workload, and every engine on chosen
workloads, and carry the other rival cells from an earlier cohort on the same
host. Its optional `cells_carried_from` object then holds `environment`, the
earlier cohort's `environment.json`, and `rerun_workloads`, the workloads
every engine reran (some, not all). The two environments must agree in
every field but `source_commit`, `source_archive_sha256`, `started_utc` and
relic's entries in `version_strings` and `engine_builds`, and the earlier
cohort must have started first. The record also keeps
`carried-answers/earlier/` and `carried-answers/current/`: one directory per
scenario (`scale-S-threads-T`), holding one file per workload, named for it,
with a digest of relic's normalized answer (sorted and deduplicated, as the
harness compares answers) at that revision. Every carried rival cell with a
time needs two equal digests, so its answer, checked against relic at the
earlier revision, equals relic's at this one. The full report and the README
conditions state which timings are carried.

Publication rejects uninvestigated failures. An optional
`investigated-failures.json` array can classify clingo ID exhaustion or
DuckDB memory exhaustion. Each entry names `scale`, `threads`, `workload`,
`engine`, `exit_code`, `classified_status`, `evidence` and `diagnostic`.
The evidence must be that scenario's stderr file, with the harness failure
header for that exact cell and the matching engine diagnostic. A mismatch,
missing data or an unexplained failure prevents publication.
