# Parallel evaluation: gaps and ideas

Section 15 of `specs/design-parallel.md` builds parallel evaluation in six
steps, each with a gate. Steps 1 to 4 are in and every identity gate holds:
output, counters and storage bytes are the same at every thread count. The
speed-up gate of step 4 is missed, and steps 5 and 6 are not started. This
page records what is missing and what to try, in the order worth trying it.

## Round 3 scales about eight times on sixteen threads

[Medium, component: `lib/round.c`, `lib/task.c`] Round 3's tasks, 3 to 4 s
of work on one thread, take 0.35 to 0.42 s on sixteen. The machine was
loaded throughout, so how much of the shortfall is contention is unknown.
On a quiet machine, record each task's time and each worker's: a long tail
of tasks points at the cut (`RELIC_CHUNKS_PER_WORKER`, `RELIC_CHUNK_WORK`),
while even tasks that are each slower than serial point at memory
bandwidth or at the workers' screens. A worker's screen is reserved from
what round 2 inserted, so round 3 no longer grows it from empty; that
gain is not yet measured at sixteen threads.

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

[Medium, component: `lib/merge.c`] `dist/3` over the `tc-cyclic` data,
`dist(X, Z, D1) :- dist(X, Y, D), par(Y, Z), D < 3, D1 = D + 1`, scales 2.5
times on sixteen threads, 4.08 s to 1.64 s, where `tc-cyclic` scales 6.2
times, 2.78 s to 0.45 s, the fastest of five runs each on a loaded machine.
Its two large rounds each insert about a million new tuples of three
columns, 1.96 million in all against `tc-cyclic`'s one million, into 51 MB
against 18 MB. The computing is not what holds it back: the same program
with the successor as facts, joined in the same order, takes 1.43 s on
sixteen threads. Time the merge's steps on a quiet machine; those worker 0
runs alone, counting the new tuples, reserving and storing the columns,
grow with the tuples a round inserts.

## A round of fewer tasks than workers reserves for every worker

[Medium, component: `lib/round.c`] A round just above
`RELIC_PARALLEL_MIN_WORK` is cut into pieces of `RELIC_CHUNK_WORK`, one
or two below 12 288, fewer than a pool of sixteen has workers. Its
candidate buffers are still reserved as twice an even share for every
worker of the pool (`worker_share()`), so the round charges twice what it
expects, and the one or two workers that run its tasks hold room for an
eighth of it each and grow from there through the cascade the reservation
exists to avoid. A worker cannot know beforehand which tasks it will
claim, so the remedy is not a smaller share for some: either cut every
shared round into at least as many pieces as the pool has workers, which
the threshold and the piece size would then have to agree on, or reserve
each worker's share as it claims its first task. Measure the chains of
six thousand edges, whose shared rounds are mostly of this kind, before
and after.

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

Ideas, each a change to section 5.4 of the design:

* Estimate an item that has not run from its plan rather than its view: the
  view's size times the fan-out of each lookup, which an index knows as
  tuples per key.
* Estimate it from the planner's cardinality estimates, which already
  order the literals.

The first uses numbers the store keeps exactly and is the one to try first.
