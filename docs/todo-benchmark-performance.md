# Performance work

[Medium, components: `lib/round.c`, `lib/task.c`, `lib/merge.c`,
`lib/atomic.h`, `bench/compare/`]

The [completed performance pass](../bench/compare/recorded/performance-pass.md)
retains the accepted engine, rejected experiments and unresolved rival gaps.
Its experiments are closed.

## Reduce parallel working memory

Select **dense binary recursive relations over assigned values**. On the
retained cyclic scale-4 comparison, sixteen-thread relic uses 8.45 GB against
Souffle's 0.42 GB. The [allocation attribution](../bench/compare/recorded/m3-max/merge-screen-reuse/README.md)
finds 102.4 million worker copies of 8.52 million new tuples, 5.18 GB of
candidate capacity and 2.57 GB of duplicate-finding scratch.

Use the [dense collection design](../specs/design-dense-rounds.md): one fixed
array records the earliest occurrence of each possible pair. The ordinary
executor runs each task once. An atomic minimum preserves the earliest task;
a scalar worker sequence preserves order within that task. Sorting the
surviving pair positions gives the ordinary ordered append. No tuple payload
is copied into worker candidate buffers for an admitted dense round.

This is a proposal, not a measured improvement. The
[design model](../bench/compare/recorded/dense-round-design/README.md) checks
ordering and derives a storage ledger. Atomic contention, ordering cost,
selection on real rounds and process RSS require an implementation experiment.

The [implementation and acceptance steps](todo-parallel.md) define the one
candidate. Files/facts throughput and remaining serial closure time remain
documented gaps; neither is an additional implementation workstream.

## Preserve gains and validate the composition

Keep the selected native probe-inline executable as the control. Preserve
complete answers, order, assigned identifiers, all ordinary counters, final
accounted storage, and serial refusal behavior. Prove actual dense collection
and successful shared publication, so a serial retry cannot conceal failure.

Start with a small screen. Advance to the
[measurement protocol](todo-performance-measurements.md) and full required
validation only after a major measured memory reduction retains useful speed.
Compare against a fresh Souffle run on the same host before declaring the
selected rival gap closed. Close this candidate if its mechanism or source
budget fails; do not append a family of collector variants.
