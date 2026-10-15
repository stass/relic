# Parallel memory and execution questions

[Medium, components: `lib/round.c`, `lib/task.c`, `lib/merge.c`, `lib/pool.c`]

Use these questions within the [performance plan](todo-benchmark-performance.md),
which sets priority and acceptance. The target is useful parallel speed with
controlled working memory, rather than a fixed speed-up ratio or another
scheduling feature queue.

## Locate the worker-dependent storage

The [current scale-4 comparison](../bench/compare/recorded/m3-max/rough-rivals/README.md)
puts cyclic closure at 8.47 GB peak RSS with sixteen workers, against
0.45 GB at one worker and Souffle's 0.42 GB at sixteen. Acyclic closure and
Linux CSPA also retain large parallel memory costs despite faster elapsed time.

The [earlier scale-2 attribution](../bench/compare/recorded/m3-max/retained-attribution/README.md)
locates its sampled charged peak before staging: candidate storage and find
scratch overlap, with about fourteen worker copies per unique new tuple in
that round. Releasing complete buffers only after staging misses that peak.
The [screen-release experiment](../bench/compare/recorded/focused-investigations.md)
improves charged admission but removes little process RSS. Use those findings
to narrow the current investigation, not as an attribution of every byte in
the larger process peak.

Measure occupied entries and columns, reserved capacity, scratch and growth
overlap at the actual lifetime boundaries. Distinguish data replicated across
active workers from reservation for workers that receive little or no work.
`worker_share()` reserves before task assignment; rounds with fewer pieces
than workers and rounds with few new tuples are useful controls for unused
capacity. Keep requested/live bytes, charged storage and allocator/RSS effects
separate. Transient growth is a documented accounting limitation; it does not
by itself justify a new growth policy or allocator workaround.

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
output order, including computed values and allocation refusal. A narrow
admission improvement can stand on its own without claiming to solve
worker-dependent candidate duplication.
