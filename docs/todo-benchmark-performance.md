# Performance work

[Medium, components: `lib/eval.c`, `lib/plan.c`, `lib/relation.c`,
`lib/import.c`, `bench/compare/`]

The [matching DuckDB source and direct phase measurements](../bench/compare/recorded/duckdb-files-analysis/README.md)
identify two substantial gaps. At one thread, Relic loads/evaluates in about
0.507/0.679 seconds against DuckDB COPY/SELECT at 0.211/0.220. At sixteen,
the intervals are 0.256/0.146 against 0.0385/0.026. These are diagnostic
intervals, with their stated boundaries and limitations. Query fusion alone
cannot close an import floor larger than the rival's whole command.

## Remove input work at the storage boundary

The [counted query](../specs/design-grouped-count.md) retains only keys and
counts and delivers them without a second answer store. Its
[measurements and validation](../bench/compare/recorded/counted-delivery/README.md)
form the query baseline for the next comparison. Design the import change
independently; the earlier DuckDB phase measurements above identify the
input work, not the remaining query-time ratio after this change.

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

Freeze the counted-query implementation as the reference before a storage
prototype. Establish the representation and ownership invariants first,
then prove unchanged answers, identifiers, order, limits and accounting.
Screen complete commands and import intervals on the same frozen fixtures;
retain ordinary joins and recursion as controls. Set numeric adoption gates
before timing the prototype, and take a useful screen through interleaved
confirmation, required sanitizers and a supported-host comparison under the
[measurement protocol](todo-performance-measurements.md). Compare the owners,
states and caller obligations added and removed across the whole system;
source size supports that review but does not decide it.

## Follow-through

Measure a useful input change together with counted queries against the
frozen reference and DuckDB, with the same complete output policy. Keep residual
expression costs explicit; vectorized functions and inline strings in
DuckDB do not by themselves justify replacing Relic's value representation.
The [dense](../bench/compare/recorded/dense-rounds/README.md) and
[serial](../bench/compare/recorded/serial-membership/README.md) recursion RSS
gaps and facts/join index costs remain separate work. The archived
[control-byte proposal](../bench/compare/recorded/files-grouping-design/control-bytes-proposal.md)
is not the active performance plan.
