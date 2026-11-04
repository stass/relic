# relic open work

Forward-looking index. Closed items are deleted; git history is the record of
what was done. Each item carries a severity and a component pointer.

## Engine

**Type tests read the type bits directly.** [Medium, component:
`lib/exec.c`, `lib/rule.c`] Floats take tag 3 with the top bit of the index
set, which works only if every type test goes through the accessors that
split an identifier into type and index
([the identifier](specs/design-floats.md#the-identifier)). Before floats
land, route the executor's type tests and `cell_type()`, and the term-type
checks in `lib/rule.c`, through third-bit-aware accessors. Ordinary folds
compare decoded types through `lib/fold.c`.

**A Magic rewrite found unstratified reports user errors first.** [Low,
component: `lib/eval.c`, `lib/stratify.c`] `evaluate_rewrite()` treats an
unstratified rewrite as an internal error, but `relic_schedule_build()`
has already run `report_cycle()`, so the user would read "not stratified"
diagnostics about generated predicates before the internal error. Build
the rewrite's schedule without reporting cycles.

**`lib/regex.h` shadows the system `<regex.h>`.** [Low, component:
`lib/regex.h`, `Makefile.am`] `AM_CPPFLAGS` puts `-I$(top_srcdir)/lib` on
the include path, so any system header that includes `<regex.h>` would get
relic's. Rename the header.

**An index built mid-component must be raised by its builder.** [Medium,
component: `lib/index.c`, `lib/relation.h`, `lib/eval.c`] Each index keeps
its own `latest_since`, yet `set_delta()` always raises every index of a
member to the same start, so an index built between rounds is correct only
because `prepare_candidate()` raises it; a build anywhere else would leave
its delta lookups walking from a stale frontier, which only an assertion
notices. Keep the start on the relation instead, which a new index's
frontiers, each at its key's first tuple, satisfy at any start; that
deletes the per-build raise, provided clearing a relation resets the start.

**A planner that emits an indexed operation must record its access.**
[Low, component: `lib/plan.c`] `place_atom()`, `place_probe()` and
`place_aggregate()` each call `record_access()`. An operation holds its
relation read-only, which `-Wcast-qual` keeps a single walk of the finished
draft from undoing, so the draft's accesses cannot be derived from its
operations. A missed call fails loudly, preparation reporting an internal
error, but undercharges acquisition until then.

## Benchmarks

**Preserve the editorial README when publishing benchmark reports.**
[Medium, component: `bench/compare/tools/render-mac-results.py`]
The report publisher replaces the whole README Performance section with its
own template, removing the curated table and chart embeds. Give generated
results a bounded destination that preserves the surrounding prose and charts.

**Refresh published comparisons with consistent output and host coverage.**
[Medium, component: `bench/compare/`]
The M3 Max composition check of the accepted changes together uses
stdout for every timed engine; it does not replace the full
[file-output tables](BENCHMARKS.md). When publishing a broader comparison,
keep those cohorts separate, validate second-host coverage, and distinguish
OS/compiler/engine-version differences from core-count effects. The selected
performance-plan comparisons take precedence over a full publication sweep.

## Testing and CI

**Qualify the FreeBSD MSan toolchain and dependencies.** [Medium, components:
`m4/relic_sanitizers.m4`, `lib/pool.c`, `tests/`, development toolchain] The full suite needs
working shadow propagation through libc memory streams and instrumented
ncurses/libedit; a standalone C program reproduces the stdio failure. Also
resolve the observed MSan worker-startup/loader-atfork stall before treating
this host as a full MSan validator. Under MSan on this host, without suppressions,
the suite records 324 passes, four failures and two unfinished tests.

**Native global merging hides source ownership from the object audit.**
[Medium, component: `tests/globals.sh`] On Apple clang at `-O3`, a mutable
file-static struct with separately used integer and pointer fields becomes
`__MergedGlobals` in the native object. Its C spelling is reserved to the
implementation, so the audit cannot distinguish it from compiler-owned
state. Preserve declaration provenance before native optimization rather
than special-casing generated names or disabling production optimizations.
The bitcode audit checks data-candidate names across lowering; native
objects need an equivalent source of ownership information.

**ThinLTO archives retain the compiler's source filename.** [Medium,
component: `m4/relic_cflags.m4`, `Makefile.am`] Clang's `-ffile-prefix-map`
rewrites debug paths but not LLVM IR's `source_filename`. An out-of-tree
build invoked through an absolute source path exposes that path in its
installed bitcode archive. Make source naming reproducible independently
of how configure is invoked, while preserving out-of-tree builds, compiler
wrappers and cross-compilation. Relative source paths avoid the disclosure;
the build instructions describe that packaging constraint.

**LeakSanitizer flags macOS's fork handler.** [Low, component:
`tests/random/main.c`, `Makefile.am`] Built with Homebrew's LLVM, where
AddressSanitizer detects leaks on macOS, every child the random driver forks
reports memory `_notify_fork_child` in libSystem allocates, and the child's
exit status makes the driver report a failure and shrink a program that does
nothing wrong. Linux, where CI runs the driver, is unaffected. A LeakSanitizer
suppression for that frame, passed by `check-random` and `check-asan`, would
let a macOS developer run them with leak detection.

## Post-1.0

Candidate features, roughly in priority order: what remains of the
language features first, since they decide which programs can be written;
then incremental maintenance, which changes the engine and costs the most;
floating point last, since no workload measured needs it. Performance work
follows the major initiative's own priority order. None may change the
language semantics of v1.

**Language and tooling gaps.** [Medium] What Souffle, LogiQL, Nemo, clingo,
CodeQL, DDlog and the tabling Prologs offer that relic does not, sorted in
`docs/todo-language.md` in the order worth adding them -- provenance
first, then disjunction, file inclusion, constraints, per-rule profiling
and floating point; what stays out on purpose is in
[non-goals](specs/architecture.md#non-goals). Each item names the engines
that have it and what a relic program does without it; the verdicts are
provisional until a design exists.

* incremental insertion maintenance [Medium, component: `lib/eval.c`]: any
  change to the program after an evaluation discards every derived relation
  and computes the fixed point again from the facts, so an embedder or a
  session that loads a large program and adds to it a little at a time pays
  for the whole program on every addition. Continuing from the tuples held
  is sound without negation, where a new base fact is a delta the
  semi-naive machinery already understands, and unsound with it, where a new
  fact can retract a derived one;
  [when the relations are current](specs/design-eval.md#when-the-relations-are-current)
  settles one rule for both, and [non-goals](specs/architecture.md#non-goals)
  defers maintenance. Measure how often a caller adds facts between queries
  before building either half.
* DRed-style deletion maintenance
* better cardinality estimation
* further Magic Sets and negation optimization
* floating point [Low, component: language], in `docs/todo-language.md`,
  specified in `specs/design-floats.md`; the item on type tests under
  Engine comes first
