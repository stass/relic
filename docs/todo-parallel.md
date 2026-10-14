# Parallel evaluation: gaps and ideas

`specs/design-parallel.md` designs parallel evaluation. Shared rounds, the
pool and the parallel merge are built, and output, counters and storage
bytes are the same at every thread count. The speed-up targets below are
missed, and components side by side are not built.
This page records what is missing and what to try, in the order worth
trying it. Times are from a macOS Xeon, loaded unless another platform is
named. M3 observations refer to the
[comparison matrix](../bench/compare/recorded/m3-max/results.tsv) and
[diagnostic probes](../bench/compare/recorded/m3-max/optimization-probes/results.json).
Cross-subsystem sequencing and acceptance criteria are in the
[benchmark performance plan](todo-benchmark-performance.md). Targets and
candidate remedies below motivate measurements; they do not approve new
execution protocols. Keep improvements local to the retained pool, tasks
and merge until a separate design earns its complexity.

## Candidate replication makes large rounds expensive

[Medium, component: `lib/task.c`, `lib/merge.c`] M3 scale-8 `tc-cyclic`
at sixteen threads takes 22.512 s and peaks at 23.46 GiB, against
Souffle's 13.616 s and 1.49 GiB; relic's serial peak is 1.46 GiB.
Per-worker screening bounds candidates by workers times distinct new
tuples, and merge staging and scratch add further storage. The
[retained scale-2 diagnostic](../bench/compare/recorded/m3-max/retained-attribution/README.md)
finds about 14 worker copies per unique tuple in the peak round. Its
candidate-finding barrier holds 1.324 GB of accounted storage, including
167.8 MB of worker screening tables no longer read after gathering.
The [screen-release prototype](../bench/compare/recorded/m3-max/screen-release/README.md)
reduces that sampled charged peak by 12.66% and admits a forced merge at
a lower frozen budget, with ordinary replay and ownership preserved.
M3 process RSS falls only 1.1% at sixteen workers; elapsed time gives no
general gain. The [p620 replica](../bench/compare/recorded/freebsd-5995wx/screen-release/README.md)
reproduces the admission result with mixed RSS and noisy timing. Consider
it for its local admission benefit, keeping that separate from the much larger
candidate-copy problem. Releasing all candidates after staging misses
the peak. Global value owners, tuple owners, resumable routing waves and
new private handle formats are not implementation commitments. A design
must demonstrate why the measured waste cannot be removed more locally.
The recorded scale-8 all-pairs tree peak of 109.1 GiB is another capacity
control, alongside low-duplicate workloads that can lose to routing work.

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
many pieces as its driver visits tuples, so the round's 45 million
derivations fall in two tasks. On sixteen threads it takes 5.7 to 5.9 s
against 3.6 to 3.7 s on one, the fastest of five runs on a loaded machine;
with `-X no-reorder`, which starts it at the delta, 1.43 s against 4.95 s.
Measure driver skew and useful suffix work before proposing second-driver
slicing or making the planner choose orders for parallelism. Validate the
existing first-driver path before extending its state machine.

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

## A job of few parts wakes every worker

[Medium, component: `lib/pool.c`] `relic_pool_for()` wakes the whole pool
whatever the job's part count, so a load's commit or a window's canonical
stripe fill, sixteen parts each, wakes 64 or 128 workers at `-j 64` and
`-j 128` (`-j 0` on a 128-thread machine). Measured on a 64-core
Threadripper, that wake costs a flat 0.26-0.7 ms per job whatever the
work, even right after a job while the workers should still be spinning,
so a commit shared at `-j 128` breaks even only at some 24-32 thousand
tuples against some 2 thousand at `-j 16`. Waking at most as many workers
as the job has parts would make every wide pool behave as a sixteen-wide
one for these jobs, and lets one threshold serve every worker count. The
cause of the slow wake is not established; wake-ups serializing on the
pool's mutex until the first woken have spent their spin is the guess to
test first.

## Deferred growth of a few stripes is slower than growing them at once

[Low, component: `lib/relation.c`] When a shared commit
grows only one or two canonical stripes, the part that completes each
growth rehashes the whole stripe alone, some three times slower than the
calling thread does it: appending 1 024 tuples to a store at the edge of
doubling took 5.1 ms deferred against 3.0 ms serial at `-j 16` and `-j 32`,
still slower with four stripes growing, and faster from twelve (8.8 against
19.9 ms). Measure whether the existing immediate-growth path is preferable for
small appends before adding a stripe-count threshold. Keep this inquiry
confined to relation appends; the symbol pool has one ordinary index.

## Driver slicing is not validated beyond its target

[Medium, component: `lib/slice.c`, `lib/round.c`] A round's drivers are
cut and its prefixes run once by worker 0 before the round is shared
(`relic_slice_prepare()`, `relic_slice_cut()`); `facts-join` is the
workload measured. Still to do:

* Tune `RELIC_PARALLEL_STARTUP_WORK` and `RELIC_PREPARATION_SHARE`, set by
  reasoning alone, on rounds with cheap suffixes, on shrinking rounds and
  on declined preparations, counting the links a declined round followed.
* Test `relic_slice_prepare()` and `relic_slice_cut()` directly against an
  independent list of the driver's visits, for every piece count up to
  the visits: the pieces partition the visits in order, a chain shorter
  than its count declines, and an item's preparation follows at most the
  F + 2M links the slicing design allows at every worker count, with no
  positioning walk repeated per worker. Assert `preparation_links` and
  `preparation_declines` in `test_round`.
* Extend `test_round` and `test_exec` to drivers with repeated or
  anonymous terms, lookup drivers over OLD and CURRENT views, a slice
  whose join rejects for longer than a quantum, a chain skewed toward one
  end, identifiers near `RELIC_TUPLE_MAX`, a prefix that computes a
  string when worker 0 then runs no task, refusal of the prepared-item and
  task arrays, and history taken from a shared round against the serial
  one.
* Give a LOOKUP driver's walk one definition. `lookup_start()` in
  `lib/exec.c` and `find_chain()` with `driver_probe()` in `lib/slice.c`
  each decide that a delta seeks from its frontier and every other view
  starts at the chain's head, and `driver_index()` repeats
  `lookup_index()`; were they to disagree, the slices would partition a
  walk other than the one a serial run makes. One helper that opens the
  driver's seek for its view, which a whole run walks to its end, removes
  the second copy.
* Hold a run's form, whole, prefix alone or slice, in one enum rather than
  `prefix_only`, `reached`, `base` and `reach_driver()`'s `sliced`, from
  which `base` follows.
* A slice outside its driver's view, a slice started on a plan with no
  driver and `relic_index_seek_walk()` given no links are asserted, not
  refused, so a release build trusts `relic_slice_cut()` and its callers.
  Decide whether those calls should refuse, finishing the scratch empty,
  as cheap insurance for a store that is otherwise checked on every read.
* Split `a_lookups_slices_derive_what_the_whole_does()` in `test_exec` into
  one scenario each for the whole run's counts, the prefix's two outcomes
  and the slices' partition.
* Raise an interrupt inside the delta seek, the count and the cut. Each
  walk looks for one every `RELIC_SLICE_LINKS` links and an interrupted
  round is abandoned whatever its tasks saw, but no test can yet raise the
  flag at a chosen link; that needs an injection point like the
  allocator's.

## An interrupt from a note stops a shared round late

[Medium, component: `lib/round.c`] A shared round reports its notes once
it has merged (`report_notes()`), so a note callback that raises the
interrupt fails the round after every task has run, and the round's
counters are then transferred and its histories recorded all the same.
Run serially, the first note is reported at the pause that met it and the
evaluation stops there. Both fail, but the counters a failed evaluation
leaves differ, and the callback reads the counters before the round's
reads are added, where a serial callback reads them with every call's
reads already added. Either report notes in a form that lets the round
stop where serial evaluation would, or state that the counters of a
failed evaluation depend on its thread count. `test_interrupt` checks
the status alone.

## Components side by side are not built

[Low, component: `lib/eval.c`, `lib/round.c`] Two components with no path
between them run one after the other, so a program of many small
independent components, such as a Magic rewrite generates, gains nothing
from the pool. The design is
[components side by side](../specs/design-parallel.md#components-side-by-side).
Build it when a workload asks for it.
