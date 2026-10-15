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

A new proposal must reduce or bound stored copies within one understandable
ownership model. Preserve logical occurrence order and value identity before
publication, and retain ordinary whole-round serial retry. Derive its bound
through growth and refusal, not only at a successful barrier. Use the retained
reporter for the affected boundaries; there is no need to repeat the initial
attribution without a change that makes it stale. Keep occupied bytes,
capacity, charged storage and process RSS distinct.

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

A scan with few tuples limits the number of pieces even when work beneath
it is large. Measure that effect on a demonstrated target before changing
partitioning or planner choices. Worker wake-up and sparse-stripe growth
measurements matter only if the selected workload's profile makes them
material. No threshold sweep, deeper slicing, component-concurrency feature
or replacement routing protocol is a prerequisite for the memory work.

## Acceptance

Follow the [memory decision](todo-benchmark-performance.md#reduce-parallel-working-memory):
reduce the dominant working-storage term while retaining useful parallel
speed. Require a successful shared merge, so serial replay cannot conceal
failed admission. Preserve ordinary retry, interruption, value numbering and
output order, including computed values and allocation refusal. Reopening
memory implementation needs a concrete design that meets this boundary.
The next design review asks whether one round-owned set with earliest-task
ownership and ordered worker references can replace replicated tuples and the
merge's duplicate tables. Require concrete capacity/phase bounds, borrowed-byte
cleanup order and a source deletion budget before a prototype. Shared mutation
and mutex ownership must replace the current isolation contract explicitly;
the CPU profile alone does not select them.
