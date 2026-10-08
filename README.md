# relic

relic is a small, high-performance Datalog engine and C library for graph
queries, dependency analysis and relational data processing. It combines
multi-core evaluation with performance competitive with compiled Datalog
on substantial analysis workloads, without third-party runtime dependencies.

The `relic` command-line tool loads and evaluates Datalog programs directly,
with no program compilation step or server required. It supports batch
queries, an interactive REPL and table import and export. The `librelic`
library provides the same engine through a C API.

## Use cases

* **Dependency analysis.** Follow package, build or service dependencies,
  detect cycles and compute the impact of a change. The
  [dependency planner](EXAMPLES.md#what-can-i-work-on-next) demonstrates
  scheduling with prerequisites.
* **Graph queries.** Compute network reachability, trace relationships and
  find paths. The [metro example](EXAMPLES.md#fewest-stops-on-the-metro)
  combines recursion and aggregation to find the fewest stops between stations.
* **Static analysis.** Express call-graph reachability or points-to analysis
  as rules over facts extracted from source code. The benchmark
  suite includes points-to inputs from Apache httpd, PostgreSQL and Linux.
* **Table processing.** Join CSV and TSV exports, group files by directory,
  or compute counts, sums and rankings. A
  [game leaderboard](EXAMPLES.md#data-in-importing-tables) demonstrates
  aggregation over imported tables.
* **Relationship modeling.** Derive indirect relationships from base facts.
  The [family tree](EXAMPLES.md#a-family-tree) covers ancestry, cousins,
  negation and descendant counts.

Datalog expresses computations as facts and rules. Its recursive rules
are useful for relationships of arbitrary depth, such as transitive
dependencies and reachability. relic evaluates those rules and answers
queries over the resulting relations.

## Example: dependency analysis

The following program defines three direct dependencies and derives their
transitive closure. Save it as `impact.dl`:

```prolog impact.dl
depends(web, api).
depends(api, database).
depends(worker, api).

affected(App, Changed) :- depends(App, Changed).
affected(App, Changed) :- depends(App, Middle), affected(Middle, Changed).
```

This query returns applications that depend directly or indirectly on the
database:

```sh
$ relic -s -q 'affected(App, database)?' impact.dl
api
web
worker
```

Lowercase identifiers such as `api` are symbols; uppercase identifiers such
as `App` are variables. `:-` means "if", and a query ends in `?`. `-s` sorts
the answers. The same rules handle longer dependency chains and cycles.

Use `relic -i impact.dl` for interactive queries, or include queries in the
program itself. [EXAMPLES.md](EXAMPLES.md) provides runnable examples of
negation, arithmetic, aggregates, table processing and the C API.

## Features

* **Recursive evaluation.** Semi-naive evaluation avoids repeating
  work from earlier rounds. Indexed joins and automatic join ordering keep
  lookups focused. Magic Sets specialize evaluation to a query's bindings,
  so a focused question can avoid computing an entire relation.
* **Multi-core execution.** `-j 0` uses the available processors, up to 256
  threads; `-j 16` requests sixteen. Large evaluation rounds share their
  work, while small rounds stay on one thread. Answers, their order and
  diagnostics stay the same at every thread count.
* **Minimal dependencies.** C11, no third-party runtime dependencies, and BSD
  2-Clause licensing. The only optional dependency is `libedit` for REPL
  line editing. A release tarball needs a compiler, `make` and a POSIX shell.
* **Data exchange and diagnostics.** Import CSV or TSV, write answers as
  text, CSV, TSV or JSON, and export derived relations for another run.
  `-e` explains the query plan; `-X stats` reports the work the engine does.

## Performance

**relic is fastest on 21 of 25 benchmarks in the recorded full-suite
comparison**, measured by median wall time against Souffle, Nemo, tabled
SWI-Prolog, clingo, DuckDB and SQLite. It runs Datalog directly; compilation
of Souffle programs is excluded from their execution times. Multi-core
execution reaches a recorded **6.2× speedup on sixteen threads** for dense
cyclic transitive closure, from 2.78 s to 0.45 s in a separate scaling run.

The tables collect the available measurements on an Intel Xeon W-3245
(16 cores, 32 hardware threads), macOS 26 and Apple clang 21. They combine
separate runs and include workload-specific follow-ups. The full-suite
ranking is preserved in
[the comparison summary](bench/compare/suite-summary.tsv);
the figures below are available as [TSV](bench/compare/results.tsv). The
`tasks-ready` follow-up uses different inputs from the full-suite summary,
producing 12,360 answer rows rather than 31.

Each populated engine cell lists, from top to bottom:
**wall time (seconds), user CPU time (seconds), peak resident memory (MiB)**.
CPU time measures work across all threads, so it can exceed wall time in a
parallel run. `N/A` means a value was not recorded, the engine does not
support the workload, or the run timed out; the TSV distinguishes these
cases. Memory values from rounded notes are approximate.

### Single-core comparison

Wall times are medians where repeated runs are available, otherwise single
measurements. CPU times are means where available. Bold wall times mark the
lowest recorded value in each row; close results may fall within timing noise.

| Workload | relic | Souffle compiled | Souffle Magic | Souffle interpreter | Nemo | SWI-Prolog | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **0.134**<br>0.125<br>11.7 | 0.277<br>0.257<br>8.5 | N/A | 0.408<br>0.389<br>13.1 | 1.083<br>1.067<br>23.1 | N/A | 0.714<br>0.728<br>42.2 | N/A | N/A |
| `chain-closure` | **0.565**<br>0.533<br>39.4 | 1.211<br>1.129<br>39.7 | 1.479<br>1.376<br>77.4 | 1.398<br>1.328<br>44.0 | 221.611<br>219.641<br>117.4 | 7.393<br>5.207<br>630.7 | 4.450<br>4.026<br>506.5 | 1.688<br>1.528<br>385.3 | 5.566<br>5.557<br>39.0 |
| `chain-left` | **0.004**<br>0.003<br>1.2 | 0.775<br>0.757<br>39.4 | 0.009<br>0.006<br>1.3 | 1.012<br>0.981<br>44.0 | 216.823<br>224.400<br>177.9 | 0.054<br>0.044<br>7.0 | 2.305<br>2.189<br>355.8 | 1.227<br>1.101<br>191.8 | 6.042<br>6.107<br>40.0 |
| `chain-right` | **0.683**<br>0.649<br>43.4 | 0.932<br>0.904<br>39.9 | 1.077<br>1.051<br>39.7 | 1.198<br>1.160<br>44.1 | 214.463<br>213.884<br>112.4 | 0.908<br>0.756<br>277.6 | 2.691<br>2.505<br>351.2 | 2.001<br>1.288<br>220.8 | 6.340<br>6.184<br>39.9 |
| `cspa` | **0.230**<br>0.224<br>11.1 | 0.524<br>0.516<br>10.0 | N/A | 0.846<br>0.828<br>15.0 | 1.671<br>1.643<br>45.5 | N/A | 1.116<br>1.090<br>56.0 | N/A | N/A |
| `cspa-httpd` | 209.890<br>196.470<br>5934.2 | **171.000**<br>N/A<br>~5245 | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| `cspa-linux` | **49.549**<br>47.386<br>1956.8 | 53.935<br>52.748<br>1644.8 | N/A | N/A | 119.063<br>114.675<br>3525.4 | N/A | N/A | N/A | N/A |
| `cspa-postgresql` | **189.890**<br>185.180<br>6329.6 | 195.000<br>N/A<br>~6866 | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| `degree-count` | **0.096**<br>0.088<br>13.4 | 0.100<br>0.093<br>9.1 | 0.100<br>0.093<br>9.1 | 0.195<br>0.182<br>13.8 | 0.397<br>0.389<br>16.0 | 1.681<br>1.515<br>38.2 | 0.947<br>0.919<br>61.9 | 0.178<br>0.164<br>32.1 | 0.310<br>0.302<br>10.9 |
| `dist-bounded` | **0.471**<br>0.453<br>27.5 | 2.189<br>1.805<br>97.9 | 1.672<br>1.532<br>100.5 | 2.819<br>2.204<br>102.4 | 7.468<br>5.405<br>242.9 | 9.377<br>7.117<br>383.7 | 3.701<br>3.544<br>375.0 | 2.064<br>1.722<br>188.5 | 4.695<br>4.634<br>36.1 |
| `facts-join` | **0.190**<br>0.172<br>27.9 | 0.392<br>0.359<br>43.2 | 0.356<br>0.358<br>43.2 | 0.440<br>0.403<br>47.4 | 1.138<br>1.122<br>63.1 | 4.825<br>4.416<br>114.9 | 2.646<br>2.573<br>83.3 | 0.227<br>0.209<br>35.8 | 0.397<br>0.394<br>9.3 |
| `files-grouped` | 0.314<br>0.291<br>32.4 | 1.234<br>1.211<br>42.4 | 2.343<br>2.336<br>48.5 | 2.159<br>2.133<br>47.2 | 2.457<br>2.443<br>52.2 | 4.858<br>4.446<br>118.0 | N/A | **0.198**<br>0.181<br>34.7 | 0.625<br>0.594<br>19.3 |
| `graph-left` | **0.054**<br>0.049<br>7.4 | N/A | 0.104<br>0.099<br>6.8 | N/A | N/A | 1.694<br>1.540<br>32.4 | N/A | N/A | N/A |
| `graph-reach` | **0.053**<br>0.048<br>8.7 | 0.069<br>0.064<br>6.1 | 0.073<br>0.079<br>6.4 | 0.162<br>0.148<br>10.7 | 0.260<br>0.252<br>11.7 | 1.625<br>1.539<br>35.7 | 0.712<br>0.726<br>29.1 | 0.199<br>0.186<br>25.7 | 0.288<br>0.309<br>8.5 |
| `grid-reach` | 0.040<br>0.033<br>7.4 | **0.034**<br>0.029<br>4.3 | **0.034**<br>0.029<br>4.3 | 0.184<br>0.148<br>8.9 | 0.274<br>0.264<br>11.1 | 0.772<br>0.732<br>23.3 | 0.328<br>0.317<br>18.8 | 0.635<br>0.609<br>38.9 | 0.135<br>0.149<br>5.5 |
| `sg-acyclic` | **0.406**<br>0.398<br>12.4 | 1.037<br>1.113<br>24.6 | 1.526<br>1.499<br>28.2 | 1.232<br>1.228<br>29.0 | 4.590<br>4.463<br>155.8 | 4.241<br>3.792<br>139.5 | 3.540<br>3.472<br>133.2 | 0.716<br>0.672<br>149.2 | 3.637<br>3.624<br>12.3 |
| `sg-acyclic-bf` | **0.008**<br>0.004<br>1.3 | 1.007<br>1.005<br>24.7 | 0.010<br>0.007<br>1.4 | 1.236<br>1.205<br>29.2 | 4.710<br>4.525<br>195.5 | 0.131<br>0.113<br>7.8 | 2.823<br>2.853<br>101.6 | 0.651<br>0.676<br>134.6 | 3.743<br>3.699<br>13.1 |
| `sg-cyclic` | 3.162<br>2.895<br>21.5 | 3.465<br>3.516<br>50.1 | 5.805<br>5.753<br>54.7 | 4.091<br>4.041<br>54.7 | 21.430<br>20.878<br>599.1 | 13.475<br>12.656<br>271.7 | 11.811<br>11.778<br>232.4 | **2.330**<br>2.168<br>379.5 | 13.284<br>13.177<br>24.5 |
| `startup` | **0.003**<br>0.001<br>1.0 | 0.007<br>0.005<br>1.1 | 0.007<br>0.005<br>1.1 | 0.093<br>0.082<br>5.0 | 0.009<br>0.005<br>5.4 | 0.018<br>0.013<br>5.9 | 0.011<br>0.007<br>4.5 | 0.099<br>0.091<br>17.6 | 0.005<br>0.002<br>2.1 |
| `tasks-ready` | **0.088**<br>0.079<br>9.2 | 0.385<br>0.167<br>10.2 | N/A | N/A | N/A | N/A | N/A | 0.319<br>0.246<br>47.8 | 107.255<br>99.944<br>10.2 |
| `tc-acyclic` | **0.652**<br>N/A<br>N/A | 0.681<br>N/A<br>N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| `tc-acyclic-bf` | **0.080**<br>N/A<br>N/A | N/A | 0.146<br>N/A<br>N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| `tc-acyclic-fb` | **0.016**<br>N/A<br>N/A | N/A | 0.042<br>N/A<br>N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| `tc-cyclic` | 2.238<br>2.239<br>22.5 | **1.906**<br>1.872<br>50.1 | 4.868<br>4.836<br>52.2 | 3.245<br>3.222<br>54.8 | N/A | N/A | N/A | 3.345<br>3.280<br>165.9 | 27.723<br>27.729<br>27.2 |
| `tc-cyclic-bf` | 4.103<br>3.456<br>27.6 | 1.851<br>1.826<br>50.2 | **1.805**<br>1.781<br>50.2 | 3.171<br>3.127<br>54.9 | N/A | N/A | N/A | 3.102<br>3.047<br>132.2 | 27.558<br>27.561<br>27.2 |
| `tc-cyclic-fb` | **0.017**<br>N/A<br>N/A | N/A | 0.045<br>N/A<br>N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| `tc-nonlinear` | **0.143**<br>N/A<br>N/A | 0.279<br>N/A<br>N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| `tc-sparse-acyclic` | **0.039**<br>N/A<br>N/A | 0.075<br>N/A<br>N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| `tc-sparse-cyclic` | **0.376**<br>0.366<br>21.5 | 0.556<br>0.527<br>39.9 | 0.867<br>0.832<br>44.9 | 0.821<br>0.786<br>45.2 | N/A | N/A | N/A | 0.766<br>0.704<br>153.4 | 4.284<br>4.281<br>19.3 |
| `tree-sg-all` | **0.039**<br>N/A<br>N/A | 0.050<br>N/A<br>N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| `tree-sg-bound` | **0.008**<br>N/A<br>N/A | N/A | 0.011<br>N/A<br>N/A | N/A | N/A | N/A | N/A | N/A | N/A |

Souffle columns distinguish compiled execution, compiled execution with
Magic Sets, and its interpreter. The recorded versions are Souffle 2.5,
Nemo 0.10.1, SWI-Prolog 10.0.2, clingo 5.8.2, DuckDB 1.5.5 and SQLite 3.51.0.

### Multi-core execution: sixteen threads

The same wall / CPU / memory layout applies. The first three rows are
medians of seven runs from one comparison on a busy machine. Souffle's
compiled programs are translated for parallel execution and built with
OpenMP. The points-to rows are individual runs; `dist-computed` is the
fastest of five runs and evaluates integer expressions over the cyclic graph.
The single-core and multi-core tables combine different measurement sessions,
so their cells should not be divided to calculate scaling.

| Workload | relic | Souffle compiled | Souffle interpreter |
| --- | ---: | ---: | ---: |
| `chain-closure` | **0.810**<br>N/A<br>N/A | 2.120<br>N/A<br>N/A | 2.410<br>N/A<br>N/A |
| `tc-cyclic` | 0.970<br>N/A<br>N/A | **0.760**<br>N/A<br>N/A | 1.290<br>N/A<br>N/A |
| `cspa` | **0.130**<br>N/A<br>N/A | 0.280<br>N/A<br>N/A | 0.450<br>N/A<br>N/A |
| `dist-computed` | 1.640<br>N/A<br>N/A | N/A | N/A |
| `cspa-httpd` | N/A | N/A | N/A |
| `cspa-postgresql` | 55.090<br>491.850<br>8998.7 | N/A | N/A |
| `cspa-linux` | 19.910<br>93.240<br>2322.7 | N/A | N/A |

Additional one- and sixteen-thread results for relic's generated workloads
are in [the workload baseline](bench/baseline.txt). Scaling depends on the
workload: small rounds stay on one thread, and loading is serial.

The [comparison harness](bench/compare/run.sh) checks answers before timing
and reports wall time, CPU time, peak memory and compilation time. To compare
engines or measure relic's scaling, use an installed executable or a binary
built with `--disable-shared`:

```sh
# With the comparison tools and selected engines installed:
RELIC=/path/to/relic bench/compare/run.sh -e souffle,nemo -o results.tsv

# Measure relic alone at one, four and sixteen threads:
BENCH_THREADS="1 4 16" bench/run.sh /path/to/relic
```

See [benchmark methodology](specs/design-bench.md) for the workloads,
measurement procedure and reproducibility requirements.

## Reliability and testing

relic checks its optimizations against a simple reference evaluator,
compares serial and parallel execution, and tests failure paths as well
as successful queries.

* **Differential fuzzing.** Four libFuzzer targets exercise program loading,
  evaluation, table import and regular expressions. The evaluation target
  compares optimized answers against naive evaluation of the whole program,
  catching wrong results as well as crashes.
* **Randomized testing.** The randomized suite compares evaluation
  strategies with indexing, join reordering,
  Magic Sets and parallel execution enabled or disabled. Weekly CI is
  configured for 10,000 program seeds and fifteen minutes per fuzz target
  under AddressSanitizer and UndefinedBehaviorSanitizer.
* **Regression and sanitizer suites.** The fuzz corpora are replayed by
  `make check`. Dedicated suites run under address, undefined-behavior,
  thread, memory and leak sanitizers where supported.
* **Resource limits and file access.** Tuple and evaluation-memory limits stop
  runaway derivations with diagnostics. Programs express computation;
  the runner chooses which data files they can read and write. The memory
  limit covers evaluation storage, not total process memory.

The [testing design](specs/design-testing.md) explains the reference
checks, randomized testing and fuzzing. Run them locally with:

```sh
make check
make check-random RELIC_SEEDS=10000

# In a separate build configured with clang and
# --enable-fuzzers --enable-asan --enable-ubsan:
make fuzz FUZZ_SECONDS=900
```

## Building

From a release tarball, nothing beyond a C11 compiler, `make` and a POSIX
shell is required:

```sh
./configure
make
make check
make install
```

Out-of-tree builds are supported and preferred:

```sh
mkdir build && cd build
../configure && make && make check
```

From a git checkout, regenerate the build system first. This needs autoconf
2.69, automake 1.13 and libtool 2.4.2 or newer, which are maintainer tools
only:

```sh
./autogen.sh
```

<details>
<summary>Build options</summary>

```text
--enable-debug          assertions, -O0 -g3                    (default: no)
--enable-werror         treat warnings as errors               (default: no)
--disable-threads       evaluate on the calling thread alone   (default: threads)
--enable-asan           AddressSanitizer                       (default: no)
--enable-ubsan          UndefinedBehaviorSanitizer             (default: no)
--enable-lsan           LeakSanitizer, where it stands alone   (default: no)
--enable-msan           MemorySanitizer, clang on Linux        (default: no)
--enable-tsan           ThreadSanitizer                        (default: no)
--enable-coverage       gcov or llvm-cov instrumentation       (default: no)
--enable-fuzzers        build tests/fuzz, clang only           (default: no)
--with-libedit=auto|yes|no
                        line editing and history in the REPL   (default: auto)
```

The standard autotools options apply unchanged, including `--prefix`,
`--program-prefix`, `--disable-shared`, `--disable-static` and `DESTDIR`
staged installation.

</details>

<details>
<summary>Sanitizer suites and strict checks</summary>

```sh
make check          the unit and language suites
make check-asan     reconfigure with AddressSanitizer, rebuild, run the suite
make check-lsan     the same under LeakSanitizer, where it stands alone
make check-msan     the same under MemorySanitizer, where it is supported
make check-ubsan    the same under UndefinedBehaviorSanitizer
make check-tsan     the same under ThreadSanitizer, with threads enabled
```

Each sanitizer suite configures a disposable build tree under `disposable/`
in the current build directory, from a copy of the distributed sources, so it
never leaves instrumented objects in the tree you work in and works whether
that tree was configured in place or not. A sanitizer the platform does not
support reports that and succeeds rather than failing the build.

A few tests need a tool the build does not, such as `mandoc`, `nm` or
`pkg-config`, and are skipped without it. On a machine set up to run every
check, set `RELIC_STRICT_CHECK=yes` in the environment: a skipped test then
fails, and so do a sanitizer suite the compiler cannot build and an
`installcheck` without `pkg-config`. A skip that is expected there is named
in `RELIC_ALLOWED_SKIPS`, a space-separated list of test file names and
targets:

```sh
RELIC_STRICT_CHECK=yes RELIC_ALLOWED_SKIPS=symbols.sh make distcheck
```

</details>

## Data files

Import data with `-I` and declare column types with `-T`. For example,
`edge.tsv` can hold these two tab-separated rows:

```text edge.tsv
a	b
b	c
```

Save the rules and query as `reach.dl`:

```prolog reach.dl
% What a can reach, over edges given as a table.
path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).
path(a, X)?
```

The file name `edge.tsv` supplies the predicate name and input format:

```sh
$ relic -T edge:symbol,symbol -I edge.tsv reach.dl
b
c
```

Programs contain facts, rules and queries; the command line chooses the
files. Loading someone else's program gives it no file access of its own.

`-F` chooses the answer format, and `-o` writes answers to a file. `-O`
exports a whole relation after the queries have run: `-O path.tsv` writes
a table for `-T` and `-I`, and `-O path.dl` writes typed Datalog facts that
another run can load:

```sh
$ relic -n -T edge:symbol,symbol -I edge.tsv -O path.dl reach.dl && relic -q 'path(a, X)?' path.dl
b
c
```

See the [table examples](EXAMPLES.md#data-in-importing-tables) and
[relic(1)](man/relic.1) for headers, column selection and explicit predicate
names.

## C library

`librelic` installs a single public header, `<relic.h>`, and a pkg-config
module, `librelic`. [examples/query.c](examples/query.c) loads a program,
answers a query and prints the rows using only the public API:

```sh
cc -o query examples/query.c $(pkg-config --cflags --libs librelic)
./query
```

Only symbols matching `relic_` are exported. `relic(3)` describes the
interface.

## Limitations

A query that binds an argument is answered through a Magic Sets rewrite,
which derives only what the query can reach. A recursive rule that calls
its own predicate twice, with the bound argument passed to the first call,
costs that rewrite a second relation as large as the closure it derives. Of
these two definitions of a path, called as `path(a, X)?`:

```prolog
path(X, Z) :- path(X, Y), path(Y, Z).
path(X, Z) :- path(X, Y), edge(Y, Z).
```

the first keeps a second relation holding a copy of every path the rewrite
derives, and the second does not. Both give the same answers, and the copy
counts against the memory limit like any other relation. `relic -e` shows
the copy as a predicate named `sup$...`. Where memory matters, prefer the
linear form.

## Documentation

| File | Contents |
| --- | --- |
| [EXAMPLES.md](EXAMPLES.md) | a tour of relic through programs you can run |
| [man/relic.1](man/relic.1) | the tool and the language |
| [man/relic.3](man/relic.3) | the library |
| [specs/README.md](specs/README.md) | the design documents, where to start reading |
| [specs/architecture.md](specs/architecture.md) | the target design of the system |
| [specs/language.md](specs/language.md) | the specification of the language |
| [specs/style.md](specs/style.md) | code style, binding on all relic-owned code |
| [NEWS](NEWS) | what changed in each release |
| [TODO.md](TODO.md) | open work, by severity |

## License

BSD 2-Clause. See [LICENSE](LICENSE).
