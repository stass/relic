# Performance plan

[Medium, components: `lib/round.c`, `lib/task.c`, `lib/merge.c`,
`lib/relation.c`, `lib/exec.c`, `lib/import.c`, `lib/value.c`, `bench/compare/`]

Reduce the remaining cost of useful commands while keeping one understandable
execution and ownership model. Derived-relation hash tags and inline probe
helpers are the working reference, with native compilation. Their
[combined comparison](../bench/compare/recorded/m3-max/composition/README.md)
establishes the retained gains, storage costs and remaining gaps.

The [shared-collection screen](../bench/compare/recorded/m3-max/shared-collection/README.md)
closes the admitted memory prototype: an 813-line runtime reduction and 76%
lower dense peak RSS came with 2.54 times the parallel elapsed time. All three
parallel computed-value/duplicate controls also lost. The
[thread-stack diagnostic](../bench/compare/recorded/m3-max/shared-collection-waits/README.md)
finds substantial direct tuple-mutex waiting under sampling. Keep the existing
engine; this result selects no lock tuning, alternate collector or atomic-table
rewrite.

Next finish the bounded FreeBSD process-specific CPU attribution, keeping
busy-host evidence exploratory and separate from the Mac screen. Then require
a concrete removal of input/expression work before another throughput
prototype. Remaining gaps are design and attribution gates, not an automatic
queue of implementations.

## Remaining gaps and execution order

The [combined rival comparison](../bench/compare/recorded/m3-max/composition/README.md#remaining-rival-gaps)
identifies the large differences. Its two fixed-order runs rank questions;
they do not establish release performance or explain allocation ownership.
Times include loading, evaluation and output. Memory figures are process
peak RSS, in decimal units.

| Priority | Remaining gap | Next gate |
| --- | --- | --- |
| 1 | Synchronization attribution on FreeBSD | Use one source-bound cyclic4 j16 user-cycle profile per variant, with exact answers/counters and bounded target-only thread observations. A saturated host cannot establish a wall-time ranking. This diagnoses the rejected design; it cannot override the Mac loss. |
| Design gate | Files/facts throughput | Grouped files is 2.72/5.16 times behind DuckDB at one/sixteen threads; facts join is 1.74/2.52 times behind. Use the retained input/expression profiles to identify one repeated or avoidable operation and its ownership contract before coding. |
| Design gate | Parallel working memory | Cyclic closure peaks at 8.45 GB against Souffle's 0.42 GB in the rival comparison. Both worker-screen reuse and shared collection lose useful parallel speed. A new design must remove copies without recreating the observed synchronization cost. |
| Attribution gate | Remaining closure time | Serial cyclic/acyclic closure is 11%/20% behind compiled Souffle; parallel cyclic is 16% behind. Retain the accepted 29%/19% serial gains. Another change needs attribution of specific work it removes. |

Eligible bound-recursion losses have reversed. Keep those gains as regression
controls. Near ties and the roughly 2 ms serial chain-left gap do not justify
new execution machinery. The [bounded Andersen/nonlinear refresh](../bench/compare/recorded/m3-max/dense-refresh/README.md)
also favors relic's time at scale 8, one and sixteen workers. Keep those
cases as controls; their higher memory use reinforces the existing memory
question without creating another time-optimization track.

## Reference and decision gates

The initial control is the scan-only engine measured in the
[trim comparison](../bench/compare/recorded/scan-only.md) and rival refresh.
Keep its frozen source, builds, fixtures and answers. The working reference
uses derived-relation tags and inline probe helpers with native compilation.
The [tag](../bench/compare/recorded/m3-max/derived-tags/README.md) and
[inlining](../bench/compare/recorded/m3-max/probe-inline/README.md) comparisons
retain exact answers, controls and resource costs. Use the inlining cohort's
selected binary for the next candidate's control. The direct composition
retains the 32/16/104 MiB additional final storage for cyclic, acyclic and
CSPA respectively; it does not infer a memory saving from varying process
RSS. Separate cohorts' improvements do not multiply into a measured result.

Use the [measurement protocol](todo-performance-measurements.md): reuse the
[completed investigations](../bench/compare/recorded/focused-investigations.md),
resolve the specific missing attribution, test one candidate, then keep or
reject it. Record the expected removed work/bytes and the rejection condition
before implementation. A stronger architecture requires stronger evidence.
Use either host when it is usable. The
[FreeBSD trim check](../bench/compare/recorded/freebsd-5995wx/stage5-trim-fresh/README.md)
records the narrower driver's cost separately from later optimizations.
The [quiet FreeBSD check](../bench/compare/recorded/freebsd-5995wx/probe-inline-fresh/README.md)
covers inlining only. Validate selected scheduling/allocation changes on
both hosts when conditions permit. A busy-host result remains exploratory.

## Reduce parallel working memory

The [scale-4 attribution and rejected screen-reuse prototype](../bench/compare/recorded/m3-max/merge-screen-reuse/README.md)
identify the remaining boundary. In the peak round at sixteen workers,
102.4 million worker copies represent 8.52 million new tuples. Candidate
capacity is 5.18 GB, with 3.89 GB occupied, and find scratch adds 2.57 GB.
Every worker is active. The sampled charged peak precedes the process's
largest sampled resident memory; those observations are not interchangeable.

Do not continue the screen-reuse prototype or add a policy to rescue it.
Earlier screen release improves charged admission but barely changes RSS;
it is not the next capacity experiment. The current evidence does not support
another reservation/lifetime tweak as a solution to the main gap.

Reopen implementation only for one focused design that reduces actual worker
copies or bounds their storage while preserving ordered publication,
deterministic value/tuple identifiers and ordinary serial retry. State the
expected bound through gathering, growth, merge and failure before coding.
The [parallel questions](todo-parallel.md) retain the relevant evidence and
constraints; they are not a scheduling feature queue.

Any such candidate must retain useful parallel speed on dense closure and a
large low-duplicate computed-value control. Check acyclic closure and Linux
CSPA before acceptance. A lower scratch charge alone, a peak shifted into
another phase or serial replay concealing failed admission is insufficient.
The [selected-engine profiles](../bench/compare/recorded/m3-max/cyclic-profile/README.md)
assign 29.0% of parallel sampled CPU to frozen-store screening, 21.8% to
merge first-occurrence checks and 21.6% to worker candidate screening,
capacity/growth and append/copy/place. Those merge checks already run across
stripes in parallel. General executor work remains 20.1%. No explicit pool
synchronization/spin bucket receives a sample, but unknown callback work and
unsampled blocked time remain; this does not prove zero spinning. Serial
insertion owns 62.0% at one worker. CPU shares are not removable wall time.

The shared set's ordering proof and source/byte gates passed, but its measured
parallel time did not. Exact proofs and all 32 timed commands retained their
required successful shared rounds, so serial replay did not conceal the loss.
The [synchronization review](../bench/compare/recorded/m3-max/shared-collection-synchronization.md)
records the alternatives and their costs. Atomic slots alone cannot publish
resizing arrays safely. Single-writer routing needs retained output or bounded
execution waves. Semantic pre-routing can remove the global value mutex, but
restores per-part value namespaces and byte assembly, and does not remove the
cyclic workload's tuple locks.

A fresh memory proposal needs its own simultaneous-capacity ledger, occurrence
proof and function-level replacement budget. Do not carry the rejected
prototype's deletion credits over to a design that restores those mechanisms.
Use the existing frozen-executor freshness invariant; the working engine
already avoids the prototype's redundant typed lookup/canonical-ID state.
Preserve thread-disabled builds, parallel count/staging and whole-round retry.
No alternative memory implementation is admitted by these results.

## Hold the closure and build decisions fixed

Use serial cyclic/acyclic closure at scale 4 as the targets, with the
selected tags enabled. Retain the same-generation, sparse, wider-tuple,
large-storage, import/startup and Linux CSPA controls from the adoption
cohort. Its measurements do not select another canonical container or
attribute all remaining insertion time to hashing.

Keep native compilation as the default and comparison baseline. The
[optional ThinLTO comparison](../bench/compare/recorded/m3-max/thinlto/README.md)
measures the tagged engine directly. ThinLTO stays explicit because its
installed static archive requires a compatible LLVM consumer toolchain;
building another library solely to change that default adds an artifact and
test matrix. Report optional-build gains separately from default-engine
improvements. The source-ownership audit and installed-path issues remain
separate correctness work in [open work](../TODO.md).

Keep the selected probe helpers and table representation fixed during
attribution. Another closure intervention needs new attribution
and one bounded removal of work; the earlier helper profiles do not select
another container or a sequence of speculative inlining changes.

## Require a focused design for remaining throughput

The [complete-command CPU profiles](../bench/compare/recorded/m3-max/files-facts-profile/README.md)
attribute facts mainly to input: 70-77% of sampled CPU, with secondary-index
construction at 7-13%. Files splits between input (42-44%), expressions
(25-35%) and intermediate materialization (11-18%); the final fold accounts
for under 2%. These are sampled CPU shares, not elapsed phase timers or
predicted speedups. The rival plans expose different work, including earlier
facts filtering and direct files grouping, but do not choose a new ownership
model for relic.

The [literal-prefix screen](../bench/compare/recorded/m3-max/regex-prefix-screen/README.md)
does not establish a useful complete-files gain. The
[private-builder bound](../bench/compare/recorded/m3-max/builder-bound/README.md)
improves serial facts but leaves a parallel-files penalty unresolved after
its one control repeat, with additional control concerns on FreeBSD. Neither
prototype is adopted. Keep the current matcher and builder representation;
these results do not justify widening either shortcut or adding policies to
rescue it.

Further throughput work needs another specific removal of measured work with
an explicit ownership and reuse contract. Replacing a first lookup with a
scan does not prove its index is avoidable or single-use. The
[symbol-package comparison](../bench/compare/recorded/m3-max/import-isolation/README.md)
shows a real parallel benefit, but only about a 10-15% complete-command saving
in its own cohort and no consistent serial gain. It does not explain the
current 2-5 times gaps. A publication or aggregate proposal must earn its
maintenance cost through the work it removes and a smaller ownership boundary
than the withdrawn general reducer.

Keep `object(M, O) :- source(M, S), O = cat(substr(S, 0, length(S) - 2),
".o")` as a low-duplicate computed-value control. Further work on computed
strings starts with direct attribution of expression work and ordered value
publication, choosing a change from the work it removes and its ownership
cost.

Retain ordinary fold/EMIT semantics, deterministic value numbering, input
limits, stream liveness and refusal behavior. Use an integer-only import,
small input and a non-grouping query as controls. Exit only on a useful
complete-command improvement with acceptable default latency and memory, or
close the candidate. A faster import microbenchmark alone is insufficient.
The symbol-reservation room shortcut remains inconclusive, and import ownership
evidence does not justify a 120 MB target through small cleanup. Those
investigations create no further implementation work.

## Preserve gains and validate the composition

Use the [accepted composition](../bench/compare/recorded/m3-max/composition/README.md)
as the frozen reference. Its fifteen paired cells, twelve rival comparisons
and the bounded Andersen/nonlinear refresh cover the three target families,
bound recursion, sparse/same-generation, Linux CSPA and small commands. Reuse
that evidence for unchanged paths. An accepted input change requires a direct
composition check on its affected targets and controls, not another full
ranking. Add sizes or a second platform to resolve a specific scaling or
portability question; compiler policy and output policy stay fixed.

Keep exact independent answers for engine comparisons and normalized-set
checks for rivals. Descriptive composition reports do not become adoption
or cross-host release claims. Do not infer small regressions from the rough
rival ordering or multiply separate cohorts' gains into a measured result.

Reuse the full normal/sanitizer validation of unchanged paths; run the required
checks for each actual change. Preserve correctness at plan/index refusal,
rewrite limits and reuse, import fallback and input limits. Report default
latency, parallel speed, peak/accounted memory and permanent maintenance cost
together. A memory/time tradeoff is acceptable only as an explicit decision.

Close completed work and publish one combined result. Keep narrower successes
without claiming they solve a larger gap. If one focused candidate is rejected,
move to the next justified priority; an inconclusive result does not authorize
a chain of heuristics, another container or another execution model.
