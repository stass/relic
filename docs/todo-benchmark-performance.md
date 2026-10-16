# Performance work

[Medium, components: `lib/eval.c`, `lib/plan.c`, `lib/relation.c`,
`lib/import.c`, `bench/compare/`]

The [matching DuckDB source and direct phase measurements](../bench/compare/recorded/duckdb-files-analysis/README.md)
identify two substantial gaps. At one thread, Relic loads/evaluates in about
0.507/0.679 seconds against DuckDB COPY/SELECT at 0.211/0.220. At sixteen,
the intervals are 0.256/0.146 against 0.0385/0.026. These are diagnostic
intervals, with their stated boundaries and limitations. Query fusion alone
cannot close an import floor larger than the rival's whole command.

## Deliver counted results without a second store

The [shared-publication screen and phase diagnostic](../bench/compare/recorded/counted-destination/README.md)
retain the files improvement and remove the long-key loss of the
[private collector](../bench/compare/recorded/counted-projection/README.md).
High-cardinality output still loses elapsed time: constructing its second
answer relation takes 15.2–15.4 ms on the caller. The ordinary join control
also has a small unresolved loss; the screen's background activity increased,
so this remains a validation question, not a clean control result.

Complete the [counted destination](../specs/design-grouped-count.md) by using
its finished keys and counts as the answer. Retain the ordinary serial
continuation, deferred resolver, candidate merging and value publication.
The original producer still runs intact; its emitted rows are projected
without replanning away source traversal. Publish count values in
caller-side batches of at most 64 groups at every worker count, then deliver
the completed destination through the common delivery loop. Keep its checked
result map with the key/count owner; remove the serial copy into a second
answer relation. Review the before/after owners, states and obligations:
the existing SET/COUNT distinction must suffice, with no new reader framework,
worker mode or replay. Ordinary set, dense and streaming paths retain their
semantics and candidate footprint.

Prove that files scale 8 retains 1,200 keys instead of 1,799,657 producer
tuples, with no named P/G/Q storage, fold index or per-P-row worker candidates.
Check complete answers, successful IDs/order, transformed counters across
worker counts, explicit-limit refusal, later intermediate queries, constant
keys, repeats across tasks, paired key/count reservation, diagnostics and
identifier-ceiling failures with both assigned and new count values. Prove
that early callback stop follows complete value publication and that owner
cleanup holds through delivery. Verify actual worker participation. Distinguish
the counted-result failure contract from the eliminated fold's batch fences.

Reuse the frozen fixtures, oracles and baseline for two alternating pairs
per screen cell: files at one/sixteen workers, high-cardinality,
duplicate-heavy and long/computed keys, plus ordinary joins and recursion.
The control losses must disappear through the shared mechanism, without a
key-size policy or a growing collection of eligibility exceptions.

The adoption gates remain at least 20% less complete-command time in a files
cell and 35% lower files peak RSS at sixteen workers, with no reproducible
default-path or control regression. Source size is supporting evidence;
architectural review decides whether concepts and coordination obligations
are justified. A useful screen proceeds to decisive interleaved measurement,
normal/ASan/UBSan/TSan suites and a supported FreeBSD comparison under the
[measurement protocol](todo-performance-measurements.md). If the common
boundary does not simplify ownership or the implementation fails those
measurements, remove it rather than add another execution protocol.

## Remove input work at the storage boundary

After deciding the counted query, design the import change independently.
DuckDB's input path appends typed strings without Relic's global value
interning plus separate relation-set membership. Disabling either semantic
contract is not an optimization of the existing importer.

One concrete hypothesis is an implicit canonical index for a unary payload
whose stored domain IDs are `first, first+1, ...`: membership and tuple ID
follow arithmetically, so that payload needs no canonical hash allocation.
A nonconsecutive new ID requires transactional promotion to the ordinary
hash representation before append. This removes lookup, placement and
canonical storage together, unlike the rejected first-column upper bound.
It is a design investigation, not an approved representation change.

Before choosing it, establish a shared canonical preparation boundary that
sees actual ordered incoming IDs. Existing scalar, deferred and builder
reservations see different inputs, and some see only stripe counts. A flag
that each importer must maintain is insufficient. Require same tuple order,
duplicate behavior, projection/accounting, batch refusal prefix and payload
adoption across worker counts. Measure actual applicability on files and
existing-value, duplicate, mixed-type, sparse-ID and late-promotion controls.
Window-local screening, value hashing and global interning remain unless a
separate measured design removes them. Retire this hypothesis if its storage
refactor does not earn a material input and complete-command benefit.

## Follow-through

Measure accepted query and input changes together against the frozen current
engine and DuckDB, with the same complete output policy. Keep residual
expression costs explicit; vectorized functions and inline strings in
DuckDB do not by themselves justify replacing Relic's value representation.
The [dense](../bench/compare/recorded/dense-rounds/README.md) and
[serial](../bench/compare/recorded/serial-membership/README.md) recursion RSS
gaps and facts/join index costs remain separate work. The archived
[control-byte proposal](../bench/compare/recorded/files-grouping-design/control-bytes-proposal.md)
is not the active performance plan.
