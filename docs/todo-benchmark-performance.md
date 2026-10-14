# Benchmark performance work

[Medium, components: `lib/plan.c`, `lib/cost.c`, `lib/eval.c`,
`lib/magic.c`, `lib/import.c`, `lib/merge.c`, `lib/relation.c`,
`bench/compare/`]

Improve demonstrated bottlenecks while keeping relic small enough to read
and audit. A benchmark gain is evidence for a change, not an obligation to
keep its architecture. Default serial performance, memory, failure paths
and the maintenance cost across subsystems are part of the decision.

Related: [open work](../TODO.md), [comparison tables](../BENCHMARKS.md),
[parallel work](todo-parallel.md), [testing](../specs/design-testing.md).
The [retained-baseline measurements](../bench/compare/recorded/retained-baseline.md)
record the simplification's throughput tradeoff and the remaining merge
workspace, with raw samples from both hosts. The
[complexity audit](../bench/compare/recorded/complexity/README.md) compares
the retained engine with the code before this plan: noncomment library
lines grow 17.8%, with bulk loading and driver slicing accounting for
about 69% of the physical-line increase. Average function branching is
similar; coordination and ownership are the larger maintenance cost.
The [focused investigation results](../bench/compare/recorded/focused-investigations.md)
connect the isolated changes, ownership measurements and their limits.

## Decisions from the implementation work

| Stage | Decision | Reason |
| --- | --- | --- |
| 1. Recursive plan selection | Keep the two-candidate policy and lazy index acquisition; do not add more policies or index-lifetime machinery without evidence. | Removes a large work cliff with one deterministic choice per round. |
| 2. Pending-value compaction | Keep. | Workspace follows referenced local values instead of repeated tuple columns, with explicit ownership and no second interner. |
| 3. Bound query factoring | Keep its narrow certificate. | Removes orders of magnitude of redundant derivations and memory on eligible recursion. |
| 4. Bulk loading | Keep transferable builders, bounded batching, regular-file selection, framing overlap and input limits. Use one symbol index as the retained baseline. | Builders, batching and framing have measured gains. Isolated comparisons establish the parallel import cost of removing the symbol package; assess that tradeoff separately from grouped reduction. |
| 5. Indexed driver slicing | Re-evaluate the retained extension's benefit before expanding it or completing a broader feature queue. | Its modest measured whole-command gain reaches fifteen library files and the ordinary serial executor; the benefit has a narrower evidence base than stages 1-3. |
| 6. General grouped reduction | Withdraw from the production plan. Keep ordinary fold semantics, the stored-count shortcut and a separate EMIT schema. | The reduction framework adds source-removal state, summary selection, value ownership and a separate parallel lifecycle across the engine. Its measured gains do not justify that scope. |
| Candidate routing and index layouts | Retain existing tags and early screen release as local candidates; no new routing or membership module is queued. | Measurements support useful comparison filtering and lower charged overlap without another representation or execution protocol. |

The grouped-reduction experiment is preserved on the git branch
`archive/stage6-experiment`, outside the production line.
Its existence does not make query-owned reductions, stored-source waves,
run-wide index credit or a new common summary planner follow-up tasks.
A future proposal must stand on its own, with a smaller ownership model and
its own complexity and performance evidence.

## Measurement and acceptance rules

Use immutable baseline and candidate binaries, matching compiler options,
data, hash/strategy settings and output sinks. Retain raw samples, exact
answer checks and timing spread. On targeted comparisons use at least ten
interleaved samples; measurements on busy machines and single diagnostic
runs identify candidates but do not establish release performance.

Record wall time, user CPU, peak RSS and accounted storage. Split loading,
planning, expression work, evaluation, merge and output when attributing a
gain. Count actual useful work and mechanism events separately from timing.
Allocator retention and a changed code layout can affect results without a
change to the algorithm; investigate them before adding a runtime policy.

Check the default one-thread path and sixteen workers, plus relevant
intermediate worker counts. Use both the M3 and p620 for a change whose
tradeoff depends on scheduling, cache locality or allocation. Compare the
changed subsystem to the existing implementation as well as the complete
command to rival engines. A competitive target does not authorize an
unbounded sequence of features to reach it.

A change must explain the work it removes, the permanent concepts it adds,
its ownership/failure boundary, and the controls on which it can lose.
Prefer an existing execution path, a local representation change or the
removal of repeated work. A modest parallel gain that slows the default
path or requires another context-wide lifetime is not an automatic win.
Reject or narrow a change when its complexity outweighs its demonstrated
benefit. Do not compensate with another heuristic before establishing the
cause of the regression.

## 1. Correct recursive plan selection

[Medium, components: `lib/plan.c`, `lib/cost.c`, `lib/eval.c`]

Keep at most delta-first and input-first plans, with identical orders kept
once, a saturating integer work estimate and lazy acquisition of indexes
only a chosen alternative needs. Existing index key counts are read
directly; there is no separate key-count cache. Selection depends on frozen
relations and views, not worker count or timing.

The guarded closure at scale 4 reads about 19.4 million tuples instead of
94.3 million; at scale 8 it makes 72.1 million lookups instead of 356.6
million. Lazy acquisition keeps chain distances at about 201.7 MB and
1.7 s when the alternative is never selected. These structural improvements
justify the bounded choice even though estimates cannot predict every skew.

Remaining validation:

* Preserve the guarded work-counter regression independently of factoring.
* Exercise input-first semantics and allocation refusal, including mutual
  recursion, repeated variables, aggregates and fallback.
* Record planning/selection cost on long rules and many small rounds, and
  index maintenance on the two-node braid and skewed fan-in controls.

An acquired index remains maintained after a different plan is selected.
The braid control holds 42.3 MB against 33.9 MB and reads 8.6 million tuples
against 8.0 million at similar wall time. Measure that tradeoff before
proposing eviction, maintenance prediction or additional candidates.
See [recursive planning](../specs/design-recursive-planning.md).

## 2. Compact pending values at the merge boundary

[Medium, component: `lib/merge.c`]

Keep one descriptor per referenced worker-local value in logical order,
then use the ordinary value resolver. Temporary maps end at the rewrite
barrier; the resolution map and copied bytes retain their own lifetime.

At scale 8, about 3.6 million pending columns over 206 distinct strings
require at most 3,296 worker-local descriptors at sixteen workers. Five
interleaved M3 runs measured 0.84 s against 0.88 s and 422 MB against
485 MB; an all-distinct control measured 1.20 s against 1.22 s and
873 MB against 938 MB. The one-thread path is unchanged.

Keep the accounted-memory regression that requires a successful merge.
Repeat the targeted comparison with charged workspace and phase timings,
including unused worker values, empty resolution and all-distinct values.
Do not add another interner or cardinality-dependent representation.
See [pending values](../specs/design-pending-values.md).

## 3. Factor eligible bound recursion

[Medium, component: `lib/magic.c`]

Keep the complete-adornment separation certificate, ordinary generated
rules and `-X no-factor` reference. Rejected recognition and replay remain
charged to the one rewrite-work counter; its default accommodates the
additional recognition pass. Do not broaden the certificate by enumerating
special expression functions or predicate shapes.

The mechanism has a strong work bound: the 1,000-edge right-recursive chain
is held to `8N + 64` derivations against `N(N + 1)/2` in the reference.
Diagnostic M3 scale-8 runs give:

| Workload | Derivations, factored / reference | One-thread wall | Sixteen-thread wall |
| --- | --- | --- | --- |
| `chain-right` | 48,002 / 128,040,001 | 0.01 / 25.1 s | 0.01 / 11.3 s |
| `tc-acyclic-bf` | 167,090 / 71,093,957 | 0.07 / 2.79 s | 0.07 / 0.51 s |
| `tc-cyclic-bf` | 805,760 / 3,191,837,759 | 0.09 / 106.0 s | 0.08 / 14.9 s |

Retain the independent ordinary-Magic and naive oracles, rejection tests,
exact explicit-limit boundaries and query reuse checks. Repeat timing and
memory comparisons under the measurement rules, including distinct base
and transition relations, multiple origins and ineligible controls.
See [query factoring](../specs/design-query-factoring.md).

## 4. Build and load base relations in bulk

[Medium, components: `lib/import.c`, `lib/window.c`, `lib/facts.c`,
`lib/relation.c`, `lib/value.c`]

Keep builders whose payload an empty destination adopts, bounded windows,
shared pure parser transitions, ordered value resolution and scalar replay.
Keep the scalar path for small imports and too few effective workers.
Only positively identified regular files may batch; forced parallel mode
does not override that restriction. Preserve `max_input`, bounded record
counts and resumption after an oversized record.

Before symbol-index striping, five-run measurements found that the shared
builders made default loading 8-26% faster on p620 and 14-44% faster on M3,
with 16-52% less peak memory. Batching at one worker regressed on p620 by
21-36%, so it is not the production serial path. At sixteen workers:

| Input | p620 scalar / batched | M3 scalar / batched |
| --- | --- | --- |
| `facts8` | 0.90 / 0.66 s | 0.69 / 0.34 s |
| `files8` | 0.66 / 0.58 s | 0.55 / 0.32 s |
| 52 MB distinct symbols | 1.47 / 0.85 s | 1.54 / 0.42 s |
| 158 MB repeated pairs | 2.34 / 1.20 s | 1.96 / 0.69 s |

Keep framing ahead through bounded caller-only steps over buffered bytes.
Seven interleaved comparisons lowered p620 repeated-pair import at eight
workers from 1.67 to 0.87 s and M3 sixteen-worker import from 0.69 to
0.48 s; the other measured inputs improved too. Keep framer state separate
from worker-read cache lines and classify counters local until publication.
These changes remove observed serialization and false sharing.

The retained implementation uses one keyed symbol index. The
[M3 isolation](../bench/compare/recorded/m3-max/import-isolation/README.md)
and [p620 isolation](../bench/compare/recorded/freebsd-5995wx/import-isolation/README.md)
each compare twelve interleaved triples per cell: the pre-reduction
engine, that engine with only the symbol package removed, and the
retained engine. All 36 exact cases and 432 timed commands pass per host.
Removing the package raises sixteen-worker files/facts import paired
medians by 31.3%/23.6% on M3 and 18.8%/18.9% on the contended p620.
Complete commands pay less because import is part of their work.
The bridge to the retained engine is small on the decisive cases.

This establishes a real tradeoff independent of grouped reduction.
The package adds about 599 net library lines for wide byte hashes, stripe
lists, deferred growth and parallel publication. It also includes a
scalar in-room reservation shortcut, so the comparison does not isolate
striping alone. Scalar differences are small or noisy in these cohorts;
the earlier blanket scalar-regression claim is not a sufficient rejection
criterion. The
[isolated scalar reservation control](../bench/compare/recorded/m3-max/symbol-room/README.md)
has favorable M3
paired medians of about 1.5% on files import and 2.2% on facts import,
with broad ranges and a near-neutral integer control. That is too small
and noisy to select a default optimization from this cohort. Keep the
whole package's substantial parallel benefit and maintenance cost visible
when deciding on symbol publication; the shortcut does not settle it.

Remaining work is validation of the retained composition: serial/default
throughput, large regular files, small imports, malformed or stalled
streams, record/window limits, resumption, allocation failure and populated
store appends. The
[import ownership study](../bench/compare/recorded/m3-max/import-memory/README.md)
finds 142.9 MB of occupied representation and metadata, plus 63.4 MB of
capacity slack, at the scalar scale-10 requested peak. Parallel `facts10`
holds about 42.4 MB of input/window/map capacity at its own published
requested peak. Small lifetime
cleanup cannot plausibly deliver the former 120 MB target with this
representation. Requested capacity is not RSS, so these observations are
not a mathematical lower bound on residency. Remove that target from the
implementation queue; any smaller representation needs its own case.

Ordinary destruction returns malloc in-use bytes to the process baseline
in all 48 diagnostic runs. RSS and physical footprint remain high through
the separate one-second observations, with no effect from pressure relief.
This establishes neither a leak nor an exact allocator-retention amount.
Do not add an allocator workaround or another delay experiment on that
evidence.
See [bulk loading](../specs/design-bulk-load.md).

## 5. Partition indexed driving cursors

[Medium, components: `lib/slice.c`, `lib/exec.c`, `lib/round.c`]

The retained extension slices the first scan or lookup after a
once-executed binding-free prefix. Lookup preparation uses two bounded
forward walks, no match array and no new insert-time metadata. It reuses
the existing executor, work history, pool and merge, but introduces
prefix/slice state and driver-visit accounting into ordinary execution too.

Seven interleaved p620 runs at scale 8 and sixteen workers improved the
complete `facts-join` command from 1.05 to 1.00 s. Loading was about
0.67 s in both builds, so the isolated evaluation improvement is larger
than the whole-command percentage. Peak RSS grew by roughly 17 MB.
Serial instruction counts grew by at most 0.83% on the measured controls;
most observed cycle variation followed code layout.

Its 1,367 net physical library lines are the weakest demonstrated benefit
relative to scope among the retained stages. Keep the regression requiring
the indexed plan and a successful shared round. Before expanding slicing,
compare its complete-command and evaluation benefit with the scan-only
reference on the target and relevant controls; retain necessary fixes if
the extension is narrowed. The independent visit-list, interrupt, skew,
allocation-failure and declined-preparation checks in
[parallel work](todo-parallel.md#driver-slicing-is-not-validated-beyond-its-target)
remain validation gaps if the extension stays, along with an M3 comparison.
Tune preparation costs from those results.
Do not add deeper-driver slicing, worker affinity or another scheduling
protocol solely to raise a parallel-round count.

## Aggregate evaluation

[Medium, components: `lib/fold.c`, `lib/exec.c`, `lib/plan.c`]

Use ordinary materialized source relations and existing row folds. Retain
the shared exact accumulator semantics, including invalidity before sum
overflow, and count an unkeyed stored relation from its tuple count. EMIT
carries an output schema; the caller owns the result destination.

The producer-reduction experiment demonstrates real memory savings.
After partial reuse, two busy-machine scale-16 runs reported 0.62/0.86 s
and about 333 MB against 0.82/0.89 s and about 720 MB, with identical
answers and lower user CPU. Those runs supersede the diagnosis that a
serial or batch-draining reducer necessarily loses at sixteen workers;
they are not controlled release measurements.

The rejection is architectural. About 6,400 net library lines span source
usage analysis, Magic promotion, promised/reduced/stored bindings,
summary costing, retained-value publication and bounded partial execution.
Stored-summary `degree-count` measurements are roughly neutral. Completing
more query forms or parallel summary families increases that commitment.

A future aggregate proposal must establish a materially smaller boundary,
for example a compiler-owned producer whose ownership follows from its
construction. It is not required to optimize arbitrary named relations or
retain every experimental capability. No replacement framework is queued.

## Next investigations

[Medium, components: `lib/task.c`, `lib/merge.c`, `lib/relation.c`,
`lib/util.c`, `bench/compare/`]

The focused investigations identify small candidates and close the case
for implementing larger mechanisms on the measured workloads. Mac
background activity and substantial p620 contention limit release claims.

* **Release expired screens at the existing barrier.** The
  [M3 prototype](../bench/compare/recorded/m3-max/screen-release/README.md)
  lowers the sampled sixteen-worker charged peak from 1.324 GB to
  1.157 GB and passes a frozen-budget regression that otherwise falls
  back to serial execution. Process RSS falls only about 1.1%, and
  parallel wall time is roughly flat to slightly worse. This is a local
  admission improvement, not a large RSS or speed fix. The
  [p620 replica](../bench/compare/recorded/freebsd-5995wx/screen-release/README.md)
  reproduces the frozen-budget result, with no consistent speed gain and
  mixed RSS effects. Its sixteen-worker paired wall ratio is 1.020,
  ranging from 0.757 to 1.274 under changing load. Consider the local
  lifetime change for its admission benefit; no routing design follows
  from its limited process-memory effect.
* **Prefer the existing tags to a new container.** The
  [membership study](../bench/compare/recorded/m3-max/membership-growth/README.md)
  changes only the constructor for fresh derived relations. M3 cyclic
  and chain closure improve about 19%/14% serially and 9%/11% at sixteen
  workers, with 8/16 MiB more final storage. An equal-length executable
  control preserves the cyclic gain and leaves serial import near neutral.
  All three sanitizer suites pass. The
  [p620 replica](../bench/compare/recorded/freebsd-5995wx/membership-growth/README.md)
  passes focused checks, twelve exact full-query cases and four import
  output/count controls. Its eighty
  timing pairs finish under severe changing contention; every sample is
  retained, and the cohort's completion does not make it a release
  measurement. Paired medians improve cyclic closure about 13% serially
  and 11% at sixteen workers, and chain closure about 6%/7%, with lower
  user CPU and the same storage tradeoff as M3. Prefer this candidate
  to a new container; default adoption still needs its explicit memory
  tradeoff and the design's remaining validation. These measurements are
  of the isolated change, not a combination with screen release.
* **Stop the cached-hash/grouped-container implementation path.** Probe
  counts show useful comparison filtering from existing tags. Sampled
  growth includes scanning and placement that a hash cache would keep;
  the hash callback accounts for only 4 of 4,562 main-thread samples on
  cyclic closure and 21-23 in the short chain captures. These samples
  do not justify another canonical storage module. High-arity workloads
  remain unmeasured, which limits the conclusion; it does not create an
  automatic follow-up project.
* **Close import-memory attribution.** Keep the owned-capacity and
  teardown evidence above. No new layout or allocator intervention is
  selected from the RSS observations. The isolated scalar reservation
  shortcut also adds no implementation task on its inconclusive timing
  evidence; retain the prototype and measured limits with the import study.

Experiments use isolated branches and immutable binaries. Each has a
specific bottleneck, baseline, counterexample and rejection criterion.
The membership-locality comparison informs these experiments. A routing
proposal needs a new design decision after the measurements; it is not
an approved implementation queue.

## Review, tests and delivery

Keep specs consistent with the selected design. Removed mechanisms leave
no dormant production paths, public strategy switches or implementation
TODOs. Preserve experimental work in git, not as unused engine code.

Before tests, perform the required code, security and readability review
with concrete functions as evidence. For a nontrivial bug fix, demonstrate
a failing regression before applying the fix. Run focused checks and the
full suite after major changes, ASan for ownership and storage, UBSan for
arithmetic, and TSan for shared-worker changes. Use LSan/MSan where supported.

Commit coherent verified changes. Separate measured results from
hypotheses and incomplete validation. The performance plan advances by
evidence and architectural review, not by completing numbered stages.
