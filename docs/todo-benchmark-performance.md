# Benchmark performance implementation plan

[Medium, components: `lib/plan.c`, `lib/magic.c`, `lib/import.c`,
`lib/facts.c`, `lib/value.c`, `lib/round.c`, `lib/task.c`, `lib/merge.c`,
`lib/relation.c`, `lib/index.c`]

Close the measured gaps to competing engines while keeping one relational
engine, deterministic execution, bounded failure paths and no third-party
runtime dependencies. This document orders the implementation work and its
acceptance criteria. Subsystem specs hold the resulting design decisions.

Related: [open work](../TODO.md), [comparison tables](../BENCHMARKS.md),
[all losing cells](../bench/compare/recorded/m3-max/optimization-probes/losses.tsv),
[diagnostic evidence](../bench/compare/recorded/m3-max/optimization-probes/results.json),
[parallel work](todo-parallel.md), [testing](../specs/design-testing.md).

## Execution order

Each row is a separately reviewable change or series of commits. Steps 4
and 5 form one work package for `facts-join`: loading and query execution
both need attention. Their mechanisms can be developed independently, but
their combined end-to-end result is the acceptance measurement.

| Order | Work | Main workloads | Prerequisite |
| ---: | --- | --- | --- |
| 1 | Correct the recursive planner's cardinality cliff | `tc-acyclic-bf`, `tc-cyclic-bf` | Counter reproducer |
| 2 | Preserve pending-value deduplication through the merge | `files-grouped`, computed-value workloads | Allocation and numbering regression |
| 3 | Factor eligible bound recursion into demand and answers | `chain-right`, `tc-acyclic-bf`, `tc-cyclic-bf` | Rewrite proof and design |
| 4 | Build and import facts in bulk, then in parallel | `facts-join`, `files-grouped`, `cspa-linux`, grid and tree inputs | Import ownership design; step 2 informs value resolution |
| 5 | Partition indexed driving cursors | `facts-join`, selective joins | Cursor and task design; evaluate with step 4 |
| 6 | Reduce eligible grouped pipelines directly into groups | `files-grouped`, `degree-count` | Uniqueness and usage proofs; steps 2 and 4 establish the remaining cost |
| 7 | Bound candidate storage across workers | Full closure, same-generation and large-tree workloads | Round ownership design; retain steps 2 and 5's invariants |
| 8 | Improve canonical membership locality where profiles justify it | Full closure, sparse cyclic closure, same-generation workloads | Profiles after step 7 |

The order addresses demonstrated planning and allocation waste first,
then unnecessary derivations, then serial loading, and finally the larger
execution and storage changes. Re-profile after each step: a cost removed
by an earlier step need not receive another mechanism later.

## Measurement and acceptance rules

Use the recorded M3 matrix to identify workloads and the diagnostic probes
to explain mechanisms. The matrix includes file exports for Souffle and
Nemo. The query-form probes are single diagnostic invocations, and the
load estimates compare separate processes. Neither is a timing promise
for an implemented optimization.

Before claiming a lead, establish comparable baseline and candidate runs
with the uniform discarded-stdout harness. Pin engine binaries, compiler
options, data and harness sources; keep binaries immutable throughout a
run. Store each cohort in a fresh directory. The sweep driver takes
`-j 16` explicitly on the M3; its default targets a different machine.
Use [the engine build instructions](../bench/compare/tools/README.md) to
set executable paths and compiler flags before running its preflight.

For each step, run its targeted comparisons at scales 1, 2, 4 and 8 and
at one and sixteen threads; fixed Graspan datasets run only at scale 1.
For scheduling and loading, also measure 4, 8 and 12 threads to expose
the M3's core and memory-bandwidth limits.
Use at least ten interleaved baseline/candidate samples on targeted rows,
retaining raw samples and timing spread. Run the complete comparison
matrix after a major feature and before publishing the combined result.
The sweep's three large-scale repetitions establish coverage; close
comparisons need additional targeted repetitions.

Record wall time, summed user CPU time, peak RSS and exact answer checks.
Measure loading, indexing, evaluation, merge and output separately where
attribution matters. Keep benchmark instrumentation out of timed production
paths, and distinguish live storage from allocator retention.

The performance goal is at least a 20 percent lower wall median than the
best successful rival on the targeted losing rows at scales 4 and 8,
or at scale 1 for fixed datasets, under matching conditions, with
separation supported by the samples.
This is an acceptance goal, not a projected speedup. A useful intermediate
commit can close part of a gap while the initiative remains open. Record
CPU or memory tradeoffs explicitly; an unexplained repeatable regression
on an existing winning workload blocks enabling a change by default.
Timeouts and failed verifications remain statuses, never numeric timings.

## 1. Correct recursive plan selection

[Medium, components: `lib/plan.c`, `lib/cost.c`, `lib/eval.c`]

Each delta variant keeps a delta-first and an input-first plan, and each
round runs the one estimated to do less work, as
[recursive plan selection](../specs/design-recursive-planning.md)
describes. The guarded closure in `tests/perf.sh` holds the work-counter
cliff closed: at scale 4 it scans and looks up 19.4 million tuples rather
than 94.3 million, and at scale 8 it makes 72.1 million lookups rather
than 356.6 million, against 72.2 million in textual order. Answers agree with textual-order and naive
evaluation, and answers and counters agree across thread counts. What
remains before the policy is accepted as the default:

**Retained-index cost.** An input-first plan looks its delta up through
an index on the recursive relation, and that index is built and
maintained on every insertion whether or not a round chooses the plan.
On `bench/run.sh`'s chain distances at scale 2 storage grows from
201.7 MB to 235.4 MB and the wall time from about 1.7 s to 1.9 s, with
every round choosing delta-first. The whole closure over the same chain
needs the index anyway and is unchanged in storage and lookups. The design
requires a repeatable regression of this kind to be resolved before the
policy is enabled by default. Implement
[first-use index preparation](../specs/design-recursive-planning.md#preparing-selected-plans):
retain alternative access requirements without creating their extra
indexes, include whole-relation acquisition work in selection, and build
only for selected plans before execution. Use the design's explicit
provisional key count for an absent index; preserve complete maintained
indexes after acquisition. Add a regression proving that every round of
chain distances stays delta-first and the extra index is never built.
Recheck charged storage and timings against the control, guarded-closure
work counters, and alternatives used briefly before substantial growth.

**Estimator simplicity.** Remove the evaluation-owned key-count catalog
and read existing index counts directly through the const accessor. Delete
catalog collection, sorting, searching and refresh state; keep the estimator
a pure function of const inputs. Test immediate reflection of index growth
and distinct derived/base relation identities, and measure selection
overhead on many-round and long-rule controls.

**Benchmarks.** Run the targeted `tc-acyclic-bf` and `tc-cyclic-bf`
comparisons against the rival engines at scales 1, 2, 4 and 8 and at one
and sixteen threads, under the measurement rules above, and confirm the
indexed-prefix control remains competitive.

**Adversarial selectivity and skew.** Check with skewed keys and
correlated columns that retaining an alternative does not make every large
delta select it unconditionally.

**Measurement.** Measure planning time, selection time and the added
index memory separately on long-rule and many-round workloads.

**Test coverage.** The design's validation table also asks for semantic
coverage of input-first plans (nonlinear and mutual recursion, repeated
variables, constants, assignments, aggregates and existence checks against
the references), for failure behaviour (an allocation failure publishes
no partial family, memory limits charge both candidates, a parallel
refusal retries the selected plan), and for the scale-8 counter bound,
which is too slow for `tests/perf.sh` and belongs in the benchmark
artifacts.

Keep the guarded reproducer outside step 3's factoring class so the
planner regression remains independently exercised.

## 2. Compact pending values at the merge boundary

[Medium, components: `lib/merge.c`, `lib/task.c`, `lib/value.c`]

**Evidence.** Scale-8 `files-grouped` derives 1,799,657 `kind` tuples with
two pending strings each, but only 206 distinct new strings. Workers
already deduplicate values; the merge allocates and resolves descriptors
per column occurrence.

**Implementation.** Apply [pending value compaction](../specs/design-pending-values.md):
map each referenced worker-local marker to one compact descriptor, ordered
by first occurrence among candidates. Keep columns local until a single
rewrite maps them to the ordinary value resolver's promised identifiers.
Release temporary mapping storage at the rewrite barrier; retain the
resolution map and copied bytes through commit or rollback. At sixteen
threads this rule needs at most 3,296 descriptors, instead of roughly
3.6 million.

**Design and regression.** Implement the
[pending-value design](../specs/design-pending-values.md) with the
[computed-value](../specs/design-values.md#interning-computed-values) and
[parallel merge](../specs/design-parallel.md#the-parallel-merge) contracts.
Add a failing allocation/count regression for many tuple occurrences of
few new values. Include values shared across tasks and workers, mixed
integer/string output, symbol/string use bits, all-distinct values, limit
refusal and injected allocation failure. Check first-occurrence numbering
and memory accounting against serial execution.

**Exit criteria.** Descriptor storage follows worker-distinct values;
the low-cardinality reproducer's temporary memory drops accordingly.
Identifiers, output, diagnostics and limit behavior agree across thread
counts. Measure all-distinct computed values as well, since the new maps
have a cost when little deduplication is possible.

## 3. Factor eligible bound recursion

[Medium, components: `lib/magic.c`, `lib/stratify.c`, `lib/eval.c`]

**Evidence.** The scale-8 cyclic bound query returns 8,000 answers after
about 3.19 billion derivations. An equivalent forward-reachability program
uses 406,919 derivations. The diagnostic timings are 28.014 s and 0.073 s
at sixteen threads; exact answer sets match. The acyclic probe reduces
71,093,957 derivations to 85,357.

**Implementation.** Apply
[bound query factoring](../specs/design-query-factoring.md): certify every
rule of an adornment as a positive state transition or a base over complete
inputs. For `P = R* B`, propagate `(origin, current state)` through `R` and
produce origin-associated answers through `B`, including source facts.
`R` and `B` need not coincide. Integrate certification with adornment search
and free-call subsumption, then emit ordinary rules through the existing
charged draft path. Complete and ineligible calls retain their established
behavior.

**Design and regression.** Implement the design's slot/arity preflight,
deterministic walk names, origin attribution and single rewrite budget.
Add `RELIC_F_NO_FACTOR` and `-X no-factor` consistently to configuration,
CLI help, manuals and the strategy matrix. Write the chain work-counter
regression first and confirm it fails before implementation. Use the
independent ordinary-Magic and naive references for different base and
transition relations, growing correlated requests and positive-length
paths, including an isolated seed that is not itself an answer.

**Exit criteria.** Eligible single-source closure does work proportional
to the relevant edges and reachable nodes and holds no all-pairs closure.
Compare exact sets with unfactored and naive evaluation on small random
graphs, including cycles, self-loops, additional base rules and correlated
calls from several seeds. Unsupported shapes take the established path.
Verify rewrite-limit and allocation-failure behavior, query reuse, and
identical plans and unsorted output across repeated runs and thread counts.
Do not require an optimized strategy's derivation order to match a different
strategy; their answer sets must match.

## 4. Build and load base relations in bulk

[Medium, components: `lib/import.c`, `lib/facts.c`, `lib/symbol.c`,
`lib/value.c`, `lib/relation.c`]

**Evidence.** Scale-8 loading alone takes roughly 0.7-0.8 s for
`facts-join` and `files-grouped`; DuckDB's complete sixteen-thread medians
are 0.191 s and 0.076 s. Import is also about half of the diagnostic
`cspa-linux` runtime and most of the grid and bounded-tree runtimes.

**Implementation, in commit order.** First introduce a private bulk
relation builder whose finished storage can become an empty base relation.
Make ownership transfer part of the builder/commit interface and retain
atomic append semantics for existing stores. Then batch parsing and value
resolution: identify valid record boundaries, parse chunks into private
staging, deduplicate values, resolve identifiers and publish only after
fallible preparation succeeds. Parallelize those stages once the serial
bulk path establishes their invariants.

**Design gate.** Write `specs/design-bulk-load.md`, link it from the
architecture, and reconcile facts, storage, values and parallel-loading
specs. Compare the existing striped-identifier proposal with bulk
dictionary merging that preserves global first-input-occurrence numbering.
Prefer the latter if its serial numbering cost is small; record the
measured tradeoff before selecting a representation. A change to visible
identifier encoding requires an explicit cross-subsystem decision. CSV
chunk boundaries must follow the parser's quote state, not newline alone.

**Regression and exit criteria.** Exercise empty and populated stores,
duplicates spanning chunks, quoted multiline records, malformed input,
first-error ordering, existing values and allocation/limit failures. Base
facts publish all or nothing, and repeated and parallel loads preserve
the documented identities and ordering. Instrument builder ownership to
prove that a first load does not hold duplicate full tuple arrays at
commit. Demonstrate lower serial load time and peak memory, then a
substantial parallel load gain on large inputs without slowing small
imports. Judge the competitive `facts-join` result together with step 5;
loading must also leave enough budget for grouped execution in step 6.

## 5. Partition indexed driving cursors

[Medium, components: `lib/plan.c`, `lib/round.c`, `lib/exec.c`,
`lib/index.c`]

**Evidence.** Scale-8 `facts-join` returns 571,953 rows and records zero
parallel rounds at sixteen threads. Its first operation is a constant-key
lookup; task cutting admits only a first-operation scan.

**Implementation.** Give the plan's driving cursor an ordered range that
can be divided into disjoint slices, whether it enumerates a scan or
indexed matches. Use its actual cardinality and downstream fan-out to
estimate work. Retain indexed selection. Choose a bounded representation
for lookup slices and account for its construction cost; materializing an
unbounded list of matches can consume the gain the split is meant to buy.

**Design and regression.** Update the access-path, task-cutting and
frozen-store contracts before implementation. Add a failing regression
showing substantial indexed work is left serial. Check that slices cover
the original cursor exactly once, in its logical order, with correct
semi-naive view boundaries. Include skew, empty matches, more workers than
matches, a constant-test prefix, cancellation and failure during slicing.

**Exit criteria.** The selective benchmark distributes useful work among
multiple workers and improves execution time after loading is isolated.
Query answers, work counters and serial-order commit remain identical
across thread counts. Combined with step 4, the complete `facts-join`
command meets the competitive acceptance goal.

## 6. Reduce eligible grouped pipelines directly

[Medium, components: `lib/plan.c`, `lib/exec.c`, `lib/rule.c`,
`lib/merge.c`]

**Evidence.** `files-grouped` materializes roughly 1.8 million `kind`
tuples to return 1,200 groups. The remaining profile after steps 2 and 4
determines how much of the gap belongs to grouping, expressions and indexes.

**Implementation.** Plan a grouped reduction over an eligible source
pipeline. Evaluate its filter and projection and combine per-group state
without retaining an intermediate whose rows no other consumer needs.
For `degree-count`, evaluate reusable per-key counts or grouped summaries
when their construction is cheaper than repeated tuple-chain walks.
Reuse the evaluator and exact aggregate arithmetic.

**Design gate.** Write `specs/design-group-reduction.md` and link it from
the architecture's aggregate entry. Prove eligibility from row uniqueness
and consumer requirements. Datalog counts distinct bindings, so counting
join derivations is not a general substitute. Preserve materialization
when another reader needs it, and preserve strict stratum boundaries,
empty-group behavior and the existing failure contract.

**Regression and exit criteria.** Differentially test against materialized
group rules, including duplicate-producing joins, shared intermediates,
empty groups, exact sums, overflow and mixed types. Require bounded
aggregate state proportional to groups for the proven-unique pipeline,
with deterministic output and failure behavior across thread counts.
Measure `files-grouped` and `degree-count` end to end against DuckDB after
loading is optimized; report retained base storage separately from the
intermediate storage eliminated by this step.

## 7. Bound candidate storage across workers

[Medium, components: `lib/round.c`, `lib/task.c`, `lib/merge.c`,
`lib/relation.c`]

**Evidence.** At scale 8 and sixteen threads, `tc-cyclic` takes 22.512 s
and peaks at 23.46 GiB. Souffle takes 13.616 s and 1.49 GiB; relic's
serial peak is 1.46 GiB. Per-worker screening admits up to workers times
distinct new tuples, with further merge staging and scratch.

**Implementation gate.** First measure globally unique candidates,
cross-worker repeats, pending values, per-step scratch and allocator
retention. If replication is substantial, prototype bounded candidate
batches routed to one owner per tuple-hash partition. An owner deduplicates
exactly and records the earliest logical derivation position. Final
identifier assignment and commit follow that position, independent of
arrival order. This changes deferred-round ownership and needs its own
`specs/design-round-routing.md` plus an architecture link before code.

**Required design properties.** Routing hashes pending values by semantic
value consistently across workers and confirms collisions exactly. The
store stays frozen until the deterministic publication boundary. A global
buffer budget and a draining protocol guarantee progress under backpressure;
refusal releases private work and permits the established serial fallback.
For `D` distinct new tuples, `V` distinct new pending values and total
producer-buffer budget `B`, target data storage of `O(D + V + B)` without
a workers-times-`D` term. Include ordering scratch in that bound.

**Regression and exit criteria.** Add a failing memory-scaling regression
with extensive cross-worker duplication. Test skewed partitions, computed
values, repeated schedules, interruption and allocation/limit refusal.
Peak candidate storage follows the specified bound, and lower memory is
accompanied by lower CPU and wall time on dense closure. Check low-duplicate
workloads for routing overhead. The scale-8 all-pairs tree, whose recorded
parallel peak is 109.1 GiB, is the large-capacity acceptance case.

## 8. Improve canonical membership locality

[Medium, components: `lib/relation.c`, `lib/index.c`, `lib/util.c`]

**Evidence.** The M3 scale-2 dense-closure profile places 1,739 of 2,542
evaluation-window samples in the insertion path. Batched hashing and
prefetching already exist. Re-profile full closures after the preceding
changes before selecting another representation.

**Implementation gate.** Measure probe lengths, successful versus failed
membership checks, dependent tuple reads and growth rehashing. Compare a
compact blocked/tagged membership layout with a wider cached-key or
cached-hash layout, preserving the relation's tuple identifiers and
insertion order. Retain keyed hashing and exact equality; fingerprints
only reject mismatches. Select a layout only when the measurements justify
its code and memory cost, and edit the storage/value specs in place.

**Exit criteria.** Show repeatable gains on `tc-cyclic`, `tc-acyclic`,
`tc-sparse-cyclic` and same-generation workloads, with sparse analyses,
small relations and high-arity relations as controls. Include growth
peaks, collision-heavy inputs and memory-limit behavior. If no candidate
provides a worthwhile net gain, retain the existing layout and record the
measured rejection in the design's alternatives section.

## Review, tests and delivery

Before implementation of a major feature, write its design and architecture
reference. Put cross-subsystem decisions and their rationale in the
architecture contracts. An unresolved design choice is resolved by the
stated experiment and proof before code depending on it is written.

For a nontrivial fix, write the regression first and demonstrate failure
against the unfixed tree. Launch the required code-review subagent before
the first test execution; its first pass can review the reproducer and
design. Have it review the implementation before the final test pass,
explicitly covering bugs, design, security and information leaks, plus
the style guide's readability review with named functions as evidence.
Resolve Critical and High findings before commit; fix or record Medium
findings. Watch `bcopy(src, dst, n)` argument order in ownership changes.

Run the focused regression after its fix and the full suite at every major
change. Use `make check-asan` for allocation, relation/index storage and
result-lifetime changes; add `make check-ubsan` for arithmetic and bit
operations, and `make check-tsan` for shared-worker changes. Run standalone
LSan and MSan on supported Linux configurations. Keep performance counter
bounds in regression tests and timing targets in benchmark reports.

Commit each verified increment with its design and relevant tests. Record
raw benchmark samples and reproducible commands, and remove closed work
from TODO documents. A step is complete when its correctness and mechanism
criteria pass; the overall initiative remains open until the comparable
matrix establishes the intended leads and documents any remaining gaps.
