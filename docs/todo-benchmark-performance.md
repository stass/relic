# Performance work

[Medium, components: `lib/eval.c`, `lib/plan.c`, `lib/relation.c`,
`lib/import.c`, `bench/compare/`]

The [matching DuckDB source and direct phase measurements](../bench/compare/recorded/duckdb-files-analysis/README.md)
identify two substantial gaps. At one thread, Relic loads/evaluates in about
0.507/0.679 seconds against DuckDB COPY/SELECT at 0.211/0.220. At sixteen,
the intervals are 0.256/0.146 against 0.0385/0.026. These are diagnostic
intervals, with their stated boundaries and limitations. Query fusion alone
cannot close an import floor larger than the rival's whole command.

## Remove projected-row materialization

Prototype the [counted base projection](../specs/design-grouped-count.md).
One complete grouped-count query owns its producer plan and private key/count
collector, and publishes only its answer. The first implementation keeps
the ordinary expression executor and its 64-row batches. Its budget is
1,000 net runtime C/header lines, including parallel partials and integration.
Do not expand it into general source replacement or add a second expression
executor to rescue a weak result.

First prove the work removal: no `kind`/group/result-predicate materialization,
no fold index over those rows, and no per-row worker candidate retention.
For files scale 8, private grouped state should correspond to 1,200 keys
rather than 1,799,657 producer tuples. Keep partials across batches and merge
their ordered first-occurrence lists; publication preserves value IDs and
answer order. Counters report the transformed work and are checked across
worker counts, not required to mimic work the ordinary evaluator performs.

Use two alternating complete-command pairs at one and sixteen workers, plus
high-cardinality keys, duplicate-heavy input, long/computed keys and a query
outside the admitted shape. Include an ordinary recursion control for the
planner API cleanup. Compare full answers with independent oracles and the
ordinary query strategy; verify IDs/order, explicit-limit refusal, later
queries of intermediate predicates, fault recovery and actual shared work.

The adoption aim is at least 20% less complete-command time in a files cell
and at least 35% lower files peak RSS at sixteen workers, with no reproducible
default-path or control regression. These are gates, not forecast gains.
A result resembling a small hash-probe improvement does not justify this
mechanism. A useful screen proceeds to decisive interleaved measurement,
normal/ASan/UBSan/TSan suites and a supported FreeBSD comparison under the
[measurement protocol](todo-performance-measurements.md). Otherwise remove
the prototype; do not broaden eligibility or add machinery to chase the target.

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
