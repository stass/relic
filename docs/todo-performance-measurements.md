# Measuring a performance change

This protocol supports the [performance plan](todo-benchmark-performance.md).
It keeps each comparison small enough to answer a decision, with source,
answers, resource costs and uncertainty preserved together.

## State the decision before building

A proposal names the bottleneck, the work or live allocation it removes,
its ownership/failure boundary, one target and at least one control that
can lose. State the result that would reject it. Apply the
[architectural complexity guidance](../specs/style.md#architectural-complexity):
compare the concepts, states, owners and coordination obligations introduced
with those eliminated across the system, including failure and fallback paths.
Source size supports that review; it is not an adoption gate. A larger module
can earn its place by simplifying the surrounding architecture.

Choose one intervention per candidate. Reuse existing fixtures, independent
oracles and reviewed collection tools. A new profiler, container or general
benchmark framework needs its own reason; measurement does not authorize
production instrumentation or another execution path.

## Use three levels of evidence

| Level | Scope | Exit |
| --- | --- | --- |
| Diagnose | Reuse the rough comparison to select a gap; measure one representative size, relevant worker counts, phase work and live owners. | Identify a material operation/allocation the proposed change actually removes, or stop. |
| Decide | Immutable reference/candidate; at least ten interleaved AB/BA pairs per decisive cell, with exact answers and relevant controls. | Keep, narrow or reject the candidate. An effect inside control spread is inconclusive. |
| Validate the composition | Only accepted changes together; representative complete commands, a larger storage case and supported-host checks. | Record the combined tradeoff and freeze a new reference. Do not extrapolate by multiplying isolated wins. |

A rough ranking can use two timed runs per engine. Once a gap is clear,
spend further measurement on its missing attribution and candidate controls,
not another broad ranking. Report absolute excess seconds and bytes alongside
ratios. Large relative losses on tiny commands need proportionate remedies.

Diagnostic binaries never supply performance timings. A high inclusive
sample count does not identify the work beneath it: separate hashing from
growth placement, expressions from publication, and waiting from useful
worker work. Count software operations and hardware samples separately.
Check hardware event metadata and sampled user/kernel modes against the
requested scope; a command accepting a qualifier does not prove it applied it.

## Select cells by the question

| Question | Initial target and controls | Useful evidence |
| --- | --- | --- |
| Parallel working memory | Cyclic closure at scale 4 and one low-duplicate/computed-value case; one/four/sixteen workers. Validate accepted changes on acyclic closure and Linux CSPA. | Occupied/capacity bytes per owner, copies/unique candidates, phase overlap, admission, shared work, wall/user time and RSS. |
| Full-closure time | Serial cyclic/acyclic scale 4; same-generation, sparse and wider-tuple controls, large storage and import/startup. | Tuple work, comparison/filter cost, growth and output; complete time and persistent/peak storage. |
| Files/facts throughput | Files grouped and facts join at scale 8, import and complete commands, one/sixteen workers; numeric import, small input and a non-grouping control. | Direct phase attribution, expression/join/fold work, materialized rows, output, identifiers, limits and complete-command benefit. |
| Build policy | A fixed engine and fixture set, changing only compiler/LTO/alignment policy. | Complete commands and code-layout sensitivity, with text size and toolchain scope. |

These are starting cells, not a Cartesian product of every workload,
scale and worker count. Add a larger size or intermediate worker count
only to resolve a specific scaling, memory or scheduling question. Reuse
valid unchanged correctness evidence; do not rerun an unrelated matrix
because another experiment exists.

## Freeze and verify

Record the revision and patch, compiler/configuration, actual source and
binary hashes, fixtures, oracles, commands and output sink. Use immutable
executables with equal-length paths for close comparisons. Keep compiler,
LTO and alignment fixed. Use ordinary randomized context hashes; no
production seed, hash, slot or probe oracle is exposed.

Compare complete answers, preserving duplicates in actual output. For
set-valued imports, the independent expected set deduplicates the input;
the actual readback does not hide duplicate answers. An import count is
a count control, not a full content proof. Keep readback-query work outside
pure import measurements.

Record ordinary work counters, mechanism events, wall time, user CPU,
peak RSS and final accounted storage. Diagnostic charged peaks and
requested live capacities are separate quantities. Compare components at
the same phase; never sum maxima from unrelated lifetimes or call an
allocator/RSS difference a demonstrated leak.

## Admit the host, then run the fixed cohort

Only one controlled timing cohort runs on a host. Builds, sanitizers,
profilers, large verification queries and other agent experiments wait.
Record host activity before, during and after the run. Use both M3 and
p620 for scheduling, allocation or locality tradeoffs when the hosts are
usable; a busy host can supply exploratory evidence, not release timing.

Do not start a long cohort on a CPU-starved host. If conditions deteriorate
enough to make the decision unanswerable, preserve the entire attempt and
mark it incomplete or inconclusive. Never discard slow samples, retry
individual losing pairs or silently turn a partial cohort into a complete
one. Monitoring intervals requested from the OS are not guaranteed sample
durations under starvation.

Report paired wall ratios and their full spread, alongside absolute
medians and memory/CPU observations. Keep control regressions visible.
A persistent unexplained default-path regression blocks promotion. A
quiet repeat or one targeted code-layout control can resolve a small
ambiguous effect; if it remains ambiguous, close the experiment rather
than grow a tuning campaign.

## Review and deliver

Review bugs, design, security/information leaks and readability before
tests. Preserve or add independent behavioral regressions; demonstrate a
nontrivial bug before fixing it. Run the full suite after major changes,
ASan for ownership/storage, UBSan for arithmetic and TSan for shared-worker
changes, with supported leak/uninitialized-memory checks as applicable.

Commit one coherent selected change with its measured tradeoff and limits.
Archive rejected prototypes as evidence, without dormant production modes.
Delete closed TODOs. A host or validation gap stays explicit; it does not
become an invented performance claim or an automatic new design project.
