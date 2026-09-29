# Parallel evaluation: gaps and ideas

Section 15 of `specs/design-parallel.md` builds parallel evaluation in six
steps, each with a gate. Steps 1 to 4 are in and every identity gate holds:
output, counters and storage bytes are the same at every thread count. The
speed-up gates of steps 3 and 4 are missed, and steps 5 and 6 are not
started. This page records where the time goes, what is missing, and what
to try, in the order worth trying it.

## Where `tc-cyclic`'s sixteen-thread time goes

`tc-cyclic` is the transitive closure of a dense cyclic graph:

    tc(X, Y) :- par(X, Y).
    tc(X, Z) :- par(X, Y), tc(Y, Z).

It runs in four rounds. Traced at sixteen threads under a load average of
about twenty, times in seconds:

| round | what it does | new tuples | at `-j 16` | time |
|---|---|---|---|---|
| 1 | base rule, copies `par` | 48 727 | one thread | 0.005 |
| 2 | first recursive round, joins those with `par` | 863 571 | one thread | about 0.5 |
| 3 | joins round 2's tuples with `par` | 87 702 | sixteen threads | 0.35 to 0.42, 3 to 4 alone |
| 4 | finds nothing new | 0 | sixteen threads | 0.02 |

The merge is no longer the bottleneck: round 3 merges its new tuples in 0.02
to 0.03 s. `tc-cyclic` is 5.3 times faster at sixteen threads than at one,
2.66 s against 0.50 s, against the six step 3 asks for and the ten step 4
asks for.

## Round 2 is never shared

[Medium, component: `lib/round.c`] A round is shared among the pool's
threads only when its estimated work, in tuples read and derived, reaches
`RELIC_PARALLEL_MIN_WORK`, 65 536. An item estimates from the work it did
the last time it ran, scaled by how its view grew; an item that has not run
has no history and is estimated at its first scan's view. Round 2 is the
recursive rule's first run, so it is estimated at 48 727, the tuples round 1
added, and runs on the calling thread. Every one of those tuples fans out
through the dense graph, and the round derives some twenty times that. At
sixteen threads it is about half of the run.

Forced to be shared, under `-X parallel`, round 2 takes 0.15 to 0.27 s, merge
included, and the whole run 0.36 s at best: 7.4 times faster than one
thread.

Ideas, each a change to section 5.4 of the design:

* Estimate an item that has not run from its plan rather than its view: the
  view's size times the fan-out of each lookup, which an index knows as
  tuples per key.
* Estimate it from the planner's cardinality estimates, which already
  order the literals.
* Share the first round of every recursive component whose view reaches a
  lower bound, since a first round is where a closure grows fastest.

The first uses numbers the store keeps exactly and is the one to try first.

## The threshold predates the parallel merge

[Low, component: `lib/round.c`] `RELIC_PARALLEL_MIN_WORK` was set in step 3,
when a round that inserts most of what it derives gained nothing from the
pool because only its screening was shared: a chain of 6 000 edges, whose
rounds estimate up to 18 000, ran ten per cent slower at sixteen threads
with the threshold at 8 000 or 16 000. Now the merge is shared as well, so a
lower threshold may pay. Measure the chains and `tc-cyclic` at 8 000, 16 000
and 32 000 on a quiet machine.

## Round 3 scales about eight times on sixteen threads

[Medium, component: `lib/round.c`, `lib/task.c`] Round 3's tasks, 3 to 4 s
of work on one thread, take 0.35 to 0.42 s on sixteen. The machine was
loaded throughout, so how much of the shortfall is contention is unknown.
On a quiet machine, record each task's time and each worker's: a long tail
of tasks points at the cut (`RELIC_CHUNKS_PER_WORKER`, `RELIC_CHUNK_WORK`),
while even tasks that are each slower than serial point at memory
bandwidth or at the workers' screens. Reserving a worker's screen from the
round's estimate rather than growing it from empty, which the item on a
deferred round's cost on one thread proposes, would help here too.

## Against Souffle at sixteen threads

Measured under a load average of 24 to 34, both engines writing their
answers, which agree; median of seven runs in seconds, Souffle's compile time
of about 9 s per program not counted:

| workload | relic `-j 1` | relic `-j 16` | Souffle compiled `-j1` | Souffle compiled `-j16` | Souffle interpreted `-j16` |
|---|---|---|---|---|---|
| `tc-cyclic` | 4.27 | 0.97 | 2.85 | 0.76 | 1.29 |
| `cspa` | 0.34 | 0.13 | 0.78 | 0.28 | 0.45 |
| `chain-closure` | 0.83 | 0.81 | 1.74 | 2.00 | 2.13 |

Compiled Souffle leads on `tc-cyclic` by starting from a faster single
thread, and scales about as relic does, 3.8 times against 4.4 here; sharing
round 2 would close most of the gap. relic leads on `cspa` and
`chain-closure`, where Souffle gains little from threads or loses.

`bench/compare/run.sh` runs every engine on one thread, and the Souffle on
this machine's `PATH` has no OpenMP, so its `-j` does nothing; these figures
use Souffle 2.5 built with OpenMP from Homebrew's `libomp`. A compiled
Souffle program has parallel loops only when it is translated with `-j`, as
in `souffle -j16 -g`. A threaded column in the harness is an item of its
own in `TODO.md`.

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
`dist(X, Z, D1) :- dist(X, Y, D), par(Y, Z), D < 3, D1 = D + 1`, scales 2.7
times on sixteen threads, 3.52 s to 1.30 s, where `tc-cyclic` scales 5.4
times, 2.60 s to 0.48 s, the fastest of five runs each on a loaded machine.
Its two large rounds each insert about a million new tuples of three
columns, 1.96 million in all against `tc-cyclic`'s one million, into 51 MB
against 18 MB. The computing is not what holds it back: the same program
with the successor as facts, joined in the same order, takes 1.43 s on
sixteen threads. Time the merge's steps on a quiet machine; those worker 0
runs alone, counting the new tuples, reserving and storing the columns,
grow with the tuples a round inserts.

## Not started

* Step 5, loading in parallel (section 10 of the design): its gate is
  `facts-join` at scale 10 importing at least four times faster at sixteen
  threads, with the identifier a symbol receives the same on every run.
* Step 6, components side by side (section 13), which the design defers
  until a workload asks for it.
