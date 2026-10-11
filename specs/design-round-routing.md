# Bounded candidate routing

This document defines candidate ownership inside a deferred evaluation
round. Producers send bounded packets to round-owned value and tuple
partitions. Each distinct new value and tuple is retained once, and final
publication follows the earliest logical derivation position rather than
arrival order. The design sits between the executor's batch output and
the ordered, reserved append into relations.

Related: [architecture](architecture.md#parallel-evaluation),
[parallel evaluation](design-parallel.md),
[driving cursors](design-driver-slicing.md),
[pending value compaction](design-pending-values.md),
[values](design-values.md#interning-computed-values),
[storage](design-storage.md), [memory](design-memory.md),
[grouped reduction](design-group-reduction.md),
[testing](design-testing.md).

## Model

A round executes the same selected plans, fixed views and ordered tasks
as serial evaluation. Its store remains frozen until all production and
private reconciliation finish. Four private objects organize that work:

| Object | Ownership and lifetime |
| --- | --- |
| Lane | One resumable executor scratch and current task; exclusive to one pool part during a production phase |
| Packet | A bounded collection of candidates and packet-local pending values; sealed until both ownership phases consume it |
| Value partition | One round-local dictionary of distinct pending semantic values, with owned bytes and private canonical handles |
| Tuple partition | One round-local set of distinct new `(target, tuple)` pairs, retaining canonical columns and the earliest logical position |

An **owner** is a partition, not a permanent worker thread. One pool part
mutates each partition in its phase; any worker can run that part. The
partition count is fixed independently of the configured worker count.
There is no mutex per tuple, concurrent dictionary growth, or worker
waiting for another worker to empty a queue.

```text
prepare plans, fixed views, tasks and bounded lane budgets
    -> produce and seal packets
    -> canonicalize packet values in value-owner parts
    -> deduplicate candidates in tuple-owner parts
    -> acknowledge packets; resume lanes; repeat
    -> seal the round's unique candidates
    -> order by earliest logical position
    -> compact and resolve referenced values; rewrite and rehash
    -> count, stage, classify keys, check and reserve
    -> append values and tuples; apply indexes; publish counters
```

Barriers separate these phases. A routing wave is a scheduling unit
inside one round, not a fixed-point iteration or publication boundary.
Relations, global values and deltas change only at the ordinary final
commit. A declined attempt releases private work and reruns the same
selected plans serially.

## Invariants

* **One owner per semantic object.** Equal pending values reach the same
  value partition; equal tuples for one target reach the same tuple
  partition. Hash equality alone never establishes object equality.
* **Only owners retain round-sized data.** Producer dictionaries, screens,
  payload and routing lists are bounded transport storage. No producer
  retains its history of candidates or pending values across waves.
* **One writer per owner per phase.** Consumers cannot observe a value
  dictionary while its arrays grow, or tuple columns while they change
  namespace. Barriers establish these access rights.
* **Identity follows logical order.** A tuple's owner stores the minimum
  derivation position of all copies, even when an earlier copy arrives
  later. Neither an owner-local handle nor arrival order is a public ID.
* **Frozen views span the whole round.** Draining a packet inserts nothing
  into a relation and advances no delta. A lookup slice starts from its
  prepared cursor, never from a new frontier position.
* **Every derivation counts once.** Frozen-store hits and duplicates in
  packets or owners still count. An unconsumed row is neither counted nor
  advanced merely because its packet has no room.
* **No fallible work follows publication.** Memory, tuple and identifier
  checks precede the ordinary value and relation append. Private failure
  is a serial retry, never a worker diagnostic or a partial answer.

## Logical order and tuple ownership

### Positions describe the serial derivation sequence

Tasks retain the order defined by [driver slicing](design-driver-slicing.md):
item order, then ordered slices within an item. An uncut plan or seed is
one task. Each emitted head receives a position:

```text
(task ordinal, emission ordinal within that task)
```

Emission ordinals count all consumed heads, including frozen-store hits
and duplicates. The number increases only when the taker consumes the
row. A row paused for packet space keeps its position for the next wave.
A seed uses the same scheme with one head. Prefix operations bind no
values and emit nothing; their counters and notes remain once-per-item
contributions as in the slicing design.

Use explicit unsigned fields with checked increments and comparisons.
Do not pack an unproved task/ordinal range into spare pointer or domain
bits. An unrepresentable private position declines the deferred attempt;
it is not a new limit on the serial evaluator. Byte calculations for
position vectors and candidate references are checked independently.

### Owner records are unique, with mutable provenance

A tuple partition's key is the target ordinal and canonical column
sequence. The target is essential: equal columns inserted into two
relations are two new tuples. Arity is the target's fixed arity, and the
empty tuple is a valid key requiring no read through a null column pointer.

On a first visit the owner stores the columns, routing hash and position.
On a repeat it keeps the smaller position and stores no second tuple or
payload. The columns do not change when an earlier witness arrives.
The owner table itself is round-local and never includes the frozen
relation's already stored tuples.

Insertion credit is assigned only after all positions are final. An
arrival-time insertion counter would credit whichever task happened to
arrive first, corrupting per-item `gained` state and inserted-tuple history.
Finalization attributes each unique tuple to the task in its minimum
position, then derives item counts and gains from those task counts.

## Bounded production and progress

### A lane survives a yield

A lane owns its executor state independently of the physical worker that
runs it. A pool job has one part per active lane, so no two threads can
run one scratch at once. At a barrier the caller can assign an unstarted
task to an idle lane. A lane with a paused task resumes the same cursor,
frame, lower-level boundary, note, counter state and partially consumed
batch. Completed tasks can release their execution state only after the
last batch is consumed and its packet acknowledged.

Task assignment is in increasing task order. A lane can execute a task
before a lower-numbered task finishes on another lane; owners' minimum
positions and final ordering make that safe. No unfinished executor stack
is transferred by copying only its frame: text offsets, operation states,
pending bytes and the batch cursor are part of the lane.

Production runs for a bounded number of executor returns or until its
packet fills. This bound applies even when every head hits the frozen
store or a packet-local duplicate, so a producer cannot avoid the barrier
forever by producing no new candidate. The executor retains its ordinary
quantum guarantee, including its allowance for an unresumable step.

### A byte budget covers transport, not just row counts

`B` is a fixed total ceiling on live transport allocations. It covers
packet entries and columns, packet-local screens and value descriptors,
copied payload, pending head-byte buffers and the lists/receipts used to
route those packets. Capacities and alignment padding count. The budget
does not mean `B` per worker or `B` per partition.

The caller divides that ceiling among active lanes and shared routing
workspace. Lane count is bounded by the minimum useful packet/batch
layout as well as available workers and tasks. Buffers grow only within
their assigned capacity envelope; the full ceiling need not be allocated
up front. A checked layout reserves enough of the ceiling for the largest
routing lists and value receipts those packet capacities can require;
producers cannot consume that reserved share. Resizing counts every live
old/replacement allocation owned by transport. Data buffers are initialized
only for active lanes and only after their allowance is installed.
Program-and-worker-sized operation state and fixed control matrices are
accounted for separately from data-dependent transport.

Batch production must honor the budget before copying pending head bytes.
The executor accepts a private output-byte allowance from this taker.
The allowance is an explicit startup input and survives resume; starting
another task or slice in a routing lane passes that lane's allowance
again, rather than silently restoring an unlimited output buffer.
If a head would exceed a nonempty batch's allowance, it yields that batch
and resumes before the head. If the head cannot fit an empty transport
batch, the deferred attempt is declined before making an oversized copy.
Ordinary serial startup has no transport allowance; the serial run judges
the row against the real memory and value limits. A transport refusal
cannot escape as a user-visible error.

Expression text stacks and regex workspaces are distinct from transport:
computing a value can need memory before `EMIT` knows its size. They keep
their existing charged scratch ownership and serial-memory-floor rules.
The candidate bound therefore does not claim that arbitrary expression
scratch is bounded by `B`. Fixed batch column/descriptor storage is part
of the lane layout; variable pending head bytes cannot hide outside the
transport budget under the name of executor scratch.

A taker checks room before consuming a row. On refusal it keeps the row
and pending descriptors in the current batch, drains the packet, and tries
again after acknowledgment. Tentative append plans and packet-local
references do not survive the pause; the unchanged batch is the source
for a fresh check against the next phase's owner view. A row that cannot
fit an empty packet declines
the round instead of repeatedly draining an empty packet. Every successful
wave either consumes executor input, advances a cursor, or retires a task.

### Packet-local screening

For a fully assigned tuple, retain the existing batched hash and prefetch
probe against the frozen canonical index. A held tuple is counted and
dropped immediately. A tuple containing a value absent from the frozen
value tables cannot already be stored.

Normalize pending inputs against the frozen tables before deciding to
skip that probe; an unnormalized marker is not evidence of absence.
Temporary checks can borrow the current batch within the production
phase. Packet dictionary additions commit with an accepted outgoing row,
so a dropped row does not leave unused new packet values behind.

A bounded local screen can collapse repeats within a packet. Its values
are canonical only within that packet, so local markers are interpreted
with that packet's dictionary. It keeps the earliest position among its
copies. This screen is an optimization of traffic; correctness and the
round-sized bound depend on the owner set, not on local screening.
Disabling the local screen in tests must leave the result unchanged.

Only values referenced by outgoing rows enter the packet's pending table.
Computation temporaries, rejected bindings and already stored tuples do
not create round-owned values. A packet owns its copies of pending text;
it borrows no pointer into a scratch that can resume before consumption.

### Reuse the immutable owner set during production

Between routing phases, owners are immutable. The taker can use a const
view of that set to avoid sending a repeat already retained by the round.
This is a collector screen only; executor body operations still read the
original frozen relations and public value tables.

For pending columns, a read-only value lookup can supply an existing
canonical handle. A packet descriptor can retain that handle and its
cached semantic hash instead of copying its payload again; an unresolved
value retains the ordinary owned descriptor and bytes. Receipts for
known handles are filled before sealing and excluded from the value-owner
work list. No pointer into an owner's arrays survives the phase boundary.

When all columns can be expressed canonically, probe the tuple owner.
An existing tuple can be dropped only when its recorded minimum position
is no later than this head's position. A late earlier witness must still
be sent to lower the minimum. If any pending value is absent from the
round dictionary, the tuple cannot already be owned. Count every consumed
head regardless of the screen's outcome.

This reuse can save repeated routing and text copies across waves without
a second retained cache. Packet-local and read-only owner screening are
both traffic optimizations: tests can disable either, and exact owner
deduplication still establishes the data bound and final order. Measure
their probe cost on all-distinct inputs as well as their savings on repeats.

## Routing phases

### Fixed partitions and bounded lists

Value and tuple partitions are separate arrays, each with a fixed
power-of-two count independent of worker count. They can use the same
internal partition-count constant without sharing their dictionaries.
Production, value ownership and tuple ownership use the same pool in
successive jobs; there is no reserved set of permanently idle consumers.

At the production barrier, count packet items by destination and build
contiguous reference lists with checked prefix sums. Value lists name
packet descriptors; tuple lists name packet rows. The sort/list workspace
is included in `B`. An owner reads only its list, rather than every owner
walking all packets. Empty value and tuple phases are skipped.

The caller publishes a phase only when its inputs and lists are complete.
The packets are immutable until both owner jobs finish. Value receipts
are disjoint output slots, one per packet value; they do not mutate the
packet payload. A tuple owner interprets local markers through those
receipts into a fixed-width temporary row, and stores canonical columns
only if the tuple is new.

### Canonicalize pending values before comparing tuples

Each value owner interns pending values privately for this round, using
typed semantic hashing and exact equality from the value layer. Integers
compare by number; text by kind, length and every byte, including NULs.
It copies a new value's payload once and returns a canonical round handle.
Repeated packet values receive that same handle and retain no second copy.
No global value table, symbol pool or use bit changes in this phase.
Each append reserves descriptor, table and payload room before publishing
a complete handle; a failed growth cannot expose a partly initialized
entry to a receipt or to cleanup.

A handle names a value partition and entry position, not a worker or a
domain identifier. Candidate columns remain ordinary 32-bit words:
assigned domains have nonzero type bits and canonical pending handles
use the zero-type namespace. A fixed partition count permits a checked
partition/local-index encoding in that namespace. Zero remains invalid.
Only handle accessors pack, validate or decode it; call sites do not
reproduce masks or offsets.

Owner entry space and pending-handle space are private representation
limits. Exhausting one declines the optimization, even if public tables
could represent more values spread differently. It never reports a false
global value-limit error. This retains the compact all-domain tuple layout
that matters for closure workloads rather than widening every column to
accommodate a rare private-handle overflow.

The value barrier makes every receipt and retained payload visible before
tuple owners run. Candidate equality is then target plus word equality:
equal pending values have the same canonical handle, unequal ones cannot
share it, and no pending handle is a domain word. Tuple owners need no
pointer into a mutable value array to decide equality.

### Hash semantic values, not producer markers

The packet constructor normalizes a computed value already present in
the frozen tables to its assigned domain word. Only absent values enter
the pending namespace. Consequently an assigned value and a pending value
cannot denote the same typed value within that frozen round.

Routing hashes assigned columns by their canonical domain identity and
pending columns by a cached, type-tagged hash of semantic payload. The
tuple hash combines those contributions with arity and target identity
using the context's seeded hashing. It never hashes a packet marker, owner
handle, pointer, arrival number or worker number as a pending value's
meaning. Equal candidate tuples therefore choose the same owner across
packets and worker counts.

Fully assigned tuples keep the ordinary relation wide-hash fast path,
with target mixing for private owner selection. They need no value job or
domain decoding. A pending tuple has a routing hash until its promised
domain identifiers exist; after rewrite it receives the ordinary stored
tuple hash and canonical stripe. Routing partitions are not relation
stripes and are never reused as stripe numbers during append.

Hash collisions still compare exact values in the value dictionaries and
exact canonical columns in tuple dictionaries. Routing can use seeded
partitions because their private layout is not visible; persistent stripe
selection retains the storage design's worker- and seed-independent rule.

### Ownership, growth and progress

Each owner job is the sole writer of its table, entries and payload.
Only that owner can grow them. Producers are paused, and tuple owners do
not run during value-owner growth, so no reader can hold a pointer into a
reallocating dictionary. Tuple columns store handles; later waves can grow
value arrays without changing the handles already stored.

Owner data is allocated lazily and grows from actual routed inputs.
Inserted-tuple history can inform one bounded total reservation hint;
it does not reserve the previous round's candidates separately for every
worker or allocate data for every empty partition. The known current
counts, not a traffic estimate, determine correctness and admission.

A hot key has one owner. Local coalescing reduces its traffic, but no
thread concurrently splits that key's equality state to improve balance.
Many owner parts distribute ordinary skew; one heavily loaded partition
can still limit scaling. This is measured as owner work and tail time,
not hidden by adding more copies of its set.

The phase protocol provides backpressure by construction: producers
return when full, the pool barrier ends their jobs, owners drain finite
lists, and only then are packets reusable. Even a one-thread pool can
perform every phase. No producer blocks while holding the only execution
slot an owner would need, and owners never enqueue work back to producers.
Failure or interruption takes all participants to the barrier and cleanup.

## Sealing and ordered finalization

The round is sealed only after every task has finished, every batch is
consumed and every packet has been acknowledged. A task reaching executor
end with an unsent final batch does not satisfy that condition. Sealing
ends owner mutation and makes candidate blocks and value dictionaries
immutable inputs to finalization.

Sealing consumes the mutable owners and releases their tuple and value
membership tables: no semantic lookup or duplicate probe remains. The
sealed representation retains entry arrays, canonical columns, minimum
positions and value payload, without a producer lookup interface. These
transferred arrays remain live for compaction and staging, while the dead
hash-table capacity is released before ordering workspace is allocated.

Build a reference vector over the `D` unique candidate entries and order
it by their minimum positions. Positions are fixed-width unsigned integer
keys, so a stable byte-radix sort avoids comparison sorting's `D log D`
work on large rounds. Sort emission-ordinal bytes least significant first,
then task-ordinal bytes least significant first. High zero bytes can be
omitted from the maxima collected with the vector; fewer than two entries
need no passes.

Each pass reads fixed contiguous ranges of the reference vector, builds
per-range byte histograms, and assigns disjoint destination spans by
bucket and then source-range order. Each part visits its source range in
order. Thus the pass is stable, and the successive passes give lexicographic
task/emission order. Two charged reference vectors alternate as input and
output; owner entries and columns never move. Histogram control space
follows the bounded part count. Prefix sums, allocation sizes and unsigned
shifts are checked, and loops poll interrupts between bounded spans.

Different unique tuples cannot have the same position because one `EMIT`
emits one tuple. Debug/test validation catches a tie rather than breaking
it by owner number or hash order.

The ordered vector is the serial insertion stream with already held
tuples and repeated derivations removed. Per-target append positions and
per-task inserted counts follow from that vector. The tuple limit is
checked against the sum of genuinely new tuples, and each target's
identifier ceiling against its own count. These counts only grow during
routing, so a round can also decline early once an exact owner-count sum
already exceeds an admissible bound.

### Values follow the unique tuples' first occurrences

Walk the ordered unique candidates in tuple and column order. Collect one
compact descriptor per referenced canonical pending handle, then use the
ordinary value resolver to promise domain identifiers. A partition-local
map translates its entry position to a compact descriptor number. The
procedure and ownership are specified in
[pending value compaction](design-pending-values.md).

This walk needs no log of duplicate derivations or arrival-time value
positions. If a fresh value occurs in a duplicate tuple, that tuple's
earliest copy contains the same value in the same column at an earlier
position. Every fresh value in a successful outgoing packet therefore
occurs in a retained unique tuple. Dropping duplicate copies cannot lose
its earliest semantic occurrence.

The tuple owner's columns change namespace once, directly from canonical
pending handles to promised domain identifiers. Packet-to-owner
normalization constructs a new owned tuple; it is not another rewrite of
a retained candidate. Recompute affected stored-tuple hashes and stripes
at this barrier before append planning. Release temporary compact maps
and, when the resolver no longer borrows them, the routing dictionaries;
keep the resolution owner's byte buffer and map through append or rollback.

### An out-of-order example

Suppose these copies reach owners in the order shown; integers are
already assigned, and the text values are pending:

| Arrival | Logical position | Target and tuple |
| ---: | --- | --- |
| 1 | `(2, 0)` | `p(1, "violet")` |
| 2 | `(1, 0)` | `p(2, "amber")` |
| 3 | `(0, 9)` | `p(1, "violet")` |
| 4 | `(3, 0)` | `q(1, "violet")` |

The owner of the first tuple changes its position to `(0,9)` on arrival
3. Final order is the first `p` tuple, the second `p` tuple, then the `q`
tuple. The value resolver encounters `"violet"` before `"amber"`, whatever
private handles the value owners assigned. The `q` tuple remains distinct,
and task 0, not task 2, receives insertion credit for `p(1,"violet")`.

## The commit boundary

The append machinery consumes ordered unique candidates instead of
worker-buffer runs. Candidate access is by immutable block/entry reference;
neither append code nor value compaction infers logical order from a
block's allocation or ownership. Target arity and counts are validated
before staging. There is no second cross-worker duplicate table at merge:
the owner set has already established global uniqueness.

The remaining steps use the ordinary storage interfaces:

1. Resolve referenced pending values in ordered first-occurrence order,
   rewrite owned candidates and compute final tuple hashes/stripes.
2. Count exact per-target appends and attribute them to tasks/items.
3. Stage tuple columns and key hashes at those append positions.
4. Classify new secondary-index keys in each target's tuple order.
5. Project serial storage growth, check limits, and reserve values and
   every target before publishing anything.
6. Append values, copy staged columns, and apply canonical and secondary
   index stripes in the established order.

Staging and key classification still have costs proportional to unique
new data. This design removes replicated candidate sets; it does not
claim to eliminate final relation storage, staging or new index entries.
After staging owns every column and task counts are recorded, candidate
blocks and ordering references are released before admission where their
remaining readers permit. The resolver's borrowed bytes follow its own,
longer lifetime. No pointer into freed owner data reaches reserve/apply.

The normal one-thread evaluator still inserts its batches directly and
does no routing. `-X parallel` exercises this deferred protocol at one or
many available workers, subject to ordinary refusal and fallback. No
second unbounded deferred collector is needed as a permanent reference;
serial execution is the semantic oracle, and immutable baseline binaries
provide the performance comparison.

## Memory bounds and admission

Let `D` be distinct new target/tuple pairs, `C` their total columns, `V`
their distinct pending values, and `S` the bytes of distinct pending text.
For fixed arities and index requirements, candidate and value storage is:

```text
owned tuple metadata and columns      O(D + C)
tuple-owner membership tables        O(D)
value metadata, tables and bytes     O(V + S)
transport and routing receipts       at most B
ordering and compaction workspace    O(D + V + S)
append and index-key workspace       O(C + D * planned indexes)
```

`C <= RELIC_MAX_ARITY * D`. The shorthand bound is `O(D + V + S + B)`
for a fixed program, without a workers-times-distinct-data term. Variable
string bytes cannot be hidden inside an entry count. Operation states,
task records and lane/partition control tables depend on the program and
worker count; expression scratch is separately charged. Neither is an
excuse for a dictionary or log that grows with all derivations.

During a value phase, new values can exist before their packet's tuples
reach owners. They are backed by that finite in-flight packet. After its
successful tuple phase, each value is referenced by some unique tuple;
on failure the entire attempt is discarded. Thus private dictionaries
need no per-occurrence reference log or reclamation protocol to keep this
bound. A packet-local cache never keeps older generations of markers.

All data allocations are charged at capacity while owned, including
tables, payload slack, route lists, sorting scratch and resolver copies.
Checked size arithmetic covers every sum, alignment and product. Growable
arrays and hash tables use the existing stepwise rules. Actual allocator
retention and transient realloc peaks are measured separately from the
account, as in [memory](design-memory.md).

### Preserve the serial refusal contract

Owned tuples and pending values only accumulate; changing a tuple's first
position changes no payload. Serial persistent storage also grows
monotonically during an eligible round. The existing final append/value
projection and serial scratch floor therefore remain the admission rule.
Transport draining cannot publish a value, erase that floor or admit only
the current wave while earlier waves' eventual storage is forgotten.

Lanes retain or record each executor buffer's capacity peaks across all
tasks and waves. Resetting batch contents or releasing transport does not
reset those peaks. Prefix scratch contributes too. After the production
barrier, admission includes the ordinary serial-equivalent floor, projected
value storage, relation growth with replaced tables still held, and all
live private staging/resolution charges.

An output allowance can clip buffer growth, so the observed pending-byte
capacity alone is insufficient. The emitter also records `H`, the largest
pending byte requirement of one head, before the allowance is applied.
Ordinary `EMIT` yields a nonempty batch when the next head will not fit;
it grows the pending-byte buffer only when the batch is empty, for that
one head. This rule remains common to ordinary and bounded output.
The serial run can have fewer pending values than the frozen run, so its
largest growth request is at most `H`.

Project `H` through the ordinary growth rule, starting at the ordinary
initial capacity, and take at least that projected capacity and the
observed peak in the admission floor. Checked sizing overflow declines
the attempt. No row log or multiplication by batch length is needed.
The other executor buffers retain their existing per-buffer maximum
floor. No allowance can weaken admission to fit the parallel path.

Routing can therefore run with configured memory and tuple limits. A
private charge refusal or an inconclusive conservative projection releases
the attempt and lets serial execution decide the actual result. Unlike
group reduction's replacement of temporary extrema, routing discards only
duplicate copies; every distinct new head value remains represented until
commit. The grouping design's serial-construction rule is not a prerequisite
for this monotone candidate path.

Private budgets, owner capacity and hash distribution can cause more
serial retries, but cannot change an answer, a limit diagnostic or the
first refused logical row. A partially successful public reservation can
retain accounted capacity as the ordinary merge permits; private owners
never make such a reservation during production or draining.

## Failure, cancellation and security

A phase refusal records a private abandoned status. Every scheduled part
returns through the pool's completion barrier, including a part that
observes abandonment before doing work. The caller schedules no subsequent
phase after failure, waits for all readers, releases packets, owners,
lanes' data buffers and finalization workspace, and restores ordinary
serial execution of the selected round. No task is restarted inside a
partially retained owner set.

Interrupt checks occur at executor returns and bounded intervals in new
counting, routing, dictionary rehash, copying and ordering loops. A failed
allocation, private marker overflow, oversized head, malformed internal
reference or task evaluation error follows the same unreported attempt
refusal. The serial run emits any user-visible error or interrupt outcome.
No owner performs callbacks or reports keys or payload bytes in a diagnostic.

Packets and receipts are internal but are validated at their phase
boundary: target and arity, row/word ranges, marker partition and index,
byte offsets/lengths and completeness of receipt entries. Empty payload
has a valid addressable representation; zero-length copies never form a
pointer from `NULL`. Exact equality follows hash matches and uses typed
value lengths. Table slots and padding never become observable value bytes.

Executors and owner parts read global state through incomplete const types
in `lib/frozen.h`. Their mutable objects are private packet or owner
storage and the existing charged-round account. Type-separated phase
inputs distinguish a sealed packet, completed value receipts and a sealed
candidate set; a mutable builder is not a public relation or value table.
The context's busy/re-entry rules protect the whole interval from preparation
through commit. Clearing or discarding a relation cannot interleave with it.

## Counters, history and integration

Public reads and derivations come from the ordinary executor and taker.
Routing probes, dictionary probes, packet lists and sorting are private
overhead, not invented relation reads. Counters are retained per task
across lane pauses; prefix counts are added once. A successful commit
publishes their totals; an abandoned attempt forgets them. One deferred
round contributes at most one `parallel_rounds` increment, however many
waves and owner jobs it required.

Notes remain in logical item/task order, with a prefix before its suffix.
Changing a tuple's minimum position changes only its insertion attribution,
not which computations counted or which note a task first encountered.
Inserted-tuple history and `gained` flags use the final unique stream;
plan-work history uses all evaluated work. The selected recursive plan
and its views stay fixed through routing and serial fallback.

The driver-slicing design supplies task coverage, starts and lower-level
executor boundaries. This protocol supplies bounded continuation and
ownership between yields. Direct grouped reduction can share the lane
lifetime and output-budget primitives but retains its own accumulator
taker: sending all its source rows through tuple owners would recreate
the intermediate store that reduction removes.

| Component | Responsibility |
| --- | --- |
| `lib/round.c`, `lib/round.h` | Round/lane lifetimes, phase ordering, budgets, abandonment, task accounting and final admission handoff |
| `lib/task.c` | Resume a task, number consumed heads, screen the frozen store and fill bounded packets |
| `lib/exec.c`, `lib/exec.h` | Preserve every continuation field and honor the taker's private pending-output allowance before copying |
| `lib/route.c`, `lib/route.h` | Packet lists, private value and tuple owners, semantic routing, canonical-handle accessors and sealed unique candidates |
| `lib/value.c`, frozen value accessors | Shared semantic hashing/equality and ordinary promised-identifier resolution/publication |
| `lib/merge.c` | Ordered unique-candidate finalization, compact resolution, counts, staging and the existing reserved append |
| `lib/relation.c`, `lib/index.c` | Ordinary projections, exact capacity reservation and infallible append/index apply; no transport state |

No language construct, public value encoding, persistent index layout or
runtime dependency is added. No source file requires a third-party queue
or concurrent hash-table package.

## Determinism proof

For every target tuple absent from the frozen store, consider all its
emitted copies. Packet-local screening retains a first copy of each local
set; routing sends every surviving equal copy to one owner. The owner's
minimum is therefore the minimum over all original derivations. Sorting
the owners' unique tuples by those minima gives exactly the new-tuple
sequence that ordinary serial insertion observes.

Every fresh head value occurs in at least one such tuple. Replacing a
tuple's duplicate copies by its earliest copy preserves the earliest
occurrence of each of its values. Scanning unique tuples and columns in
that order gives the serial value resolver the same semantic sequence of
first appearances. Its promised identifiers, use bits and capacity
projections are consequently the ordinary ones.

The established ordered append then produces the same tuple identifiers,
canonical entries, secondary chains, storage capacities, deltas and output.
Exact equality prevents hash collisions from changing this argument.
Worker count, owner assignment, wave boundaries and arrival order affect
cost only. A failure before publication discards the attempt, and the
serial retry establishes the ordinary failure behavior instead.

## Validation

### Evidence and regressions

Instrument a baseline before tuning: per round, record emitted heads,
frozen hits, packet/worker-local repeats, sum of local candidates, globally
distinct target tuples, distinct pending values and bytes, every merge
workspace and allocator retention. The recorded RSS gap does not prove
that all excess memory is candidate replication. Separate required final
relation/index storage from candidate, value and staging amplification.

The first failing regression forces many ordered tasks to derive the
same modest set of new tuples while the frozen target is empty. Check
that retained tuple slots follow the global distinct count at 1/2/4/16
workers, with transport at or below `B`; the old per-worker representation
must fail that bound. Deterministic unit fixtures control task arrivals,
rather than relying on a thread scheduler to exercise every owner.

A second regression repeats a few long computed strings and integers
across many distinct output tuples, tasks and targets. It checks one
round-owned copy per semantic value, compact descriptor counts, final IDs
and use bits, and the payload bound in bytes. A tuple-only bound cannot
hide a worker-local value dictionary that still retains the whole round.

A compact all-domain fixture has 128 `source` facts and 2,048 `value`
facts, with `out(V) :- source(_), value(V).` under no-exists, no-reorder
and forced deferral. Its producer emits 262,144 heads for 2,048 unique
outputs. Constructed ownership schedules pin worker replication in the
reference; evaluator runs check that slicing and the query keep identical
output and semantic work across thread counts.

| Area | Required cases and assertions |
| --- | --- |
| Order | Later tasks arrive first; an earlier duplicate arrives in the final wave; duplicates within/across packets and items; exact first positions, target IDs, per-task insertions and gains. |
| Views and continuations | Scan and lookup drivers; every semi-naive view; constant prefixes; seeds, empty and uncut tasks; full batch at a boundary; partially consumed batches; repeated pauses never restart a cursor. |
| Identity | Equal packet markers with different values; different markers with equal values; frozen IDs and normalized raw values; multiple targets; zero-arity tuples; existing pool bytes used as a new string type. |
| Payload | Empty text, NULs, long common prefixes, many equal long values, all-distinct values, largest fitting head and an oversized head; no hidden copies outside the budget. |
| Skew and hashing | One hot tuple, one hot value, one overloaded partition, deliberate hash collisions and different hash seeds; exact equality and unchanged final storage. |
| Progress | One worker for all phases; one tiny packet; all local duplicates; all frozen hits; no output; more workers than useful lanes; last task ends with an unsent batch; every pending row is consumed or the attempt declines. |
| Admission | Tight tuple and memory limits; owner counts at target ID limits; private handle/ordinal limits; executor scratch peaks across waves; output allowance cannot weaken the serial memory floor. |
| Refusal | Allocation injection in packets, receipts, owners, rehash, ordering, resolution and append reservation; interruption at every phase; no blocked part, stale packet pointer, public partial value or leaked charge. |
| Accounting | Public work equals serial work; insertion attribution follows minima; prefix notes occur once; no extra fixed-point iterations or per-wave parallel-round increments; history updates only on success. |

Use a set-and-sequence model as an independent oracle: assign positions
before scheduling, vary packet sizes and arrival order, canonicalize values
by content, and compare ordered firsts and first-value numbering with a
serial walk. Check the bounded representation separately from answer
equality. Exercise the real route and merge interfaces with constructed
packets as well as full evaluator tests; fabricated invalid receipts and
partially initialized owners need direct tests.

Reference runs use ordinary serial evaluation and forced deferral at one
worker, then several worker counts and a threads-disabled build. Include
no-index/no-reorder, recursive alternative selection, grouped outputs,
string functions, limits and allocation exhaustion. A frozen store can
remain readable after every abandoned pre-reservation attempt; retrying
the selected plans must not depend on leftover private data.

Implementation reviews cover bugs, design, security/information leaks
and the style guide's readability review before tests. Run focused
regressions and the full suite at major changes, plus ASan, UBSan and TSan;
LSan/MSan run where supported. Memory ownership and arithmetic paths
require their own failure/overflow cases, not merely a successful large run.

### Performance acceptance

Follow the [benchmark measurement rules](../docs/todo-benchmark-performance.md#measurement-and-acceptance-rules).
Use immutable baseline/candidate binaries after preceding optimizations,
matching output sinks and exact answer checks. For closure, same-generation
and large-tree workloads, record live owned data, bounded transport, order
vectors, resolution, staging, final stores and RSS separately.

Measure production, local screening, value routing, tuple routing, ordering,
value resolution, staging and index apply. Report packets per wave, active
lanes, owner skew, discarded repeat traffic and serial retries. At fixed
`D`, `V` and payload size, increasing workers must not multiply retained
candidate or value storage. At zero new tuples, owner data stays empty.

Run scales 1/2/4/8 at 1 and 16 workers and 4/8/12 for scaling, with at least
ten interleaved targeted samples. The scale-8 full-tree case is a capacity
gate; low-duplicate joins, narrow rounds, computed values and already-held
closures are overhead controls. Include all-distinct text to measure
ownership and publication cost when deduplication saves little.

Owner count, byte budget and production quantum are internal tuning
constants, not workload-name switches or public resource settings. Their
values are selected from measured latency, throughput and charged peaks.
Sharing policy accounts for repeated barriers, routing and ordering as well
as suffix work. A predicted unprofitable round runs serially; it does not
switch to an unbounded parallel collector.

Acceptance requires the data bound and lower CPU/wall time on targeted
replication-heavy workloads, with the competitive goal in the benchmark
plan and no repeatable unexplained regression on controls. A lower RSS
that simply moves the bottleneck into owner hashing or final sorting is
not enough. The ordinary append cost remains visible in the result.

## Alternatives not taken

* **Only cap worker candidate arrays.** A cap without draining turns large
  rounds into serial retries and does not establish a bounded useful
  parallel path. Retaining all worker value tables leaves another replica.
* **Insert globally as packets arrive.** Tuple/value identifiers, deltas
  and later reads would depend on scheduling. The store stays frozen.
* **Publish complete task prefixes early.** A later task's reads can still
  need the frozen store, and expression/value failure order spans the whole
  round. Retaining one publication boundary avoids rollback and epochs.
* **One shared concurrent candidate table.** It introduces synchronization
  on every tuple and concurrent growth. Phase-owned partitions provide
  global deduplication with ordinary private tables.
* **Concurrent producer/consumer queues.** They require a bounded queue
  and wake-up/deadlock proof for every producer/owner resource dependency.
  Finite barrier-separated lists make that dependency graph empty.
* **Route by packet markers or physical worker.** Equal values can have
  unrelated markers, and worker assignment is not semantic identity.
* **Number on first arrival.** Late earlier derivations change serial
  first occurrence. Minimum positions and final ordering preserve it.
* **Comparison-sort every unique candidate.** Logical positions already
  have fixed-width integer fields. Stable radix passes bound ordering work
  linearly and parallelize with disjoint output spans.
* **Reclaim values by duplicate occurrence counts.** Every retained fresh
  value is needed by a unique tuple; bounded packets account for the
  temporary gap. A reference-counted occurrence log adds no necessary bound.
* **Spill candidates to disk.** It changes the runtime and failure model
  and adds cleanup and confidentiality obligations. Private memory refusal
  retains the existing serial fallback instead.
