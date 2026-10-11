# Parallel evaluation

A context may share the work of evaluation among threads it owns. The unit
of sharing is a round of the evaluator: its plans run on a worker pool over a
store nothing modifies while they run, what they derive is gathered per
worker, and the result is merged in the order the serial evaluator would
have inserted it. So a successful evaluation gives the same relations,
identifiers, answers, diagnostics and counters at every thread count, and a
shared round never fails: anything that stops it runs the round again
serially, where every refusal is the serial evaluator's. This document
covers the pool, which rounds are shared and how they are cut into tasks,
the screen that bounds a worker's buffer by its distinct new tuples, the
cost model of a deferred round, the ordered and striped merge, what the
store promises a concurrent reader, and the target design for parallel
loading and for components side by side. It sits inside the evaluator,
between planning a round and advancing its deltas.

Related: [architecture](architecture.md#parallel-evaluation),
[threads](architecture.md#threads), [determinism](architecture.md#determinism),
[limits](architecture.md#limits), [evaluation](design-eval.md),
[storage](design-storage.md), [memory](design-memory.md),
[values](design-values.md), [expressions](design-expressions.md),
[library interface](design-api.md), [testing](design-testing.md).

## Model

### Where the time goes

**Almost every derivation is a duplicate, and a duplicate is a read.** On a
dense closure, tens of derivations produce each new tuple. Finding that a
tuple is held is a hash, a probe of the canonical index and a comparison,
and none of it modifies anything. The join loop and the duplicate check
together are nearly all of an evaluation's time, and both are reads.

**What must be serial is small.** New tuples must be appended, indexed and
given identifiers in one order, which is a small fraction of the
derivations where cores help; and that part too is parallelized by the
[striped merge](#the-parallel-merge).

**Rounds come in two shapes.** A dense closure reaches its fixed point in a
few rounds of millions of derivations; a long chain takes thousands of
rounds of a few thousand. Cores help the first. The second is a chain of
dependent steps, and no partitioning of a sub-millisecond step pays for a
barrier. The boundary is a round's size, told apart at run time per round.

Loading is a separate problem: an import's time is interning, which assigns
identifiers in order of first appearance and needs its own decomposition
([loading in parallel](#loading-in-parallel)).

### The principle: a round reads a frozen store

The semi-naive fixed point rests on one fact: no view of a round reaches
past the end its relation had when the round began, so what a round inserts
is invisible to every variant of that round
([a round's insertions may wait](design-eval.md#a-rounds-insertions-may-wait)).
Hence **every insertion of a round may wait for the round's end**. Three
terms name what follows:

* a **deferred round** is a round whose insertions wait until its end;
* a **shared round** is a deferred round whose work is divided among
  threads;
* the **frozen store** is the store as it stood when the round began, which
  is what every plan of a deferred round reads.

While a shared round's plans run, no relation they read is modified -- not
its columns, its indexes or its count. What the plans derive is gathered as
**candidates**, tuples the frozen store does not hold, and inserted after
the last plan of the round has finished. Four things follow, and they are
the whole design.

**There is no concurrent writer, so there is no reader-writer question.** An
evaluator that inserted while other threads read would have to say, for
every array the store grows, what a reader holding a pointer into it sees
during a reallocation, rehash or relink -- a list of arrays and readers, one
entry short the first time a new array is added. With every insertion after
the barrier, a plan on a worker reads memory nothing writes until it
returns. That holds for every array the store has or comes to have, and the
compiler enforces it ([the frozen-store contract](#the-frozen-store-contract)).

**The result is the serial evaluator's, tuple for tuple**, since tasks run
in serial order and the merge inserts by the serial rules
([determinism](#determinism)).

**A shared round never fails, because it can always be run again
serially.** Until the merge applies, the store is exactly as the round found
it and the candidates are the round's only state. A worker that cannot go on
abandons the job; the calling thread drops every candidate, forgets the
workers' counters and runs the same round serially. Whatever that run
meets, it meets as the serial evaluator does: the same tuple refused by the
same limit with the same message, or the same evaluation error. The
parallel path adds no failure, no diagnostic, and no dependence of any
outcome on how tasks fell across threads.

**The serial evaluator remains the fast path.** A context with one thread,
the default, inserts batches as plans run and crosses no barrier. Deferral
is chosen per round, and a round that would not benefit runs serially even
with many threads. Both ways leave the store in the same state at the round
boundary, so a component may mix them from round to round.

## Invariants

* Between publishing a job and its barrier, no relation, index, value table
  or symbol pool is modified. The only shared words a task writes are the
  job's coordination words, index frontiers, and the memory account's total
  for its own buffers.
* Tasks are enumerated in serial order and each worker claims them in
  increasing order, so each worker's candidates are in serial order.
* A worker's candidates are the tuples the frozen store lacked that it had
  not gathered earlier in the round: at most its distinct new tuples.
* Every step of a round that can fail precedes every step that modifies the
  store; any failure before then reruns the round serially.
* Every diagnostic, limit refusal and caller callback happens on the
  calling thread, in serial order.
* What a table holds and costs is independent of the thread count.
* Nothing the pool holds for itself can fail an evaluation.
* Nothing is allocated per task or per tuple: a worker's data-sized buffers
  are reserved before the job and grow, by doubling, only when a round
  exceeds its reservation.

## The worker pool

**Ownership.** A context owns its pool. The library holds no global mutable
state, so there is no process-wide pool and nothing `pthread_once()` would
guard; two contexts on two threads keep two pools that never meet. Threads
are started the first time an evaluation shares a round, so a context that
only loads, or has one thread, never starts one; they are joined when the
context is destroyed. The calling thread is worker 0 and takes part in every
job. A thread that cannot be started is not an error: the pool runs with
those it got, down to the calling thread alone. The thread count is a
performance knob and must not decide whether a program can be evaluated.

**Fork.** A pool carried into a child of `fork(2)` has none of its threads
there. It records the process that started them and checks before every job
and on destruction: in another process it releases the threads' records
without joining them and is the calling thread alone, so a child may
evaluate on the context as well as destroy it.

**Signals.** A pool thread blocks every signal except those a fault in the
thread raises (`SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL`, `SIGTRAP`,
`SIGSYS`). A signal sent to the process is therefore taken by a caller's
thread, and a fault in a task ends the process as it would on the calling
thread, where a blocked fault would repeat forever. The calling thread
disables cancellation while it waits for a job or joins the threads, since
the job lives in its frame and the threads are reading it.

**Waiting.** Between jobs a worker spins for a bounded time, then parks on a
condition variable; the spin spares a component of many small shared rounds
a sleep and wake-up per round. The bound is a time, not a count of looks,
because the processor's pause instruction lasts very different times on
different architectures. A pool with more workers than processors online
parks at once: a spinner there would occupy the processor the working thread
needs.

**What a worker runs.** Parts of a job -- a round's tasks or a step of its
merge -- and nothing else: no planning, no index build, no discard, no
diagnostic, no caller callback. Caller code runs only inside the operation
that called it and only on the thread that called it, so an embedder that
installed a callback without a thought for threads is right not to have had
one ([design-api.md](design-api.md)).

**Synchronization.** No lock is taken on any path that touches data. The
shared words are few: a **claim word**, advanced by one `fetch_add` per part,
wait-free; an **abandon word**, set by a worker that cannot go on and read
between tasks; a **completion count**, one `fetch_add` per worker as it
leaves for whatever reason, so the barrier never waits for a worker that
failed; the **generation word** that publishes a job; the **account total**,
on the rare growth of a worker's buffers; and the index **frontiers**
([the benign frontier race](#the-benign-frontier-race)).
Everything else a task writes is its own -- its worker's scratch, candidates
and counters, its own task record -- and worker 0 reads them after the
barrier, whose acquire covers them.

A job is published under the pool's mutex: worker 0 stores it, advances the
generation and broadcasts. A waiting worker spins on the generation, then
takes the mutex, re-checks, and waits while the generation stays the same. One
mutex orders publication against parking, so no wake-up is lost and no
argument about the ordering of two flags is needed; it is taken once per job
and once per park, never per part or tuple. The barrier is the same shape
reversed: worker 0 spins on the completion count and then parks, and the
worker whose report completes the count -- `fetch_add` tells it so -- takes
the mutex before signalling, so the signal cannot fall between worker 0's
check and its wait. A worker touches the job for the last time when it counts
itself in.

**Abandoning.** A worker abandons when its candidate buffer cannot grow, for
memory or the memory limit; when its candidates exceed what the tuple limit
could admit; when its plan meets an evaluation error (overflow, zero
divisor, a string too long, an invalid pattern, a refused string buffer);
or when the context is interrupted, which every worker checks after each
return of the executor. It sets the abandon word and leaves; others finish
the task they hold and leave, bounding the wait at one task, or one executor
quantum when interrupted. Nothing records why, since nothing is reported:
worker 0 releases the candidates, discards the workers' counters and runs
the round serially. That run meets the same error or one before it in serial
order, so an evaluation error names the first error in serial order at every
thread count.

## Rounds and tasks

### What a round is

The unit of shared work is what the evaluator already runs one at a time: a
**semi-naive round** of a recursive component (its due variants in planning
order); [round 0](design-eval.md#round-0) of such a component; the
**single pass** of a component that is not recursive, a query's group rules
included; and the plan of a **query that gathers** its answer. Each is a
sequence of **items** in serial order -- a seed, or a plan with the relation
its EMIT writes.

**No item of a round reads a relation another item writes, except through a
view fixed before the round began.** A variant reads members through old,
delta or current views whose ranges the round fixes at its start; an exit
rule and a seed read no member; a non-recursive component's rules do not
read it; a query's answer is read by nothing. The property is checked from
the round's plans rather than trusted: a round that reads what it writes --
a pass of the naive fixed point, whose rules read the members as the rules
before them left them -- is declined and runs serially. A round that broke
the rule would not fail; it would silently derive less, which is why it is
checked.

A **query that streams** runs on the calling thread: its rows go to the
caller's callback in derivation order, and buffering to reorder them would
be the gathered path without the relation. A Magic rewrite is planned on the
calling thread and evaluated as any schedule is; nothing in it knows of the
pool.

### Cutting tasks

A seed is one task emitting one tuple. A plan whose first operation is a
`SCAN` is cut by that view into contiguous pieces, a task being the plan run
with its first cursor confined to one piece; nothing else in the executor
changes. A plan starting any other way -- a lookup keyed by constants -- is
one task: a chain cannot be cut without walking it, and such plans are rare
and short.

The number of pieces follows the item's **estimated work**, not its range: a
work-per-piece constant divides the estimate, clamped to at least one and at
most a multiple of the pool's size and the range itself. A delta-first plan
over a small delta with a large fan-out is then cut finely, where cutting by
range would give it one task and a barrier for nothing. Enough pieces that a
worker drawing a heavy one -- a node of high degree -- is balanced by others
drawing light ones, few enough that claiming stays cheap. Correctness
depends only on the pieces being contiguous and in order. The work per piece
is a fraction of the sharing threshold, so a round just past it is cut into
several pieces rather than run by one worker.

Tasks are enumerated into the **task array**, in serial order, before the
job is published. The task array and the per-worker executor scratches
belong to the pool, grow to the largest round seen, and are not charged:
they are sized by the program and the thread count, not the data, as
relation headers are. A failed growth runs the round serially, so a memory
limit that admits a program at one thread admits it at any count.

### Which rounds are shared

A round is shared when the pool has more than one thread and its estimated
work reaches a threshold (constants in lib/round.c). The estimate sums, over
its items, the work the same item did when it last ran -- tuples read plus
derivations -- scaled by the ratio of its first range in this round to its
range in that run; an item that has not run is estimated at its first range. A
component's rounds change size gradually, so the previous round predicts the
next well enough for a threshold whose only cost is a missed opportunity or a
wasted barrier. A chain's closure never crosses it; a dense closure crosses it
every round; a component whose rounds shrink toward its fixed point runs its
last rounds serially, which is right, since a barrier costs more than a round
of a hundred derivations.

**`-X parallel`** (`RELIC_F_PARALLEL_ALWAYS`) defers every eligible round
however small and whatever the pool's size, and cuts every item as finely as
the clamps allow. On one thread it is the deferred path with the merge run
by the calling thread, which is how a build without threads tests the merge
and how small test rounds cross piece boundaries their estimates never make.
It changes only cost, which is all a debug switch may change.

## Running a task

For several workers to run one plan, the executor's state belongs to whoever
runs it: an **executor scratch** per worker holds the frame, operation
states and the batch of derived tuples, and a running plan is read-only.
Every task resolves the same view ranges, fixed before the job was
published, and runs the executor exactly as the serial evaluator does up to
its EMIT. Only a full batch's destination differs: serially it is inserted;
in a deferred round it is **screened**.

### The two-stage screen

1. **Against the frozen store.** The batch is hashed and probed in the
   frozen canonical index, in the batch insertion's own overlapping passes.
   A tuple the store holds is counted as a derivation and dropped -- exactly
   one the merge would have found present.
2. **Against the worker's own candidates.** A tuple the store lacks is
   looked up in the worker's **screen**, one identifier table keyed by the
   tuple's hash folded with its relation's identifier and holding positions
   in the worker's candidate buffer, so a component of many members costs a
   worker one table. A tuple the screen holds repeats a candidate this
   worker gathered earlier in the round, and so earlier in serial order; it
   is counted and dropped. Otherwise it is placed in the screen and appended,
   with its 64-bit hash, to the worker's **candidate buffer**.

**The distinct-tuple bound.** The second stage keeps a worker's candidates
proportional to its distinct new tuples rather than its derivations.
Without it, a round whose target starts empty -- round 0, a
non-recursive component, a gathered query -- would buffer every derivation,
since the frozen store screens nothing: `q(X) :- edge(X, _)` over fifty
million edges and a thousand distinct `X` would hold fifty million
candidates where the serial evaluator holds a thousand tuples. With it each
worker holds at most a thousand, and a round's candidates are bounded by the
workers times its distinct new tuples; a tuple several workers derive is
resolved by the merge. The store is asked first because on a dense closure it
holds the answer for almost every derivation.

**Pending values are screened by value.** A head column holding a computed
value the context has not assigned -- an integer, or a string with its
bytes -- holds a pending marker. The worker renumbers it as its position
among the distinct pending values it derived this round, through a
per-worker table keyed by value, so equal values hold equal markers and the
screen compares by value. Such a candidate is new to the frozen store
without a lookup, since a value the context lacked when the round began is
in no relation the round reads ([design-values.md](design-values.md)).

A worker runs its tasks one after another, so its buffer holds their
candidates contiguously in task order, each task recording its offset and
count.

**Counters and notes.** Executor counters are per worker, summed by worker 0
after the barrier when the round is merged. The sum is the serial count: a
piece reads exactly the tuples the serial plan read while its first cursor
was inside it, and every derivation is counted whether screened out or kept.
A mixed-type comparison noted under `RELIC_F_NOTE_MIXED` is recorded in the
task's record and reported by worker 0 when the merge reaches that task,
once per rule, so it comes in task order and names the comparison the serial
evaluator names first. Within the task where a tuple limit refuses, it may
name a comparison the serial run reaches only after the refused tuple -- the
same latitude a batch already has.

## The cost of a deferred round

On one thread a deferred round does what a serial round does -- runs the
executor, probes the canonical index for every derivation, appends and
indexes new tuples -- and besides:

* **per derivation the frozen store holds: nothing**, since the store probe
  is the insertion's own probe without the insertion;
* **per derivation the store lacks: one screen probe**, which the serial
  evaluator does not pay, its earlier insertion having put the tuple in the
  store;
* **per new tuple: a fixed handful** of places and copies -- the screen
  entry and candidate copy, then in the merge a place among a stripe's
  firsts, the copy into staging, and a place among each index's new keys;
* **per round: setup** -- reserving buffers, the barrier, and a few
  allocations for the merge's scratch.

So the floor is proportional to a round's **new tuples**, not its
derivations or its number of rounds: a round inserting one tuple in hundreds
costs about its serial self, and one inserting a large share of what it
derives costs a multiple of it on one thread and gains only once its work
outweighs that.

**What deferral requires.** Three costs are what deferral is. A tuple
several workers derive is gathered by each and resolved by the merge -- the
choice that keeps workers from sharing a table -- so the merge's table of
firsts stays. The staging assigns identifiers in serial order before any
part applies them, so its copy stays. A worker that could not screen its own
repeats would buffer every derivation, so the screen stays. Everything else
is representation and is kept cheap: the screen is probed in overlapping
passes as the canonical index is, and a tuple it lacks is placed by
re-probing from its hash, since a growth can intervene
([design-memory.md](design-memory.md)).

**A round's buffers are sized once, from history.** A screen grown from a
minimal table moves every candidate one and a half to two times. So each
worker's buffers are
**reserved before the job is published** for the round's expected
candidates, and grow only past the estimate, from the estimate. Each item
records the tuples its last run **inserted** -- serially, the change in the
context's inserted count across its turn; deferred, the firsts the merge
counted for its tasks. The record belongs to what derives the tuples, so the
two plans of a delta variant share one and a round that switches plans is
predicted as well. A worker gets room for all expected candidates when the
pool has one worker, and twice its even share otherwise, since a worker
claims whatever tasks it reaches.

* **Why unscaled.** The work estimate scales with the delta; the inserted
  count does not. A recursive component's new tuples do not follow its
  delta -- a round can read many times the previous delta and insert a
  fraction as much -- and the previous count predicts the next within a
  small factor, where scaling would overshoot by orders of magnitude.
* **Why an item that has not run expects nothing.** Its first view bounds
  neither what it derives nor what of that is new; a rule that reads a
  hundred million tuples to keep ten would reserve gigabytes on that view's
  word. Such an item pays the growth cascade instead.
* **Why buffers are not kept across rounds.** They hold data, and
  data-sized memory is charged while it exists. Kept uncharged, they would
  hold memory the limit cannot see; kept charged, they would make what the
  limit refuses in a later serial round depend on whether an earlier round
  was deferred, breaking the identity across thread counts. They are
  released at every round's end, memory and charge alike.

A reservation the limit or the allocator refuses **declines the round
before it begins**, and it runs serially; a round predicted too large near
the memory limit therefore costs time, never a different answer. When
nothing grows, adding a candidate does no accounting arithmetic; the cost
projection runs only when an array must grow. A round whose plans call
string functions also gives each worker its own data-sized text buffers
([design-expressions.md](design-expressions.md)), charged to the round,
released once the job completes and before the merge asks the limit for
room, and declined like the rest if refused.

**The threshold model.** A round's deferred time is its serial time less its
insertions, divided by the workers, plus its new tuples at the merge's
per-tuple rate, plus the barrier. It is worth deferring when that is below
its serial time, and the threshold sits where the two cross for real
workloads.

## Admission and the ordered merge

After the barrier and before anything is modified, worker 0 decides whether
the round can be merged. Every fallible step -- a scratch block refused, a
tuple or memory limit the new tuples would pass, a relation's identifier
space exhausted -- precedes every step that changes what a relation holds. A
failure **declines the merge**: candidates are released and the round runs
serially, where each tuple is admitted or refused as always. The merge never
reports and never fails part way.

**Admission is never laxer than the serial run.** Memory is asked of
exactly the storage the serial insertions would reach, with the tables the
growth replaces held beside their replacements and the merge's scratch
charged, so an admitted merge cannot be refused while it applies. The
workers' text buffers count as the serial run's would: admission adds, per
buffer, the largest capacity any worker reached, which is at least the
serial capacity because a buffer doubles until it fits
([growable arrays](design-memory.md#growable-arrays)). It can exceed it for
pending bytes -- a string the serial run interned earlier in the round is
not pending there -- and a merge declined on that excess runs serially,
where the limit judges exactly.

**Pending values are resolved first.** Worker 0 walks candidates in task,
candidate and column order and collects one descriptor per referenced
worker-local value. A map per worker reuses that descriptor for later
occurrences, so value measurement consumes the compact sequence without
expanding it per column. The ordinary value resolver promises identifiers
in semantic first-occurrence order. Workers rewrite local markers directly
to those promises and recompute affected hashes and stripes before the
tuple merge reads them. Temporary compaction storage is released at that
barrier; the resolution map and the bytes it borrows remain live through
reservation and apply. These mappings and lifetimes are specified in
[pending value compaction](design-pending-values.md).

The value table's and symbol pool's growth joins the relations' in the
admission question; values are appended in the steps that cannot fail,
before the tuples naming them. A merge declined after reserving gives the
reservation back, rewinding the pool so no uncommitted copy of a new
string remains.

**The order.** Candidates are inserted task by task in task-array order,
into each task's relation, by the serial insertion's rules; a seed's tuple
is screened as a batch of one and is a candidate like any other. Newness is
counted as tuples inserted, and a relation that gained is recorded as
having gained, which is what the next round's deltas advance from. A
candidate another worker's candidate already inserted is found as a
duplicate in the ordinary way: that is the whole treatment of cross-worker
duplicates. Candidates whose columns already hold final identifiers reuse
the hashes computed during screening. Resolving pending columns recomputes
their hashes and stripes before any merge stripe list is built.

## The parallel merge

The merge is the serial remainder of a shared round, and the ceiling on
rounds that insert a large share of what they derive. It is parallelized
keeping both serial order and the store's rule that every fallible step
precedes every modifying one ([design-storage.md](design-storage.md)).

### Stripes

The canonical index and every secondary index's key table are **striped**:
a constant number of ordinary identifier tables, a key choosing its stripe
by an unkeyed fold of its words computed beside the keyed hash. The stripe
choice is unkeyed and the count constant, whatever the thread count, so what
a table holds and costs against the memory limit is a function of the input
alone; slots stay keyed within a stripe, so the hash-flooding defence is
untouched ([striped tables](design-values.md#the-identifier-table)). A
secondary index keeps one `next` array by tuple, and per stripe a key table
with its keys' entries, frontiers and key values. A key is named by its
stripe and its identifier within it, so stripes number keys independently
and no stripe's growth waits for another's. A lookup costs one shift more
than in an unstriped table.

**Classify, check, reserve, apply.** Striping means an insertion must know,
before it reserves, its tuple's canonical stripe and, per secondary index,
its key's stripe and whether the key is new, since a key's arrays grow in its
stripe. So an insertion probes first, checks both limits against exactly
the growth that implies, reserves it, and applies, probing nothing again; a
key's room is reserved only when the key is new. That lets a whole round's
growth be computed from counts: arrays grow stepwise to a capacity that
depends only on how many entries they hold and tables double, so counts per
stripe of new tuples and new keys fix the capacities the serial insertions
reach.

### The steps

Steps are separated by the pool's barrier; *parallel* steps are cut into
parts, each writing only what its stripe, task or worker owns.

1. **Resolve** (worker 0, then parallel). Compact pending descriptors in
   logical first-occurrence order and measure them through the value
   resolver. Rewrite each worker's candidates to promised identifiers and
   recompute their hashes and stripes. Release temporary compaction storage
   at the completion barrier.
2. **Find new tuples** (parallel). Each worker lists its candidates by
   canonical stripe, a counting sort over counts kept while screening. Then
   part *s* walks, task by task, the stripe-*s* candidates of each task's
   worker list and marks each *first* or *repeat* with a table keyed by
   target and tuple, counting each task's firsts. The firsts in task order
   are exactly the tuples the serial evaluator inserts.
3. **Count** (worker 0). Each task's firsts get positions in its target's
   append, in task order; each target's new tuples are summed per stripe.
   The tuple limit and identifier space are asked of the exact total.
4. **Stage** (parallel). Each task's firsts are copied, with their hashes
   and their keys' hashes, into their target's staging; staging offset *i*
   becomes tuple *count + 1 + i*. Each target's staged tuples are then
   listed per stripe of each secondary index.
5. **Classify keys** (parallel). A part per index stripe walks its staged
   tuples in identifier order, probes the frozen key stripe and screens the
   keys it lacks, recording the key each tuple joins and counting new keys.
6. **Check and reserve** (worker 0). The projection is exact. The memory
   limit is checked with the replaced tables held beside their replacements
   and the staging charged; every array is grown to exactly the capacity the
   serial insertions reach, and each growing table's new slots are allocated
   without moving its entries.
7. **Apply** (worker 0, then parallel). Worker 0 appends the new values and
   copies the staging into the columns. Then a part per stripe rehashes the
   stripe into its new slots, places its new tuples, and, for a key stripe,
   creates new keys and links each tuple at the end of its key's chain.
   Chains are per key and a key is in one stripe, so no two parts touch one
   chain, entry, frontier or `next` slot. Nothing here can fail; worker 0
   then reconciles the storage total from what the relations hold.

Every fallible step is in 1 to 6, none of which changes what a relation
holds; a growth refused part way through step 6 keeps what it obtained,
accounted. The rehash, the largest part of a growth, is what step 7 does in
parallel. Identifiers are assigned in serial order in steps 3 and 4, so the
relations, and the storage total byte for byte, are the serial evaluator's.

**Stripe lists.** Parallel steps read their stripe's items through lists
made by counting sort, rather than every part walking every item and testing
its stripe. The sort copies one position per item, once and sequentially;
what it removes is all but one in *stripe-count* of each parallel step's
reads. A part finds a task's run in a worker's list by searching for the
task's first position, so it reads exactly that task's candidates whatever
order the workers ran tasks in.

**One scratch block per step.** Every count a step's scratch is sized by is
known from the steps before it, so the step sums them, charges one block,
allocates once and carves it by part: a part opens nothing, frees nothing
and cannot fail. Tables made in a block take their slots from it, and each
part clears its own, so clearing is parallel too. The firsts' block is
released once they are counted and the new keys' once they are classified,
so neither is held beside the other or at admission; staging is held until
the merge ends. One block cannot serve the whole merge, since new-key counts
exist only once staging does. A block holds every part's tables at once, so
near the memory limit a merge may be declined that part-by-part tables would
have fitted; its round runs serially, slower and the same.

## What the store guarantees a concurrent reader

### The frozen-store contract

`const` alone does not keep a task from writing the store: it covers the
members a pointer reaches, not what they point at, and a relation reaches
its columns and indexes, and the context everything, through pointers. So
the executor and task code are compiled against one narrow header,
lib/frozen.h, in which the context, a relation, an index and the memory
account are **incomplete types**, and the headers that complete them refuse
to be included beside it. Such code can hold and pass a pointer to one but
cannot name a member, to read or to write. It can call exactly the functions
that header declares -- tuple access, index walks, value order, the lookup
half of interning, the screen's probe, charging and releasing a round's
bytes -- each taking its operands `const` and returning nothing writable
without a cast the compiler's warnings report.

An incomplete type covers every array a relation holds and any it comes to
hold, so a future write on the task path does not compile, however the
representation grows. That replaces the table a design would otherwise keep
of what writes when; the list of the store's writers -- the merge, planning,
a load's commit, a query's clear, the discard -- is a consequence rather than
a rule. A task therefore reads counts, columns and indexes as they stood when
the job was published, holding pointers as long as it likes.

### The benign frontier race

Looking up a delta moves an index key's **frontier**, a chain position from
which a later delta's lookup can start
([design-storage.md](design-storage.md)). Several tasks look up the same
delta of one member in a round, so several workers would write one frontier
at once. Two decisions make the race benign, defined and cheap:

* **The recorded start is written by worker 0 alone.** Within a round
  every lookup of a member's delta starts at the delta's start, fixed before
  any task runs, so worker 0 raises the recorded start
  ([lookup of a delta](design-storage.md#lookup-of-a-delta)) on each of the
  member's indexes when it sets the delta, and a lookup never writes it.
* **Frontiers are an array of their own, of atomic words**, reached through
  a pointer in the index, so a task holding the index `const` can store to
  them and a frontier is not on the line a lookup reads a key's first and
  last from. A task stores one only when it moved it, with relaxed ordering,
  a plain store on every target relic builds for.

**Why it is benign.** Every writer of one key's frontier in one round walks
toward the same start and stores the same stop: the first chain tuple at or
past it, or the chain's last. A walk from any tuple at or before the stop
reaches the stop, so a stale frontier costs a few links and gives the same
answer. Relaxed atomics make it a defined program rather than a C11 data
race, and ThreadSanitizer has nothing to report. Between rounds only worker
0 runs, and the merge sets a new key's frontier to its first tuple.

## Failure paths and limits

**The memory limit** has two holders in a shared round. The relation store's
part is written by the merge alone, on worker 0, by projection and
reconciliation. Workers' buffers are a second holder, charged atomically: a
compare-and-swap loop admits bytes only if the total stays under the limit,
and release is atomic. It runs for a round's reservation, once per growth
past it, and once per merge step's block. A refused charge records nothing
-- the account's refusal record is for an arena worker 0 reports on -- and
is an abandonment, not a failure. The buffers exist only from reservation to
round end and are released before a serial rerun and before the next round,
so no serial growth ever asks the limit for room they hold; the check that
an operation gave back every charged byte covers them.

## Loading in parallel

A parallel load decomposes as follows. An import's time is interning, and a
symbol's identifier is visible to a caller, so a parallel load must keep
identifiers a function of the input: the same bytes intern to the same
identifier on every run and at every thread count. That calls for the value
table and symbol pool striped by hash, with the same constant stripe count
as the indexes, assigning identifiers per stripe:

1. **Cut the input at record boundaries**, a piece per worker: a statement
   end for a fact file; a record end for a table, where whether a line feed
   ends a record depends on the quotes before it, which a serial scan counts
   up to each cut so each worker starts in the right state.
2. **Parse each piece on a worker** into its own staging: every field as a
   byte span with its hash, and the piece's distinct spans. A parse error
   fails the load with the first error in input order.
3. **Intern by stripe**: worker *s* walks the pieces in input order and
   interns stripe *s*'s distinct spans into stripe *s* of the tables, from
   the stripe's own counter. An identifier carries its stripe in a fixed
   field of its index, so a stripe's identifiers follow first appearance
   within the stripe -- a function of the input and the constant count.
4. **Resolve and commit**: each worker rewrites its facts as identifiers and
   stages them; the commit reserves and appends per base store, striped by
   relation.

Steps 1, 2 and 4 share nothing, and step 3 shares nothing per stripe. The
cost is a stripe field in the identifier, which decoding and the tables'
growth read, and a per-stripe share of the identifier space, which a hash
fills evenly.

## Components side by side

Two components with no path between them in the dependency graph run at
once on separate workers; a program of many small independent components --
a Magic rewrite generates such -- gains from this where partitioning a round
gains nothing. Running two components at once requires:

* A component is planned when it starts, and planning builds indexes over
  earlier components' relations, growing each relation's array of index
  headers. Index headers and the relation table must therefore be
  pointer-stable: arrays of pointers to individually allocated entries, grown
  by publishing a new pointer array and keeping the old one until the
  evaluation ends.
* The memory account and the store's tuple total written by several
  merges at once, through the atomics the round's buffers use, and the
  materialized record and explain output ordered by schedule position, not
  completion.
* The component dependency edges, which the dependency graph has, carried
  into the schedule.

## Configuration

The library refuses a `threads` value above its maximum when the context is
created rather than clamping it, since a caller who wrote it meant
something else (see include/relic.h). The library reads the processors
online only to decide whether idle workers spin; it never chooses a thread
count and sets no affinity.

`configure` looks for POSIX threads, a monotonic clock and C11 atomics, all
system interfaces ([dependency policy](architecture.md#dependency-policy)).
`--disable-threads`, or a platform lacking one, builds a pool of the calling
thread alone; `threads` is accepted and ignored and nothing in the language
or store differs. `--enable-threads` makes their absence an error rather
than a quiet downgrade. `make check-tsan` runs the suite under
ThreadSanitizer ([design-testing.md](design-testing.md)).

## Determinism

A successful evaluation gives identical output at every thread count,
`parallel_rounds` aside:

* A plan run over a piece derives exactly what the whole plan derives while
  its first cursor is in the piece, in the same order: everything below the
  first operation is a function of the binding and the frozen store, and the
  round's rule makes that store the one the serial plan reads.
* The tasks' candidates in task order are the serial derivation order with
  duplicates removed -- those the frozen store held, duplicates serially too,
  and those a worker's screen caught, repeats of its own earlier, and so
  serially earlier, candidates.
* The merge gives the firsts of that sequence identifiers in order, so every
  new tuple takes its serial identifier; chains are built in identifier
  order, deltas are identifier ranges, and rows stream in identifier order.
* Computed integers and strings are interned in serial order of first
  occurrence, under the serial identifiers and capacities, so the value
  table, symbol pool and storage total agree too.
* Work counters are sums over pieces of the serial counts.
* The hash seed decides where a key sits within a table and nothing
  reported; stripes are unkeyed and constant in number.
* An abandoned round runs serially, so every refusal is the serial
  evaluator's.

A merge begins only once neither limit can refuse it, so an evaluation a
limit or an evaluation error fails is refused in a serial round and held to
the same identity, aggregates and computed values included. An evaluation
the allocator fails is not: where the allocator refuses is not a function of
the input, and a reservation refused part way keeps its room, so relations
may hold larger arrays than serially. Such an evaluation guarantees one
diagnostic and an account matching what the relations hold, which the next
evaluation discards.

## Alternatives not taken

* **Concurrent insertion into a shared relation.** A lock-free canonical
  index lets workers insert as they derive, but identifiers then depend on
  which thread got there first, output differs between runs, and the
  parallel evaluator cannot be checked against the serial one byte
  for byte. The deferred round and ordered merge keep the order; the striped
  merge parallelizes it without a shared write.
* **A failure a worker reports.** The limit would depend on how tasks fell
  across threads, and the message on which worker crossed first.
* **Work stealing.** Many tasks per worker claimed from one counter balance
  skew without per-worker deques and a steal protocol.
* **Parallelizing the naive fixed point.** It is the oracle, and a pass of it
  is defined to see the insertions of the rules before it.
* **Parallel planning.** A small fraction of evaluation, and it builds
  indexes other plans read.
* **A thread count chosen by the library, affinity, NUMA placement.** The
  default is one thread; the library reads the processors online only to
  decide whether idle workers spin.
* **A stripe count following the thread count.** It would make a program's
  memory, and a parallel load's identifiers, depend on `-j`.
* **`pthread_atfork()` handlers.** A pool learns of a fork from the process
  it records; a handler would be global state.
* **C11 `<threads.h>`.** Not on every platform relic builds for; POSIX
  threads are.
* **A screen that forgets.** A fixed-size screen needs no sizing or charge,
  and an evicted repeat reaches the merge, which finds it; but candidates
  would be bounded by the eviction rate, not the distinct new tuples, and a
  round deriving millions of repeats of thousands of tuples would gather
  millions.
* **Skipping the table of firsts with one worker.** Every candidate is then a
  first, but the single-worker deferred round exists to test the merge that
  runs with many; a different path alone would test something else.
* **A threshold weighing expected new tuples.** It follows from the cost
  model, but a single work bound already separates the two shapes of round,
  and a second term is one more estimate that can mispredict.
