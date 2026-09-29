# relic benchmark comparison design: relic against other engines

**Status.** Design document for the harness that runs one set of workloads
through relic, through other Datalog and tabled-Prolog engines and through
SQL databases, checks that every engine gives the same answer, and times
them. Forward-looking, like every document under `specs/`; `STATUS.md`
records what is implemented.

**Companions.** `specs/architecture.md` sections 62 and 66;
`specs/design-hardening.md` section 7, which specifies `bench/run.sh`, the
relic-only instrument whose data sets this harness shares.

---

## 1. What this document settles

* which engines relic is compared with, in which modes, and why;
* which workloads are run, and the rule that decides whether a workload may
  join the set;
* how one data set reaches every engine, each through its own native loading
  path;
* how the answers are checked, so that a fast wrong answer never becomes a
  number in a table;
* what is timed, how often, and how the results are recorded;
* where external data sets come from and where they are kept.

It changes neither the language nor the library. The harness is a
development instrument: it is not run by `make check`, it is not in the
release tarball, and nothing fails on a time.

---

## 2. Why compare

`bench/run.sh` measures relic against itself: its counters show whether a
change did more or less work. It cannot say whether relic's absolute
performance is reasonable. The comparison answers that, and answers it per
workload: where relic is within a small factor of a mature engine, where it is
an order of magnitude away, and whether the gap grows with the input, which
points at an algorithm rather than a constant.

The results serve two readers. Internally they rank performance work: a gap
that grows with scale is a design problem to record in `TODO.md`. Externally,
once the method below is followed in full, they are numbers relic can
publish.

---

## 3. Engines

| Engine | Why | Modes |
| --- | --- | --- |
| relic | the subject | the tool as a user runs it: the query in the program, answered through the Magic rewrite where it applies |
| SWI-Prolog | the Prolog most users have; tabling makes it a goal-directed Datalog engine | tabled predicates, one table per call variant |
| Souffle | the reference high-performance Datalog engine, bottom-up and compiled | `souffle-interp`: the interpreter; `souffle`: the program compiled to C++; `souffle-magic`: compiled, with `--magic-transform=*` |
| clingo | the most widely used answer-set system; for a stratified program its grounder is a complete bottom-up Datalog engine | the default solver, one thread |
| Nemo | a modern in-memory Datalog engine built for large data | the default |
| DuckDB | what a user who knows SQL reaches for instead: an in-process analytical database with recursive common table expressions | an in-memory database, one thread |
| SQLite | the most widely deployed SQL database, whose shell comes with most systems | an in-memory database |

Souffle appears three times because its modes answer different questions.
The interpreter is the fair comparison with relic's interpreter; the compiled
program is the best Souffle can do; the magic transform isolates what relic's
Magic rewrite buys over plain bottom-up evaluation, which Souffle performs by
default. The compiled modes are built by the harness: it asks Souffle for the
C++ program (`souffle -g`) and compiles it with the system C++ compiler at
`-O3`, defining the width of numbers Souffle reports it was built with, so
that the compiler and its flags are chosen and recorded by the harness rather
than inherited from a package's configuration. The definitions `souffle -o`
adds for SQLite, zlib and ncurses input and output, and OpenMP, are left out:
no workload uses them, and every run is single-threaded. Compile time is
reported separately and is not part of the run time.

**The SQL engines.** A recursive common table expression is SQL's Datalog:
a rule set becomes one `WITH RECURSIVE` expression per recursive relation,
its base rules and its recursive rule joined by `UNION`, which gives set
semantics, and a stratified negation becomes `NOT EXISTS`. Each relation is
a table the engine loads with its own bulk loader, and no index is created:
the engines build what their joins need, SQLite an automatic index and
DuckDB a hash table. The query is written as the final `SELECT`
over the relation, its constants in its `WHERE` clause. Moving a constant
into the expression's base case instead would be the Magic rewrite done by
hand, which no Datalog engine here is given; that neither DuckDB nor SQLite
pushes the constant into a recursive expression itself is a measurement of
them.

**One thread.** relic is single-threaded, so the primary numbers are too:
Souffle runs with `-j1`, clingo with its default of one thread, DuckDB with
`SET threads = 1`; SQLite runs a query on one thread. An engine
that parallelizes without a switch is reported as it is, and its user time
beside its wall time shows the parallelism. Multi-threaded Souffle may be
added as an extra column for context; it is never the comparison.

**No tuning beyond the documented switches.** Each engine runs as its manual
suggests for this kind of program. Where an engine needs a limit raised to
finish at all -- SWI-Prolog's stack and table space -- the harness raises it
and records the setting. No engine is given hand-written indexing hints,
modes or pragmas the others do not get.

Engines that were considered and left out: DDlog (archived); Scryer and
Trealla Prolog (tabling not mature enough to be a fair representative);
XSB (the reference for tabling, but SWI-Prolog's tabling descends from it and
is the one users have); embedded Rust engines such as Crepe and Ascent, which
compile each program into a Rust binary and measure a different use; and
PostgreSQL, whose recursive expressions are SQLite's and DuckDB's, behind a
server the harness would have to run.

---

## 4. Workloads

### 4.1 The admission rule

A workload joins the set only if every engine can express it natively and
relic accepts it. relic's language has no arithmetic and no aggregates, and
negation must be stratified, so those features are absent from every
workload. A program that needs one of them is not translated around it for
relic's benefit or anyone else's.

The SQL engines are a baseline outside the rule. SQL allows a recursive
expression only linear recursion, one reference to itself in its recursive
part, and DuckDB and SQLite allow no recursion through another expression
either, so the nonlinear closure and the points-to analyses cannot be
written for them, and those workloads have no SQL program; the report shows
`n/a`. DuckDB accepts
a nonlinear expression but answers it incompletely, since each recursive
reference sees only the rows the previous iteration added.

Every workload is a data set, a set of rules, and one query. The query is
either bound -- it names a constant, and a goal-directed engine may explore
only what it reaches -- or free, when the whole relation is the answer. Both
kinds are in the set, because they favour different designs: relic, Souffle
with the magic transform and SWI-Prolog's tabling are goal-directed; Souffle,
clingo and Nemo evaluate bottom-up by default, as DuckDB and SQLite evaluate
a recursive expression.

### 4.2 Set A: the relic workloads

The data sets of `bench/run.sh`, queried as a user would query them:

| Workload | Data set | Query |
| --- | --- | --- |
| `startup` | one fact | the fact; measures process start-up, loading and output |
| `chain-right` | a chain of 2000 edges | bound; right-recursive closure |
| `chain-left` | the chain | bound; left-recursive closure |
| `chain-closure` | the chain | free; the whole closure, two million tuples |
| `graph-reach` | 20000 nodes, 100000 edges | reachability from one node |
| `graph-left` | the graph | bound; left-recursive closure from one node |
| `grid-reach` | a 150 by 150 grid | reachability from a corner |
| `tree-sg-bound` | a complete binary tree of depth 12 | bound; same generation |
| `tree-sg-all` | a tree of depth 9 | free; same generation |
| `tasks-ready` | 10000 tasks and their dependencies | stratified negation |
| `facts-join` | 300000 ternary facts | a join with integer constants; mostly loading |

Every size is multiplied by `BENCH_SCALE`, as in `bench/run.sh`, so the same
workloads trace a curve as the input grows.

### 4.3 Set B: workloads others will recognize

* The transitive-closure and same-generation programs of OpenRuleBench
  (Liang, Fodor, Wan and Kifer, 2009), with bound and free queries, on
  acyclic and cyclic graphs at several densities; the suite is the usual
  comparison between Prolog and Datalog engines. Its programs that need
  recursion through negation or arithmetic fail the admission rule and are
  omitted.
* The nonlinear transitive closure, whose recursive rule calls the closure
  twice, which exercises a rule with more than one recursive literal.
* Context-sensitive points-to analysis (CSPA) over the Graspan data sets, and
  an Andersen-style points-to analysis: realistic program-analysis workloads
  that need only joins and recursion, and are used by several recent Datalog
  papers.

The programs are OpenRuleBench's as rbench, Stefan Brass's collection of
logic-programming benchmarks, writes them, same generation starting from
`sg(Y, Y) :- par(_, Y)`. The graphs are OpenRuleBench's: random, over
integer nodes, as `par(From, To)`. The dense ones draw 50000 edges among
1000 nodes, the size of OpenRuleBench's `tc_d1000_parsize50000`; the sparse
ones draw 5000. An acyclic graph points every edge forward in a fixed order
of its nodes, one that puts node 500, the node the bound queries name, in
the middle at every scale, so that it has ancestors and descendants both.
OpenRuleBench's query modes are kept by name: `bf` binds the first
argument, `fb` the second.

| Workload | Data set | Query |
| --- | --- | --- |
| `tc-cyclic` | dense, cyclic | free; the closure, every pair of nodes |
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
| `dist-bounded` | sparse, cyclic | free; `dist(X, Y, D)`, every length up to 5 of a walk from `X` to `Y`, computed by `E = D + 1` |
| `andersen` | a generated program of 2000 functions | free; every points-to fact |
| `cspa` | a generated program of 1000 functions | free; the value-flow relation, which needs the other two |

The nonlinear closure runs on the sparse acyclic graph only: its cost is
cubic in the nodes a cycle joins, and on a cyclic graph of this size it runs
for minutes in every engine.

The two points-to analyses also run on generated programs, so that they can
be scaled and need nothing downloaded. A generated program is functions of
twenty variables whose statements relate variables of the same function,
joined by a binary call tree down which arguments are passed. Statements
drawn uniformly across the program would not do: past a threshold density
they join nearly every variable into one set of aliases, and the answer
jumps from linear to quadratic in the program's size at a point that
depends on the draws, so the scale sweep would measure the draws. The
Graspan data sets remain the real-world case, run from the download cache of
section 9.

### 4.4 Programs

Each workload's rules are written by hand, once per engine, in that engine's
own syntax, and kept beside each other in the workload's directory. A
translator from one dialect to the others would be a second compiler to
trust; short programs are easier to read and to check against each other.
relic's program ends with the query itself, as a relic user writes it. Every
other program writes the query's answer, its columns the query's named
variables in order of first occurrence -- the columns relic prints -- as its
engine writes a result: SWI-Prolog's defines `answer/N`, which a shared driver
prints; Souffle's, Nemo's and clingo's output a relation to the file
`answer.csv`, `answer.tsv` or standard output; DuckDB's and SQLite's end with
the query, whose rows the engine prints to standard output. When the query
is a whole relation, the bottom-up programs output that relation itself
rather than copying it into a second one, which relic and SWI-Prolog would
not pay for.

Each workload's directory also names its data set in a file, `dataset`, and
the harness finds the workloads by looking for that file, so adding a
workload is adding a directory.

---

## 5. Data

A data set is generated once per run into a directory of relation files: one
file `<relation>.tsv` per relation, one tuple per line, columns separated by
tabs. Symbols are written bare and integers in decimal; no workload uses
strings, whose quoting rules differ between engines. Every relation file a
generator writes is a set: a generator may draw a row twice, and the repeat
is removed, since an engine that keeps facts as clauses, as SWI-Prolog does,
would use a repeated fact twice where the others use it once.

Each engine loads the data through its own native path, because that is the
path its users take and the one its authors optimized:

| Engine | Loads |
| --- | --- |
| Souffle | the relation files, through `.input` |
| Nemo | the relation files, through `@import` |
| relic | the relation files, through `-I` |
| SWI-Prolog | one source file of facts, consulted |
| clingo | one source file of facts |
| DuckDB | the relation files, through `COPY` |
| SQLite | the relation files, through `.import` |

relic imports each relation file as a tsv table, with its columns typed
from the file's first row: a column of integers is `integer`, and any other
is `symbol`. The `facts-join` workload is mostly loading, so it isolates what
each engine's native path costs.

The generators are shared with `bench/run.sh` through `bench/datasets.sh`, so
both harnesses measure the same data. They draw from a fixed linear
congruential sequence, so a data set is identical on every run and platform.

---

## 6. A run

For each workload, relic runs first, since its answer is the reference, and
then every other engine. Each is run twice over:

1. **The verification run**, once, under a time limit (`BENCH_TIMEOUT`, 300
   seconds by default). Its answer is written to a file and checked (section
   7). An engine that exceeds the limit is reported as `timeout`, one that
   exits unsuccessfully as `failed`, one whose answer differs as `mismatch`;
   in every case it is not timed. The verification run also warms the file
   cache.
2. **The timed runs**, `BENCH_RUNS` of them, 5 by default, by `hyperfine`,
   with the engine's standard output discarded. Each run is the whole
   process: start-up, loading, evaluation and writing the answer.

Souffle's compiled modes are compiled before the verification run; the
compilation is timed once and reported in its own column.

An engine with no program for a workload, which only the SQL engines may
lack, is reported `n/a` and not run.

A failing engine is recorded and the harness continues with the next; it
exits unsuccessfully at the end if any engine failed or disagreed, so a
mismatch is never lost in a long table. An engine that runs out of time is
reported but does not fail the run: a timeout is a measurement of the engine,
and some engines exceed any reasonable limit on the deep recursions, which
would otherwise leave the exit status unable to signal a mismatch.

---

## 7. Answers

The verification run's output is normalized to rows of tab-separated
columns -- relic's `-F tsv`, SWI-Prolog's rows as the driver prints them,
Souffle's and Nemo's exported files, clingo's atoms taken apart, and the SQL
engines' rows printed with tabs between columns -- then sorted with
duplicates removed, since every engine has set semantics but not every one
prints a set in order. relic's normalized answer is the reference,
and every other engine's must equal it byte for byte. An engine that leaves
no answer file has failed, not answered nothing.

Every workload has a non-empty answer, and the harness fails a workload whose
reference is empty: engines agreeing on no rows check nothing, and would let
any program that exits successfully pass. The generators are written with
that in mind -- a draw from their sequence takes its high bits, because the
low bits of a power-of-two linear congruential sequence cycle with short
periods and would correlate consecutive draws.

A disagreement is a finding in its own right, whichever engine is wrong. An
engine outside relic cannot be fixed here, but a disagreement it causes is
investigated before the workload's numbers are used: it is how a program
written for that engine is found to mean something different from relic's.

---

## 8. Measurement and reporting

For each engine and workload the harness reports the median wall-clock time
of the timed runs, the fastest, their standard deviation, the mean user time,
which is the better guide when the machine is not idle, the peak resident set
size as `hyperfine` measures it, the number of answer rows and the status.
The table is printed per workload, and a summary gives each engine's median
as a multiple of relic's. With `-o` the same data is written as a
tab-separated file, one row per engine and workload, for plotting.

The header of every report names the machine, the operating system, the scale,
the number of runs, and every engine's version, path and switches, so that a
recorded run can be reproduced; paths under the home directory are written
from `~`, so that a recorded report names no user. What the harness cannot see
-- the power and thermal settings, and whether the machine was idle -- is
added to a recorded run by whoever records it. A run recorded in the
repository for comparison by eye is kept in `bench/compare/results.txt`, as
`bench/baseline.txt` is kept for `bench/run.sh`.

Numbers are published only from a run that meets all of these:

* an idle machine, with the power and thermal settings recorded;
* at least ten timed runs per cell, with the spread reported beside the
  median;
* every engine at a pinned, recorded release;
* the scale sweep, not a single scale;
* a second machine of a different operating system and processor, to show
  the ranking is not an artifact of one platform.

---

## 9. External data sets

Set B's real-world inputs are downloaded on demand into
`bench/compare/cache/`, which is ignored by git. Each has its source URL and
SHA-256 checksum in the harness, and a download that does not match is
discarded. An input with no stable public URL is placed in the cache by
hand, and the harness checks it against its checksum the same way. Data is
never committed, and a workload whose data is absent is skipped with a note
rather than failing the run.

---

## 10. Layout and running

```text
bench/datasets.sh            the data-set generators, shared
bench/run.sh                 the relic-only instrument
bench/compare/run.sh         this harness
bench/compare/driver.pl      prints SWI-Prolog's answer relation
bench/compare/<workload>/    dataset, relic.dl, swipl.pl, souffle.dl,
                             clingo.lp, nemo.rls, duckdb.sql, sqlite.sql
bench/compare/results.txt    one recorded run
bench/compare/cache/         downloaded data sets, not committed
```

```text
bench/compare/run.sh [-e engines] [-o results.tsv] [-w workloads]
```

`-e` and `-w` take comma-separated names and restrict the run; by default it
covers every workload and every engine found. relic is required; any other
engine that is not installed is skipped with a note. Each engine's command
can be named by an environment variable -- `RELIC`, `SWIPL`, `SOUFFLE`,
`CLINGO`, `NMO`, `DUCKDB`, `SQLITE`, and `CXX` for Souffle's compiler -- and
`BENCH_SCALE`, `BENCH_RUNS` and `BENCH_TIMEOUT` set the scale, the number of
timed runs and the time limit.

relic must be a binary. In a build tree configured for shared libraries,
`./relic` is libtool's wrapper script, whose start-up would be timed as
relic's; the harness refuses a script, and is pointed at an installed relic
or one configured with `--disable-shared`. `bench/run.sh` refuses one too.

---

## 11. What is deliberately absent

* **No limits on the engines.** Each engine runs without resource limits
  beyond those it needs raised to finish; a limit would compare refusals.
* **No in-engine timers.** Every engine reports its own phases differently
  and some not at all. The process's wall-clock time is the one measure every
  engine shares, and the `startup` and `facts-join` workloads show how much of
  it is fixed cost and loading.
* **No failing on a time.** Timing is noisy and machine-dependent;
  `tests/perf.sh` bounds relic's performance on counters. The comparison
  fails only on an answer.
* **No ranking by one number.** A geometric mean over workloads hides the
  pattern that matters -- which kinds of program relic is slow on -- so the
  report gives every cell.
