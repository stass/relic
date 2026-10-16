# Performance work

[Medium, components: `lib/eval.c`, `lib/plan.c`, `lib/relation.c`,
`lib/import.c`, `bench/compare/`]

The [matching DuckDB source and direct phase measurements](../bench/compare/recorded/duckdb-files-analysis/README.md)
identify two substantial gaps. At one thread, Relic loads/evaluates in about
0.507/0.679 seconds against DuckDB COPY/SELECT at 0.211/0.220. At sixteen,
the intervals are 0.256/0.146 against 0.0385/0.026. These are diagnostic
intervals, with their stated boundaries and limitations. Query fusion alone
cannot close an import floor larger than the rival's whole command.

## Share publication before adding counting

The [private-collector experiment](../bench/compare/recorded/counted-projection/README.md)
meets the files target but fails its controls: delayed interning repeats
long-string work, and a private capacity cutoff causes discarded collection
and replay. Implement the [counted destination](../specs/design-grouped-count.md)
through the ordinary evaluation protocol, removing those obligations.

First establish one checked destination boundary for ordinary batch
publication and tuple consumption. Keep the actual serial executor continuation
and the existing deferred-round resolver, ordering and retry. Review the
before/after ownership model across evaluation, workers, merging and storage;
a wrapper around separate publication paths does not qualify. Ordinary set,
dense and streaming paths retain their semantics and candidate footprint.

Then add the counted destination: an anonymous canonical key relation with
counts, fed by the certified original producer. Project emitted rows without
replanning away source traversal. Shared workers combine key multiplicities
inside the existing candidate and merge protocol. Final count rows use one
caller-side publication stream at every worker count. Remove private typed-key
storage, separate value publication, capacity cutoffs and whole-query replay.

Prove that files scale 8 retains 1,200 keys instead of 1,799,657 producer
tuples, with no named P/G/Q storage, fold index or per-P-row worker candidates.
Check complete answers, successful IDs/order, transformed counters across
worker counts, explicit-limit refusal, later intermediate queries, constant
keys, repeats across tasks, paired key/count reservation, diagnostics and
identifier-ceiling failures. Verify actual worker participation. Distinguish
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
