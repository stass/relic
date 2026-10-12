# Parallel evaluation

A context may share the work of evaluation among threads it owns. The unit
of sharing is a round of the evaluator: its plans run on a worker pool over a
store nothing modifies while they run. Round-owned partitions retain what
they derive, and the result is merged in the order the serial evaluator
would have inserted it. A successful evaluation gives the same relations,
identifiers, answers, diagnostics and counters at every thread count, and a
shared round never fails: anything that stops it runs the round again
serially, where every refusal is the serial evaluator's. This document
covers the pool, which rounds are shared and how they are cut into tasks,
the routing that bounds candidates by globally distinct new tuples, the
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

From cursor preparation through the workers' completion barrier, no
relation a shared round reads is modified -- not its columns, its indexes
or its count. What the plans derive is gathered as
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

**The result is the serial evaluator's, tuple for tuple**, since tasks are
enumerated in serial order and the merge inserts by the serial rules
([determinism](#determinism)).

**A shared round never fails, because it can always be run again
serially.** Until the merge applies, the store is exactly as the round found
it and all packets, candidates and pending values are private attempt state.
A worker that cannot go on abandons the job; the calling thread drops
private data, forgets the workers' counters and runs the same round
serially. Whatever that run
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

* From cursor preparation through the round's final production barrier,
  no relation, index, value table or symbol pool is modified. The only shared words a task
  writes are the job's coordination words, index frontiers, and the memory
  account's total for its own buffers.
* Tasks are enumerated in serial order. Every candidate retains its minimum
  logical task/emission position, independent of when it arrives.
* A tuple owner retains one copy per distinct new target tuple; a value
  owner retains one copy per distinct pending value. Producer state is
  bounded transport, never a full candidate history per worker.
* Every step of a round that can fail precedes every step that modifies the
  store; any failure before then reruns the round serially.
* Every diagnostic, limit refusal and caller callback happens on the
  calling thread, in serial order.
* What a table holds and costs is independent of the thread count.
* Nothing the pool holds for itself can fail an evaluation.
* Transport has one total byte ceiling. Private owner arrays grow in
  batches under the round account; no per-worker reservation replicates
  a round-sized candidate or value set.

## The worker pool

**Ownership.** A context owns its pool. The library holds no global mutable
state, so there is no process-wide pool and nothing `pthread_once()` would
guard; two contexts on two threads keep two pools that never meet. Threads
are started the first time an evaluation shares a round or an import
batches a table, so a context that only loads program text, or has one
thread, never starts one; a load's commit uses a pool the context already
has and never starts one. The threads are joined when the context is
destroyed. The calling thread is worker 0 and takes part in every
job. A thread that cannot be started is not an error: the pool runs with
those it got, down to the calling thread alone. The thread count is a
performance knob and must not decide whether a program can be evaluated.

**Fork.** A pool carried into a child of `fork(2)` has none of its threads
there. It records the process that started them and checks before every job
-- in the one function that dispatches a job, before it reads the worker
count or touches a synchronization object -- and on destruction: in another
process it releases the threads' records without joining them and is the
calling thread alone, so a child may evaluate on the context as well as
destroy it. The worker count an operation decides by is read only through
a function that makes the same check, so no caller can read a count the
child does not have.

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

**What a worker runs.** Parts of a job -- production lanes, private owner
partitions or a step of the merge -- and nothing else: no planning,
no relation-index build, no discard, diagnostic or caller callback.
Caller code runs only inside the operation
that called it and only on the thread that called it, so an embedder that
installed a callback without a thought for threads is right not to have had
one ([design-api.md](design-api.md)).

**Synchronization.** No lock is taken on any path that touches data. The
shared words are few: a **claim word**, advanced by one `fetch_add` per part,
wait-free; an **abandon word**, set by a worker that cannot go on and read
between tasks; a **completion count**, one `fetch_add` per worker as it
leaves for whatever reason, so the barrier never waits for a worker that
failed; the **generation word** that publishes a job; the **account total**,
on the rare growth of private buffers; and the index **frontiers**
([the benign frontier race](#the-benign-frontier-race)).
Everything else a part writes belongs to its lane or owner for that phase,
including scratch, candidates and counters. Worker 0 reads them after the
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

**A job's filler.** A job may carry a filler: bounded steps of the
caller's own serial work, which only worker 0 runs. Before it claims a
part, worker 0 runs steps while the filler has one; once the parts are
all claimed, and before it counts itself into the job, it runs steps
while the filler has one and another worker has not counted itself in,
reading that count with acquire ordering between steps. It then counts
itself in once and waits at the ordinary barrier. Steps run outside the
pool's mutex. A job that runs on the calling thread alone runs no step.
The pool keeps no state about a filler beyond the job, and a job without
one is unchanged.

A step runs while parts write, so it must neither block nor run caller
code, and it reads and writes nothing a part writes or reads. Only a bulk
import's window-analysis jobs carry one, frozen-read jobs whose parts
write only their window's private slices; the step frames the next
window from bytes already buffered, performing no I/O
([framing ahead](design-bulk-load.md#framing-ahead)). No job that
publishes carries a filler.

**Abandoning.** A private allocation refusal, an exact unique count beyond
admissible tuples, an oversized transport row, an evaluation error or an
interrupt abandons the attempt. A full packet that can drain is ordinary
backpressure, not abandonment. Parts observe the abandon flag at bounded
progress points and all reach their completion barrier. Worker 0 releases
private data, discards counters and runs the selected round serially; no
worker diagnostic escapes. That run finds the same error or an earlier
one in logical order, so the reported error is independent of scheduling.

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

A seed is one task emitting one tuple. An eligible plan's first `SCAN` or
`LOOKUP` is its **driver**, possibly following tests that bind nothing.
The caller executes those tests once, privately, then partitions the
driver's ordered visits into contiguous slices. Tasks start at the driver
and cannot backtrack above it. A plan with an assignment or aggregate
before its first generator remains a whole-plan task. Empty sequences and
rejected prefixes retain an empty task record for their item.

[Ordered driving cursors](design-driver-slicing.md) defines recognition,
cursor lifetime and construction. Scans use arithmetic boundaries; lookup
chains are counted once and walked again to locate match-ordinal boundaries,
with no full match array or persistent index metadata. A slice holds its
first identifier and exclusive fence. Indexed selection and all original
operation views remain intact; slice starts never become delta starts.

The number of pieces follows estimated driver-and-suffix work, including
downstream fan-out, clamped by actual driver visits and a multiple of the
pool's size. Several ordered pieces per worker help distribute uneven
suffix work. One driver tuple remains indivisible. Preparation's prefix
work does not multiply the piece count or the public counters.

Tasks are enumerated into the **task array**, in serial order, before the
job is published. The task array, prepared-item records and executor scratches
belong to the pool, grow to the largest round seen, and are not charged:
they are sized by the program and the thread count, not the data, as
relation headers are. A failed growth runs the round serially, so a memory
limit that admits a program at one thread admits it at any count.

### Which rounds are shared

A round is shared when multiple available workers have enough independent
work to repay preparation, task setup and the pool barrier. The
[scheduling model](design-driver-slicing.md#scheduling-by-useful-work)
uses the measured driver cardinality and downstream costs on a first run,
then scales the selected plan's successful suffix-work history by its
driver visits. Prefix work is kept separate. This model uses the same
execution costing rules as recursive estimation; it does not change that
estimator's pure, thread-independent choice of plan.

Lookup counting and boundary walks have an explicit serial preparation
charge, including a count pass followed by a decision to run serially.
The policy requires useful tasks and an estimated saving over that charge,
with conservative thresholds in lib/round.c. Data skew and merge cost can
make the estimate optimistic, so default enablement requires measured gains
on targeted joins and controls. A one-worker ordinary round performs no
slicing preparation.

**`-X parallel`** (`RELIC_F_PARALLEL_ALWAYS`) defers every eligible round
however small and whatever the pool's size, and cuts every item as finely as
the clamps allow. On one thread it is the deferred path with the merge run
by the calling thread, which is how a build without threads tests the merge
and how small test rounds cross piece boundaries their estimates never make.
It changes only cost, which is all a debug switch may change.

## Running a task

A running task belongs to a resumable lane: its executor scratch, current
slice, partially consumed batch, counters and first note stay together
across production waves. A pool part exclusively runs the lane for one
phase; a different thread can resume it after the barrier. Sliced tasks
start at their prepared driver and cannot backtrack into the once-executed
prefix. Uncut tasks use ordinary whole-plan startup. Views stay fixed for
the entire round, including all routing waves.

### Frozen screening and round ownership

[Bounded candidate routing](design-round-routing.md) defines the collector:

1. The taker counts a consumed head and screens assigned words against the
   frozen canonical index with ordinary batched hashing and prefetching.
   An already stored tuple is dropped. A head containing a value absent
   from the frozen context cannot already be stored.
2. A bounded packet-local screen can coalesce repeated outgoing tuples.
   Local value markers are meaningful only with that packet's dictionary.
3. At the production barrier, value-owner parts give packet pending values
   canonical round handles. A second barrier precedes tuple-owner parts,
   which retain one copy per new target tuple and its earliest logical
   task/emission position.
4. After acknowledgment, packets can be reused and lanes resume. The
   store remains frozen. Nothing is appended until the entire round ends.

The global candidate bound follows distinct new tuples, not workers times
that count. A separate fixed byte ceiling covers transport, including
pending head bytes and routing lists. A full packet drains; a head too
large for empty transport declines the attempt for serial execution.
Producer dictionaries and screens retain no history across waves.

**Counters and notes.** Every consumed head counts, including frozen hits
and duplicates. A row paused for space retains its batch position and is
not counted again on resume. Successful work is summed per task, with
prefix counters once per item. Notes are reported in item/task order,
prefix before suffix, with the existing once-per-rule suppression. The
minimum witness of a retained tuple determines its insertion credit,
not which task reached the owner first. Abandoned work and notes are
forgotten before serial retry.

## The cost of a deferred round

The ordinary serial evaluator inserts batches directly. A deferred round
adds bounded packet collection, owner-table probes, barriers, ordering
references and the reserved append. A fully assigned tuple held by the
frozen store uses its ordinary probe and creates no owner record. A new
tuple is retained once globally; repeats can cause traffic and owner
probes without creating more retained copies. Pending values require a
value-owner phase, skipped entirely when packets contain no new values.

The fixed transport ceiling bounds memory, not traffic. If many distinct
heads recur across waves, owners see them again. A packet-local screen
coalesces nearby repetition, while its storage remains bounded. Owner
skew and the number of barriers therefore join suffix work and append cost
in the sharing decision. A predicted unprofitable round runs serially.
There is no unbounded parallel collector selected for a cheaper estimate.

Indexed slicing also pays positioning, count and boundary walks before
production, as the [driver scheduling model](design-driver-slicing.md#scheduling-by-useful-work)
states. A routing wave does not change that plan selection or task coverage.

Private owners allocate from actual traffic. Insertion history belongs to
a logical variant and can inform a bounded total capacity hint, without
replicating it per worker or eagerly allocating every empty partition.
Capacity grows with distinct tuples and values. Transport and owner data
are released at round end; retaining them uncharged would hide data from
the limit, and retaining them charged would change later serial admission.

The append still stores unique new rows and updates every required index.
Its staging, new-key classification and final relation growth remain real
costs even when candidate replication disappears. Measurements separate
production, local screening, ownership, final ordering and append rather
than treating all private memory as candidate columns.

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
executor scratch floor survives every wave and includes prefix scratch.
Transport allowances can split batches, so pending bytes use the
[conservative serial batch bound](design-round-routing.md#preserve-the-serial-refusal-contract),
not merely the observed packet peak. A conservative refusal runs the round
serially, where the limit judges exactly.

**Order and resolve.** After sealing the round, order globally unique
candidates by their minimum derivation positions. Walk those candidates
and their columns to collect one descriptor per referenced canonical
pending handle. Partition-local maps reuse that descriptor at later
references. The ordinary resolver promises identifiers in semantic
first-occurrence order, and a parallel rewrite replaces owned handles
directly with those promises, recomputing affected hashes and stripes.
Temporary maps and descriptors end at the rewrite barrier; the resolution
map and copied bytes remain live through reserve and apply, as
[pending value compaction](design-pending-values.md) specifies.

The value table's and symbol pool's growth joins the relations' in the
admission question; values are appended in the steps that cannot fail,
before the tuples naming them. A merge declined after reserving gives the
reservation back, rewinding the pool so no uncommitted copy of a new
string remains.

**The order.** The ordered unique vector is exactly the sequence of new
tuples serial insertion observes. Its task positions assign insertion
credit and per-target append offsets. A seed follows the same path as a
one-row task. No second cross-worker duplicate table is needed after
ownership. Fully assigned tuples retain their ordinary hashes; pending
routing hashes are replaced with stored-tuple hashes before staging.

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

1. **Order** (caller and bounded sorting work). Seal the unique owner
   blocks and build candidate references sorted by minimum logical
   position. No production or owner mutation remains.
2. **Resolve** (worker 0, then parallel). Compact referenced canonical
   pending handles in that order and measure them through the ordinary
   resolver. Rewrite disjoint candidate spans to promises and recompute
   final hashes and stripes. Release temporary compaction state at the
   completion barrier.
3. **Count** (worker 0). Each task's unique tuples get positions in its
   target's append, in task order; new tuples are summed per stripe.
   The tuple limit and identifier space are asked of the exact total.
4. **Stage** (parallel). Each task's ordered unique tuples are copied,
   with their hashes and key hashes, into target staging; staging offset *i*
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

**Stripe lists.** Index classification and apply read staged tuples through
counting-sort lists for their canonical or secondary-key stripe, rather
than every part scanning every tuple. Those stripes come from final domain
words; routing partitions are a different private organization. Candidate
references supply logical order, independently of which owner stored them.

**Scoped workspace.** Ordering references and their sort scratch are
charged by unique candidate count. After staging takes the normalized
columns and insertion attribution, owner blocks and references can be
released. New-key classification blocks end once their receipts are ready;
staging and resolver payload remain through reservation and apply. Each
step sizes and owns its checked block, and a refusal leaves cleanup with
one owner for every live allocation. No workspace is sized by all emitted
derivations or by workers times global distinct tuples.

## What the store guarantees a concurrent reader

### The frozen-store contract

`const` alone does not keep a task from writing the store: it covers the
members a pointer reaches, not what they point at, and a relation reaches
its columns and indexes, and the context everything, through pointers. So
the executor, task code and a batch import's window analysis are compiled
against one narrow header, lib/frozen.h, in which the context, a relation,
an index and the memory account are **incomplete types**, and the headers
that complete them refuse to be included beside it. Such code can hold and
pass a pointer to one but cannot name a member, to read or to write. Of
the context's operations it can call exactly the functions that header
declares -- tuple access, index walks, value order, the lookup half of
interning, the screen's probe, charging and releasing a round's bytes --
each taking its operands `const` and returning nothing writable without a
cast the compiler's warnings report; beside them it calls only code that
reaches no context, such as the table decoder.

An incomplete type covers every array a relation holds and any it comes to
hold, so a future write on the task path does not compile, however the
representation grows. That replaces the table a design would otherwise keep
of what writes when; the list of the store's writers -- the merge, planning,
a load's commit, a query's clear, the discard -- is a consequence rather than
a rule. Cursor preparation and tasks therefore read counts, columns and
indexes from one frozen interval. Storage-owned cursor helpers expose only
identifiers and progress, including bounded delta positioning. A prepared
slice keeps identifiers rather than pointers into the store, expires at
round end and cannot survive an index growth, clear or discard.

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

Driver preparation positions at that same delta start through a bounded
storage helper. Its count and boundary walks publish no frontiers. Tasks
start directly from the identifiers preparation found; a slice boundary
is never passed off as a delta start. Thus slicing introduces no second
target for the frontier writers of one round.

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

## Grouped reduction

[Direct group reduction](design-group-reduction.md) uses ordinary executor
batches with a reducer taker in place of candidate collection. Certified
producer rows are already distinct, so their contributions combine into
group state without retaining a tuple per derivation. A source already
stored as a complete relation supplies distinct rows directly.

Parallel reduction uses resumable lanes and a fixed total budget for
partial maps. Each pool part exclusively owns one lane's scratch for a
wave; it need not run on the same physical thread on the next wave.
At barriers the caller drains partials into one private global builder.
The state bound is groups plus the partial budget and retained payload,
not one complete group table per worker. Logical first occurrences,
rather than drain order, determine key and retained-value publication.

A configured memory limit selects serial summary construction, even under
forced deferral. The same logical reduction still runs; this scheduling
rule preserves serial admission when partial combination changes transient
payload lifetimes. Completed summary consumers remain eligible for the
ordinary shared-round path.

No global values or readable summaries are published until construction
and reservation succeed. A failed parallel attempt releases all private
state and reruns the same physical reduction serially. A completed summary
is a frozen read source for ordinary outer-plan aggregate operations;
those operations report type-invalid or overflowing outcomes only when
reached. Outer result tuples still use the ordinary ordered merge.

## Loading in parallel

[Bulk loading](design-bulk-load.md) preserves the ordinary global
first-occurrence identifiers and uses private dictionaries to distribute
work. CSV/TSV input is framed into bounded windows by the same state
machine that decodes it; pieces are independent of worker count. Workers
decode, validate, hash and reconcile values against frozen tables, then
classify records against frozen builders and base stores.

The caller assigns promised identifiers and proves that the entire window
can pass every scalar admission check. An unproved window changes no
semantic state and replays through the scalar importer, which establishes
the first error and retained-value prefix. A proved window reserves all
space before its builder appends; the caller copies the new values' bytes
and appends their entries to the global value tables, and the pool's
striped index is filled a stripe to a part. Barriers separate every
frozen-read phase from reservation and mutation. While a window's
analysis jobs run, the caller frames the next window from buffered bytes
as their filler, in bounded steps it runs in place of claiming parts or
idling at the barrier.

Only the final load commit changes base facts. Empty destinations adopt
complete builder payloads; populated ones receive reserved, ordered
appends with one writer per canonical stripe, run on the context's
existing pool when they are large. Source program parsing and individual fact additions retain
their scalar semantic boundaries and share these builders. Parallel
import neither changes the domain encoding nor contributes evaluation
parallel-round counters. Numbering and entry append remain serial phases
whose measured cost is part of parallel-loading acceptance.

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
  its driving cursor is in the piece, in the same order: the prefix runs
  once, and everything below the driver is a function of its binding and
  the frozen store. The round's rule makes that store the one the serial
  plan reads.
* The ordered unique candidates are the serial derivation order with
  duplicates removed: frozen-store hits and copies whose owner retains an
  earlier logical witness. Owner arrival order changes no first position.
* The merge gives the firsts of that sequence identifiers in order, so every
  new tuple takes its serial identifier; chains are built in identifier
  order, deltas are identifier ranges, and rows stream in identifier order.
* Computed integers and strings are interned in serial order of first
  occurrence, under the serial identifiers and capacities, so the value
  table, symbol pool and storage total agree too.
* Work counters are the prefix counts once per item plus the sums over
  pieces of the serial suffix counts; preparation links add no tuple reads.
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
  persistent storage layout and memory depend on `-j`.
* **`pthread_atfork()` handlers.** A pool learns of a fork from the process
  it records; a handler would be global state.
* **C11 `<threads.h>`.** Not on every platform relic builds for; POSIX
  threads are.
* **Only a bounded local screen.** Forgetting a repeat is safe when a round
  owner retains the global set. Without that owner, buffered candidates
  can grow with all derivations rather than distinct tuples.
* **Skipping ownership with one worker.** Packets can repeat earlier
  packets even on one thread. Forced deferral exercises the same canonical
  values, global uniqueness and ordered publication as a shared round.
