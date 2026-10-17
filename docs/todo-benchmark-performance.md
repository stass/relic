# Performance work

[Medium, components: `lib/util.h`, `lib/util.c`, `bench/compare/`]

The [remaining-cost investigation](../bench/compare/recorded/remaining-costs/README.md)
separates input maintenance, query formulation and representation costs.
Keep the [architectural north star](../specs/architecture.md#north-star): one
relational evaluator, canonical values, set relations, deterministic
publication and no third-party runtime dependency. Matching an unindexed
bulk-SQL command is not an acceptance requirement. Improvements must earn
their owners, states and coordination obligations across the whole system.

## One bounded table experiment

Test the [metadata-first probe proposal](../bench/compare/recorded/files-grouping-design/control-bytes-proposal.md)
against the accepted counted-query engine. Its existing tag byte can reject
an empty or unrelated slot before the identifier is loaded. The shared table
owns initialization, placement, growth and probing; callers acquire no new
maintenance rule. Preserve arrays, capacities, keyed hashes, exact equality,
identifier order, accounting and failure behavior. Update the value-table
design before implementation; the archived proposal is a design reference,
not the current representation.

Freeze one candidate and reference with the same native compiler policy.
Inspect their generated probe code, then screen complete files and facts
commands at one and sixteen workers, with duplicate/existing-value input,
successful membership and recursion controls. Use the frozen fixtures and
independent complete-answer proofs. Record user CPU and peak RSS alongside
elapsed time; do not convert avoided source loads into a speedup estimate.

The gate is at least 5% lower complete-command time in a primary files/facts
cell, with no reproducible default-path or control regression. A useful
screen proceeds to interleaved confirmation, required normal/sanitizer
checks and a supported-host comparison under the
[measurement protocol](todo-performance-measurements.md). Review the
representation's single writer and unchanged publication boundaries as well
as the tests. Keep only a demonstrated improvement. If it fails, close the
candidate without adding a different probe sequence, SIMD, prefetch policy
or symbol-publication architecture to rescue it.

## Use existing query structure where it helps

Explicit selective relations improve the measured facts join using ordinary
rules, indexes and Magic Sets. Preserve that result as a separate query-shape
comparison; the original benchmark remains the comparator. Where an
application uses the same selective subsets, this is an available formulation.
It does not establish an always-beneficial automatic rewrite. Such a rewrite
would need evidence about selectivity, repeated consumers, derived storage,
ordering and limits before becoming implementation work.

## Stop before a different engine

The unary-range membership index is a measured possibility, not the next
implementation. It removes about 16 MiB of requested canonical-index storage
on files, leaves the much larger intern pool, and does not apply to the facts
table. Its implicit/hash states and shared prepare-before-publish transition
need a larger storage refactor. Reconsider it only if its broader usefulness
and memory benefit justify those obligations.

Keep vector execution, alternate value/row representations, a revived
general reducer and global interner-publication redesign outside this plan.
The investigation records the residual recursion-memory tradeoff separately.
A failed bounded experiment may leave the remaining gap accepted; it does
not require another architecture project. Refresh rival measurements when
there is an accepted change to compare, retaining complete output and the
different input-storage contracts.
