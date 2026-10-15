# relic open work

Forward-looking index. Closed items are deleted; git history is the record of
what was done. Each item carries a severity and a component pointer.

## Major initiatives

**Release relic 1.0.** [Critical, component: `specs/design-release.md`]
What remains, per the design's
[release gate](specs/design-release.md#the-release-gate): a green run of
`extended.yml`, its random-program and fuzz jobs included, and `macos.yml`
on the release commit, since every job of the matrix must pass on it; FreeBSD, OpenBSD and NetBSD, which no workflow
reaches, built and tested from the tarball by hand with no autotools
present, the result recorded in the release's `NEWS` entry; the packaging
skeletons the design's
[packaging skeletons](specs/design-release.md#packaging-skeletons) describe,
of which `contrib/` holds only the homebrew formula, so
`contrib/freebsd/devel/relic/`, `contrib/openbsd/devel/relic/` and
`contrib/pkgsrc/devel/relic/` are to write; and the
[release procedure](specs/design-release.md#the-release-procedure).

**Close the measured performance gaps.** [Medium, components:
`lib/round.c`, `lib/task.c`, `lib/merge.c`, `lib/relation.c`, `lib/exec.c`,
`lib/import.c`, `bench/compare/`]
Use the derived-tag engine with native compilation as the working reference.
Next test whether inlining the existing probe functions reduces closure CPU cost,
then attribute files/facts throughput. ThinLTO remains an explicit build
option. Parallel working memory remains a larger structural gap: further
implementation needs a design that reduces or bounds
actual worker copies without the rejected screen-reuse prototype's slowdown. The
[performance plan](docs/todo-benchmark-performance.md) sets each remaining
measurement, candidate and acceptance boundary. Use the
[measurement protocol](docs/todo-performance-measurements.md) to separate
rough ranking from implementation decisions; [parallel questions](docs/todo-parallel.md)
feed that work rather than creating another queue.

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

**Refresh published comparisons with consistent output and host coverage.**
[Medium, component: `bench/compare/`]
The [rough rival refresh](bench/compare/recorded/m3-max/rough-rivals/README.md)
uses stdout for every timed engine; it does not replace the full
[file-output tables](BENCHMARKS.md). When publishing a broader comparison,
keep those cohorts separate, validate second-host coverage, and distinguish
OS/compiler/engine-version differences from core-count effects. The selected
performance-plan comparisons take precedence over a full publication sweep.

**Check the remaining dense-analysis cases before assigning an optimization.**
[Medium, components: `lib/eval.c`, `bench/compare/andersen`,
`bench/compare/tc-nonlinear`]
Older dense Andersen and nonlinear-reachability observations are not covered
by the rough refresh; the current parallel Linux CSPA win does not settle them.
Use one bounded representative comparison under the
[composition validation](docs/todo-benchmark-performance.md#preserve-gains-and-validate-the-composition)
before reopening a general per-derivation optimization. Membership tags are
a closure candidate with their own controls, not evidence for a new container
on every analysis.

## Testing and CI

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

* string operations, specified in
  [functions](specs/language.md#functions) and
  `specs/design-expressions.md`, are built; what remains:
  * [Medium, performance, `lib/exec.c`, `lib/value.c`] the serial cost is
    above its target. Over a million sources, `object/2`, `object(M, O)
    :- source(M, S), O = cat(substr(S, 0, length(S) - 2), ".o")`, adds
    0.60 s to the load where the same join assigning a constant adds
    0.13 s, 4.6 times against a target of twice. Each computed string is
    probed in the pool twice, when its text slot is bound and again by the
    measure, which is most of the 0.32 s `str(M)` over the same sources
    costs; interning a million new strings is most of the rest. Probing
    only for a slot some later operation reads as a key, and not for one
    only an `EMIT` or an expression reads, would halve the probes without
    changing what the measure decides; it is a planner decision that
    changes [slots and cells](specs/design-expressions.md#slots-and-cells),
    which probes at every assignment.
  * [Medium, performance, `lib/merge.c`] `object/2` should scale as
    `tc-cyclic` does at sixteen threads: over a million sources it
    evaluates in 0.60 s past the load on one thread and 0.33 s on sixteen,
    1.8 times, where `tc-cyclic` scales 5.3 times (2.66 s to 0.50 s). At
    sixteen threads the tasks take 0.059 s, gathering the candidates
    0.025 s, applying them 0.070 s, and the measure of the pending strings
    0.163 s, serially: it probes the pool for each gathered string and
    numbers the new ones in order of first occurrence. Striping the measure
    by the string's hash, as the apply is striped by the tuple's, would run
    the probes in parallel and leave only the numbering serial, over the
    new strings alone; that changes
    [interning computed values](specs/design-values.md#interning-computed-values),
    which specifies one serial measure.
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
