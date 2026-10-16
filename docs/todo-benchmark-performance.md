# Performance work

[Medium, components: `lib/util.h`, `lib/util.c`, `bench/compare/`]

The [files/grouping investigation](../bench/compare/recorded/files-grouping-design/README.md)
selects the next experiment. The current native engine takes about 1.19 s
against DuckDB's 0.44 s at one thread, and 0.40 s against 0.079 s at sixteen.
Input, expressions and intermediate materialization dominate retained CPU
profiles; folding is small. Use the engine with bounded serial membership
and dense parallel collection as the reference. A bounded current-source
diagnostic finds that control bytes can decide 27.8 million of 35.1 million
serial files lookup-slot visits before reading identifiers; this is an
operation count, not a measured speedup or cache-miss reduction.

## Screen control-byte probing

Implement the [control-byte design](../specs/design-control-bytes.md) in an
isolated candidate. Reserving zero for emptiness lets the existing tagged
probe reject empty or mismatching slots before reading identifiers. Keep
linear placement, existing arrays, capacities, allocation and owner choices.
The budget is at most 100 net runtime lines in `lib/util.h` and `lib/util.c`,
with no new fields, allocations or public flags. Do not combine it with SIMD,
prefetch changes, a new hash or grouping reduction.

Add the design's collision, lifecycle and refusal tests; compare exact output,
order, identifiers, all 11 public counters and final accounted storage. Check
native code for the conditional identifier load and any added work when
opening an untagged probe. Diagnostic operation counts are not performance
samples or hardware cache-miss counts.

Begin with two fixed alternating pairs for files-grouped scale 8 at one and
sixteen workers. Use facts-join scale 8 at both counts, serial cyclic closure
scale 4 and a small repeated import into a populated base relation as controls.
The repeated import exercises successful untagged probes. Freeze equal-build
reference/candidate identities and independent complete-answer proofs before
timing. Stop if
neither files cell shows a plausible 5% complete-command gain, or if the
serial default or a control has an unexplained repeatable loss.
A tiny hash-only benchmark does not rescue a failed complete-command screen.

## Decide on complete workloads

A useful screen warrants ten interleaved pairs for the target and controls
that can lose, following the [measurement protocol](todo-performance-measurements.md).
Include an import-only readback proof, numeric import and wider/computed-key
coverage; use a larger storage case only to test a concrete capacity concern.
Require a repeatable gain of at least 5% in one files cell, no reproducible
serial-default or control regression, unchanged final accounted bytes and
no unexplained RSS increase. Record user CPU, all attempts and full paired
spreads. An unresolved effect inside control/code-layout variation rejects
the candidate.

Run the normal and ASan/UBSan/TSan suites and supported-host checks before
adoption. A rough FreeBSD comparison tests the locality tradeoff on a second
architecture; label a contended run exploratory. One targeted quiet repeat
may resolve ambiguity. A remaining contradiction closes the candidate rather
than starting a tuning campaign. Keep or remove the complete candidate, then
freeze the accepted composition.

## Remaining gaps

This experiment changes throughput and keeps the materialized relations, so
it does not resolve files/grouping's RSS gap. A design that removes those
relations needs its own evidence and a substantially simpler ownership model
than general grouped reduction. Facts/join retains input and index costs.
The [dense comparison](../bench/compare/recorded/dense-rounds/README.md) retains
the parallel memory gap, and the
[serial comparison](../bench/compare/recorded/serial-membership/README.md)
retains the rival RSS costs after the closure gains. Choose a further owner
only after this candidate is decided; none of these gaps authorizes another
optimizer framework.
