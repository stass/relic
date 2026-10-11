# Memory

How relic allocates, how it fails, and what the memory limit counts. Memory
is the foundation every subsystem sits on, through the primitives described
here: arenas for allocations that
share a lifetime, growable arrays over the system allocator for storage that
must resize, a sticky failure flag that keeps a failed construction from
passing silently into the next phase, and one account that `max_memory` is
enforced against. The tables built on these primitives -- the identifier
table, the symbol pool, the value table -- are in [values](design-values.md);
the relations and indexes that hold most of the bytes are in
[storage](design-storage.md).

Related:
[architecture: memory and failure](architecture.md#memory-and-failure),
[architecture: limits](architecture.md#limits), [values](design-values.md),
[storage](design-storage.md), [parallel evaluation](design-parallel.md),
[testing](design-testing.md).

## Model

**Lifetime, not size, decides how a thing is allocated.** Allocations fall
into two kinds.

*Grouped by lifetime.* Parsed rules, plans, a query's compiled rule, the
Magic rewrite of one query, the dependency graph of an analysis: they are
made in large numbers, never freed one at a time, and all die at one moment.
Each such lifetime has an arena, and the arena frees everything in it in one
operation when the lifetime ends. Some arenas live as long as the context
(the program's rules, the symbol pool's bytes, the predicate names), some as
long as an operation (an evaluation's plans, a query's rewrite), some as long
as a query object. The arenas a context owns are declared in `lib/internal.h`;
what matters to a reader is that each one is reset exactly when the things it
holds stop being reachable, so there is no per-object free to forget.

*Individually owned.* Relation tuples and indexes outlive individual queries,
are dropped and rebuilt as the program changes, and are what a memory limit
is chiefly enforced against. They are growable arrays with explicit failure
handling and are never arena allocated. The arrays inside the symbol pool and
the value table are individually owned for a mechanical reason rather than a
conceptual one: an arena cannot resize a block, and an array that doubles
inside one would leave every superseded generation of itself behind. Their
*contents* can live in an arena -- the pool's bytes do -- which is what
keeps a pointer to a symbol's bytes valid for the life of the context even
though the array indexing it has moved. A load's staging, a collected result
and the buffers of a round are individually owned as well.

**Every allocation has exactly one owner and one lifetime.** There is no
reference counting and no shared ownership. A context owns everything it
allocates; a query object owns its answer and its own arena; a collected
result owns its rows and their bytes.

## Invariants

* **The library never aborts and never terminates its host.** An allocation
  that fails is reported as an ordinary diagnostic and the operation returns
  `-1` (or `NULL`). The memory limit refusing a growth takes exactly the same
  path.
* **No mutable global state.** The library contains no writable file-scope or
  global variable -- no cache, no counter, no lazily initialized table, and no
  `_Thread_local` either, which two contexts on one thread would share just as
  surely. Two contexts share nothing, which is what lets an embedder hold
  several without a lock; how the build checks it is in
  [threads](architecture.md#threads).
* **The account is the sum of what its holders hold**, at every moment,
  because each part changes at one site per holder: tables and relations
  reconcile their part from what they actually hold, a charged arena counts
  at its one place of obtaining and releasing chunks, and a round's buffers
  at one compare-and-swap (below). No counter is adjusted at each allocation
  site.
* **A charge precedes its allocation.** A limit that fires has prevented the
  growth, not reported it afterwards.

## Arenas

An arena is a list of chunks; allocation bumps an offset in the head chunk,
and a request that does not fit takes a new chunk, each larger than the last
up to a cap. Nothing is freed until the arena is reset, which releases every
chunk and serves both as the reset between phases and as teardown.

**An oversize chunk is linked behind the head, not in front of it.** A request
larger than the chunk size gets a chunk of exactly its own size. Pushed at
the head, that chunk would hide the partly used chunk before it, and since it
is full the moment it is made, every small allocation that followed would
take a fresh chunk of its own. Linked behind the head, it leaves the partly
used chunk where the next small allocation finds its room.

**Alignment is the creator's choice.** Every request is rounded up to the
arena's alignment, so the running offset stays aligned and no allocation
inspects or repairs it. An arena of structures uses the strictest alignment
any object needs; an arena of byte strings -- the symbol pool's, the
predicate name store -- uses one, so each string follows the previous one with
no padding. A request of zero bytes is rounded up like any other, so it yields
a distinct non-null pointer, and a null return means failure and nothing
else.

**A mark and a rewind** return an arena to an earlier state, freeing every
chunk obtained since. An owner that makes several allocations that must take
effect together or not at all -- the pool copying a batch of new strings --
marks first and rewinds if it declines. The failure flag is not part of the
mark; an owner that rewinds after a refusal clears the flag itself.

**A charged arena** is attached to the memory account when it is initialized,
asks the account before it obtains each chunk, and gives the chunk back to the
account on reset. Attached and unattached arenas are made by two distinct
initializers, so an arena's accounting class is a choice its creator makes in
the open where a reviewer can see it. An arena owned by a charged table -- the
pool's string arena, the predicate table's name store -- is not attached: the
table counts it within its own part, projecting its chunks before it grows.

## The sticky failure flag

An arena allocation that cannot be satisfied returns `NULL` and sets a failure
flag on the arena. No subsequent allocation clears it, and while it is set the
arena refuses every request without calling the allocator. The contract has
three parts, and each exists for a reason:

1. **Callers check the returned pointer**, exactly as they would check
   `malloc()`. Nothing about the arena changes ordinary defensive C.
2. **A helper deep in a construction may return early without inventing an
   error path.** If it allocated ten nodes and the third failed, the seventh
   also returns `NULL`, so the helper need not thread a status through every
   level to stop the remaining work from running against a half-built
   structure.
3. **The phase that owns the arena confirms the flag at its boundary** before
   handing its output to the next phase, and fails the whole operation if it
   is set. This is what makes the first two safe: a missed pointer check
   cannot turn into a silently truncated program.

Refusing every request after a failure is deliberate. It keeps the flag from
being contradicted by a subsequent small allocation that happens to succeed,
and it stops an unwinding path from issuing a storm of requests to an
allocator that has already said no.

**The flag belongs to a phase, not to a lifetime.** The contract above
describes an arena that is reset when its phase ends, and the reset clears the
flag. The symbol pool's string arena lives as long as the context and is
never reset, so a flag left standing on it would turn one transient exhaustion
into a context that can never intern anything again -- and a REPL or a
long-running embedder that reported the failure and carried on would find
every subsequent operation failing for a reason that had passed. The string
arena is therefore reached only through the pool's reservation, which
rewinds any partial copy and clears the flag once the failure has been
observed and is about to be reported. This is a funnel rather than a rule to
remember: nothing else can reach the arena, so no future caller has to know.
What the exception gives up is the second part of the contract, and it gives
it up safely, because each allocation there is one string's bytes stored by a
caller that checks the pointer. The deep construction that wants the early
return is the parsed program, and its arena is reset.

**An operation reports running out of memory once.** A flag that refuses every
subsequent request means one exhaustion can be met many times: by a caller
that makes a second allocation before checking the first, by a construction
whose helpers each return early, by the boundary that confirms the flag after
a helper already reported. The reporting function remembers, for the
operation in progress, that it has reported, and stays silent for the rest of
the operation, so the message is the first failure's. That makes "check each
allocation before the next" a matter of style rather than correctness. The
next operation reports its own.

## Growable arrays

Every growable array resizes through one function, `relic_grow_array()`
(`lib/util.h`). It returns the array as it is when the capacity suffices,
and otherwise doubles it, never below the requested count and from a small
minimum, rejecting a product that would overflow. On failure it returns
`NULL` and leaves the caller's array and capacity untouched, so a failed
growth loses nothing already stored. The point is not that resizing is hard;
it is that the doubling, the overflow check and the failure discipline are
written once, while the array stays a plain typed pointer the surrounding code
indexes directly.

**Pure companions predict a growth without making it**, so that a limit can
be asked first, and each is what the growth itself is written in terms of, so
the prediction and the policy cannot drift apart. One gives the capacity a
single growth reaches. Another gives the **stepwise** capacity: what a
sequence of one-at-a-time growths reaches. The two differ, because a growth
raises a capacity to the request only when doubling falls short: one request
for 128 more items from a capacity of 8 lands on 136, while 128 single
insertions land on 256. An owner that makes room for many insertions at once
-- a round's merge, a load's commit, a batch of computed values -- grows to the
stepwise capacity in one allocation, so it holds exactly what inserting one at
a time would have held. That is what makes the bytes a table holds a function
of what it holds and not of how it was batched, and so the same at every
thread count.

**A buffer sized by the data a plan meets doubles until it fits**, and is
never raised to the request. The text stack, the pending bytes and the regex
matcher's buffers ([expressions](design-expressions.md)) are rooms a
run asks for rather than arrays of counted items. Their capacity is then a
function of the largest request alone, whatever came before it, which is
what lets a shared round's merge ask the limit for the serial capacity
without having run serially ([round-scoped buffers](#round-scoped-buffers)).

## The memory limit

`max_memory` is **a ceiling on accounted storage and evaluation memory**,
not on the process: it bounds what the program's stores hold and what
running the program allocates. It does not bound the program as written,
and it does not bound peak usage.

**What it charges**, each part kept by its holder in the context's account
(`lib/account.h`):

* relation and index storage -- columns, the canonical index, every secondary
  index, and an index while it is being built -- over base stores and derived
  relations alike. A fact of a derived predicate is held twice, in its base
  store and in its derived relation, and counted twice;
* the integer table and the value table's record of which symbols are string
  values;
* the symbol pool, whole: entries, identifier table and string arena;
* the predicate table, whole: entries, index, the source-name array and its
  index, and the name store holding predicate and source names;
* the evaluation arena, the arena of a query's **group run** (the step that
  runs each of the query's aggregate group rules once, in an arena of its
  own; see [aggregates](design-aggregates.md#queries)), and the arenas of a
  query's Magic rewrite;
* the round-scoped buffers ([below](#round-scoped-buffers)).

[Grouped reduction](design-group-reduction.md#ownership-failure-paths-and-limits)
also charges its run-scoped group tables, accumulator arrays, retained key
and extreme bytes, bounded partial maps, ordering scratch and resolution
descriptors at capacity. Their global state follows groups rather than
source rows; removing a source store creates no accounting exemption for
its replacement. Published summaries stay charged through their consumers
and are released when the run ends. The logical source row ceiling that
bounds exact sums is separate from the configured stored-tuple limit.

Summary construction is serial whenever `max_memory` is set. This gives
the same allocation and refusal sequence at each worker count; a partial
group merge can otherwise retain different temporary extrema at different
times. Unrestricted parallel construction still charges every allocation,
and its bounded partial budget is independent of the number of groups.

Each is charged because it grows with what running the program does, not only
with what the caller wrote. `n(Y) :- n(X), Y = X + 1` interns an integer per
derivation and `s(Y) :- s(X), Y = cat(X, "a")` a string per derivation; each
new adornment a rewrite generates adds predicate names. The evaluation arena
holds plans whose count and length are each a body's length, and the rewrite
arena a copy of every body per call pattern: both are the square of what the
author wrote, at the author's choice, and nothing else would bound them. A
fact is not planned, so it adds nothing to the evaluation arena and counts
once, as its tuple. The tables are charged *whole*, literals and
computed values alike, rather than only the part evaluation added, so the
account stays a sum of what exists instead of a record of who allocated it.
Names and values the caller writes go through the same admission as computed
ones: a load, an import, a fact or a query whose values would take a table
past the limit is refused as it interns them.

**What it does not charge** is sized by what the caller wrote: the arenas
holding the program's rules, the schedule and a prepared query, the analysis
scratch the dependency graph is built in, and the parser's held bytes for the
statement being parsed; the text a load reads and the staging that holds its
facts until commit (proportional to the text and released when the load
returns); the tables and evaluation order of a rewrite, a constant fraction of
the rewrite the limit does count; and the tables of relation pointers, the
individually allocated relation headers and each relation's array of
secondary-index headers, all sized by the program's predicates and binding
patterns. A collected result is the caller's and charged to nothing. A
caller-owned arena *cannot* be charged at all: a query may be destroyed after
its context, and an arena charged to an account the context owns could not give
its bytes back once the context is gone.

[Bulk-load builders](design-bulk-load.md#ownership-and-memory-accounting)
and bounded import windows belong to that input-sized staging. An empty
base store takes ownership of a builder payload at commit and counts its
actual capacity once in the store account. An append reserves ordinary
destination capacity while its new-row builder remains private. Moving
ownership changes which holder accounts for the bytes; it does not allocate
another copy or give an adopted array an accounting exemption.

The pool's task array, per-item cursor-preparation records and fixed
executor state are bounded by the program and worker count and are not
charged. [Driving slices](design-driver-slicing.md#bounds-and-storage-ownership)
add no data-sized match array. Prefix function buffers use the ordinary
charged round scratch and contribute to its retained serial-memory floor,
even when a prefix rejects before a worker runs the driver.

**It charges capacity, not transients.** A holder is charged the bytes its
arrays actually hold -- allocated capacity, not the smaller number a count
implies, since a limit that ignored the difference would let storage reach
twice what the user allowed. The second generation an array passes through
while `realloc()` copies it is not charged: the limit bounds what is held, not
what a copy passes through, nor what the allocator keeps afterwards. A grown
table that is deliberately kept allocated beside the one it replaces across a
phase, as a round's merge does, is held, and is charged.

**Every growth is projected before it is made.** The holder computes the bytes
it would hold afterwards with the same pure functions its growth uses (the
growable-array companions above, the identifier table's bytes after a
reservation, an arena's projection of its next chunks), asks the account
whether the total with that projection stays within the limit, and only then
allocates. The account answers against the whole total -- every holder's part
-- so no holder can spend what another was admitted. A charged arena asks per
chunk, against the chunk's whole size, so the check is paid once per chunk and
the limit can fire up to one chunk before the total would have crossed it.

**A table's or relation's total is maintained by reconciliation, not by
bookkeeping at each site.** After a growth, successful or not, the holder
sets its part of the account to what it then holds. That is exact whether a
reservation grew every array, grew one and then failed, or grew nothing, so
no failure path has to remember to give anything back. A total incremented
where allocations succeed and decremented where they fail would be a list of
places to remember, and a step added afterwards would not be on it.

**A refusal is reported as the limit, through the exhaustion path.** A holder
the limit refuses fails exactly as if the allocator had refused it, and the
account records the refusal. The one function every exhaustion reports
through takes that record and reports the limit instead --
`relic: error: limit exceeded: memory=N while deriving p/2` -- so no caller
distinguishes the two and every refusal reaches the user in one form. A
refused deferred round's buffers are the exception, and are not recorded
([below](#round-scoped-buffers)).

### Round-scoped buffers

The executor allocates nothing plan-sized while it runs; plans are built
before. What it does allocate is data-sized scratch, which is admitted and
may grow during execution: the text stack and the pending bytes of the
strings its plans compute, the regex matcher's work space, and, in a
**deferred round** (one whose insertions wait for its end; see
[parallel evaluation](design-parallel.md#model)), the candidates it has
derived. These buffers live for one round and are charged as one part of
the account.

The merge's [pending-value workspace](design-pending-values.md#storage-ownership)
is charged through that same round account. Local marker maps and compact
descriptors are sized by worker-local distinct values and released after
the candidate rewrite barrier. The resolution map and the copied string
bytes are opened only when some candidate holds a pending value, the map
sized by the values candidates reference; they are charged together and
remain charged until value append or reservation rollback and cleanup.
Capacity, alignment padding and the addressable byte used for empty-string
payloads are counted; early release follows actual ownership lifetime.

**They grow on several threads at once**, so their part of the account is
one atomic word, raised by compare-and-swap only when the whole total with
the new bytes stays within the limit. Every other part changes only on the
calling thread while no round's workers run.

**A shared round's merge admits them as the serial run would hold them.** A
shared round is a deferred round whose work is divided among threads. Before
its merge, admission adds per buffer the largest capacity any worker
reached. A buffer doubles until it fits ([growable
arrays](#growable-arrays)), so its capacity is a function of the largest
request alone, and the workers together make every request the serial run
makes: the largest capacity any of them reached is at least the serial
run's. It can exceed it for pending bytes -- a string the serial run
interned earlier in the round is not pending there -- so admission is never
laxer than the serial run, and a merge declined on that excess runs
serially, where the limit judges exactly.

**A refused deferred round reports nothing of its own.** In a serial round a
buffer growth the limit refuses is reported as any refusal is. In a deferred
round it is not recorded: the round is run again inserting as it derives,
which holds no candidate buffers, and whatever refuses that run reports
itself.

## The system allocator

A growable array is a block of the system allocator: it grows by `realloc()`,
an identifier table takes its slots from `calloc()`, and both are released by
`free()`. A freed generation the allocator keeps for reuse is the allocator's
to manage and give back. Mapping large arrays itself would let relic unmap an
outgrown generation at once, but by the physical footprint memory pressure
charges that is no steadier, since the allocator already marks such blocks
reclaimable; and owning the policy would mean a header on every array saying
how it was obtained, a seal so a damaged header could not unmap a neighbour,
an abort when the seal failed, and a mapping per large array, multiplied by
every striped table's stripes, toward the per-process mapping limit some
systems keep. The gap between relic's peak and its live heap -- that residue
plus the slack of power-of-two tables and doubling arrays -- is outside what
the limit promises.

## Failure paths and limits

* Exhaustion and a limit refusal both fail the operation with a diagnostic and
  leave every structure consistent: arenas through the flag and the phase
  boundary, tables and relations by taking every fallible step before any
  mutation ([identifier table](design-values.md#the-identifier-table),
  [insertion](design-storage.md#insertion)).
* A context that cannot be allocated, or cannot draw a hash seed
  ([hash seed](design-values.md#the-hash-function-and-the-seed)), is not
  created, and there is no context to report through.
* The value and name tables release nothing before the context is destroyed;
  the limit bounds them
  ([growth for life](design-values.md#growth-for-the-life-of-the-context)).
* The allocation-failure suite renames every allocator the library calls to
  one that refuses the *n*th request, and `tests/allocators.sh` fails closed on
  an allocator the renaming would miss, so every failure path above is one the
  suite takes ([testing](design-testing.md)).

## Determinism

* **What a table holds is a function of its contents, not of how they
  arrived.** An owner that makes room for many insertions at once grows to
  the stepwise capacity, so the bytes it holds are those one-at-a-time
  insertion would reach, whatever the batching and whatever the thread
  count ([growable arrays](#growable-arrays)).
* **A data-sized buffer's capacity depends on its largest request alone**,
  so a shared round's admission can ask for the serial capacity without
  running serially ([round-scoped buffers](#round-scoped-buffers)).
* **Every limit decision is asked of the whole account** against projections
  made by the growth functions themselves, so whether an operation fits
  depends on what is held, not on the order holders grew in. What is held
  can depend on earlier runs: interned values and predicates persist for the
  life of the context, so a query's memory headroom may differ after
  earlier queries ([growth for
  life](design-values.md#growth-for-the-life-of-the-context)).

## Alternatives not taken

* **Per-allocation free, or a free list, in the arena.** That is a general
  allocator with extra steps; the point is that a phase's allocations are
  forgotten together.
* **Retaining chunks across a reset.** It pays only for an arena reset in a
  hot loop, and none resets an arena that way.
* **Charging the caller's arenas**, **per-site bookkeeping of the account**,
  and **a relic-owned mapping policy**: see above.
* **Reference counting or shared ownership.** One owner and one lifetime per
  allocation is what makes "who frees this" a question with one answer.
