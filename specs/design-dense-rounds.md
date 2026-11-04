# Dense collection for deferred rounds

This document defines a temporary candidate representation for a dense binary
relation. It sits between the ordinary frozen executor and ordered append
preparation. A fixed array records occurrences instead of storing worker
copies of tuples. The relation store, serial evaluator and general candidate
collector retain their representations.

Related: [parallel evaluation](architecture.md#parallel-evaluation),
[tasks and merge](design-parallel.md), [values](design-values.md),
[storage](design-storage.md), [memory](design-memory.md),
[method references](../docs/research.md#earliest-occurrence-ranks--used-local-adaptation).

## Model

A dense round has one target of arity two. Every participating EMIT term is
a constant or domain-slot reference; number and text slots do not qualify.
This is a property of the output schema, not an allowlist of body operations.
Body expressions retain ordinary scratch. No candidate value needs interning
after the round starts.

Assigned heads do not imply expression-free bodies. Dense execution uses
`relic_round_first_call()`, `begin_worker_texts()` and `end_worker_texts()`,
including the serial text-capacity projection passed to common memory
admission. The text charge applies even without candidate-buffer reservation.

Let `I` be the frozen integer-table length and `S` the symbol-pool length.
Map assigned identifiers to three concatenated ranges: integers `[0, I)`,
symbols `[I, I + S)` and strings `[I + S, I + 2S)`. Symbol and string positions
distinguish typed values sharing pool bytes; unused type-use positions are
harmless holes. No bytes or mapping table are copied. Encoding and decoding
use the value representation's type and index boundaries.

Write `D = I + 2S` and `C = D * D`. Pair `(a, b)` names cell
`rank(a) * D + rank(b)`. Checked arithmetic proves `0 < C <= UINT32_MAX`
before allocation, so a cell position fits 32 bits. A nonrepresentable
domain declines dense collection without changing evaluation.

One round owner holds the fixed atomic rank array, scalar worker sequences,
temporary ordering arrays and their charges. The round that opens the
collector is the only one to close it, after the merge returns; ordering and
staging advance it through states that each remain readable, so no reader
meets a closed collector. The rank array never moves
while workers can reach it. The shared path requires eight-byte lock-free
64-bit atomics. The build enables the update primitive only where that
support is established without an additional runtime library; a run-time
branch alone must not leave a `libatomic` link requirement on other targets.
Unsupported platforms retain the ordinary collector.

## Invariants

* Each original task runs once over its original frozen views. Plan order,
  task cutting, executor counters and diagnostics retain their meaning.
* One worker runs a task; each worker claims tasks in increasing order, as
  the pool already requires for ordinary candidate screening.
* A cell's rank only decreases during gathering. Its entire state is one
  atomic word, with no separately published pointer, payload or length.
* A worker successfully lowers a given cell at most once in a round.
* Sorting surviving cells by rank reproduces serial first-occurrence order.
  Numeric ranks themselves need not be identical across schedules.
* All private work and possible failure precede ordinary append application.
  The relation store remains the sole publication mechanism.

## Eligibility and selection

The round constructor checks the single target, binary schema, assigned-value
terms, domain bounds and task count. Seeds name assigned constants. Task
numbers start at zero and remain below `UINT32_MAX`. Immutable domain spans
come through a frozen accessor returning only `I` and `S`; workers receive
no mutable value-table representation and cannot reach an interner.

Selection compares byte estimates. Let `R` be the target's existing tuple
count and `P` the active worker count. Model the ordinary collector holding
`R` binary candidates per worker, using actual entry/column/stripe sizes,
tagged screen sizing, merge references and stripe-list positions. A single
tagged table for `P * R` estimates the combined merge tables; their actual
stripe distribution is unknown. Include scalar bookkeeping in both layouts.
Overflow declines the dense choice rather than wrapping an estimate.

The dense estimate is `8C + 8 min(C - R, P * R)`: the fixed rank array and
two position arrays at an estimated distinct count, plus histogram room.
Choose it only when smaller than the ordinary estimate. With `R == 0`, the
ordinary estimate is zero. This heuristic does not predict that every worker
derives `R` new tuples. Actual arrays are charged at their actual sizes,
including a distinct count above the estimate. A poor estimate can cost time
or decline private work; it cannot change results or publication order.

The ordinary decision whether to share a round precedes this choice. Default
one-thread evaluation retains its fast path. Small bound relations and large
sparse domains make the dense estimate unattractive without workload names,
new query flags or a second execution-cost model.

## Gathering earliest occurrences

Initialize cells to `UINT64_MAX`, meaning unseen. Set cells for tuples already
in the frozen target to zero. Initialization and the old-target walk complete
before the gather job starts, polling at bounded intervals. These storage
preparation walks are not executor tuple visits.

Each worker starts a scalar sequence at zero. For every emitted row, advance
the ordinary derivation counter, encode the pair and load its cell. A cell
that is zero, or whose task number is no greater than this task's, needs no
update. Otherwise propose:

```text
occurrence = (task_number << 32) | (worker_sequence + 1)
```

A strong compare-and-swap using relaxed memory ordering lowers the cell.
On success advance the sequence. On failure recheck the returned word, then retry the same
proposal or stop because an earlier task won. Only one worker runs this task,
so a cell already naming it records that task's first occurrence.

After a worker successfully lowers a cell, its later tasks have greater task
numbers and that cell can only acquire an earlier owner. The worker cannot
lower it again. Its successful updates are therefore bounded by `C`, however
many duplicates it derives. The low half stays positive, excluding the zero
sentinel; the task bound excludes the unseen sentinel. Check packing bounds
before forming the word, including the largest representable sequence.

Only the rank word is shared during an update. Relaxed ordering suffices
because it publishes no other data; the existing pool barrier publishes the
completed array. This is lock-free on supported atomics, not a portable
wait-free guarantee. Contention and cache traffic remain performance costs.

## Ordering and staging

After the barrier, count cells that are neither zero nor unseen. Divide cell
space into ordered parts, prefix their counts, and compact 32-bit cell
positions into disjoint ranges of an array. Check tuple and identifier limits
against the exact total.

Order positions by their immutable 64-bit ranks with a stable radix sort:
eight byte passes, two position arrays, private per-part histograms and
prefix-assigned output ranges. Unsigned shifts extract digits independently
of machine byte order. Each pass preserves input order within a digit. Pool
barriers separate counting, prefixing and scattering; long walks poll inside
passes. All allocation precedes each pass, and each part owns its writes.

There are at most `P` ordered parts, each a contiguous input range, regardless
of which worker executes it. One `P * 256` array is reused in place from
histogram counts to prefix cursors; any additional scratch belongs in the
layout and charge. Thus the ordering ledger does not hide a task-sized
collection of histograms.

While the ranks still exist, the calling thread walks the sorted positions
and counts each task's winning rows, filling its insertion history and prefix
position. It is the sole writer of those counts. Tasks without winners stay
zero, and the counts sum to `U`. This adds one serial `U`-element walk; it
avoids shared counter updates and `P * T` private count arrays. Its cost is
part of ordering measurements.

Release the extra position array and rank array after sorting. Decode the
ordered positions to their two assigned identifiers, compute each ordinary
wide hash once, and fill the normal append through `relic_relation_stage()`.
Count canonical stripes from staged entries. Release the last position array
before key classification and reservation, leaving the collector with no
positions. A round with no new tuple is the same state reached directly:
ordering releases the rank and ordering arrays and allocates no position
array, and every reader asks only for positions below the count.

The existing classification, exact serial-memory admission, reservation and
application consume this append. Dense collection creates no public relation,
canonical index or persistent cache. The general collector reaches the same
append boundary after pending resolution and duplicate finding.

## Why the order is serial

For a new tuple, let `t` be the first serial task deriving it. Later tasks can
contribute larger temporary ranks; `t` lowers them. No earlier task owns that
tuple by the definition of `t`. Within `t`, its first occurrence contributes.

For two tuples whose first task is `t`, neither could have been contributed
by that worker's earlier tasks. Their successful updates occur in derivation
order, so their sequences have that order. Overwritten contributions leave
gaps, which do not change it. Different first tasks sort by task number.
Together these properties recover serial insertion order without worker
tuple buffers, worker bitmaps or executor replay.

## Failure paths and limits

Check assigned identifiers against the frozen spans before cell access. A
violated private invariant abandons the round; it never indexes unchecked
memory. Test malformed input directly so fallback cannot hide a collector bug.

Allocation, admission, rank-bound, interrupt or executor failure discards the
rank array, ordering arrays, unfinished append and private counters. The
[serial retry](design-parallel.md#failure-paths-and-limits) reports the actual
evaluation outcome. Interrupts during callbacks after publication follow the
[delivery contract](design-eval.md#interruption). No task publishes separately.

Charge the rank array before initialization and ordering room before allocation;
release both at the stated phase boundaries. Replacement tables and array
growth retain their existing lifetime rules. Allocator retention and realloc
transients are distinct from requested storage; measure process RSS separately.

## Storage and complexity

Gathering uses `8C + O(P + T)` bytes for `T` tasks. Ordering uses
`8C + 8U + O(P * 256 + T)` for `U` new tuples. Staging keeps `4U` positions
alongside the ordinary append. Classification and reservation retain no dense
array. There is no `P * U` tuple-storage term.

With 4,000 assigned integers and an empty symbol pool, the rank array is
128 MB in decimal units. The ordinary append, indexes, final relation,
expression scratch and allocation overlap remain real costs.
Measurements distinguish requested storage from process RSS.

## Alternatives not taken

Souffle's [concurrent B-tree](https://souffle-lang.github.io/pdf/ppopp19.pdf)
shares duplicate suppression with optimistic node locking. Its
[Brie](https://souffle-lang.github.io/pdf/pmam19.pdf) uses prefix sharing and
atomic membership bits for dense data. They demonstrate that sharing need
not require a coarse lock; neither provides relic's first-occurrence order.

A full concurrent tree adds node publication and growth machinery. A
temporary rectangle instead has a known size and stable addresses for one
round, giving up broad applicability for a small ownership boundary. A
presence bit or first-thread-wins insertion loses occurrence order. Input
repartitioning requires routing and order metadata; replay requires extra
traversals. The rank word keeps the existing executor, tasks and counters.
