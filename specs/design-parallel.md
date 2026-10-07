# relic parallel evaluation design

**Status.** Design document for evaluating on several cores: the worker pool
a context owns, which parts of an evaluation run on it, how a semi-naive
round is partitioned and its results merged in the order a single thread
would have produced them, what the relation store guarantees a concurrent
reader, how the limits and diagnostics stay exact, and what each step is
measured against. Forward-looking, like every document under `specs/`;
`TODO.md` records what is not yet implemented.

**Companions.** `specs/architecture.md` sections 2.1, 26 to 28, 63, 69 and
74; `specs/design-eval.md` for the executor and the two fixed points this
design partitions; `specs/design-evaluation-cost.md` sections 3 and 6 for
the views and the batched insertion it builds on; `specs/design-relations.md`
sections 4 and 6 and `specs/design-index.md` sections 4.1 and 5 for the
store it freezes; `specs/design-memory.md` sections 6 and 10 for the tables
it stripes and the rule it keeps; `specs/design-api.md` section 6 for the
busy state; `specs/design-facts.md` section 4 and `specs/design-import.md`
for the load this design parallelizes last; `specs/decisions.md` D-4,
D-6, D-7, D-10 and D-14.

---

## 1. What this document settles

* where the time of an evaluation goes, measured, and which part of it can
  run on several cores without a change of algorithm;
* the principle everything rests on: **a round reads a frozen store**, so
  the parallel phase has no writer and the question of what a concurrent
  reader may see has an answer by construction rather than by a list;
* that a successful evaluation gives the same result -- every tuple, its
  identifier, the order of every answer, and every counter of work -- at
  any thread count, which is what makes the parallel evaluator testable
  against the serial one and keeps D-4;
* that the parallel evaluator **cannot fail where the serial one would
  not**: whatever goes wrong on a worker, the round is run again serially,
  so every refusal a program can meet is the serial evaluator's refusal;
* the worker pool: owned by the context, created when first needed,
  sharing nothing but the job in progress, with no lock on any data path;
* how a round is cut into tasks, how a task is run, and where its
  derivations go;
* the merge: how a round's candidates become tuples in serial order, and
  how the merge itself is later parallelized without giving that order up;
* what the store must promise, enforced by the type system rather than by
  a list, and the one benign race it keeps;
* the configuration, the tool's switch, the build's detection, the
  sanitizer that guards it, and the tests;
* the order of work, each step with the measurement that gates it;
* what is deferred, and why: loading in parallel, and running independent
  components side by side.

It does not change what a program means, what a plan is, or how the
planner orders a body. Every optimization here is in how a plan is run and
how its results are stored, and none is visible to a program.

---

## 2. Where the time goes, measured

Six workloads of `bench/compare`, at scale 1 unless said otherwise,
without output (`-n`), with `-X stats`, on one Xeon W-3245 (16 cores)
running other work; the seconds are indicative and were read for their
ratios and for the shape of the rounds:

| Workload | Seconds | Rounds | Derivations | Inserted | Looked up | Scanned |
| --- | --- | --- | --- | --- | --- | --- |
| `tc-cyclic` | 5.4 | 4 | 49,775,727 | 1,000,000 | 48,727,000 | 2,048,727 |
| `chain-closure` | 0.7 | 2,001 | 4,002,000 | 2,001,000 | 1,999,000 | 4,004,000 |
| `cspa` | 0.4 | 30 | 2,887,364 | 256,296 | 2,986,189 | 595,543 |
| `andersen` | 0.2 | 32 | 606,055 | 90,581 | 1,255,723 | 533,542 |
| `tc-nonlinear` | 0.3 | 6 | 2,748,982 | 140,369 | 2,603,636 | 421,107 |
| `facts-join`, scale 10 | 1.6 | 0 | 27,694 | 13,847 | 17,604 | 13,847 |

The shapes hold as the data grows. `tc-cyclic` at scale 2 takes the same
four rounds for 201,582,742 derivations and 4,000,000 insertions, one
insertion per fifty derivations again; `chain-closure` at scale 4 takes
8,001 rounds for 64,008,000 derivations and 32,004,000 insertions, one
per two.

A sampled profile of `tc-cyclic` puts the time in four places, in order:
the duplicate check of a derived tuple (`relation_insert_hashed()`, its
probe and its hash, about 40 per cent), the executor's loop
(`execute_plan()`, 25 per cent), the walk of an index chain
(`relic_index_next()`, 17 per cent) and the batch that overlaps the
duplicate checks (`relic_relation_insert_batch()`, 11 per cent). `cspa`
and `chain-closure` have the same shape with the duplicate check and the
hash ahead. The import of `facts-join` is two thirds interning, as
`specs/design-comparison-cost.md` section 2 measured.

Four things follow.

**Almost every derivation is a duplicate, and a duplicate is a read.** In
`tc-cyclic` 49.8 million derivations insert one million tuples; in `cspa`
2.9 million insert a quarter million. Finding that a tuple is already held
is a hash, a probe of the canonical index and a comparison against the
tuple it names, and none of that modifies anything. The parallel part of
this design is therefore not just the join loop; it is the join loop and
the duplicate check together, which is nearly all of the time.

**What remains serial is small.** The tuples that are new must be
appended, indexed and given identifiers in one order. In `tc-cyclic` that
is one insertion per fifty derivations; in `cspa` one per eleven. Section
6 keeps that part serial at first, and section 6.3 says how it is
parallelized when the measurement of section 15 asks for it.

**Rounds come in two shapes.** `tc-cyclic` reaches its fixed point in
four rounds of millions of derivations each; `chain-closure` takes 2,001
rounds of about two thousand. The first shape is where cores help; the
second is a chain of 2,000 dependent steps, and no partitioning of a step
that takes a third of a millisecond can pay for a barrier. Section 5.4
tells the two apart at run time and runs the second shape exactly as the
serial evaluator does.

**Loading is a separate problem.** An import spends its time interning
symbols into two tables that assign identifiers in order of first
appearance. It is parallelized last, by a different decomposition,
section 10, once the evaluator's is measured.

An estimate of what the first three steps of section 15 can reach, by
Amdahl's law with the serial part taken as the merge of section 6 at its
measured share of the insertions: `tc-cyclic` at 16 threads about 7 times
faster, `cspa` about 5, `chain-closure` unchanged. The parallel merge of
section 6.3 raises the first two; nothing raises the third, and nothing
should.

---

## 3. The principle: a round reads a frozen store

`specs/design-eval.md` section 4.2 rests the semi-naive fixed point on one
fact: no view of a round reaches past the end its relation had when the
round began, so *what a round inserts is invisible to every variant of that
round*. `specs/design-evaluation-cost.md` section 6.2 draws the consequence
that a plan's batches may be inserted at any moment during the round
without changing what any plan reads.

This design draws the consequence one step further. **Every insertion of a
round is deferred to the round's end.** While the round's plans run, no
relation any of them reads is modified: not its columns, not its canonical
index, not a secondary index, not its count. The plans read a store that is
frozen, and what they derive is gathered as *candidates* -- tuples the
frozen store does not hold -- and inserted only after the last plan of the
round has finished.

Four things follow, and they are the whole design.

**There is no concurrent writer, so there is no reader-writer question.**
An evaluator that inserted while other threads read the same relation would
have to answer, for every array the store grows, what a reader holding a
pointer or an index into it sees while it is reallocated, rehashed or
relinked; the answer would be a list of arrays and of the places that read
them, and the list would be one entry short the first time a new array was
added. With every insertion after the barrier, a plan running on a worker
reads memory that nothing writes until it has returned. The property holds
for arrays that do not exist yet, which is what "by construction" means
here, and section 7.1 has the compiler enforce it: a task holds the context
and every relation through `const` pointers. The one mutation a plan makes
while it runs, the frontier of an index key, is examined in section 7.2
and kept.

**The result is the same as the serial evaluator's, tuple for tuple.** The
serial evaluator inserts each plan's derivations in the order they are
derived, and the plans of a round in the order they were planned; a tuple
takes the next identifier when it is first inserted and is a duplicate
every time after. The parallel evaluator gathers candidates per task, and a
task is a contiguous piece of one plan's first generator range; so the
concatenation of the tasks' candidates, in task order, is the serial
derivation order with some duplicates removed. The merge inserts that
sequence with the same insertion the serial evaluator uses, so the first
occurrence of a tuple takes the identifier it would have taken and every
later one is found as the duplicate it would have been. Identifiers,
chains, deltas, the order rows are streamed in and every counter of work
come out the same at one thread and at sixteen. Section 12 gives the
argument in full.

**A parallel round can always be run again serially, so it never fails.**
Until the merge begins, the store is exactly as it was when the round
began, and the candidates are the only state the round has produced. A
worker that cannot go on -- its buffer could not grow, for memory or for
the memory limit -- abandons the job; worker 0 drops every candidate,
forgets the workers' counters, and runs the same round on the calling
thread as the serial evaluator does, inserting as it derives. Whatever
that run meets, it meets as the serial evaluator meets it: the same tuple
refused by the same limit with the same message, or the same exhaustion.
The parallel path therefore adds no failure a program can see, no
diagnostic, and no dependence of any outcome on how tasks fell across
threads. Section 8 rests on this.

**The serial evaluator is unchanged and stays the fast path.** A context
with one thread, which is the default, runs today's code: batches are
inserted as the plan runs, nothing is gathered, no barrier is crossed and
the few atomic words of section 4.3 are read and written as plain words.
Deferral is the parallel round's way of working, chosen per round by the
rule of section 5.4, and a round that would not benefit runs the serial way
even on a context with sixteen threads. Both ways leave the store in the
same state at the round boundary -- the same tuples under the same
identifiers -- so a component may mix them freely from round to round.

---

## 4. The worker pool

### 4.1 Ownership and lifetime

A `relic_t` owns its pool. The library holds no global state
(`specs/design-memory.md` section 10), so there is no process-wide pool,
no lazily initialized global and nothing `pthread_once()` would guard; two
contexts on two threads keep two pools that never meet. The pool is
created the first time an evaluation on the context decides to run a round
in parallel, so a context that only loads, or that is configured with one
thread, never starts a thread. It is destroyed with the context, which
joins its threads. A pool carried into a child of `fork(2)` has none of
its threads there, which it knows because it records the process that
created it and compares it before every job and when it is destroyed: in
another process it releases the threads' records without joining them
and is the calling thread alone from then on, so a child may evaluate on
the context as well as destroy it.

The calling thread is worker 0 and takes part in every job; the pool
starts `threads - 1` others. A thread that cannot be started -- `EAGAIN`,
a limit on threads -- is not an error: the pool runs with the threads it
got, down to the calling thread alone, which is the serial evaluator. The
thread count is a performance knob and is not allowed to decide whether a
program can be evaluated.

A thread of the pool blocks every signal but those a fault in the thread
itself raises -- `SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL`, `SIGTRAP`,
`SIGSYS` -- so a signal sent to the process is taken by a thread that is
the caller's, and a fault in a task ends the process as it would on the
calling thread rather than repeating forever. The calling thread cannot
be cancelled while it waits for a job or joins the threads, since the job
lives in its frame and the threads are still reading it.

Between jobs the workers are parked on a condition variable and cost
nothing. Within an evaluation, between two rounds of one component, they
spin for a bounded time before parking, so a component of many small
parallel rounds does not pay a sleep and a wake-up per round.

### 4.2 What a worker runs

A worker runs **tasks**, section 5, from a **job**, one parallel round.
A job is published by worker 0, claimed task by task by every worker, and
completed by a barrier. Nothing else ever runs on a worker: no planning,
no index build, no discard, no diagnostic, no callback of the caller's.
`specs/design-api.md` section 6 makes caller code run only inside the
operation that called it; this design adds that it runs only on the thread
that called it, so an embedder that installed a diagnostic or a row
callback without a thought for threads is right not to have had one.

### 4.3 Synchronization, and where there is none

The design's rule is that no lock is taken on any path that touches data,
and that the shared words a round needs are few and on cache lines of
their own. They are, completely:

* **the claim counter**: claiming a task is one `fetch_add`, wait-free;
  a worker that lost a race has still claimed the next task;
* **the abandon word**: set by a worker that cannot go on, section 4.4,
  and read by every worker between tasks; a worker that reads it set
  claims no more;
* **the completion counter**: one `fetch_add` by each worker as it leaves
  its claim loop, for whatever reason it leaves -- no task left, or the
  abandon word set; so every worker reports and the barrier cannot wait
  for one that failed;
* **the generation word and the job pointer**, by which a job is
  published, section 4.5;
* **the account's total**, section 8, on the rare growth of a candidate
  buffer;
* **the frontiers**, section 7.2.

These words live in the **job**, which a task is handed as its one mutable
handle beside its own scratch; the context and the relations it is handed
as `const`, section 7.1, so the shared words are visible exceptions in a
signature rather than casts in a body. Everything else a task reads is
frozen, and everything else a task writes is its own: the worker's
scratch, candidates and counters, and the task's own entry in the task
array -- its offset, count and note record -- which no other task writes.
Worker 0 reads them after the barrier, whose acquire covers them.

### 4.4 Abandoning a job

A worker abandons a job when it cannot go on: its candidate buffer could
not grow, for memory or because the memory limit refused it, or the round
holds more candidates than the tuple limit could ever admit, section 8, or
its plan met an evaluation error -- an integer overflow, a zero divisor, a
string too long, a pattern that is not one, or a growth of the buffers it
computes strings in that was refused, or the context was interrupted,
which every worker looks for after each return of the executor,
`specs/design-interrupt.md` section 4. The serial rerun meets that error,
or one before it in serial order, and reports it, so an evaluation error
names the first error in serial order at every thread count. It
sets the abandon word, drops its task and leaves its claim loop, reporting
completion as any worker does; every other worker finishes the task it
holds and leaves too, which bounds the wait at one task, and at one
quantum of the executor when the context was interrupted. Nothing is
recorded about why, because nothing is reported: after the barrier, worker
0 finds the abandon word set, releases every worker's candidates, discards
the workers' counters, and runs the round serially, section 3. The records
of a job -- offsets, counts, notes -- are written when the job is published
and never read after it completes, so no record outlives the job.

### 4.5 Publishing and waiting

A job is published under the pool's mutex: worker 0 stores the job
pointer, bumps the generation and broadcasts the condition variable, then
releases the mutex and runs tasks itself. A worker waiting for a job spins
on the generation for a bounded time, with the processor's pause
instruction between looks, and then takes the mutex, re-checks the
generation, and waits on the condition variable while it is unchanged.
Publication and parking are ordered by one mutex, so no wake-up is lost
and no argument about the ordering of two flags is needed; the mutex is
taken once per job and once per park, never per task or per tuple.

The barrier is the same shape the other way round. Worker 0, out of tasks,
spins on the completion counter for the same bound and then takes the
mutex, re-checks the counter, and waits on a second condition variable
while it is short. The worker whose report brings the counter to the
pool's size -- it knows, because `fetch_add` returns the count before it
-- takes the mutex before it signals, so its signal cannot fall between
worker 0's check and worker 0's wait; every other reporter takes no lock.
A worker 0 that spun for ever would burn a core on a host where its
workers are descheduled; one that parks costs a wake-up on a job that
took longer than the spin, which is the job that could afford it.

---

## 5. Partitioning a round

### 5.1 What a round is

The unit of parallel work is what `specs/design-eval.md` already runs one
at a time:

* one **semi-naive round**: the due variants of a recursive component, in
  planning order, each with the plan its delta's size selects;
* the **base pass** of such a component, round 0: its seeds and exit rules
  in program order;
* the **single pass** of a component that is not recursive;
* the plan of a **query that gathers** its answer into its relation.

Each is a sequence of *items* in serial order -- a seed, or a plan with
the relation its EMIT writes -- and each item's derivations are inserted
in that order. The **naive fixed point** is not among them: it is the
oracle of `specs/design-eval.md` section 3.1, a pass of it sees the
insertions of the rules before it, and it stays serial for good. A
**query that streams** its rows is not among them either, section 9.2.

**No item of a round reads a relation another item of the round writes,
except through a view fixed before the round began.** That is what lets
every insertion wait for the round's end, and it holds for each kind: a
variant reads a member through the old, delta or current view, whose
ranges are fixed by the deltas when the round starts; an exit rule and a
seed read no member; a component that is not recursive is one predicate
whose rules do not read it; a query's answer is read by nothing. It holds
by how the schedule and the planner are built, and it is asserted when a
round's items are enumerated, since an item that broke it would not fail
but derive less.

### 5.2 Tasks

An item that is a seed is one task that emits one tuple. An item that is a
plan is cut into tasks by the range its first operation reads:

* when the first operation is a `SCAN`, its view `[first, end)` is cut
  into contiguous chunks, and a task is the plan run with the first
  operation's cursor set to one chunk instead of the whole view; nothing
  else in the executor changes;
* when the first operation is anything else -- a `LOOKUP` whose key is all
  constants, a `MEMBER` or `FILTER` over constants -- the item is one task
  running the whole plan. A chain cannot be cut without walking it, and
  such a plan is rare and short.

The number of chunks follows the item's **estimated work** of section 5.4
and not its range alone: the estimate divided by `RELIC_CHUNK_WORK`,
raised to one and lowered to `RELIC_CHUNKS_PER_WORKER` times the pool's
size and to the range itself, since a chunk holds at least one tuple. A
delta-first plan over a small delta with a large fan-out below it is then
cut finely, where cutting by range would give it one task and a barrier
for nothing. Enough chunks that a worker which draws a heavy one -- a key
held by a thousand tuples, a node of high degree -- is balanced by the
others drawing several light ones, and few enough that claiming and
bookkeeping stay a small fraction of a chunk's work. Both constants are
constants of performance alone, measured on `tc-cyclic` and `cspa` in
section 15; correctness does not depend on how a range is cut, only on
the chunks being contiguous and in order.

The round's tasks are enumerated into the **task array** before the job is
published, in serial order: item by item, chunk by chunk. The array
belongs to the pool, grows to the largest round it has held and is not
charged to the memory limit, since it is sized by the program and the
thread count and not by the data, as the arrays of relation headers are
not; a growth that fails runs the round serially, section 4.4, so nothing
the pool needs can fail an evaluation. A task records the plan it runs,
its chunk, the relation its EMIT writes, and, once it has run, which
worker ran it, where its candidates lie in that worker's buffer, and its
note record, section 5.3.

### 5.3 Running a task

For several workers to run one plan at once, the executor's state belongs
to whoever runs the plan rather than to the plan: an **executor scratch**
holding the frame, an array of operation states sized for the longest plan
it runs, and the batch of derived tuples, one for the serial evaluation and
one per worker. A running plan holds only its plan, its rule and its
target, and is read-only while it runs. For the serial evaluator this is a
refactoring with no change in what it does, and it is the first step of
section 15 so that everything after it is measured against an evaluator of
the same shape.

A task resolves its plan's views into its own scratch when it starts the
plan. Every task of a round resolves the same ranges, since the deltas and
every relation's end were fixed before the job was published and nothing
moves them while it runs, which is section 5.1's rule. A task then runs
the executor exactly as the serial evaluator does, up to its EMIT. The EMIT
appends the head tuple to the scratch's batch of `RELIC_INSERT_BATCH`
tuples; what differs is where a full batch goes. In a serial round it is
inserted. In a parallel round it is **screened** twice:

1. against the frozen store: the batch's tuples are hashed and their
   canonical slots and first candidates prefetched, in the two passes
   `relic_relation_insert_batch()` already makes, and each is looked up in
   the frozen canonical index. A tuple the store holds is counted as a
   derivation and dropped. This is the read-only half of the insertion,
   sharing its hash and its probe, so a tuple screened out here is exactly
   one the merge would have found present;
2. against the worker's own candidates: a tuple the store lacks is looked
   up in the worker's **screen**, one identifier table keyed by the
   tuple's hash folded with its relation's identifier and holding
   positions in the worker's candidate buffer, so that a candidate is
   compared against the relation and the tuple a position names, and a
   component of many members costs a worker one table. A tuple the
   screen holds is a repeat of a
   candidate this worker gathered earlier in this round, and earlier in
   serial order, since a worker claims tasks in increasing order; it is
   counted as a derivation and dropped, and the merge would have found it
   a duplicate. A tuple the screen lacks is placed in it and appended,
   with its 64-bit hash -- the low half is what the tables probe by, the
   high half is what the stripes of section 6.3 are chosen by -- to the
   worker's **candidate buffer**.

The second screen is probed as the first is. The tuples the store lacks
are taken in passes over the batch -- the screen slot each selects is
prefetched, then the entry of the candidate an occupied slot names, then
that candidate's columns -- before any is probed, so that their misses
overlap as the store probes' do; `specs/design-deferral-cost.md` section
4.

**A pending value is screened by value.** A head column holding a value
the plan computed and the context has not assigned -- an integer, or a
string with its bytes -- holds a pending marker, and the worker gives the
value a marker of its own: its position among the distinct pending values
the worker has derived this round, found through a per-worker table keyed
by the integer or by the string's bytes, so that equal values hold equal
markers and a candidate is compared by value as by identifier. The
worker's candidate buffer carries those values, the strings' bytes beside
them, charged as the buffer is. Against the frozen store such a candidate
is new and is not looked up, since a value the context had not assigned
when the round began is in no relation the round reads;
`specs/design-arithmetic.md` section 9.2 and `specs/design-strings.md`
section 9.2.

The second screen is what keeps the candidates proportional to the
worker's *distinct* new tuples rather than to its derivations. Without it
a round whose target starts empty -- a base pass, a component that is not
recursive, a gathered query -- would buffer every derivation, since the
frozen store screens nothing: `q(X) :- edge(X, _)` over fifty million
edges and a thousand distinct `X` would hold fifty million candidates
where the serial evaluator holds a thousand tuples. With it each worker
holds at most a thousand. A tuple derived by several workers is gathered
once per worker and resolved by the merge, so a round's candidates are
bounded by the workers times its distinct new tuples.

A worker runs its tasks one after another, so its buffer holds their
candidates in task order, contiguously; the task records its offset and
count when it finishes. The buffer and the screen are growable arrays
charged to the account, section 8, **reserved before the job is
published** for the candidates the round is expected to gather -- the
tuples its items inserted the last time they ran, which their histories
record, section 5.4; a worker's share of them doubled when the pool has
several workers, since a worker claims whatever tasks it reaches -- and
grown only by a round that exceeds its estimate, from the estimate
rather than from nothing. They are **released at the round's end**,
memory and charge alike, once the merge has taken the candidates: they
hold data, so they count against the limit while they exist, and they
must not exist while anything else asks the limit for room, section 8.
Releasing and reallocating them costs a round a few system calls, which a
round above the threshold of section 5.4 does not notice; a reservation
the limit or the allocator refuses declines the round before it begins,
as a refused string buffer does below. `specs/design-deferral-cost.md`
section 5.

The executor scratches, one per worker, belong to the pool as the task
array does: sized by the longest plan of the evaluation, grown when a
round needs more, not charged, and a failed growth runs the round
serially. The exception is the four buffers sized by the data a plan
meets rather than by the plan -- the text stack, the pending bytes and the
two parts of the match space of `specs/design-strings.md` section 8. A
round whose plans call a string function gives every worker's scratch its
own before the job is published, charged to the account as a round's
buffers, and releases them, charge and all, once the job completes and
before the merge asks the limit for room; a round whose buffers the limit
or the allocator would refuse is declined before it begins and runs
serially.

Every counter the executor advances -- tuples scanned, looked up,
derivations -- is the worker's own, summed into the context's by worker 0
after the barrier, once the round is to be merged rather than abandoned. The sum is what the serial evaluator would have
counted: a chunk reads exactly the tuples the serial plan read while its
first cursor was inside the chunk, and every derivation is counted whether
it was screened out or kept.

A `FILTER` under `RELIC_F_NOTE_MIXED` does not call the diagnostic
callback from a worker. The task records the first mixed comparison it
meets -- the filter and the two values; the rule is the task's -- in its
entry of the task array, and worker 0 reports it during the merge, when
the merge reaches that task, once per rule, through the same
`note_mixed_comparison()` the serial evaluator calls. The note therefore
comes in task order, names the comparison the serial evaluator names
first, and is not reported for a task past one the merge refused, since
the serial evaluator never ran that far. Within the refused task itself
the note may name a comparison the serial evaluator would have reached
only after the refused tuple; that is the latitude a batch already has
(`specs/design-evaluation-cost.md` section 6.2), a task being a batch of
tasks' size.

### 5.4 Which rounds run in parallel

A round runs on the pool when the pool has more than one thread and the
round's **estimated work** is at least `RELIC_PARALLEL_MIN_WORK`. The
estimate is the sum over the round's items of the work the same item did
the last time it ran, measured in tuples read plus derivations from the
counters, scaled by the ratio of its first-operation range now to then;
an item that has not run yet is estimated at its first-operation range.
A recursive component's rounds change size gradually, so the previous
round predicts the next well enough for a threshold whose only cost is a
missed opportunity or a barrier paid for nothing. Beside the history an
item records the tuples it **inserted**, unscaled, since a round's new
tuples do not follow its delta, one record for both plans of a delta
variant; that is what sizes the round's buffers, section 5.3, and an
item that has not run is expected to insert nothing, since its range
bounds neither what it derives nor what of that is new.

`chain-closure` estimates at most six thousand per round and never
crosses the threshold: it runs serially, inserting as it derives, and pays
nothing for the pool but its existence. `tc-cyclic` crosses it in every
round but its first two: the base pass, and the recursive rule's first
round, which the base pass predicts from a closure still empty. That
round inserts most of what it derives, and such a round gains little
from the pool before the parallel merge of section 6.3 whatever its
size: its screen and its merge each hash every new tuple, and only the
screen is shared. A component whose rounds shrink toward its fixed point
-- most do -- runs its last rounds serially, which is right: a barrier
costs more than a round of a hundred derivations.

The threshold sits well above the barrier's cost, which measures some
tens of microseconds, about what a few thousand units of work take,
because a round just above break-even is usually one of those that insert
most of what they derive. A chain of six thousand edges, whose rounds
estimate up to eighteen thousand, runs ten per cent slower at sixteen
threads with the threshold at eight or sixteen thousand, and as fast as
one thread with it at 65 536. That measurement predates the parallel
merge of section 6.3 and the cost `specs/design-deferral-cost.md` takes
out of a deferred round; its section 7 gives the cost model the threshold
is measured against once both are in.

The debug switch `-X parallel`, `RELIC_F_PARALLEL_ALWAYS` in the library,
runs every eligible round deferred, however small its estimate and
whatever the pool's size: on one thread it is the deferred path and the
merge run by the calling thread alone, which is how step 2 of section 15
is tested before a thread exists and how a build without threads keeps
testing the merge. It also cuts every item as finely as section 5.2's
clamps allow, into `RELIC_CHUNKS_PER_WORKER` times the pool's size pieces
or one per tuple of the range, whatever the item's estimate, so that the
test suite's small rounds cross piece boundaries that their estimates
would never make. It changes nothing but the cost, which is what a debug
switch may change.

---

## 6. The merge

### 6.1 In serial order

After the barrier, and before anything is modified, worker 0 asks whether
the round can be merged at all with the candidates still charged: it
projects, for each relation the round wrote, the storage it would hold if
every candidate were new, at the capacities the per-tuple growth would
reach, and asks the account with the buffers' bytes still in the total. A
round that would not fit is abandoned, section 4.4: the buffers are
released and the round rerun serially, where the serial evaluator admits
or refuses each tuple as it does today. A round that fits cannot be
refused by the memory limit during its merge, so the buffers' charge,
which exists only while they do, decides nothing the serial evaluator
would not have decided.

Worker 0 then inserts the round's candidates, item by item
and task by task in the order of the task array, into each task's relation
through `relic_relation_insert_batch()`, exactly as the serial evaluator
inserts a plan's batches. A seed's task screens its one tuple as a batch
of one, so the tuple is a candidate like any other, and its insertion is
the one `insert_seed()` makes. Each insertion's
newness is counted as a tuple inserted, and a task's relation that gained
a tuple is recorded as having gained, which is what the next round's
deltas are advanced from. The merge is the serial evaluator's insertion,
fed from buffers instead of from the EMIT.

The candidates carry their hashes, computed when they were screened, so
the merge hashes nothing; it probes, and the probes of a batch are
prefetched as they are today.

**Pending values are resolved first.** Before phase 1 of section 6.3,
worker 0 gathers every worker's pending values into one array in task
order, each task's candidates in the order its worker derived them and
each candidate's columns in order -- the order the serial evaluator meets
them in -- copying each worker's string bytes once at that worker's own
offset. It measures them, promising each value the identifier the serial
run gives it: its own when the context has assigned one, and otherwise
the next in order of first occurrence. The workers then rewrite their
candidates' markers to those identifiers and hash each rewritten
candidate again, so that the stripes and the screens read final
identifiers. The value table's and the pool's growth join the relations'
in the one question put to the memory limit; the commit's reservation is
made with the relations' in phase 5, where it can fail only for memory,
and a merge declined after it succeeded gives the reservation back,
rewinding the pool's arena so that no copy of a new string is left
behind; the values are appended in the phases that cannot fail, before
the tuples that name them are. `specs/design-arithmetic.md` section 9.2
and `specs/design-strings.md` section 9.2.

**The workers' text buffers are counted as the serial run's would be.**
The serial evaluator asks the limit with its text buffers charged at
whatever capacity they have reached; the merge asks with the workers'
already released. So the merge's admission adds, for each of the four
buffers, the largest capacity any worker's reached. A buffer that doubles
until it fits has a capacity that depends only on the largest request
made of it, and the tasks together make every request the serial run
makes, so that capacity is at least the serial run's at the round's end,
and the admission is at least as strict as every question the serial run
puts in the round. It can be larger for the pending bytes, since a string
the serial run interned earlier in the round is no longer pending there
while a worker, reading the frozen pool, still carries its bytes; a merge
declined on that excess is run again serially, where the limit judges it
exactly. `specs/design-strings.md` section 8.1.

A candidate that another worker's candidate has already inserted is found
as a duplicate in the ordinary way, and that is the whole treatment of
cross-worker duplicates. What reaches the merge is each worker's distinct
new tuples, so the merge probes at most the pool's size times the round's
new tuples and inserts exactly the round's new tuples.

### 6.2 What the merge costs, and why it is serial first

The merge is the serial part of a parallel round, and section 2 sized it:
one insertion per fifty derivations on `tc-cyclic`, one per eleven on
`cspa`. An insertion costs more than a screened derivation -- it appends,
places in the canonical index and links into every secondary index -- so
the merge is a larger share of a round than the insertion counts suggest,
and the Amdahl estimate of section 2 takes it at about a tenth of the
serial time. That bounds the first three steps at roughly seven times on
sixteen threads, which is worth having before the merge is touched, and
it is the measurement that decides whether section 6.3 is built.

### 6.3 The parallel merge

When the measurement says the merge is the ceiling, it is parallelized
without giving up the serial order and without giving up the store's rule
that every step which can fail comes before every step that modifies
(`specs/design-relations.md` section 6).

**Stripes.** The canonical index and every secondary index's key table
become **striped**: `RELIC_STRIPES` ordinary identifier tables, a key
choosing its stripe by the top bits of a 64-bit hash whose low 32 bits are
the keyed hash the tables use for the slot and the tag, and whose high 32
are an unkeyed fold of the same words, made in the same loop. The stripe
is unkeyed so that which stripe a key lives in, and so what each stripe
holds and costs, is a function of the input: a keyed stripe would make a
program's storage, and the insertion a memory limit refuses, differ from
run to run. Crowding keys into one stripe buys an author one larger
table, keyed within as any table is. A secondary index keeps one
`next` array, by tuple, and per stripe a key table and the `entries`,
`frontiers` and `key_values` of the stripe's keys. A key is named by its
stripe and its identifier within the stripe, so each stripe numbers its
keys from one on its own and no stripe's growth waits for another's. The
stripe count is a constant, the same on every context whatever its
thread count, so what a table holds and what it costs against the memory
limit do not depend on `-j`; section 12 needs that. It is a small power
of two, and section 15 step 4 measures its cost on the serial context
before it is kept.

**One insertion: classify, check, reserve, apply.** Striping changes what
an insertion must know before it reserves: the canonical stripe its tuple
falls in, and for each secondary index the stripe of its key and whether
that key is new, since a key's arrays grow in its stripe. An insertion
therefore classifies before it checks. It probes the canonical stripe for
the tuple and each index's key stripe for the key, keeping each key's
hash and what the probe found; checks both limits against exactly the
growth that classification implies; reserves that growth; and applies
it, appending the tuple, placing it and linking it into each index by
what the probes found, probing nothing again. Each index is probed once
per insertion, as before, only earlier, and a key's room is reserved only
when the key is new. That last rule is what lets a whole round's growth
be computed from counts: every array grows one insertion at a time to a
capacity that depends only on how many entries it comes to hold
(`relic_grow_array_capacity_stepwise()`), and every table doubles, so a
count per stripe of new tuples and of new keys fixes the capacities the
serial insertions reach.

**The merge.** Section 6.1 admits a merge only when neither limit can
refuse it. With the round's new tuples classified as one insertion's
tuple is, that admission becomes exact rather than a worst case, and the
merge runs in phases separated by barriers, those marked *parallel* on
the pool:

1. **Find new tuples by stripe** (parallel). Each candidate carries the
   canonical stripe its hash chooses, recorded when its worker screened
   it. Part `s` walks the round's candidates in task order, each task's
   in the order its worker derived them, takes those of stripe `s`, and
   marks each *first* or *repeat* with a scratch screen keyed by target
   and tuple, counting each task's firsts in the stripe. A first is a new
   tuple, since the screen of section 5.3 found it absent from the
   frozen store, and the firsts are exactly the tuples the serial
   evaluator inserts. Stripes share nothing, and each writes only its
   own candidates' marks. Before the phase, each worker lists the
   positions of its candidates by stripe -- a counting sort over the
   counts it kept while screening, one pass, a worker to a part -- and
   part `s` walks each worker's list for `s`, which is in derivation
   order, with a cursor per worker that the tasks advance in task order,
   so that it reads its own candidates and no other's. The same lists
   are made for a target's staging once phase 3 has filled it, per
   canonical stripe and per key stripe of each secondary index, a target
   and index to a part, and phases 4 and 7 walk them;
   `specs/design-deferral-cost.md` section 6.
2. **Count** (serial). Worker 0 turns each task's count of firsts into
   offsets per target in task order, and sums each target's new tuples
   per canonical stripe. The tuple limit is asked of their exact number.
3. **Stage** (parallel). Each task's firsts are copied, columns and
   hash, into their target's staging at those offsets, with the hash and
   the stripe of each secondary index's key. A target's staging lists
   its new tuples in identifier order: the one at offset `i` becomes
   tuple `count + 1 + i`.
4. **Classify keys** (parallel). Part `(target, index, s)` walks the
   target's staged tuples listed for stripe `s` of the index, in
   identifier order, probes stripe `s` of the frozen key table for each key
   and screens the keys it lacks, recording for each staged tuple the
   key it joins: one the index holds, or the new key the first staged
   tuple holding it creates. It counts the stripe's new keys.
5. **Check and reserve** (serial). Worker 0 now knows each target's new
   tuples per canonical stripe and each index's new keys per key stripe,
   which is all an exact projection needs. It checks the memory limit
   against that projection, with the tables the growth replaces held
   beside it until phase 7 frees them, and with the candidates and the
   staging still charged. It then grows every array to exactly that
   capacity -- the capacity the serial insertions reach, so the storage
   total after the merge is the serial evaluator's byte for byte -- and
   allocates each growing table's new slots without yet moving its
   entries.

   Every step that can fail is in phases 1 to 5, and none of them changes
   what a relation holds: a part refused its scratch screen, a limit that
   would refuse the projection, or a growth the allocator refuses
   declines the merge, and the round is abandoned as section 6.1 abandons
   one. Its serial run admits or refuses each tuple as the serial
   evaluator does, so the merge itself never reports and never fails part
   way. A growth refused part way keeps what it obtained, accounted: the
   relations hold what they held, some of them in larger arrays.
6. **Append** (serial). Worker 0 copies each target's staging into its
   columns and advances its count: a copy of `arity` words per new
   tuple, which is memory bandwidth and not a probe.
7. **Place and link** (parallel). The parts of a target are the stripes
   of its canonical index and of each of its secondary indexes' key
   tables, which the relation store numbers and the merge runs every one
   of. Part `s` moves stripe `s` of the target's canonical index into the
   slots phase 5 allocated, if it grew, freeing the old ones, and places
   the stripe's new tuples. Part `(index, s)` does the same for stripe
   `s` of the index's key table, then walks the tuples listed for the
   stripe in identifier order, creating each new key and linking each tuple at the end of its key's chain. Placing
   reads nothing linking writes, so both kinds of part run in one phase.
   Chains are per key and a key is in one stripe, so no two parts touch
   one chain, one entry, one frontier or one `next` slot: `next[last]`
   belongs to the key's chain and `next[id]` to the tuple. Nothing here
   can fail. Worker 0 then reconciles the storage total from what the
   relations hold, as a reservation does.

A table's rehash, which moves every entry it holds, is the largest part
of a growth, and it is the part phase 7 does in parallel: phase 5 only
allocates. Identifiers are assigned in phases 2 and 3 in serial order, so
the result is the serial evaluator's, and phase 5 checks the memory limit
on the capacities the serial evaluator reaches. A lookup through a
striped table costs one shift more than an unstriped one, to choose the
stripe.

**A step's scratch is one allocation.** Every count a step's scratch is
sized by -- each worker's candidates for its list and each stripe's for
its table of firsts and their references, each target's new tuples for
its staging and its lists, each part's tuples for its table of new
keys -- is known before the step begins, from the steps before it. The
step sums them, charges one block to the round, allocates it once and
carves it by part; a part opens nothing, frees nothing and cannot fail.
The blocks of the firsts and of the new keys are released when their
steps end, and a target's staging block when the merge ends, applied or
declined. The tables made in a block take their slots from it rather
than from the allocator, the form of `specs/design-memory.md` section
6; `specs/design-deferral-cost.md` section 6.

---

## 7. What the store guarantees a concurrent reader

### 7.1 The frozen-store contract, checked by the compiler

Between the moment a job is published and the moment its barrier
completes, **no relation is modified**, and the executor cannot modify
one: a task is handed the context as `const relic_t *`, every relation and
index as `const`, and its own scratch as the only mutable state.

`const` alone would not hold that. It covers the members of the structure
a pointer reaches and not what those members point at, and a relation
reaches its columns and its indexes, and the context everything it owns,
through pointers. So the code a task runs -- the executor, and the
screening and candidate buffers of section 5.3 -- is compiled against one
narrow header, `lib/frozen.h`, in which the context, a relation and an
index are **incomplete types**, and refuses to compile beside the headers
that complete them. Such code can hold a pointer to one and pass it on,
and cannot name a member of one, to read it or to write it. What it can do
with them is exactly the functions that header declares --
`relic_relation_tuple()`, `relic_relation_find()`, `relic_relation_all()`,
`relic_relation_index_at()`, `relic_index_first()`, `relic_index_next()`,
`relic_index_first_since()`, `relic_value_order()`,
`relic_value_find_integer()` and `relic_value_find_string()`, the lookup
half of interning, which a plan needs to tell a computed value the context
holds from one it lacks, and the few more the screening needs -- each of
which takes its operands `const` and hands back nothing that could be
written through. What a plan needs of a relation for
every tuple, its arity, the planner copies into the operation. The
exception of section 7.2 reaches its mutable words inside `lib/index.c`,
through a pointer the `const` does not cover, and the account's total of
section 8 is reached the same way: the account is incomplete too, and a
task can only charge and release a round's bytes through the two
functions that do so. A future write on the task
path would not compile, whatever array it reached and however the
representation grew. That is the by-construction form of the table a
design would otherwise keep of what writes when, and the list of when the
store's writers run -- the merge, planning, a load's commit, a query's
clear, the discard -- is a consequence of it rather than a rule.

A reader in a job therefore reads a relation's count, its columns, its
canonical index and its secondary indexes as they stood when the job was
published, holding pointers for as long as it likes within the task.

### 7.2 The frontier: the one write a reader makes

`relic_index_first_since()` moves a key's frontier as it positions a
lookup of a delta (`specs/design-index.md` section 4.1). Several tasks look
up the same delta of the same member in one round, so several workers
would write the frontiers at once. Two decisions make the race benign,
defined, and cheap:

* **`latest_since` is written by worker 0 alone.** Within one round every
  lookup of a delta of one relation is positioned at one `since`, the
  delta's start, which the round fixed before any task ran. Worker 0
  therefore raises `latest_since` for every index of a member when it sets
  the member's delta, before publishing the job, and a lookup never writes
  it.
* **The frontiers are an array of their own**, `frontiers`, by key
  identifier and beside the entries -- per
  stripe once section 6.3 stripes them -- of atomic identifiers reached
  through a pointer in the index, so that a task holding the index as
  `const` can store to them, and so that a frontier's line is not the line
  a lookup reads `first` and `last` from. A task stores a frontier only
  when it moved it, with relaxed ordering, which is a plain store on every
  target relic builds for; a task that reads a stale one walks a few more
  links and reaches the same answer, because every writer of one key's
  frontier in one round walks toward the same `since` and stores the same
  stop, the first chain tuple at or past it or the chain's last tuple,
  whichever it started from -- `walk_toward()` from any tuple at or before
  the stop reaches the stop. Relaxed atomics make the race a defined
  program rather than the undefined one C11 calls a data race, and
  ThreadSanitizer has nothing to report.

Between rounds only worker 0 runs, and the merge sets the frontier of a
new key as an insertion does, to the key's first tuple.

---

## 8. Limits, accounting and diagnostics

**The rule.** A parallel round either completes its merge or is run again
serially, section 3. Every refusal a program can meet is therefore made by
the serial evaluator's code, on the calling thread, in the serial order,
with the serial message: the tuple limit refuses the tuple it refuses
today, and the memory limit refuses the growth it refuses today, in the
serial rerun; the merge declines rather than refuses. No worker reports
anything, and no outcome depends on which worker ran which task.

**The tuple limit** is asked of the round's exact number of new tuples
once the merge has found them, section 6.3 phase 2, before anything is
modified; a round it would refuse is abandoned and run again serially,
where the serial evaluator refuses the tuple it refuses today. The limit
also bounds what a round may gather: a worker whose candidates exceed the
tuples the limit could still admit abandons the job, section 4.4, so a
program that runs away under `-l tuples=` alone meets the limit in the
serial rerun rather than exhausting memory in buffers first.

**The memory limit** has two holders in a parallel round. The relation
store's part, `store_bytes`, is written by the merge alone, on worker 0,
with the projection and reconciliation of `specs/design-relations.md`
section 9. From the reservation until the merge is applied it also
counts the tables the growth replaces, which stay allocated beside their
replacements until the parts of phase 7 move their entries and free
them; the merge's admission asks the limit of that, so the limit holds
while every stripe of every target grows at once. The workers' candidate
buffers and screens are
charged as a second kind of holder, beside the arenas, through an atomic
charge: a compare-and-swap loop that admits the bytes only if the total
with them stays under the limit, and an atomic release. The loop runs once
for the reservation a round makes before it is published, section 5.3,
once per growth beyond it, which is once per doubling, and once for each
step of the merge's scratch, section 6.3. A refused charge records nothing
-- the account's record of a refusal is for an arena worker 0 reports on
-- and is an abandonment, not a failure: the round is rerun serially, and
if the serial evaluator then refuses, it refuses as it does today. The
charge exists so that the buffers count against the bound the caller set
while they exist, and they exist only from a round's reservation to its
end: section 6.1 admits a merge only when the store's growth fits
beside them, and they are released before a serial rerun and before the
next round, so no growth the serial evaluator makes -- in a rerun, a
later round, a merge or an index build -- ever asks the limit for room
the buffers hold. `relic_leave()`'s check that an operation gave back
every charged byte covers them.

What the pool holds for itself -- the task array, the executor scratches
but for the buffers of section 5.3 sized by the data, the threads -- is
sized by the program and the thread count, not by the data, and is not
charged, as the arrays of relation and index headers are
not; a growth of it that fails runs the round serially rather than
failing the evaluation, section 4.4, so a memory limit that admits a
program at one thread admits it at sixteen.

**Diagnostics** are reported from worker 0 only: a limit refusal in the
serial rerun as it happens, a mixed-comparison note once the round is
merged, in task order. The diagnostic callback, the explain callback and
the row callback are never called from another thread, and nothing has to
check that: no worker has a path to any of them.

---

## 9. What runs how

### 9.1 Components

A recursive component evaluated semi-naively runs its base pass and each
of its rounds as section 5 says, each round choosing serial or parallel by
section 5.4. A component that is not recursive runs its one pass the same
way. The naive fixed point runs serially, section 5.1. Components run one
after another in schedule order, as today; section 13 says why they are
not run side by side yet.

### 9.2 Queries

A query that **gathers** its answer runs its one plan as a round with one
item whose relation is the answer, and the answer is delivered from the
relation afterwards as today. A query that **streams** -- one whose plan
derives each row once, on a context without limits -- runs serially on the
calling thread: its rows go to the caller's callback in derivation order,
the callback runs only on the calling thread, and buffering the rows to
reorder them would be the gathered path with the relation left out. The
streaming path is the one that wins on the closure workloads because
formatting rows costs more than deriving them
(`specs/design-comparison-cost.md` section 2); its derivation could be
parallelized by gathering per task and delivering in task order once the
job completes, which keeps the contract that no row is delivered before
everything that can fail has, and is left for a measurement to ask for.

### 9.3 Magic

A query's rewrite is planned and stratified on the calling thread and
evaluated as any schedule is. Nothing in the rewrite is aware of the pool.

---

## 10. Loading in parallel

Section 2 puts the import's time in interning, and interning assigns
symbol and integer identifiers in order of first appearance
(`specs/design-memory.md` sections 7 and 8). A symbol's identifier is
visible to a caller through `relic_value_t`, so a parallel load must keep
it a function of the input alone: the same bytes intern to the same
identifier on every run and at every thread count.

The decomposition that keeps it stripes the tables by hash, as section 6.3
stripes the indexes, with the same constant stripe count, and assigns
identifiers per stripe:

1. **Cut the input at record boundaries** into as many pieces as there
   are workers, each piece a whole number of records. For a fact file the
   cut is a statement end; for a table it is a record end, and whether a
   line feed ends a record depends on the quotes before it, which a
   serial pass counts up to each cut -- a scan at memory speed over bytes
   the parse reads anyway -- so that each worker starts its piece in the
   right state.
2. **Parse each piece on a worker** into the piece's own staging: every
   fact's fields as byte spans into the input with their hashes, and the
   piece's distinct spans. A parse error in any piece fails the load with
   the first error in input order, as today.
3. **Intern by stripe**: worker `s` walks the pieces in input order and
   interns the distinct spans of stripe `s` into stripe `s` of the symbol
   pool and the value table, assigning identifiers from the stripe's own
   counter. An identifier then carries its stripe in a fixed field of its
   index, and a stripe's identifiers are in order of first appearance
   within the stripe, which is a function of the input and the constant
   stripe count.
4. **Resolve and commit**: each worker rewrites its piece's facts as
   identifiers, hashes them, and stages them; the commit reserves and
   appends per base store as today, itself striped by relation.

Steps 1, 2 and 4 are share-nothing; step 3 is share-nothing per stripe.
The change to identifiers -- a stripe field inside the 30-bit index -- is
the part that touches the most code, since `relic_value_decode()`,
`relic_symbol_bytes()` and the two tables' growth must all read it, and
it moves the ceiling `test_ceiling` guards from one table of 2^30 entries
to `RELIC_STRIPES` tables of a share each, which a hash fills evenly. It
is the last step of section 15 and is built only after the evaluator's
parallelism is measured, because the import is a different workload with
a different ceiling: `facts-join` at scale 10 reads five megabytes and
interns three million symbols in about a second and a half, and the
reading itself is a quarter of that.

---

## 11. Configuration and interface

**The library.** `relic_config_t` gains one field, after `flags` so that
a positional initializer written for today's structure still means what
it meant:

```c
struct relic_config {
	size_t		max_memory;
	size_t		max_tuples;
	size_t		max_magic;
	unsigned int	flags;
	size_t		threads;	/* the pool's size; 0 and 1 are one */
};
```

A zeroed configuration still asks for the defaults, and the default is
one thread: an embedder gets the evaluator it had, and asks for more. The
field is a count including the calling thread, at most
`RELIC_THREADS_MAX`; a larger value is refused when the context is
created, as an impossible limit would be, rather than clamped, since a
caller who wrote it meant something else. The comment in `relic.h` that
maps the fields one to one onto `-l` gains the exception. A change to the
structure is an incompatible change to the interface and bumps the
version-info as `specs/design-api.md` section 9 requires; it is made
once, with the counter below, so the interface changes once.

`relic_stats_t` gains `parallel_rounds`, the rounds whose merge completed
on the pool, so that a test can assert the path was taken and a user can
see whether a program benefited. It is reported by `-X stats` with the
other counters, and it is the one counter that differs between thread
counts.

The contract of D-14 stands as the caller sees it: a context is used by
one thread at a time, and distinct contexts share nothing. The threads
this design adds are the context's own, they run only inside an operation
the caller started, and they touch nothing outside the context.

**The tool.** `-j threads` sets the pool's size, in the BSD style of the
other switches, parsed and range-checked as `-l` values are; `-j 0` asks
for the number of processors online, `sysconf(_SC_NPROCESSORS_ONLN)`,
which is the only place the tool asks the machine anything. The default is
one. `-X parallel` is the debug switch of section 5.4. The manual page's
option list and the usage message gain both, and `tests/man.sh` holds them
together as it does today.

**The build.** `configure` looks for POSIX threads and C11 atomics; both
are on every platform relic builds for, and both are system interfaces in
the sense of `specs/architecture.md` section 74, so no dependency is
added. `librelic.pc` names the thread library in `Libs.private` for a
static link. `--disable-threads` builds a library whose pool is the
calling thread alone and whose `-j` is accepted and ignored, for a
platform or a packager that wants no thread; a build where
`__STDC_NO_ATOMICS__` is defined is such a build. Nothing in the language
or the store depends on which was built, and the continuous-integration
matrix of `specs/architecture.md` section 68 holds one job that builds that
way.

`--enable-tsan` and `make check-tsan` run the suite under
ThreadSanitizer. The tests that exercise deferred rounds ask for the pool
themselves, so that every sanitizer suite runs both paths: the unit tests
of rounds, evaluation, allocation failure, the interface and the pool at
four threads under `RELIC_F_PARALLEL_ALWAYS`, and `tests/langtest.sh`
every golden program at one, two, four and sixteen threads under `-X
parallel`. Forcing every evaluation of the suite onto the pool instead
would leave the serial evaluator, which every context of one thread
runs, uncovered by the sanitizer suites. `check-tsan` joins `check-asan`
and `check-ubsan` as a gate: a report is a HIGH finding. `check-asan`
runs the same tests, because a use-after-free in a worker is what a
wrong lifetime would look like.

---

## 12. Determinism

D-4 asks that identical input produce identical output. This design keeps
the stronger statement: **a successful evaluation gives identical output
at every thread count**, and the argument is short because of section 3.

* The tasks of a round are contiguous chunks of each item's first
  generator range, in item order; a plan run over a chunk derives exactly
  what the whole plan derives while its first cursor is in the chunk, in
  the same order, because everything below the first operation is a
  function of the binding and the frozen store, and section 5.1 is what
  makes the store the same one the serial plan would have read.
* Concatenating the tasks' candidates in task order gives the serial
  derivation order with some duplicates removed: those the frozen store
  held, which were duplicates in the serial order too, and those a
  worker's screen caught, each a repeat of a candidate earlier in that
  worker's tasks and so earlier in serial order.
* The merge inserts that sequence as the serial insertion would, in that
  order, so every first occurrence takes the identifier it takes in the
  serial run and every later one is the duplicate it is there. Chains
  are built by identifier order and so are the same; deltas are ranges of
  identifiers and so are the same; rows stream in identifier order.
* Counters of work are sums over tasks of what the serial run counts
  inside the same chunks, and a sum does not depend on who added its
  terms. `parallel_rounds` counts the pool's rounds and is the one
  counter meant to differ.
* The hash seed decides where a key sits within its table and nothing
  reported; the stripe a key lives in is chosen by an unkeyed hash and the
  stripe count is a constant, so what the tables hold and what they cost is
  the same in every run and at every thread count.
* A round abandoned on the pool is rerun serially, and a serial round is
  the serial evaluator; so every refusal is the serial evaluator's, on
  the same tuple or the same growth, with the same message.

A merge is begun only once neither limit can refuse it, sections 6.1 and
8, so an evaluation a limit fails is refused in a round run serially,
after the serial evaluator's own work, and is held to the identity like a
successful one. An evaluation exhaustion fails is not: where the
allocator refuses is not a function of the input, a merge it refuses is
declined and its round run again serially, and a reservation refused part
way keeps the room it obtained, so the relations may hold larger arrays
than the serial evaluator's would. What such an evaluation guarantees is
one diagnostic and an account that holds what the relations hold, which
the next evaluation discards.

A program with aggregates is held to the same identities, after a
successful run, after a run a tuple limit fails and after a run an
overflowing sum fails: a fold reads a complete relation and writes only
its task's scratch, a group rule's pass is shared as any one-pass
component's is, and an overflow abandons the job so that the serial
evaluator reports it, `specs/design-aggregates.md` sections 9.4 and 16.

A program that computes values is held to them too, and they include what
the value table and the symbol pool hold: the merge interns the round's
new integers and strings in the serial order of first occurrence among
the candidates in task order, which is the serial derivation order with
duplicates removed, under the identifiers the serial run gives them, and
reserves the stepwise capacity the serial run reaches. So `integer_values`,
`string_values` and `storage_bytes` agree at every thread count, after a
successful run, after a run a tuple limit fails and after a run an
evaluation error fails; `specs/design-arithmetic.md` section 9.2 and
`specs/design-strings.md` sections 9.2 and 17.

The golden tests are therefore run at one thread and at several, with
`-X parallel`, and compared byte for byte, `parallel_rounds` aside;
section 16.

---

## 13. Components side by side, deferred

Two components with no path between them in the dependency graph could
run at once, each on its own thread, and a program of many small
independent components -- a Magic rewrite generates such -- would gain
from it where partitioning a round gains nothing. It is deferred, and the
reason is recorded so that it is not rediscovered:

* a component is planned when it starts, on worker 0, and planning builds
  indexes over complete relations of earlier components and grows each
  relation's array of index headers; a component running beside that
  planning would read that array while it moved, which is exactly the
  reader-writer question section 3 removed. Answering it needs the index
  headers to be pointer-stable -- an array of pointers to individually
  allocated indexes, grown by publishing a new pointer array and keeping
  the old one until the evaluation ends -- and the same for the reltab;
* the account and the store's tuple total would be written by several
  merges at once, which the atomics of section 8 allow, and the
  materialized record and the explain output would need an order;
* the schedule would have to carry the component dependency edges, which
  `relic_depgraph_t` has and `relic_schedule_t` drops.

None of it is hard, and all of it is a list. It is worth building when a
workload shows the need, after the round-level parallelism that helps the
workloads section 2 measured.

---

## 14. Memory

Per pool, uncharged, sized by the program and the thread count: the task
array, one executor scratch per worker of one frame, one operation state
per operation of the longest plan, and one batch, each grown to the
largest round the pool has run. Per parallel round, charged, sized by the
data and released at the round's end: each worker's candidate buffer and
screen, and for the parallel merge each candidate's stripe and mark, the
staging, the keys' hashes and classifications, and the stripes' scratch
screens.
Nothing is allocated per task, per tuple or inside the executor's loop,
which section 9 of `specs/design-eval.md` promises.

The striped tables of section 6.3 hold what the unstriped ones hold in
`RELIC_STRIPES` pieces, and a relation's minimum table is that many times
the smallest table; section 15 step 4 measures the difference on the
small relations of the test suite before the constant is set.

---

## 15. Order of work

Each step is a commit with its tests and its measurement, in this order,
because each is measured against the one before it. The measurement is
`bench/run.sh` and `bench/compare/run.sh` for relic alone, without output,
at one thread and at sixteen, on the workloads of section 2.

1. **The executor scratch** (section 5.3). The frame, the operation
   states and the batch move from the evaluation and the running plan
   into one scratch structure the executor is handed, and the executor
   takes the context and the relations as `const`, section 7.1, with the
   frontiers moved into their own array, section 7.2; the serial
   evaluator is otherwise unchanged. Gate: every test passes, every golden
   output and every counter is unchanged, and no workload of
   `bench/run.sh` is slower.
2. **Deferred rounds on one thread** (sections 3, 5 and 6.1). Screening,
   candidate buffers and screens, the task array, the merge, and the
   serial rerun of an abandoned round, run by the calling thread alone
   under `-X parallel`, with no pool; the abandonment is exercised by the
   allocation-failure sweep. Gate: the whole suite passes under
   `-X parallel` with output and counters identical to the serial path;
   this is the proof of section 12 before any thread exists.
3. **The pool** (sections 4, 5.4, 7.2, 8 and 11). Threads, the claim,
   abandon and completion words, publication and the barrier, per-worker
   counters, the atomic account, `threads`, `-j`, `parallel_rounds`, the
   threshold, `--disable-threads`, `check-tsan`. Gate: identical output
   and counters at `-j 1`, `-j 2`, `-j 4` and `-j 16` under `-X parallel`;
   `check-tsan` and `check-asan` clean; `tc-cyclic` at least six times
   faster at `-j 16` than at `-j 1`; `chain-closure` at `-j 16` within
   three per cent of `-j 1`; the allocation-failure sweep passes with the
   failure injected in a worker.
4. **The parallel merge** (section 6.3). The 64-bit hash, striped
   canonical index and key tables, the four-phase merge. Gate:
   `tc-cyclic` at least ten times faster at `-j 16`; the serial context's
   `bench/run.sh` and its peak memory unchanged within noise with the
   striping present; the golden files that print storage bytes updated
   once, in this commit, and the same at every thread count; the identity
   gates of step 3.
5. **Loading in parallel** (section 10). Gate: `facts-join` at scale 10
   imports at least four times faster at `-j 16`, and the identifier a
   symbol receives is the same on every run and at every thread count.
6. **Components side by side** (section 13), when a workload asks for it.

What a deferred round costs over a serial one on a single thread, measured
once steps 1 to 4 were in, is taken down by the steps of
`specs/design-deferral-cost.md` section 10, each gated there; the
threshold of step 3 is measured again after them.

Steps 1 and 2 are serial refactorings and could ship on their own; step 3
is the first that starts a thread. The constants -- `RELIC_CHUNK_WORK`,
`RELIC_CHUNKS_PER_WORKER`, `RELIC_PARALLEL_MIN_WORK`, the spin bound,
`RELIC_THREADS_MAX`, `RELIC_STRIPES` -- are set by measurement in the
step that introduces them and recorded with it, as `RELIC_INSERT_BATCH`
was.

---

## 16. Tests

* **Identity.** `tests/langtest.sh` runs every golden test four times
  more, at `-j 1`, `-j 2`, `-j 4` and `-j 16` under `-X parallel`, and
  compares status, output, diagnostics and `-X stats` counters byte for
  byte with the serial run, the `parallel rounds` line aside, whether its
  evaluation succeeds or a limit fails it, per section 12; a difference
  fails the test. `test_round` compares a serial context with a deferred
  one on one thread and on four, word for word on every relation and on
  every counter, including under swept limits. The equivalence suite of
  `tests/equiv` and the random programs of `tests/random` are run the
  same way.
* **The path was taken.** `test_eval` asserts `parallel_rounds` is
  nonzero for a program under `RELIC_F_PARALLEL_ALWAYS` with two threads
  and zero with one, and that a component whose rounds fall below the
  threshold reports the rounds that crossed it and no more.
* **Abandonment.** `test_exhaustion` sweeps allocation failures with the
  pool at four threads and `RELIC_F_PARALLEL_ALWAYS`, so that the failing
  allocation lands in a candidate buffer's or a screen's growth on a
  worker; the sweep's injector becomes an atomic counter, and the
  assertion is that the evaluation either succeeds with the serial
  result or fails once with the serial diagnostic, from the calling
  thread, and that the next evaluation succeeds. `test_query` sets a
  memory limit that admits the relations and refuses a candidate buffer,
  and checks the round completes serially with the serial result and no
  diagnostic. `test_eval` sets a tuple limit smaller than a round's
  candidates and checks the limit's message names the tuple the serial
  run refuses.
* **The same tuple refused.** `test_eval` sets a tuple limit that fires
  in the middle of a merge and checks that the message names the relation
  the serial run names, and the relation holds what the serial run's
  holds.
* **The note.** `test_api`'s mixed-comparison cases run under the pool
  and see the note once per rule, from the diagnostic callback, on the
  calling thread, which the test records with the thread that called it;
  a case with two rules noted in one round sees both, in program order.
* **The frontier.** `test_index` checks `relic_index_first_since()` from
  several threads at one `since` on one chain leaves the frontier at the
  stop every one of them returned, and that a task never writes
  `latest_since`.
* **The contract.** `test_eval` builds a plan whose executor is handed
  `const` relations, which the compiler checks; the test exists so that
  removing the `const` fails a build rather than a review.
* **Striping.** `test_relation` and `test_index` run their suites at a
  stripe count of one and at the constant; membership, lookup, growth and
  accounting are the same.
* **The pool.** `test_pool` compiles the library with `pthread_create()`
  and `pthread_join()` renamed to its own, which count the threads
  started and joined and can refuse a start, since no portable interface
  gives a process's thread count: a context with threads and no shared
  round starts none; the first shared round starts one per worker but
  the calling thread, each with every signal blocked but a fault's;
  destroying the context joins them; a pool whose starts are refused
  evaluates with the threads that started and reports nothing, and
  declines the rounds it would have shared when none started; a pool
  carried into a child of `fork(2)` joins nothing, and a child that
  evaluates does so on the calling thread alone. `test_round` and
  `test_eval` require that the pool's threads ran some of a large
  program's tasks, which no identity check would notice were they never
  to. `test_api` checks that a `threads` above
  `RELIC_THREADS_MAX` is refused.
* **Sanitizers.** `make check-tsan`, `make check-asan` and `make
  check-ubsan` over the whole suite, whose deferred tests run on the pool
  as section 11 describes.
* **Performance.** `tests/perf.sh` gains no timing assertion -- timing is
  not asserted, as that script says -- but `bench/run.sh` gains a column
  for the thread count and `bench/baseline.txt` a run at sixteen, so a
  regression in the speed-up is visible by eye where every other
  performance regression is.

---

## 17. Amendments this design makes

* `specs/decisions.md` D-14: "A `relic_t` is not thread-safe" stands for the
  caller; the context may own threads of its own that run only inside an
  operation, section 4. A new decision records it.
* `specs/design-eval.md` section 2.1: the executor's state is the
  worker's scratch, section 5.3; section 4: a round's insertions may be
  deferred to its end, section 3, and a round may run on several threads;
  section 11: parallel evaluation is no longer absent.
* `specs/design-evaluation-cost.md` section 6.2: the batch may be
  screened rather than inserted, section 5.3; section 6.4: one buffer per
  worker rather than one per evaluation.
* `specs/design-index.md` section 3: the frontiers are an array of their
  own, atomic, and the key table may be striped, sections 7.2 and 6.3;
  section 4.1: `latest_since` is set by the round, and several lookups
  may position at one `since` at once.
* `specs/design-relations.md` section 5: the canonical index may be
  striped, section 6.3; section 6: a round's insertions may be made as
  one merge whose fallible steps precede its appends; section 9: the
  account has a third holder, charged atomically, section 8.
* `specs/design-memory.md` section 6: a striped identifier table and a
  64-bit hash; sections 7 and 8: per-stripe identifiers, section 10, when
  step 5 is built.
* `specs/design-api.md` section 2: `threads` in the configuration and
  `parallel_rounds` in the statistics; section 6: caller code runs on the
  calling thread; section 9: one version-info bump; section 11: "thread
  safety within a context" stays absent for the caller.
* `specs/design-cli.md` section 2: the `-j` switch and `-X parallel`.
* `specs/architecture.md` section 2.1: parallel execution stays outside
  v1 and is specified here; section 69: "parallel SCC evaluation" points
  here; sections 65.3 and 65.4: `--enable-tsan`, `--disable-threads` and
  `check-tsan`; section 68: a job without threads.

---

## 18. What is deliberately absent

* **Concurrent insertion into a shared relation.** A lock-free canonical
  index would let every worker insert as it derives, and would give up
  the serial order: identifiers would depend on which thread got there
  first, output would differ between runs, and the parallel evaluator
  could no longer be checked against the serial one by comparing bytes.
  The frozen round and the ordered merge keep the order, and section 6.3
  parallelizes the merge without a shared write.
* **A lock on any data.** Section 4.3. The mutex exists to publish a job
  and to park a thread that has nothing to do.
* **A failure a worker reports.** Section 3: a worker abandons, and the
  serial rerun fails or succeeds as the serial evaluator does. A design
  in which a worker's refusal was the evaluation's would make the limit
  depend on how tasks fell across threads, and the message on which
  worker crossed first.
* **Work stealing between workers.** Tasks are claimed from one counter
  and are many per worker, which balances skew without a deque per worker
  and a steal protocol.
* **Parallelizing the naive fixed point.** It is the oracle, and a pass
  of it is defined to see the insertions of the rules before it.
* **Parallel planning.** Planning is a small fraction of an evaluation
  and builds indexes over relations other plans read.
* **Thread affinity, NUMA placement, or a thread count chosen by the
  library.** The default is one thread and the tool's `-j 0` asks the
  system; the library never counts processors.
* **A stripe count that follows the thread count.** It would make the
  memory a program needs, and the identifiers a load assigns, depend on
  `-j`; a constant keeps both a function of the input.
* **`pthread_atfork()` handlers.** A pool carried into a child of
  `fork(2)` finds out from the process it records, section 4.1, and a
  handler would be the global state section 4.1 rules out.
* **C11 `<threads.h>`.** Not on every platform relic builds for; POSIX
  threads are, and are a BSD interface in the sense of section 74.
* **Cancelling a worker's thread.** An interrupt reaches a worker as a flag
  it reads between two returns of the executor, and ends its task as any
  other reason to abandon the job does, section 4.4; no thread is
  cancelled or signalled.
* **A screen that forgets.** A fixed-size screen owned by the pool would
  need no sizing and no charge, and a repeat it had evicted would reach
  the merge as a candidate; but the candidates would then be bounded by
  the eviction rate rather than by the distinct new tuples, which is the
  bound of section 5.3. `specs/design-deferral-cost.md` section 11.
* **Buffers kept from one round to the next.** They hold data, and
  data-sized memory is charged while it exists, section 8; a buffer kept
  uncharged would hold memory the limit cannot see, one kept charged
  would make what the limit refuses later depend on whether an earlier
  round was deferred. A round reserves them from its history instead,
  section 5.3.
