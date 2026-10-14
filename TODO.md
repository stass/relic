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

**Improve performance with a smaller execution model.** [Medium, component:
`lib/exec.c`, `lib/round.c`, `lib/task.c`, `lib/merge.c`, `lib/import.c`,
`lib/relation.c`, `bench/compare/`]
Use the validated scan-only reference to rank remaining command costs.
Evaluate existing derived tags and earlier screen release as separate local
changes, then address candidate-memory growth and the import-publication
tradeoff through bounded measurements and focused designs where needed.
The [performance plan](docs/todo-benchmark-performance.md) defines this
sequence and its stopping rules; the
[measurement protocol](docs/todo-performance-measurements.md) keeps source,
answers, resources and uncertainty tied to each decision. No general
reduction, routing or replacement membership framework is committed.

**Parallel evaluation misses its speed-up targets.** [Medium, component:
`lib/round.c`, `docs/todo-parallel.md`] With the pool, `tc-cyclic` should
run at least six times faster at `-j 16` than at `-j 1`, and with the
parallel merge at least ten times; it is 6.2-fold, 2.78 s to 0.45 s, the
fastest of five runs on a loaded macOS Xeon, which meets the first and
misses the second. `dist/3` and `object/2`, which compute integers and
strings, should scale as `tc-cyclic` does; `dist/3` scales 2.5-fold.
Components side by side, designed in `specs/design-parallel.md`, are not
built, and parallel loading misses its target. The causes and what to try
are sorted in `docs/todo-parallel.md`.

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

**Validate a build policy against code-placement regressions.** [Medium,
component: `bench/compare/`, `configure.ac`, `tests/globals.sh`]
The [original cohorts](bench/compare/recorded/freebsd-5995wx/recursive-planning/report.txt)
measure 2.6-2.7% more serial cyclic-closure time and 1.3-1.4% more
facts-join time with equal tuple work and relation storage. The
[diagnostic controls](bench/compare/recorded/freebsd-5995wx/recursive-planning-analysis/report.txt)
reproduce the facts increase with import and validation alone. Permuting
only linked text sections changes the cyclic revision comparison from
+2.69% to -0.89% and facts-join from +1.70% to +0.02%; one permutation
does not establish a robust build or attribute every part of the cyclic
difference. The
[alignment and hardware-counter comparison](bench/compare/recorded/freebsd-5995wx/code-alignment/report.txt)
identifies 64-byte function alignment as a candidate: cyclic median spread
across three layouts falls from 2.67% to 0.83%, with 5.90% more text. The
[isolated probe-placement test](bench/compare/recorded/freebsd-5995wx/probe-alignment/report.txt)
does not support a probe-only attribute: offsets 0, 16 and 32 differ by at
most 0.20% on closure with surrounding code fixed. The C alignment
attribute slows closure by 1.37% in ordinary link order. The
[four-way build comparison](bench/compare/recorded/freebsd-5995wx/build-policy/report.txt)
finds ThinLTO lowers ordinary serial medians by 12.5-24.2% and selected
64-worker medians by 5.8-18.4%, with 29.6% more text. Adding global
64-byte alignment keeps the nine substantive serial layout spreads below
0.75%, but grows text by 35.1% and costs 1.1-1.5% on 64-worker cyclic
closure against ThinLTO alone in two cohorts. Validate ThinLTO across
supported machines and toolchains before setting a default; keep alignment
a separately measured choice.
Preserve equal argument lengths, multiple layouts, and guarded and
ordinary-Magic references. Repeat these controls after bulk loading and
the scan-only trim under the performance plan's measurement protocol.

**Compare output sinks and validate scaling on a second platform.**
[Medium, component: `bench/compare/`]
Complete and audit the FreeBSD scale sweep, then compare the machine-specific
results with [the Mac report](BENCHMARKS.md). Separate OS/compiler/engine
version differences from core-count effects. Run the uniform stdout-output sweep and compare it with the preserved
file-output cohort to quantify output-sink costs, especially for large
answers. Keep their raw measurements and comparison tables distinct.

**Bound query factoring awaits its rival comparison.** [Medium, component:
`lib/magic.c`, `bench/compare/tc-cyclic-bf`] The M3 Max scale-8 matrix
records 189.242 s at one thread and 27.032 s at sixteen for the 8,000
answers of `tc-cyclic-bf`, which the unfactored rewrite reaches after
3,191,837,759 derivations. [Bound query factoring](specs/design-query-factoring.md)
answers it with 805,760 derivations in about 0.1 s in a local run. Measure
`chain-right`, `tc-acyclic-bf` and `tc-cyclic-bf` against the rival engines
under the [performance plan](docs/todo-benchmark-performance.md#characterize-the-reference).

**Attribute the remaining per-derivation cost on program analyses.**
[Medium, component: `lib/eval.c`, `bench/compare/andersen`]
Busy-host observations show a substantial gap against Souffle's interpreter
on Andersen and nonlinear reachability. Profile representative analyses on
the simplified reference before selecting an intervention. The
[membership study](bench/compare/recorded/focused-investigations.md) supports
existing tags on closures, not a new cached-hash container or more prefetching.
Use the [performance plan's cost map](docs/todo-benchmark-performance.md#characterize-the-reference)
to distinguish remaining tuple comparison, lookup, expression and growth work.

**A growth's slack and transient keep the peak above the live heap.**
[Medium, component: `lib/util.c`] `chain-closure`'s peak footprint runs
from 34 to 49 MiB and its resident peak from 44 to 59 MiB, against a live
heap of 33.6 MB. An identifier table's capacity is a power of two because a probe
masks the hash, and a column array doubles because one policy serves every
arity, so a relation just past a doubling is half empty on both. A table's
growth holds both generations of its slots while it rehashes, one and a
half times the new size at that moment, and the allocator keeps the freed
generation for reuse. Growth by three halves under a multiply-shift range
reduction, `(hash * capacity) >> 32`, would keep a mask-free probe and
halve the slack. Measure it on `chain-closure` and `facts-join`, by peak
footprint as well as resident set, before changing the policy. The memory
limit does not count the transient either: an insertion or a round's merge
admitted at the limit holds one array's or one table's old generation
beside the new one while it is copied, one at a time.

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
then incremental maintenance and parallel evaluation, which change the
engine and cost the most; floating point last, since no workload measured
needs it. None may change the language semantics of v1.

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
* parallel evaluation, specified in `specs/design-parallel.md`: shared
  rounds, the parallel merge and parallel loading are built; components
  side by side, a threshold that weighs new tuples, streaming queries and
  the speed-up targets remain, in `docs/todo-parallel.md` and the major
  initiative above
* better cardinality estimation
* further Magic Sets and negation optimization
* floating point [Low, component: language], in `docs/todo-language.md`,
  specified in `specs/design-floats.md`; the item on type tests under
  Engine comes first
