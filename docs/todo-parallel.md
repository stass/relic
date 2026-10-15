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

In the current cyclic comparison, relic uses about 70.5 s of user CPU against
Souffle's 29.6 s at sixteen workers, while elapsed time is 5.94/4.70 s.
This does not identify spinning or duplicated useful work. If a candidate's
performance depends on that distinction, separate executor/screen work,
ordered merge work, pool waiting and task imbalance in the profile.

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
memory implementation needs a concrete design that meets this boundary;
the next admitted work is the remaining closure/build cost in the main plan.
