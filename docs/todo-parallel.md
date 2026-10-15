# Parallel memory and execution questions

[Medium, components: `lib/round.c`, `lib/task.c`, `lib/merge.c`, `lib/pool.c`]

Use these questions within the [performance plan](todo-benchmark-performance.md),
which sets priority and acceptance. The target is useful parallel speed with
controlled working memory, rather than a fixed speed-up ratio or another
scheduling feature queue.

## Reduce the stored worker copies

The [scale-4 attribution](../bench/compare/recorded/m3-max/merge-screen-reuse/README.md)
places the sixteen-worker dense round's sampled charged peak at find:
5.18 GB of candidate capacity plus 2.57 GB of scratch. Its 102.4 million
worker candidates represent 8.52 million new tuples, about twelve copies
each. All sixteen workers run eight tasks and hold about 6.4 million
candidates each. The dominant copies are not unused reservation for idle
workers. Candidate slack is still measurable, especially at four workers,
but occupied representation alone is 3.89 GB at sixteen.

Worker-screen reuse removes redundant find tables and lowers process peak RSS
about 21%, but slows the target 31% and leaves candidate copies intact. That
prototype is rejected. Earlier screen release changes admission more than
RSS. Neither experiment justifies another lifetime tweak, filter policy or
lookup variant as the next memory intervention.

The [shared collector](../bench/compare/recorded/m3-max/shared-collection/README.md)
reduces the dense process peak from 8.48 to 2.02 GB and runtime source by 813
lines, but takes 13.65 rather than 5.37 seconds at sixteen workers. Its three
parallel controls also slow by 35-70%. The exact proofs and timed counters
confirm successful shared work. This replacement is rejected as well.

A new proposal must reduce or bound stored copies within one understandable
ownership model while retaining useful parallel speed. Preserve occurrence
order, value identity and whole-round serial retry. Derive simultaneous
capacity through gathering, growth, merge and refusal. Keep occupied bytes,
capacity, charged storage and process RSS distinct; do not inherit deletion
credits for mechanisms a new design restores.

## Account for the work behind parallel elapsed time

In the [current cyclic comparison](../bench/compare/recorded/m3-max/composition/README.md#remaining-rival-gaps),
relic uses about 63.5 s of user CPU against Souffle's 29.5 s at sixteen
workers, while elapsed time is 5.43/4.67 s.
The [selected-engine CPU profiles](../bench/compare/recorded/m3-max/cyclic-profile/README.md)
put 29.0% of parallel sampled CPU in frozen-store screening, 21.8% in the
merge's parallel first-occurrence checks and 21.6% in local candidate
screening/growth/append. General executor work remains 20.1%. No sample gets
the explicit pool synchronization/spin owner; unknown callback work and
unsampled blocked time prevent a claim of zero waiting or spinning. These
CPU shares do not predict a replacement's elapsed gain.

The [separate thread-stack diagnostic](../bench/compare/recorded/m3-max/shared-collection-waits/README.md)
finds direct collector tuple-mutex waits in 17,866 of 38,289 shared observations,
versus none of 14,946 native observations. Pool condition waits are separate.
These are perturbed stack appearances with unequal coverage, not CPU shares,
blocked durations or an attributed fraction of the original slowdown. The
cyclic rule propagates assigned values and bypasses the global value lock.

Finish one bounded exploratory FreeBSD user-cycle profile per variant using
process-specific PMCs, with exact work/source bindings and explicit sample
loss/unwinding limits. CPU samples cannot measure sleeping time; any target
thread-state observations remain a separate measure. Do not rank wall times
under host saturation or expand this diagnostic into a tuning cohort.

The [synchronization review](../bench/compare/recorded/m3-max/shared-collection-synchronization.md)
explains why atomics need safe publication/growth, and why single-writer routing
needs buffers or execution waves. Partition-local values are coherent but add
namespaces, copying and up to P-fold occupied payload before capacity slack.
None is an approved implementation. Retain exclusive ownership in the existing
post-gather phases and avoid a new scheduler without a demonstrated need and
replacement budget.

## Acceptance

Follow the [memory decision](todo-benchmark-performance.md#reduce-parallel-working-memory):
reduce the dominant working-storage term while retaining useful parallel
speed. Require a successful shared merge, so serial replay cannot conceal
failed admission. Preserve ordinary retry, interruption, value numbering and
output order, including computed values and allocation refusal. Reopening
memory implementation needs a concrete design that meets this boundary.
A different ownership design must earn a fresh source, byte and lifetime gate;
neither the CPU profile nor the existence of an atomic primitive selects one.
The working engine already relies on deferred pending values being fresh as
their type. Keep that derived invariant instead of adding state for arbitrary
pending inputs at a collector restricted to frozen execution. Generic serial
and import descriptors retain their broader normalization contract.
