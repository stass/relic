# Direct grouped reduction

This document defines physical plans that compute aggregate groups once,
either directly from a certified producer or from an existing complete
relation. It spans query preparation, planning and evaluation. A direct
reduction removes an intermediate row store only when distinctness and
all its uses are proved; an immutable summary makes subsequent folds key
lookups. The ordinary materialized group and row-walking fold remain the
reference and the fallback for an ineligible plan.

Related: [architecture](architecture.md#aggregates),
[aggregate semantics](language.md#aggregates),
[aggregate compilation](design-aggregates.md),
[planning](design-planning.md), [evaluation](design-eval.md),
[Magic Sets](design-magic.md), [parallel evaluation](design-parallel.md),
[driving cursors](design-driver-slicing.md),
[pending values](design-pending-values.md),
[memory](design-memory.md), [testing](design-testing.md).

## Model

A **source** is the logical relation an aggregate folds, including every
column that distinguishes its rows. A **family** identifies that source
and one grouping key mask. It contains the aggregate signatures its
consumers need: function and aggregated column. A `count` needs no
signature, since every entry keeps its group's row count, and a family
records only whether some consumer counts. Identical signatures share
state. Outer slot numbers are bindings of a
consumer, not part of a family's identity.

The run's use inventory supplies a family's complete signature set before
construction. A published family never grows another accumulator in place.
Magic copies of a fold reuse that signature; an unexpected unsupported
use cannot read a removed source through an incomplete summary.

A **summary** holds one entry per nonempty key group. An entry contains
its key, row count, the requested accumulators, and enough information to
give the ordinary fold's outcome. A missing key denotes an empty group.
The summary is immutable before any consumer can read it.

There are two ways to build it:

| Physical source | Input to reduction | What remains stored |
| --- | --- | --- |
| Certified producer | The ordinary executor's emitted rows, proved to be distinct | Group state and any required key projection; the intermediate source rows have no store |
| Complete relation | Each stored tuple exactly once | The original relation and an optional summary reused by its folds |

These modes share accumulator operations, semantic key equality, value
publication and summary lookup. They differ in the proof that their input
is a set: a relation supplies that property already; a producer requires
a certificate. Neither mode turns join multiplicity into aggregate
multiplicity.

```text
logical rules, aggregate signatures and consumer uses
    -> certify and choose physical sources for one evaluation scope
    -> complete lower inputs
    -> execute a distinct producer, or visit a stored relation
    -> reduce into private group state
    -> reconcile keys and retained values; publish an immutable summary
    -> ordinary outer plans read a summary at their AGGREGATE operations
```

### Filter, projection and grouping

The representative pipeline is:

```datalog
kind(P, D, E) :- file(P), match(P, "/test_[0-9]+[.]") < 0,
    D = substr(P, 0, 7), E = substr(P, index(P, ".") + 1).
group(D, E) :- kind(_, D, E).
grouped(D, E, N) :- group(D, E), N = count { kind(P, D, E) }.
```

`file/1` is a set, and the producer retains `P`, so no two accepted input
rows produce the same `kind` tuple. The grouping key is columns `{1,2}`.
The reducer executes the filter and both assignments normally, counts
each emitted row under `(D,E)`, and retains keys in their first source
occurrence order. It supplies `group/2` with those distinct keys and the
fold over `kind/3` with the counts. It does not store `kind` rows or build
an index over them.

This is a proof about the plan, not about the spelling of `file`, `kind`
or `group`. Dropping `P` from the producer head destroys the proof: many
files can then produce one source tuple. Counting those derivations would
be wrong. Equal aggregate values in distinct source rows, on the other
hand, still contribute separately to a sum or count.

### Reusing complete inputs

In a degree query, `edge/2` remains a base relation. Outgoing and incoming
counts are families keyed on columns `{0}` and `{1}`. A chosen summary
build visits each edge once and updates both families. The existing
`node/1` relation and the outer degree rule retain their execution order;
an endpoint missing from one family receives zero from that fold.

This mode avoids repeated chain walks and, when no other reader needs
them, secondary indexes built only for folds. It does not imply that
precomputation pays for a single selective fold. Selection accounts for
the full source visit, group storage and the indexes the ordinary plan
actually needs.

## Invariants

* **Every contribution is one distinct source row.** The complete-row
  mode reads a set; the producer mode consumes only a certified distinct
  plan executed once. Accumulators do no hidden per-group row deduplication.
* **A source loses its row store only when every executable use is
  supported.** A fold and an exact projection of the same keys can use
  the reduction; a consumer of source rows requires materialization.
* **Logical completeness precedes folding.** Reduction consumes the
  entire source, with complete lower inputs, before publishing a summary.
  No aggregate reads a partially accumulated group or a recursive delta.
* **Building an outcome does not report it.** Type-invalid groups and
  overflowing totals are observed only by an outer fold that actually runs.
* **The executor remains a reader.** It produces ordinary batches into a
  reducer taker, or reads an immutable summary. It does not insert into
  a group map, publish values or call diagnostics from a worker.
* **No row representation is counterfeited.** A summary cannot be
  passed to a scan, lookup, membership test or ordinary relation reader.
* **Thread count changes execution placement, not physical selection.**
  Producer order, retained-value numbering, key order, answers and
  successful semantic counters agree across worker counts for one strategy.
* **Group state has one global owner.** Parallel scratch has a fixed total
  budget; there is no complete group table retained per worker.

## Eligibility and consumer ownership

### A proof of distinct producer rows

The source has one nonrecursive producer rule, no source facts to union
with it, and a component containing no other predicate. Its inputs are
complete before its pass. Its plan contains no aggregate operation;
ordinary generators, tests and assignments use the existing executor.
The whole producer runs once, not once per requested key.

The completed plan must pass `relic_plan_derives_distinct()`:

* no generating `SCAN` or `LOOKUP` has an anonymous or ignored column;
* its head reads every slot a generator binds;
* all other operations produce at most one continuation for each binding.

The existing proof applies to computed columns too: two paths differ in
a generator-bound slot retained verbatim in the head, regardless of
whether an assignment is injective. A test can reject paths and cannot
duplicate them. Repeated variables match through the ordinary binding
rules. An `EXISTS` is a test, but a wildcard generator under
`-X no-exists` can make the same logical rule ineligible.

There is no inferred functional dependency from column names, observed
data or an index's average fan-out. Joins whose full distinguishing
bindings survive in the source head can qualify; an apparently unique
projection without that proof cannot. Multiple producers and source
facts stay materialized even when a sample contains no duplicate.

Recognition first builds a structural candidate. The certificate is
confirmed against the actual completed producer plan before any source
row is consumed. A rejected certificate selects the ordinary source and
all its ordinary consumers before execution. No midstream switch tries
to reconstruct rows already discarded.

### Prove uses before suppressing a source

A scope builds one indexed inventory of semantic read sites from the
program and current query, including the query's group rules. Compiled
aggregate-body descriptions are not counted as a second executable read
when a fold already denotes them. The inventory is built once in linear
space in those read sites, rather than rescanning every rule for every
source. Considering all program rules is conservative: an unused ordinary
reader can prevent removal without endangering correctness.

A removable source has at least one fold consumer. Every fold over it
uses the same key mask, though functions and aggregated columns may
differ. Every other consumer is a **key projection**: one positive atom
over the source, no other literal, a head containing each key variable
once, and all non-key columns ignored. Key arguments are distinct named
variables, without constants or extra equality constraints. A permutation
of the key columns in the head is recorded explicitly. Each projection
has one producer, no base facts and a nonrecursive component of its own.

Projection relations remain ordinary relations. Their other consumers
need no special treatment because the projection retains exactly its
original tuples and first-occurrence order. A direct query over the
source, negation of it, a membership test, an extra ordinary reader, a
second grouping mask or an unsupported projection preserves the source
row store. Such a source can still have summaries built over its stored
rows when worthwhile.

Compiler-generated group predicates qualify under the same proof. A body
such as `count { edge(a, _) }` denotes one empty row when any match exists,
not the number of matching edges. Its generated producer must establish
distinctness itself; recognizing a fold or a projection does not bypass
that obligation. Keys and local columns come from the compiler's group
schema, not from whichever columns an aggregate happens to inspect.

### Query scope and full materialization

Removal of a named source is confined to a query evaluation that starts
without a current whole-program fixed point. That run already leaves
partial derived state and records no materialization currency. The next
query or `relic_materialize()` discards it through the ordinary boundary.
A later query for `kind` therefore computes the actual `kind` relation;
it cannot find an empty placeholder declared complete.

Whole-program materialization and `-X no-magic` keep named source and
projection relations. A query over an already current program keeps them
too. These modes can use summaries over stored rows. A compiler-generated
group owned by a query can be reduced without deleting any program
relation, because only that query owns its run and consumers. Program
group relations remain materialized on the whole-program reference path.

The choice and certificates belong to one evaluation run. They are
recomputed after program or fact changes; no prepared query holds a
certificate across runs. The context remains busy through preparation,
reduction, consumption and cleanup.

## Scheduling and Magic Sets

The logical dependency graph and its strict aggregate edges do not change.
The physical action at a certified producer's scheduled turn builds a
summary instead of inserting its source rows. Each accepted key projection
is filled at that projection's scheduled turn from the summary's ordered
keys. Its original row-producing rule is suppressed by that action, so
the two ways cannot both run. Aggregate consumers keep their own schedule
positions and complete lower inputs.

For named-source removal, the current query must actually reach a fold
over the source. After `relic_magic_find_complete()` discovers this but
before it evaluates any complete component, query preparation inventories
uses and records structural candidates. Their reached key projections
are included in the complete part and closed over their dependencies.
The final distinctness check occurs when the source's lower inputs are
complete and its actual plan is built. A passing certificate installs
reduction and projection actions; otherwise those components retain their
ordinary row plans. Promotion to complete evaluation is still safe for
that fallback because a projection computes the same complete key set.
Unreached projections are not run.

This preserves Magic's two evaluation phases. It does not require running
adornment search before the complete inputs exist. A generated rule may
copy a certified aggregate literal with renumbered outer slots; the
source, key mask, function and column identify the same immutable family.
The use proof covers these copies because copying a fold changes no
source access, and the supported projections are read completely. A new
rewrite that changes an access shape must invalidate that certificate.

Logical completeness and physical row availability are separate facts.
Every executable plan resolves sources through one evaluation-owned
binding table. A row binding supplies a relation; a fold binding supplies
either a relation access or a completed summary with a signature offset.
Ordinary atom planning requests a row binding and cannot accept the
summary form. A missing certified binding is an internal error before
execution, never an empty relation supplied as a substitute.

Plan-order statistics over a reduced logical source use its recorded row
count where needed and conservative estimates for unavailable column
statistics. They never inspect a nonexistent row store. Aggregate cost
recipes classify a summary read as one key probe plus fixed accumulator
finalization; they request no secondary index. Other consumers retain
their own access requirements. Recursive plan selection remains pure and
uses already chosen, immutable physical sources.

A Magic abandonment releases reduction handles and query-group plans
before the ordinary discard and materialized fallback. No summary or
projection plan survives that discard. A partial optimized run cannot
mark the whole program current.

## Aggregate state and lookup-time outcomes

### One accumulator implementation

The accumulator operations used by `fold_row()` and `fold_value()` live
in a shared internal module. It supports initialization, adding a
semantic value, combining two disjoint row sets, and obtaining a completed
outcome. Ordinary row-walking folds use it too. The executor owns note
and error reporting at the aggregate's original source location.

| Function | Retained state | Combination and final outcome |
| --- | --- | --- |
| `count` | Number of source rows | Add counts; an absent key gives zero |
| `sum` | Exact two-word signed total and an invalid-type flag | Add exact totals and OR invalidity; invalidity gives no value, otherwise check `int64_t` fit only at the consumer |
| `min`, `max` | Whether any row exists, type information, extreme value and its first logical occurrence | Combine emptiness and type validity, then compare in the language order; mixed types give no value |

A family's row count is shared by its signatures. Invalidity is per
signature: a noninteger sum does not invalidate a count of the same group.
An invalid group is still present for key enumeration and other folds.
Type classification uses value accessors, not a second interpretation of
domain tag bits.

The summary records outcomes without reporting them. A group whose exact
sum is outside `int64_t` may never be requested; a later outer filter may
reject it before the aggregate operation. Precomputation must not turn
that program into a failure. At the operation, lookup and finalization
produce the same value, no-value result, mixed-type note or overflow error
as walking the materialized group there. The ordinary result-slot binding
or equality comparison then runs unchanged.

Invalid type takes precedence over sum range. A group containing a
noninteger has no sum even if its integer subset totals outside `int64_t`.
No partial sum is an overflow. `min` and `max` over an empty group have no
value and no note; over mixed types they have no value and the ordinary
aggregate note. A count or sum over an absent key is zero, even when the
outer key contains a computed value not present in the value table.

Summary construction consumes the whole producer even after one group
becomes invalid. A later producer expression can fail, and other groups
or signatures still need their rows. Producer errors and notes retain
their original rule and schedule position; they are different from the
deferred outcomes of the aggregate consumers.

### Exact arithmetic and the source cardinality bound

`relic_sum_t` remains two unsigned 64-bit words. Combining accumulators
adds their low words, carries into their high words, and performs no
signed overflowing operation or dependence on `__int128`. A range test is
made once on a completed, requested, type-valid group.

Eliminating a source's store must not erase the bound behind this proof.
The reducer tracks its total distinct source rows and enforces
`RELIC_TUPLE_MAX`, the ordinary relation identifier ceiling, independently
of configurable storage limits. It checks before accepting a row beyond
that ceiling. A stored source already obeys it. Consequently each group
has fewer than `2^32` addends, every partial total is within `2^95` in
magnitude, and combining partitions cannot wrap 128 bits.

Parallel partial counts are checked before combination; an attempt that
exceeds the source ceiling retries through serial reduction, which reports
the ordinary source-capacity refusal at its logical row. A test build can
lower the ceiling to exercise this without billions of tuples. This
logical ceiling does not charge removed rows to `max_tuples`: that limit
continues to bound tuples actually stored across relations.

## Building and owning group state

### Consume ordinary executor batches

A reducer is a batch taker, alongside insertion, streaming and candidate
collection. The producer's plan still ends in `EMIT`; expressions and
filters execute once through the existing interpreter. Planning validates
the head against the reducer's tuple schema as it validates a relation
target's arity; emitting a row requires that schema, not a readable source
store. Its batch contains the full source head and ordinary pending values.
The taker updates group state and discards each consumed row without
inserting it.

`EMIT` carries head terms and a validated output arity, not a readable
relation handle. The destination belongs to its taker: an insertion target,
answer sink or reducer. Static analysis, including the distinctness proof,
reads this output schema directly. This common representation avoids a
dummy source relation and keeps row-read operations' relation handles
mandatory.

It reads pending values directly from the current batch's representation.
It does not add every emitted value to a worker dictionary retained for
the whole producer, or send source rows through the candidate merge.
Only keys and potential extrema outlive a consumed batch. Count and sum
need no stored payload per source row, and an unused head column is not
interned solely because the materialized reference would store it.

Keys use typed semantic values: a domain value, computed integer or text
compares and hashes consistently with another representation of the same
value. Hashes are keyed by the context seed and equality is exact.
Symbols and strings remain different values even when they share bytes.
A pointer, worker-local marker or hash collision never establishes key
identity. These operations reuse the value layer's semantic primitives.

The builder owns retained text. No key or extreme borrows a pointer into
an executor's text stack or pending batch after that batch is consumed.
Key bytes are retained once per key entry. A text extreme has a reusable,
capacity-accounted buffer; replacement releases or reuses its previous
storage. An append-only arena of every successive extreme would silently
make one group's storage proportional to its rows and is not suitable.

### Bound parallel partials

There is one caller-owned global builder and a fixed total byte budget
`B` for partial maps and their retained payload. Active **lanes** each
own an executor scratch, one current ordered driver slice, its resumable
position and a portion of that budget. A pool job runs one part per lane;
a lane is the part's state, not the physical thread that claims it. Thus
the same scratch can resume on a different thread after a barrier without
being concurrently owned. No new pool affinity or stealing protocol is
required.

Parallel construction is used only when no configured `max_memory` limit
is active. With that limit set, the chosen reduction builds serially,
including under `-X parallel`; immutable summary consumers can still run
in ordinary shared rounds. Commutative group combination preserves values,
but its allocation history need not dominate the serial history: an early
large text extreme can disappear before other groups arrive on one
schedule and coexist with them on another. Serial construction makes limit
admission independent of worker count without a second resource-history
proof. All allocations remain accounted in both paths. The partial budget
is an internal constant, independent of source size and configured worker
count. The number of active lanes is also bounded by the budget needed
for each lane's minimum table; no empty lane allocation can exceed it.

One wave advances each lane through a bounded execution quantum or until
its partial map needs draining. The lane returns with its cursor and any
unconsumed batch intact. At the barrier, the caller combines the partials
into the global builder, clears them, and assigns the next ordered slice
to lanes that finished. Lanes with a paused slice resume that same slice.
The caller may combine parts in any completion-independent fixed order:
arithmetic and validity combine by group, and occurrence positions use
their minimum, not arrival order.

Admission of a row into a partial is all or nothing, across every family
updated from that source row. Required key/payload space is checked before
any count or accumulator changes; if the row does
not fit, its batch position remains unconsumed for the next wave. A single
row that cannot fit even an empty partial declines the parallel attempt
and uses serial reduction, whose ordinary charged builder can hold a
large key. This prevents an endlessly empty drain cycle. An already
accepted contribution is never replayed into the global builder.

The global builder grows under the memory account at barriers. A private
allocation refusal, producer error or interrupt abandons all private
partials and global state; the caller reruns the same reduction serially.
Workers report nothing and publish no values. Serial reduction updates
the global builder directly and is the reference for the parallel path.

For `G` groups and a fixed family width, accumulator storage is `O(G+B)`,
plus retained key/extreme byte capacities and executor scratch. Several
families use the sum of their group counts and state widths. There is no
`workers * G` persistent term and no `source rows` term. Executor text
scratch and pending batches remain separately charged and can depend on
the widest computed value; the bound does not pretend that expressions
or loaded facts occupy only group-sized memory.

The producer can reuse ordered driver slicing, including its once-only
prefix and views. A plan without a cuttable driver uses serial reduction;
parallelizing a nested continuation is not necessary for eligibility.
For a stored source, lanes visit disjoint identifier ranges. A wave is a
scheduling detail, not a new Datalog iteration or a public parallel round.
A successfully shared producer pass contributes one parallel round;
auxiliary construction over stored rows contributes none, like index
construction. Both have separate internal build and wave counters.

### Order, retained values and publication

Each group records its first logical source occurrence. A producer
occurrence is ordered by driver-task ordinal and emission ordinal within
that task; a stored source uses tuple identifier order. Column position
breaks ties between values in one row. Different worker counts can choose
different slice boundaries, but concatenated logical order is unchanged.

A key's first occurrence determines key enumeration. An extreme retains
its earliest winning occurrence, including equality ties. Only keys and
final valid extrema survive into the completed summary; integer aggregate
results remain raw numbers until a consumer emits them. Finalization sorts
the `G` group positions by first occurrence, not hash-table order. An
iterative merge sort over position records polls between bounded runs;
its temporary position array is charged. It does not move live builder
entries while their key table still refers to them.

The caller resolves retained values with the ordinary value resolver,
deduplicating equal values and sorting their retained first occurrences.
No value is published from a partial map or on arrival from a worker.
Descriptors follow retained key/extreme values, not source-column
occurrences. Reserve the global value capacity and final summary storage
before applying publication. Rewrite to promised identifiers and build
the final key lookup under those identifiers; only after successful
publication does the summary become a frozen fold source.

The final summary owns a compact array of keys and accumulator states
and one exact key table. Its entry identifiers are internal group
positions, not predicate, tuple or domain identifiers. A source with no
rows has no key entries. With an empty key mask, a nonempty source has
one entry; an empty source still has no key to emit into a support
projection, while an independent unkeyed fold obtains its empty result.

At a projection's scheduled turn, its ordinary relation receives the
summary keys in first-occurrence order and in that projection's recorded
column permutation. Limit admission and insertion use the ordinary
relation interface. The reducer does not eagerly emit aggregate results
for keys an outer rule has not visited; the outer plan controls result
order, interning of counts/sums, notes and errors.

Across reduction and materialized strategies, removed rows, intermediate
values, counters and storage can differ, as other strategy switches allow.
Decoded answers and reached aggregate semantics agree. Across worker
counts of the same strategy, retained values and output have the same
identifiers and order. Value publication includes symbol/string use bits
and byte-pool sharing; it adds no second persistent interner.

## Reusable summaries and profitability

A summary over a stored relation needs no producer or usage certificate
to retain the rows: it reads each row once and changes no reader's input.
Summaries with several key masks can share one source traversal, updating
each selected family. A mask belongs to the fold schema, so unrelated
outer variable names cannot create duplicate summaries. Construction never
runs inside a worker's `AGGREGATE` lookup.

The calling thread chooses summaries before preparing the consumer plans
and their index requirements. It uses deterministic integer estimates of
source visits, expected fold probes, group sizes and index acquisition:

```text
row folds:     needed index construction + expected repeated group visits
summary:       one source traversal + key updates + expected summary probes
```

Existing exact index key counts are usable without constructing an index.
An absent count is an estimate, clearly distinguished from a measurement;
the source's row count is a safe upper bound on summary keys. Include
one state update per requested family, retained bytes, finalization and
publication work. An index needed by another reader is not charged as a
saving, and a full-key fold already has at most one row per probe.

Concretely, the expected probes of each fold come from the work walk of
the consumer rule's draft, planned as it will be but with no index
built: the bindings reaching its `AGGREGATE` operation. A family's
group estimate is the exact key count of its index when one exists, and
otherwise the product of its key columns' distinct-value counts, capped
at the row count. That product is never below the true group count, so
it never overestimates the rows a lookup visits, and an uncertain
estimate leans toward row folds. Row folds cost each fold's probes and
visits plus two units per row for an index only the folds need: one not
yet built that no other operation of the component's drafts reads. A
summary costs, per row and per group, one unit and one per accumulator
the family holds, and two units per probe. A family's accumulators are
every signature the run's folds of it need, so a consumer that uses
few of them pays for them all. The families of one relation with
positive savings are chosen together only when their combined saving
exceeds the one traversal that builds them all. A family whose folds no
binding reaches is not chosen, since nothing shows it would be read. A
recursive consumer's draft reads its own relations as empty, so a fold
after such a read chooses no summary, though it reads one already built.
The drafts weighed are those the component will prepare, a recursive
rule's delta-first plans included, in storage released once the choice
is made.

Every value of a stored row already has its identifier, so a summary of
stored rows finds a group by its key's identifiers and reads the key
from the group's first row, which stays in place during the traversal,
rather than copying it. Its groups hold row counts and the requested
accumulators only; the completed summary has the same layout and lookup
as a producer's.

An unkeyed count over a stored complete relation reads its tuple count
directly; it does not build a one-entry table or visit every row. This
shortcut is disabled with the group-reduction reference so its work
counter difference remains testable. An unkeyed sum or extreme still
needs to examine values. A keyed fold over an almost-unique key may be
cheaper as an ordinary lookup than as another table of results.

Producer removal uses structural admission: a certified closed pipeline
is reduced. It does not require guessing the number of distinct results
of string functions to decide whether its source rows exist. The group
table replaces work already required by both the key projection and the
fold, and the all-distinct-key control measures its least compressible
case. Default enablement depends on those measurements.

Reusable summaries over retained rows use the cost comparison above,
since they add storage rather than replace the source pipeline. Costs and
estimates are saturating integers; conservative ties keep ordinary folds.
Neither choice depends on worker count, wall time, predicate names or a
speculative allocation's success. Constants and estimates are accepted
through benchmark controls, not through a claimed universal benefit of
preaggregation.

Once selected, the physical mode remains fixed for that run. A parallel
refusal retries the serial form of that same mode; it does not silently
choose a different set of stored intermediates. The explicit materialized
reference selects row folds before execution.

## Module boundaries

| Component | Responsibility |
| --- | --- |
| `lib/rule.c`, `lib/program.h` | Preserve the logical group schema and aggregate signatures; classify semantic read sites without changing language compilation. |
| `lib/plan.c`, `lib/plan.h` | Reuse the distinctness proof, validate reducer output schemas and resolve each aggregate to a row access or immutable summary. |
| `lib/uses.c` | Inventory every use of every predicate once per run, admit sources and key projections, collect aggregate signatures, and resolve each fold and row read against the run's scope. |
| `lib/eval.c`, `lib/magic.c` | Own the run scope's lifetime and source bindings, promote reached key projections into the complete part, dispatch physical schedule actions, and destroy them before fallback/discard. |
| `lib/fold.c`, `lib/fold.h` | Shared accumulator add/combine/outcome operations over borrowed semantic value views; no context mutation or diagnostic emission. |
| `lib/reduce.c`, `lib/reduce.h` | Private group builders, bounded lane jobs, retained payload ownership, ordering, publication and immutable summary lookup. |
| `lib/exec.c`, `lib/frozen.h` | Preserve normal expression evaluation and result-slot handling; expose completed summaries only through const read operations. |
| `lib/arith.c`, `lib/arith.h` | Exact two-word sum combination and final fit checks. |
| Value resolution and ordered merge | Resolve retained values with ordinary identities; merge outer output through the existing candidate path. Source rows never enter that path. |

`fold` separates semantic accumulator operations from the reducer's
owned payload. The latter copies borrowed text when retaining it; the
former never assumes that a value already has a domain identifier. The
ordinary row fold and the direct reducer share these semantics without
requiring the executor to know a group table's mutable layout.

## Ownership, failure paths and limits

An evaluation-owned reduction scope contains certificates, physical
actions, private builders and completed summary handles. A query's scope
outlives its complete-part evaluations, query group run, generated-rule
evaluation and final query plan. Nested schedule executions borrow it.
A full materialization owns a separate scope for optional summaries over
its rows. None is a persistent cache in a relation or prepared query.

The source-binding table publishes only completed read handles. Builders
have a different type and no lookup interface available to the executor.
Fold operations carry a tagged relation-or-summary source; ordinary atom
operations carry only relations. Summary readers are exposed through the
frozen interface, with no mutable accumulator, payload or table pointer.
This keeps an ordinary reader from accepting a summary by construction.

All data-sized allocations are charged at capacity: global group tables,
partial-map budgets actually allocated, retained bytes, ordered-position
scratch, value descriptors, final key arrays and states. Program-sized
certificates and plans belong to the charged evaluation arena. Release
temporary builders and mapping scratch at publication; keep completed
summaries through their last consumer and release them on every scope exit.
Reusing two copies of a complete group map through the whole query is not
part of the storage bound.

Every capacity sum, multiplication by entry width, byte offset and ordinal
increment is checked before allocation or mutation. Borrowed value views
carry explicit lengths and are validated against their owning batch or
payload span; empty text has an addressable representation without null
pointer arithmetic. Publication initializes every readable state field
and exposes no scratch bytes through explain, diagnostics or results.

`max_tuples` counts source rows only when stored, and counts materialized
key projections and output relations normally. Summary accumulators are
evaluation memory rather than fictitious tuples. The source identifier
ceiling is enforced separately. A strategy can therefore answer within
a storage limit that its materialized reference exceeds, without omitting
an input row or returning a partial answer.

If certificate or structural-plan allocation fails before selection, the
ordinary planning failure contract applies. Private parallel build refusal
discards private state and retries serial reduction. Serial memory or
capacity refusal reports the producer, aggregate source or query through
the ordinary diagnostic descriptions. No worker publishes a note, error,
domain value or key relation. A value reservation refused part way can
retain accounted capacity as the ordinary resolver permits; it cannot
publish a partially readable summary.

Errors while executing a producer are evaluated in its original logical
order by the serial retry. Successful builds retain the producer's first
mixed-type note for ordered reporting at its scheduled turn. Invalid or
overflowing aggregate outcomes remain private data until a consumer
reaches them. A consumer error follows the existing shared-round retry
and serial diagnostic rules.

The executor and reducer poll interrupts between quanta. Key traversal,
partial combination, retained-value collection and sorting also have
bounded progress points; a large group table does not create one
uninterruptible finalization step. No gathered answer is delivered until
all required reductions and consumer evaluation succeed. An outer query
that contains an aggregate still gathers, even when its fold is a summary
lookup; a simple query over an already computed result may stream normally.

On exit, destroy borrowing plans before their summary handles, release
all round and scope charges, and leave no materialization record for a
partial query run. A query may be destroyed after its context because its
run owns no live reduction state outside the operation. Repeated prepared
queries do not accumulate summary predicate names or cache entries.

## Explain, counters and the reference strategy

Explain describes the chosen physical source as well as the logical rule.
A producer reduction names its source, key columns and aggregate signatures;
key projection actions name their summary source; a fold prints `summary`
beside its existing function and source. Ordinary row folds keep their
established access description. For example, these are annotations to the
existing rule/component layout, not generated language predicates:

```text
reduce kind/3 key={1,2} count
project group/2 from summary kind/3 key={1,2}
aggregate N = count kind(P, D, E) key={1,2} summary
summarize edge/2 key={0} count
aggregate N = count edge(X, Y) cardinality
```

A summary of a stored relation is named beneath the heading of the
component whose folds chose it, one line per family; an unkeyed count
that reads the tuple count ends `cardinality`.

Worker counts, private pointers, hash slots, partial-map arrival order and
data values appear nowhere in those annotations. One selected strategy
has identical explain output at every thread count.

Producer `EMIT` rows count as derivations even when reduced rather than
inserted. A support projection action derives each emitted key once.
`tuples_inserted` and `tuples_held` count actual relation tuples. Source
visits used to build stored-row summaries count as scanned tuples; a
summary key-table probe is not a lookup of a source tuple and invents no
`tuples_looked_up` visit. Failed speculative attempts contribute no public
work; the serial retry supplies it. Internal counters cover reduced rows,
groups, partial drains, summary probes, source-store avoidance and peak
workspace without extending `relic_stats_t`.

`RELIC_F_NO_GROUP_REDUCTION`, exposed as `-X no-group-reduction`, disables
producer removal, reusable summaries and the unkeyed count shortcut. It
retains the existing compiler and ordinary row folds as an independent
reference. `-X group-rules` continues to force materialization of compiler
group rules, so those groups are not removed; combining it with
`no-group-reduction` gives the full aggregate reference. `-X no-index`
disables index access, not grouping itself; pair it with the new switch
to compare unindexed row folds. `-X parallel` exercises the selected
reduction's deferred path, including at one worker when a driver is
eligible. Public flag documentation, CLI help/manuals and strategy tests
describe the same reference behavior.

## Determinism and correctness

For source set `S`, key projection `K` and a fixed key `k`, the logical
group is `{s in S | K(s) = k}`. The row-store mode enumerates `S` once.
The producer certificate proves a bijection between accepted derivations
and `S`; driver slices and resumable lanes partition those derivations
without changing the bijection. Each group state therefore combines
exactly its logical row set.

Count, exact sum and valid extrema combine independently of arrival order.
Type-invalid outcomes also depend only on the set. Deferring observation
to the original fold preserves empty groups, bound-result comparisons and
the outer plan's choice of which failure or note is first. Keeping the
original strict edges proves that the set is complete when observed.

Minimum logical occurrence determines key and retained-value order,
independent of worker assignment and hash layout. Key projections thus
enumerate the same keys in the source's first-occurrence order; ordinary
outer plans determine output order and publish their computed results
through the existing serial-order merge. Removing named rows only in a
partial query scope, and never publishing a current-program record there,
keeps subsequent queries and full materialization correct.

## Validation

### Mechanism regressions

The producer-removal regression uses many distinct source identities and
few computed keys, with an ordinary key projection and fold as in the
file pipeline. It must fail against the row-materializing implementation
on a structural assertion: the source retains rows or has a fold index,
instead of an immutable summary. Assert reduced row count, exact answers,
group count and peak charged group workspace. A faster clock or fewer
pending descriptors alone does not satisfy this regression.

The stored-source regression repeats keyed folds over a complete relation
and checks that the chosen summary is built once per source/mask family,
not once per outer binding. A degree fixture includes sources, sinks,
self-loops and duplicate input facts, which the base relation removes.
The unkeyed count regression checks the absence of row visits separately.
Keep timing thresholds in benchmark artifacts, not correctness tests.

| Area | Cases and required checks |
| --- | --- |
| Distinctness | Retained source identity; dropped generator columns; wildcard generators versus `EXISTS`; duplicate-producing joins; repeated variables; constants; multiple producer rules; facts plus rules; equal values in distinct sum rows. |
| Uses and scope | An extra positive, negative or query reader of the source; several folds of one mask; another mask; key permutations; projections with filters or facts; full materialization and a query after it. Unsupported uses retain real rows. |
| Lifetime | Repeated prepared queries; query the removed source next; add a fact or rule between runs; Magic abandonment; destroy query after context. No summary, stale binding or current-program record survives incorrectly. |
| Empty groups | Missing independent-domain keys; all rows filtered out; nullary keys and sources; no support key for an empty source; zero for independent count/sum; no-value min/max. |
| Exact arithmetic | `INT64_MAX, 1, -1` in every order and partition; final positive and negative overflow; many negatives; a shared aggregated column; reduced source ceiling and partial-count combination. |
| Outcome timing | Unrequested overflowing group; outer filter before fold; bound aggregate result; mixed-type group with an overflowing integer subset; invalid sum alongside valid count; later producer error after an invalid group. |
| Values | Fresh computed keys and extrema; existing IDs versus equal pending values; symbol/string shared bytes; equality ties; replaced long extrema; non-key computed values not retained. No pointer escapes a batch. |
| Parallel bounds | One hot key; all keys distinct; skewed suffixes; many drains; a batch spanning drains; a key too large for a partial; fewer tasks than workers; fixed total partial budget at all worker counts. |
| Failure | Allocation injection in maps, payload, sorting, descriptors, value reservation and projection insertion; tuple limits; memory-limited construction selects serial even under forced deferral; long-lived early extrema test its admission order; interrupts and complete cleanup. |
| Reuse | Several masks in one source scan; duplicate signatures and renumbered Magic copies; existing versus avoided indexes; sparse single-key demand; exact all-key probes. |
| Observability | Identical chosen plan, unsorted output, value identifiers, successful counters and final stored capacities across threads; sorted decoded answers and reached errors against the materialized reference. |

Test accumulator laws independently with an exact integer model, including
invalidity, emptiness and final range checks. Model random partitions and
drains against sets of complete source rows, not sets of aggregate values.
Then exercise the real executor, resolver, source-binding types and failure
injection paths. Use curated mixed-type and error cases in addition to
the random differential driver, whose successful-answer comparisons alone
cannot detect a prematurely reported overflow.

Tests span ordinary and forced-deferred execution, 1/2/4/16 workers,
threads-disabled builds, no-Magic, group-rules, no-index, no-reorder and
no-exists combinations. Fix corpus expectations for strategy-dependent
counters rather than requiring removed work to count as performed. Keep
the whole-program derivation oracle on materialized row folds with
`no-group-reduction`; compare optimized answers to it independently.

Before implementation commits, perform the required bugs/design/security
and readability review before tests, then run the focused regressions and
full suite. Allocation and lifetime changes require ASan; exact arithmetic
requires UBSan; shared lanes, summaries and barriers require TSan. Run
LSan/MSan where supported. Tests verify exact equality after hash collisions
and checked capacity arithmetic, not merely that the common key layout runs.

### Performance acceptance

Follow the [benchmark plan's measurement rules](../docs/todo-benchmark-performance.md#measurement-and-acceptance-rules).
Use post-loading and pending-value baselines, with immutable binaries and
matching output sinks, to separate the work this optimization removes.
Measure producer expressions, group updates, partial drains, finalization,
value publication, key projection, fold lookups and output independently.

For `files-grouped`, require that the approximately 1.8 million-row
intermediate has no tuple array or secondary index on the reduced path,
while state follows the 1,200 groups and retained values. Record loaded
base storage separately from reduction workspace and allocator retention.
For `degree-count`, compare summary construction and probes with ordinary
index building and folds; a result that only changes when work is paid
does not establish an improvement.

Run scales 1/2/4/8 at 1 and 16 workers and 4/8/12 for scaling, with at least
ten interleaved targeted samples. Include tiny inputs, nearly unique keys,
one huge group, large computed keys, repeated extrema replacements,
selective demand and many different aggregate signatures as controls.
Retain wall/user time, RSS, charged peaks, exact answer checks and spread.
The combined end-to-end target is the benchmark plan's lead over the best
successful rival under comparable conditions. Serial drain or publication
cost is reported explicitly; removing source rows alone is not evidence
of a speedup. Repeatable unexplained regressions block default selection.

## Alternatives not taken

* **Count every join derivation.** Aggregate rows are distinct bindings;
  a uniqueness proof or a stored set is essential.
* **Deduplicate all source rows inside each group.** This can be correct
  but restores storage proportional to the removed intermediate. The
  ordinary materialized relation already provides that implementation.
* **Delete a named relation during full materialization.** Later queries
  are entitled to read it from the current fixed point. Query-scoped
  removal and an explicit source binding avoid a false currency claim.
* **Emit one result for every precomputed key.** Independent outer domains
  need empty-group results, outer tests decide whether a fold is reached,
  and the outer rule determines result order and error precedence.
* **Fail on a partial sum or while building an unrequested group.** Both
  change whether the program fails. Exact state and lookup-time outcomes
  retain the language's aggregate semantics.
* **Intern on workers or at each drain.** Value identifiers then depend
  on scheduling, or eliminated intermediate values accumulate permanently.
  Only retained values are resolved at ordered publication.
* **Keep a full group table per worker.** Its memory is proportional to
  workers times groups. Bounded partials and one global builder retain a
  group-sized state bound even with many distinct keys.
* **Add counters to every secondary-index key.** This charges insertion
  even when no aggregate reads them and solves only count for that mask.
  Evaluation-scoped summaries serve chosen complete-input folds.
* **Cache arbitrary folds forever in the context.** It adds invalidation,
  retention and prepared-query lifetime obligations. A run-scoped immutable
  summary is reusable wherever its complete source is needed in that run.
