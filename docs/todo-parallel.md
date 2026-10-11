# Parallel evaluation: gaps and ideas

`specs/design-parallel.md` designs parallel evaluation. Shared rounds, the
pool and the parallel merge are built, and output, counters and storage
bytes are the same at every thread count. The speed-up targets below are
missed, and loading in parallel and components side by side are not built.
This page records what is missing and what to try, in the order worth
trying it. Times are from a macOS Xeon, loaded unless another platform is
named. M3 observations refer to the
[comparison matrix](../bench/compare/recorded/m3-max/results.tsv) and
[diagnostic probes](../bench/compare/recorded/m3-max/optimization-probes/results.json).
Cross-subsystem sequencing and acceptance criteria are in the
[benchmark performance plan](todo-benchmark-performance.md).

## A lookup cannot drive parallel tasks

[Medium, component: `lib/round.c`, `lib/plan.c`] `item_is_cut()` accepts
only a first-operation scan; `item_range()` estimates every other plan at
one tuple. The M3 scale-8 `facts-join` starts with a constant-key lookup,
returns 571,953 answers and records zero parallel rounds at sixteen
threads. Give the driving cursor an ordered, partitionable range for
both scans and indexed matches, and estimate work from its cardinality
and downstream fan-out. Preserve indexed selection rather than forcing
a full scan to obtain parallelism.

## Pending values are expanded back into occurrences

[Medium, component: `lib/merge.c`, `lib/task.c`] A worker already gives
each distinct pending value one marker, but `pending_columns()` and
`gather_pending()` size and populate the round's value map once per
candidate column. M3 scale-8 `files-grouped` derives 1,799,657 `kind`
tuples with two pending strings each, although there are only 206 new
directory and extension strings. Retain a local-to-round marker map per
worker: walk candidates in serial task order, create a round descriptor
only at that local marker's first occurrence, and reuse it thereafter.
This bounds descriptors by worker-distinct values, at most 3,296 for
this sixteen-thread rule, while retaining first-occurrence numbering.
The column rewrite still visits every pending column. Test identifiers,
diagnostics, limit refusals and allocation failure before changing this
ownership path.

## Candidate replication makes large rounds expensive

[Medium, component: `lib/task.c`, `lib/merge.c`] M3 scale-8 `tc-cyclic`
at sixteen threads takes 22.512 s and peaks at 23.46 GiB, against
Souffle's 13.616 s and 1.49 GiB; relic's serial peak is 1.46 GiB.
Per-worker screening bounds candidates by workers times distinct new
tuples, and merge staging and scratch add further storage. Measure live
candidates, cross-worker repeats and each merge allocation separately
from allocator retention. Evaluate bounded batches routed to one owner
per tuple-hash partition, retaining the earliest logical derivation
position for deterministic commit. This requires a design change to
the deferred round, including worker-independent hashes for pending
values, exact collision checks and the serial fallback on refusal.
The target is scratch proportional to distinct new tuples plus bounded
producer buffers, independent of worker count. The scale-8 all-pairs
tree, which peaks at 109.1 GiB at sixteen threads, is a capacity gate
alongside the closure timings.

## The targets

* `tc-cyclic` at least ten times faster at `-j 16` than at `-j 1`; it is
  6.2 times, 2.78 s to 0.45 s, the fastest of five runs.
* `dist/3` (`dist(X, Z, D1) :- dist(X, Y, D), par(Y, Z), D < 3, D1 = D +
  1` over the `tc-cyclic` data) and `object/2` (the item on string
  operations in `TODO.md`) scaling as `tc-cyclic` does at sixteen threads;
  they scale 2.5 and 1.8 times.
* Loading in parallel: `facts-join` at scale 10 imports at least four times
  faster at `-j 16`, and the identifier a symbol receives is the same on
  every run and at every thread count.

## Round 3 scales about eight times on sixteen threads

[Medium, component: `lib/round.c`, `lib/task.c`] Round 3's tasks, 3 to 4 s
of work on one thread, take 0.35 to 0.42 s on sixteen. The machine was
loaded throughout, so how much of the shortfall is contention is unknown.
On a quiet machine, record each task's time and each worker's: a long tail
of tasks points at the cut (`RELIC_CHUNKS_PER_WORKER`, `RELIC_CHUNK_WORK`),
while even tasks that are each slower than serial point at memory
bandwidth or at the workers' screens. A worker's screen is reserved from
what round 2 inserted, so round 3 does not grow it from empty; that
gain is not measured at sixteen threads.

## A round cut by a scan over a few tuples

[Medium, component: `lib/plan.c`, `lib/round.c`] `dist/3` written with its
successor as facts, `dist(X, Z, D1) :- dist(X, Y, D), par(Y, Z), D < 3,
succ(D, D1)`, over the `tc-cyclic` data: the planner starts the recursive
rule with `scan succ(D, D1)`, two tuples, and a plan is cut into at most as
many pieces as its first view holds tuples, so the round's 45 million
derivations fall in two tasks. On sixteen threads it takes 5.7 to 5.9 s
against 3.6 to 3.7 s on one, the fastest of five runs on a loaded machine;
with `-X no-reorder`, which starts it at the delta, 1.43 s against 4.95 s.
Cut such a plan by its second operation, or let the planner weigh how a
first operation can be cut when it picks one.

## Rounds that insert what they derive

[Medium, component: `lib/merge.c`, `lib/round.c`] `dist/3` over the
`tc-cyclic` data scales 2.5 times on sixteen threads, 4.08 s to 1.64 s,
where `tc-cyclic` scales 6.2 times, 2.78 s to 0.45 s, the fastest of five
runs each on a loaded machine. Its two large rounds each insert about a
million new tuples of three columns, 1.96 million in all against
`tc-cyclic`'s one million, into 51 MB against 18 MB. The computing is not
what holds it back: the same program with the successor as facts, joined
in the same order, takes 1.43 s on sixteen threads. Time the merge's steps
on a quiet machine; those worker 0 runs alone, counting the new tuples,
reserving and storing the columns, grow with the tuples a round inserts.
The threshold that decides whether a round is shared weighs only its work,
tuples read and derived, not the new tuples its merge will insert, so a
round whose merge dominates is shared on the same terms as one whose tasks
do; whether to weigh expected new tuples follows from the timing.

## A round of fewer tasks than workers reserves for every worker

[Medium, component: `lib/round.c`] A round just above
`RELIC_PARALLEL_MIN_WORK` is cut into pieces of `RELIC_CHUNK_WORK`, a
quarter of the threshold, so a round of one item below 32 000 has fewer
pieces than a pool of sixteen has workers. Its candidate buffers are still
reserved as twice an even share for every worker of the pool
(`worker_share()`), so the round charges twice what it expects, and the
few workers that run its tasks hold room for an eighth of it each and grow
from there through the cascade the reservation exists to avoid. A worker
cannot know beforehand which tasks it will claim, so the remedy is not a
smaller share for some: either cut every shared round into at least as
many pieces as the pool has workers, or reserve each worker's share as it
claims its first task. Measure the chains of six thousand edges, whose
shared rounds are mostly of this kind, before and after, and on an idle
machine compare the threshold at 4 000 with 8 000, which a loaded one
could not separate.

## A first round is estimated at its view

[Low, component: `lib/round.c`] A round is shared among the pool's threads
only when its estimated work, in tuples read and derived, reaches
`RELIC_PARALLEL_MIN_WORK`, 8 000. An item estimates from the work it did the
last time it ran, scaled by how its view grew; an item that has not run has
no history and is estimated at its first scan's view. A recursive rule's
first round whose view is below the threshold therefore runs on the calling
thread however far each tuple of it fans out. `tc-cyclic`'s, estimated at
48 727 and deriving some twenty times that, crosses the threshold and is
shared; a closure from a few seeds would not be.

Ideas, each a change to
[which rounds are shared](../specs/design-parallel.md#which-rounds-are-shared):

* Estimate an item that has not run from its plan rather than its view: the
  view's size times the fan-out of each lookup, which an index knows as
  tuples per key.
* Estimate it from the planner's cardinality estimates, which already
  order the literals.

The first uses numbers the store keeps exactly and is the one to try first.

## Loading in parallel is not built

[Medium, component: `lib/symbol.c`, `lib/value.c`, `lib/facts.c`,
`lib/import.c`] A load and an import run on the calling thread: the value
table and the symbol pool are not striped, so interning, most of an
import's time, is serial. The design is
[loading in parallel](../specs/design-parallel.md#loading-in-parallel); its
target is in the list above. It changes the identifier's layout, a stripe
field in its index, so it interacts with the item on type tests in
`TODO.md`.

## Components side by side are not built

[Low, component: `lib/eval.c`, `lib/round.c`] Two components with no path
between them run one after the other, so a program of many small
independent components, such as a Magic rewrite generates, gains nothing
from the pool. The design is
[components side by side](../specs/design-parallel.md#components-side-by-side).
Build it when a workload asks for it.
