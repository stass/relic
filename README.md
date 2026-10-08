# relic

**A small, lean, fast Datalog engine and C library.** Ask what depends on a
changed package, find a route across a city, or discover who can work on
what next. Give relic the facts and a few rules; let it follow the connections.

relic combines recursive queries, multi-core evaluation and performance
competitive with compiled Datalog on substantial analysis workloads. It
runs your rules directly, with no compilation step, server to manage or
third-party runtime dependencies. Use `relic` from the shell, explore in the
REPL, or bring the same engine into your application through `librelic`.

## What will you ask it?

* **What breaks if I change this?** Follow package, build or service
  dependencies, find cycles, and compute the impact of a change. Try the
  [dependency planner](EXAMPLES.md#what-can-i-work-on-next).
* **How are these things connected?** Explore network reachability, trace
  relationships, or find the fewest stops across a
  [metro map](EXAMPLES.md#fewest-stops-on-the-metro).
* **What does this code do?** Express call-graph reachability or points-to
  analysis as rules over facts extracted from source code. The benchmark
  suite includes points-to inputs from Apache httpd, PostgreSQL and Linux.
* **What is hiding in my tables?** Join CSV and TSV exports, group files by
  directory, or build a [game leaderboard](EXAMPLES.md#data-in-importing-tables)
  with counts, sums and high scores.
* **Who is whose cousin?** Explore a [family tree](EXAMPLES.md#a-family-tree),
  count descendants, or invent a puzzle of your own.

Datalog is a language for stating facts and the rules that follow from
them. Recursion makes it especially useful when the question involves
connections of unknown depth. You describe the relationship; the engine
finds the answers.

## Try it: the impact of a change

Save this as `impact.dl`. Three dependencies and two rules are enough to
follow a change through several layers:

```prolog impact.dl
depends(web, api).
depends(api, database).
depends(worker, api).

affected(App, Changed) :- depends(App, Changed).
affected(App, Changed) :- depends(App, Middle), affected(Middle, Changed).
```

Which applications depend, directly or indirectly, on the database?

```sh
$ relic -s -q 'affected(App, database)?' impact.dl
api
web
worker
```

A lowercase name is a value; an uppercase name is a variable. `:-` means
"if", and a query ends in `?`. `-s` sorts the answers. Add more facts and
the same rules follow the longer chains, including cycles.

Run `relic -i impact.dl` to ask more questions interactively, or write
queries in the program itself. The [example tour](EXAMPLES.md) takes you
from these first rules through negation, arithmetic, aggregates, tables
and the C API, with programs you can run.

## Small engine, serious work

* **Fast recursive evaluation.** Semi-naive evaluation avoids repeating
  work from earlier rounds. Indexed joins and automatic join ordering keep
  lookups focused. Magic Sets specialize evaluation to a query's bindings,
  so a focused question can avoid computing an entire relation.
* **Use your cores.** `-j 0` uses the available processors, up to 256 threads;
  `-j 16` requests sixteen. Large evaluation rounds share their work, while
  small rounds stay on one thread. Answers, their order and diagnostics
  stay the same at every thread count.
* **Easy to carry.** C11, no third-party runtime dependencies, and BSD
  2-Clause licensing. The only optional dependency is `libedit` for REPL
  line editing. A release tarball needs a compiler, `make` and a POSIX shell.
* **Fits your workflow.** Import CSV or TSV, write answers as text, CSV,
  TSV or JSON, and export derived relations for another run. `-e` explains
  the query plan; `-X stats` reports the work the engine does.

## Performance you can inspect

On the benchmark suite's dense cyclic transitive-closure workload
(`tc-cyclic`), a recorded run takes **2.78 s on one thread and 0.45 s on
sixteen: a 6.2× speedup**. These are the fastest of five runs on a busy
macOS Xeon; speedup depends on the workload, and small rounds or loading
may gain little from additional threads.

For context-sensitive points-to analysis over the Graspan data sets,
recorded single-thread runs on a quiet macOS Xeon put relic close to
**compiled Soufflé**, without compiling the Datalog program:

| Input | Base facts | relic | Compiled Soufflé |
| --- | ---: | ---: | ---: |
| Apache httpd | 1.5 million | 210 s | 171 s |
| PostgreSQL | 4.7 million | 190 s | 195 s |
| Linux | 9.5 million | 50 s | 54 s |

These are individual measurements on one machine, with Soufflé compilation
excluded from its run time. They illustrate the engine's capabilities;
rankings vary with the program, input and hardware. The measurements are
recorded in [the benchmark notes](TODO.md#benchmarks), with scaling details
in [the parallel evaluation notes](docs/todo-parallel.md).

The [comparison harness](bench/compare/run.sh) checks answers before timing
relic against Soufflé's interpreted and compiled modes, Nemo, tabled
SWI-Prolog, clingo, DuckDB and SQLite. It reports wall time, peak memory
and compilation time separately. Run your own comparison, or inspect
relic's scaling at several thread counts. Use an installed `relic` or a
binary built with `--disable-shared`, so the harness times the executable
directly:

```sh
# With the comparison tools and selected engines installed:
RELIC=/path/to/relic bench/compare/run.sh -e souffle,nemo -o results.tsv

# Measure relic alone at one, four and sixteen threads:
BENCH_THREADS="1 4 16" bench/run.sh /path/to/relic
```

See [benchmark methodology](specs/design-bench.md) and the
[recorded workload baseline](bench/baseline.txt) for details.

## Reliability is part of the engine

Speed matters when the answers are right. relic checks its optimizations
against a simple reference evaluator, compares serial and parallel
execution, and tests failure paths as well as successful queries.

* **Fuzz the whole path.** Four libFuzzer targets exercise program loading,
  evaluation, table import and regular expressions. The evaluation target
  compares optimized answers against naive evaluation of the whole program,
  catching wrong results as well as crashes.
* **Generate programs, then challenge the optimizations.** The randomized
  suite compares evaluation strategies with indexing, join reordering,
  Magic Sets and parallel execution enabled or disabled. Weekly CI is
  configured for 10,000 program seeds and fifteen minutes per fuzz target
  under AddressSanitizer and UndefinedBehaviorSanitizer.
* **Keep failures reproducible.** The fuzz corpora are replayed by
  `make check`. Dedicated suites run under address, undefined-behavior,
  thread, memory and leak sanitizers where supported.
* **Keep control of execution.** Tuple and evaluation-memory limits stop
  runaway derivations with diagnostics. Programs express computation;
  the runner chooses which data files they can read and write. The memory
  limit covers evaluation storage, not total process memory.

The [testing design](specs/design-testing.md) explains the reference
checks, randomized testing and fuzzing. To exercise them locally:

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

## The same engine in your application

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
