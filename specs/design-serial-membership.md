# Serial positive membership

The evaluator avoids repeated insertion checks with a bounded bitmap owned
by one serial plan. It remembers only confirmed presence in that plan's
target relation; the relation store remains the authority for every miss.

Related: [evaluation](design-eval.md#the-executor-does-not-insert),
[storage](design-storage.md), [memory](design-memory.md#round-scoped-buffers),
[values](design-values.md), [dense parallel collection](design-dense-rounds.md).

## Model

`execute_plan()` in `lib/eval.c` has one fixed insertion or delivery target.
An insertion target is append-only for the whole call. The plan owns an
optional zeroed bitmap from entry through cleanup, including every executor
batch and quantum pause. No bitmap survives into another plan or relation.

A set bit means that ordinary insertion of that exact typed tuple completed.
Completion may have added the tuple or found it already present. An unset
bit makes no claim: the ordinary batch inserter checks membership, enforces
limits, reserves storage and publishes new tuples in the usual order.

The cache needs no initial relation scan and no maintenance from other
writers. Another insertion can add positive facts but cannot invalidate a
fact already learned. A tuple omitted from the cache is checked ordinarily.
The executor, its views and the relation's canonical and secondary indexes
are unchanged.

## Invariants

* Every bit belongs to the one insertion target of its owning plan.
* Only a completed ordinary insertion teaches a bit. A failed or unattempted
  row teaches nothing.
* Cached positives stay true because that target only appends while the
  owner lives. No stored column pointer is borrowed by the cache.
* Misses retain derivation order. Completion and refusal map back to the
  original batch before derivation counters advance.
* Pending values resolve and executor notes/errors are handled before
  screening. Streaming delivery allocates no bitmap.

## Typed coordinates and admission

The owner snapshots the integer table's span `I` and symbol pool's span `S`.
Integers occupy `[0,I)`, symbols `[I,I+S)`, and strings `[I+S,I+2S)`. A symbol
and string with identical bytes occupy distinct coordinates. Indices are
checked against their own snapshot span before use; zero and out-of-span
identifiers have no coordinate. Later interning appends values without
renumbering earlier ones, so a new value simply remains an ordinary miss.

For a binary relation, coordinates `a,b` select bit `a*D+b`, where
`D=I+2S`. Saturating arithmetic checks the square before allocation. The
bitmap requires `ceil(D*D/8)` bytes, including typed holes in the pool.

The admission rule requires at least 4,096 tuples already held by the
target, no more than 4 MiB of bitmap storage, and no more bitmap bytes than
held tuples. These bounds limit setup on small work and prevent a sparse
domain from reserving a large auxiliary representation. The constants live
with the owner in `lib/eval.c`; there is no public tuning switch.

## Taking a batch

The taker compacts unknown tuples into a stack buffer and records their
original positions. Cached positives need no relation lookup. All misses,
including repeated misses within this batch, go through the existing batch
insertion in their original order. Learning happens after insertion returns,
using only its completed prefix.

On success every original row counts as a completed derivation. On refusal
at compacted miss `k`, its saved position is the original completed-prefix
length. For `hit, miss, hit, refused miss, hit`, the first three original
rows count; the last hit does not. Added flags map through those same saved
positions, preserving tuple identifiers and fixed-point detection.

## Failure paths and limits

A finite memory limit declines the optional bitmap. This preserves the
ordinary limit boundary without eviction, reserve prediction or replay.
With no memory limit, allocation is charged through the round account and
released when the plan ends. Optional allocation failure releases its charge
and leaves ordinary insertion in use without reporting a diagnostic.

Every execution exit releases the owner, including insertion or expression
failure, an interrupt and normal completion. The allocation is capped and
batch screening has the existing fixed batch bound; executor quantum checks
continue to provide interruption points. No worker shares or mutates this
serial bitmap.

## Determinism

The cache suppresses only checks whose answer is already established. New
tuples still reach ordinary insertion in derivation order. Value assignment,
tuple identifiers, output order, work counters and final accounted storage
are unchanged. Configured memory-limit runs use ordinary insertion; tuple
limits meet the same first absent row and preserve the same completed prefix.
The bitmap's temporary requested bytes and process RSS remain real costs.

## Alternatives not taken

Pre-seeding the bitmap scans existing tuples. Treating an unset bit as absence
also requires complete coverage of the stored tuples and a closed domain.
Learning positives alone avoids both duties.
A persistent cache requires an invalidation or maintenance contract across
relation lifetimes; ownership by one plan makes that contract unnecessary.
The atomic ranks and ordered publication of parallel collection serve worker
occurrence ordering, which serial insertion already supplies.
