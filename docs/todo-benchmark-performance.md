# Performance work

[Medium, components: `lib/eval.c`, `lib/import.c`, `lib/facts.c`,
`lib/relation.c`, `bench/compare/`]

Use the engine with bounded serial membership and dense parallel collection
as the native control. The [serial comparison](../bench/compare/recorded/serial-membership/README.md)
records the closure gains, variable parallel control and remaining serial
RSS costs against Souffle. The [dense comparison](../bench/compare/recorded/dense-rounds/README.md)
retains its missed 90% RSS aim and roughly twofold parallel memory gap. The
[earlier performance pass](../bench/compare/recorded/performance-pass.md)
retains the files/facts gaps and rejected approaches.

## Attribute files/facts throughput

Choose the representative files/grouping or facts/join cell by absolute
excess time and bytes, using the existing rough comparison. Confirm its gap
with the current engine and the same output policy, then attribute parsing,
value assignment, relation/index work or executor work before proposing a
change. Further dense-memory work requires attribution of the common append
and storage, which outlive the collector arrays.

## Decide and validate one change

Each candidate needs one measured owner or operation, the mechanism it
removes, a source budget, controls and a rejection criterion. Preserve full
answers, order, assigned identifiers, counters, final accounted storage and
refusal behavior. Require actual execution of the changed path. Begin with
a small fixed screen; only a useful result warrants decisive measurement
under the [measurement protocol](todo-performance-measurements.md).
Measure accepted changes together and freeze a new reference. Close an
unproductive candidate; keep unresolved toolchain validation work in the
testing category of `TODO.md`.
