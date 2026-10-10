# Benchmarks

How relic's performance is measured: `bench/run.sh`, an instrument that runs
a fixed set of generated workloads through relic and reports its work
counters beside the time, and `bench/compare/run.sh`, a harness that runs
one set of workloads through relic, other Datalog and tabled-Prolog engines
and SQL databases, checks that every engine gives the same answer, and times
them. Both are engineering instruments outside the system: neither is run
by `make check`, and nothing fails on a time.

Related: [performance targets](architecture.md#performance-targets),
[testing](design-testing.md), [parallel evaluation](design-parallel.md),
[tables](design-tables.md).

## Model

Two instruments answer two questions, over one set of data generators,
`bench/datasets.sh`, so both measure the same data.

* **Is relic doing more or less work than it did?** `bench/run.sh` runs each
  workload once and reports wall-clock seconds with the counters `-X stats`
  gives: tuples derived, held, scanned and looked up. The counters are exact
  and machine-independent; the seconds are not. It is the instrument for
  looking closer before a change meant to be faster, after one that might be
  slower, or when `tests/perf.sh` fails.
* **Is relic's absolute performance reasonable?** `bench/compare/run.sh`
  answers per workload: where relic is within a small factor of a mature
  engine, where it is an order of magnitude away, and whether the gap grows
  with the input, which points at an algorithm rather than a constant. A gap
  that grows with scale is a design problem to record in `TODO.md`.

Timing is machine-dependent and noisy, so the test that bounds performance
is `tests/perf.sh`, which checks counters, not the clock (see
[testing](design-testing.md)). The comparison fails only on an answer.

`bench/run.sh`, `bench/datasets.sh` and `bench/baseline.txt` ship in the
release tarball. The comparison harness source and bitmap verifier are
included for regression tests; running the engine comparison requires the
workload programs from the repository and the other engines installed.

## The relic instrument

`bench/run.sh` generates each data set with `awk`, writes it as facts
followed by the workload's rules, and runs it through a `relic` named on the
command line, by `RELIC`, or found on the `PATH`. The workloads are a long
chain, a random graph, a grid, a same-generation tree, a negation-heavy task
graph, a large fact file and a set of file paths:

* the chain's closure from one node through a right-recursive and a
  left-recursive rule, and the whole closure without the rewrite. With the
  first argument bound, the right-recursive form makes the Magic rewrite
  demand every node the query reaches and the left-recursive form does not,
  so the pair shows what the order of a recursive body costs;
* the chain's closure with each path's length computed as it is derived,
  so a new integer is interned for every length;
* reachability and a left-recursive closure over the graph, and each node's
  out- and in-degree, two `count` aggregates per node;
* reachability over the grid, same generation over the tree bound and free;
* the tasks that are ready, through stratified negation;
* a join over the fact file, which is mostly loading;
* the file paths that are not tests, grouped and counted by directory and
  extension: a string computed and a pattern matched per path, then groups
  of strings.

`BENCH_SCALE` multiplies every size (a tree deepens one level, doubling, per
step), so the same workloads trace a curve as the input grows.
`BENCH_THREADS` lists thread counts, each passed as `-j`, and each workload
runs at each count in a row of its own, so `"1 16"` shows what sixteen
threads gain it. The counters should be identical at every thread count;
the script prints them side by side and leaves the comparison to the eye
(see [parallel evaluation](design-parallel.md)).

`bench/baseline.txt` records one machine's run, naming its processor,
operating system, compiler and load, for comparison by eye: the counters
exactly, the seconds by order of magnitude.

## Engines

| Engine | Why | Modes |
| --- | --- | --- |
| relic | the subject | the tool as a user runs it: the query in the program, answered through the Magic rewrite where it applies |
| SWI-Prolog | the Prolog most users have; tabling makes it a goal-directed Datalog engine | tabled predicates |
| Souffle | the reference high-performance Datalog engine, bottom-up and compiled | `souffle-interp`, the interpreter; `souffle`, the program compiled to C++; `souffle-magic`, compiled with `--magic-transform=*` |
| clingo | the most widely used answer-set system; for a stratified program its grounder is a complete bottom-up Datalog engine | the default solver |
| Nemo | a modern in-memory Datalog engine built for large data | the default |
| DuckDB | what a user who knows SQL reaches for: an in-process analytical database with recursive common table expressions | an in-memory database |
| SQLite | the most widely deployed SQL database | an in-memory database |

Souffle appears three times because its modes answer different questions.
The interpreter is the fair comparison with relic's interpreter; the
compiled program is the best Souffle can do; the magic transform isolates
what relic's rewrite buys over the plain bottom-up evaluation Souffle
performs by default. The harness builds the compiled modes itself: it asks
Souffle for the C++ program and compiles it with the system C++ compiler at
`-O3`, defining the number width Souffle reports it was built with, so the
compiler and its flags are chosen and recorded by the harness rather than
inherited from a package. The definitions for SQLite, zlib and ncurses
input and output are left out because no workload uses them. Parallel runs
translate programs with the requested thread count and compile and link
with OpenMP; a Souffle build without OpenMP is refused for these runs. Compile time is reported in its own column
and is not part of the run time.

**The SQL engines.** A recursive common table expression is SQL's Datalog: a
rule set becomes one `WITH RECURSIVE` expression per recursive relation, its
base and recursive rules joined by `UNION`, which gives set semantics, and a
stratified negation becomes `NOT EXISTS`. Each relation is a table loaded
with the engine's own bulk loader, with no index created: the engines build
what their joins need. The query is the final `SELECT`, its constants in its
`WHERE` clause. Moving a constant into the expression's base case would be
the Magic rewrite done by hand, which no Datalog engine here is given; that
neither DuckDB nor SQLite pushes the constant into a recursive expression
itself is a measurement of them.

**Threads.** `BENCH_THREADS` sets one thread count per comparison run,
from 1 to 256. relic receives `-j`, Souffle receives `-j` during translation
and execution, and DuckDB receives `SET threads`. The default is one thread.
SWI-Prolog, clingo's Datalog grounding, Nemo and SQLite retain their serial
execution; each result records the configured count the engine supports,
rather than labeling a serial engine as parallel. A parallel comparison can
select relic, Souffle's three modes and DuckDB with `-e`; the single-core
run includes the serial engines. Souffle's compiled parallel modes use
`BENCH_OPENMP_FLAGS`, default `-fopenmp`, with the selected C++ compiler.
These are trusted compiler flags, with the same shell interpretation as
the harness's build command.

**No tuning beyond the documented switches.** Each engine runs as its manual
suggests for this kind of program. Where an engine needs a limit raised to
finish at all -- SWI-Prolog's stack and table space -- the harness raises it
and records the setting. No engine gets indexing hints, modes or pragmas the
others do not.

**Engines left out.** An engine joins only if it is freely installable and runs
locally from a command line over the same facts and program, on one thread,
with only its documented switches. That excludes DDlog (archived, and built to
maintain a result across a stream of changes, which no workload measures);
Scryer and Trealla Prolog (tabling not mature enough to be a fair
representative); XSB (SWI-Prolog's tabling descends from it and is the one
users have); embedded Rust engines such as Crepe and Ascent, which compile each
program into a binary and measure a different use; PostgreSQL, whose recursive
expressions are those of SQLite and DuckDB behind a server; LogicBlox, a
proprietary product that cannot be run locally; CodeQL, whose evaluator is
closed and reads databases its extractors build from source code; and Datomic,
a query language over its own store rather than an engine over a program's
facts.

## Workloads

### The admission rule

A workload joins the set only if relic accepts it, and it is expressed natively
in each engine whose language can say it. An engine whose language cannot --
clingo has no string functions beyond those a script defines -- is named in the
workload's `absent` file with the reason, and is reported `n/a` there. Nothing
is translated around a missing feature, for any engine's benefit, relic's
included. Within that rule, workloads use whatever the engines share: integer
arithmetic (`dist-bounded`), aggregates (`degree-count`, `files-grouped`),
strings and pattern matching (`files-grouped`), and stratified negation
(`tasks-ready`). An engine's program may take a different but native route to
the same answer: Souffle, which has no function giving a position, finds a
path's extension by trying each extension the data lists.

The SQL engines are a baseline outside the rule. SQL allows a recursive
expression only linear recursion, and DuckDB and SQLite allow no recursion
through another expression either, so the nonlinear closure and the
points-to analyses cannot be written for them and have no SQL program;
the report shows `n/a`. DuckDB accepts a nonlinear expression but answers it
incompletely, since each recursive reference sees only the previous
iteration's rows.

Every workload is a data set, a set of rules and one query. The query is
either bound -- it names a constant, and a goal-directed engine may explore
only what it reaches -- or free, when the whole relation is the answer. Both
kinds are in the set because they favour different designs: relic, Souffle
with the magic transform and SWI-Prolog's tabling are goal-directed;
Souffle, clingo and Nemo evaluate bottom-up by default, as the SQL engines
evaluate a recursive expression.

### Set A: the relic workloads

The data sets of `bench/run.sh`, queried as a user would query them:

| Workload | Data set | Query |
| --- | --- | --- |
| `startup` | one fact | the fact; process start-up, loading and output |
| `chain-right` | a chain of 2000 edges | bound; right-recursive closure |
| `chain-left` | the chain | bound; left-recursive closure |
| `chain-closure` | the chain | free; the whole closure, two million tuples |
| `graph-reach` | 20000 nodes, 100000 edges | reachability from one node |
| `graph-left` | the graph | bound; left-recursive closure from one node |
| `degree-count` | the graph | free; each node's out- and in-degree, two `count` aggregates |
| `grid-reach` | a 150 by 150 grid | reachability from a corner |
| `tree-sg-bound` | a complete binary tree of depth 12 | bound; same generation |
| `tree-sg-all` | a tree of depth 9 | free; same generation |
| `tasks-ready` | 50000 tasks and their dependencies, half done | stratified negation |
| `facts-join` | 300000 ternary facts | a join with integer constants; mostly loading |
| `files-grouped` | 300000 file paths | free; non-test files counted per directory and extension: string functions, a pattern and `count` |

The sizes are those at a `BENCH_SCALE` of one, which multiplies every one.

### Set B: workloads others will recognize

* The transitive-closure and same-generation programs of OpenRuleBench
  (Liang, Fodor, Wan and Kifer, 2009), with bound and free queries, on
  acyclic and cyclic graphs at two densities; the suite is the usual
  comparison between Prolog and Datalog engines. Its programs that need
  recursion through negation fail the admission rule and are omitted.
* The nonlinear transitive closure, whose recursive rule calls the closure
  twice, exercising a rule with more than one recursive literal.
* Context-sensitive points-to analysis (CSPA) over the Graspan data sets,
  and an Andersen-style points-to analysis: realistic program-analysis
  workloads used by several recent Datalog papers.

The programs follow rbench, Stefan Brass's collection of logic-programming
benchmarks, which writes OpenRuleBench's programs with same generation
starting from `sg(Y, Y) :- par(_, Y)`. The graphs are OpenRuleBench's:
random, over integer nodes, as `par(From, To)`. The dense ones draw 50000
edges among 1000 nodes, the size of OpenRuleBench's `tc_d1000_parsize50000`;
the sparse ones 5000. An acyclic graph points every edge forward in a fixed
order of its nodes that puts node 500, the node the bound queries name, in
the middle at every scale, so it has ancestors and descendants both.
OpenRuleBench's query modes keep their names: `bf` binds the first argument,
`fb` the second.

| Workload | Data set | Query |
| --- | --- | --- |
| `tc-cyclic` | dense, cyclic | free; every pair of nodes |
| `tc-cyclic-bf` | dense, cyclic | `tc(500, X)`; right recursion carries the binding to every node reached |
| `tc-cyclic-fb` | dense, cyclic | `tc(X, 500)`; the binding stays on the one node |
| `tc-acyclic` | dense, acyclic | free |
| `tc-acyclic-bf` | dense, acyclic | `tc(500, X)` |
| `tc-acyclic-fb` | dense, acyclic | `tc(X, 500)` |
| `tc-sparse-cyclic` | sparse, cyclic | free |
| `tc-sparse-acyclic` | sparse, acyclic | free |
| `tc-nonlinear` | sparse, acyclic | free; `tc(X, Z) :- tc(X, Y), tc(Y, Z)` |
| `sg-cyclic` | sparse, cyclic | free; same generation |
| `sg-acyclic` | sparse, acyclic | free |
| `sg-acyclic-bf` | sparse, acyclic | `sg(500, X)` |
| `dist-bounded` | sparse, cyclic | free; `dist(X, Y, D)`, every walk length up to 5, computed by `E = D + 1` |
| `andersen` | a generated program of 2000 functions | free; every points-to fact |
| `cspa` | a generated program of 1000 functions | free; the value-flow relation, which needs the other two |
| `cspa-httpd` | Graspan's Apache httpd 2.2.18, 1.5 million facts | free; as `cspa` |
| `cspa-postgresql` | Graspan's PostgreSQL 8.3.9, 4.7 million facts | free; as `cspa` |
| `cspa-linux` | Graspan's Linux 4.4.0-rc5, 9.5 million facts | free; as `cspa` |

The nonlinear closure runs on the sparse acyclic graph only: its cost is
cubic in the nodes a cycle joins, and on a cyclic graph of this size it runs
for minutes in every engine.

The points-to analyses also run on generated programs, so they scale and
need nothing downloaded. A generated program is functions of twenty
variables whose statements relate variables of the same function, joined by
a binary call tree down which arguments are passed. Statements drawn
uniformly across the program would not do: past a threshold density they
join nearly every variable into one alias set, and the answer jumps from
linear to quadratic in the program's size at a point that depends on the
draws, so a scale sweep would measure the draws. The Graspan data sets are
the real-world case. The three share one set of programs, except that
Souffle's declares `number` columns where the generated one declares
`symbol`: the Graspan identifiers are integers, and as symbols they would
cost Souffle a symbol table the papers it is compared with do not pay for.

### Programs

Each workload's rules are written by hand, once per engine, in that
engine's own syntax, and kept side by side in the workload's directory. A
translator from one dialect to the others would be a second compiler to
trust; short programs are easier to read and to check against each other.
relic's program ends with the query, as a relic user writes it. Every other
program writes the query's answer -- its columns the query's named variables
in order of first occurrence, the columns relic prints -- as its engine
writes a result: SWI-Prolog's defines `answer/N`, which a shared driver
prints; Souffle's, Nemo's and clingo's output a relation to a file or to
standard output; the SQL programs end with the query. When the query is a
whole relation, the bottom-up programs output that relation itself rather
than copying it, which relic and SWI-Prolog would not pay for.

A workload's directory is found by the harness through three files:

* `dataset` names the data set, and marks the directory as a workload, so
  adding a workload is adding a directory;
* `programs`, in place of the programs, names the directory whose programs
  the workload runs, for workloads that differ only in their data (the
  three Graspan workloads share `cspa-graspan/`);
* `absent` lists, one per line, an engine the workload has no program for,
  followed by why that engine's language cannot say what the workload asks.
  It lives beside the programs, so workloads sharing programs share it. An
  engine missing a program and not named there is a harness failure, not a
  silent `n/a`.

## Data

A data set is generated once per run into a directory of relation files: one
`<relation>.tsv` per relation, one tuple per line, columns separated by
tabs, every value written as its text. That is the form Souffle, Nemo,
DuckDB and SQLite read directly. For the engines that read facts as source
-- SWI-Prolog and clingo -- the harness writes one file of facts, spelling a
value as an integer, a symbol, or else a quoted string; no data set writes a
string holding a quote or a backslash, so the quoting rules the engines
disagree on never arise. Every relation file is a set: a generator may draw
a row twice and the repeat is removed, since an engine that keeps facts as
clauses, as SWI-Prolog does, would use a repeated fact twice where the
others use it once.

Each engine loads the data through its own native path, because that is the
path its users take and its authors optimized:

| Engine | Loads |
| --- | --- |
| Souffle | the relation files, through `.input` |
| Nemo | the relation files, through `@import` |
| relic | the relation files, through `-I` |
| SWI-Prolog | one source file of facts, consulted |
| clingo | one source file of facts |
| DuckDB | the relation files, through `COPY` |
| SQLite | the relation files, through `.import` |

relic imports each relation file as a `tsv` table, typing each column from
the file's first row: decimal digits make an `integer` column, a symbol's
spelling a `symbol` column, and anything else a `string` column (see
[tables](design-tables.md)). The `facts-join` workload is mostly loading, so
it isolates what each engine's native path costs.

The generators draw from a fixed linear congruential sequence, so a data set
is identical on every run and platform. A draw takes the sequence's high
bits, because the low bits of a power-of-two linear congruential sequence
cycle with short periods and would correlate consecutive draws.

## A run

For each workload relic runs first, since its answer is the reference, then
every other engine, each twice over:

1. **The verification run**, once, under a time limit (`BENCH_TIMEOUT`). Its
   answer is written to a file and checked (see [Answers](#answers)). An
   engine that exceeds the limit is reported `timeout`, one that exits
   unsuccessfully `failed`, one whose answer differs `mismatch`; none of
   them is timed. The verification run also warms the file cache.
2. **The timed runs**, `BENCH_RUNS` of them, by `hyperfine`, with the
   engine's standard output discarded. Each is the whole process:
   start-up, loading, evaluation and writing the answer.
   Every engine serializes its complete answer to discarded standard output.
   Souffle uses `-D -`; Nemo exports TSV to the empty resource through its
   defaulted `answer_resource` parameter. Verification uses the same query
   and format, with file exports for these engines. Command construction
   requires an explicit verification or timing phase. Disabling answer
   serialization is not an equivalent timing mode.

`bench/compare/sweep.sh` runs scales 1, 2, 4 and 8 sequentially, at one
thread and a selected parallel thread count. Scale 1 uses ten repetitions
and includes the three fixed Graspan datasets; larger scales use three
repetitions and generated data only. Each sweep requires a fresh results
directory, executes an isolated source snapshot, and records its source
revision, source archive and SHA-256 checksums, host, output policy,
commands, raw samples and per-scenario exit codes. An interrupted scenario is marked
before it starts and loses its marker only after the harness returns.
The runner stops on usage errors and preserves engine failures as outcomes.
It never resumes or overwrites an existing cohort. File-output measurements
and stdout-output measurements are kept as separate cohorts.

Souffle's compiled modes are compiled before the verification run; the
compilation is timed once and reported in its own column. An engine with no
program for a workload is reported `n/a` and not run.

A failing engine is recorded and the harness continues; it exits
unsuccessfully at the end if any engine failed or disagreed, so a mismatch
is never lost in a long table. A timeout is reported but does not fail the
run: it is a measurement of the engine, and some engines exceed any
reasonable limit on the deep recursions, which would otherwise leave the
exit status unable to signal a mismatch.

## Answers

The verification run's output is normalized to rows of tab-separated columns
-- relic's `-F tsv`, SWI-Prolog's rows as the driver prints them, Souffle's
and Nemo's exported files, clingo's atoms taken apart, the SQL engines' rows
printed with tabs. The distinct rows must equal relic's reference set exactly,
regardless of order or duplicates. The general verifier sorts and removes
duplicates, then compares the normalized files byte for byte. For tree answers,
a parallel bitmap verifier can compare canonical pairs of `t<number>` symbols
directly. Each pair has an injective bit index within the configured node
domain; membership and distinct cardinality establish exact equality, without
hash collisions. Malformed or out-of-domain rows fail verification. The
verifier uses all available CPUs independently of the engine's timed thread
count, and verification is excluded from reported engine timings.
An engine that leaves no answer file has failed, not
answered nothing.

Every workload has a non-empty answer, and the harness fails a workload
whose reference is empty: engines agreeing on no rows check nothing, and
would let any program that exits successfully pass.

A disagreement is a finding whichever engine is wrong. An engine outside
relic cannot be fixed here, but a disagreement it causes is investigated
before the workload's numbers are used: it is how a program written for
that engine is found to mean something different from relic's.

## Measurement and reporting

For each engine and workload the harness reports the median wall-clock time
of the timed runs, the fastest, their standard deviation, the mean user
time (the better guide when the machine is not idle), the peak resident set
size as `hyperfine` measures it, the number of answer rows and the status.
The table is printed per workload, and a summary gives each engine's median
as a multiple of relic's. With `-o` the same data is written as a
tab-separated file, one row per engine and workload, for plotting.

The header of every report names the machine, the operating system, the
scale, the number of runs, and every engine's version, path and switches, so
a recorded run can be reproduced; paths under the home directory are written
from `~`, so a recorded report names no user. What the harness cannot see --
power and thermal settings, and whether the machine was idle -- is added by
whoever records the run. A run kept for comparison by eye is recorded as
`bench/compare/results.txt`, as `bench/baseline.txt` is for `bench/run.sh`.

A machine-specific measurement report records its conditions and limits;
it does not imply a ranking on other platforms. General performance claims
are published only from a run that meets all of these:

* an idle machine, with its power and thermal settings recorded;
* at least ten timed runs per cell, with the spread beside the median;
* every engine at a pinned, recorded release;
* the scale sweep, not a single scale;
* a second machine of a different operating system and processor, to show
  the ranking is not an artifact of one platform.

## External data sets

The Graspan inputs are downloaded on demand into `bench/compare/cache/`,
which git ignores. Each has its source URL and SHA-256 checksum in the
harness; the archive is checked before every use, and a download that does
not match is discarded. An input with no stable public URL may be placed in
the cache by hand and is checked the same way. Data is never committed, and a
workload whose data cannot be had is skipped with a note rather than failing
the run.

A workload of downloaded data runs only when `-w` names it. Its data does not
scale. A default run uses only generated data and needs no network.
Its duration depends on the engines and the number of repetitions; a
successful verification can be followed by many minutes of timed runs.

Graspan's own release has no working download. The harness takes the inputs
the papers comparing Datalog engines on CSPA use: the FlowLog benchmark
collection's archives, one per program, each holding `Assign.csv` and
`Dereference.csv`, published under the Apache License 2.0 on Hugging Face.
The harness's URL names the revision its checksums are of; the checksum, not
the URL, decides that an archive is the one recorded. Unpacking turns each
comma-separated file into a relation file.

## Layout and running

```text
bench/datasets.sh            the data-set generators, shared
bench/run.sh                 the relic instrument
bench/baseline.txt           one recorded run of bench/run.sh
bench/compare/run.sh         the comparison harness
bench/compare/driver.pl      prints SWI-Prolog's answer relation
bench/compare/<workload>/    dataset; the programs relic.dl, swipl.pl,
                             souffle.dl, clingo.lp, nemo.rls, duckdb.sql,
                             sqlite.sql, or a programs file; optionally absent
bench/compare/cache/         downloaded data sets, not committed
```

```text
bench/run.sh [relic]
bench/compare/run.sh [-e engines] [-o results.tsv] [-w workloads]
```

`-e` and `-w` take comma-separated names and restrict the run; by default
it covers every workload of generated data and every engine found. relic is
required; any other engine not installed is skipped with a note. Each
engine's command can be named by an environment variable -- `RELIC`,
`SWIPL`, `SOUFFLE`, `CLINGO`, `NMO`, `DUCKDB`, `SQLITE`, and `CXX` for
Souffle's compiler -- and `BENCH_SCALE`, `BENCH_RUNS` and `BENCH_TIMEOUT`
set the scale, the number of timed runs and the verification time limit.
`BENCH_THREADS` sets the thread count. `BENCH_ARTIFACTS` retains hyperfine's
individual samples under `scale-<scale>/threads-<threads>/`; an existing
run directory is refused so results are not overwritten. Commands in JSON
reports are replaced with engine/workload labels to avoid publishing local
paths; sample values are unchanged. The text report records the executable
versions, switches and compiler. `-o` writes each completed cell immediately,
so results survive an interrupted sweep. Reports from hyperfine's two
pretty-printed JSON schemas use the same seconds and bytes.

relic must be a binary. In a build tree configured for shared libraries,
`./relic` is libtool's wrapper script, whose start-up would be timed as
relic's; both harnesses refuse a script, and are pointed at an installed
relic or one configured with `--disable-shared`.

## What is deliberately absent

* **No limits on the engines.** Each runs without resource limits beyond
  those it needs raised to finish; a limit would compare refusals.
* **No in-engine timers.** Engines report their own phases differently and
  some not at all. The process's wall-clock time is the one measure every
  engine shares, and the `startup` and `facts-join` workloads show how much
  of it is fixed cost and loading.
* **No failing on a time.** Timing is noisy and machine-dependent;
  `tests/perf.sh` bounds relic's performance on counters.
* **No ranking by one number.** A geometric mean over workloads hides the
  pattern that matters -- which kinds of program relic is slow on -- so the
  report gives every cell.
* **No translator between dialects.** See [Programs](#programs).
