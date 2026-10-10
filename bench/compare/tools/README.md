# Benchmark engine builds on Apple Silicon

For native ports and source builds on FreeBSD, see the
[FreeBSD build instructions](README-freebsd.md).

The M3 Max comparison uses pinned native executables: Soufflé 2.5,
clingo 5.8.0, Nemo 0.10.1, SWI-Prolog 9.2.9, DuckDB 1.5.5 and SQLite 3.51.2.
The benchmark driver uses hyperfine 2.0.0. relic is built with Apple clang,
`--disable-shared --enable-threads CFLAGS='-O3 -g'`. Soufflé's generated
programs use Homebrew clang 23.1.3 at `-O3`, with OpenMP for parallel runs.

Build prerequisites are installed with Homebrew:

```sh
brew install autoconf automake libtool cmake ninja bison flex hyperfine llvm lld libomp
```

Sources and executable archives come from the projects' release repositories:

| Archive | URL | SHA-256 |
| --- | --- | --- |
| Soufflé | https://github.com/souffle-lang/souffle/archive/refs/tags/2.5.tar.gz | `5d009ad6c74ccec10207d865c059716afac625759bff7c8070e529bd80385067` |
| clingo | https://github.com/potassco/clingo/archive/refs/tags/v5.8.0.tar.gz | `4ddd5975e79d7a0f8d126039f1b923a371b1a43e0e0687e1537a37d6d6d5cc7c` |
| Nemo | https://github.com/knowsys/nemo/releases/download/v0.10.1/nemo_v0.10.1_aarch64-apple-darwin.tar.gz | `4b1e9a9f46a315ab8c590fb0124cb470c33b5293b9f3a61dd53225b31ed79f16` |

Download each archive with `curl -fL`, check it with `shasum -a 256`, and
extract it with `tar -xzf`. Nemo's checksum also matches its release's
`.sha256sum` asset. Its `nmo` executable needs no local compilation.
SWI-Prolog, DuckDB and SQLite use the installed Homebrew releases.

Soufflé's [build patch](souffle-2.5-build.patch) supplies the `<exception>`
header, loads an atomic Boolean explicitly instead of copying its atomic
object, and removes legacy Apple linker flags incompatible with lld. The
source and installed headers carry the same patch. The changes permit the
release to compile with OpenMP on this compiler; they do not tune a workload.
Apply it from the extracted `souffle-2.5` directory with
`patch -p1 < /path/to/relic/bench/compare/tools/souffle-2.5-build.patch`.

From the directory containing the extracted sources, choose an installation
prefix and build:

```sh
BENCH_TOOLS="$PWD/installed"
export PATH="/opt/homebrew/opt/lld/bin:$PATH"
cmake -S souffle-2.5 -B souffle-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
    -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
    -DCMAKE_INSTALL_PREFIX="$BENCH_TOOLS/souffle" \
    -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/libomp \
    -DSOUFFLE_GIT=OFF -DGIT_PACKAGE_VERSION=2.5 -DPACKAGE_VERSION=2.5 \
    -DSOUFFLE_USE_OPENMP=ON -DSOUFFLE_ENABLE_TESTING=OFF \
    -DSOUFFLE_USE_CURSES=OFF -DSOUFFLE_USE_SQLITE=OFF \
    -DSOUFFLE_USE_ZLIB=OFF -DSOUFFLE_USE_LIBFFI=OFF
cmake --build souffle-build -j8
cmake --install souffle-build
"$BENCH_TOOLS/souffle/bin/souffle" --version

cmake -S clingo-5.8.0 -B clingo-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$BENCH_TOOLS/clingo" \
    -DCLINGO_BUILD_STATIC=ON -DCLINGO_BUILD_WITH_PYTHON=OFF \
    -DCLINGO_BUILD_WITH_LUA=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build clingo-build -j8
cmake --install clingo-build
```

Confirm Soufflé's version reports `Options enabled: openmp`. Parallel
compiled programs require both translation with `-j16` and C++ compilation
and linking with `-fopenmp`; an OpenMP interpreter alone is insufficient.

Set `RELIC`, `SOUFFLE`, `CLINGO`, `NMO`, `SQLITE` and `CXX` to these native
executables when running the [comparison harness](../run.sh).

## ARM parallel build

The parallel comparison uses a separate Soufflé 2.5 build with the source
changes from [upstream PR 2581](https://github.com/souffle-lang/souffle/pull/2581),
pinned at `ecb775c9d90ac7c12c44f4b2d73985addd36587b`. The
[ARM locking patch](souffle-2.5-arm-locking.patch) has SHA-256
`981c8a13c041dd86cb5882752ad8a28951601be12277ef8ef152e5d01f74afcd`.
Apply it after the compatibility build patch. Use a separate build and
installation directory and set `GIT_PACKAGE_VERSION=2.5-arm-ecb775c`.
The compiler and OpenMP runtime remain Homebrew clang/libomp 23.1.3.

The stock release [documents sporadic ARM concurrency failures](https://github.com/souffle-lang/souffle/releases/tag/2.5).
The chain-left interpreter reproducer fails on its second invocation with
sixteen threads. The proposed patch passes 100 generated-program and 100
interpreter repetitions with exact answer equality, plus the six upstream
relation and locking tests. These checks establish observed benchmark
behavior, not portable C++ race freedom. The proposal remains under upstream
review, and the original failed attempt is preserved with the measurements.

The single-core comparison retains the stock release plus compatibility
patch. Soufflé's build variants therefore differ across thread settings;
their timing differences are not interpreted as speedups caused solely by
thread count. relic uses the same executable for both settings.

## Memory allowances

The harness gives SWI-Prolog a 16 GiB stack and 64 GiB table space. The
scale-2 single-core cspa measurement preserves an initial failure with
16 GiB table space; a retry with the larger allowance is queued separately.

DuckDB uses its default memory limit unless `BENCH_DUCKDB_MEMORY_LIMIT` is
set to a positive integer number of GiB. On this 128 GiB host, the default
limit is 102.3 GiB. Queued scale-4 graph-left retries use
`BENCH_DUCKDB_MEMORY_LIMIT=120` after the default allowance is exhausted.
The original failures are retained, and retry measurements are recorded
separately when they complete.

## Exact tree answer verification

The optional `pairset` verifier compares `tree-sg-all` answers as exact sets of node
pairs, ignoring row order and duplicates. It uses an injective bitmap index,
so equality does not rely on a probabilistic checksum. It rejects malformed
rows and IDs outside the generated tree's domain. Verification runs outside
the measured engine commands and uses all online CPUs, including when the
engine is measured with one thread.

The verifier's regression tests use Python 3 (`brew install python` on macOS).
Build it with the system C compiler and enable it for a benchmark invocation:

```sh
cc -O3 -std=c11 -pthread -o /tmp/relic-pairset bench/compare/pairset.c
BENCH_PAIRSET=/tmp/relic-pairset bench/compare/run.sh -w tree-sg-all,tree-sg-bound
```

For scales up to 8, each bitmap uses at most 2 GiB; comparing two answers
uses two bitmaps. Input files are mapped read-only and must remain unchanged
while verification runs. Their resident pages also contribute to peak RSS.
Other workloads, including the single-column `tree-sg-bound` answer, and tree
scales above 8 use sorted TSV comparison.
