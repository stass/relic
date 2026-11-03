# Performance direction

Select further work from measured gaps, one change at a time. Keep the
[architectural north star](../specs/architecture.md#north-star): one
relational evaluator, canonical values, set relations, deterministic
publication and no third-party runtime dependency. Matching an unindexed
bulk-SQL command is not an acceptance requirement. Improvements must earn
their owners, states and coordination obligations across the whole system.

The remaining-cost investigation separates input maintenance, query
formulation and representation costs. The metadata-first probe comparison
rejects a bounded table experiment: clear serial target
gains accompany a repeatable parallel duplicate-loading CPU cost. Keep the
ordinary identifier-table representation; that result selects no follow-up
probe variant or publication redesign.

The serial-expression study keeps separate interpreter and inline-helper
sample views and tests one local checked-argument prototype. Its serial
files gain falls below the declared 5% gate, and parallel files is slower in both screen pairs. Keep the
ordinary CALL implementation. Batching operations across rows still needs
evidence that its benefit justifies the added text lifetimes, error ordering
and publication coordination. This study selects no broader executor change.

## Choosing another experiment

Identify a material operation or live allocation, the work a change removes,
its owner and failure boundary, and a control that can lose. A proposal must
state its acceptance and rejection criteria before implementation. Follow
the [measurement protocol](todo-performance-measurements.md): freeze one
candidate and reference, prove complete answers and work, screen complete
commands, then confirm a useful result with interleaved measurements and
the required correctness and supported-host checks.

Report elapsed time, user CPU and memory together. A wall-time win does not
erase a repeatable CPU regression in a control. Preserve failed and
inconclusive attempts; source-level work removal and a small diff do not
establish a worthwhile optimization. Refresh rival measurements when there
is an accepted change to compare, retaining complete output and the
different input-storage contracts.

## Use existing query structure where it helps

Explicit selective relations improve the measured facts join using ordinary
rules, indexes and Magic Sets. Preserve that result as a separate query-shape
comparison; the original benchmark remains the comparator. Where an
application uses the same selective subsets, this is an available formulation.
It does not establish an always-beneficial automatic rewrite. Such a rewrite
would need evidence about selectivity, repeated consumers, derived storage,
ordering and limits before becoming implementation work.

## Architectural boundary

Canonical IDs support targeted batching within the existing value and
ownership model. Choose a batch kernel when attribution identifies useful
work to remove and the benefit justifies its state and failure handling.
Batching does not require changing the value representation or adding an
execution mode.

A wholesale vector executor, alternate value/row representations, a general
reducer, global interner-publication redesign and a unary-range membership
index remain unselected. The unary index's roughly 16 MiB saving on files
leaves the much larger intern pool and does not apply to the facts table;
its implicit/hash states and shared preparation need broader justification.
The investigation records the residual recursion-memory tradeoff separately.
Accepting a measured gap is a valid outcome.
